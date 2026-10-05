#include "postgamma/embedded_kernel.h"
#include "postgamma/private/data_directory_lock.h"
#include "postgamma/private/kernel_supervisor_adapter.h"
#include "postgamma/private/libpq_memory_adapter.h"
#include "postgamma/private/memory_transport.h"
#include "postgamma/private/server_transport_adapter.h"
#include "postgamma/private/supervisor.h"
#include "postgamma/thread_runtime.h"
#include "tests/embedded/kernel/process_contract_probe.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#ifndef POSTGAMMA_KERNEL_SOAK_MINIMUM_ITERATIONS
#define POSTGAMMA_KERNEL_SOAK_MINIMUM_ITERATIONS UINT64_C(1000)
#endif
#define TEST_MINIMUM_ITERATIONS POSTGAMMA_KERNEL_SOAK_MINIMUM_ITERATIONS
#define TEST_MAXIMUM_ITERATIONS UINT64_C(1000000)
#define TEST_QUEUE_CAPACITY (64U * 1024U)
#define TEST_TIMEOUT_NS UINT64_C(30000000000)
#define TEST_POLL_INTERVAL_NS 1000000L
#define TEST_WARM_GENERATION_BASE UINT64_C(630000000)
#define TEST_MEASURED_GENERATION_BASE UINT64_C(640000000)


typedef struct KernelDriver
{
	PostgammaKernelBootOptions options;
	PostgammaKernelHostProvider host;
	PostgammaKernelResult result;
	const PostgammaKernelEntrypoints *entrypoints;
	PostgammaThreadStackInfo supervisor_stack;
	int			supervisor_stack_status;
} KernelDriver;


typedef struct FaultInjection
{
	uint64_t	generation;
	PostgammaKernelFaultPoint target;
	uint64_t	callbacks;
	uint64_t	injections;
} FaultInjection;


typedef struct SuccessCycleResult
{
	uint64_t	network_calls;
	uint64_t	secure_reads;
	uint64_t	secure_writes;
	int			backend_pid;
} SuccessCycleResult;


typedef struct FaultCycleResult
{
	uint64_t	callbacks;
	uint64_t	injections;
	bool		failure_recorded;
} FaultCycleResult;


static uint64_t deadline_after(uint64_t interval_ns);
static bool libpq_deadline(int64_t *deadline);
static int parse_uint64(
	const char *text, uint64_t minimum, uint64_t maximum, uint64_t *value);
static uint64_t next_random(uint64_t *state);
static void prepare_fault_order(
	uint64_t *random_state,
	PostgammaKernelFaultPoint fault_order[POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1]);
static int run_kernel(PostgammaSupervisor *supervisor, void *argument);
static int cancel_not_supported(
	void *argument, uint64_t connection_generation,
	uint64_t request_generation, int backend_pid);
static int inject_fault(
	void *context, uint64_t generation, PostgammaKernelFaultPoint point);
static int run_success_cycle(
	uint64_t generation, const char *data_directory,
	const char *executable_path, const char *resource_root,
	const PostgammaKernelEntrypoints *entrypoints,
	PostgammaSupervisorShutdownMode shutdown_mode,
	SuccessCycleResult *cycle_result);
static int run_fault_cycle(
	uint64_t generation, const char *data_directory,
	const char *executable_path, const char *resource_root,
	const PostgammaKernelEntrypoints *entrypoints,
	PostgammaKernelFaultPoint fault_point,
	FaultCycleResult *cycle_result);
static int wait_and_destroy_ticket(
	PostgammaSupervisorTicket **ticket, int *operation_status);
static int stop_supervisor(
	PostgammaSupervisor *supervisor, uint64_t generation,
	PostgammaSupervisorShutdownMode shutdown_mode);
static int wait_for_memory_baseline(
	const PostgammaMemoryGlobalTelemetry *baseline);
static bool memory_telemetry_equal(
	const PostgammaMemoryGlobalTelemetry *left,
	const PostgammaMemoryGlobalTelemetry *right);
static bool query_result_is_one(const PostgammaPrivateQueryResult *result);
static bool stack_info_is_valid(
	const PostgammaThreadStackInfo *stack_info,
	PostgammaThreadRole expected_role);
static void update_resource_peaks(
	const PostgammaKernelProcessContractSnapshot *snapshot,
	int *mapping_peak,
	uint64_t *virtual_memory_peak_kib,
	uint64_t *resident_memory_peak_kib,
	uint64_t *allocator_live_peak_bytes);
static bool steady_resource_drift_is_bounded(
	int first_mapping_peak,
	int second_mapping_peak,
	uint64_t first_virtual_peak_kib,
	uint64_t second_virtual_peak_kib,
	uint64_t first_resident_peak_kib,
	uint64_t second_resident_peak_kib,
	bool allocator_metrics_supported,
	uint64_t first_allocator_live_peak_bytes,
	uint64_t second_allocator_live_peak_bytes);
static int fail_cycle(
	const char *kind, uint64_t iteration, uint64_t seed, int status);


