#include "postgamma/private/bootstrap_probe.h"
#include "tests/embedded/bootstrap/host_process_probe.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef pgm_bootstrap_private_libpq_result (*PostgammaBootstrapPrivateLibpqProbe)(void);

int
main(int argc, char **argv)
{
	PostgammaBootstrapPrivateLibpqProbe probe = NULL;
	pgm_bootstrap_private_libpq_result result;
	PostgammaBootstrapHostSnapshot baseline;
	PostgammaBootstrapHostSnapshot after;
	void       *handle;
	void       *symbol;
	char       *end = NULL;
	unsigned long parsed_iterations = 1;
	unsigned int iterations;
	unsigned int completed_iterations = 0;
	unsigned int total_checks = 0;
	unsigned int warmup_checks;
	unsigned int index;
	bool resources_restored;

	memset(&result, 0, sizeof(result));
	if (argc != 2 && argc != 3)
		return 2;
	if (argc == 3)
	{
		errno = 0;
		parsed_iterations = strtoul(argv[2], &end, 10);
		if (errno != 0 || end == argv[2] || *end != '\0' ||
			parsed_iterations == 0 || parsed_iterations > 1000 ||
			parsed_iterations > UINT_MAX)
			return 2;
	}
	iterations = (unsigned int) parsed_iterations;
	handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL)
	{
		fprintf(stderr, "cannot load bootstrap library: %s\n", dlerror());
		return 3;
	}
	symbol = dlsym(handle, "pgm_bootstrap_probe_private_libpq");
	if (symbol == NULL || sizeof(symbol) != sizeof(probe))
	{
		fprintf(stderr, "cannot resolve private libpq probe: %s\n", dlerror());
		(void) dlclose(handle);
		return 4;
	}
	memcpy(&probe, &symbol, sizeof(probe));
	result = probe();
	warmup_checks = result.checks;
	if (result.status != PGM_BOOTSTRAP_PROBE_PASS)
	{
		fprintf(stderr, "private libpq warmup failed: %s\n", result.detail);
		(void) dlclose(handle);
		return 7;
	}
	if (postgamma_bootstrap_capture_host(&baseline) != 0)
		return 6;
	for (index = 0; index < iterations; index++)
	{
		result = probe();
		total_checks += result.checks;
		if (result.status != PGM_BOOTSTRAP_PROBE_PASS)
			break;
		completed_iterations++;
	}
	if (postgamma_bootstrap_capture_host(&after) != 0)
		return 6;
	resources_restored = postgamma_bootstrap_same_host_resources(&baseline, &after);
	if (!resources_restored)
		fprintf(
			stderr,
			"host resource mismatch: descriptors %d/%d, threads %d/%d, "
			"children %d/%d, mappings %d/%d, sysv mappings %d/%d\n",
			baseline.descriptors, after.descriptors,
			baseline.threads, after.threads,
			baseline.children, after.children,
			baseline.mappings, after.mappings,
			baseline.sysv_mappings, after.sysv_mappings);
	printf(
		"{\"status\":%d,\"warmup_status\":%d,\"warmup_checks\":%u,"
		"\"iterations\":%u,\"completed_iterations\":%u,"
		"\"checks\":%u,\"total_checks\":%u,\"queue_capacity\":%u,"
		"\"startup_packets\":%u,\"query_packets\":%u,"
		"\"copy_packets\":%u,\"copy_bytes_before_response\":%u,"
		"\"copy_bytes_received\":%u,"
		"\"simultaneous_queue_saturation\":%u,"
		"\"secure_read_calls\":%u,\"secure_write_calls\":%u,"
		"\"socket_wait_calls\":%u,\"notice_count\":%u,"
		"\"cancel_dispatches\":%u,\"network_connect_calls\":%u,"
		"\"optional_security_calls\":%u,\"backend_pid\":%u,"
		"\"connection_generation\":%llu,\"request_generation\":%llu,"
		"\"active_transports_after_close\":%llu,"
		"\"endpoint_references_after_close\":%llu,"
		"\"allocated_bytes_after_close\":%llu,\"host_pid\":%ld,"
		"\"host_pid_unchanged\":%s,\"resources_restored\":%s,"
		"\"baseline_descriptors\":%d,\"baseline_threads\":%d,"
		"\"baseline_children\":%d,\"baseline_mappings\":%d,"
		"\"baseline_sysv_mappings\":%d}\n",
		(int) result.status, (int) PGM_BOOTSTRAP_PROBE_PASS, warmup_checks,
		iterations, completed_iterations,
		result.checks, total_checks, result.queue_capacity,
		result.startup_packets, result.query_packets, result.copy_packets,
		result.copy_bytes_before_response, result.copy_bytes_received,
		result.simultaneous_queue_saturation,
		result.secure_read_calls, result.secure_write_calls,
		result.socket_wait_calls, result.notice_count,
		result.cancel_dispatches, result.network_connect_calls,
		result.optional_security_calls, result.backend_pid,
		(unsigned long long) result.connection_generation,
		(unsigned long long) result.request_generation,
		(unsigned long long) result.active_transports_after_close,
		(unsigned long long) result.endpoint_references_after_close,
		(unsigned long long) result.allocated_bytes_after_close,
		(long) baseline.pid,
		baseline.pid == after.pid ? "true" : "false",
		resources_restored ? "true" : "false",
		baseline.descriptors, baseline.threads, baseline.children,
		baseline.mappings, baseline.sysv_mappings);
	if (result.status != PGM_BOOTSTRAP_PROBE_PASS)
		fprintf(
			stderr,
			"private libpq probe failed: %s; libpq: %s; "
			"result=%u rows=%d columns=%d value=%s command=%s transaction=%d\n",
			result.detail, result.last_error, result.observed_result_status,
			result.observed_rows, result.observed_columns,
			result.observed_value, result.observed_command_status,
			result.observed_transaction_status);
	if (dlclose(handle) != 0)
		return 5;
	return result.status == PGM_BOOTSTRAP_PROBE_PASS &&
		completed_iterations == iterations && resources_restored ? 0 : 1;
}
