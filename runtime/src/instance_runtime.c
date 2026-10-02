/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * instance_runtime.c
 *    Instance-owned backend registry, process policy, and telemetry.
 *
 *-------------------------------------------------------------------------
 */

#include "postgamma/instance_runtime.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>


#define POSTGAMMA_INSTANCE_RUNTIME_MAGIC UINT64_C(0x50474952554E544D)
#define POSTGAMMA_CHECKPOINT_TRACKER_MAGIC UINT64_C(0x5047434B5054524B)


typedef enum PostgammaCheckpointTrackerState
{
	POSTGAMMA_CHECKPOINT_TRACKER_NEW = 0,
	POSTGAMMA_CHECKPOINT_TRACKER_ARMED,
	POSTGAMMA_CHECKPOINT_TRACKER_TERMINAL
} PostgammaCheckpointTrackerState;


struct PostgammaCheckpointTracker
{
	uint64_t	magic;
	PostgammaInstanceRuntime *runtime;
	uint64_t	generation;
	PostgammaWakeTarget *wake_target;
	PostgammaCheckpointCompletionFunction completion;
	void	   *completion_argument;
	struct PostgammaCheckpointTracker *next;
	_Atomic int state;
	_Atomic int32_t baseline_started;
	_Atomic int32_t baseline_failed;
};


struct PostgammaInstanceRuntime
{
	uint64_t	magic;
	uint64_t	generation;
	PostgammaRuntimeProfile profile;
	PostgammaBackendProviderConfig backend_provider;
	PostgammaMutex *mutex;
	PostgammaBackendRegistry *backend_registry;
	uint32_t	backend_capacity;
	PostgammaInstanceWakeNotify wake_notification;
	void	   *wake_notification_argument;
	PostgammaKernelEmitLogFunction emit_log;
	void	   *log_argument;
	_Atomic bool bundled_extensions_active;
	uint64_t	completion_notifications;
	uint64_t	supervisor_signal_notifications;
	uint64_t	pending_supervisor_signals;
	uint64_t	external_processes_started;
	uint64_t	external_processes_finished;
	uint64_t	external_process_launch_rejections;
	uint64_t	external_process_reservations;
	uint64_t	external_processes_active;
	uint64_t	wake_callbacks_active;
	PostgammaCheckpointTracker *checkpoint_trackers;
};


static _Atomic uint64_t PostgammaNextInstanceGeneration = UINT64_C(1);


static bool runtime_is_valid(const PostgammaInstanceRuntime *runtime);
static int lock_runtime(PostgammaInstanceRuntime *runtime);
static int unlock_runtime(PostgammaInstanceRuntime *runtime, int status);
static int assign_generation(uint64_t requested, uint64_t *generation);
static int notify_wake_locked(PostgammaInstanceRuntime *runtime);
static bool checkpoint_tracker_is_valid(
	const PostgammaCheckpointTracker *tracker);


int
postgamma_instance_runtime_create(
	PostgammaInstanceRuntime **runtime,
	const PostgammaInstanceRuntimeOptions *options)
{
	PostgammaInstanceRuntime *created;
	int			status;

	if (runtime == NULL || options == NULL ||
		options->struct_size != sizeof(*options) ||
		options->profile <= POSTGAMMA_RUNTIME_PROFILE_INVALID ||
		options->profile > POSTGAMMA_RUNTIME_PROFILE_EMBEDDED ||
		options->backend_provider.kind <
			POSTGAMMA_BACKEND_PROVIDER_DEDICATED ||
		options->backend_provider.kind >= POSTGAMMA_BACKEND_PROVIDER_COUNT ||
		(options->backend_provider.kind == POSTGAMMA_BACKEND_PROVIDER_POOLED &&
		 (options->backend_provider.worker_count < 2 ||
		  options->backend_provider.execution_token_count < 2)) ||
		(options->wake_notification == NULL &&
		 options->wake_notification_argument != NULL) ||
		(options->emit_log == NULL && options->log_argument != NULL))
		return EINVAL;
	*runtime = NULL;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	status = assign_generation(options->generation, &created->generation);
	if (status != 0)
	{
		free(created);
		return status;
	}
	status = postgamma_mutex_create(&created->mutex);
	if (status != 0)
	{
		free(created);
		return status;
	}
	created->magic = POSTGAMMA_INSTANCE_RUNTIME_MAGIC;
	created->profile = options->profile;
	created->backend_provider = options->backend_provider;
	created->wake_notification = options->wake_notification;
	created->wake_notification_argument = options->wake_notification_argument;
	created->emit_log = options->emit_log;
	created->log_argument = options->log_argument;
	atomic_init(&created->bundled_extensions_active, false);
	*runtime = created;
	return 0;
}


