/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * backend_execution_runtime.c
 *    Thread registry and completion queue for backend executions.
 *
 *-------------------------------------------------------------------------
 */

#include "postgamma/backend_execution_runtime.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef POSTGAMMA_EVENT_TIMERS
#include "postgamma/deadline_heap.h"
#endif


#define POSTGAMMA_NO_SLOT UINT32_MAX
#define POSTGAMMA_BACKEND_ID_SLOT_MASK UINT64_C(0xffffffff)


typedef struct PostgammaBackendExecutionStart PostgammaBackendExecutionStart;
typedef struct PostgammaBackendProviderOps PostgammaBackendProviderOps;
typedef struct PostgammaPooledWorker PostgammaPooledWorker;

typedef struct PostgammaBackendSlot
{
	uint32_t	generation;
	PostgammaBackendSlotState state;
	PostgammaBackendHandle handle;
	PostgammaThread *thread;
	PostgammaWakeTarget *wake_target;
	PostgammaBackendExecutionStart *start;
	PostgammaBackendCompletion completion;
	PostgammaBackendInterruptState interrupts;
	int			native_wake_signal;
	uint32_t	next_completion;
	uint32_t	next_runnable;
	uint32_t	assigned_worker;
	uint32_t	last_worker;
	uint64_t	queued_at_ns;
	uint64_t	deadline_ns;
#ifdef POSTGAMMA_EVENT_TIMERS
	PostgammaDeadline alarm;
	uint64_t	alarm_interval_ns;
#endif
	uint64_t	quantum_sequence;
	uint64_t	schedule_epoch;
	bool		paused;
	bool		pooled;
	bool		queued;
	bool		quantum_running;
	bool		completion_delivery_complete;
	bool		reschedule_requested;
	bool		pinned;
	bool		blocked;
	bool		execution_token_held;
	bool		has_last_worker;
	bool		stack_observed;
	bool		stack_depth_recorded;
	size_t		stack_reservation_bytes;
} PostgammaBackendSlot;

struct PostgammaBackendRegistry
{
#ifdef POSTGAMMA_EVENT_TIMERS
	PostgammaThread *alarm_thread;
	PostgammaCondition *alarm_condition;
	PostgammaDeadlineHeap alarms;
	uint64_t	alarm_wait_deadline;
	bool		alarm_stopping;
#endif
	const PostgammaBackendProviderOps *provider;
	uint32_t	capacity;
	PostgammaMutex *mutex;
	PostgammaCondition *condition;
	PostgammaWakeTarget *wake_target;
	PostgammaBackendSlot *slots;
	uint32_t	completion_head;
	uint32_t	completion_tail;
	uint32_t	active_count;
	PostgammaCompatPid next_compat_pid;
	PostgammaBackendTelemetry telemetry;
	uint64_t	client_threads_active;
	PostgammaPooledWorker *workers;
	uint32_t	worker_count;
	uint32_t	queue_capacity;
	uint32_t	runnable_head;
	uint32_t	runnable_tail;
	uint32_t	runnable_depth;
	uint32_t	next_worker;
	uint32_t	execution_token_count;
	uint32_t	execution_tokens_active;
	bool		stopping;
};

struct PostgammaPooledWorker
{
	PostgammaBackendRegistry *registry;
	PostgammaThread *thread;
	uint32_t	index;
	bool		running;
};

struct PostgammaBackendProviderOps
{
	PostgammaBackendProviderKind kind;
	int		(*launch_locked) (PostgammaBackendRegistry *registry,
							  PostgammaBackendSlot *slot,
							  const PostgammaBackendLaunchRequest *request);
	int		(*join) (PostgammaBackendSlot *slot);
};

struct PostgammaBackendExecutionStart
{
	PostgammaBackendRegistry *registry;
	uint32_t	slot_index;
	PostgammaBackendStartInfo info;
	PostgammaBackendMain main_function;
	void	   *main_argument;
	PostgammaBackendCleanup cleanup_function;
	void	   *cleanup_argument;
	PostgammaBackendCleanup startup_failure_function;
	PostgammaBackendCompletionNotify completion_notification;
	void	   *completion_notification_argument;
	size_t		startup_data_length;
	unsigned char startup_data[];
};


/* Bound only while the registered execution owns its slot. */
static _Thread_local PostgammaBackendExecutionStart *PostgammaCurrentBackendExecution;


static PostgammaBackendSlot *
current_execution_slot(PostgammaBackendRegistry *registry, PostgammaBackendId id)
{
	PostgammaBackendExecutionStart *start = PostgammaCurrentBackendExecution;

	if (start == NULL || start->registry != registry || start->info.handle.id != id)
		return NULL;
	/* Completion/join cannot recycle this slot until main_function returns. */
	return &registry->slots[start->slot_index];
}

const PostgammaBackendInterruptState *
postgamma_backend_borrow_interrupts(
	PostgammaBackendRegistry *registry, PostgammaBackendId id)
{
	PostgammaBackendSlot *slot = current_execution_slot(registry, id);

	return slot == NULL ? NULL : &slot->interrupts;
}

/* Publish the payload before the summary observed by executor hot paths. */
static void
publish_pending_signals(PostgammaBackendSlot *slot, uint64_t pending)
{
	__atomic_fetch_or(&slot->interrupts.pending_signals, pending, __ATOMIC_RELEASE);
	__atomic_fetch_or(&slot->interrupts.pending_events,
		POSTGAMMA_BACKEND_EVENT_SIGNAL, __ATOMIC_RELEASE);
}

static uint64_t
take_pending_signals(PostgammaBackendSlot *slot)
{
	/*
	 * Acknowledge BEFORE draining. A racing publisher either contributes to
	 * this drain or leaves the summary set for the next check. It may cause
	 * one harmless empty dispatch, but cannot lose a signal. Do not clear
	 * the independent pause bit while acknowledging signals.
	 */
	__atomic_fetch_and(&slot->interrupts.pending_events,
		~POSTGAMMA_BACKEND_EVENT_SIGNAL, __ATOMIC_ACQUIRE);
	return __atomic_exchange_n(&slot->interrupts.pending_signals, 0,
		__ATOMIC_ACQUIRE);
}

/* Called under the registry lock, which serializes pause/resume writers. */
static void
set_pause_requested(PostgammaBackendSlot *slot, bool requested)
{
	__atomic_store_n(&slot->interrupts.pause_requested, requested, __ATOMIC_SEQ_CST);
	if (requested)
		__atomic_fetch_or(&slot->interrupts.pending_events,
			POSTGAMMA_BACKEND_EVENT_PAUSE, __ATOMIC_RELEASE);
	else
		__atomic_fetch_and(&slot->interrupts.pending_events,
			~POSTGAMMA_BACKEND_EVENT_PAUSE, __ATOMIC_RELEASE);
}


static PostgammaBackendId make_backend_id(uint32_t slot_index,
										  uint32_t generation);
static bool decode_backend_id(PostgammaBackendId id,
							  uint32_t *slot_index,
							  uint32_t *generation);
static PostgammaCompatPid allocate_compat_pid(PostgammaBackendRegistry *registry);
static void *backend_thread_main(void *argument);
static void *backend_thread_start_failed(int status, void *argument);
static void begin_backend_thread(PostgammaBackendExecutionStart *start);
static void record_backend_stack_locked(PostgammaBackendRegistry *registry,
										PostgammaBackendSlot *slot);
static void *pooled_worker_main(void *argument);
static int dedicated_provider_launch_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot,
	const PostgammaBackendLaunchRequest *request);
static int dedicated_provider_join(PostgammaBackendSlot *slot);
static int pooled_provider_launch_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot,
	const PostgammaBackendLaunchRequest *request);
static int pooled_provider_join(PostgammaBackendSlot *slot);
static int start_pooled_workers(PostgammaBackendRegistry *registry);
static int stop_pooled_workers(PostgammaBackendRegistry *registry);
#ifdef POSTGAMMA_EVENT_TIMERS
static void *alarm_owner_main(void *argument);
#endif
static int enqueue_slot_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot);
static PostgammaBackendSlot *dequeue_slot_for_worker_locked(
	PostgammaBackendRegistry *registry, uint32_t worker_index);
static PostgammaBackendSlot *remove_runnable_slot_locked(
	PostgammaBackendRegistry *registry, uint32_t previous, uint32_t current,
	bool stolen);
static uint64_t schedule_due_slots_locked(
	PostgammaBackendRegistry *registry, uint64_t now_ns);
static int schedule_slot_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot);
static bool acquire_execution_token_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot);
static void release_execution_token_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot);
static void complete_backend_execution(
	PostgammaBackendExecutionStart *start,
	PostgammaBackendExitStatus exit_status, bool pooled_quantum,
	uint32_t worker_index, bool startup_failed);
static PostgammaBackendExitStatus execute_backend(
	PostgammaBackendExecutionStart *start, uint32_t carrier_index,
	uint64_t quantum_sequence);
static void update_peak(uint64_t *peak, uint64_t candidate);
static void update_minimum(size_t *minimum, size_t candidate);
static int record_stack_launch_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendClass backend_class,
	const PostgammaThreadStackInfo *stack_info);
