#include "postgamma/embedded_kernel.h"
#include "postgamma/private/data_directory_lock.h"
#include "postgamma/private/kernel_supervisor_adapter.h"
#include "postgamma/private/supervisor.h"
#include "postgamma/thread_runtime.h"
#include "tests/embedded/bootstrap/host_process_probe.h"

#include <errno.h>
#include <dirent.h>
#include <execinfo.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#define TEST_TIMEOUT_NS UINT64_C(30000000000)
#define TEST_CONTRACT_SETTLE_TIMEOUT_NS UINT64_C(100000000)
#define TEST_POLL_INTERVAL_NS 1000000L
#define TEST_LIFECYCLE_CYCLES 4
#define TEST_FAULT_CYCLES \
	((int) POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1)
#define TEST_FAULT_GENERATION_BASE UINT64_C(1000)
#define TEST_LOCALE_NAME_CAPACITY 512
#define TEST_ENVIRONMENT_VALUE_CAPACITY 1024


typedef struct EnvironmentValue
{
	bool		present;
	char		value[TEST_ENVIRONMENT_VALUE_CAPACITY];
} EnvironmentValue;


typedef struct HostContractSnapshot
{
	PostgammaBootstrapHostSnapshot resources;
	char		cwd[PATH_MAX];
	char		locale_name[TEST_LOCALE_NAME_CAPACITY];
	EnvironmentValue environment[8];
	sigset_t	signal_mask;
	struct sigaction signal_actions[NSIG];
	bool		signal_action_valid[NSIG];
} HostContractSnapshot;


typedef struct KernelDriver
{
	PostgammaKernelBootOptions options;
	PostgammaKernelHostProvider host;
	PostgammaKernelResult result;
	const PostgammaKernelEntrypoints *entrypoints;
	PostgammaThreadStackInfo supervisor_stack;
	int			supervisor_stack_status;
} KernelDriver;


typedef struct LifecycleCycleResult
{
	PostgammaKernelResult kernel;
	PostgammaSupervisorTelemetry supervisor;
	PostgammaBootstrapHostSnapshot ready_resources;
	uint64_t	open_to_ready_ns;
	PostgammaThreadStackInfo supervisor_stack;
} LifecycleCycleResult;


typedef struct LifecycleCycleSpec
{
	const char *name;
	PostgammaSupervisorShutdownMode shutdown_mode;
} LifecycleCycleSpec;


typedef struct FaultInjection
{
	uint64_t	generation;
	PostgammaKernelFaultPoint target;
	uint64_t	callbacks;
	uint64_t	injections;
} FaultInjection;


typedef struct FaultCycleResult
{
	PostgammaKernelResult kernel;
	PostgammaSupervisorTelemetry supervisor;
	PostgammaThreadStackInfo supervisor_stack;
	uint64_t	callbacks;
	uint64_t	injections;
} FaultCycleResult;


static uint64_t deadline_after(uint64_t interval_ns);
static int run_kernel(PostgammaSupervisor *supervisor, void *argument);
static int run_lifecycle_cycle(
	uint64_t generation, const char *data_directory,
	const char *executable_path, const char *resource_root,
	const PostgammaKernelEntrypoints *entrypoints,
	PostgammaSupervisorShutdownMode shutdown_mode,
	LifecycleCycleResult *cycle_result);
static int run_fault_cycle(
	uint64_t generation, const char *data_directory,
	const char *executable_path, const char *resource_root,
	const PostgammaKernelEntrypoints *entrypoints,
	PostgammaKernelFaultPoint fault_point,
	FaultCycleResult *cycle_result);
static int inject_fault(
	void *context, uint64_t generation, PostgammaKernelFaultPoint point);
static int fail(const char *operation, int status);
static void install_crash_diagnostics(void);
static void crash_diagnostic_handler(int signal_number);
static int capture_host_contract(HostContractSnapshot *snapshot);
static int wait_for_cycle_contract(
	const char *cycle_name, const HostContractSnapshot *before,
	HostContractSnapshot *after, const char *data_directory,
	bool require_mapping_stability);
static bool same_environment(
	const HostContractSnapshot *before,
	const HostContractSnapshot *after);
static bool same_signal_state(
	const HostContractSnapshot *before,
	const HostContractSnapshot *after);
static bool same_signal_set(const sigset_t *left, const sigset_t *right);
static bool same_process_owned_resources(
	const PostgammaBootstrapHostSnapshot *before,
	const PostgammaBootstrapHostSnapshot *after);
static bool cycle_contract_is_restored(
	const HostContractSnapshot *before,
	const HostContractSnapshot *after, const char *data_directory,
	bool require_mapping_stability);
static bool validate_cycle_contract(
	const char *cycle_name, const HostContractSnapshot *before,
	const HostContractSnapshot *after, const char *data_directory,
	bool require_mapping_stability);
static bool pid_file_is_absent(const char *data_directory);
static void dump_open_descriptors(void);
static uint64_t elapsed_ns(uint64_t started_at, uint64_t finished_at);
static uint64_t milliseconds_ceil(uint64_t nanoseconds);
static uint64_t positive_memory_delta(uint64_t after, uint64_t before);
static int positive_resource_delta(int after, int before);
static bool stack_info_is_valid(
	const PostgammaThreadStackInfo *stack_info,
	PostgammaThreadRole expected_role);


