#include "postgamma/private/supervisor.h"

#include "postgamma/thread_runtime.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>


#define TEST_TIMEOUT_NS UINT64_C(5000000000)
#define LIFECYCLE_REPETITIONS 100


typedef struct TestKernel
{
	int			boot_status;
	int			shutdown_status;
	int			fail_dispatch_status;
	bool		fail_dispatch_requires_restart;
	_Atomic bool block_first_dispatch;
	_Atomic bool first_dispatch_entered;
	_Atomic bool release_first_dispatch;
	_Atomic unsigned int notification_wakes;
	unsigned int boot_calls;
	unsigned int dispatch_calls;
	unsigned int shutdown_calls;
	PostgammaSupervisorShutdownMode observed_shutdown_mode;
	PostgammaThreadStackInfo supervisor_stack;
} TestKernel;


static uint64_t deadline_after(uint64_t interval_ns);
static int boot_kernel(PostgammaSupervisor *supervisor, void *argument);
static int dispatch_control(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorControlKind kind,
	void *payload,
	void *argument);
static int run_kernel_control_loop(
	PostgammaSupervisor *supervisor,
	void *argument);
static int shutdown_kernel(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorShutdownMode mode,
	void *argument);
static PostgammaSupervisor *create_supervisor(
	uint64_t generation, size_t capacity, TestKernel *kernel);
static void wait_for_atomic_flag(_Atomic bool *flag);
static void wait_and_destroy_ticket(
	PostgammaSupervisorTicket *ticket, int expected_status);
static void test_bounded_queue_and_shutdown(void);
static void test_kernel_driven_control_loop(void);
static void test_boot_failure(void);
static void test_fail_stop(void);
static void test_shutdown_failure_record(void);
static void test_repeated_lifecycle(void);


static uint64_t
deadline_after(uint64_t interval_ns)
{
	uint64_t	now = postgamma_monotonic_now_ns();

	assert(now != 0);
	assert(now <= UINT64_MAX - interval_ns);
	return now + interval_ns;
}


static int
boot_kernel(PostgammaSupervisor *supervisor, void *argument)
{
	TestKernel *kernel = argument;

	kernel->boot_calls++;
	assert(postgamma_thread_current_stack_info(
		&kernel->supervisor_stack) == 0);
	assert(kernel->supervisor_stack.role ==
		   POSTGAMMA_THREAD_ROLE_SUPERVISOR);
	assert(postgamma_supervisor_mark_recovering(supervisor) == 0);
	return kernel->boot_status;
}


static int
dispatch_control(PostgammaSupervisor *supervisor,
				 PostgammaSupervisorControlKind kind,
				 void *payload,
				 void *argument)
{
	TestKernel *kernel = argument;
	int		   *value = payload;

	assert(kind != POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN);
	if (atomic_load_explicit(
			&kernel->block_first_dispatch, memory_order_acquire) &&
		kernel->dispatch_calls == 0)
	{
		atomic_store_explicit(
			&kernel->first_dispatch_entered, true, memory_order_release);
		while (!atomic_load_explicit(
				&kernel->release_first_dispatch, memory_order_acquire))
			(void) sched_yield();
	}
	kernel->dispatch_calls++;
	if (value != NULL)
		(*value)++;
	if (kernel->fail_dispatch_status != 0)
	{
		int			failure_status = kernel->fail_dispatch_status;

		kernel->fail_dispatch_status = 0;
		assert(postgamma_supervisor_fail_detailed(
			supervisor, failure_status,
			kernel->fail_dispatch_requires_restart) == 0);
	}
	return 0;
}


