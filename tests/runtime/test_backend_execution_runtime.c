#include "postgamma/backend_execution_runtime.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


typedef struct WorkerState
{
	PostgammaBackendRegistry *registry;
	int			ready;
	int			cleaned;
	uint64_t	observed_signals;
} WorkerState;

typedef struct QuantumState
{
	PostgammaBackendRegistry *registry;
	_Atomic uint32_t calls;
	_Atomic uint32_t cleaned;
	uint32_t	carriers[3];
} QuantumState;

typedef struct CompletionNotificationState
{
	_Atomic bool entered;
	_Atomic bool release;
	_Atomic bool exited;
} CompletionNotificationState;

typedef struct CompletionJoinState
{
	PostgammaBackendRegistry *registry;
	PostgammaBackendCompletion completion;
	_Atomic bool finished;
	int			status;
} CompletionJoinState;


static size_t
assert_stack_contract(const PostgammaBackendStartInfo *start_info)
{
	PostgammaThreadStackInfo current;
	PostgammaThreadRole expected_role = postgamma_backend_thread_role(
		start_info->handle.execution_class);
	size_t		default_stack_size;
	size_t		default_guard_size;
	size_t		stack_limit;

	assert(start_info->stack_status == 0);
	assert(start_info->stack_info.role == expected_role);
	assert(postgamma_thread_role_stack_policy(
		expected_role, &default_stack_size, &default_guard_size) == 0);
	assert(start_info->stack_info.configured_stack_size == default_stack_size);
	assert(start_info->stack_info.configured_guard_size == default_guard_size);
	assert(start_info->stack_info.native_stack_size >= default_stack_size);
	assert(start_info->stack_info.native_guard_size >= default_guard_size);
	assert(start_info->stack_info.usable_stack_size ==
		   start_info->stack_info.native_stack_size -
		   start_info->stack_info.native_guard_size);
	assert(start_info->stack_info.stack_high_address >
		   start_info->stack_info.stack_low_address);
	assert(start_info->stack_info.stack_high_address -
		   start_info->stack_info.stack_low_address ==
		   start_info->stack_info.native_stack_size);
	assert(postgamma_thread_current_stack_info(&current) == 0);
	assert(memcmp(&current, &start_info->stack_info, sizeof(current)) == 0);
	assert(postgamma_thread_current_stack_limit(&stack_limit) == 0);
	assert(stack_limit == current.usable_stack_size -
		   POSTGAMMA_THREAD_STACK_RUNTIME_SLOP);
	return stack_limit;
}


static void
record_stack_validation(
	PostgammaBackendRegistry *registry,
	const PostgammaBackendStartInfo *start_info,
	bool check_duplicate)
{
	size_t		stack_limit = assert_stack_contract(start_info);
	size_t		configured_depth = 2U * 1024U * 1024U;

	assert(postgamma_backend_registry_record_stack_validation(
		registry, start_info->handle.id, configured_depth,
		stack_limit, true) == 0);
	if (check_duplicate)
		assert(postgamma_backend_registry_record_stack_validation(
			registry, start_info->handle.id, configured_depth,
			stack_limit, true) == EALREADY);
}


static PostgammaBackendExitStatus
wait_for_signal(const PostgammaBackendStartInfo *start_info,
				const void *startup_data,
				size_t startup_data_length,
				void *main_argument)
{
	WorkerState *state = main_argument;
	PostgammaBackendExitStatus result = {
		POSTGAMMA_BACKEND_EXIT_NORMAL,
		0,
	};
	struct pollfd descriptor = {
		.fd = postgamma_wake_target_fd(start_info->wake_target),
		.events = POLLIN,
	};
	uint64_t	wake_count;
	uint64_t	pending;
	int			expected_code;

	record_stack_validation(state->registry, start_info, true);
	assert(startup_data_length == sizeof(expected_code));
	memcpy(&expected_code, startup_data, sizeof(expected_code));
	__atomic_store_n(&state->ready, 1, __ATOMIC_RELEASE);
	assert(poll(&descriptor, 1, 5000) == 1);
	assert(descriptor.revents & POLLIN);
	assert(postgamma_wake_target_drain(start_info->wake_target,
									 &wake_count) == 0);
	assert(wake_count > 0);
	assert(postgamma_backend_take_pending_signals(state->registry,
											 start_info->handle.id,
											 &pending) == 0);
	state->observed_signals = pending;
	result.code = expected_code;
	return result;
}