int
main(int argument_count, char **arguments)
{
	PostgammaKernelProcessContractSnapshot baseline;
	PostgammaKernelProcessContractSnapshot current;
	PostgammaKernelGlobalTelemetry kernel_after =
		POSTGAMMA_KERNEL_GLOBAL_TELEMETRY_INIT;
	PostgammaKernelFaultPoint
		fault_order[POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1];
	const PostgammaKernelEntrypoints *entrypoints;
	uint64_t	iterations;
	uint64_t	seed;
	uint64_t	random_state;
	uint64_t	started_at;
	uint64_t	finished_at;
	uint64_t	data_a_cycles = 0;
	uint64_t	data_b_cycles = 0;
	uint64_t	fast_shutdowns = 0;
	uint64_t	immediate_shutdowns = 0;
	uint64_t	recovery_queries = 0;
	uint64_t	fault_callbacks = 0;
	uint64_t	fault_injections = 0;
	uint64_t	failure_records = 0;
	uint64_t	fault_coverage = 0;
	uint64_t	network_calls = 0;
	uint64_t	secure_reads = 0;
	uint64_t	secure_writes = 0;
	uint64_t	virtual_memory_peak_kib;
	uint64_t	resident_memory_peak_kib;
	uint64_t	first_virtual_peak_kib;
	uint64_t	second_virtual_peak_kib;
	uint64_t	first_resident_peak_kib;
	uint64_t	second_resident_peak_kib;
	uint64_t	allocator_live_peak_bytes;
	uint64_t	first_allocator_live_peak_bytes;
	uint64_t	second_allocator_live_peak_bytes;
	int			mapping_peak;
	int			first_mapping_peak;
	int			second_mapping_peak;
	int			status;

	if (argument_count != 7)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY_A DATA_DIRECTORY_B EXECUTABLE_PATH"
			" RESOURCE_ROOT ITERATIONS SEED\n",
			arguments[0]);
		return 2;
	}
	status = parse_uint64(
		arguments[5], TEST_MINIMUM_ITERATIONS, TEST_MAXIMUM_ITERATIONS,
		&iterations);
	if (status == 0)
		status = parse_uint64(arguments[6], UINT64_C(1), UINT64_MAX, &seed);
	if (status != 0)
		return fail_cycle("parse arguments", 0, 0, status);
	entrypoints = postgamma_embedded_kernel_entrypoints();
	if (entrypoints == NULL ||
		entrypoints->struct_size != sizeof(*entrypoints) ||
		entrypoints->abi_version != POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION ||
		entrypoints->instance_main == NULL ||
		(entrypoints->capabilities &
		 POSTGAMMA_KERNEL_CAP_SERVER_LIFECYCLE) == 0 ||
		(entrypoints->capabilities & POSTGAMMA_KERNEL_CAP_MEMORY_PROTOCOL) == 0)
		return fail_cycle("validate kernel entrypoints", 0, seed, EPROTO);

	random_state = seed;
	prepare_fault_order(&random_state, fault_order);
	fprintf(stderr,
		"PostGamma soak warmup: two data directories, recovery, query, and %d"
		" fault points\n",
		(int) POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1);
	{
		SuccessCycleResult warm_success;

		status = run_success_cycle(
			TEST_WARM_GENERATION_BASE + UINT64_C(1), arguments[1],
			arguments[3], arguments[4], entrypoints,
			POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE, &warm_success);
		if (status == 0)
			status = run_success_cycle(
				TEST_WARM_GENERATION_BASE + UINT64_C(2), arguments[1],
				arguments[3], arguments[4], entrypoints,
				POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST, &warm_success);
		if (status == 0)
			status = run_success_cycle(
				TEST_WARM_GENERATION_BASE + UINT64_C(3), arguments[2],
				arguments[3], arguments[4], entrypoints,
				POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST, &warm_success);
		if (status != 0)
			return fail_cycle("warm successful lifecycle", 0, seed, status);
	}
	for (PostgammaKernelFaultPoint point =
			 POSTGAMMA_KERNEL_FAULT_AFTER_ARGUMENTS;
		 point < POSTGAMMA_KERNEL_FAULT_POINT_COUNT;
		 point++)
	{
		FaultCycleResult warm_fault;

		status = run_fault_cycle(
			TEST_WARM_GENERATION_BASE + UINT64_C(100) + (uint64_t) point,
			arguments[1], arguments[3], arguments[4], entrypoints,
			point, &warm_fault);
		if (status != 0)
			return fail_cycle("warm fault lifecycle", 0, seed, status);
	}
	status = postgamma_kernel_process_contract_capture(&baseline);
	if (status != 0 ||
		!postgamma_kernel_process_contract_restored(
			&baseline, &baseline, arguments[1], arguments[2], "baseline", true))
		return fail_cycle(
			"capture clean baseline", 0, seed, status != 0 ? status : EPROTO);
	mapping_peak = baseline.host.mappings;
	virtual_memory_peak_kib = baseline.host.virtual_memory_kib;
	resident_memory_peak_kib = baseline.host.resident_memory_kib;
	first_mapping_peak = baseline.host.mappings;
	second_mapping_peak = baseline.host.mappings;
	first_virtual_peak_kib = baseline.host.virtual_memory_kib;
	second_virtual_peak_kib = baseline.host.virtual_memory_kib;
	first_resident_peak_kib = baseline.host.resident_memory_kib;
	second_resident_peak_kib = baseline.host.resident_memory_kib;
	allocator_live_peak_bytes = baseline.allocator_live_bytes;
	first_allocator_live_peak_bytes = baseline.allocator_live_bytes;
	second_allocator_live_peak_bytes = baseline.allocator_live_bytes;

	started_at = postgamma_monotonic_now_ns();
	for (uint64_t iteration = 0; iteration < iterations; iteration++)
	{
		const char *data_directory = iteration % UINT64_C(3) == UINT64_C(2) ?
			arguments[2] : arguments[1];
		PostgammaKernelFaultPoint fault_point;
		PostgammaSupervisorShutdownMode shutdown_mode;
		SuccessCycleResult success;
		FaultCycleResult fault;
		uint64_t	generation = TEST_MEASURED_GENERATION_BASE +
			iteration * UINT64_C(2);
		char		label[64];

		if (iteration < (uint64_t) (POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1))
			fault_point = fault_order[iteration];
		else
			fault_point = (PostgammaKernelFaultPoint) (
				UINT64_C(1) + next_random(&random_state) %
					(uint64_t) (POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1));
		status = run_fault_cycle(
			generation, data_directory, arguments[3], arguments[4],
			entrypoints, fault_point, &fault);
		if (status != 0)
			return fail_cycle("fault lifecycle", iteration, seed, status);
		fault_callbacks += fault.callbacks;
		fault_injections += fault.injections;
		failure_records += fault.failure_recorded ? UINT64_C(1) : UINT64_C(0);
		fault_coverage |= UINT64_C(1) << ((unsigned int) fault_point - 1U);
		(void) snprintf(
			label, sizeof(label), "fault-%" PRIu64 "-%s", iteration,
			postgamma_kernel_fault_point_name(fault_point));
		status = postgamma_kernel_process_contract_wait(
			&baseline, &current, arguments[1], arguments[2], label,
			deadline_after(TEST_TIMEOUT_NS));
		if (status != 0)
			return fail_cycle("fault quiescence", iteration, seed, status);
		if (!postgamma_kernel_process_contract_resources_bounded(
				&baseline, &current, label))
			return fail_cycle("fault cleanup contract", iteration, seed, EPROTO);
		update_resource_peaks(
			&current, &mapping_peak, &virtual_memory_peak_kib,
			&resident_memory_peak_kib, &allocator_live_peak_bytes);
		if (iteration < iterations / UINT64_C(2))
			update_resource_peaks(
				&current, &first_mapping_peak, &first_virtual_peak_kib,
				&first_resident_peak_kib,
				&first_allocator_live_peak_bytes);
		else
			update_resource_peaks(
				&current, &second_mapping_peak, &second_virtual_peak_kib,
				&second_resident_peak_kib,
				&second_allocator_live_peak_bytes);

		shutdown_mode =
			iteration % UINT64_C(3) == 0 && iteration + UINT64_C(1) < iterations ?
			POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE :
			POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST;
		status = run_success_cycle(
			generation + UINT64_C(1), data_directory, arguments[3], arguments[4],
			entrypoints, shutdown_mode, &success);
		if (status != 0)
			return fail_cycle("query lifecycle", iteration, seed, status);
		if (iteration % UINT64_C(3) != UINT64_C(2))
			data_a_cycles++;
		else
			data_b_cycles++;
		if (shutdown_mode == POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE)
			immediate_shutdowns++;
		else
			fast_shutdowns++;
		if (iteration % UINT64_C(3) == UINT64_C(1))
			recovery_queries++;
		network_calls += success.network_calls;
		secure_reads += success.secure_reads;
		secure_writes += success.secure_writes;
		(void) snprintf(label, sizeof(label), "query-%" PRIu64, iteration);
		status = postgamma_kernel_process_contract_wait(
			&baseline, &current, arguments[1], arguments[2], label,
			deadline_after(TEST_TIMEOUT_NS));
		if (status != 0)
			return fail_cycle("query quiescence", iteration, seed, status);
		if (!postgamma_kernel_process_contract_resources_bounded(
				&baseline, &current, label))
			return fail_cycle("query cleanup contract", iteration, seed, EPROTO);
		update_resource_peaks(
			&current, &mapping_peak, &virtual_memory_peak_kib,
			&resident_memory_peak_kib, &allocator_live_peak_bytes);
		if (iteration < iterations / UINT64_C(2))
			update_resource_peaks(
				&current, &first_mapping_peak, &first_virtual_peak_kib,
				&first_resident_peak_kib,
				&first_allocator_live_peak_bytes);
		else
			update_resource_peaks(
				&current, &second_mapping_peak, &second_virtual_peak_kib,
				&second_resident_peak_kib,
				&second_allocator_live_peak_bytes);
		if ((iteration + UINT64_C(1)) % UINT64_C(100) == 0 ||
			iteration + UINT64_C(1) == iterations)
			fprintf(stderr,
				"PostGamma soak progress: completed=%" PRIu64 "/%" PRIu64
				" seed=%" PRIu64 "\n",
				iteration + UINT64_C(1), iterations, seed);
	}
	finished_at = postgamma_monotonic_now_ns();
	status = postgamma_embedded_kernel_global_telemetry(&kernel_after);
	if (status != 0 || kernel_after.active_instances != 0 ||
		kernel_after.active_memory_contexts != 0 ||
		kernel_after.instances_entered - baseline.kernel.instances_entered !=
			iterations * UINT64_C(2) ||
		kernel_after.instances_closed - baseline.kernel.instances_closed !=
			iterations ||
		kernel_after.instances_failed - baseline.kernel.instances_failed !=
			iterations ||
		kernel_after.cleanup_failures != baseline.kernel.cleanup_failures ||
		postgamma_data_directory_lock_active_count() != 0 ||
		fault_injections != iterations || failure_records != iterations ||
		fault_coverage !=
			(UINT64_C(1) << (POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1)) -
				UINT64_C(1) ||
		network_calls != 0 || secure_reads < iterations ||
		secure_writes < iterations ||
		!steady_resource_drift_is_bounded(
			first_mapping_peak, second_mapping_peak,
			first_virtual_peak_kib, second_virtual_peak_kib,
			first_resident_peak_kib, second_resident_peak_kib,
			baseline.allocator_metrics_supported,
			first_allocator_live_peak_bytes,
			second_allocator_live_peak_bytes))
		return fail_cycle(
			"validate final soak telemetry", iterations, seed,
			status != 0 ? status : EPROTO);

	printf(
		"POSTGAMMA_KERNEL_SOAK seed=%" PRIu64 " iterations=%" PRIu64
		" lifecycle_cycles=%" PRIu64 " query_cycles=%" PRIu64
		" fault_cycles=%" PRIu64 " data_a_cycles=%" PRIu64
		" data_b_cycles=%" PRIu64 " fast_shutdowns=%" PRIu64
		" immediate_shutdowns=%" PRIu64 " recovery_queries=%" PRIu64
		" fault_points=%d fault_coverage=0x%" PRIx64
		" fault_callbacks=%" PRIu64 " fault_injections=%" PRIu64
		" failure_records=%" PRIu64 " cleanup_failures=0"
		" network_calls=%" PRIu64 " secure_reads=%" PRIu64
		" secure_writes=%" PRIu64
		" process_checks=%" PRIu64 " mapping_checks=%" PRIu64
		" memory_checks=%" PRIu64 " active_transports=%" PRIu64
		" endpoint_references=%" PRIu64 " active_locks=%zu"
		" active_instances=%" PRIu64 " active_memory_contexts=%" PRIu64
		" mapping_baseline=%d mapping_peak=%d mapping_final=%d"
		" mapping_delta_budget=%d mapping_first_half_peak=%d"
		" mapping_second_half_peak=%d mapping_steady_drift_budget=%d"
		" virtual_kib_baseline=%" PRIu64 " virtual_kib_peak=%" PRIu64
		" virtual_kib_final=%" PRIu64 " virtual_kib_delta_budget=%" PRIu64
		" virtual_kib_first_half_peak=%" PRIu64
		" virtual_kib_second_half_peak=%" PRIu64
		" virtual_kib_steady_drift_budget=%" PRIu64
		" resident_kib_baseline=%" PRIu64 " resident_kib_peak=%" PRIu64
		" resident_kib_final=%" PRIu64 " resident_kib_delta_budget=%" PRIu64
		" resident_kib_first_half_peak=%" PRIu64
		" resident_kib_second_half_peak=%" PRIu64
		" resident_kib_steady_drift_budget=%" PRIu64
		" allocator_metrics_supported=%s"
		" allocator_live_baseline=%" PRIu64
		" allocator_live_peak=%" PRIu64
		" allocator_live_final=%" PRIu64
		" allocator_live_delta_budget=%" PRIu64
		" allocator_live_first_half_peak=%" PRIu64
		" allocator_live_second_half_peak=%" PRIu64
		" allocator_live_steady_drift_budget=%" PRIu64
		" resources_restored=true mappings_bounded=true"
		" allocator_live_bounded=true steady_state_bounded=true"
		" process_globals_restored=true"
		" pid_files_absent=true"
		" duration_ms=%" PRIu64 " host_pid=%ld\n",
		seed, iterations, iterations, iterations, iterations,
		data_a_cycles, data_b_cycles, fast_shutdowns, immediate_shutdowns,
		recovery_queries, (int) POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1,
		fault_coverage, fault_callbacks, fault_injections, failure_records,
		network_calls, secure_reads, secure_writes,
		iterations * UINT64_C(2), iterations * UINT64_C(2),
		iterations * UINT64_C(2), current.memory.active_transports,
		current.memory.endpoint_references,
		postgamma_data_directory_lock_active_count(),
		kernel_after.active_instances, kernel_after.active_memory_contexts,
		baseline.host.mappings, mapping_peak, current.host.mappings,
		POSTGAMMA_KERNEL_MAPPING_DELTA_BUDGET,
		first_mapping_peak, second_mapping_peak,
		POSTGAMMA_KERNEL_MAPPING_STEADY_DRIFT_BUDGET,
		baseline.host.virtual_memory_kib, virtual_memory_peak_kib,
		current.host.virtual_memory_kib,
		POSTGAMMA_KERNEL_VIRTUAL_DELTA_BUDGET_KIB,
		first_virtual_peak_kib, second_virtual_peak_kib,
		POSTGAMMA_KERNEL_VIRTUAL_STEADY_DRIFT_BUDGET_KIB,
		baseline.host.resident_memory_kib, resident_memory_peak_kib,
		current.host.resident_memory_kib,
		POSTGAMMA_KERNEL_RESIDENT_DELTA_BUDGET_KIB,
		first_resident_peak_kib, second_resident_peak_kib,
		POSTGAMMA_KERNEL_RESIDENT_STEADY_DRIFT_BUDGET_KIB,
		baseline.allocator_metrics_supported ? "true" : "false",
		baseline.allocator_live_bytes, allocator_live_peak_bytes,
		current.allocator_live_bytes,
		POSTGAMMA_KERNEL_ALLOCATOR_LIVE_DELTA_BUDGET_BYTES,
		first_allocator_live_peak_bytes,
		second_allocator_live_peak_bytes,
		POSTGAMMA_KERNEL_ALLOCATOR_LIVE_STEADY_DRIFT_BUDGET_BYTES,
		finished_at >= started_at ?
			(finished_at - started_at) / UINT64_C(1000000) : UINT64_C(0),
		(long) getpid());
	return 0;
}


