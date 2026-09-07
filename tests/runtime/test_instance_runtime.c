#include "postgamma/instance_runtime.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>


#define ENSURE_THREAD_COUNT 16


typedef struct EnsureWorker
{
	PostgammaInstanceRuntime *runtime;
	PostgammaBackendRegistry *registry;
	int			status;
} EnsureWorker;

typedef struct NotificationState
{
	PostgammaInstanceRuntime *runtime;
	uint64_t	expected_generation;
	uint64_t	calls;
	int			status;
} NotificationState;

typedef struct CheckpointCompletionState
{
	PostgammaCheckpointTracker *tracker;
	uint64_t	expected_generation;
	uint64_t	calls;
	int		operation_status;
} CheckpointCompletionState;


static void *ensure_registry(void *argument);
static int record_wake(void *argument, uint64_t generation);
static void record_checkpoint_completion(
	void *argument, uint64_t generation, int operation_status);
static PostgammaBackendExitStatus return_immediately(
	const PostgammaBackendStartInfo *start_info,
	const void *startup_data,
	size_t startup_data_length,
	void *main_argument);
static PostgammaBackendCompletion wait_for_completion(
	PostgammaBackendRegistry *registry);
static void test_concurrent_registry_ownership(void);
static void test_explicit_generation(void);
static void test_embedded_process_policy_and_notifications(void);
static void test_threaded_server_external_process_lifetime(void);
static void test_active_backend_blocks_instance_destruction(void);
static void test_checkpoint_tracker_lifecycle(void);


static void *
ensure_registry(void *argument)
{
	EnsureWorker *worker = argument;
	PostgammaThreadStackInfo stack_info;
	sigset_t	current_mask;

	assert(postgamma_thread_current_stack_info(&stack_info) == 0);
	assert(stack_info.role == POSTGAMMA_THREAD_ROLE_GENERAL);
	assert(stack_info.usable_stack_size >=
		   POSTGAMMA_THREAD_STACK_MINIMUM_USABLE);
	assert(pthread_sigmask(SIG_SETMASK, NULL, &current_mask) == 0);
	assert(sigismember(&current_mask, SIGHUP) == 1);
	assert(sigismember(&current_mask, SIGABRT) == 0);
	assert(sigismember(&current_mask, SIGBUS) == 0);
	assert(sigismember(&current_mask, SIGFPE) == 0);
	assert(sigismember(&current_mask, SIGILL) == 0);
	assert(sigismember(&current_mask, SIGSEGV) == 0);
	worker->status = postgamma_instance_runtime_ensure_backend_registry(
		worker->runtime, 8, &worker->registry);
	return NULL;
}


static int
record_wake(void *argument, uint64_t generation)
{
	NotificationState *state = argument;
	PostgammaInstanceRuntimeTelemetry telemetry;

	assert(generation == state->expected_generation);
	assert(postgamma_instance_runtime_telemetry(
		state->runtime, &telemetry) == 0);
	assert(telemetry.generation == generation);
	state->calls++;
	return state->status;
}


static void
record_checkpoint_completion(
	void *argument, uint64_t generation, int operation_status)
{
	CheckpointCompletionState *state = argument;

	assert(generation == state->expected_generation);
	state->calls++;
	state->operation_status = operation_status;
	assert(postgamma_instance_checkpoint_tracker_wake(state->tracker) == 0);
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

	assert(start_info->stack_status == 0);
	assert(start_info->stack_info.role == POSTGAMMA_THREAD_ROLE_DEDICATED);
	(void) startup_data;
	(void) startup_data_length;
	(void) main_argument;
	return result;
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
		int			status = postgamma_backend_completion_pop(
			registry, &completion);

		if (status == 0)
			return completion;
		assert(status == EAGAIN);
		assert(poll(&descriptor, 1, 5000) == 1);
		assert(descriptor.revents & POLLIN);
	}
}


