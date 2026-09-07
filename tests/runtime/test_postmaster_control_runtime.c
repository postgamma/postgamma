#include "postgamma/postmaster_control_runtime.h"

#include <assert.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>


#define TEST_GENERATION UINT64_C(3000000)


typedef struct TestHost
{
	uint64_t	generation;
	PostgammaWakeTarget *wake_target;
	PostgammaKernelControl queued_control;
	PostgammaKernelControl completed_control;
	unsigned int recovering_calls;
	unsigned int ready_calls;
	unsigned int notify_calls;
	unsigned int take_calls;
	unsigned int complete_calls;
	unsigned int fail_calls;
	int			operation_status;
	int			failure_status;
	bool		control_queued;
} TestHost;


typedef struct BindWorker
{
	PostgammaPostmasterControlRuntime *control_runtime;
	int			status;
} BindWorker;


static int validate_generation(TestHost *host, uint64_t generation);
static int mark_recovering(void *context, uint64_t generation);
static int mark_ready(void *context, uint64_t generation);
static int control_wake_fd(
	void *context, uint64_t generation, int *wake_fd);
static int control_wake_drain(
	void *context, uint64_t generation, uint64_t *wake_count);
static int control_notify(void *context, uint64_t generation);
static int control_take(
	void *context, uint64_t generation, PostgammaKernelControl *control);
static int control_complete(
	void *context, PostgammaKernelControl *control, int operation_status);
static int fail_host(
	void *context, uint64_t generation, int failure_status);
static void *try_bind(void *argument);
static PostgammaKernelHostProvider make_provider(TestHost *host);


static int
validate_generation(TestHost *host, uint64_t generation)
{
	return host != NULL && generation == host->generation ? 0 : ESTALE;
}


static int
mark_recovering(void *context, uint64_t generation)
{
	TestHost   *host = context;
	int			status = validate_generation(host, generation);

	if (status == 0)
		host->recovering_calls++;
	return status;
}


static int
mark_ready(void *context, uint64_t generation)
{
	TestHost   *host = context;
	int			status = validate_generation(host, generation);

	if (status == 0)
		host->ready_calls++;
	return status;
}


static int
control_wake_fd(void *context, uint64_t generation, int *wake_fd)
{
	TestHost   *host = context;
	int			status;

	if (wake_fd == NULL)
		return EINVAL;
	*wake_fd = -1;
	status = validate_generation(host, generation);
	if (status != 0)
		return status;
	*wake_fd = postgamma_wake_target_fd(host->wake_target);
	return *wake_fd >= 0 ? 0 : EIO;
}


static int
control_wake_drain(
	void *context, uint64_t generation, uint64_t *wake_count)
{
	TestHost   *host = context;
	int			status = validate_generation(host, generation);

	return status == 0 ?
		postgamma_wake_target_drain(host->wake_target, wake_count) : status;
}


static int
control_notify(void *context, uint64_t generation)
{
	TestHost   *host = context;
	int			status = validate_generation(host, generation);

	if (status != 0)
		return status;
	host->notify_calls++;
	return postgamma_wake_target_wake(host->wake_target);
}


static int
control_take(
	void *context, uint64_t generation, PostgammaKernelControl *control)
{
	TestHost   *host = context;
	int			status;

	if (control == NULL)
		return EINVAL;
	status = validate_generation(host, generation);
	if (status != 0)
		return status;
	if (!host->control_queued)
		return EAGAIN;
	*control = host->queued_control;
	host->control_queued = false;
	host->take_calls++;
	return 0;
}


static int
control_complete(
	void *context, PostgammaKernelControl *control, int operation_status)
{
	TestHost   *host = context;

	if (control == NULL ||
		validate_generation(host, control->generation) != 0)
		return EINVAL;
	host->completed_control = *control;
	host->operation_status = operation_status;
	host->complete_calls++;
	return 0;
}


static int
fail_host(void *context, uint64_t generation, int failure_status)
{
	TestHost   *host = context;
	int			status = validate_generation(host, generation);

	if (status != 0)
		return status;
	host->failure_status = failure_status;
	host->fail_calls++;
	return 0;
}