static uint64_t
deadline_after(uint64_t interval_ns)
{
	uint64_t	now = postgamma_monotonic_now_ns();

	if (now == 0 || now > UINT64_MAX - interval_ns)
		return POSTGAMMA_SUPERVISOR_NO_DEADLINE;
	return now + interval_ns;
}


static bool
libpq_deadline(int64_t *deadline)
{
	int64_t		now;

	if (deadline == NULL ||
		postgamma_memory_clock_now(&now) != POSTGAMMA_MEMORY_STATUS_OK ||
		now > INT64_MAX - (int64_t) TEST_TIMEOUT_NS)
		return false;
	*deadline = now + (int64_t) TEST_TIMEOUT_NS;
	return true;
}


static int
parse_uint64(
	const char *text, uint64_t minimum, uint64_t maximum, uint64_t *value)
{
	char	   *end = NULL;
	uint64_t	parsed;

	if (text == NULL || value == NULL || text[0] == '\0')
		return EINVAL;
	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
		parsed < minimum || parsed > maximum)
		return EINVAL;
	*value = parsed;
	return 0;
}


static uint64_t
next_random(uint64_t *state)
{
	uint64_t	value = *state;

	value ^= value << 13;
	value ^= value >> 7;
	value ^= value << 17;
	*state = value;
	return value;
}