int
main(int argument_count, char **arguments)
{
	static const LifecycleCycleSpec cycle_specs[TEST_LIFECYCLE_CYCLES] =
	{
		{"smart", POSTGAMMA_SUPERVISOR_SHUTDOWN_SMART},
		{"fast", POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST},
		{"immediate", POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE},
		{"recovery", POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST}
	};
	HostContractSnapshot snapshots[TEST_LIFECYCLE_CYCLES + 1];
	LifecycleCycleResult cycles[TEST_LIFECYCLE_CYCLES];
	const PostgammaKernelEntrypoints *entrypoints;
	PostgammaKernelGlobalTelemetry kernel_before =
		POSTGAMMA_KERNEL_GLOBAL_TELEMETRY_INIT;
	PostgammaKernelGlobalTelemetry kernel_after =
		POSTGAMMA_KERNEL_GLOBAL_TELEMETRY_INIT;
	int			cold_mapping_growth;
	int			maximum_steady_mapping_growth = 0;
	uint64_t	maximum_rss_delta_kib = 0;
	uint64_t	maximum_virtual_delta_kib = 0;
	uint64_t	maximum_open_to_ready_ns = 0;
	int			maximum_thread_delta = 0;
	int			maximum_descriptor_delta = 0;
	uint64_t	fault_callbacks = 0;
	uint64_t	fault_injections = 0;
	uint64_t	fault_records = 0;
	uint64_t	fault_host_fail_stop_records = 0;
	uint64_t	fault_threads_exited = 0;
	uint64_t	fault_threads_joined = 0;
	uint64_t	fault_process_restart_required = 0;
	int			status;

	install_crash_diagnostics();

	if (argument_count != 4)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT\n",
			arguments[0]);
		return 2;
	}
	status = capture_host_contract(&snapshots[0]);
	if (status != 0)
		return fail("capture host contract", status);
	entrypoints = postgamma_embedded_kernel_entrypoints();
	if (entrypoints == NULL ||
		entrypoints->struct_size != sizeof(*entrypoints) ||
		entrypoints->abi_version !=
		POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION ||
		entrypoints->instance_main == NULL ||
		(entrypoints->capabilities &
		 POSTGAMMA_KERNEL_CAP_SERVER_LIFECYCLE) == 0)
		return fail("validate kernel entrypoints", EPROTO);
	status = postgamma_embedded_kernel_global_telemetry(&kernel_before);
	if (status != 0 || kernel_before.active_instances != 0 ||
		kernel_before.active_memory_contexts != 0)
		return fail("capture initial kernel telemetry",
			status != 0 ? status : EPROTO);
	for (size_t index = 0; index < TEST_LIFECYCLE_CYCLES; index++)
	{
		status = run_lifecycle_cycle(
			(uint64_t) index + 1, arguments[1], arguments[2], arguments[3],
			entrypoints, cycle_specs[index].shutdown_mode, &cycles[index]);
		if (status != 0)
			return fail("embedded lifecycle cycle", status);
		status = wait_for_cycle_contract(
			cycle_specs[index].name,
			&snapshots[index], &snapshots[index + 1], arguments[1],
			index != 0);
		if (status != 0)
			return fail("wait for restored host contract", status);
	}
	cold_mapping_growth = snapshots[1].resources.mappings -
		snapshots[0].resources.mappings;
	for (size_t index = 1; index < TEST_LIFECYCLE_CYCLES; index++)
	{
		int			growth = snapshots[index + 1].resources.mappings -
			snapshots[index].resources.mappings;

		if (growth > maximum_steady_mapping_growth)
			maximum_steady_mapping_growth = growth;
	}
	for (size_t index = 0; index < TEST_LIFECYCLE_CYCLES; index++)
	{
		uint64_t	rss_delta = positive_memory_delta(
			cycles[index].ready_resources.resident_memory_kib,
			snapshots[index].resources.resident_memory_kib);
		uint64_t	virtual_delta = positive_memory_delta(
			cycles[index].ready_resources.virtual_memory_kib,
			snapshots[index].resources.virtual_memory_kib);
		int			thread_delta = positive_resource_delta(
			cycles[index].ready_resources.threads,
			snapshots[index].resources.threads);
		int			descriptor_delta = positive_resource_delta(
			cycles[index].ready_resources.descriptors,
			snapshots[index].resources.descriptors);

		if (!stack_info_is_valid(
				&cycles[index].supervisor_stack,
				POSTGAMMA_THREAD_ROLE_SUPERVISOR))
			return fail("validate supervisor stack bounds", EPROTO);

		if (rss_delta > maximum_rss_delta_kib)
			maximum_rss_delta_kib = rss_delta;
		if (virtual_delta > maximum_virtual_delta_kib)
			maximum_virtual_delta_kib = virtual_delta;
		if (cycles[index].open_to_ready_ns > maximum_open_to_ready_ns)
			maximum_open_to_ready_ns = cycles[index].open_to_ready_ns;
		if (thread_delta > maximum_thread_delta)
			maximum_thread_delta = thread_delta;
		if (descriptor_delta > maximum_descriptor_delta)
			maximum_descriptor_delta = descriptor_delta;
	}
	for (PostgammaKernelFaultPoint point =
			 POSTGAMMA_KERNEL_FAULT_AFTER_ARGUMENTS;
		 point < POSTGAMMA_KERNEL_FAULT_POINT_COUNT;
		 point++)
	{
		HostContractSnapshot before;
		HostContractSnapshot after;
		FaultCycleResult fault;
		uint64_t	generation = TEST_FAULT_GENERATION_BASE +
			(uint64_t) point;

		status = capture_host_contract(&before);
		if (status != 0)
			return fail("capture fault-cycle host contract", status);
		status = run_fault_cycle(
			generation, arguments[1], arguments[2], arguments[3],
			entrypoints, point, &fault);
		if (status != 0)
			return fail("embedded fault-injection cycle", status);
		status = wait_for_cycle_contract(
				postgamma_kernel_fault_point_name(point), &before, &after,
				arguments[1], true);
		if (status != 0)
			return fail("wait for restored fault-cycle host contract", status);
		fault_callbacks += fault.callbacks;
		fault_injections += fault.injections;
		if (fault.supervisor.last_failure.present)
			fault_records++;
		if (fault.supervisor.last_failure.origin ==
			POSTGAMMA_SUPERVISOR_FAILURE_HOST_FAIL_STOP)
			fault_host_fail_stop_records++;
		if (fault.supervisor.last_failure.supervisor_thread_exited)
			fault_threads_exited++;
		if (fault.supervisor.last_failure.supervisor_thread_joined)
			fault_threads_joined++;
		if (fault.supervisor.last_failure.process_restart_required)
			fault_process_restart_required++;
	}
	status = postgamma_embedded_kernel_global_telemetry(&kernel_after);
	if (status != 0 || kernel_after.active_instances != 0 ||
		kernel_after.active_memory_contexts != 0 ||
		kernel_after.instances_entered - kernel_before.instances_entered !=
			TEST_LIFECYCLE_CYCLES + TEST_FAULT_CYCLES ||
		kernel_after.instances_closed - kernel_before.instances_closed !=
			TEST_LIFECYCLE_CYCLES ||
		kernel_after.instances_failed - kernel_before.instances_failed !=
			TEST_FAULT_CYCLES ||
		kernel_after.cleanup_failures != kernel_before.cleanup_failures)
		return fail("validate final kernel telemetry",
			status != 0 ? status : EPROTO);
	printf(
		"POSTGAMMA_KERNEL_LIFECYCLE generation=%d cycles=%d"
		" smart_shutdown=true fast_shutdown=true"
		" immediate_shutdown=true recovery_reopen=true"
		" phase=%s state=%s resources_restored=true"
		" mappings_stable=true cold_mapping_growth=%d"
		" steady_mapping_growth=%d"
		" cwd_restored=true locale_restored=true"
		" environment_restored=true signals_restored=true"
		" pid_file_absent=true role_process_launches=0\n",
		TEST_LIFECYCLE_CYCLES, TEST_LIFECYCLE_CYCLES,
		cycles[TEST_LIFECYCLE_CYCLES - 1].kernel.phase,
		postgamma_supervisor_state_name(
			cycles[TEST_LIFECYCLE_CYCLES - 1].supervisor.state),
		cold_mapping_growth, maximum_steady_mapping_growth);
	printf(
		"POSTGAMMA_KERNEL_FOOTPRINT generation=%d profile=embedded-default"
		" cold_open_to_ready_ms=%" PRIu64
		" warm_open_to_ready_ms=%" PRIu64
		" immediate_open_to_ready_ms=%" PRIu64
		" recovery_open_to_ready_ms=%" PRIu64
		" maximum_open_to_ready_ms=%" PRIu64
		" peak_rss_delta_kib=%" PRIu64
		" peak_virtual_delta_kib=%" PRIu64
		" peak_thread_delta=%d peak_fd_delta=%d\n",
		TEST_LIFECYCLE_CYCLES,
		milliseconds_ceil(cycles[0].open_to_ready_ns),
		milliseconds_ceil(cycles[1].open_to_ready_ns),
		milliseconds_ceil(cycles[2].open_to_ready_ns),
		milliseconds_ceil(cycles[3].open_to_ready_ns),
		milliseconds_ceil(maximum_open_to_ready_ns),
		maximum_rss_delta_kib, maximum_virtual_delta_kib,
		maximum_thread_delta, maximum_descriptor_delta);
	printf(
		"POSTGAMMA_KERNEL_SUPERVISOR_STACK generation=%d"
		" cycles_validated=%d class=supervisor"
		" configured_stack_bytes=%zu configured_guard_bytes=%zu"
		" native_stack_bytes=%zu native_guard_bytes=%zu"
		" usable_stack_bytes=%zu actual_bounds=true\n",
		TEST_LIFECYCLE_CYCLES, TEST_LIFECYCLE_CYCLES,
		cycles[TEST_LIFECYCLE_CYCLES - 1].supervisor_stack.
			configured_stack_size,
		cycles[TEST_LIFECYCLE_CYCLES - 1].supervisor_stack.
			configured_guard_size,
		cycles[TEST_LIFECYCLE_CYCLES - 1].supervisor_stack.native_stack_size,
		cycles[TEST_LIFECYCLE_CYCLES - 1].supervisor_stack.native_guard_size,
		cycles[TEST_LIFECYCLE_CYCLES - 1].supervisor_stack.usable_stack_size);
	printf(
		"POSTGAMMA_KERNEL_FAULT_MATRIX generation=%" PRIu64
		" cycles=%d fault_points=%d callbacks=%" PRIu64
		" injections=%" PRIu64 " cleanup_failures=0"
		" failure_records=%" PRIu64
		" host_fail_stop_records=%" PRIu64
		" process_restart_required=%" PRIu64
		" supervisor_threads_exited=%" PRIu64
		" supervisor_threads_joined=%" PRIu64
		" resources_restored=true mappings_stable=true"
		" fault_status=%d\n",
		TEST_FAULT_GENERATION_BASE + (uint64_t) TEST_FAULT_CYCLES,
		TEST_FAULT_CYCLES, TEST_FAULT_CYCLES,
		fault_callbacks, fault_injections, fault_records,
		fault_host_fail_stop_records, fault_process_restart_required,
		fault_threads_exited, fault_threads_joined, ECANCELED);
	printf(
		"POSTGAMMA_KERNEL_TELEMETRY generation=%" PRIu64
		" instances_entered=%" PRIu64
		" instances_closed=%" PRIu64
		" instances_failed=%" PRIu64
		" cleanup_failures=%" PRIu64
		" active_instances=%" PRIu64
		" active_memory_contexts=%" PRIu64 "\n",
		TEST_FAULT_GENERATION_BASE + (uint64_t) TEST_FAULT_CYCLES,
		kernel_after.instances_entered - kernel_before.instances_entered,
		kernel_after.instances_closed - kernel_before.instances_closed,
		kernel_after.instances_failed - kernel_before.instances_failed,
		kernel_after.cleanup_failures - kernel_before.cleanup_failures,
		kernel_after.active_instances, kernel_after.active_memory_contexts);
	return 0;
}