static void record_stack_completion_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot);
static void record_stack_failure(uint64_t *failures);
static int lock_registry(PostgammaBackendRegistry *registry);
static void unlock_registry_or_abort(PostgammaBackendRegistry *registry);
static int wake_backend_slot(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot);
static PostgammaBackendSlot *find_running_slot_by_compat_pid(
	PostgammaBackendRegistry *registry, PostgammaCompatPid compat_pid);

static const PostgammaBackendProviderOps PostgammaDedicatedProvider = {
	.kind = POSTGAMMA_BACKEND_PROVIDER_DEDICATED,
	.launch_locked = dedicated_provider_launch_locked,
	.join = dedicated_provider_join,
};

static const PostgammaBackendProviderOps PostgammaPooledProvider = {
	.kind = POSTGAMMA_BACKEND_PROVIDER_POOLED,
	.launch_locked = pooled_provider_launch_locked,
	.join = pooled_provider_join,
};


int
postgamma_backend_registry_create(PostgammaBackendRegistry **registry,
								  uint32_t capacity)
{
	PostgammaBackendProviderConfig config = {
		.kind = POSTGAMMA_BACKEND_PROVIDER_DEDICATED,
	};

	return postgamma_backend_registry_create_with_provider(
		registry, capacity, &config);
}


int
postgamma_backend_registry_create_with_provider(
	PostgammaBackendRegistry **registry, uint32_t capacity,
	const PostgammaBackendProviderConfig *provider_config)
{
	PostgammaBackendRegistry *created;
	int			status;

	if (registry == NULL)
		return EINVAL;
	*registry = NULL;
	if (capacity == 0 || provider_config == NULL ||
		provider_config->kind < POSTGAMMA_BACKEND_PROVIDER_DEDICATED ||
		provider_config->kind >= POSTGAMMA_BACKEND_PROVIDER_COUNT ||
		(provider_config->kind == POSTGAMMA_BACKEND_PROVIDER_DEDICATED &&
		 (provider_config->worker_count != 0 ||
		  provider_config->queue_capacity != 0 ||
		  provider_config->execution_token_count != 0)) ||
		(provider_config->kind == POSTGAMMA_BACKEND_PROVIDER_POOLED &&
		 (provider_config->worker_count < 2 ||
		  provider_config->worker_count > capacity ||
		  provider_config->queue_capacity < capacity ||
		  provider_config->execution_token_count < 2)))
		return EINVAL;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->slots = calloc(capacity, sizeof(*created->slots));
	if (created->slots == NULL)
	{
		free(created);
		return ENOMEM;
	}
	status = postgamma_mutex_create(&created->mutex);
	if (status != 0)
	{
		free(created->slots);
		free(created);
		return status;
	}
	status = postgamma_condition_create(&created->condition);
	if (status != 0)
	{
		(void) postgamma_mutex_destroy(created->mutex);
		free(created->slots);
		free(created);
		return status;
	}
	status = postgamma_wake_target_create(&created->wake_target);
	if (status != 0)
	{
		(void) postgamma_condition_destroy(created->condition);
		(void) postgamma_mutex_destroy(created->mutex);
		free(created->slots);
		free(created);
		return status;
	}
	created->capacity = capacity;
	created->provider = provider_config->kind ==
		POSTGAMMA_BACKEND_PROVIDER_POOLED ?
		&PostgammaPooledProvider : &PostgammaDedicatedProvider;
	created->telemetry.provider_kind = created->provider->kind;
	created->completion_head = POSTGAMMA_NO_SLOT;
	created->completion_tail = POSTGAMMA_NO_SLOT;
	created->runnable_head = POSTGAMMA_NO_SLOT;
	created->runnable_tail = POSTGAMMA_NO_SLOT;
	created->next_compat_pid = POSTGAMMA_COMPAT_PID_FIRST;
	created->worker_count = provider_config->worker_count;
	created->queue_capacity = provider_config->queue_capacity;
	created->execution_token_count = provider_config->execution_token_count;
	created->telemetry.execution_token_budget =
		provider_config->execution_token_count;
	for (uint32_t index = 0; index < capacity; index++)
	{
		created->slots[index].state = POSTGAMMA_BACKEND_SLOT_FREE;
		created->slots[index].next_completion = POSTGAMMA_NO_SLOT;
		created->slots[index].next_runnable = POSTGAMMA_NO_SLOT;
	}
	if (created->provider->kind == POSTGAMMA_BACKEND_PROVIDER_POOLED)
	{
		status = start_pooled_workers(created);
		if (status != 0)
		{
			(void) postgamma_wake_target_destroy(created->wake_target);
			(void) postgamma_condition_destroy(created->condition);
			(void) postgamma_mutex_destroy(created->mutex);
			free(created->workers);
			free(created->slots);
			free(created);
			return status;
		}
	}
#ifdef POSTGAMMA_EVENT_TIMERS
	/*
	 * The timer service belongs to the registry, not to its first timed
	 * request. Establish its resources before publishing a ready registry.
	 * Arming a timer must not change the instance's native thread footprint.
	 */
	created->alarms.entries = calloc(capacity,
		sizeof(*created->alarms.entries));
	status = created->alarms.entries == NULL ? ENOMEM :
		postgamma_condition_create(&created->alarm_condition);
	if (status == 0)
	{
		PostgammaThreadAttributes attributes = {
			.name = "pg-alarm", .role = POSTGAMMA_THREAD_ROLE_GENERAL,
		};

		created->alarms.capacity = capacity;
		created->alarm_wait_deadline = UINT64_MAX;
		status = postgamma_thread_create(&created->alarm_thread,
			&attributes, alarm_owner_main, created);
	}
	if (status != 0)
	{
		if (postgamma_backend_registry_destroy(created) != 0)
			abort();
		return status;
	}
#endif
	*registry = created;
	return 0;
}


PostgammaBackendProviderKind
postgamma_backend_registry_provider_kind(
	const PostgammaBackendRegistry *registry)
{
	return registry == NULL || registry->provider == NULL ?
		POSTGAMMA_BACKEND_PROVIDER_COUNT : registry->provider->kind;
}


int
postgamma_backend_registry_destroy(PostgammaBackendRegistry *registry)
{
	int			status;

	if (registry == NULL)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	if (registry->active_count != 0 ||
		registry->completion_head != POSTGAMMA_NO_SLOT)
	{
		unlock_registry_or_abort(registry);
		return EBUSY;
	}
	for (uint32_t index = 0; index < registry->capacity; index++)
	{
		if (registry->slots[index].state != POSTGAMMA_BACKEND_SLOT_FREE)
		{
			unlock_registry_or_abort(registry);
			return EBUSY;
		}
	}
#ifdef POSTGAMMA_EVENT_TIMERS
	registry->alarm_stopping = true;
	if (registry->alarm_condition != NULL)
		(void) postgamma_condition_signal(registry->alarm_condition);
#endif
	unlock_registry_or_abort(registry);
#ifdef POSTGAMMA_EVENT_TIMERS
	/* The notifier cannot outlive its registry, slots or wake descriptors. */
	if (registry->alarm_thread != NULL)
	{
		status = postgamma_thread_join(registry->alarm_thread, NULL);
		if (status != 0)
			return status;
		status = postgamma_thread_destroy(registry->alarm_thread);
		if (status != 0)
			return status;
		registry->alarm_thread = NULL;
	}
	if (registry->alarm_condition != NULL)
	{
		status = postgamma_condition_destroy(registry->alarm_condition);
		if (status != 0)
			return status;
		registry->alarm_condition = NULL;
	}
#endif
#ifdef POSTGAMMA_EVENT_TIMERS
	if (registry->alarms.count != 0)
		abort();
	free(registry->alarms.entries);
	memset(&registry->alarms, 0, sizeof(registry->alarms));
#endif
	status = stop_pooled_workers(registry);
	if (status != 0)
		return status;
	status = postgamma_wake_target_destroy(registry->wake_target);
	if (status != 0)
		return status;
	status = postgamma_condition_destroy(registry->condition);
	if (status != 0)
		return status;
	status = postgamma_mutex_destroy(registry->mutex);
	if (status != 0)
		return status;
	free(registry->slots);
	free(registry->workers);
	free(registry);
	return 0;
}


PostgammaThreadRole
postgamma_backend_thread_role(PostgammaBackendClass backend_class)
{
	switch (backend_class)
	{
		case POSTGAMMA_BACKEND_CLASS_CLIENT:
			return POSTGAMMA_THREAD_ROLE_CLIENT;
		case POSTGAMMA_BACKEND_CLASS_PARALLEL:
			return POSTGAMMA_THREAD_ROLE_PARALLEL;
		case POSTGAMMA_BACKEND_CLASS_DEDICATED:
			return POSTGAMMA_THREAD_ROLE_DEDICATED;
		case POSTGAMMA_BACKEND_CLASS_COUNT:
			break;
	}
	return POSTGAMMA_THREAD_ROLE_COUNT;
}


