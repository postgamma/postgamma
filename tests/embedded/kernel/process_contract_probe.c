#include "tests/embedded/kernel/process_contract_probe.h"

#include "postgamma/private/data_directory_lock.h"
#include "postgamma/thread_runtime.h"

#include <dirent.h>
#include <errno.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


static const char *const PostgammaKernelEnvironmentNames[] =
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


static bool same_environment(
	const PostgammaKernelProcessContractSnapshot *left,
	const PostgammaKernelProcessContractSnapshot *right);
static bool same_signal_state(
	const PostgammaKernelProcessContractSnapshot *left,
	const PostgammaKernelProcessContractSnapshot *right);
static bool same_signal_set(const sigset_t *left, const sigset_t *right);
static bool same_owned_resources(
	const PostgammaBootstrapHostSnapshot *left,
	const PostgammaBootstrapHostSnapshot *right,
	bool require_mapping_stability);
static bool process_contract_restored(
	const PostgammaKernelProcessContractSnapshot *baseline,
	const PostgammaKernelProcessContractSnapshot *current,
	const char *data_directory_a,
	const char *data_directory_b,
	const char *cycle_label,
	bool require_mapping_stability,
	bool diagnose);
static bool delta_is_bounded(
	uint64_t baseline, uint64_t current, uint64_t budget);
static bool pid_file_is_absent(const char *data_directory);
static void dump_open_descriptors(void);
static void dump_threads(void);