static int
run_kernel_control_loop(PostgammaSupervisor *supervisor, void *argument)
{
	TestKernel *kernel = argument;
	struct pollfd descriptor = {
		.fd = postgamma_supervisor_control_wake_fd(supervisor),
		.events = POLLIN,
	};

	assert(descriptor.fd >= 0);
	assert(postgamma_supervisor_mark_ready(supervisor) == 0);
	for (;;)
	{
		bool		took_control = false;
		uint64_t	wake_count;
		int			poll_status = poll(&descriptor, 1, 5000);

		assert(poll_status == 1);
		assert(descriptor.revents & POLLIN);
		assert(postgamma_supervisor_control_wake_drain(
			supervisor, &wake_count) == 0);
		assert(wake_count != 0);
		for (;;)
		{
			PostgammaSupervisorControl control;
			int			status = postgamma_supervisor_control_take(
				supervisor, &control);

			if (status == EAGAIN)
				break;
			assert(status == 0);
			took_control = true;
			if (control.kind == POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN)
			{
				status = shutdown_kernel(
					supervisor, control.shutdown_mode, kernel);
				assert(postgamma_supervisor_control_complete(
					supervisor, &control, status) == 0);
				return status;
			}
			status = dispatch_control(
				supervisor, control.kind, control.payload, kernel);
			assert(postgamma_supervisor_control_complete(
				supervisor, &control, status) == 0);
			if (status != 0)
				return status;
		}
		if (!took_control)
			atomic_fetch_add_explicit(
				&kernel->notification_wakes, 1, memory_order_release);
	}
}


static int
shutdown_kernel(PostgammaSupervisor *supervisor,
				PostgammaSupervisorShutdownMode mode,
				void *argument)
{
	TestKernel *kernel = argument;

	assert(postgamma_supervisor_state(supervisor) ==
		   POSTGAMMA_SUPERVISOR_STATE_STOPPING);
	kernel->shutdown_calls++;
	kernel->observed_shutdown_mode = mode;
	return kernel->shutdown_status;
}


static PostgammaSupervisor *
create_supervisor(uint64_t generation, size_t capacity, TestKernel *kernel)
{
	PostgammaSupervisorOptions options = POSTGAMMA_SUPERVISOR_OPTIONS_INIT;
	PostgammaSupervisor *supervisor;

	options.generation = generation;
	options.queue_capacity = capacity;
	options.callbacks.boot = boot_kernel;
	options.callbacks.dispatch = dispatch_control;
	options.callbacks.shutdown = shutdown_kernel;
	options.callback_argument = kernel;
	assert(postgamma_supervisor_create(&options, &supervisor) == 0);
	assert(postgamma_supervisor_generation(supervisor) == generation);
	return supervisor;
}


static void
wait_for_atomic_flag(_Atomic bool *flag)
{
	uint64_t	deadline = deadline_after(TEST_TIMEOUT_NS);

	while (!atomic_load_explicit(flag, memory_order_acquire))
	{
		assert(postgamma_monotonic_now_ns() < deadline);
		(void) sched_yield();
	}
}


static void
wait_and_destroy_ticket(
	PostgammaSupervisorTicket *ticket, int expected_status)
{
	int			operation_status = -1;

	assert(postgamma_supervisor_ticket_wait(
		ticket, deadline_after(TEST_TIMEOUT_NS), &operation_status) == 0);
	assert(operation_status == expected_status);
	assert(postgamma_supervisor_ticket_destroy(ticket) == 0);
}