static PostgammaBackendExitStatus
return_immediately(const PostgammaBackendStartInfo *start_info,
				   const void *startup_data,
				   size_t startup_data_length,
				   void *main_argument)
{
	PostgammaBackendExitStatus result = {
		POSTGAMMA_BACKEND_EXIT_NORMAL,
		0,
	};

	record_stack_validation(main_argument, start_info, false);
	(void) startup_data;
	(void) startup_data_length;
	return result;
}


static PostgammaBackendExitStatus
run_three_quantums(
	const PostgammaBackendStartInfo *start_info,
	const void *startup_data, size_t startup_data_length,
	void *main_argument)
{
	QuantumState *state = main_argument;
	PostgammaBackendExitStatus result = {
		POSTGAMMA_BACKEND_EXIT_YIELD,
		0,
	};
	uint32_t	call;

	(void) startup_data;
	assert(startup_data_length == 0);
	assert(start_info->carrier_index > 0);
	call = atomic_fetch_add_explicit(
		&state->calls, UINT32_C(1), memory_order_acq_rel);
	assert(call < 3);
	assert(start_info->quantum_sequence == (uint64_t) call + 1);
	state->carriers[call] = start_info->carrier_index;
	if (call == 0)
		record_stack_validation(state->registry, start_info, true);
	if (call == 2)
		result.kind = POSTGAMMA_BACKEND_EXIT_NORMAL;
	return result;
}


static int
record_quantum_cleanup(
	const PostgammaBackendStartInfo *start_info, void *cleanup_argument)
{
	QuantumState *state = cleanup_argument;

	assert(start_info->quantum_sequence == 3);
	atomic_fetch_add_explicit(
		&state->cleaned, UINT32_C(1), memory_order_release);
	return 0;
}


static void
block_completion_notification(void *notification_argument)
{
	CompletionNotificationState *state = notification_argument;

	atomic_store_explicit(&state->entered, true, memory_order_release);
	while (!atomic_load_explicit(&state->release, memory_order_acquire))
		(void) poll(NULL, 0, 1);
	atomic_store_explicit(&state->exited, true, memory_order_release);
}


static void *
join_completion(void *argument)
{
	CompletionJoinState *state = argument;

	state->status = postgamma_backend_completion_join(
		state->registry, &state->completion);
	atomic_store_explicit(&state->finished, true, memory_order_release);
	return NULL;
}


static PostgammaBackendExitStatus
wait_for_pause_and_signal(const PostgammaBackendStartInfo *start_info,
						  const void *startup_data,
						  size_t startup_data_length,
						  void *main_argument)
{
	WorkerState *state = main_argument;
	PostgammaBackendExitStatus result = {
		POSTGAMMA_BACKEND_EXIT_NORMAL,
		0,
	};
	struct pollfd descriptor = {
		.fd = postgamma_wake_target_fd(start_info->wake_target),
		.events = POLLIN,
	};
	uint64_t	wake_count;
	uint64_t	pending;

	record_stack_validation(state->registry, start_info, false);
	(void) startup_data;
	assert(startup_data_length == 0);
	__atomic_store_n(&state->ready, 1, __ATOMIC_RELEASE);
	assert(poll(&descriptor, 1, 5000) == 1);
	assert(postgamma_wake_target_drain(start_info->wake_target,
									 &wake_count) == 0);
	assert(wake_count > 0);
	assert(postgamma_backend_wait_while_paused(
		state->registry, start_info->handle.id) == 0);
	__atomic_store_n(&state->ready, 2, __ATOMIC_RELEASE);
	assert(poll(&descriptor, 1, 5000) == 1);
	assert(postgamma_wake_target_drain(start_info->wake_target,
									 &wake_count) == 0);
	assert(postgamma_backend_take_pending_signals(state->registry,
											 start_info->handle.id,
											 &pending) == 0);
	state->observed_signals = pending;
	return result;
}


