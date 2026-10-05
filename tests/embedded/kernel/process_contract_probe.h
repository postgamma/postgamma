#ifndef POSTGAMMA_KERNEL_PROCESS_CONTRACT_PROBE_H
#define POSTGAMMA_KERNEL_PROCESS_CONTRACT_PROBE_H

#include "postgamma/embedded_kernel.h"
#include "postgamma/private/memory_transport.h"
#include "tests/embedded/bootstrap/host_process_probe.h"

#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define POSTGAMMA_KERNEL_LOCALE_CAPACITY 512
#define POSTGAMMA_KERNEL_ENVIRONMENT_CAPACITY 1024
#define POSTGAMMA_KERNEL_ENVIRONMENT_VARIABLES 8
#define POSTGAMMA_KERNEL_MAPPING_DELTA_BUDGET 32
#define POSTGAMMA_KERNEL_VIRTUAL_DELTA_BUDGET_KIB UINT64_C(262144)
#define POSTGAMMA_KERNEL_RESIDENT_DELTA_BUDGET_KIB UINT64_C(131072)
#define POSTGAMMA_KERNEL_MAPPING_STEADY_DRIFT_BUDGET 4
#define POSTGAMMA_KERNEL_VIRTUAL_STEADY_DRIFT_BUDGET_KIB UINT64_C(32768)
#define POSTGAMMA_KERNEL_RESIDENT_STEADY_DRIFT_BUDGET_KIB UINT64_C(16384)
#define POSTGAMMA_KERNEL_ALLOCATOR_LIVE_DELTA_BUDGET_BYTES UINT64_C(8388608)
#define POSTGAMMA_KERNEL_ALLOCATOR_LIVE_STEADY_DRIFT_BUDGET_BYTES \
	UINT64_C(1048576)


typedef struct PostgammaKernelEnvironmentValue
{
	bool		present;
	char		value[POSTGAMMA_KERNEL_ENVIRONMENT_CAPACITY];
} PostgammaKernelEnvironmentValue;


typedef struct PostgammaKernelProcessContractSnapshot
{
	PostgammaBootstrapHostSnapshot host;
	PostgammaMemoryGlobalTelemetry memory;
	PostgammaKernelGlobalTelemetry kernel;
	size_t		active_data_directory_locks;
	uint64_t	allocator_live_bytes;
	uint64_t	allocator_free_bytes;
	uint64_t	allocator_system_bytes;
	bool		allocator_metrics_supported;
	char		cwd[PATH_MAX];
	char		locale_name[POSTGAMMA_KERNEL_LOCALE_CAPACITY];
	PostgammaKernelEnvironmentValue
		environment[POSTGAMMA_KERNEL_ENVIRONMENT_VARIABLES];
	sigset_t	signal_mask;
	struct sigaction signal_actions[NSIG];
	bool		signal_action_valid[NSIG];
} PostgammaKernelProcessContractSnapshot;


int postgamma_kernel_process_contract_capture(
	PostgammaKernelProcessContractSnapshot *snapshot);
int postgamma_kernel_process_contract_wait(
	const PostgammaKernelProcessContractSnapshot *baseline,
	PostgammaKernelProcessContractSnapshot *current,
	const char *data_directory_a,
	const char *data_directory_b,
	const char *cycle_label,
	uint64_t deadline_ns);
bool postgamma_kernel_process_contract_restored(
	const PostgammaKernelProcessContractSnapshot *baseline,
	const PostgammaKernelProcessContractSnapshot *current,
	const char *data_directory_a,
	const char *data_directory_b,
	const char *cycle_label,
	bool require_mapping_stability);
bool postgamma_kernel_process_contract_resources_bounded(
	const PostgammaKernelProcessContractSnapshot *baseline,
	const PostgammaKernelProcessContractSnapshot *current,
	const char *cycle_label);

#endif