static int
run_lifecycle_cycle(
	uint64_t generation, const char *data_directory,
	const char *executable_path, const char *resource_root,
	const PostgammaKernelEntrypoints *entrypoints,
	PostgammaSupervisorShutdownMode shutdown_mode,
	LifecycleCycleResult *cycle_result)
{
	PostgammaDataDirectoryLockOptions lock_options =
		POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT;
	PostgammaDataDirectoryLockTelemetry lock_telemetry;
	PostgammaSupervisorOptions supervisor_options =
		POSTGAMMA_SUPERVISOR_OPTIONS_INIT;
	PostgammaDataDirectoryLock *data_lock = NULL;
	PostgammaSupervisor *supervisor = NULL;
	PostgammaSupervisorTicket *shutdown_ticket = NULL;
	KernelDriver driver;
	uint64_t	ready_at;
	uint64_t	started_at;
	int			operation_status = 0;
	int			status;

	if (cycle_result == NULL)
		return EINVAL;
	memset(cycle_result, 0, sizeof(*cycle_result));
	memset(&driver, 0, sizeof(driver));
	driver.options = (PostgammaKernelBootOptions)
		POSTGAMMA_KERNEL_BOOT_OPTIONS_INIT;
	driver.host = (PostgammaKernelHostProvider)
		POSTGAMMA_KERNEL_HOST_PROVIDER_INIT;
	driver.result = (PostgammaKernelResult)
		POSTGAMMA_KERNEL_RESULT_INIT;
	driver.entrypoints = entrypoints;
	lock_options.generation = generation;
	lock_options.path = data_directory;
	status = postgamma_data_directory_lock_acquire(
		&lock_options, &data_lock);
	if (status != 0)
		goto cleanup;
	status = postgamma_data_directory_lock_telemetry(
		data_lock, &lock_telemetry);
	if (status != 0)
		goto cleanup;
	supervisor_options.generation = generation;
	supervisor_options.queue_capacity = 16;
	supervisor_options.callbacks.run = run_kernel;
	supervisor_options.callback_argument = &driver;
	status = postgamma_supervisor_create(
		&supervisor_options, &supervisor);
	if (status != 0)
		goto cleanup;
	status = postgamma_kernel_supervisor_provider_init(
		supervisor, &driver.host);
	if (status != 0)
		goto cleanup;
	driver.options.generation = generation;
	driver.options.data_directory_fd =
		lock_telemetry.directory_descriptor;
	driver.options.logical_umask = 0077;
	driver.options.data_directory = lock_telemetry.canonical_path;
	driver.options.executable_path = executable_path;
	driver.options.resource_root = resource_root;
	driver.options.host = &driver.host;
	started_at = postgamma_monotonic_now_ns();
	status = postgamma_supervisor_start(
		supervisor, deadline_after(TEST_TIMEOUT_NS));
	if (status != 0)
		goto cleanup;
	ready_at = postgamma_monotonic_now_ns();
	cycle_result->open_to_ready_ns = elapsed_ns(started_at, ready_at);
	if (cycle_result->open_to_ready_ns == 0)
	{
		status = EIO;
		goto cleanup;
	}
	status = postgamma_bootstrap_capture_host(&cycle_result->ready_resources);
	if (status != 0)
		goto cleanup;
	status = postgamma_supervisor_request_shutdown(
		supervisor, generation, shutdown_mode,
		&shutdown_ticket);
	if (status != 0)
		goto cleanup;
	status = postgamma_supervisor_ticket_wait(
		shutdown_ticket, deadline_after(TEST_TIMEOUT_NS),
		&operation_status);
	if (status == 0 && operation_status != 0)
		status = operation_status;
	if (status != 0)
		goto cleanup;
	status = postgamma_supervisor_ticket_destroy(shutdown_ticket);
	if (status != 0)
		goto cleanup;
	shutdown_ticket = NULL;
	status = postgamma_supervisor_join(
		supervisor, deadline_after(TEST_TIMEOUT_NS));
	if (status != 0)
		goto cleanup;
	status = postgamma_supervisor_telemetry(
		supervisor, &cycle_result->supervisor);
	if (status != 0)
		goto cleanup;
	cycle_result->kernel = driver.result;
	cycle_result->supervisor_stack = driver.supervisor_stack;
	if (cycle_result->supervisor.state !=
		POSTGAMMA_SUPERVISOR_STATE_CLOSED ||
		cycle_result->supervisor.threads_started != 1 ||
		cycle_result->supervisor.threads_joined != 1 ||
		cycle_result->supervisor.active_tickets != 0 ||
		driver.supervisor_stack_status != 0 ||
		cycle_result->kernel.status != 0 ||
		cycle_result->kernel.postgres_exit_code != 0 ||
		cycle_result->kernel.cleanup_status != 0 ||
		cycle_result->kernel.fault_point != POSTGAMMA_KERNEL_FAULT_NONE)
	{
		status = EPROTO;
		goto cleanup;
	}
	status = postgamma_supervisor_destroy(supervisor);
	if (status != 0)
		goto cleanup;
	supervisor = NULL;
	status = postgamma_data_directory_lock_release(data_lock);
	if (status != 0)
		goto cleanup;
	data_lock = NULL;
	return 0;

cleanup:
	if (status == 0)
		status = EIO;
	if (shutdown_ticket != NULL)
	{
		int destroy_status =
			postgamma_supervisor_ticket_destroy(shutdown_ticket);

		if (destroy_status != 0 && destroy_status != EBUSY)
			status = destroy_status;
	}
	if (supervisor != NULL)
	{
		PostgammaSupervisorState state =
			postgamma_supervisor_state(supervisor);

		if (state == POSTGAMMA_SUPERVISOR_STATE_QUIESCING ||
			state == POSTGAMMA_SUPERVISOR_STATE_STOPPING ||
			state == POSTGAMMA_SUPERVISOR_STATE_FAILED)
			(void) postgamma_supervisor_join(
				supervisor, deadline_after(TEST_TIMEOUT_NS));
		(void) postgamma_supervisor_destroy(supervisor);
	}
	if (data_lock != NULL)
		(void) postgamma_data_directory_lock_release(data_lock);
	if (driver.result.diagnostic[0] != '\0')
		fprintf(stderr, "kernel generation=%" PRIu64 " phase=%s: %s\n",
			generation, driver.result.phase, driver.result.diagnostic);
	return status;
}