int
postgamma_backend_launch(PostgammaBackendRegistry *registry,
					 const PostgammaBackendLaunchRequest *request,
					 PostgammaBackendHandle *handle)
{
	PostgammaBackendExecutionStart *start;
	PostgammaBackendSlot *slot = NULL;
	size_t		allocation_size;
	uint32_t	slot_index = POSTGAMMA_NO_SLOT;
	int			status;

	if (registry == NULL || request == NULL || handle == NULL ||
		request->main_function == NULL || request->backend_type < 0 ||
		request->execution_class < POSTGAMMA_BACKEND_CLASS_DEDICATED ||
		request->execution_class > POSTGAMMA_BACKEND_CLASS_PARALLEL ||
		request->native_wake_signal < 0 ||
		(request->startup_data_length != 0 && request->startup_data == NULL))
		return EINVAL;
	if (request->startup_data_length >
		SIZE_MAX - sizeof(PostgammaBackendExecutionStart))
		return EOVERFLOW;
	allocation_size = sizeof(PostgammaBackendExecutionStart) +
		request->startup_data_length;

	status = lock_registry(registry);
	if (status != 0)
		return status;
	for (uint32_t index = 0; index < registry->capacity; index++)
	{
		if (registry->slots[index].state == POSTGAMMA_BACKEND_SLOT_FREE)
		{
			slot = &registry->slots[index];
			slot_index = index;
			break;
		}
	}
	if (slot == NULL)
	{
		unlock_registry_or_abort(registry);
		return EAGAIN;
	}

	start = calloc(1, allocation_size);
	if (start == NULL)
	{
		unlock_registry_or_abort(registry);
		return ENOMEM;
	}
	status = postgamma_wake_target_create(&slot->wake_target);
	if (status != 0)
	{
		free(start);
		unlock_registry_or_abort(registry);
		return status;
	}

	slot->generation++;
	if (slot->generation == 0)
		slot->generation = 1;
	slot->handle.id = make_backend_id(slot_index, slot->generation);
	slot->handle.compat_pid = allocate_compat_pid(registry);
	slot->handle.backend_type = request->backend_type;
	slot->handle.execution_class = request->execution_class;
	slot->state = POSTGAMMA_BACKEND_SLOT_STARTING;
	slot->thread = NULL;
	slot->start = start;
	slot->interrupts.pending_signals = 0;
	slot->interrupts.pending_events = 0;
#ifdef POSTGAMMA_EVENT_TIMERS
	slot->alarm.at = 0;
	slot->alarm_interval_ns = 0;
#endif
	slot->native_wake_signal = request->native_wake_signal;
	slot->next_completion = POSTGAMMA_NO_SLOT;
	slot->next_runnable = POSTGAMMA_NO_SLOT;
	slot->assigned_worker = POSTGAMMA_NO_SLOT;
	slot->last_worker = POSTGAMMA_NO_SLOT;
	slot->queued_at_ns = 0;
	slot->deadline_ns = 0;
	slot->quantum_sequence = 0;
	slot->schedule_epoch = 0;
	set_pause_requested(slot, false);
	slot->paused = false;
	slot->pooled = false;
	slot->queued = false;
	slot->quantum_running = false;
	slot->completion_delivery_complete = false;
	slot->reschedule_requested = false;
	slot->pinned = false;
	slot->blocked = false;
	slot->execution_token_held = false;
	slot->has_last_worker = false;
	slot->stack_observed = false;
	slot->stack_depth_recorded = false;
	slot->stack_reservation_bytes = 0;

	start->registry = registry;
	start->slot_index = slot_index;
	start->info.handle = slot->handle;
	start->info.wake_target = slot->wake_target;
	start->main_function = request->main_function;
	start->main_argument = request->main_argument;
	start->cleanup_function = request->cleanup_function;
	start->cleanup_argument = request->cleanup_argument;
	start->startup_failure_function = request->startup_failure_function;
	start->completion_notification = request->completion_notification;
	start->completion_notification_argument =
		request->completion_notification_argument;
	start->startup_data_length = request->startup_data_length;
	if (request->startup_data_length != 0)
		memcpy(start->startup_data, request->startup_data,
			   request->startup_data_length);

	slot->state = POSTGAMMA_BACKEND_SLOT_RUNNING;
	registry->active_count++;
	status = registry->provider->launch_locked(registry, slot, request);
	if (status != 0)
	{
		registry->active_count--;
		(void) postgamma_wake_target_destroy(slot->wake_target);
		free(start);
		slot->wake_target = NULL;
		slot->start = NULL;
		slot->native_wake_signal = 0;
		slot->next_runnable = POSTGAMMA_NO_SLOT;
		memset(&slot->handle, 0, sizeof(slot->handle));
		slot->state = POSTGAMMA_BACKEND_SLOT_FREE;
		if (postgamma_condition_broadcast(registry->condition) != 0)
			abort();
		unlock_registry_or_abort(registry);
		return status;
	}

	*handle = start->info.handle;
	unlock_registry_or_abort(registry);
	return 0;
}


int
postgamma_backend_completion_pop(PostgammaBackendRegistry *registry,
								 PostgammaBackendCompletion *completion)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint64_t	wake_count;
	int			status;

	if (registry == NULL || completion == NULL)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	if (registry->completion_head == POSTGAMMA_NO_SLOT)
	{
		(void) postgamma_wake_target_drain(registry->wake_target, &wake_count);
		unlock_registry_or_abort(registry);
		return EAGAIN;
	}
	slot_index = registry->completion_head;
	slot = &registry->slots[slot_index];
	if (slot->state != POSTGAMMA_BACKEND_SLOT_COMPLETED)
	{
		unlock_registry_or_abort(registry);
		return EPROTO;
	}
	registry->completion_head = slot->next_completion;
	if (registry->completion_head == POSTGAMMA_NO_SLOT)
	{
		registry->completion_tail = POSTGAMMA_NO_SLOT;
		(void) postgamma_wake_target_drain(registry->wake_target, &wake_count);
	}
	slot->next_completion = POSTGAMMA_NO_SLOT;
	slot->state = POSTGAMMA_BACKEND_SLOT_DEQUEUED;
	*completion = slot->completion;
	unlock_registry_or_abort(registry);
	return 0;
}


int
postgamma_backend_completion_join(PostgammaBackendRegistry *registry,
								  const PostgammaBackendCompletion *completion)
{
	PostgammaBackendSlot *slot;
	PostgammaWakeTarget *wake_target;
	PostgammaBackendExecutionStart *start;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL || completion == NULL ||
		!decode_backend_id(completion->handle.id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation ||
		slot->handle.id != completion->handle.id)
	{
		unlock_registry_or_abort(registry);
		return ESTALE;
	}
	if (slot->state != POSTGAMMA_BACKEND_SLOT_DEQUEUED ||
		slot->completion.handle.id != completion->handle.id ||
		slot->completion.handle.compat_pid != completion->handle.compat_pid ||
		slot->completion.handle.backend_type != completion->handle.backend_type ||
		slot->completion.handle.execution_class !=
		completion->handle.execution_class ||
		slot->completion.exit_status.kind != completion->exit_status.kind ||
		slot->completion.exit_status.code != completion->exit_status.code)
	{
		unlock_registry_or_abort(registry);
		return EINVAL;
	}
	slot->state = POSTGAMMA_BACKEND_SLOT_JOINING;
	wake_target = slot->wake_target;
	start = slot->start;
	unlock_registry_or_abort(registry);

	status = registry->provider->join(slot);
	if (status != 0)
	{
		if (lock_registry(registry) == 0)
		{
			slot->state = POSTGAMMA_BACKEND_SLOT_DEQUEUED;
			unlock_registry_or_abort(registry);
		}
		return status;
	}
	/* Pooled carriers outlive a session, so their delivery needs its own join. */
	status = lock_registry(registry);
	if (status != 0)
		return status;
	while (slot->state == POSTGAMMA_BACKEND_SLOT_JOINING &&
		   slot->start == start && !slot->completion_delivery_complete)
	{
		status = postgamma_condition_wait(
			registry->condition, registry->mutex);
		if (status != 0)
		{
			slot->state = POSTGAMMA_BACKEND_SLOT_DEQUEUED;
			unlock_registry_or_abort(registry);
			return status;
		}
	}
	if (slot->state != POSTGAMMA_BACKEND_SLOT_JOINING ||
		slot->start != start || !slot->completion_delivery_complete)
	{
		unlock_registry_or_abort(registry);
		return EPROTO;
	}
	unlock_registry_or_abort(registry);

	status = postgamma_wake_target_destroy(wake_target);
	if (status != 0)
		return status;

	status = lock_registry(registry);
	if (status != 0)
		return status;
	if (slot->state != POSTGAMMA_BACKEND_SLOT_JOINING || slot->start != start)
	{
		unlock_registry_or_abort(registry);
		return EPROTO;
	}
	free(start);
	slot->start = NULL;
	slot->thread = NULL;
	slot->wake_target = NULL;
	memset(&slot->handle, 0, sizeof(slot->handle));
	memset(&slot->completion, 0, sizeof(slot->completion));
	slot->interrupts.pending_signals = 0;
	slot->interrupts.pending_events = 0;
#ifdef POSTGAMMA_EVENT_TIMERS
	slot->alarm.at = 0;
	slot->alarm_interval_ns = 0;
#endif
	slot->native_wake_signal = 0;
	slot->next_runnable = POSTGAMMA_NO_SLOT;
	slot->assigned_worker = POSTGAMMA_NO_SLOT;
	slot->last_worker = POSTGAMMA_NO_SLOT;
	slot->queued_at_ns = 0;
	slot->deadline_ns = 0;
	slot->quantum_sequence = 0;
	slot->schedule_epoch = 0;
	set_pause_requested(slot, false);
	slot->paused = false;
	slot->pooled = false;
	slot->queued = false;
	slot->quantum_running = false;
	slot->completion_delivery_complete = false;
	slot->reschedule_requested = false;
	slot->pinned = false;
	slot->blocked = false;
	slot->execution_token_held = false;
	slot->has_last_worker = false;
	slot->stack_observed = false;
	slot->stack_depth_recorded = false;
	slot->stack_reservation_bytes = 0;
	slot->state = POSTGAMMA_BACKEND_SLOT_FREE;
	registry->active_count--;
	unlock_registry_or_abort(registry);
	return 0;
}


