#ifndef POSTGAMMA_BOOTSTRAP_HOST_PROCESS_PROBE_H
#define POSTGAMMA_BOOTSTRAP_HOST_PROCESS_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>


typedef struct PostgammaBootstrapHostSnapshot
{
	pid_t		pid;
	int			descriptors;
	int			threads;
	int			children;
	int			mappings;
	int			sysv_mappings;
	uint64_t	virtual_memory_kib;
	uint64_t	resident_memory_kib;
	mode_t		umask_value;
	bool		umask_observed;
} PostgammaBootstrapHostSnapshot;

int postgamma_bootstrap_read_status_umask(
	const char *path, mode_t *umask_value, bool *observed);
int postgamma_bootstrap_capture_host(PostgammaBootstrapHostSnapshot *snapshot);
int postgamma_bootstrap_count_file_mappings(const char *path);
bool postgamma_bootstrap_same_umask(
	const PostgammaBootstrapHostSnapshot *left,
	const PostgammaBootstrapHostSnapshot *right);
bool postgamma_bootstrap_same_host_resources(
	const PostgammaBootstrapHostSnapshot *left,
	const PostgammaBootstrapHostSnapshot *right);

#endif