static int
run_fault_cycle(
	uint64_t generation, const char *data_directory,
	const char *executable_path, const char *resource_root,
	const PostgammaKernelEntrypoints *entrypoints,
	PostgammaKernelFaultPoint fault_point,
	FaultCycleResult *cycle_result)
{
	PostgammaDataDirectoryLockOptions lock_options =
		POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT;
	PostgammaDataDirectoryLockTelemetry lock_telemetry;
	PostgammaSupervisorOptions supervisor_options =
		POSTGAMMA_SUPERVISOR_OPTIONS_INIT;
	PostgammaKernelFaultProvider faults =
		POSTGAMMA_KERNEL_FAULT_PROVIDER_INIT;
	PostgammaDataDirectoryLock *data_lock = NULL;
	PostgammaSupervisor *supervisor = NULL;
	FaultInjection injection;
	KernelDriver driver;
	int			status;
	int			start_status;

	if (cycle_result == NULL ||
		fault_point <= POSTGAMMA_KERNEL_FAULT_NONE ||
		fault_point >= POSTGAMMA_KERNEL_FAULT_POINT_COUNT)
		return EINVAL;
	memset(cycle_result, 0, sizeof(*cycle_result));
	memset(&driver, 0, sizeof(driver));
	memset(&injection, 0, sizeof(injection));
	driver.options = (PostgammaKernelBootOptions)
		POSTGAMMA_KERNEL_BOOT_OPTIONS_INIT;
	driver.host = (PostgammaKernelHostProvider)
		POSTGAMMA_KERNEL_HOST_PROVIDER_INIT;
	driver.result = (PostgammaKernelResult)
		POSTGAMMA_KERNEL_RESULT_INIT;
	driver.entrypoints = entrypoints;
	injection.generation = generation;
	injection.target = fault_point;
	faults.context = &injection;
	faults.check = inject_fault;
	lock_options.generation = generation;
	lock_options.path = data_directory;
	status = postgamma_data_directory_lock_acquire(
		&lock_options, &data_lock);
	if (status != 0)
		goto cleanup;
	status = postgamma_data_directory_lock_telemetry(
		data_lock, &lock_telemetry);
	if (status != 0)
		goto cleanup;
	supervisor_options.generation = generation;
	supervisor_options.queue_capacity = 16;
	supervisor_options.callbacks.run = run_kernel;
	supervisor_options.callback_argument = &driver;
	status = postgamma_supervisor_create(
		&supervisor_options, &supervisor);
	if (status != 0)
		goto cleanup;
	status = postgamma_kernel_supervisor_provider_init(
		supervisor, &driver.host);
	if (status != 0)
		goto cleanup;
	driver.options.generation = generation;
	driver.options.data_directory_fd =
		lock_telemetry.directory_descriptor;
	driver.options.logical_umask = 0077;
	driver.options.data_directory = lock_telemetry.canonical_path;
	driver.options.executable_path = executable_path;
	driver.options.resource_root = resource_root;
	driver.options.host = &driver.host;
	driver.options.faults = &faults;
	start_status = postgamma_supervisor_start(
		supervisor, deadline_after(TEST_TIMEOUT_NS));
	if (start_status != ECANCELED)
	{
		status = start_status != 0 ? start_status : EPROTO;
		goto cleanup;
	}
	status = postgamma_supervisor_join(
		supervisor, deadline_after(TEST_TIMEOUT_NS));
	if (status != 0)
		goto cleanup;
	status = postgamma_supervisor_telemetry(
		supervisor, &cycle_result->supervisor);
	if (status != 0)
		goto cleanup;
	cycle_result->kernel = driver.result;
	cycle_result->supervisor_stack = driver.supervisor_stack;
	cycle_result->callbacks = injection.callbacks;
	cycle_result->injections = injection.injections;
	if (injection.callbacks != (uint64_t) fault_point ||
		injection.injections != 1 ||
		driver.supervisor_stack_status != 0 ||
		!stack_info_is_valid(
			&cycle_result->supervisor_stack,
			POSTGAMMA_THREAD_ROLE_SUPERVISOR) ||
		cycle_result->supervisor.state !=
			POSTGAMMA_SUPERVISOR_STATE_FAILED ||
		cycle_result->supervisor.threads_started != 1 ||
		cycle_result->supervisor.threads_joined != 1 ||
		cycle_result->supervisor.active_tickets != 0 ||
		cycle_result->supervisor.failure_attempts == 0 ||
		cycle_result->supervisor.failure_status != ECANCELED ||
		!cycle_result->supervisor.last_failure.present ||
		cycle_result->supervisor.last_failure.generation != generation ||
		cycle_result->supervisor.last_failure.ordinal != 1 ||
		cycle_result->supervisor.last_failure.recorded_at_ns == 0 ||
		cycle_result->supervisor.last_failure.origin !=
			POSTGAMMA_SUPERVISOR_FAILURE_HOST_FAIL_STOP ||
		cycle_result->supervisor.last_failure.control_kind !=
			POSTGAMMA_SUPERVISOR_CONTROL_NONE ||
		cycle_result->supervisor.last_failure.control_sequence != 0 ||
		cycle_result->supervisor.last_failure.status != ECANCELED ||
		cycle_result->supervisor.last_failure.process_restart_required ||
		!cycle_result->supervisor.last_failure.supervisor_thread_exited ||
		!cycle_result->supervisor.last_failure.supervisor_thread_joined ||
		cycle_result->kernel.status != ECANCELED ||
		cycle_result->kernel.postgres_exit_code != 0 ||
		cycle_result->kernel.cleanup_status != 0 ||
		cycle_result->kernel.fault_point != fault_point ||
		strcmp(cycle_result->kernel.phase, "fault-injection") != 0 ||
		strstr(cycle_result->kernel.diagnostic,
			postgamma_kernel_fault_point_name(fault_point)) == NULL)
	{
		status = EPROTO;
		goto cleanup;
	}
	status = postgamma_supervisor_destroy(supervisor);
	if (status != 0)
		goto cleanup;
	supervisor = NULL;
	status = postgamma_data_directory_lock_release(data_lock);
	if (status != 0)
		goto cleanup;
	data_lock = NULL;
	return 0;

cleanup:
	if (status == 0)
		status = EIO;
	if (supervisor != NULL)
	{
		PostgammaSupervisorState state =
			postgamma_supervisor_state(supervisor);

		if (state != POSTGAMMA_SUPERVISOR_STATE_NEW &&
			state != POSTGAMMA_SUPERVISOR_STATE_CLOSED)
			(void) postgamma_supervisor_join(
				supervisor, deadline_after(TEST_TIMEOUT_NS));
		(void) postgamma_supervisor_destroy(supervisor);
	}
	if (data_lock != NULL)
		(void) postgamma_data_directory_lock_release(data_lock);
	fprintf(stderr,
		"fault generation=%" PRIu64 " point=%s callbacks=%" PRIu64
		" injections=%" PRIu64 " kernel_status=%d cleanup_status=%d"
		" phase=%s diagnostic=%s\n",
		generation, postgamma_kernel_fault_point_name(fault_point),
		injection.callbacks, injection.injections, driver.result.status,
		driver.result.cleanup_status, driver.result.phase,
		driver.result.diagnostic);
	return status;
}