int
postgamma_backend_registry_wake_fd(const PostgammaBackendRegistry *registry)
{
	return registry == NULL ? -1 :
		postgamma_wake_target_fd(registry->wake_target);
}


uint32_t
postgamma_backend_registry_active_count(PostgammaBackendRegistry *registry)
{
	uint32_t	count;

	if (registry == NULL || lock_registry(registry) != 0)
		return UINT32_MAX;
	count = registry->active_count;
	unlock_registry_or_abort(registry);
	return count;
}


int
postgamma_backend_registry_lookup(PostgammaBackendRegistry *registry,
							  PostgammaBackendId id,
							  PostgammaBackendHandle *handle,
							  PostgammaBackendSlotState *state)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		slot->state == POSTGAMMA_BACKEND_SLOT_FREE)
	{
		unlock_registry_or_abort(registry);
		return ESTALE;
	}
	if (handle != NULL)
		*handle = slot->handle;
	if (state != NULL)
		*state = slot->state;
	unlock_registry_or_abort(registry);
	return 0;
}


int
postgamma_backend_signal(PostgammaBackendRegistry *registry,
					 PostgammaCompatPid compat_pid,
					 int signal_number)
{
	int			status;

	if (registry == NULL || compat_pid <= 0 || signal_number < 0 ||
		signal_number > POSTGAMMA_BACKEND_SIGNAL_COUNT)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	{
		PostgammaBackendSlot *slot =
			find_running_slot_by_compat_pid(registry, compat_pid);

		if (slot != NULL)
		{
			if (signal_number != 0)
			{
				publish_pending_signals(slot, UINT64_C(1) << (signal_number - 1));
				if (signal_number == SIGQUIT || signal_number == SIGKILL)
				{
					set_pause_requested(slot, false);
					(void) postgamma_condition_broadcast(registry->condition);
				}
				status = wake_backend_slot(registry, slot);
			}
			unlock_registry_or_abort(registry);
			return status;
		}
	}
	unlock_registry_or_abort(registry);
	return ESRCH;
}


int
postgamma_backend_pause(PostgammaBackendRegistry *registry,
						PostgammaCompatPid compat_pid)
{
	PostgammaBackendSlot *slot;
	PostgammaBackendId id;
	int			status;

	if (registry == NULL || compat_pid <= 0)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = find_running_slot_by_compat_pid(registry, compat_pid);
	if (slot == NULL)
	{
		unlock_registry_or_abort(registry);
		return ESRCH;
	}
	id = slot->handle.id;
	set_pause_requested(slot, true);
	status = wake_backend_slot(registry, slot);
	while (status == 0 && slot->handle.id == id &&
		   (slot->state == POSTGAMMA_BACKEND_SLOT_STARTING ||
			slot->state == POSTGAMMA_BACKEND_SLOT_RUNNING) &&
		   __atomic_load_n(&slot->interrupts.pause_requested, __ATOMIC_SEQ_CST) && !slot->paused)
		status = postgamma_condition_wait(registry->condition, registry->mutex);
	if (status == 0 &&
		(slot->handle.id != id ||
		 slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING))
		status = ESRCH;
	else if (status == 0 && !slot->paused)
		status = ECANCELED;
	unlock_registry_or_abort(registry);
	return status;
}


int
postgamma_backend_resume(PostgammaBackendRegistry *registry,
						 PostgammaCompatPid compat_pid)
{
	PostgammaBackendSlot *slot;
	PostgammaBackendId id;
	bool		was_paused;
	int			status;

	if (registry == NULL || compat_pid <= 0)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = find_running_slot_by_compat_pid(registry, compat_pid);
	if (slot == NULL)
	{
		unlock_registry_or_abort(registry);
		return ESRCH;
	}
	id = slot->handle.id;
	was_paused = slot->paused;
	set_pause_requested(slot, false);
	status = postgamma_condition_broadcast(registry->condition);
	while (status == 0 && slot->handle.id == id &&
		   slot->state == POSTGAMMA_BACKEND_SLOT_RUNNING && slot->paused)
		status = postgamma_condition_wait(registry->condition, registry->mutex);
	if (status == 0 && !was_paused &&
		(slot->handle.id != id ||
		 slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING))
		status = ESRCH;
	unlock_registry_or_abort(registry);
	return status;
}


int
postgamma_backend_wait_while_paused(PostgammaBackendRegistry *registry,
									   PostgammaBackendId id)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	slot = current_execution_slot(registry, id);
	if (slot != NULL &&
		!__atomic_load_n(&slot->interrupts.pause_requested, __ATOMIC_ACQUIRE))
		return 0;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING)
	{
		unlock_registry_or_abort(registry);
		return ESTALE;
	}
	if (__atomic_load_n(&slot->interrupts.pause_requested, __ATOMIC_SEQ_CST))
	{
		slot->paused = true;
		(void) postgamma_condition_broadcast(registry->condition);
	}
	while (status == 0 && __atomic_load_n(&slot->interrupts.pause_requested, __ATOMIC_SEQ_CST) &&
		   slot->state == POSTGAMMA_BACKEND_SLOT_RUNNING)
		status = postgamma_condition_wait(registry->condition, registry->mutex);
	if (slot->paused)
	{
		slot->paused = false;
		(void) postgamma_condition_broadcast(registry->condition);
	}
	unlock_registry_or_abort(registry);
	return status;
}


int
postgamma_backend_wake(PostgammaBackendRegistry *registry,
					   PostgammaCompatPid compat_pid)
{
	int			status;

	if (registry == NULL || compat_pid <= 0)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	for (uint32_t index = 0; index < registry->capacity; index++)
	{
		PostgammaBackendSlot *slot = &registry->slots[index];

		if ((slot->state == POSTGAMMA_BACKEND_SLOT_STARTING ||
			 slot->state == POSTGAMMA_BACKEND_SLOT_RUNNING) &&
			slot->handle.compat_pid == compat_pid)
		{
			status = wake_backend_slot(registry, slot);
			unlock_registry_or_abort(registry);
			return status;
		}
	}
	unlock_registry_or_abort(registry);
	return ESRCH;
}


int
postgamma_backend_schedule(
	PostgammaBackendRegistry *registry, PostgammaBackendId id)

{
	return postgamma_backend_notify(registry, id, true);
}


int
postgamma_backend_notify(
	PostgammaBackendRegistry *registry, PostgammaBackendId id, bool runnable)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING)
		status = ESTALE;
	else
	{
		if (runnable)
			status = schedule_slot_locked(registry, slot);
		else
		{
			status = postgamma_wake_target_wake(slot->wake_target);
			if (status == 0 && !slot->pooled &&
				slot->native_wake_signal != 0)
				status = postgamma_thread_signal(
					slot->thread, slot->native_wake_signal);
		}
	}
	unlock_registry_or_abort(registry);
	return status;
}


int
postgamma_backend_schedule_epoch(
	PostgammaBackendRegistry *registry, PostgammaBackendId id, uint64_t *epoch)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL || epoch == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING ||
		!slot->pooled || !slot->quantum_running)
		status = ESTALE;
	else
		*epoch = slot->schedule_epoch;
	unlock_registry_or_abort(registry);
	return status;
}


int
postgamma_backend_prepare_yield(
	PostgammaBackendRegistry *registry, PostgammaBackendId id,
	uint64_t observed_schedule_epoch, bool runnable)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING ||
		!slot->pooled || !slot->quantum_running)
		status = ESTALE;
	else if (slot->schedule_epoch != observed_schedule_epoch)
		status = EAGAIN;
	else
		slot->reschedule_requested = runnable;
	unlock_registry_or_abort(registry);
	return status;
}


int
postgamma_backend_set_pinned(
	PostgammaBackendRegistry *registry, PostgammaBackendId id, bool pinned)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING)
		status = ESTALE;
	else if (slot->pinned != pinned)
	{
		slot->pinned = pinned;
		if (pinned)
		{
			registry->telemetry.pinned_sessions++;
			update_peak(
				&registry->telemetry.pinned_sessions_peak,
				registry->telemetry.pinned_sessions);
		}
		else if (registry->telemetry.pinned_sessions == 0)
			status = EPROTO;
		else
			registry->telemetry.pinned_sessions--;
	}
	unlock_registry_or_abort(registry);
	return status;
}