static void
prepare_fault_order(
	uint64_t *random_state,
	PostgammaKernelFaultPoint fault_order[POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1])
{
	for (size_t index = 0;
		 index < (size_t) POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1;
		 index++)
		fault_order[index] = (PostgammaKernelFaultPoint) (index + 1);
	for (size_t remaining =
			 (size_t) POSTGAMMA_KERNEL_FAULT_POINT_COUNT - 1;
		 remaining > 1;
		 remaining--)
	{
		size_t		selected = (size_t) (next_random(random_state) % remaining);
		PostgammaKernelFaultPoint temporary = fault_order[remaining - 1];

		fault_order[remaining - 1] = fault_order[selected];
		fault_order[selected] = temporary;
	}
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


static int
cancel_not_supported(
	void *argument, uint64_t connection_generation,
	uint64_t request_generation, int backend_pid)
{
	(void) argument;
	(void) connection_generation;
	(void) request_generation;
	(void) backend_pid;
	return ENOTSUP;
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


static int
run_success_cycle(
	uint64_t generation, const char *data_directory,
	const char *executable_path, const char *resource_root,
	const PostgammaKernelEntrypoints *entrypoints,
	PostgammaSupervisorShutdownMode shutdown_mode,
	SuccessCycleResult *cycle_result)
{
	PostgammaMemoryGlobalTelemetry memory_before;
	PostgammaMemoryGlobalTelemetry memory_after;
	PostgammaDataDirectoryLockOptions lock_options =
		POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT;
	PostgammaDataDirectoryLockTelemetry lock_telemetry;
	PostgammaSupervisorOptions supervisor_options =
		POSTGAMMA_SUPERVISOR_OPTIONS_INIT;
	const PostgammaKernelSetting instance_settings[] =
	{
		{"max_connections", "5"},
	};
	PostgammaKernelConnectRequest connect_request =
		POSTGAMMA_KERNEL_CONNECT_REQUEST_INIT;
	PostgammaPrivateLibpqOptions libpq_options;
	PostgammaPrivateLibpqTelemetry libpq_telemetry;
	PostgammaPrivateQueryResult query_result;
	PostgammaDataDirectoryLock *data_lock = NULL;
	PostgammaSupervisor *supervisor = NULL;
	PostgammaSupervisorTicket *connect_ticket = NULL;
	PostgammaPrivateLibpqConnection *connection = NULL;
	PostgammaMemoryEndpoint *backend_endpoint = NULL;
	PostgammaSupervisorTelemetry supervisor_telemetry;
	PostgammaPrivateLibpqStatus libpq_status;
	KernelDriver driver;
	int64_t		deadline;
	int			operation_status = 0;
	int			status = 0;

	if (cycle_result == NULL ||
		(shutdown_mode != POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST &&
		 shutdown_mode != POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE))
		return EINVAL;
	memset(cycle_result, 0, sizeof(*cycle_result));
	memset(&driver, 0, sizeof(driver));
	memset(&lock_telemetry, 0, sizeof(lock_telemetry));
	memset(&libpq_options, 0, sizeof(libpq_options));
	memset(&libpq_telemetry, 0, sizeof(libpq_telemetry));
	memset(&query_result, 0, sizeof(query_result));
	memset(&supervisor_telemetry, 0, sizeof(supervisor_telemetry));
	postgamma_memory_global_telemetry(&memory_before);
	driver.options = (PostgammaKernelBootOptions)
		POSTGAMMA_KERNEL_BOOT_OPTIONS_INIT;
	driver.host = (PostgammaKernelHostProvider)
		POSTGAMMA_KERNEL_HOST_PROVIDER_INIT;
	driver.result = (PostgammaKernelResult) POSTGAMMA_KERNEL_RESULT_INIT;
	driver.entrypoints = entrypoints;
	lock_options.generation = generation;
	lock_options.path = data_directory;
	status = postgamma_data_directory_lock_acquire(&lock_options, &data_lock);
	if (status != 0)
		goto cleanup;
	status = postgamma_data_directory_lock_telemetry(data_lock, &lock_telemetry);
	if (status != 0)
		goto cleanup;
	supervisor_options.generation = generation;
	supervisor_options.queue_capacity = 16;
	supervisor_options.callbacks.run = run_kernel;
	supervisor_options.callback_argument = &driver;
	status = postgamma_supervisor_create(&supervisor_options, &supervisor);
	if (status != 0)
		goto cleanup;
	status = postgamma_kernel_supervisor_provider_init(supervisor, &driver.host);
	if (status != 0)
		goto cleanup;
	driver.options.generation = generation;
	driver.options.data_directory_fd = lock_telemetry.directory_descriptor;
	driver.options.logical_umask = 0077;
	driver.options.data_directory = lock_telemetry.canonical_path;
	driver.options.executable_path = executable_path;
	driver.options.resource_root = resource_root;
	driver.options.settings = instance_settings;
	driver.options.setting_count =
		sizeof(instance_settings) / sizeof(instance_settings[0]);
	driver.options.host = &driver.host;
	status = postgamma_supervisor_start(
		supervisor, deadline_after(TEST_TIMEOUT_NS));
	if (status != 0)
		goto cleanup;

	libpq_options = (PostgammaPrivateLibpqOptions) {
		.generation = generation,
		.queue_capacity = TEST_QUEUE_CAPACITY,
		.user = "postgamma",
		.database = "postgres",
		.application_name = "postgamma-kernel-soak",
		.cancel_callback = cancel_not_supported,
		.cancel_argument = NULL,
	};
	libpq_status = postgamma_private_libpq_create(
		&libpq_options, &connection, &backend_endpoint);
	if (libpq_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		status = EPROTO;
		goto cleanup;
	}
	status = postgamma_memory_server_connect_init(
		generation, backend_endpoint, &connect_request);
	if (status != 0)
		goto cleanup;
	connect_request.connection_id = generation;
	status = postgamma_supervisor_submit(
		supervisor, generation, POSTGAMMA_SUPERVISOR_CONTROL_CONNECT,
		&connect_request, &connect_ticket);
	if (status != 0)
		goto cleanup;
	status = wait_and_destroy_ticket(&connect_ticket, &operation_status);
	if (status == 0 && operation_status != 0)
		status = operation_status;
	if (status != 0 || connect_request.backend_pid <= 0)
	{
		if (status == 0)
			status = EPROTO;
		goto cleanup;
	}
	if (postgamma_memory_endpoint_release(&backend_endpoint, generation) !=
		POSTGAMMA_MEMORY_STATUS_OK)
	{
		status = EPROTO;
		goto cleanup;
	}
	connect_request.transport = NULL;
	if (!libpq_deadline(&deadline))
	{
		status = EOVERFLOW;
		goto cleanup;
	}
	libpq_status = postgamma_private_libpq_connect(connection, deadline);
	if (libpq_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		status = EPROTO;
		goto cleanup;
	}
	if (!libpq_deadline(&deadline) ||
		postgamma_private_libpq_query(
			connection, "SELECT 1", deadline, &query_result) !=
			POSTGAMMA_PRIVATE_LIBPQ_OK ||
		!query_result_is_one(&query_result))
	{
		status = EPROTO;
		goto cleanup;
	}
	libpq_status = postgamma_private_libpq_telemetry(
		connection, &libpq_telemetry);
	if (libpq_status != POSTGAMMA_PRIVATE_LIBPQ_OK ||
		libpq_telemetry.backend_pid != connect_request.backend_pid ||
		libpq_telemetry.secure_read_calls == 0 ||
		libpq_telemetry.secure_write_calls == 0 ||
		libpq_telemetry.network_connect_calls != 0 ||
		libpq_telemetry.optional_security_calls != 0)
	{
		status = EPROTO;
		goto cleanup;
	}
	libpq_status = postgamma_private_libpq_close(&connection);
	if (libpq_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		status = EPROTO;
		goto cleanup;
	}
	status = wait_for_memory_baseline(&memory_before);
	if (status != 0)
		goto cleanup;
	status = stop_supervisor(supervisor, generation, shutdown_mode);
	if (status != 0)
		goto cleanup;
	status = postgamma_supervisor_telemetry(supervisor, &supervisor_telemetry);
	if (status != 0)
		goto cleanup;
	if (supervisor_telemetry.state != POSTGAMMA_SUPERVISOR_STATE_CLOSED ||
		supervisor_telemetry.threads_started != 1 ||
		supervisor_telemetry.threads_joined != 1 ||
		supervisor_telemetry.active_tickets != 0 ||
		driver.supervisor_stack_status != 0 ||
		!stack_info_is_valid(
			&driver.supervisor_stack, POSTGAMMA_THREAD_ROLE_SUPERVISOR) ||
		driver.result.status != 0 || driver.result.postgres_exit_code != 0 ||
		driver.result.cleanup_status != 0 ||
		driver.result.fault_point != POSTGAMMA_KERNEL_FAULT_NONE)
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
	postgamma_memory_global_telemetry(&memory_after);
	if (!memory_telemetry_equal(&memory_before, &memory_after) ||
		postgamma_data_directory_lock_active_count() != 0)
		return EPROTO;
	cycle_result->network_calls = libpq_telemetry.network_connect_calls;
	cycle_result->secure_reads = libpq_telemetry.secure_read_calls;
	cycle_result->secure_writes = libpq_telemetry.secure_write_calls;
	cycle_result->backend_pid = libpq_telemetry.backend_pid;
	return 0;

cleanup:
	if (status == 0)
		status = EIO;
	if (connect_ticket != NULL)
	{
		int destroy_status = postgamma_supervisor_ticket_destroy(connect_ticket);

		if (destroy_status != 0 && destroy_status != EBUSY)
			status = destroy_status;
	}
	if (connection != NULL)
		(void) postgamma_private_libpq_close(&connection);
	if (backend_endpoint != NULL)
	{
		(void) postgamma_memory_endpoint_abort(
			backend_endpoint, generation, POSTGAMMA_MEMORY_ABORT_SHUTDOWN);
		(void) postgamma_memory_endpoint_release(&backend_endpoint, generation);
	}
	if (supervisor != NULL)
	{
		PostgammaSupervisorState state = postgamma_supervisor_state(supervisor);

		if (state == POSTGAMMA_SUPERVISOR_STATE_READY)
			(void) stop_supervisor(
				supervisor, generation, POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST);
		else if (state != POSTGAMMA_SUPERVISOR_STATE_NEW &&
			state != POSTGAMMA_SUPERVISOR_STATE_CLOSED)
		{
			(void) postgamma_supervisor_fail(supervisor, status);
			(void) postgamma_supervisor_join(
				supervisor, deadline_after(TEST_TIMEOUT_NS));
		}
		(void) postgamma_supervisor_destroy(supervisor);
	}
	if (data_lock != NULL)
		(void) postgamma_data_directory_lock_release(data_lock);
	fprintf(stderr,
		"successful cycle generation=%" PRIu64 " kernel_phase=%s"
		" diagnostic=%s status=%d\n",
		generation, driver.result.phase, driver.result.diagnostic, status);
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
	PostgammaKernelFaultProvider faults = POSTGAMMA_KERNEL_FAULT_PROVIDER_INIT;
	PostgammaDataDirectoryLock *data_lock = NULL;
	PostgammaSupervisor *supervisor = NULL;
	PostgammaSupervisorTelemetry supervisor_telemetry;
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
	memset(&lock_telemetry, 0, sizeof(lock_telemetry));
	memset(&supervisor_telemetry, 0, sizeof(supervisor_telemetry));
	driver.options = (PostgammaKernelBootOptions)
		POSTGAMMA_KERNEL_BOOT_OPTIONS_INIT;
	driver.host = (PostgammaKernelHostProvider)
		POSTGAMMA_KERNEL_HOST_PROVIDER_INIT;
	driver.result = (PostgammaKernelResult) POSTGAMMA_KERNEL_RESULT_INIT;
	driver.entrypoints = entrypoints;
	injection.generation = generation;
	injection.target = fault_point;
	faults.context = &injection;
	faults.check = inject_fault;
	lock_options.generation = generation;
	lock_options.path = data_directory;
	status = postgamma_data_directory_lock_acquire(&lock_options, &data_lock);
	if (status != 0)
		goto cleanup;
	status = postgamma_data_directory_lock_telemetry(data_lock, &lock_telemetry);
	if (status != 0)
		goto cleanup;
	supervisor_options.generation = generation;
	supervisor_options.queue_capacity = 16;
	supervisor_options.callbacks.run = run_kernel;
	supervisor_options.callback_argument = &driver;
	status = postgamma_supervisor_create(&supervisor_options, &supervisor);
	if (status != 0)
		goto cleanup;
	status = postgamma_kernel_supervisor_provider_init(supervisor, &driver.host);
	if (status != 0)
		goto cleanup;
	driver.options.generation = generation;
	driver.options.data_directory_fd = lock_telemetry.directory_descriptor;
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
	status = postgamma_supervisor_telemetry(supervisor, &supervisor_telemetry);
	if (status != 0)
		goto cleanup;
	if (injection.callbacks != (uint64_t) fault_point ||
		injection.injections != 1 ||
		driver.supervisor_stack_status != 0 ||
		!stack_info_is_valid(
			&driver.supervisor_stack, POSTGAMMA_THREAD_ROLE_SUPERVISOR) ||
		supervisor_telemetry.state != POSTGAMMA_SUPERVISOR_STATE_FAILED ||
		supervisor_telemetry.threads_started != 1 ||
		supervisor_telemetry.threads_joined != 1 ||
		supervisor_telemetry.active_tickets != 0 ||
		!supervisor_telemetry.last_failure.present ||
		supervisor_telemetry.last_failure.generation != generation ||
		supervisor_telemetry.last_failure.origin !=
			POSTGAMMA_SUPERVISOR_FAILURE_HOST_FAIL_STOP ||
		supervisor_telemetry.last_failure.status != ECANCELED ||
		supervisor_telemetry.last_failure.process_restart_required ||
		!supervisor_telemetry.last_failure.supervisor_thread_exited ||
		!supervisor_telemetry.last_failure.supervisor_thread_joined ||
		driver.result.status != ECANCELED ||
		driver.result.cleanup_status != 0 ||
		driver.result.fault_point != fault_point ||
		strcmp(driver.result.phase, "fault-injection") != 0)
	{
		status = EPROTO;
		goto cleanup;
	}
	cycle_result->callbacks = injection.callbacks;
	cycle_result->injections = injection.injections;
	cycle_result->failure_recorded = true;
	status = postgamma_supervisor_destroy(supervisor);
	if (status != 0)
		goto cleanup;
	supervisor = NULL;
	status = postgamma_data_directory_lock_release(data_lock);
	if (status != 0)
		goto cleanup;
	data_lock = NULL;
	return postgamma_data_directory_lock_active_count() == 0 ? 0 : EPROTO;

cleanup:
	if (status == 0)
		status = EIO;
	if (supervisor != NULL)
	{
		PostgammaSupervisorState state = postgamma_supervisor_state(supervisor);

		if (state != POSTGAMMA_SUPERVISOR_STATE_NEW &&
			state != POSTGAMMA_SUPERVISOR_STATE_CLOSED)
			(void) postgamma_supervisor_join(
				supervisor, deadline_after(TEST_TIMEOUT_NS));
		(void) postgamma_supervisor_destroy(supervisor);
	}
	if (data_lock != NULL)
		(void) postgamma_data_directory_lock_release(data_lock);
	fprintf(stderr,
		"fault cycle generation=%" PRIu64 " point=%s callbacks=%" PRIu64
		" injections=%" PRIu64 " kernel_status=%d cleanup_status=%d"
		" diagnostic=%s status=%d\n",
		generation, postgamma_kernel_fault_point_name(fault_point),
		injection.callbacks, injection.injections, driver.result.status,
		driver.result.cleanup_status, driver.result.diagnostic, status);
	return status;
}


static int
wait_and_destroy_ticket(
	PostgammaSupervisorTicket **ticket, int *operation_status)
{
	int			status;

	if (ticket == NULL || *ticket == NULL || operation_status == NULL)
		return EINVAL;
	status = postgamma_supervisor_ticket_wait(
		*ticket, deadline_after(TEST_TIMEOUT_NS), operation_status);
	if (status != 0)
		return status;
	status = postgamma_supervisor_ticket_destroy(*ticket);
	if (status == 0)
		*ticket = NULL;
	return status;
}


static int
stop_supervisor(
	PostgammaSupervisor *supervisor, uint64_t generation,
	PostgammaSupervisorShutdownMode shutdown_mode)
{
	PostgammaSupervisorTicket *shutdown_ticket = NULL;
	int			operation_status = 0;
	int			status;

	status = postgamma_supervisor_request_shutdown(
		supervisor, generation, shutdown_mode, &shutdown_ticket);
	if (status == 0)
		status = wait_and_destroy_ticket(&shutdown_ticket, &operation_status);
	if (status == 0 && operation_status != 0)
		status = operation_status;
	if (status == 0)
		status = postgamma_supervisor_join(
			supervisor, deadline_after(TEST_TIMEOUT_NS));
	if (shutdown_ticket != NULL)
	{
		int destroy_status = postgamma_supervisor_ticket_destroy(shutdown_ticket);

		if (status == 0 && destroy_status != 0 && destroy_status != EBUSY)
			status = destroy_status;
	}
	return status;
}


static int
wait_for_memory_baseline(const PostgammaMemoryGlobalTelemetry *baseline)
{
	struct timespec pause = {0, TEST_POLL_INTERVAL_NS};
	uint64_t	deadline = deadline_after(TEST_TIMEOUT_NS);

	for (;;)
	{
		PostgammaMemoryGlobalTelemetry current;

		postgamma_memory_global_telemetry(&current);
		if (memory_telemetry_equal(baseline, &current))
			return 0;
		if (deadline != POSTGAMMA_SUPERVISOR_NO_DEADLINE &&
			postgamma_monotonic_now_ns() >= deadline)
			return ETIMEDOUT;
		(void) nanosleep(&pause, NULL);
	}
}


static bool
memory_telemetry_equal(
	const PostgammaMemoryGlobalTelemetry *left,
	const PostgammaMemoryGlobalTelemetry *right)
{
	return left != NULL && right != NULL &&
		left->active_transports == right->active_transports &&
		left->endpoint_references == right->endpoint_references &&
		left->allocated_bytes == right->allocated_bytes;
}


static bool
query_result_is_one(const PostgammaPrivateQueryResult *result)
{
	return result != NULL &&
		result->status == POSTGAMMA_PRIVATE_RESULT_TUPLES_OK &&
		result->rows == 1 && result->columns == 1 &&
		strcmp(result->value, "1") == 0 &&
		strcmp(result->command_status, "SELECT 1") == 0 &&
		result->transaction_status == 0;
}


static bool
stack_info_is_valid(
	const PostgammaThreadStackInfo *stack_info,
	PostgammaThreadRole expected_role)
{
	size_t		expected_stack_size;
	size_t		expected_guard_size;

	if (postgamma_thread_role_stack_policy(
			expected_role, &expected_stack_size, &expected_guard_size) != 0)
		return false;
	return stack_info != NULL && stack_info->role == expected_role &&
		stack_info->configured_stack_size == expected_stack_size &&
		stack_info->configured_guard_size == expected_guard_size &&
		stack_info->configured_stack_size > stack_info->configured_guard_size &&
		stack_info->native_stack_size >= stack_info->configured_stack_size &&
		stack_info->native_guard_size >= stack_info->configured_guard_size &&
		stack_info->usable_stack_size ==
			stack_info->native_stack_size - stack_info->native_guard_size &&
		stack_info->stack_high_address > stack_info->stack_low_address &&
		stack_info->stack_high_address - stack_info->stack_low_address ==
			stack_info->native_stack_size;
}


static void
update_resource_peaks(
	const PostgammaKernelProcessContractSnapshot *snapshot,
	int *mapping_peak,
	uint64_t *virtual_memory_peak_kib,
	uint64_t *resident_memory_peak_kib,
	uint64_t *allocator_live_peak_bytes)
{
	if (snapshot->host.mappings > *mapping_peak)
		*mapping_peak = snapshot->host.mappings;
	if (snapshot->host.virtual_memory_kib > *virtual_memory_peak_kib)
		*virtual_memory_peak_kib = snapshot->host.virtual_memory_kib;
	if (snapshot->host.resident_memory_kib > *resident_memory_peak_kib)
		*resident_memory_peak_kib = snapshot->host.resident_memory_kib;
	if (snapshot->allocator_metrics_supported &&
		snapshot->allocator_live_bytes > *allocator_live_peak_bytes)
		*allocator_live_peak_bytes = snapshot->allocator_live_bytes;
}


static bool
steady_resource_drift_is_bounded(
	int first_mapping_peak,
	int second_mapping_peak,
	uint64_t first_virtual_peak_kib,
	uint64_t second_virtual_peak_kib,
	uint64_t first_resident_peak_kib,
	uint64_t second_resident_peak_kib,
	bool allocator_metrics_supported,
	uint64_t first_allocator_live_peak_bytes,
	uint64_t second_allocator_live_peak_bytes)
{
	return (second_mapping_peak <= first_mapping_peak ||
			second_mapping_peak - first_mapping_peak <=
				POSTGAMMA_KERNEL_MAPPING_STEADY_DRIFT_BUDGET) &&
		(second_virtual_peak_kib <= first_virtual_peak_kib ||
		 second_virtual_peak_kib - first_virtual_peak_kib <=
			POSTGAMMA_KERNEL_VIRTUAL_STEADY_DRIFT_BUDGET_KIB) &&
		(second_resident_peak_kib <= first_resident_peak_kib ||
		 second_resident_peak_kib - first_resident_peak_kib <=
			POSTGAMMA_KERNEL_RESIDENT_STEADY_DRIFT_BUDGET_KIB) &&
		(!allocator_metrics_supported ||
		 second_allocator_live_peak_bytes <=
			first_allocator_live_peak_bytes ||
		 second_allocator_live_peak_bytes -
			first_allocator_live_peak_bytes <=
			POSTGAMMA_KERNEL_ALLOCATOR_LIVE_STEADY_DRIFT_BUDGET_BYTES);
}


static int
fail_cycle(const char *kind, uint64_t iteration, uint64_t seed, int status)
{
	fprintf(stderr,
		"%s failed: iteration=%" PRIu64 " seed=%" PRIu64 " error=%s (%d)"
		" replay_iterations=%" PRIu64 " replay_seed=%" PRIu64 "\n",
		kind, iteration, seed, strerror(status), status,
		iteration + UINT64_C(1), seed);
	return 1;
}