static int
inject_fault(
	void *context, uint64_t generation, PostgammaKernelFaultPoint point)
{
	FaultInjection *injection = context;

	if (injection == NULL || generation != injection->generation ||
		point <= POSTGAMMA_KERNEL_FAULT_NONE ||
		point >= POSTGAMMA_KERNEL_FAULT_POINT_COUNT ||
		injection->callbacks == UINT64_MAX)
		return EPROTO;
	injection->callbacks++;
	if (point != injection->target)
		return 0;
	if (injection->injections != 0)
		return EPROTO;
	injection->injections++;
	return ECANCELED;
}


static void
install_crash_diagnostics(void)
{
	struct sigaction action;
	int			signals[] = {SIGABRT, SIGBUS, SIGSEGV};

	memset(&action, 0, sizeof(action));
	action.sa_handler = crash_diagnostic_handler;
	sigemptyset(&action.sa_mask);
	for (size_t index = 0; index < sizeof(signals) / sizeof(signals[0]); index++)
		(void) sigaction(signals[index], &action, NULL);
}


static void
crash_diagnostic_handler(int signal_number)
{
	void   *frames[64];
	int		frame_count;

	frame_count = backtrace(frames, sizeof(frames) / sizeof(frames[0]));
	backtrace_symbols_fd(frames, frame_count, STDERR_FILENO);
	_exit(128 + signal_number);
}