static int
record_cleanup(const PostgammaBackendStartInfo *start_info,
			   void *cleanup_argument)
{
	WorkerState *state = cleanup_argument;

	assert(start_info->handle.id != 0);
	__atomic_add_fetch(&state->cleaned, 1, __ATOMIC_RELEASE);
	return 0;
}


static void
wait_until_state(WorkerState *state, int expected)
{
	uint64_t	deadline = postgamma_monotonic_now_ns() + UINT64_C(5000000000);

	while (__atomic_load_n(&state->ready, __ATOMIC_ACQUIRE) != expected)
	{
		struct pollfd pause = {.fd = -1};

		assert(postgamma_monotonic_now_ns() < deadline);
		(void) poll(&pause, 0, 1);
	}
}


static void
wait_for_quantum_wave(QuantumState *states, size_t count, uint32_t expected)
{
	uint64_t	deadline = postgamma_monotonic_now_ns() + UINT64_C(10000000000);

	for (;;)
	{
		size_t		complete = 0;

		for (size_t index = 0; index < count; index++)
		{
			if (atomic_load_explicit(
					&states[index].calls, memory_order_acquire) >= expected)
				complete++;
		}
		if (complete == count)
			return;
		assert(postgamma_monotonic_now_ns() < deadline);
		(void) poll(NULL, 0, 1);
	}
}


static void
wait_for_notification(CompletionNotificationState *state)
{
	uint64_t	deadline = postgamma_monotonic_now_ns() + UINT64_C(5000000000);

	while (!atomic_load_explicit(&state->entered, memory_order_acquire))
	{
		assert(postgamma_monotonic_now_ns() < deadline);
		(void) poll(NULL, 0, 1);
	}
}


static void
wait_for_blocked_join(
	CompletionJoinState *join_state, PostgammaBackendId backend_id)
{
	uint64_t	deadline = postgamma_monotonic_now_ns() + UINT64_C(5000000000);

	for (;;)
	{
		PostgammaBackendSlotState slot_state;

		assert(!atomic_load_explicit(
			&join_state->finished, memory_order_acquire));
		assert(postgamma_backend_registry_lookup(
			join_state->registry, backend_id, NULL, &slot_state) == 0);
		if (slot_state == POSTGAMMA_BACKEND_SLOT_JOINING)
			return;
		assert(postgamma_monotonic_now_ns() < deadline);
		(void) poll(NULL, 0, 1);
	}
}


static PostgammaBackendCompletion
wait_for_completion(PostgammaBackendRegistry *registry)
{
	PostgammaBackendCompletion completion;
	struct pollfd descriptor = {
		.fd = postgamma_backend_registry_wake_fd(registry),
		.events = POLLIN,
	};

	for (;;)
	{
		int			status = postgamma_backend_completion_pop(registry,
												  &completion);

		if (status == 0)
			return completion;
		assert(status == EAGAIN);
		assert(poll(&descriptor, 1, 5000) == 1);
		assert(descriptor.revents & POLLIN);
	}
}