int
postgamma_backend_set_blocked(
	PostgammaBackendRegistry *registry, PostgammaBackendId id, bool blocked)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING)
		status = ESTALE;
	else if (slot->blocked != blocked)
	{
		slot->blocked = blocked;
		if (blocked)
		{
			registry->telemetry.blocked_sessions++;
			update_peak(
				&registry->telemetry.blocked_sessions_peak,
				registry->telemetry.blocked_sessions);
		}
		else if (registry->telemetry.blocked_sessions == 0)
			status = EPROTO;
		else
			registry->telemetry.blocked_sessions--;
	}
	unlock_registry_or_abort(registry);
	return status;
}


int
postgamma_backend_set_deadline(
	PostgammaBackendRegistry *registry, PostgammaBackendId id,
	uint64_t deadline_ns)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING)
		status = ESTALE;
	else
	{
		slot->deadline_ns = deadline_ns;
		status = postgamma_condition_broadcast(registry->condition);
	}
	unlock_registry_or_abort(registry);
	return status;
}


#ifdef POSTGAMMA_EVENT_TIMERS
static void *
alarm_owner_main(void *argument)
{
	PostgammaBackendRegistry *registry = argument;

	if (lock_registry(registry) != 0)
		abort();
	while (!registry->alarm_stopping)
	{
		uint64_t now = postgamma_monotonic_now_ns();
		uint64_t next = UINT64_MAX;
		int status;

		while (registry->alarms.count != 0)
		{
			PostgammaDeadline *entry = registry->alarms.entries[0];
			PostgammaBackendSlot *slot = (PostgammaBackendSlot *)
				((char *) entry - offsetof(PostgammaBackendSlot, alarm));
			uint64_t due = entry->at;
			uint64_t interval = slot->alarm_interval_ns;
			uint64_t following = 0;

			if (slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING)
				abort();
			if (due > now)
			{
				next = due;
				break;
			}
			if (interval != 0)
			{
				uint64_t periods = (now - due) / interval;

				if (periods < UINT64_MAX &&
					periods + 1 <= (UINT64_MAX - due) / interval)
					following = due + (periods + 1) * interval;
			}
			postgamma_deadline_set(&registry->alarms, entry, following);
			publish_pending_signals(slot, UINT64_C(1) << (SIGALRM - 1));
			if (wake_backend_slot(registry, slot) != 0)
				abort();
		}
		registry->alarm_wait_deadline = next;
		status = next == UINT64_MAX ?
			postgamma_condition_wait(registry->alarm_condition, registry->mutex) :
			postgamma_condition_timed_wait(registry->alarm_condition, registry->mutex, next);
		if (status != 0 && status != ETIMEDOUT)
			abort();
	}
	unlock_registry_or_abort(registry);
	return NULL;
}

int
postgamma_backend_set_alarm(PostgammaBackendRegistry *registry,
	PostgammaBackendId id, uint64_t deadline_ns, uint64_t interval_ns)
{
	PostgammaBackendSlot *slot;
	uint32_t index, generation;
	int status;

	if (registry == NULL || !decode_backend_id(id, &index, &generation) ||
		index >= registry->capacity)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[index];
	if (registry->alarm_stopping || slot->generation != generation ||
		slot->handle.id != id || slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING)
		status = ESTALE;
	else
	{
		if (status == 0)
		{
			uint64_t previous_deadline = slot->alarm.at;
			uint64_t previous_interval = slot->alarm_interval_ns;

			postgamma_deadline_set(&registry->alarms, &slot->alarm, deadline_ns);
			slot->alarm_interval_ns = interval_ns;
			/* Later/canceled deadlines may wake early, but cannot miss an alarm. */
			if (deadline_ns != 0 && deadline_ns < registry->alarm_wait_deadline)
				status = postgamma_condition_signal(registry->alarm_condition);
			if (status != 0)
			{
				/* The owner cannot observe a rejected arm while we hold its lock. */
				postgamma_deadline_set(&registry->alarms, &slot->alarm, previous_deadline);
				slot->alarm_interval_ns = previous_interval;
			}
		}
	}
	unlock_registry_or_abort(registry);
	return status;
}
#endif

bool
postgamma_backend_interrupt_poll_required(
	PostgammaBackendRegistry *registry, PostgammaBackendId id)
{
	PostgammaBackendSlot *slot = current_execution_slot(registry, id);

	return slot == NULL || postgamma_backend_interrupts_pending(&slot->interrupts);
}


int
postgamma_backend_take_pending_signals(PostgammaBackendRegistry *registry,
									 PostgammaBackendId id,
									 uint64_t *pending)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL || pending == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	slot = current_execution_slot(registry, id);
	if (slot != NULL)
	{
		*pending = take_pending_signals(slot);
		return 0;
	}
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		(slot->state != POSTGAMMA_BACKEND_SLOT_STARTING &&
		 slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING))
	{
		unlock_registry_or_abort(registry);
		return ESTALE;
	}
	*pending = take_pending_signals(slot);
	unlock_registry_or_abort(registry);
	return 0;
}


int
postgamma_backend_requeue_pending_signals(PostgammaBackendRegistry *registry,
										PostgammaBackendId id,
										uint64_t pending)
{
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity)
		return EINVAL;
	if (pending == 0)
		return 0;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		(slot->state != POSTGAMMA_BACKEND_SLOT_STARTING &&
		 slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING))
	{
		unlock_registry_or_abort(registry);
		return ESTALE;
	}
	publish_pending_signals(slot, pending);
	unlock_registry_or_abort(registry);
	return 0;
}


void
postgamma_backend_registry_record_process_launch(PostgammaBackendRegistry *registry)
{
	if (registry != NULL && lock_registry(registry) == 0)
	{
		registry->telemetry.backend_process_launches++;
		unlock_registry_or_abort(registry);
	}
}


void
postgamma_backend_registry_record_forbidden_process_launch(
	PostgammaBackendRegistry *registry)
{
	if (registry != NULL && lock_registry(registry) == 0)
	{
		registry->telemetry.forbidden_process_launch_attempts++;
		unlock_registry_or_abort(registry);
	}
}


void
postgamma_backend_registry_record_unsupported_request(
	PostgammaBackendRegistry *registry)
{
	if (registry != NULL && lock_registry(registry) == 0)
	{
		registry->telemetry.unsupported_backend_requests++;
		unlock_registry_or_abort(registry);
	}
}


int
postgamma_backend_registry_record_stack_validation(
	PostgammaBackendRegistry *registry, PostgammaBackendId id,
	size_t configured_depth, size_t available_depth, bool valid)
{
	PostgammaBackendStackTelemetry *stack;
	PostgammaBackendSlot *slot;
	uint32_t	slot_index;
	uint32_t	generation;
	int			status;

	if (registry == NULL ||
		!decode_backend_id(id, &slot_index, &generation) ||
		slot_index >= registry->capacity ||
		(valid && (configured_depth == 0 ||
			available_depth < configured_depth)))
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	slot = &registry->slots[slot_index];
	if (slot->generation != generation || slot->handle.id != id ||
		(slot->state != POSTGAMMA_BACKEND_SLOT_STARTING &&
		 slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING))
	{
		unlock_registry_or_abort(registry);
		return ESTALE;
	}
	if (slot->stack_depth_recorded)
	{
		unlock_registry_or_abort(registry);
		return EALREADY;
	}
	stack = &registry->telemetry.stack[slot->handle.execution_class];
	if (stack->depth_validations == UINT64_MAX ||
		(!valid && stack->depth_validation_failures == UINT64_MAX))
	{
		unlock_registry_or_abort(registry);
		return EOVERFLOW;
	}
	stack->depth_validations++;
	if (valid)
	{
		update_minimum(&stack->minimum_depth_limit, available_depth);
		if (configured_depth > stack->maximum_configured_depth)
			stack->maximum_configured_depth = configured_depth;
	}
	else
		stack->depth_validation_failures++;
	slot->stack_depth_recorded = true;
	unlock_registry_or_abort(registry);
	return 0;
}


int
postgamma_backend_registry_telemetry(PostgammaBackendRegistry *registry,
									PostgammaBackendTelemetry *telemetry)
{
	int			status;

	if (registry == NULL || telemetry == NULL)
		return EINVAL;
	status = lock_registry(registry);
	if (status != 0)
		return status;
	*telemetry = registry->telemetry;
	unlock_registry_or_abort(registry);
	return 0;
}


static PostgammaBackendId
make_backend_id(uint32_t slot_index, uint32_t generation)
{
	return ((uint64_t) generation << 32) | ((uint64_t) slot_index + 1);
}


static bool
decode_backend_id(PostgammaBackendId id, uint32_t *slot_index,
				  uint32_t *generation)
{
	uint32_t	encoded_slot = (uint32_t) (id & POSTGAMMA_BACKEND_ID_SLOT_MASK);
	uint32_t	encoded_generation = (uint32_t) (id >> 32);

	if (id == 0 || encoded_slot == 0 || encoded_generation == 0)
		return false;
	*slot_index = encoded_slot - 1;
	*generation = encoded_generation;
	return true;
}