static int
capture_host_contract(HostContractSnapshot *snapshot)
{
	static const char *const environment_names[] =
	{
		"LC_ALL",
		"LC_COLLATE",
		"LC_CTYPE",
		"LC_MESSAGES",
		"LC_MONETARY",
		"LC_NUMERIC",
		"LC_TIME",
		"LANG"
	};
	const char *locale_name;
	int			status;

	if (snapshot == NULL)
		return EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	status = postgamma_bootstrap_capture_host(&snapshot->resources);
	if (status != 0)
		return status;
	if (getcwd(snapshot->cwd, sizeof(snapshot->cwd)) == NULL)
		return errno != 0 ? errno : EIO;
	locale_name = setlocale(LC_ALL, NULL);
	if (locale_name == NULL)
		return errno != 0 ? errno : EIO;
	if (strlen(locale_name) >= sizeof(snapshot->locale_name))
		return ENAMETOOLONG;
	memcpy(snapshot->locale_name, locale_name, strlen(locale_name) + 1);
	for (size_t index = 0;
		 index < sizeof(environment_names) / sizeof(environment_names[0]);
		 index++)
	{
		const char *value = getenv(environment_names[index]);

		if (value == NULL)
			continue;
		if (strlen(value) >= sizeof(snapshot->environment[index].value))
			return ENAMETOOLONG;
		snapshot->environment[index].present = true;
		memcpy(snapshot->environment[index].value,
			value, strlen(value) + 1);
	}
	status = pthread_sigmask(SIG_SETMASK, NULL, &snapshot->signal_mask);
	if (status != 0)
		return status;
	for (int signal_number = 1; signal_number < NSIG; signal_number++)
	{
		if (sigaction(signal_number, NULL,
				&snapshot->signal_actions[signal_number]) == 0)
			snapshot->signal_action_valid[signal_number] = true;
		else if (errno != EINVAL)
			return errno != 0 ? errno : EIO;
	}
	return 0;
}