int
postgamma_kernel_process_contract_capture(
	PostgammaKernelProcessContractSnapshot *snapshot)
{
	const char *locale_name;
	int			status;

	if (snapshot == NULL)
		return EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->kernel = (PostgammaKernelGlobalTelemetry)
		POSTGAMMA_KERNEL_GLOBAL_TELEMETRY_INIT;
	status = postgamma_bootstrap_capture_host(&snapshot->host);
	if (status != 0)
		return status;
	postgamma_memory_global_telemetry(&snapshot->memory);
	status = postgamma_embedded_kernel_global_telemetry(&snapshot->kernel);
	if (status != 0)
		return status;
	snapshot->active_data_directory_locks =
		postgamma_data_directory_lock_active_count();
#if defined(__GLIBC__)
	{
		struct mallinfo2 allocator = mallinfo2();

		snapshot->allocator_live_bytes =
			(uint64_t) allocator.uordblks + (uint64_t) allocator.hblkhd;
		snapshot->allocator_free_bytes = (uint64_t) allocator.fordblks;
		snapshot->allocator_system_bytes =
			(uint64_t) allocator.arena + (uint64_t) allocator.hblkhd;
		snapshot->allocator_metrics_supported = true;
	}
#endif
	if (getcwd(snapshot->cwd, sizeof(snapshot->cwd)) == NULL)
		return errno != 0 ? errno : EIO;
	locale_name = setlocale(LC_ALL, NULL);
	if (locale_name == NULL)
		return errno != 0 ? errno : EIO;
	if (strlen(locale_name) >= sizeof(snapshot->locale_name))
		return ENAMETOOLONG;
	memcpy(snapshot->locale_name, locale_name, strlen(locale_name) + 1);
	for (size_t index = 0;
		 index < sizeof(PostgammaKernelEnvironmentNames) /
			sizeof(PostgammaKernelEnvironmentNames[0]);
		 index++)
	{
		const char *value = getenv(PostgammaKernelEnvironmentNames[index]);

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


int
postgamma_kernel_process_contract_wait(
	const PostgammaKernelProcessContractSnapshot *baseline,
	PostgammaKernelProcessContractSnapshot *current,
	const char *data_directory_a,
	const char *data_directory_b,
	const char *cycle_label,
	uint64_t deadline_ns)
{
	struct timespec pause = {0, 1000000L};
	int			status;

	if (baseline == NULL || current == NULL || data_directory_a == NULL ||
		data_directory_b == NULL || cycle_label == NULL || deadline_ns == 0)
		return EINVAL;
	for (;;)
	{
		uint64_t	now;

		status = postgamma_kernel_process_contract_capture(current);
		if (status != 0)
			return status;
		if (process_contract_restored(
				baseline, current, data_directory_a, data_directory_b,
				cycle_label, false, false))
			return 0;
		now = postgamma_monotonic_now_ns();
		if (now == 0)
			return EIO;
		if (now >= deadline_ns)
		{
			(void) process_contract_restored(
				baseline, current, data_directory_a, data_directory_b,
				cycle_label, false, true);
			return ETIMEDOUT;
		}
		(void) nanosleep(&pause, NULL);
	}
}


bool
postgamma_kernel_process_contract_restored(
	const PostgammaKernelProcessContractSnapshot *baseline,
	const PostgammaKernelProcessContractSnapshot *current,
	const char *data_directory_a,
	const char *data_directory_b,
	const char *cycle_label,
	bool require_mapping_stability)
{
	return process_contract_restored(
		baseline, current, data_directory_a, data_directory_b, cycle_label,
		require_mapping_stability, true);
}


bool
postgamma_kernel_process_contract_resources_bounded(
	const PostgammaKernelProcessContractSnapshot *baseline,
	const PostgammaKernelProcessContractSnapshot *current,
	const char *cycle_label)
{
	bool		mapping_count;
	bool		virtual_memory;
	bool		resident_memory;
	bool		allocator_live;

	if (baseline == NULL || current == NULL || cycle_label == NULL ||
		baseline->host.mappings < 0 || current->host.mappings < 0 ||
		baseline->allocator_metrics_supported !=
			current->allocator_metrics_supported)
		return false;
	mapping_count = delta_is_bounded(
		(uint64_t) baseline->host.mappings,
		(uint64_t) current->host.mappings,
		POSTGAMMA_KERNEL_MAPPING_DELTA_BUDGET);
	virtual_memory = delta_is_bounded(
		baseline->host.virtual_memory_kib,
		current->host.virtual_memory_kib,
		POSTGAMMA_KERNEL_VIRTUAL_DELTA_BUDGET_KIB);
	resident_memory = delta_is_bounded(
		baseline->host.resident_memory_kib,
		current->host.resident_memory_kib,
		POSTGAMMA_KERNEL_RESIDENT_DELTA_BUDGET_KIB);
	allocator_live = !baseline->allocator_metrics_supported ||
		delta_is_bounded(
			baseline->allocator_live_bytes, current->allocator_live_bytes,
			POSTGAMMA_KERNEL_ALLOCATOR_LIVE_DELTA_BUDGET_BYTES);
	if (mapping_count && virtual_memory && resident_memory && allocator_live)
		return true;
	fprintf(stderr,
		"process resource budget cycle=%s mappings=%d/%d+%d"
		" virtual_kib=%llu/%llu+%llu resident_kib=%llu/%llu+%llu"
		" allocator_live=%llu/%llu+%llu supported=%s\n",
		cycle_label, current->host.mappings, baseline->host.mappings,
		POSTGAMMA_KERNEL_MAPPING_DELTA_BUDGET,
		(unsigned long long) current->host.virtual_memory_kib,
		(unsigned long long) baseline->host.virtual_memory_kib,
		(unsigned long long) POSTGAMMA_KERNEL_VIRTUAL_DELTA_BUDGET_KIB,
		(unsigned long long) current->host.resident_memory_kib,
		(unsigned long long) baseline->host.resident_memory_kib,
		(unsigned long long) POSTGAMMA_KERNEL_RESIDENT_DELTA_BUDGET_KIB,
		(unsigned long long) current->allocator_live_bytes,
		(unsigned long long) baseline->allocator_live_bytes,
		(unsigned long long) POSTGAMMA_KERNEL_ALLOCATOR_LIVE_DELTA_BUDGET_BYTES,
		baseline->allocator_metrics_supported ? "true" : "false");
	if (baseline->allocator_metrics_supported &&
		current->allocator_metrics_supported)
		fprintf(stderr,
			"allocator baseline={live=%llu free=%llu system=%llu}"
			" current={live=%llu free=%llu system=%llu}\n",
			(unsigned long long) baseline->allocator_live_bytes,
			(unsigned long long) baseline->allocator_free_bytes,
			(unsigned long long) baseline->allocator_system_bytes,
			(unsigned long long) current->allocator_live_bytes,
			(unsigned long long) current->allocator_free_bytes,
			(unsigned long long) current->allocator_system_bytes);
	return false;
}


static bool
process_contract_restored(
	const PostgammaKernelProcessContractSnapshot *baseline,
	const PostgammaKernelProcessContractSnapshot *current,
	const char *data_directory_a,
	const char *data_directory_b,
	const char *cycle_label,
	bool require_mapping_stability,
	bool diagnose)
{
	bool		resources;
	bool		cwd;
	bool		locale;
	bool		environment;
	bool		signals;
	bool		memory;
	bool		kernel;
	bool		locks;
	bool		pid_files;

	if (baseline == NULL || current == NULL || data_directory_a == NULL ||
		data_directory_b == NULL || cycle_label == NULL)
		return false;
	resources = same_owned_resources(
		&baseline->host, &current->host, require_mapping_stability);
	cwd = strcmp(baseline->cwd, current->cwd) == 0;
	locale = strcmp(baseline->locale_name, current->locale_name) == 0;
	environment = same_environment(baseline, current);
	signals = same_signal_state(baseline, current);
	memory = baseline->memory.active_transports ==
			current->memory.active_transports &&
		baseline->memory.endpoint_references ==
			current->memory.endpoint_references &&
		baseline->memory.allocated_bytes == current->memory.allocated_bytes;
	kernel = baseline->kernel.active_instances == 0 &&
		baseline->kernel.active_memory_contexts == 0 &&
		current->kernel.active_instances == 0 &&
		current->kernel.active_memory_contexts == 0 &&
		baseline->kernel.cleanup_failures == current->kernel.cleanup_failures;
	locks = baseline->active_data_directory_locks == 0 &&
		current->active_data_directory_locks == 0;
	pid_files = pid_file_is_absent(data_directory_a) &&
		pid_file_is_absent(data_directory_b);
	if (resources && cwd && locale && environment && signals && memory &&
		kernel && locks && pid_files)
		return true;
	if (!diagnose)
		return false;
	fprintf(stderr,
		"process contract cycle=%s resources=%s cwd=%s locale=%s"
		" environment=%s signals=%s memory=%s kernel=%s locks=%s"
		" pid_files=%s\n",
		cycle_label, resources ? "restored" : "changed",
		cwd ? "restored" : "changed", locale ? "restored" : "changed",
		environment ? "restored" : "changed",
		signals ? "restored" : "changed",
		memory ? "restored" : "changed",
		kernel ? "restored" : "changed",
		locks ? "released" : "active", pid_files ? "absent" : "present");
	fprintf(stderr,
		"host before={pid=%ld fd=%d threads=%d children=%d mappings=%d"
		" sysv=%d umask=%03o} after={pid=%ld fd=%d threads=%d children=%d"
		" mappings=%d sysv=%d umask=%03o}\n",
		(long) baseline->host.pid, baseline->host.descriptors,
		baseline->host.threads, baseline->host.children,
		baseline->host.mappings, baseline->host.sysv_mappings,
		(unsigned int) baseline->host.umask_value,
		(long) current->host.pid, current->host.descriptors,
		current->host.threads, current->host.children,
		current->host.mappings, current->host.sysv_mappings,
		(unsigned int) current->host.umask_value);
	fprintf(stderr,
		"memory before={transports=%llu references=%llu bytes=%llu}"
		" after={transports=%llu references=%llu bytes=%llu}\n",
		(unsigned long long) baseline->memory.active_transports,
		(unsigned long long) baseline->memory.endpoint_references,
		(unsigned long long) baseline->memory.allocated_bytes,
		(unsigned long long) current->memory.active_transports,
		(unsigned long long) current->memory.endpoint_references,
		(unsigned long long) current->memory.allocated_bytes);
	fprintf(stderr,
		"kernel before={active=%llu memory_contexts=%llu cleanup=%llu}"
		" after={active=%llu memory_contexts=%llu cleanup=%llu}\n",
		(unsigned long long) baseline->kernel.active_instances,
		(unsigned long long) baseline->kernel.active_memory_contexts,
		(unsigned long long) baseline->kernel.cleanup_failures,
		(unsigned long long) current->kernel.active_instances,
		(unsigned long long) current->kernel.active_memory_contexts,
		(unsigned long long) current->kernel.cleanup_failures);
	dump_open_descriptors();
	dump_threads();
	return false;
}


static bool
delta_is_bounded(uint64_t baseline, uint64_t current, uint64_t budget)
{
	return current <= baseline || current - baseline <= budget;
}


static bool
same_environment(
	const PostgammaKernelProcessContractSnapshot *left,
	const PostgammaKernelProcessContractSnapshot *right)
{
	for (size_t index = 0;
		 index < sizeof(left->environment) / sizeof(left->environment[0]);
		 index++)
	{
		if (left->environment[index].present !=
			right->environment[index].present)
			return false;
		if (left->environment[index].present &&
			strcmp(left->environment[index].value,
				right->environment[index].value) != 0)
			return false;
	}
	return true;
}


static bool
same_signal_state(
	const PostgammaKernelProcessContractSnapshot *left,
	const PostgammaKernelProcessContractSnapshot *right)
{
	if (!same_signal_set(&left->signal_mask, &right->signal_mask))
		return false;
	for (int signal_number = 1; signal_number < NSIG; signal_number++)
	{
		const struct sigaction *left_action =
			&left->signal_actions[signal_number];
		const struct sigaction *right_action =
			&right->signal_actions[signal_number];

		if (left->signal_action_valid[signal_number] !=
			right->signal_action_valid[signal_number])
			return false;
		if (!left->signal_action_valid[signal_number])
			continue;
		if (left_action->sa_flags != right_action->sa_flags ||
			left_action->sa_handler != right_action->sa_handler ||
			!same_signal_set(&left_action->sa_mask, &right_action->sa_mask))
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
same_owned_resources(
	const PostgammaBootstrapHostSnapshot *left,
	const PostgammaBootstrapHostSnapshot *right,
	bool require_mapping_stability)
{
	return left != NULL && right != NULL && left->pid == right->pid &&
		left->descriptors == right->descriptors &&
		left->threads == right->threads &&
		left->children == right->children &&
		(!require_mapping_stability || left->mappings == right->mappings) &&
		left->sysv_mappings == right->sysv_mappings &&
		left->umask_value == right->umask_value;
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


static void
dump_threads(void)
{
	DIR		   *directory;
	struct dirent *entry;

	directory = opendir("/proc/self/task");
	if (directory == NULL)
		return;
	while ((entry = readdir(directory)) != NULL)
	{
		char		path[128];
		char		name[256] = "unknown";
		char	   *end = NULL;
		long		thread_id;
		FILE	   *comm;

		errno = 0;
		thread_id = strtol(entry->d_name, &end, 10);
		if (errno != 0 || end == entry->d_name || *end != '\0')
			continue;
		if (snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", thread_id) >=
			(int) sizeof(path))
			continue;
		comm = fopen(path, "r");
		if (comm != NULL)
		{
			size_t		length;

			if (fgets(name, sizeof(name), comm) != NULL)
			{
				length = strlen(name);
				if (length != 0 && name[length - 1] == '\n')
					name[length - 1] = '\0';
			}
			(void) fclose(comm);
		}
		fprintf(stderr, "host thread=%ld name=%s\n", thread_id, name);
	}
	(void) closedir(directory);
}