static PostgammaCompatPid
allocate_compat_pid(PostgammaBackendRegistry *registry)
{
	PostgammaCompatPid candidate;
	bool		collision;

	do
	{
		candidate = registry->next_compat_pid++;
		if (registry->next_compat_pid <= 0 ||
			registry->next_compat_pid == INT32_MAX)
			registry->next_compat_pid = POSTGAMMA_COMPAT_PID_FIRST;
		collision = false;
		for (uint32_t index = 0; index < registry->capacity; index++)
		{
			if (registry->slots[index].state != POSTGAMMA_BACKEND_SLOT_FREE &&
				registry->slots[index].handle.compat_pid == candidate)
			{
				collision = true;
				break;
			}
		}
	} while (collision);
	return candidate;
}


static int
dedicated_provider_launch_locked(PostgammaBackendRegistry *registry,
								 PostgammaBackendSlot *slot,
								 const PostgammaBackendLaunchRequest *request)
{
	PostgammaThreadAttributes attributes;
	PostgammaThread *thread = NULL;
	int			status;

	attributes.name = request->thread_name;
	attributes.role = postgamma_backend_thread_role(request->execution_class);
	attributes.stack_size = request->stack_size;
	attributes.guard_size = request->guard_size;
	/*
	 * Reserve the slot while creating its carrier outside the registry lock.
	 * active_count prevents destruction and STARTING prevents completion or
	 * reuse.  The child waits for publication below before calling user code.
	 */
	slot->state = POSTGAMMA_BACKEND_SLOT_STARTING;
	unlock_registry_or_abort(registry);
	if (request->startup_failure_function != NULL)
		status = postgamma_thread_create_async(&thread, &attributes,
			backend_thread_main, backend_thread_start_failed, slot->start);
	else
		status = postgamma_thread_create(&thread, &attributes,
			backend_thread_main, slot->start);
	if (lock_registry(registry) != 0)
		abort();
	if (status != 0)
		return status;
	slot->thread = thread;

	registry->telemetry.threads_started = true;
	registry->telemetry.backend_threads_active++;
	/* Asynchronous carriers record their stack after validating it themselves. */
	if (request->startup_failure_function == NULL)
		record_backend_stack_locked(registry, slot);
	if (request->execution_class == POSTGAMMA_BACKEND_CLASS_CLIENT)
	{
		registry->telemetry.client_threads_started++;
		registry->client_threads_active++;
		update_peak(&registry->telemetry.client_threads_peak,
					registry->client_threads_active);
	}
	else if (request->execution_class == POSTGAMMA_BACKEND_CLASS_PARALLEL)
		registry->telemetry.parallel_threads_started++;
	else
		registry->telemetry.dedicated_threads_started++;
	if (request->startup_failure_function == NULL)
		slot->state = POSTGAMMA_BACKEND_SLOT_RUNNING;
	if (postgamma_condition_broadcast(registry->condition) != 0)
		abort();
	return 0;
}


static int
dedicated_provider_join(PostgammaBackendSlot *slot)
{
	int			status;

	if (slot == NULL || slot->thread == NULL)
		return EPROTO;
	status = postgamma_thread_join(slot->thread, NULL);
	if (status == 0)
		status = postgamma_thread_destroy(slot->thread);
	return status;
}


static int
pooled_provider_launch_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot,
	const PostgammaBackendLaunchRequest *request)
{
	int			status;

	if (request->execution_class != POSTGAMMA_BACKEND_CLASS_CLIENT)
	{
		if (request->execution_class == POSTGAMMA_BACKEND_CLASS_PARALLEL &&
			!acquire_execution_token_locked(registry, slot))
		{
			registry->telemetry.execution_token_rejections++;
			return EAGAIN;
		}
		status = dedicated_provider_launch_locked(registry, slot, request);
		if (status != 0 && slot->execution_token_held)
			release_execution_token_locked(registry, slot);
		return status;
	}
	slot->pooled = true;
	slot->assigned_worker = registry->next_worker++ % registry->worker_count;
	return enqueue_slot_locked(registry, slot);
}


static int
pooled_provider_join(PostgammaBackendSlot *slot)
{
	if (slot == NULL)
		return EINVAL;
	return slot->pooled ? 0 : dedicated_provider_join(slot);
}


static int
start_pooled_workers(PostgammaBackendRegistry *registry)
{
	PostgammaThreadAttributes attributes = {
		.role = POSTGAMMA_THREAD_ROLE_CLIENT,
	};
	int			status = 0;

	registry->workers = calloc(
		registry->worker_count, sizeof(*registry->workers));
	if (registry->workers == NULL)
		return ENOMEM;
	for (uint32_t index = 0; index < registry->worker_count; index++)
	{
		registry->workers[index].registry = registry;
		registry->workers[index].index = index;
		attributes.name = "pg-pool";
		status = postgamma_thread_create(
			&registry->workers[index].thread, &attributes,
			pooled_worker_main, &registry->workers[index]);
		if (status != 0)
			break;
		registry->telemetry.threads_started = true;
		registry->telemetry.client_threads_started++;
		registry->telemetry.pooled_worker_threads++;
	}
	if (status != 0)
	{
		int stop_status = stop_pooled_workers(registry);

		if (stop_status != 0)
			return stop_status;
	}
	return status;
}


static int
stop_pooled_workers(PostgammaBackendRegistry *registry)
{
	int			status = 0;

	if (registry == NULL || registry->provider->kind !=
		POSTGAMMA_BACKEND_PROVIDER_POOLED || registry->workers == NULL)
		return 0;
	if (lock_registry(registry) != 0)
		return EBUSY;
	registry->stopping = true;
	if (postgamma_condition_broadcast(registry->condition) != 0)
		status = EIO;
	unlock_registry_or_abort(registry);
	for (uint32_t index = 0; index < registry->worker_count; index++)
	{
		PostgammaThread *thread = registry->workers[index].thread;
		int operation_status;

		if (thread == NULL)
			continue;
		operation_status = postgamma_thread_join(thread, NULL);
		if (operation_status == 0)
			operation_status = postgamma_thread_destroy(thread);
		if (status == 0 && operation_status != 0)
			status = operation_status;
		if (operation_status == 0)
			registry->workers[index].thread = NULL;
	}
	return status;
}


static int
enqueue_slot_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot)
{
	uint32_t	slot_index;

	if (slot->queued || slot->quantum_running || !slot->pooled ||
		slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING)
		return EINVAL;
	if (registry->runnable_depth >= registry->queue_capacity)
		return EAGAIN;
	slot_index = (uint32_t) (slot - registry->slots);
	slot->next_runnable = POSTGAMMA_NO_SLOT;
	slot->queued = true;
	slot->queued_at_ns = postgamma_monotonic_now_ns();
	if (registry->runnable_tail == POSTGAMMA_NO_SLOT)
		registry->runnable_head = slot_index;
	else
		registry->slots[registry->runnable_tail].next_runnable = slot_index;
	registry->runnable_tail = slot_index;
	registry->runnable_depth++;
	registry->telemetry.runnable_sessions = registry->runnable_depth;
	update_peak(
		&registry->telemetry.runnable_sessions_peak,
		registry->telemetry.runnable_sessions);
	return postgamma_condition_broadcast(registry->condition);
}


static PostgammaBackendSlot *
dequeue_slot_for_worker_locked(
	PostgammaBackendRegistry *registry, uint32_t worker_index)
{
	uint32_t	previous = POSTGAMMA_NO_SLOT;
	uint32_t	current = registry->runnable_head;
	uint32_t	steal_previous = POSTGAMMA_NO_SLOT;
	uint32_t	steal_current = POSTGAMMA_NO_SLOT;

	if (registry->execution_tokens_active >= registry->execution_token_count)
		return NULL;

	while (current != POSTGAMMA_NO_SLOT)
	{
		PostgammaBackendSlot *slot = &registry->slots[current];

		if (slot->assigned_worker == worker_index)
			return remove_runnable_slot_locked(
				registry, previous, current, false);
		if (steal_current == POSTGAMMA_NO_SLOT &&
			(slot->assigned_worker >= registry->worker_count ||
			 registry->workers[slot->assigned_worker].running))
		{
			steal_previous = previous;
			steal_current = current;
		}
		previous = current;
		current = slot->next_runnable;
	}
	if (steal_current != POSTGAMMA_NO_SLOT)
		return remove_runnable_slot_locked(
			registry, steal_previous, steal_current, true);
	return NULL;
}


static PostgammaBackendSlot *
remove_runnable_slot_locked(
	PostgammaBackendRegistry *registry, uint32_t previous, uint32_t current,
	bool stolen)
{
	PostgammaBackendSlot *slot = &registry->slots[current];

	if (previous == POSTGAMMA_NO_SLOT)
		registry->runnable_head = slot->next_runnable;
	else
		registry->slots[previous].next_runnable = slot->next_runnable;
	if (registry->runnable_tail == current)
		registry->runnable_tail = previous;
	slot->next_runnable = POSTGAMMA_NO_SLOT;
	slot->queued = false;
	registry->runnable_depth--;
	registry->telemetry.runnable_sessions = registry->runnable_depth;
	if (!acquire_execution_token_locked(registry, slot))
		abort();
	if (stolen)
		registry->telemetry.work_steals++;
	return slot;
}