static int
wait_for_cycle_contract(
	const char *cycle_name, const HostContractSnapshot *before,
	HostContractSnapshot *after, const char *data_directory,
	bool require_mapping_stability)
{
	struct timespec pause = {0, TEST_POLL_INTERVAL_NS};
	uint64_t	deadline = deadline_after(TEST_CONTRACT_SETTLE_TIMEOUT_NS);
	int			status;

	if (cycle_name == NULL || before == NULL || after == NULL ||
		data_directory == NULL)
		return EINVAL;
	if (deadline == POSTGAMMA_SUPERVISOR_NO_DEADLINE)
		return EIO;
	for (;;)
	{
		uint64_t	now;

		status = capture_host_contract(after);
		if (status != 0)
			return status;
		if (cycle_contract_is_restored(
				before, after, data_directory, require_mapping_stability))
			return 0;
		now = postgamma_monotonic_now_ns();
		if (now == 0)
			return EIO;
		if (now >= deadline)
		{
			(void) validate_cycle_contract(
				cycle_name, before, after, data_directory,
				require_mapping_stability);
			return ETIMEDOUT;
		}
		(void) nanosleep(&pause, NULL);
	}
}


static bool
same_environment(
	const HostContractSnapshot *before, const HostContractSnapshot *after)
{
	for (size_t index = 0;
		 index < sizeof(before->environment) / sizeof(before->environment[0]);
		 index++)
	{
		if (before->environment[index].present !=
			after->environment[index].present)
			return false;
		if (before->environment[index].present &&
			strcmp(before->environment[index].value,
				after->environment[index].value) != 0)
			return false;
	}
	return true;
}


static bool
same_signal_state(
	const HostContractSnapshot *before, const HostContractSnapshot *after)
{
	if (!same_signal_set(&before->signal_mask, &after->signal_mask))
		return false;
	for (int signal_number = 1; signal_number < NSIG; signal_number++)
	{
		const struct sigaction *left = &before->signal_actions[signal_number];
		const struct sigaction *right = &after->signal_actions[signal_number];

		if (before->signal_action_valid[signal_number] !=
			after->signal_action_valid[signal_number])
			return false;
		if (!before->signal_action_valid[signal_number])
			continue;
		if (left->sa_flags != right->sa_flags ||
			left->sa_handler != right->sa_handler ||
			!same_signal_set(&left->sa_mask, &right->sa_mask))
			return false;
	}
	return true;
}


static bool
same_signal_set(const sigset_t *left, const sigset_t *right)
{
	for (int signal_number = 1; signal_number < NSIG; signal_number++)
	{
		if (sigismember(left, signal_number) !=
			sigismember(right, signal_number))
			return false;
	}
	return true;
}


static bool
same_process_owned_resources(
	const PostgammaBootstrapHostSnapshot *before,
	const PostgammaBootstrapHostSnapshot *after)
{
	return before != NULL && after != NULL &&
		before->pid == after->pid &&
		before->descriptors == after->descriptors &&
		before->threads == after->threads &&
		before->children == after->children &&
		before->sysv_mappings == after->sysv_mappings &&
		before->umask_value == after->umask_value;
}


static bool
cycle_contract_is_restored(
	const HostContractSnapshot *before,
	const HostContractSnapshot *after, const char *data_directory,
	bool require_mapping_stability)
{
	if (before == NULL || after == NULL || data_directory == NULL)
		return false;
	return same_process_owned_resources(
			&before->resources, &after->resources) &&
		(!require_mapping_stability ||
		 before->resources.mappings == after->resources.mappings) &&
		strcmp(before->cwd, after->cwd) == 0 &&
		strcmp(before->locale_name, after->locale_name) == 0 &&
		same_environment(before, after) &&
		same_signal_state(before, after) &&
		pid_file_is_absent(data_directory);
}


