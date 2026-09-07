#include "postgamma/private/kernel_supervisor_adapter.h"

#include "postgamma/thread_runtime.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>


#define TEST_GENERATION UINT64_C(91)
#define TEST_TIMEOUT_NS UINT64_C(5000000000)


typedef struct TestKernel
{
	PostgammaKernelHostProvider host;
	unsigned int controls;
	unsigned int notifications;
} TestKernel;


static uint64_t
deadline_after(uint64_t interval_ns)
{
	uint64_t	now = postgamma_monotonic_now_ns();

	assert(now != 0);
	assert(now <= UINT64_MAX - interval_ns);
	return now + interval_ns;
}


static int
run_kernel(PostgammaSupervisor *supervisor, void *argument)
{
	TestKernel *kernel = argument;
	struct pollfd descriptor = {
		.fd = -1,
		.events = POLLIN,
	};

	(void) supervisor;
	assert(kernel->host.mark_recovering(
		kernel->host.context, TEST_GENERATION) == 0);
	assert(kernel->host.control_wake_fd(
		kernel->host.context, TEST_GENERATION, &descriptor.fd) == 0);
	assert(descriptor.fd >= 0);
	assert(kernel->host.mark_ready(
		kernel->host.context, TEST_GENERATION) == 0);
	for (;;)
	{
		uint64_t	wake_count = 0;

		assert(poll(&descriptor, 1, 5000) == 1);
		assert(descriptor.revents & POLLIN);
		assert(kernel->host.control_wake_drain(
			kernel->host.context, TEST_GENERATION, &wake_count) == 0);
		assert(wake_count != 0);
		for (;;)
		{
			PostgammaKernelControl control;
			int			status = kernel->host.control_take(
				kernel->host.context, TEST_GENERATION, &control);

			if (status == EAGAIN)
			{
				kernel->notifications++;
				break;
			}
			assert(status == 0);
			kernel->controls++;
			assert(control.generation == TEST_GENERATION);
			if (control.kind == POSTGAMMA_KERNEL_CONTROL_SHUTDOWN)
			{
				assert(control.shutdown_mode ==
					   POSTGAMMA_KERNEL_SHUTDOWN_FAST);
				assert(kernel->host.control_complete(
					kernel->host.context, &control, 0) == 0);
				return 0;
			}
			assert(control.kind == POSTGAMMA_KERNEL_CONTROL_RELOAD);
			assert(kernel->host.control_complete(
				kernel->host.context, &control, 0) == 0);
		}
	}
}


int
main(void)
{
	PostgammaSupervisorOptions options = POSTGAMMA_SUPERVISOR_OPTIONS_INIT;
	PostgammaSupervisor *supervisor;
	PostgammaSupervisorTicket *control_ticket;
	PostgammaSupervisorTicket *shutdown_ticket;
	PostgammaSupervisorTelemetry telemetry;
	TestKernel kernel;
	int			operation_status;

	memset(&kernel, 0, sizeof(kernel));
	options.generation = TEST_GENERATION;
	options.queue_capacity = 4;
	options.callbacks.run = run_kernel;
	options.callback_argument = &kernel;
	assert(postgamma_supervisor_create(&options, &supervisor) == 0);
	assert(postgamma_kernel_supervisor_provider_init(
		supervisor, &kernel.host) == 0);
	assert(kernel.host.abi_version ==
		   POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION);
	assert(kernel.host.struct_size == sizeof(kernel.host));
	assert(kernel.host.capabilities ==
		   (POSTGAMMA_KERNEL_HOST_CAP_CONTROL_WAKE_FD |
			POSTGAMMA_KERNEL_HOST_CAP_ASYNC_NOTIFICATION |
			POSTGAMMA_KERNEL_HOST_CAP_FAIL_STOP));
	assert(postgamma_supervisor_start(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(kernel.host.control_notify(
		kernel.host.context, TEST_GENERATION - 1) == ESTALE);
	assert(kernel.host.control_notify(
		kernel.host.context, TEST_GENERATION) == 0);
	assert(postgamma_supervisor_submit(
		supervisor, TEST_GENERATION,
		POSTGAMMA_SUPERVISOR_CONTROL_RELOAD,
		NULL, &control_ticket) == 0);
	assert(postgamma_supervisor_ticket_wait(
		control_ticket, deadline_after(TEST_TIMEOUT_NS),
		&operation_status) == 0);
	assert(operation_status == 0);
	assert(postgamma_supervisor_ticket_destroy(control_ticket) == 0);
	assert(postgamma_supervisor_request_shutdown(
		supervisor, TEST_GENERATION,
		POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST, &shutdown_ticket) == 0);
	assert(postgamma_supervisor_ticket_wait(
		shutdown_ticket, deadline_after(TEST_TIMEOUT_NS),
		&operation_status) == 0);
	assert(operation_status == 0);
	assert(postgamma_supervisor_ticket_destroy(shutdown_ticket) == 0);
	assert(postgamma_supervisor_join(
		supervisor, deadline_after(TEST_TIMEOUT_NS)) == 0);
	assert(kernel.controls == 2);
	assert(postgamma_supervisor_telemetry(supervisor, &telemetry) == 0);
	assert(telemetry.control_notifications == 1);
	assert(telemetry.stale_controls_rejected == 1);
	assert(postgamma_supervisor_destroy(supervisor) == 0);
	return 0;
}