int
main(void)
{
	PostgammaBackendRegistry *registry;
	PostgammaBackendLaunchRequest request;
	PostgammaBackendHandle handles[2];
	PostgammaBackendCompletion completions[2];
	PostgammaBackendTelemetry telemetry;
	PostgammaBackendProviderConfig provider_config = {
		.kind = POSTGAMMA_BACKEND_PROVIDER_DEDICATED,
	};
	WorkerState states[2] = {0};
	int			startup_codes[2] = {17, 23};
	int			signal_number = 10;
	size_t		client_stack_size;
	size_t		dedicated_stack_size;
	size_t		parallel_stack_size;
	size_t		guard_size;
	PostgammaThreadStackInfo unmanaged_stack;

	assert(postgamma_thread_current_stack_info(&unmanaged_stack) == ENOENT);
	assert(postgamma_thread_role_stack_policy(
		POSTGAMMA_THREAD_ROLE_CLIENT, &client_stack_size, &guard_size) == 0);
	assert(postgamma_thread_role_stack_policy(
		POSTGAMMA_THREAD_ROLE_DEDICATED, &dedicated_stack_size, &guard_size) == 0);
	assert(postgamma_thread_role_stack_policy(
		POSTGAMMA_THREAD_ROLE_PARALLEL, &parallel_stack_size, &guard_size) == 0);
	assert(client_stack_size > dedicated_stack_size);
	assert(parallel_stack_size == client_stack_size);
	assert(postgamma_thread_role_stack_policy(
		POSTGAMMA_THREAD_ROLE_COUNT, &parallel_stack_size, &guard_size) == EINVAL);
	assert(postgamma_backend_registry_provider_kind(NULL) ==
		   POSTGAMMA_BACKEND_PROVIDER_COUNT);
	provider_config.kind = POSTGAMMA_BACKEND_PROVIDER_POOLED;
	assert(postgamma_backend_registry_create_with_provider(
		&registry, 2, &provider_config) == EINVAL);
	assert(registry == NULL);
	provider_config.kind = POSTGAMMA_BACKEND_PROVIDER_DEDICATED;
	provider_config.worker_count = 1;
	assert(postgamma_backend_registry_create_with_provider(
		&registry, 2, &provider_config) == EINVAL);
	assert(registry == NULL);
	provider_config.worker_count = 0;
	assert(postgamma_backend_registry_create_with_provider(
		&registry, 2, &provider_config) == 0);
	assert(postgamma_backend_registry_provider_kind(registry) ==
		   POSTGAMMA_BACKEND_PROVIDER_DEDICATED);
	for (int index = 0; index < 2; index++)
	{
		states[index].registry = registry;
		memset(&request, 0, sizeof(request));
		request.backend_type = 100 + index;
		request.execution_class = index == 0 ?
			POSTGAMMA_BACKEND_CLASS_CLIENT :
			POSTGAMMA_BACKEND_CLASS_PARALLEL;
		request.thread_name = index == 0 ? "pg-client" : "pg-parallel";
		request.startup_data = &startup_codes[index];
		request.startup_data_length = sizeof(startup_codes[index]);
		request.main_function = wait_for_signal;
		request.main_argument = &states[index];
		request.cleanup_function = record_cleanup;
		request.cleanup_argument = &states[index];
		assert(postgamma_backend_launch(registry, &request, &handles[index]) == 0);
		startup_codes[index] = -1;
	}

	assert(handles[0].id != handles[1].id);
	assert(handles[0].compat_pid != handles[1].compat_pid);
	assert(postgamma_backend_registry_active_count(registry) == 2);
	assert(postgamma_backend_registry_destroy(registry) == EBUSY);
	for (int index = 0; index < 2; index++)
	{
		wait_until_state(&states[index], 1);
		assert(postgamma_backend_signal(registry, handles[index].compat_pid,
									   signal_number) == 0);
	}

	completions[0] = wait_for_completion(registry);
	completions[1] = wait_for_completion(registry);
	for (int index = 0; index < 2; index++)
	{
		int			worker_index = completions[index].handle.id == handles[0].id ? 0 : 1;

		assert(completions[index].handle.id == handles[worker_index].id);
		assert(completions[index].exit_status.kind ==
			   POSTGAMMA_BACKEND_EXIT_NORMAL);
		assert(completions[index].exit_status.code == (worker_index == 0 ? 17 : 23));
		assert(postgamma_backend_completion_join(registry,
											&completions[index]) == 0);
		assert(states[worker_index].observed_signals ==
			   (UINT64_C(1) << (signal_number - 1)));
		assert(__atomic_load_n(&states[worker_index].cleaned,
							   __ATOMIC_ACQUIRE) == 1);
	}
	assert(postgamma_backend_registry_active_count(registry) == 0);
	assert(postgamma_backend_signal(registry, handles[0].compat_pid, 0) == ESRCH);
	assert(postgamma_backend_registry_record_stack_validation(
		registry, handles[0].id, 2U * 1024U * 1024U,
		3U * 1024U * 1024U, true) == ESTALE);
	assert(postgamma_backend_registry_record_stack_validation(
		registry, 0, 2U * 1024U * 1024U,
		3U * 1024U * 1024U, true) == EINVAL);
	assert(postgamma_backend_registry_record_stack_validation(
		registry, handles[0].id, 3U * 1024U * 1024U,
		2U * 1024U * 1024U, true) == EINVAL);

	assert(postgamma_backend_registry_telemetry(registry, &telemetry) == 0);
	assert(telemetry.provider_kind == POSTGAMMA_BACKEND_PROVIDER_DEDICATED);
	assert(telemetry.threads_started);
	assert(telemetry.client_threads_started == 1);
	assert(telemetry.client_threads_peak == 1);
	assert(telemetry.parallel_threads_started == 1);
	assert(telemetry.dedicated_threads_started == 0);
	assert(telemetry.backend_completions == 2);
	assert(telemetry.backend_threads_active == 0);
	assert(telemetry.backend_process_launches == 0);
	assert(telemetry.forbidden_process_launch_attempts == 0);
	assert(telemetry.unsupported_backend_requests == 0);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].observations == 1);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
		   observation_failures == 0);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
		   accounting_failures == 0);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].depth_validations == 1);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
		   depth_validation_failures == 0);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
		   active_reservation_bytes == 0);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
		   peak_reservation_bytes == client_stack_size);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
		   minimum_configured_stack_size == client_stack_size);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
		   maximum_configured_stack_size == client_stack_size);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
		   minimum_configured_guard_size == guard_size);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
		   minimum_depth_limit >= 2U * 1024U * 1024U);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_CLIENT].
		   maximum_configured_depth == 2U * 1024U * 1024U);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_PARALLEL].observations == 1);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_PARALLEL].
		   depth_validations == 1);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_PARALLEL].
		   active_reservation_bytes == 0);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_PARALLEL].
		   peak_reservation_bytes == parallel_stack_size);

	memset(&request, 0, sizeof(request));
	request.backend_type = 199;
	request.execution_class = POSTGAMMA_BACKEND_CLASS_CLIENT;
	request.thread_name = "pg-too-small";
	request.stack_size = POSTGAMMA_THREAD_STACK_MINIMUM_USABLE;
	request.main_function = return_immediately;
	assert(postgamma_backend_launch(registry, &request, &handles[0]) == EINVAL);
	assert(postgamma_backend_registry_active_count(registry) == 0);

	memset(&request, 0, sizeof(request));
	request.backend_type = 200;
	request.execution_class = POSTGAMMA_BACKEND_CLASS_DEDICATED;
	request.thread_name = "pg-dedicated";
	request.main_function = return_immediately;
	request.main_argument = registry;
	assert(postgamma_backend_launch(registry, &request, &handles[0]) == 0);
	completions[0] = wait_for_completion(registry);
	assert(completions[0].handle.id == handles[0].id);
	assert(postgamma_backend_completion_join(registry, &completions[0]) == 0);
	assert(postgamma_backend_registry_telemetry(registry, &telemetry) == 0);
	assert(telemetry.dedicated_threads_started == 1);
	assert(telemetry.backend_completions == 3);

	memset(&states[0], 0, sizeof(states[0]));
	states[0].registry = registry;
	memset(&request, 0, sizeof(request));
	request.backend_type = 201;
	request.execution_class = POSTGAMMA_BACKEND_CLASS_DEDICATED;
	request.thread_name = "pg-pause";
	request.main_function = wait_for_pause_and_signal;
	request.main_argument = &states[0];
	assert(postgamma_backend_launch(registry, &request, &handles[0]) == 0);
	wait_until_state(&states[0], 1);
	assert(postgamma_backend_pause(registry, handles[0].compat_pid) == 0);
	assert(__atomic_load_n(&states[0].ready, __ATOMIC_ACQUIRE) == 1);
	assert(postgamma_backend_resume(registry, handles[0].compat_pid) == 0);
	wait_until_state(&states[0], 2);
	assert(postgamma_backend_signal(registry, handles[0].compat_pid,
								   signal_number) == 0);
	completions[0] = wait_for_completion(registry);
	assert(completions[0].handle.id == handles[0].id);
	assert(postgamma_backend_completion_join(registry, &completions[0]) == 0);
	assert(states[0].observed_signals ==
		   (UINT64_C(1) << (signal_number - 1)));

	postgamma_backend_registry_record_process_launch(registry);
	postgamma_backend_registry_record_forbidden_process_launch(registry);
	postgamma_backend_registry_record_unsupported_request(registry);
	assert(postgamma_backend_registry_telemetry(registry, &telemetry) == 0);
	assert(telemetry.backend_process_launches == 1);
	assert(telemetry.forbidden_process_launch_attempts == 1);
	assert(telemetry.unsupported_backend_requests == 1);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_DEDICATED].observations == 2);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_DEDICATED].
		   observation_failures == 0);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_DEDICATED].
		   accounting_failures == 0);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_DEDICATED].
		   depth_validations == 2);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_DEDICATED].
		   depth_validation_failures == 0);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_DEDICATED].
		   active_reservation_bytes == 0);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_DEDICATED].
		   peak_reservation_bytes == dedicated_stack_size);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_DEDICATED].
		   minimum_configured_stack_size == dedicated_stack_size);
	assert(telemetry.stack[POSTGAMMA_BACKEND_CLASS_DEDICATED].
		   maximum_configured_stack_size == dedicated_stack_size);
	assert(postgamma_backend_registry_destroy(registry) == 0);
	{
		CompletionNotificationState notification_state = {0};
		CompletionJoinState join_state = {0};
		PostgammaBackendHandle handle;
		PostgammaThread *join_thread;
		PostgammaThreadAttributes attributes = {
			.name = "pg-join",
			.role = POSTGAMMA_THREAD_ROLE_GENERAL,
		};

		provider_config.kind = POSTGAMMA_BACKEND_PROVIDER_POOLED;
		provider_config.worker_count = 2;
		provider_config.queue_capacity = 2;
		provider_config.execution_token_count = 2;
		assert(postgamma_backend_registry_create_with_provider(
			&registry, 2, &provider_config) == 0);
		memset(&request, 0, sizeof(request));
		request.backend_type = 299;
		request.execution_class = POSTGAMMA_BACKEND_CLASS_CLIENT;
		request.thread_name = "completion-delivery";
		request.main_function = return_immediately;
		request.main_argument = registry;
		request.completion_notification = block_completion_notification;
		request.completion_notification_argument = &notification_state;
		assert(postgamma_backend_launch(registry, &request, &handle) == 0);
		join_state.registry = registry;
		join_state.completion = wait_for_completion(registry);
		assert(join_state.completion.handle.id == handle.id);
		wait_for_notification(&notification_state);
		assert(postgamma_thread_create(
			&join_thread, &attributes, join_completion, &join_state) == 0);
		wait_for_blocked_join(&join_state, handle.id);
		assert(!atomic_load_explicit(
			&notification_state.exited, memory_order_acquire));
		atomic_store_explicit(
			&notification_state.release, true, memory_order_release);
		assert(postgamma_thread_join(join_thread, NULL) == 0);
		assert(postgamma_thread_destroy(join_thread) == 0);
		assert(join_state.status == 0);
		assert(atomic_load_explicit(
			&notification_state.exited, memory_order_acquire));
		assert(postgamma_backend_registry_active_count(registry) == 0);
		assert(postgamma_backend_registry_destroy(registry) == 0);
	}
	{
		enum { SESSION_COUNT = 1000 };
		PostgammaBackendHandle *pooled_handles = calloc(
			SESSION_COUNT, sizeof(*pooled_handles));
		QuantumState *quantum_states = calloc(
			SESSION_COUNT, sizeof(*quantum_states));

		assert(pooled_handles != NULL);
		assert(quantum_states != NULL);
		provider_config.kind = POSTGAMMA_BACKEND_PROVIDER_POOLED;
		provider_config.worker_count = 2;
		provider_config.queue_capacity = SESSION_COUNT;
		provider_config.execution_token_count = 2;
		assert(postgamma_backend_registry_create_with_provider(
			&registry, SESSION_COUNT, &provider_config) == 0);
		for (size_t index = 0; index < SESSION_COUNT; index++)
		{
			quantum_states[index].registry = registry;
			memset(&request, 0, sizeof(request));
			request.backend_type = 300;
			request.execution_class = POSTGAMMA_BACKEND_CLASS_CLIENT;
			request.thread_name = "logical-client";
			request.main_function = run_three_quantums;
			request.main_argument = &quantum_states[index];
			request.cleanup_function = record_quantum_cleanup;
			request.cleanup_argument = &quantum_states[index];
			assert(postgamma_backend_launch(
				registry, &request, &pooled_handles[index]) == 0);
		}
		wait_for_quantum_wave(quantum_states, SESSION_COUNT, 1);
		for (size_t index = 0; index < SESSION_COUNT; index++)
			assert(postgamma_backend_schedule(
				registry, pooled_handles[index].id) == 0);
		wait_for_quantum_wave(quantum_states, SESSION_COUNT, 2);
		for (size_t index = 0; index < SESSION_COUNT; index++)
			assert(postgamma_backend_schedule(
				registry, pooled_handles[index].id) == 0);
		for (size_t index = 0; index < SESSION_COUNT; index++)
		{
			PostgammaBackendCompletion completion =
				wait_for_completion(registry);

			assert(completion.exit_status.kind ==
				   POSTGAMMA_BACKEND_EXIT_NORMAL);
			assert(postgamma_backend_completion_join(
				registry, &completion) == 0);
		}
		for (size_t index = 0; index < SESSION_COUNT; index++)
		{
			assert(atomic_load_explicit(
				&quantum_states[index].cleaned, memory_order_acquire) == 1);
			assert(quantum_states[index].carriers[0] != 0);
			assert(quantum_states[index].carriers[1] != 0);
			assert(quantum_states[index].carriers[2] != 0);
		}
		assert(postgamma_backend_registry_telemetry(
			registry, &telemetry) == 0);
		assert(telemetry.provider_kind == POSTGAMMA_BACKEND_PROVIDER_POOLED);
		assert(telemetry.pooled_worker_threads == 2);
		assert(telemetry.client_threads_started == 2);
		assert(telemetry.client_threads_peak <= 2);
		assert(telemetry.client_quantums == SESSION_COUNT * 3);
		assert(telemetry.quantum_yields == SESSION_COUNT * 2);
		assert(telemetry.carrier_migrations >= SESSION_COUNT);
		assert(telemetry.carrier_migrations <= SESSION_COUNT * 2);
		assert(telemetry.work_steals <= telemetry.client_quantums);
		assert(telemetry.execution_token_budget == 2);
		assert(telemetry.execution_tokens_active == 0);
		assert(telemetry.execution_tokens_peak == 2);
		assert(telemetry.runnable_sessions == 0);
		assert(telemetry.running_quantums == 0);
		assert(telemetry.backend_threads_active == 0);
		assert(postgamma_backend_registry_active_count(registry) == 0);
		assert(postgamma_backend_registry_destroy(registry) == 0);
		free(quantum_states);
		free(pooled_handles);
	}
	{
		QuantumState state = {0};
		PostgammaBackendHandle handle;
		PostgammaBackendCompletion completion;

		provider_config.kind = POSTGAMMA_BACKEND_PROVIDER_POOLED;
		provider_config.worker_count = 2;
		provider_config.queue_capacity = 2;
		provider_config.execution_token_count = 2;
		assert(postgamma_backend_registry_create_with_provider(
			&registry, 2, &provider_config) == 0);
		state.registry = registry;
		memset(&request, 0, sizeof(request));
		request.backend_type = 301;
		request.execution_class = POSTGAMMA_BACKEND_CLASS_CLIENT;
		request.thread_name = "aba-client";
		request.main_function = run_three_quantums;
		request.main_argument = &state;
		request.cleanup_function = record_quantum_cleanup;
		request.cleanup_argument = &state;
		assert(postgamma_backend_launch(registry, &request, &handle) == 0);
		wait_for_quantum_wave(&state, 1, 1);
		assert(postgamma_backend_schedule(registry, handle.id) == 0);
		wait_for_quantum_wave(&state, 1, 2);
		assert(postgamma_backend_schedule(registry, handle.id) == 0);
		completion = wait_for_completion(registry);
		assert(completion.handle.id == handle.id);
		assert(postgamma_backend_completion_join(registry, &completion) == 0);
		assert(state.carriers[0] == state.carriers[2]);
		assert(state.carriers[0] != state.carriers[1]);
		assert(atomic_load_explicit(
			&state.cleaned, memory_order_acquire) == 1);
		assert(postgamma_backend_registry_destroy(registry) == 0);
	}
	assert(postgamma_backend_registry_create(&registry, 1) == 0);
	assert(postgamma_backend_registry_provider_kind(registry) ==
		   POSTGAMMA_BACKEND_PROVIDER_DEDICATED);
	assert(postgamma_backend_registry_destroy(registry) == 0);
	return 0;
}