static bool
validate_cycle_contract(
	const char *cycle_name, const HostContractSnapshot *before,
	const HostContractSnapshot *after, const char *data_directory,
	bool require_mapping_stability)
{
	bool		resources_restored = same_process_owned_resources(
		&before->resources, &after->resources);
	bool		mappings_stable = before->resources.mappings ==
		after->resources.mappings;
	bool		cwd_restored = strcmp(before->cwd, after->cwd) == 0;
	bool		locale_restored = strcmp(
		before->locale_name, after->locale_name) == 0;
	bool		environment_restored = same_environment(before, after);
	bool		signals_restored = same_signal_state(before, after);
	bool		pid_file_absent = pid_file_is_absent(data_directory);

	if (cycle_contract_is_restored(
			before, after, data_directory, require_mapping_stability))
		return true;
	fprintf(stderr,
		"host contract cycle=%s resources=%s mappings=%s cwd=%s locale=%s"
		" environment=%s signals=%s pid_file=%s\n",
		cycle_name, resources_restored ? "restored" : "changed",
		mappings_stable ? "stable" : "changed",
		cwd_restored ? "restored" : "changed",
		locale_restored ? "restored" : "changed",
		environment_restored ? "restored" : "changed",
		signals_restored ? "restored" : "changed",
		pid_file_absent ? "absent" : "present");
	fprintf(stderr,
		"host resources before={pid=%ld fd=%d threads=%d children=%d"
		" mappings=%d sysv=%d umask=%03o}"
		" after={pid=%ld fd=%d threads=%d children=%d"
		" mappings=%d sysv=%d umask=%03o}\n",
		(long) before->resources.pid, before->resources.descriptors,
		before->resources.threads, before->resources.children,
		before->resources.mappings, before->resources.sysv_mappings,
		(unsigned int) before->resources.umask_value,
		(long) after->resources.pid, after->resources.descriptors,
		after->resources.threads, after->resources.children,
		after->resources.mappings, after->resources.sysv_mappings,
		(unsigned int) after->resources.umask_value);
	fprintf(stderr, "host locale before=%s after=%s\n",
		before->locale_name, after->locale_name);
	dump_open_descriptors();
	return false;
}


static bool
pid_file_is_absent(const char *data_directory)
{
	char		path[PATH_MAX];
	struct stat status_buffer;
	int			length;

	length = snprintf(
		path, sizeof(path), "%s/postmaster.pid", data_directory);
	if (length < 0 || length >= (int) sizeof(path))
		return false;
	errno = 0;
	return lstat(path, &status_buffer) != 0 && errno == ENOENT;
}


static void
dump_open_descriptors(void)
{
	DIR		   *directory;
	struct dirent *entry;
	int			directory_descriptor;

	directory = opendir("/proc/self/fd");
	if (directory == NULL)
		return;
	directory_descriptor = dirfd(directory);
	while ((entry = readdir(directory)) != NULL)
	{
		char		path[64];
		char		target[PATH_MAX];
		char	   *end = NULL;
		long		descriptor;
		ssize_t		length;

		errno = 0;
		descriptor = strtol(entry->d_name, &end, 10);
		if (errno != 0 || end == entry->d_name || *end != '\0' ||
			descriptor == directory_descriptor)
			continue;
		if (snprintf(path, sizeof(path), "/proc/self/fd/%ld", descriptor) >=
			(int) sizeof(path))
			continue;
		length = readlink(path, target, sizeof(target) - 1);
		if (length < 0)
			continue;
		target[length] = '\0';
		fprintf(stderr, "host fd=%ld target=%s\n", descriptor, target);
	}
	(void) closedir(directory);
}


static uint64_t
deadline_after(uint64_t interval_ns)
{
	uint64_t	now = postgamma_monotonic_now_ns();

	if (now == 0 || now > UINT64_MAX - interval_ns)
		return POSTGAMMA_SUPERVISOR_NO_DEADLINE;
	return now + interval_ns;
}


static uint64_t
elapsed_ns(uint64_t started_at, uint64_t finished_at)
{
	return started_at != 0 && finished_at >= started_at ?
		finished_at - started_at : 0;
}


static uint64_t
milliseconds_ceil(uint64_t nanoseconds)
{
	return nanoseconds / UINT64_C(1000000) +
		(nanoseconds % UINT64_C(1000000) != 0 ? 1 : 0);
}


static uint64_t
positive_memory_delta(uint64_t after, uint64_t before)
{
	return after > before ? after - before : 0;
}


static int
positive_resource_delta(int after, int before)
{
	return after > before ? after - before : 0;
}


static int
run_kernel(PostgammaSupervisor *supervisor, void *argument)
{
	KernelDriver *driver = argument;

	(void) supervisor;
	driver->supervisor_stack_status = postgamma_thread_current_stack_info(
		&driver->supervisor_stack);
	if (driver->supervisor_stack_status != 0)
		return driver->supervisor_stack_status;
	return driver->entrypoints->instance_main(
		&driver->options, &driver->result);
}


static bool
stack_info_is_valid(
	const PostgammaThreadStackInfo *stack_info,
	PostgammaThreadRole expected_role)
{
	size_t		expected_stack_size;
	size_t		expected_guard_size;

	if (postgamma_thread_role_stack_policy(
			expected_role, &expected_stack_size,
			&expected_guard_size) != 0)
		return false;
	return stack_info != NULL && stack_info->role == expected_role &&
		stack_info->configured_stack_size == expected_stack_size &&
		stack_info->configured_guard_size == expected_guard_size &&
		stack_info->configured_stack_size >
			stack_info->configured_guard_size &&
		stack_info->native_stack_size >=
			stack_info->configured_stack_size &&
		stack_info->native_guard_size >=
			stack_info->configured_guard_size &&
		stack_info->usable_stack_size ==
			stack_info->native_stack_size - stack_info->native_guard_size &&
		stack_info->stack_high_address > stack_info->stack_low_address &&
		stack_info->stack_high_address - stack_info->stack_low_address ==
			stack_info->native_stack_size;
}


static int
fail(const char *operation, int status)
{
	fprintf(stderr, "%s failed: %s (%d)\n",
		operation, strerror(status), status);
	return 1;
}