int
postgamma_instance_runtime_destroy(PostgammaInstanceRuntime *runtime)
{
	int			status;

	if (!runtime_is_valid(runtime))
		return EINVAL;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	if (runtime->external_processes_active != 0 ||
		runtime->external_process_reservations != 0 ||
		runtime->wake_callbacks_active != 0 ||
		runtime->checkpoint_trackers != NULL)
		return unlock_runtime(runtime, EBUSY);
	if (runtime->backend_registry != NULL)
	{
		status = postgamma_backend_registry_destroy(runtime->backend_registry);
		if (status != 0)
			return unlock_runtime(runtime, status);
		runtime->backend_registry = NULL;
		runtime->backend_capacity = 0;
	}
	status = unlock_runtime(runtime, 0);
	if (status != 0)
		return status;
	status = postgamma_mutex_destroy(runtime->mutex);
	if (status != 0)
		return status;
	runtime->magic = 0;
	memset(runtime, 0, sizeof(*runtime));
	free(runtime);
	return 0;
}


uint64_t
postgamma_instance_runtime_generation(const PostgammaInstanceRuntime *runtime)
{
	return runtime_is_valid(runtime) ? runtime->generation : 0;
}


PostgammaRuntimeProfile
postgamma_instance_runtime_profile(const PostgammaInstanceRuntime *runtime)
{
	return runtime_is_valid(runtime) ? runtime->profile :
		POSTGAMMA_RUNTIME_PROFILE_INVALID;
}


