#include "postgamma/private/bootstrap_probe.h"
#include "tests/embedded/bootstrap/host_process_probe.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef pgm_bootstrap_probe_result (*PostgammaBootstrapProbeFunction)(void);

void *
palloc(size_t size)
{
	(void) size;
	return NULL;
}

static uintptr_t
host_palloc_address(void)
{
	void *(*allocator)(size_t) = palloc;
	uintptr_t address = 0;

	_Static_assert(sizeof(allocator) <= sizeof(address),
				   "function pointer does not fit in the bootstrap address probe");
	memcpy(&address, &allocator, sizeof(allocator));
	return address;
}

int
main(int argc, char **argv)
{
	PostgammaBootstrapProbeFunction probe = NULL;
	pgm_bootstrap_probe_result first;
	pgm_bootstrap_probe_result second;
	PostgammaBootstrapHostSnapshot before;
	PostgammaBootstrapHostSnapshot after;
	void *handle;
	void *symbol;
	int host_symbol_interposed;
	int close_status;
	int library_mappings_after_close;
	bool resources_restored;

	if (argc != 2)
		return 2;
	if (postgamma_bootstrap_capture_host(&before) != 0)
		return 6;
	handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL)
	{
		fprintf(stderr, "cannot load bootstrap library: %s\n", dlerror());
		return 3;
	}
	symbol = dlsym(handle, "pgm_bootstrap_probe_static_module");
	if (symbol == NULL || sizeof(symbol) != sizeof(probe))
	{
		fprintf(stderr, "cannot resolve bootstrap probe: %s\n", dlerror());
		dlclose(handle);
		return 4;
	}
	memcpy(&probe, &symbol, sizeof(probe));
	first = probe();
	second = probe();
	host_symbol_interposed = first.kernel_symbol_address == host_palloc_address();
	close_status = dlclose(handle);
	if (close_status != 0 || postgamma_bootstrap_capture_host(&after) != 0)
		return 5;
	library_mappings_after_close = postgamma_bootstrap_count_file_mappings(argv[1]);
	if (library_mappings_after_close < 0)
		return 5;
	resources_restored = before.pid == after.pid &&
		before.descriptors == after.descriptors &&
		before.threads == after.threads &&
		before.children == after.children &&
		before.sysv_mappings == after.sysv_mappings &&
		library_mappings_after_close == 0;
	printf(
		"{\"status\":%d,\"checks\":%u,\"dynamic_load_attempts\":%u,"
		"\"init_calls\":%u,\"repeat_status\":%d,\"repeat_init_calls\":%u,"
		"\"host_symbol_interposed\":%s,\"host_pid\":%ld,"
		"\"host_pid_unchanged\":%s,\"resources_restored\":%s,"
		"\"baseline_descriptors\":%d,\"baseline_threads\":%d,"
		"\"baseline_children\":%d,\"baseline_mappings\":%d,"
		"\"baseline_sysv_mappings\":%d,"
		"\"library_mappings_after_close\":%d}\n",
		(int) first.status,
		first.checks,
		first.dynamic_load_attempts,
		first.init_calls,
		(int) second.status,
		second.init_calls,
		host_symbol_interposed ? "true" : "false",
		(long) before.pid,
		before.pid == after.pid ? "true" : "false",
		resources_restored ? "true" : "false",
		before.descriptors, before.threads, before.children,
		before.mappings, before.sysv_mappings, library_mappings_after_close);
	return first.status == PGM_BOOTSTRAP_PROBE_PASS &&
		second.status == PGM_BOOTSTRAP_PROBE_PASS &&
		first.dynamic_load_attempts == 0 &&
		first.init_calls == 1 && second.init_calls == 1 &&
		first.kernel_symbol_address != 0 &&
		!host_symbol_interposed && resources_restored ? 0 : 1;
}