static void
test_concurrent_registry_ownership(void)
{
	PostgammaInstanceRuntimeOptions options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaInstanceRuntime *first;
	PostgammaInstanceRuntime *second;
	PostgammaBackendRegistry *second_registry;
	PostgammaBackendRegistry *mismatch = NULL;
	PostgammaThread *threads[ENSURE_THREAD_COUNT];
	EnsureWorker workers[ENSURE_THREAD_COUNT];
	PostgammaThreadAttributes attributes = {
		.name = "pgm-ensure",
		.role = POSTGAMMA_THREAD_ROLE_GENERAL,
		.stack_size = 0,
		.guard_size = 0,
	};

	assert(postgamma_instance_runtime_create(&first, &options) == 0);
	assert(postgamma_instance_runtime_create(&second, &options) == 0);
	assert(postgamma_instance_runtime_generation(first) != 0);
	assert(postgamma_instance_runtime_generation(second) != 0);
	assert(postgamma_instance_runtime_generation(first) !=
		   postgamma_instance_runtime_generation(second));
	assert(postgamma_instance_runtime_profile(first) ==
		   POSTGAMMA_RUNTIME_PROFILE_EMBEDDED);
	assert(!postgamma_instance_runtime_bundled_extensions_active(first));
	assert(postgamma_instance_runtime_activate_bundled_extensions(first) == 0);
	assert(postgamma_instance_runtime_bundled_extensions_active(first));

	memset(workers, 0, sizeof(workers));
	for (int index = 0; index < ENSURE_THREAD_COUNT; index++)
	{
		workers[index].runtime = first;
		assert(postgamma_thread_create(
			&threads[index], &attributes, ensure_registry,
			&workers[index]) == 0);
	}
	for (int index = 0; index < ENSURE_THREAD_COUNT; index++)
	{
		assert(postgamma_thread_join(threads[index], NULL) == 0);
		assert(postgamma_thread_destroy(threads[index]) == 0);
		assert(workers[index].status == 0);
		assert(workers[index].registry == workers[0].registry);
	}
	assert(postgamma_instance_runtime_backend_registry(first) ==
		   workers[0].registry);
	assert(postgamma_instance_runtime_ensure_backend_registry(
		first, 9, &mismatch) == EALREADY);
	assert(mismatch == NULL);
	assert(postgamma_instance_runtime_ensure_backend_registry(
		second, 8, &second_registry) == 0);
	assert(second_registry != workers[0].registry);
	assert(postgamma_instance_runtime_destroy(second) == 0);
	assert(postgamma_instance_runtime_destroy(first) == 0);
}


static void
test_explicit_generation(void)
{
	PostgammaInstanceRuntimeOptions options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaInstanceRuntime *explicit_runtime;
	PostgammaInstanceRuntime *automatic_runtime;

	options.generation = UINT64_C(1000000);
	assert(postgamma_instance_runtime_create(
		&explicit_runtime, &options) == 0);
	assert(postgamma_instance_runtime_generation(explicit_runtime) ==
		   options.generation);
	options.generation = 0;
	assert(postgamma_instance_runtime_create(
		&automatic_runtime, &options) == 0);
	assert(postgamma_instance_runtime_generation(automatic_runtime) >
		   postgamma_instance_runtime_generation(explicit_runtime));
	assert(postgamma_instance_runtime_destroy(automatic_runtime) == 0);
	assert(postgamma_instance_runtime_destroy(explicit_runtime) == 0);

	options.generation = UINT64_MAX;
	assert(postgamma_instance_runtime_create(
		&explicit_runtime, &options) == EOVERFLOW);
}