static void *
try_bind(void *argument)
{
	BindWorker *worker = argument;

	worker->status = postgamma_postmaster_control_runtime_bind(
		worker->control_runtime);
	return NULL;
}


static PostgammaKernelHostProvider
make_provider(TestHost *host)
{
	PostgammaKernelHostProvider provider =
		POSTGAMMA_KERNEL_HOST_PROVIDER_INIT;

	provider.capabilities =
		POSTGAMMA_KERNEL_HOST_CAP_CONTROL_WAKE_FD |
		POSTGAMMA_KERNEL_HOST_CAP_ASYNC_NOTIFICATION |
		POSTGAMMA_KERNEL_HOST_CAP_FAIL_STOP;
	provider.context = host;
	provider.mark_recovering = mark_recovering;
	provider.mark_ready = mark_ready;
	provider.control_wake_fd = control_wake_fd;
	provider.control_wake_drain = control_wake_drain;
	provider.control_notify = control_notify;
	provider.control_take = control_take;
	provider.control_complete = control_complete;
	provider.fail = fail_host;
	return provider;
}


int
main(void)
{
	PostgammaInstanceRuntimeOptions options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaInstanceRuntime *instance_runtime;
	PostgammaPostmasterControlRuntime *control_runtime;
	PostgammaPostmasterControlRuntime *invalid_runtime;
	PostgammaKernelHostProvider provider;
	PostgammaKernelHostProvider invalid_provider;
	PostgammaKernelControl control;
	PostgammaThreadAttributes attributes = {
		.name = "pgm-bind",
		.role = POSTGAMMA_THREAD_ROLE_GENERAL,
		.stack_size = 0,
		.guard_size = 0,
	};
	PostgammaThread *thread;
	BindWorker worker;
	TestHost host;
	sigset_t	mask;
	sigset_t	old_mask;
	uint64_t	pending_signals;
	uint64_t	wake_count;
	int			wake_fd;
	char		host_numeric_locale[128];
	const char *locale_name;

	memset(&host, 0, sizeof(host));
	host.generation = TEST_GENERATION;
	assert(postgamma_wake_target_create(&host.wake_target) == 0);
	provider = make_provider(&host);
	options.generation = TEST_GENERATION;
	options.profile = POSTGAMMA_RUNTIME_PROFILE_EMBEDDED;
	options.wake_notification = control_notify;
	options.wake_notification_argument = &host;
	assert(postgamma_instance_runtime_create(
		&instance_runtime, &options) == 0);

	invalid_provider = provider;
	invalid_provider.control_take = NULL;
	assert(postgamma_postmaster_control_runtime_create(
		&invalid_runtime, instance_runtime, &invalid_provider) == EINVAL);
	assert(postgamma_postmaster_control_runtime_create(
		&control_runtime, instance_runtime, &provider) == 0);
	assert(!postgamma_postmaster_control_runtime_is_bound());
	assert(postgamma_postmaster_control_current_wake_fd(&wake_fd) == ENOENT);
	assert(wake_fd == -1);
	assert(postgamma_postmaster_control_runtime_bind(control_runtime) == 0);
	assert(postgamma_postmaster_control_runtime_is_bound());
	locale_name = setlocale(LC_NUMERIC, NULL);
	assert(locale_name != NULL);
	assert(strlen(locale_name) < sizeof(host_numeric_locale));
	memcpy(host_numeric_locale, locale_name, strlen(locale_name) + 1);
	locale_name = postgamma_postmaster_control_current_setlocale(
		LC_NUMERIC, NULL);
	assert(locale_name != NULL);
	assert(postgamma_postmaster_control_current_setlocale(
		LC_NUMERIC, "C") != NULL);
	assert(strcmp(setlocale(LC_NUMERIC, NULL), host_numeric_locale) == 0);
	assert(postgamma_postmaster_control_runtime_bind(control_runtime) ==
		EALREADY);
	assert(postgamma_postmaster_control_runtime_destroy(control_runtime) ==
		EBUSY);
	worker.control_runtime = control_runtime;
	worker.status = 0;
	assert(postgamma_thread_create(
		&thread, &attributes, try_bind, &worker) == 0);
	assert(postgamma_thread_join(thread, NULL) == 0);
	assert(postgamma_thread_destroy(thread) == 0);
	assert(worker.status == EBUSY);

	assert(postgamma_postmaster_control_current_wake_fd(&wake_fd) == 0);
	assert(wake_fd == postgamma_wake_target_fd(host.wake_target));
	assert(postgamma_postmaster_control_current_mark_recovering() == 0);
	assert(postgamma_postmaster_control_current_mark_recovering() == 0);
	assert(postgamma_postmaster_control_current_mark_ready() == 0);
	assert(postgamma_postmaster_control_current_mark_ready() == 0);
	assert(host.recovering_calls == 1);
	assert(host.ready_calls == 1);

	assert(postgamma_instance_runtime_notify_completion(instance_runtime) == 0);
	assert(postgamma_instance_runtime_notify_supervisor_signal(
		instance_runtime, TEST_GENERATION, SIGHUP) == 0);
	assert(host.notify_calls == 2);
	assert(postgamma_postmaster_control_current_wake_drain(&wake_count) == 0);
	assert(wake_count == 2);
	assert(postgamma_postmaster_control_current_take_signals(
		&pending_signals) == 0);
	assert(pending_signals == (UINT64_C(1) << (SIGHUP - 1)));
	assert(postgamma_postmaster_control_current_take_signals(
		&pending_signals) == EAGAIN);

	assert(sigemptyset(&mask) == 0);
	assert(sigaddset(&mask, SIGHUP) == 0);
	assert(postgamma_postmaster_control_current_sigprocmask(
		SIG_BLOCK, &mask, &old_mask) == 0);
	assert(sigismember(&old_mask, SIGHUP) == 0);
	assert(postgamma_postmaster_control_current_sigprocmask(
		SIG_SETMASK, NULL, &old_mask) == 0);
	assert(sigismember(&old_mask, SIGHUP) == 1);
	assert(postgamma_postmaster_control_current_sigprocmask(
		-1, &mask, NULL) == EINVAL);

	host.queued_control.generation = TEST_GENERATION;
	host.queued_control.sequence = UINT64_C(7);
	host.queued_control.kind = POSTGAMMA_KERNEL_CONTROL_RELOAD;
	host.control_queued = true;
	assert(postgamma_postmaster_control_current_take(&control) == 0);
	assert(control.sequence == UINT64_C(7));
	assert(postgamma_postmaster_control_current_take(&control) == EAGAIN);
	control = host.queued_control;
	assert(postgamma_postmaster_control_current_complete(
		&control, ENOTSUP) == 0);
	assert(host.take_calls == 1);
	assert(host.complete_calls == 1);
	assert(host.completed_control.sequence == UINT64_C(7));
	assert(host.operation_status == ENOTSUP);
	assert(postgamma_postmaster_control_current_fail(EPROTO) == 0);
	assert(host.fail_calls == 1);
	assert(host.failure_status == EPROTO);

	assert(postgamma_postmaster_control_runtime_unbind(control_runtime) == 0);
	assert(!postgamma_postmaster_control_runtime_is_bound());
	assert(strcmp(setlocale(LC_NUMERIC, NULL), host_numeric_locale) == 0);
	errno = 0;
	assert(postgamma_postmaster_control_current_setlocale(
		LC_NUMERIC, NULL) == NULL);
	assert(errno == ENOENT);
	assert(postgamma_postmaster_control_current_take(&control) == ENOENT);
	assert(postgamma_postmaster_control_runtime_destroy(control_runtime) == 0);
	assert(postgamma_instance_runtime_destroy(instance_runtime) == 0);
	assert(postgamma_wake_target_destroy(host.wake_target) == 0);
	return 0;
}