static uint64_t
schedule_due_slots_locked(
	PostgammaBackendRegistry *registry, uint64_t now_ns)
{
	uint64_t	next_deadline = 0;

	for (uint32_t index = 0; index < registry->capacity; index++)
	{
		PostgammaBackendSlot *slot = &registry->slots[index];

		if (!slot->pooled || slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING ||
			slot->queued || slot->quantum_running || slot->deadline_ns == 0)
			continue;
		if (slot->deadline_ns <= now_ns)
		{
			slot->deadline_ns = 0;
			if (enqueue_slot_locked(registry, slot) != 0)
				abort();
		}
		else if (next_deadline == 0 || slot->deadline_ns < next_deadline)
			next_deadline = slot->deadline_ns;
	}
	return next_deadline;
}


static int
schedule_slot_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot)
{
	int			status;

	/* Signals and ordinary wakes must also invalidate a pending idle decision. */
	if (slot->schedule_epoch == UINT64_MAX)
		return EOVERFLOW;
	slot->schedule_epoch++;
	status = postgamma_wake_target_wake(slot->wake_target);
	if (status != 0)
		return status;
	if (!slot->pooled)
	{
		/* The wake fd and pending bits already retain signals during startup. */
		if (slot->state == POSTGAMMA_BACKEND_SLOT_RUNNING &&
			slot->native_wake_signal != 0)
			return postgamma_thread_signal(
				slot->thread, slot->native_wake_signal);
		return 0;
	}
	slot->deadline_ns = 0;
	if (slot->quantum_running)
	{
		slot->reschedule_requested = true;
		return 0;
	}
	if (slot->queued)
		return 0;
	return enqueue_slot_locked(registry, slot);
}


static void *
backend_thread_main(void *argument)
{
	PostgammaBackendExecutionStart *start = argument;
	PostgammaBackendExitStatus exit_status;

	begin_backend_thread(start);
	exit_status = execute_backend(start, 0, 1);

	if (exit_status.kind == POSTGAMMA_BACKEND_EXIT_YIELD)
	{
		exit_status.kind = POSTGAMMA_BACKEND_EXIT_PANIC;
		exit_status.code = EPROTO;
	}
	complete_backend_execution(start, exit_status, false, 0, false);
	return NULL;
}


static void
begin_backend_thread(PostgammaBackendExecutionStart *start)
{
	PostgammaBackendRegistry *registry = start->registry;
	PostgammaBackendSlot *slot = &registry->slots[start->slot_index];

	if (lock_registry(registry) != 0)
		abort();
	/* The creator publishes the handle before any completion can reclaim it. */
	while (slot->state == POSTGAMMA_BACKEND_SLOT_STARTING && slot->thread == NULL)
		if (postgamma_condition_wait(registry->condition, registry->mutex) != 0)
			abort();
	if (slot->state == POSTGAMMA_BACKEND_SLOT_STARTING)
	{
		record_backend_stack_locked(registry, slot);
		slot->state = POSTGAMMA_BACKEND_SLOT_RUNNING;
		if (postgamma_condition_broadcast(registry->condition) != 0)
			abort();
	}
	if (slot->start != start || slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING)
		abort();
	unlock_registry_or_abort(registry);
}


static void *
backend_thread_start_failed(int status, void *argument)
{
	PostgammaBackendExecutionStart *start = argument;
	PostgammaBackendExecutionStart *previous = PostgammaCurrentBackendExecution;
	PostgammaBackendExitStatus exit_status = {POSTGAMMA_BACKEND_EXIT_FATAL, 1};

	begin_backend_thread(start);
	start->info.stack_status = status;
	PostgammaCurrentBackendExecution = start;
	complete_backend_execution(start, exit_status, false, 0, true);
	PostgammaCurrentBackendExecution = previous;
	return NULL;
}


static void *
pooled_worker_main(void *argument)
{
	PostgammaPooledWorker *worker = argument;
	PostgammaBackendRegistry *registry = worker->registry;
	PostgammaThreadStackInfo stack_info;
	int			stack_status;

	stack_status = postgamma_thread_current_stack_info(&stack_info);
	for (;;)
	{
		PostgammaBackendSlot *slot;
		PostgammaBackendExecutionStart *start;
		PostgammaBackendExitStatus exit_status;
		uint64_t	now;
		uint64_t	wait_ns;
		int			status;

		status = lock_registry(registry);
		if (status != 0)
			abort();
		for (;;)
		{
			uint64_t next_deadline = schedule_due_slots_locked(
				registry, postgamma_monotonic_now_ns());

			slot = dequeue_slot_for_worker_locked(registry, worker->index);
			if (slot != NULL || registry->stopping)
				break;
			status = next_deadline == 0 ?
				postgamma_condition_wait(
					registry->condition, registry->mutex) :
				postgamma_condition_timed_wait(
					registry->condition, registry->mutex, next_deadline);
			if (status != 0 && status != ETIMEDOUT)
				abort();
		}
		if (slot == NULL)
		{
			unlock_registry_or_abort(registry);
			break;
		}
		if (!slot->pooled || slot->quantum_running ||
			slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING ||
			slot->start == NULL)
			abort();
		start = slot->start;
		slot->quantum_running = true;
		worker->running = true;
		slot->reschedule_requested = false;
		slot->quantum_sequence++;
		now = postgamma_monotonic_now_ns();
		wait_ns = now >= slot->queued_at_ns ? now - slot->queued_at_ns : 0;
		if (UINT64_MAX - registry->telemetry.queue_wait_ns_total < wait_ns)
			registry->telemetry.queue_wait_ns_total = UINT64_MAX;
		else
			registry->telemetry.queue_wait_ns_total += wait_ns;
		update_peak(&registry->telemetry.queue_wait_ns_max, wait_ns);
		registry->telemetry.client_quantums++;
		registry->telemetry.running_quantums++;
		update_peak(
			&registry->telemetry.running_quantums_peak,
			registry->telemetry.running_quantums);
		registry->telemetry.backend_threads_active++;
		registry->client_threads_active++;
		update_peak(
			&registry->telemetry.client_threads_peak,
			registry->client_threads_active);
		if (slot->has_last_worker && slot->last_worker != worker->index)
			registry->telemetry.carrier_migrations++;
		if (stack_status != 0 ||
			stack_info.role != POSTGAMMA_THREAD_ROLE_CLIENT)
			record_stack_failure(
				&registry->telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
				observation_failures);
		else
		{
			status = record_stack_launch_locked(
				registry, POSTGAMMA_BACKEND_CLASS_CLIENT, &stack_info);
			if (status == 0)
			{
				slot->stack_observed = true;
				slot->stack_reservation_bytes =
					stack_info.configured_stack_size;
			}
			else
				record_stack_failure(
					&registry->telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
					accounting_failures);
		}
		unlock_registry_or_abort(registry);

		exit_status = execute_backend(
			start, worker->index + 1, slot->quantum_sequence);
		complete_backend_execution(
			start, exit_status, true, worker->index, false);
		/* Make accidental carrier-state dependencies visible on the next bind. */
		errno = (int) (EDOM + (worker->index % 3));
	}
	return NULL;
}


static PostgammaBackendExitStatus
execute_backend(
	PostgammaBackendExecutionStart *start, uint32_t carrier_index,
	uint64_t quantum_sequence)
{
	PostgammaBackendExitStatus exit_status;
	PostgammaBackendExecutionStart *previous = PostgammaCurrentBackendExecution;

	start->info.carrier_index = carrier_index;
	start->info.quantum_sequence = quantum_sequence;
	start->info.stack_status = postgamma_thread_current_stack_info(
		&start->info.stack_info);
	PostgammaCurrentBackendExecution = start;
	exit_status = start->main_function(
		&start->info, start->startup_data, start->startup_data_length,
		start->main_argument);
	PostgammaCurrentBackendExecution = previous;
	if (exit_status.kind < POSTGAMMA_BACKEND_EXIT_NORMAL ||
		exit_status.kind > POSTGAMMA_BACKEND_EXIT_UNEXPECTED_RETURN)
	{
		exit_status.kind = POSTGAMMA_BACKEND_EXIT_PANIC;
		exit_status.code = EPROTO;
	}
	return exit_status;
}