static void
test_embedded_process_policy_and_notifications(void)
{
	PostgammaInstanceRuntimeOptions options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaInstanceRuntime *runtime;
	PostgammaBackendRegistry *registry;
	PostgammaInstanceRuntimeTelemetry telemetry;
	uint64_t	external_process_count;
	uint64_t	pending_signals;
	NotificationState state = {0};

	options.generation = UINT64_C(2000000);
	options.wake_notification = record_wake;
	options.wake_notification_argument = &state;
	assert(postgamma_instance_runtime_create(&runtime, &options) == 0);
	state.runtime = runtime;
	state.expected_generation = postgamma_instance_runtime_generation(runtime);
	assert(postgamma_instance_runtime_ensure_backend_registry(
		runtime, 4, &registry) == 0);
	assert(postgamma_instance_runtime_reserve_external_process(runtime) ==
		   ENOTSUP);
	assert(postgamma_instance_runtime_record_external_process_finish(runtime) ==
		   EPROTO);
	assert(postgamma_instance_runtime_external_process_count(
		runtime, &external_process_count) == 0);
	assert(external_process_count == 0);
	assert(postgamma_instance_runtime_notify_completion(runtime) == 0);
	assert(postgamma_instance_runtime_notify_completion(runtime) == 0);
	assert(postgamma_instance_runtime_notify_supervisor_signal(
		runtime, state.expected_generation - 1, SIGHUP) == ESTALE);
	assert(postgamma_instance_runtime_notify_supervisor_signal(
		runtime, state.expected_generation, 0) == EINVAL);
	assert(postgamma_instance_runtime_notify_supervisor_signal(
		runtime, state.expected_generation,
		POSTGAMMA_INSTANCE_SIGNAL_MAX + 1) == EINVAL);
	assert(postgamma_instance_runtime_notify_supervisor_signal(
		runtime, state.expected_generation, SIGHUP) == 0);
	assert(postgamma_instance_runtime_notify_supervisor_signal(
		runtime, state.expected_generation, SIGHUP) == 0);
	assert(postgamma_instance_runtime_notify_supervisor_signal(
		runtime, state.expected_generation, SIGTERM) == 0);
	assert(state.calls == 5);
	assert(postgamma_instance_runtime_telemetry(runtime, &telemetry) == 0);
	assert(telemetry.generation == state.expected_generation);
	assert(telemetry.profile == POSTGAMMA_RUNTIME_PROFILE_EMBEDDED);
	assert(telemetry.backend_registry_created);
	assert(telemetry.backend_capacity == 4);
	assert(telemetry.completion_notifications == 2);
	assert(telemetry.supervisor_signal_notifications == 3);
	assert(telemetry.pending_supervisor_signals ==
		   ((UINT64_C(1) << (SIGHUP - 1)) |
			(UINT64_C(1) << (SIGTERM - 1))));
	assert(postgamma_instance_runtime_take_supervisor_signals(
		runtime, state.expected_generation - 1, &pending_signals) == ESTALE);
	assert(pending_signals == 0);
	assert(postgamma_instance_runtime_take_supervisor_signals(
		runtime, state.expected_generation, &pending_signals) == 0);
	assert(pending_signals == telemetry.pending_supervisor_signals);
	assert(postgamma_instance_runtime_take_supervisor_signals(
		runtime, state.expected_generation, &pending_signals) == EAGAIN);
	assert(pending_signals == 0);
	state.status = EIO;
	assert(postgamma_instance_runtime_notify_completion(runtime) == EIO);
	state.status = 0;
	assert(state.calls == 6);
	assert(telemetry.external_process_launch_rejections == 1);
	assert(telemetry.external_processes_started == 0);
	assert(telemetry.external_processes_finished == 0);
	assert(telemetry.external_process_reservations == 0);
	assert(telemetry.external_processes_active == 0);
	assert(telemetry.backend.forbidden_process_launch_attempts == 1);
	assert(postgamma_instance_runtime_destroy(runtime) == 0);
}


static void
test_threaded_server_external_process_lifetime(void)
{
	PostgammaInstanceRuntimeOptions options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaInstanceRuntime *runtime;
	PostgammaBackendRegistry *registry;
	PostgammaInstanceRuntimeTelemetry telemetry;
	uint64_t	external_process_count;

	options.profile = POSTGAMMA_RUNTIME_PROFILE_THREADED_SERVER;
	assert(postgamma_instance_runtime_create(&runtime, &options) == 0);
	assert(postgamma_instance_runtime_generation(runtime) != 0);
	assert(postgamma_instance_runtime_ensure_backend_registry(
		runtime, 2, &registry) == 0);
	assert(postgamma_instance_runtime_reserve_external_process(runtime) == 0);
	assert(postgamma_instance_runtime_external_process_count(
		runtime, &external_process_count) == 0);
	assert(external_process_count == 1);
	assert(postgamma_instance_runtime_destroy(runtime) == EBUSY);
	assert(postgamma_instance_runtime_cancel_external_process(runtime) == 0);
	assert(postgamma_instance_runtime_external_process_count(
		runtime, &external_process_count) == 0);
	assert(external_process_count == 0);
	assert(postgamma_instance_runtime_reserve_external_process(runtime) == 0);
	assert(postgamma_instance_runtime_commit_external_process(runtime) == 0);
	assert(postgamma_instance_runtime_destroy(runtime) == EBUSY);
	assert(postgamma_instance_runtime_record_external_process_finish(runtime) == 0);
	assert(postgamma_instance_runtime_telemetry(runtime, &telemetry) == 0);
	assert(telemetry.external_processes_started == 1);
	assert(telemetry.external_processes_finished == 1);
	assert(telemetry.external_processes_active == 0);
	assert(telemetry.external_process_reservations == 0);
	assert(telemetry.external_process_launch_rejections == 0);
	assert(telemetry.backend.backend_process_launches == 0);
	assert(postgamma_instance_runtime_destroy(runtime) == 0);
}