static void
test_bounded_queue_and_shutdown(void)
{
	TestKernel kernel;
	PostgammaSupervisor *supervisor;
	PostgammaSupervisorTicket *first;
	PostgammaSupervisorTicket *queued[3];
	PostgammaSupervisorTicket *shutdown;
	PostgammaSupervisorTicket *rejected = NULL;
	PostgammaSupervisorTelemetry telemetry;
	PostgammaSupervisorFailureRecord failure =
		POSTGAMMA_SUPERVISOR_FAILURE_RECORD_INIT;
	int			values[4] = {0};

	memset(&kernel, 0, sizeof(kernel));
	atomic_init(&kernel.notification_wakes, 0);
	atomic_init(&kernel.block_first_dispatch, true);
	atomic_init(&kernel.first_dispatch_entered, false);
	atomic_init(&kernel.release_first_dispatch, false);
	supervisor = create_supervisor(UINT64_C(41), 4, &kernel);
	assert(postgamma_supervisor_start(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(postgamma_supervisor_state(supervisor) ==
		   POSTGAMMA_SUPERVISOR_STATE_READY);
	assert(postgamma_supervisor_submit(
		supervisor, UINT64_C(40), POSTGAMMA_SUPERVISOR_CONTROL_CONNECT,
		NULL, &rejected) == ESTALE);
	assert(rejected == NULL);
	assert(postgamma_supervisor_submit(
		supervisor, UINT64_C(41), POSTGAMMA_SUPERVISOR_CONTROL_CONNECT,
		&values[0], &first) == 0);
	wait_for_atomic_flag(&kernel.first_dispatch_entered);
	for (int index = 0; index < 3; index++)
	{
		assert(postgamma_supervisor_submit(
			supervisor, UINT64_C(41),
			POSTGAMMA_SUPERVISOR_CONTROL_RELOAD,
			&values[index + 1], &queued[index]) == 0);
	}
	assert(postgamma_supervisor_submit(
		supervisor, UINT64_C(41), POSTGAMMA_SUPERVISOR_CONTROL_CANCEL,
		NULL, &rejected) == EAGAIN);
	assert(rejected == NULL);
	assert(postgamma_supervisor_request_shutdown(
		supervisor, UINT64_C(41), POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST,
		&shutdown) == 0);
	assert(postgamma_supervisor_state(supervisor) ==
		   POSTGAMMA_SUPERVISOR_STATE_QUIESCING);
	assert(postgamma_supervisor_submit(
		supervisor, UINT64_C(41), POSTGAMMA_SUPERVISOR_CONTROL_CONNECT,
		NULL, &rejected) == ESHUTDOWN);
	atomic_store_explicit(
		&kernel.release_first_dispatch, true, memory_order_release);
	wait_and_destroy_ticket(first, 0);
	for (int index = 0; index < 3; index++)
		wait_and_destroy_ticket(queued[index], 0);
	wait_and_destroy_ticket(shutdown, 0);
	assert(postgamma_supervisor_join(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(kernel.boot_calls == 1);
	assert(kernel.dispatch_calls == 4);
	assert(kernel.shutdown_calls == 1);
	assert(kernel.observed_shutdown_mode ==
		   POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST);
	for (int index = 0; index < 4; index++)
		assert(values[index] == 1);
	assert(postgamma_supervisor_telemetry(supervisor, &telemetry) == 0);
	assert(telemetry.state == POSTGAMMA_SUPERVISOR_STATE_CLOSED);
	assert(telemetry.queue_depth == 0);
	assert(telemetry.queue_depth_peak == 4);
	assert(telemetry.controls_submitted == 5);
	assert(telemetry.controls_completed == 5);
	assert(telemetry.stale_controls_rejected == 1);
	assert(telemetry.controls_rejected_after_quiesce == 1);
	assert(telemetry.shutdown_requests == 1);
	assert(telemetry.threads_started == 1);
	assert(telemetry.threads_joined == 1);
	assert(telemetry.active_tickets == 0);
	assert(telemetry.failure_attempts == 0);
	assert(telemetry.failure_status == 0);
	assert(!telemetry.last_failure.present);
	assert(telemetry.last_failure.struct_size ==
		   sizeof(telemetry.last_failure));
	assert(postgamma_supervisor_failure_record(supervisor, &failure) == 0);
	assert(!failure.present);
	failure.struct_size = 0;
	assert(postgamma_supervisor_failure_record(supervisor, &failure) == EINVAL);
	assert(postgamma_supervisor_destroy(supervisor) == 0);
}


static void
test_kernel_driven_control_loop(void)
{
	PostgammaSupervisorOptions options = POSTGAMMA_SUPERVISOR_OPTIONS_INIT;
	TestKernel kernel;
	PostgammaSupervisor *supervisor;
	PostgammaSupervisorTicket *control_ticket;
	PostgammaSupervisorTicket *shutdown_ticket;
	PostgammaSupervisorControl unavailable;
	PostgammaSupervisorTelemetry telemetry;
	uint64_t	notification_deadline;
	int			value = 0;

	memset(&kernel, 0, sizeof(kernel));
	atomic_init(&kernel.notification_wakes, 0);
	options.generation = UINT64_C(44);
	options.queue_capacity = 4;
	options.callbacks.boot = boot_kernel;
	options.callbacks.run = run_kernel_control_loop;
	options.callback_argument = &kernel;
	assert(postgamma_supervisor_create(&options, &supervisor) == 0);
	assert(postgamma_supervisor_start(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(postgamma_supervisor_state(supervisor) ==
		   POSTGAMMA_SUPERVISOR_STATE_READY);
	assert(postgamma_supervisor_control_take(
		supervisor, &unavailable) == EPERM);
	assert(postgamma_supervisor_control_notify(
		supervisor, UINT64_C(43)) == ESTALE);
	assert(postgamma_supervisor_control_notify(
		supervisor, UINT64_C(44)) == 0);
	notification_deadline = deadline_after(TEST_TIMEOUT_NS);

	while (atomic_load_explicit(
			&kernel.notification_wakes, memory_order_acquire) == 0)
	{
		assert(postgamma_monotonic_now_ns() < notification_deadline);
		(void) sched_yield();
	}
	assert(postgamma_supervisor_submit(
		supervisor, UINT64_C(44), POSTGAMMA_SUPERVISOR_CONTROL_CONNECT,
		&value, &control_ticket) == 0);
	wait_and_destroy_ticket(control_ticket, 0);
	assert(value == 1);
	assert(postgamma_supervisor_request_shutdown(
		supervisor, UINT64_C(44),
		POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE,
		&shutdown_ticket) == 0);
	wait_and_destroy_ticket(shutdown_ticket, 0);
	assert(postgamma_supervisor_join(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(kernel.boot_calls == 1);
	assert(kernel.dispatch_calls == 1);
	assert(kernel.shutdown_calls == 1);
	assert(kernel.observed_shutdown_mode ==
		   POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE);
	assert(postgamma_supervisor_telemetry(supervisor, &telemetry) == 0);
	assert(telemetry.state == POSTGAMMA_SUPERVISOR_STATE_CLOSED);
	assert(telemetry.controls_submitted == 2);
	assert(telemetry.controls_completed == 2);
	assert(telemetry.control_notifications == 1);
	assert(telemetry.stale_controls_rejected == 1);
	assert(telemetry.active_tickets == 0);
	assert(telemetry.failure_attempts == 0);
	assert(!telemetry.last_failure.present);
	assert(postgamma_supervisor_destroy(supervisor) == 0);
}


static void
test_boot_failure(void)
{
	TestKernel kernel;
	PostgammaSupervisor *supervisor;
	PostgammaSupervisorTelemetry telemetry;
	PostgammaSupervisorFailureRecord failure =
		POSTGAMMA_SUPERVISOR_FAILURE_RECORD_INIT;

	memset(&kernel, 0, sizeof(kernel));
	kernel.boot_status = EIO;
	supervisor = create_supervisor(UINT64_C(42), 4, &kernel);
	assert(postgamma_supervisor_start(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == EIO);
	assert(postgamma_supervisor_state(supervisor) ==
		   POSTGAMMA_SUPERVISOR_STATE_FAILED);
	assert(postgamma_supervisor_join(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(postgamma_supervisor_telemetry(supervisor, &telemetry) == 0);
	assert(telemetry.failure_status == EIO);
	assert(telemetry.threads_started == 1);
	assert(telemetry.threads_joined == 1);
	assert(telemetry.failure_attempts >= 1);
	assert(telemetry.last_failure.present);
	assert(telemetry.last_failure.generation == UINT64_C(42));
	assert(telemetry.last_failure.ordinal == 1);
	assert(telemetry.last_failure.recorded_at_ns != 0);
	assert(telemetry.last_failure.origin ==
		   POSTGAMMA_SUPERVISOR_FAILURE_BOOT);
	assert(telemetry.last_failure.state_before_failure ==
		   POSTGAMMA_SUPERVISOR_STATE_RECOVERING);
	assert(telemetry.last_failure.control_kind ==
		   POSTGAMMA_SUPERVISOR_CONTROL_NONE);
	assert(telemetry.last_failure.control_sequence == 0);
	assert(telemetry.last_failure.status == EIO);
	assert(!telemetry.last_failure.process_restart_required);
	assert(telemetry.last_failure.supervisor_thread_exited);
	assert(telemetry.last_failure.supervisor_thread_joined);
	assert(postgamma_supervisor_failure_record(supervisor, &failure) == 0);
	assert(memcmp(&failure, &telemetry.last_failure, sizeof(failure)) == 0);
	assert(strcmp(postgamma_supervisor_failure_origin_name(failure.origin),
		   "boot") == 0);
	assert(postgamma_supervisor_destroy(supervisor) == 0);
}


static void
test_fail_stop(void)
{
	TestKernel kernel;
	PostgammaSupervisor *supervisor;
	PostgammaSupervisorTicket *failed;
	PostgammaSupervisorTicket *discarded;
	PostgammaSupervisorTelemetry telemetry;
	PostgammaSupervisorFailureRecord failure =
		POSTGAMMA_SUPERVISOR_FAILURE_RECORD_INIT;
	int			value = 0;

	memset(&kernel, 0, sizeof(kernel));
	atomic_init(&kernel.block_first_dispatch, true);
	atomic_init(&kernel.first_dispatch_entered, false);
	atomic_init(&kernel.release_first_dispatch, false);
	kernel.fail_dispatch_status = EFAULT;
	kernel.fail_dispatch_requires_restart = true;
	supervisor = create_supervisor(UINT64_C(43), 4, &kernel);
	assert(postgamma_supervisor_start(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(postgamma_supervisor_submit(
		supervisor, UINT64_C(43), POSTGAMMA_SUPERVISOR_CONTROL_CONNECT,
		&value, &failed) == 0);
	wait_for_atomic_flag(&kernel.first_dispatch_entered);
	assert(postgamma_supervisor_submit(
		supervisor, UINT64_C(43), POSTGAMMA_SUPERVISOR_CONTROL_RELOAD,
		&value, &discarded) == 0);
	atomic_store_explicit(
		&kernel.release_first_dispatch, true, memory_order_release);
	wait_and_destroy_ticket(failed, EFAULT);
	wait_and_destroy_ticket(discarded, ESHUTDOWN);
	assert(postgamma_supervisor_join(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(postgamma_supervisor_state(supervisor) ==
		   POSTGAMMA_SUPERVISOR_STATE_FAILED);
	assert(value == 1);
	assert(postgamma_supervisor_fail(supervisor, EIO) == 0);
	assert(postgamma_supervisor_telemetry(supervisor, &telemetry) == 0);
	assert(telemetry.failure_attempts >= 2);
	assert(telemetry.failure_status == EFAULT);
	assert(telemetry.last_failure.present);
	assert(telemetry.last_failure.generation == UINT64_C(43));
	assert(telemetry.last_failure.ordinal == 1);
	assert(telemetry.last_failure.origin ==
		   POSTGAMMA_SUPERVISOR_FAILURE_HOST_FAIL_STOP);
	assert(telemetry.last_failure.state_before_failure ==
		   POSTGAMMA_SUPERVISOR_STATE_READY);
	assert(telemetry.last_failure.control_kind ==
		   POSTGAMMA_SUPERVISOR_CONTROL_CONNECT);
	assert(telemetry.last_failure.control_sequence == 1);
	assert(telemetry.last_failure.status == EFAULT);
	assert(telemetry.last_failure.process_restart_required);
	assert(telemetry.last_failure.supervisor_thread_exited);
	assert(telemetry.last_failure.supervisor_thread_joined);
	assert(postgamma_supervisor_failure_record(supervisor, &failure) == 0);
	assert(memcmp(&failure, &telemetry.last_failure, sizeof(failure)) == 0);
	assert(postgamma_supervisor_destroy(supervisor) == 0);
}


static void
test_shutdown_failure_record(void)
{
	TestKernel kernel;
	PostgammaSupervisor *supervisor;
	PostgammaSupervisorTicket *shutdown;
	PostgammaSupervisorTelemetry telemetry;

	memset(&kernel, 0, sizeof(kernel));
	kernel.shutdown_status = ENOSPC;
	supervisor = create_supervisor(UINT64_C(45), 4, &kernel);
	assert(postgamma_supervisor_start(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(postgamma_supervisor_request_shutdown(
		supervisor, UINT64_C(45), POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE,
		&shutdown) == 0);
	wait_and_destroy_ticket(shutdown, ENOSPC);
	assert(postgamma_supervisor_join(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(postgamma_supervisor_telemetry(supervisor, &telemetry) == 0);
	assert(telemetry.state == POSTGAMMA_SUPERVISOR_STATE_FAILED);
	assert(telemetry.failure_status == ENOSPC);
	assert(telemetry.last_failure.present);
	assert(telemetry.last_failure.origin ==
		   POSTGAMMA_SUPERVISOR_FAILURE_SHUTDOWN);
	assert(telemetry.last_failure.state_before_failure ==
		   POSTGAMMA_SUPERVISOR_STATE_STOPPING);
	assert(telemetry.last_failure.control_kind ==
		   POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN);
	assert(telemetry.last_failure.control_sequence == 1);
	assert(telemetry.last_failure.status == ENOSPC);
	assert(!telemetry.last_failure.process_restart_required);
	assert(telemetry.last_failure.supervisor_thread_exited);
	assert(telemetry.last_failure.supervisor_thread_joined);
	assert(strcmp(postgamma_supervisor_failure_origin_name(
		   telemetry.last_failure.origin), "shutdown") == 0);
	assert(strcmp(postgamma_supervisor_failure_origin_name(
		   (PostgammaSupervisorFailureOrigin) 999), "invalid") == 0);
	assert(postgamma_supervisor_destroy(supervisor) == 0);
}


static void
test_repeated_lifecycle(void)
{
	for (uint64_t iteration = 0;
		 iteration < LIFECYCLE_REPETITIONS;
		 iteration++)
	{
		TestKernel kernel;
		PostgammaSupervisor *supervisor;
		PostgammaSupervisorTicket *control;
		PostgammaSupervisorTicket *shutdown;
		int			value = 0;

		memset(&kernel, 0, sizeof(kernel));
		supervisor = create_supervisor(
			UINT64_C(1000) + iteration, 8, &kernel);
		assert(postgamma_supervisor_start(
			supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
		assert(postgamma_supervisor_submit(
			supervisor, UINT64_C(1000) + iteration,
			POSTGAMMA_SUPERVISOR_CONTROL_CONNECT,
			&value, &control) == 0);
		wait_and_destroy_ticket(control, 0);
		assert(postgamma_supervisor_request_shutdown(
			supervisor, UINT64_C(1000) + iteration,
			POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST, &shutdown) == 0);
		wait_and_destroy_ticket(shutdown, 0);
		assert(postgamma_supervisor_join(
			supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
		assert(value == 1);
		assert(postgamma_supervisor_destroy(supervisor) == 0);
	}
}


int
main(void)
{
	test_bounded_queue_and_shutdown();
	test_kernel_driven_control_loop();
	test_boot_failure();
	test_fail_stop();
	test_shutdown_failure_record();
	test_repeated_lifecycle();
	return 0;
}