static void
complete_backend_execution(
	PostgammaBackendExecutionStart *start,
	PostgammaBackendExitStatus exit_status, bool pooled_quantum,
	uint32_t worker_index, bool startup_failed)
{
	PostgammaBackendRegistry *registry = start->registry;
	PostgammaBackendSlot *slot = &registry->slots[start->slot_index];
	PostgammaBackendCompletionNotify completion_notification =
		start->completion_notification;
	void	   *completion_notification_argument =
		start->completion_notification_argument;
	int			cleanup_status;
	PostgammaBackendCleanup cleanup = startup_failed ?
		start->startup_failure_function : start->cleanup_function;

	if (exit_status.kind != POSTGAMMA_BACKEND_EXIT_YIELD &&
		cleanup != NULL)
	{
		cleanup_status = cleanup(
			&start->info, start->cleanup_argument);
		if (cleanup_status != 0)
		{
			exit_status.kind = POSTGAMMA_BACKEND_EXIT_PANIC;
			exit_status.code = cleanup_status;
		}
	}

	if (lock_registry(registry) != 0)
		abort();
	if (slot->handle.id != start->info.handle.id ||
		slot->state != POSTGAMMA_BACKEND_SLOT_RUNNING ||
		(pooled_quantum && !slot->quantum_running))
		abort();
	if (pooled_quantum)
	{
		slot->quantum_running = false;
		registry->workers[worker_index].running = false;
		if (registry->telemetry.running_quantums == 0)
			abort();
		registry->telemetry.running_quantums--;
	}
	if (slot->execution_token_held)
		release_execution_token_locked(registry, slot);
	if (registry->telemetry.backend_threads_active == 0)
		abort();
	registry->telemetry.backend_threads_active--;
	record_stack_completion_locked(registry, slot);
	if (slot->handle.execution_class == POSTGAMMA_BACKEND_CLASS_CLIENT)
	{
		if (registry->client_threads_active == 0)
			abort();
		registry->client_threads_active--;
	}
	if (exit_status.kind == POSTGAMMA_BACKEND_EXIT_YIELD)
	{
		if (!pooled_quantum || !slot->pooled)
			abort();
		registry->telemetry.quantum_yields++;
		slot->last_worker = worker_index;
		slot->has_last_worker = true;
		slot->assigned_worker =
			(worker_index + 1) % registry->worker_count;
		if (slot->reschedule_requested)
		{
			slot->reschedule_requested = false;
			if (enqueue_slot_locked(registry, slot) != 0)
				abort();
		}
		(void) postgamma_condition_broadcast(registry->condition);
		unlock_registry_or_abort(registry);
		return;
	}
	if (slot->pinned)
	{
		if (registry->telemetry.pinned_sessions == 0)
			abort();
		registry->telemetry.pinned_sessions--;
		slot->pinned = false;
	}
	if (slot->blocked)
	{
		if (registry->telemetry.blocked_sessions == 0)
			abort();
		registry->telemetry.blocked_sessions--;
		slot->blocked = false;
	}
#ifdef POSTGAMMA_EVENT_TIMERS
	/* Remove the old generation before completion can publish slot reuse. */
	postgamma_deadline_set(&registry->alarms, &slot->alarm, 0);
#endif
	slot->completion.handle = slot->handle;
	slot->completion.exit_status = exit_status;
	set_pause_requested(slot, false);
	slot->paused = false;
	slot->queued = false;
	slot->reschedule_requested = false;
	slot->state = POSTGAMMA_BACKEND_SLOT_COMPLETED;
	if (postgamma_condition_broadcast(registry->condition) != 0)
		abort();
	slot->next_completion = POSTGAMMA_NO_SLOT;
	if (registry->completion_tail == POSTGAMMA_NO_SLOT)
		registry->completion_head = start->slot_index;
	else
		registry->slots[registry->completion_tail].next_completion =
			start->slot_index;
	registry->completion_tail = start->slot_index;
	registry->telemetry.backend_completions++;
	unlock_registry_or_abort(registry);
	if (postgamma_wake_target_wake(registry->wake_target) != 0)
		abort();
	if (completion_notification != NULL)
		completion_notification(completion_notification_argument);

	/* Join may reclaim start only after every out-of-lock delivery action. */
	if (lock_registry(registry) != 0)
		abort();
	if (slot->start != start || slot->completion_delivery_complete ||
		(slot->state != POSTGAMMA_BACKEND_SLOT_COMPLETED &&
		 slot->state != POSTGAMMA_BACKEND_SLOT_DEQUEUED &&
		 slot->state != POSTGAMMA_BACKEND_SLOT_JOINING))
		abort();
	slot->completion_delivery_complete = true;
	if (postgamma_condition_broadcast(registry->condition) != 0)
		abort();
	unlock_registry_or_abort(registry);
}


static bool
acquire_execution_token_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot)
{
	if (slot->execution_token_held ||
		registry->execution_tokens_active >= registry->execution_token_count)
		return false;
	slot->execution_token_held = true;
	registry->execution_tokens_active++;
	registry->telemetry.execution_tokens_active =
		registry->execution_tokens_active;
	update_peak(
		&registry->telemetry.execution_tokens_peak,
		registry->telemetry.execution_tokens_active);
	return true;
}


static void
release_execution_token_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot)
{
	if (!slot->execution_token_held || registry->execution_tokens_active == 0)
		abort();
	slot->execution_token_held = false;
	registry->execution_tokens_active--;
	registry->telemetry.execution_tokens_active =
		registry->execution_tokens_active;
	(void) postgamma_condition_broadcast(registry->condition);
}


static void
update_peak(uint64_t *peak, uint64_t candidate)
{
	if (candidate > *peak)
		*peak = candidate;
}


static void
update_minimum(size_t *minimum, size_t candidate)
{
	if (*minimum == 0 || candidate < *minimum)
		*minimum = candidate;
}


static void
record_backend_stack_locked(PostgammaBackendRegistry *registry,
							PostgammaBackendSlot *slot)
{
	PostgammaThreadStackInfo stack_info;
	PostgammaBackendClass backend_class = slot->handle.execution_class;

	if (postgamma_thread_stack_info(slot->thread, &stack_info) != 0 ||
		stack_info.role != postgamma_backend_thread_role(backend_class))
		record_stack_failure(
			&registry->telemetry.stack[backend_class].observation_failures);
	else if (record_stack_launch_locked(registry, backend_class, &stack_info) != 0)
		record_stack_failure(
			&registry->telemetry.stack[backend_class].accounting_failures);
	else
	{
		slot->stack_observed = true;
		slot->stack_reservation_bytes = stack_info.configured_stack_size;
	}
}


static int
record_stack_launch_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendClass backend_class,
	const PostgammaThreadStackInfo *stack_info)
{
	PostgammaBackendStackTelemetry *stack =
		&registry->telemetry.stack[backend_class];

	if (stack->observations == UINT64_MAX ||
		UINT64_MAX - stack->active_reservation_bytes <
		stack_info->configured_stack_size)
		return EOVERFLOW;
	stack->observations++;
	stack->active_reservation_bytes += stack_info->configured_stack_size;
	update_peak(
		&stack->peak_reservation_bytes,
		stack->active_reservation_bytes);
	update_minimum(
		&stack->minimum_configured_stack_size,
		stack_info->configured_stack_size);
	if (stack_info->configured_stack_size >
		stack->maximum_configured_stack_size)
		stack->maximum_configured_stack_size =
			stack_info->configured_stack_size;
	update_minimum(
		&stack->minimum_configured_guard_size,
		stack_info->configured_guard_size);
	update_minimum(
		&stack->minimum_native_stack_size,
		stack_info->native_stack_size);
	update_minimum(
		&stack->minimum_native_guard_size,
		stack_info->native_guard_size);
	update_minimum(
		&stack->minimum_usable_stack_size,
		stack_info->usable_stack_size);
	return 0;
}


static void
record_stack_completion_locked(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot)
{
	PostgammaBackendStackTelemetry *stack =
		&registry->telemetry.stack[slot->handle.execution_class];

	if (!slot->stack_observed)
		return;
	if (slot->stack_reservation_bytes == 0 ||
		stack->active_reservation_bytes < slot->stack_reservation_bytes)
	{
		record_stack_failure(&stack->accounting_failures);
		stack->active_reservation_bytes = 0;
	}
	else
		stack->active_reservation_bytes -= slot->stack_reservation_bytes;
	slot->stack_observed = false;
	slot->stack_reservation_bytes = 0;
}


static void
record_stack_failure(uint64_t *failures)
{
	if (*failures != UINT64_MAX)
		(*failures)++;
}


static int
lock_registry(PostgammaBackendRegistry *registry)
{
	return postgamma_mutex_lock(registry->mutex);
}


static void
unlock_registry_or_abort(PostgammaBackendRegistry *registry)
{
	if (postgamma_mutex_unlock(registry->mutex) != 0)
		abort();
}


static int
wake_backend_slot(
	PostgammaBackendRegistry *registry, PostgammaBackendSlot *slot)
{
	return schedule_slot_locked(registry, slot);
}


static PostgammaBackendSlot *
find_running_slot_by_compat_pid(PostgammaBackendRegistry *registry,
								PostgammaCompatPid compat_pid)
{
	for (uint32_t index = 0; index < registry->capacity; index++)
	{
		PostgammaBackendSlot *slot = &registry->slots[index];

		if ((slot->state == POSTGAMMA_BACKEND_SLOT_STARTING ||
			 slot->state == POSTGAMMA_BACKEND_SLOT_RUNNING) &&
			slot->handle.compat_pid == compat_pid)
			return slot;
	}
	return NULL;
}