static void
test_active_backend_blocks_instance_destruction(void)
{
	PostgammaInstanceRuntimeOptions options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaInstanceRuntime *runtime;
	PostgammaBackendRegistry *registry;
	PostgammaBackendLaunchRequest request;
	PostgammaBackendHandle handle;
	PostgammaBackendCompletion completion;

	assert(postgamma_instance_runtime_create(&runtime, &options) == 0);
	assert(postgamma_instance_runtime_ensure_backend_registry(
		runtime, 1, &registry) == 0);
	memset(&request, 0, sizeof(request));
	request.backend_type = 1;
	request.execution_class = POSTGAMMA_BACKEND_CLASS_DEDICATED;
	request.thread_name = "pgm-owned";
	request.main_function = return_immediately;
	assert(postgamma_backend_launch(registry, &request, &handle) == 0);
	assert(handle.id != 0);
	assert(postgamma_instance_runtime_destroy(runtime) == EBUSY);
	completion = wait_for_completion(registry);
	assert(completion.handle.id == handle.id);
	assert(postgamma_backend_completion_join(registry, &completion) == 0);
	assert(postgamma_instance_runtime_destroy(runtime) == 0);
}


static void
test_checkpoint_tracker_lifecycle(void)
{
	PostgammaInstanceRuntimeOptions options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaInstanceRuntime *runtime;
	PostgammaCheckpointTracker *tracker;
	CheckpointCompletionState state = {0};
	struct pollfd descriptor = {.fd = -1, .events = POLLIN};
	uint64_t	wake_count = 0;

	options.generation = UINT64_C(3000000);
	assert(postgamma_instance_runtime_create(&runtime, &options) == 0);
	state.expected_generation = options.generation;
	assert(postgamma_instance_checkpoint_tracker_create(
		runtime, options.generation, record_checkpoint_completion,
		&state, &tracker) == 0);
	state.tracker = tracker;
	descriptor.fd = postgamma_instance_checkpoint_tracker_waitable_fd(tracker);
	assert(descriptor.fd >= 0);
	assert(poll(&descriptor, 1, 0) == 0);
	assert(postgamma_instance_runtime_destroy(runtime) == EBUSY);
	assert(postgamma_instance_checkpoint_tracker_arm(
		tracker, options.generation - 1, INT32_C(7), INT32_C(2)) == EINVAL);
	assert(postgamma_instance_checkpoint_tracker_arm(
		tracker, options.generation, INT32_C(7), INT32_C(2)) == 0);
	assert(postgamma_instance_checkpoint_tracker_arm(
		tracker, options.generation, INT32_C(7), INT32_C(2)) == EALREADY);
	assert(postgamma_instance_checkpoint_observe(
		runtime, options.generation, INT32_C(7), INT32_C(2)) == 0);
	assert(state.calls == 0);
	assert(postgamma_instance_checkpoint_observe(
		runtime, options.generation, INT32_C(8), INT32_C(2)) == 0);
	assert(state.calls == 1);
	assert(state.operation_status == 0);
	assert(poll(&descriptor, 1, 0) == 1);
	assert(descriptor.revents & POLLIN);
	assert(postgamma_instance_checkpoint_tracker_wake_drain(
		tracker, &wake_count) == 0);
	assert(wake_count == 1);
	assert(postgamma_instance_checkpoint_tracker_finish(
		tracker, options.generation, 0) == EALREADY);
	assert(postgamma_instance_checkpoint_tracker_destroy(tracker) == 0);

	memset(&state, 0, sizeof(state));
	state.expected_generation = options.generation;
	assert(postgamma_instance_checkpoint_tracker_create(
		runtime, options.generation, record_checkpoint_completion,
		&state, &tracker) == 0);
	state.tracker = tracker;
	assert(postgamma_instance_checkpoint_tracker_arm(
		tracker, options.generation, INT32_MAX, INT32_MAX) == 0);
	assert(postgamma_instance_checkpoint_observe(
		runtime, options.generation, INT32_MIN, INT32_MIN) == 0);
	assert(state.calls == 1);
	assert(state.operation_status == EIO);
	assert(postgamma_instance_checkpoint_tracker_destroy(tracker) == 0);

	memset(&state, 0, sizeof(state));
	state.expected_generation = options.generation;
	assert(postgamma_instance_checkpoint_tracker_create(
		runtime, options.generation, record_checkpoint_completion,
		&state, &tracker) == 0);
	state.tracker = tracker;
	assert(postgamma_instance_checkpoint_tracker_finish(
		tracker, options.generation, ECANCELED) == 0);
	assert(state.calls == 1);
	assert(state.operation_status == ECANCELED);
	assert(postgamma_instance_checkpoint_tracker_arm(
		tracker, options.generation, 0, 0) == ECANCELED);
	assert(postgamma_instance_checkpoint_tracker_destroy(tracker) == 0);
	assert(postgamma_instance_runtime_destroy(runtime) == 0);
}


int
main(void)
{
	test_concurrent_registry_ownership();
	test_explicit_generation();
	test_embedded_process_policy_and_notifications();
	test_threaded_server_external_process_lifetime();
	test_active_backend_blocks_instance_destruction();
	test_checkpoint_tracker_lifecycle();
	return 0;
}