int
postgamma_instance_runtime_activate_bundled_extensions(
	PostgammaInstanceRuntime *runtime)
{
	if (!runtime_is_valid(runtime) ||
		runtime->profile != POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
		return EINVAL;
	atomic_store_explicit(
		&runtime->bundled_extensions_active, true, memory_order_release);
	return 0;
}


bool
postgamma_instance_runtime_bundled_extensions_active(
	const PostgammaInstanceRuntime *runtime)
{
	return runtime_is_valid(runtime) &&
		atomic_load_explicit(
			&runtime->bundled_extensions_active, memory_order_acquire);
}


int
postgamma_instance_runtime_ensure_backend_registry(
	PostgammaInstanceRuntime *runtime,
	uint32_t capacity,
	PostgammaBackendRegistry **registry)
{
	PostgammaBackendProviderConfig provider_config;
	int			status;

	if (!runtime_is_valid(runtime) || registry == NULL || capacity == 0)
		return EINVAL;
	*registry = NULL;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	if (runtime->backend_registry != NULL)
	{
		if (runtime->backend_capacity != capacity)
			return unlock_runtime(runtime, EALREADY);
		*registry = runtime->backend_registry;
		status = unlock_runtime(runtime, 0);
		if (status != 0)
			*registry = NULL;
		return status;
	}
	provider_config = runtime->backend_provider;
	if (provider_config.kind == POSTGAMMA_BACKEND_PROVIDER_POOLED)
	{
		if (provider_config.worker_count > capacity)
			provider_config.worker_count = capacity;
		if (provider_config.queue_capacity < capacity)
			provider_config.queue_capacity = capacity;
	}
	status = postgamma_backend_registry_create_with_provider(
		&runtime->backend_registry, capacity, &provider_config);
	if (status == 0)
	{
		runtime->backend_capacity = capacity;
		*registry = runtime->backend_registry;
	}
	status = unlock_runtime(runtime, status);
	if (status != 0)
		*registry = NULL;
	return status;
}


PostgammaBackendRegistry *
postgamma_instance_runtime_backend_registry(PostgammaInstanceRuntime *runtime)
{
	PostgammaBackendRegistry *registry;

	if (!runtime_is_valid(runtime) || lock_runtime(runtime) != 0)
		return NULL;
	registry = runtime->backend_registry;
	if (unlock_runtime(runtime, 0) != 0)
		return NULL;
	return registry;
}


int
postgamma_instance_runtime_reserve_external_process(
	PostgammaInstanceRuntime *runtime)
{
	int			status;

	if (!runtime_is_valid(runtime))
		return EINVAL;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	if (runtime->profile == POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
	{
		if (runtime->external_process_launch_rejections == UINT64_MAX)
			return unlock_runtime(runtime, EOVERFLOW);
		runtime->external_process_launch_rejections++;
		postgamma_backend_registry_record_forbidden_process_launch(
			runtime->backend_registry);
		return unlock_runtime(runtime, ENOTSUP);
	}
	if (runtime->external_process_reservations >=
		UINT64_MAX - runtime->external_processes_active ||
		runtime->external_processes_started == UINT64_MAX)
		return unlock_runtime(runtime, EOVERFLOW);
	runtime->external_process_reservations++;
	return unlock_runtime(runtime, 0);
}


int
postgamma_instance_runtime_commit_external_process(
	PostgammaInstanceRuntime *runtime)
{
	int			status;

	if (!runtime_is_valid(runtime))
		return EINVAL;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	if (runtime->external_process_reservations == 0)
		return unlock_runtime(runtime, EPROTO);
	if (runtime->external_processes_started == UINT64_MAX)
		return unlock_runtime(runtime, EOVERFLOW);
	runtime->external_process_reservations--;
	runtime->external_processes_active++;
	runtime->external_processes_started++;
	return unlock_runtime(runtime, 0);
}


int
postgamma_instance_runtime_cancel_external_process(
	PostgammaInstanceRuntime *runtime)
{
	int			status;

	if (!runtime_is_valid(runtime))
		return EINVAL;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	if (runtime->external_process_reservations == 0)
		return unlock_runtime(runtime, EPROTO);
	runtime->external_process_reservations--;
	return unlock_runtime(runtime, 0);
}


int
postgamma_instance_runtime_record_external_process_finish(
	PostgammaInstanceRuntime *runtime)
{
	int			status;

	if (!runtime_is_valid(runtime))
		return EINVAL;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	if (runtime->external_processes_active == 0 ||
		runtime->external_processes_finished == UINT64_MAX)
		return unlock_runtime(runtime, EPROTO);
	runtime->external_processes_active--;
	runtime->external_processes_finished++;
	return unlock_runtime(runtime, 0);
}


int
postgamma_instance_runtime_external_process_count(
	PostgammaInstanceRuntime *runtime, uint64_t *count)
{
	int			status;

	if (!runtime_is_valid(runtime) || count == NULL)
		return EINVAL;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	*count = runtime->external_process_reservations +
		runtime->external_processes_active;
	return unlock_runtime(runtime, 0);
}


int
postgamma_instance_runtime_notify_completion(
	PostgammaInstanceRuntime *runtime)
{
	int			status;

	if (!runtime_is_valid(runtime))
		return EINVAL;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	if (runtime->completion_notifications == UINT64_MAX ||
		runtime->wake_callbacks_active == UINT64_MAX)
		return unlock_runtime(runtime, EOVERFLOW);
	runtime->completion_notifications++;
	return notify_wake_locked(runtime);
}


int
postgamma_instance_runtime_notify_supervisor_signal(
	PostgammaInstanceRuntime *runtime,
	uint64_t generation,
	int signal_number)
{
	uint64_t	mask;
	int			status;

	if (!runtime_is_valid(runtime) || signal_number <= 0 ||
		signal_number > POSTGAMMA_INSTANCE_SIGNAL_MAX)
		return EINVAL;
	mask = UINT64_C(1) << (signal_number - 1);
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	if (runtime->generation != generation)
		return unlock_runtime(runtime, ESTALE);
	if (runtime->supervisor_signal_notifications == UINT64_MAX ||
		runtime->wake_callbacks_active == UINT64_MAX)
		return unlock_runtime(runtime, EOVERFLOW);
	runtime->pending_supervisor_signals |= mask;
	runtime->supervisor_signal_notifications++;
	return notify_wake_locked(runtime);
}


int
postgamma_instance_runtime_take_supervisor_signals(
	PostgammaInstanceRuntime *runtime,
	uint64_t generation,
	uint64_t *pending_signals)
{
	int			status;

	if (!runtime_is_valid(runtime) || pending_signals == NULL)
		return EINVAL;
	*pending_signals = 0;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	if (runtime->generation != generation)
		return unlock_runtime(runtime, ESTALE);
	if (runtime->pending_supervisor_signals == 0)
		return unlock_runtime(runtime, EAGAIN);
	*pending_signals = runtime->pending_supervisor_signals;
	runtime->pending_supervisor_signals = 0;
	return unlock_runtime(runtime, 0);
}


static int
notify_wake_locked(PostgammaInstanceRuntime *runtime)
{
	PostgammaInstanceWakeNotify notification;
	void	   *argument;
	uint64_t	generation;
	int			callback_status;
	int			status;

	notification = runtime->wake_notification;
	argument = runtime->wake_notification_argument;
	generation = runtime->generation;
	if (notification != NULL)
		runtime->wake_callbacks_active++;
	status = unlock_runtime(runtime, 0);
	if (status != 0)
		return status;

	if (notification != NULL)
	{
		callback_status = notification(argument, generation);
		status = lock_runtime(runtime);
		if (status != 0)
			return status;
		if (runtime->wake_callbacks_active == 0)
			return unlock_runtime(runtime, EPROTO);
		runtime->wake_callbacks_active--;
		return unlock_runtime(runtime, callback_status);
	}
	return 0;
}


int
postgamma_instance_runtime_telemetry(
	PostgammaInstanceRuntime *runtime,
	PostgammaInstanceRuntimeTelemetry *telemetry)
{
	int			status;

	if (!runtime_is_valid(runtime) || telemetry == NULL)
		return EINVAL;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	memset(telemetry, 0, sizeof(*telemetry));
	telemetry->generation = runtime->generation;
	telemetry->profile = runtime->profile;
	telemetry->bundled_extensions_active = atomic_load_explicit(
		&runtime->bundled_extensions_active, memory_order_acquire);
	telemetry->backend_registry_created = runtime->backend_registry != NULL;
	telemetry->backend_capacity = runtime->backend_capacity;
	telemetry->completion_notifications =
		runtime->completion_notifications;
	telemetry->supervisor_signal_notifications =
		runtime->supervisor_signal_notifications;
	telemetry->pending_supervisor_signals =
		runtime->pending_supervisor_signals;
	telemetry->external_processes_started = runtime->external_processes_started;
	telemetry->external_processes_finished = runtime->external_processes_finished;
	telemetry->external_process_launch_rejections =
		runtime->external_process_launch_rejections;
	telemetry->external_process_reservations =
		runtime->external_process_reservations;
	telemetry->external_processes_active = runtime->external_processes_active;
	if (runtime->backend_registry != NULL)
		status = postgamma_backend_registry_telemetry(
			runtime->backend_registry, &telemetry->backend);
	return unlock_runtime(runtime, status);
}


int
postgamma_instance_runtime_emit_log(
	PostgammaInstanceRuntime *runtime,
	const PostgammaKernelLogRecord *record)
{
	if (!runtime_is_valid(runtime) || record == NULL ||
		record->struct_size != sizeof(*record))
		return EINVAL;
	if (runtime->emit_log == NULL)
		return 0;
	return runtime->emit_log(
		runtime->log_argument, runtime->generation, record);
}


int
postgamma_instance_checkpoint_tracker_create(
	PostgammaInstanceRuntime *runtime,
	uint64_t generation,
	PostgammaCheckpointCompletionFunction completion,
	void *completion_argument,
	PostgammaCheckpointTracker **tracker)
{
	PostgammaCheckpointTracker *created;
	int			status;

	if (!runtime_is_valid(runtime) || generation == 0 ||
		generation != runtime->generation || completion == NULL ||
		tracker == NULL)
		return EINVAL;
	*tracker = NULL;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	status = postgamma_wake_target_create(&created->wake_target);
	if (status != 0)
	{
		free(created);
		return status;
	}
	created->magic = POSTGAMMA_CHECKPOINT_TRACKER_MAGIC;
	created->runtime = runtime;
	created->generation = generation;
	created->completion = completion;
	created->completion_argument = completion_argument;
	atomic_init(&created->state, POSTGAMMA_CHECKPOINT_TRACKER_NEW);
	atomic_init(&created->baseline_started, INT32_C(0));
	atomic_init(&created->baseline_failed, INT32_C(0));
	status = lock_runtime(runtime);
	if (status != 0)
		goto fail;
	if (!runtime_is_valid(runtime) || runtime->generation != generation)
		status = ESTALE;
	else
	{
		created->next = runtime->checkpoint_trackers;
		runtime->checkpoint_trackers = created;
	}
	status = unlock_runtime(runtime, status);
	if (status != 0)
		goto fail;
	*tracker = created;
	return 0;

fail:
	created->magic = 0;
	(void) postgamma_wake_target_destroy(created->wake_target);
	free(created);
	return status;
}


int
postgamma_instance_checkpoint_tracker_arm(
	PostgammaCheckpointTracker *tracker,
	uint64_t generation,
	int32_t baseline_started,
	int32_t baseline_failed)
{
	int			expected = POSTGAMMA_CHECKPOINT_TRACKER_NEW;

	if (!checkpoint_tracker_is_valid(tracker) ||
		tracker->generation != generation)
		return EINVAL;
	atomic_store_explicit(
		&tracker->baseline_started, baseline_started, memory_order_relaxed);
	atomic_store_explicit(
		&tracker->baseline_failed, baseline_failed, memory_order_relaxed);
	if (!atomic_compare_exchange_strong_explicit(
			&tracker->state, &expected,
			POSTGAMMA_CHECKPOINT_TRACKER_ARMED,
			memory_order_release, memory_order_acquire))
		return expected == POSTGAMMA_CHECKPOINT_TRACKER_TERMINAL ?
			ECANCELED : EALREADY;
	return 0;
}


int
postgamma_instance_checkpoint_tracker_finish(
	PostgammaCheckpointTracker *tracker,
	uint64_t generation,
	int operation_status)
{
	PostgammaCheckpointCompletionFunction completion;
	void	   *completion_argument;
	int			state;

	if (!checkpoint_tracker_is_valid(tracker) ||
		tracker->generation != generation)
		return EINVAL;
	state = atomic_load_explicit(&tracker->state, memory_order_acquire);
	for (;;)
	{
		if (state == POSTGAMMA_CHECKPOINT_TRACKER_TERMINAL)
			return EALREADY;
		if (atomic_compare_exchange_weak_explicit(
				&tracker->state, &state,
				POSTGAMMA_CHECKPOINT_TRACKER_TERMINAL,
				memory_order_acq_rel, memory_order_acquire))
			break;
	}
	completion = tracker->completion;
	completion_argument = tracker->completion_argument;
	/* The completion callback may destroy tracker, so it is the final action. */
	completion(completion_argument, generation, operation_status);
	return 0;
}


int
postgamma_instance_checkpoint_observe(
	PostgammaInstanceRuntime *runtime,
	uint64_t generation,
	int32_t checkpoint_done,
	int32_t checkpoint_failed)
{
	int			status;

	if (!runtime_is_valid(runtime) || runtime->generation != generation)
		return EINVAL;
	for (;;)
	{
		PostgammaCheckpointTracker *candidate = NULL;
		int			operation_status = 0;

		status = lock_runtime(runtime);
		if (status != 0)
			return status;
		for (PostgammaCheckpointTracker *current =
			 runtime->checkpoint_trackers;
			 current != NULL; current = current->next)
		{
			int tracker_state = atomic_load_explicit(
				&current->state, memory_order_acquire);
			int32_t baseline_started;
			int32_t baseline_failed;

			if (tracker_state != POSTGAMMA_CHECKPOINT_TRACKER_ARMED)
				continue;
			baseline_started = atomic_load_explicit(
				&current->baseline_started, memory_order_relaxed);
			if ((int32_t) ((uint32_t) checkpoint_done -
					(uint32_t) baseline_started) <= 0)
				continue;
			baseline_failed = atomic_load_explicit(
				&current->baseline_failed, memory_order_relaxed);
			candidate = current;
			operation_status = checkpoint_failed != baseline_failed ? EIO : 0;
			break;
		}
		status = unlock_runtime(runtime, 0);
		if (status != 0)
			return status;
		if (candidate == NULL)
			return 0;
		status = postgamma_instance_checkpoint_tracker_finish(
			candidate, generation, operation_status);
		if (status != 0 && status != EALREADY)
			return status;
	}
}


int
postgamma_instance_checkpoint_tracker_waitable_fd(
	const PostgammaCheckpointTracker *tracker)
{
	return checkpoint_tracker_is_valid(tracker) ?
		postgamma_wake_target_fd(tracker->wake_target) : -1;
}


int
postgamma_instance_checkpoint_tracker_wake(
	PostgammaCheckpointTracker *tracker)
{
	return checkpoint_tracker_is_valid(tracker) ?
		postgamma_wake_target_wake(tracker->wake_target) : EINVAL;
}


int
postgamma_instance_checkpoint_tracker_wake_drain(
	PostgammaCheckpointTracker *tracker,
	uint64_t *wake_count)
{
	return checkpoint_tracker_is_valid(tracker) ?
		postgamma_wake_target_drain(tracker->wake_target, wake_count) : EINVAL;
}


int
postgamma_instance_checkpoint_tracker_destroy(
	PostgammaCheckpointTracker *tracker)
{
	PostgammaInstanceRuntime *runtime;
	PostgammaCheckpointTracker **link;
	int			state;
	int			status;

	if (!checkpoint_tracker_is_valid(tracker))
		return EINVAL;
	state = atomic_load_explicit(&tracker->state, memory_order_acquire);
	if (state == POSTGAMMA_CHECKPOINT_TRACKER_ARMED)
		return EBUSY;
	runtime = tracker->runtime;
	status = lock_runtime(runtime);
	if (status != 0)
		return status;
	link = &runtime->checkpoint_trackers;
	while (*link != NULL && *link != tracker)
		link = &(*link)->next;
	if (*link != tracker)
		return unlock_runtime(runtime, ENOENT);
	*link = tracker->next;
	tracker->next = NULL;
	status = unlock_runtime(runtime, 0);
	if (status != 0)
		return status;
	status = postgamma_wake_target_destroy(tracker->wake_target);
	if (status != 0)
		return status;
	tracker->magic = 0;
	tracker->runtime = NULL;
	tracker->wake_target = NULL;
	free(tracker);
	return 0;
}


static bool
runtime_is_valid(const PostgammaInstanceRuntime *runtime)
{
	return runtime != NULL &&
		runtime->magic == POSTGAMMA_INSTANCE_RUNTIME_MAGIC;
}


static bool
checkpoint_tracker_is_valid(const PostgammaCheckpointTracker *tracker)
{
	return tracker != NULL &&
		tracker->magic == POSTGAMMA_CHECKPOINT_TRACKER_MAGIC &&
		runtime_is_valid(tracker->runtime) &&
		tracker->generation == tracker->runtime->generation &&
		tracker->wake_target != NULL && tracker->completion != NULL;
}


static int
lock_runtime(PostgammaInstanceRuntime *runtime)
{
	return postgamma_mutex_lock(runtime->mutex);
}


static int
unlock_runtime(PostgammaInstanceRuntime *runtime, int status)
{
	int			unlock_status = postgamma_mutex_unlock(runtime->mutex);

	return unlock_status != 0 ? unlock_status : status;
}


static int
assign_generation(uint64_t requested, uint64_t *generation)
{
	uint64_t	allocated;
	uint64_t	next;

	if (generation == NULL)
		return EINVAL;
	allocated = atomic_load_explicit(
		&PostgammaNextInstanceGeneration, memory_order_relaxed);
	if (requested != 0)
	{
		if (requested == UINT64_MAX)
			return EOVERFLOW;
		while (allocated <= requested &&
			   !atomic_compare_exchange_weak_explicit(
				   &PostgammaNextInstanceGeneration, &allocated,
				   requested + UINT64_C(1), memory_order_relaxed,
				   memory_order_relaxed))
		{
			/* Retry after a concurrent automatic or explicit allocation. */
		}
		*generation = requested;
		return 0;
	}
	for (;;)
	{
		if (allocated == 0 || allocated == UINT64_MAX)
			return EOVERFLOW;
		next = allocated + UINT64_C(1);
		if (atomic_compare_exchange_weak_explicit(
				&PostgammaNextInstanceGeneration, &allocated, next,
				memory_order_relaxed, memory_order_relaxed))
			break;
	}
	*generation = allocated;
	return 0;
}
