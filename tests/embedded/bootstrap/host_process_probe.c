#define _GNU_SOURCE

#include "tests/embedded/bootstrap/host_process_probe.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static int
count_directory(const char *path)
{
	DIR		   *directory = opendir(path);
	struct dirent *entry;
	int			count = 0;

	if (directory == NULL)
		return -1;
	while ((entry = readdir(directory)) != NULL)
	{
		if (strcmp(entry->d_name, ".") != 0 &&
			strcmp(entry->d_name, "..") != 0)
			count++;
	}
	if (closedir(directory) != 0)
		return -1;
	return count;
}


static int
count_children(void)
{
	char		path[64];
	char		content[4096];
	ssize_t		length;
	ssize_t		index;
	int			descriptor;
	int			count = 0;
	bool		in_number = false;

	if (snprintf(path, sizeof(path), "/proc/self/task/%ld/children",
				 (long) getpid()) >= (int) sizeof(path))
		return -1;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	length = read(descriptor, content, sizeof(content));
	if (close(descriptor) != 0 || length < 0)
		return -1;
	for (index = 0; index < length; index++)
	{
		if (content[index] >= '0' && content[index] <= '9')
		{
			if (!in_number)
				count++;
			in_number = true;
		}
		else
			in_number = false;
	}
	return count;
}


static int
count_mappings(int *sysv_mappings)
{
	FILE	   *maps = fopen("/proc/self/maps", "r");
	char	   *line = NULL;
	size_t		capacity = 0;
	int			count = 0;
	int			sysv = 0;

	if (maps == NULL)
		return -1;
	while (getline(&line, &capacity, maps) >= 0)
	{
		count++;
		if (strstr(line, "/SYSV") != NULL)
			sysv++;
	}
	free(line);
	if (fclose(maps) != 0)
		return -1;
	*sysv_mappings = sysv;
	return count;
}


static int
read_process_umask(mode_t *umask_value)
{
	FILE	   *status_file;
	char	   *line = NULL;
	size_t		capacity = 0;
	unsigned int parsed = 0;
	bool		found = false;

	if (umask_value == NULL)
		return EINVAL;
	status_file = fopen("/proc/self/status", "r");
	if (status_file == NULL)
		return errno != 0 ? errno : EIO;
	while (getline(&line, &capacity, status_file) >= 0)
	{
		if (sscanf(line, "Umask:\t%o", &parsed) == 1)
		{
			found = true;
			break;
		}
	}
	free(line);
	if (fclose(status_file) != 0)
		return errno != 0 ? errno : EIO;
	if (!found || parsed > 0777)
		return EPROTO;
	*umask_value = (mode_t) parsed;
	return 0;
}


static int
read_process_memory(uint64_t *virtual_kib, uint64_t *resident_kib)
{
	FILE	   *status_file;
	char	   *line = NULL;
	size_t		capacity = 0;
	bool		virtual_found = false;
	bool		resident_found = false;

	if (virtual_kib == NULL || resident_kib == NULL)
		return EINVAL;
	status_file = fopen("/proc/self/status", "r");
	if (status_file == NULL)
		return errno != 0 ? errno : EIO;
	while (getline(&line, &capacity, status_file) >= 0)
	{
		uint64_t	value;

		if (sscanf(line, "VmSize:%" SCNu64 " kB", &value) == 1)
		{
			*virtual_kib = value;
			virtual_found = true;
		}
		else if (sscanf(line, "VmRSS:%" SCNu64 " kB", &value) == 1)
		{
			*resident_kib = value;
			resident_found = true;
		}
	}
	free(line);
	if (fclose(status_file) != 0)
		return errno != 0 ? errno : EIO;
	return virtual_found && resident_found ? 0 : EPROTO;
}


int
postgamma_bootstrap_count_file_mappings(const char *path)
{
	FILE	   *maps;
	char		target[PATH_MAX];
	char	   *line = NULL;
	size_t		capacity = 0;
	int			count = 0;

	if (path == NULL || realpath(path, target) == NULL)
		return -1;
	maps = fopen("/proc/self/maps", "r");
	if (maps == NULL)
		return -1;
	while (getline(&line, &capacity, maps) >= 0)
	{
		if (strstr(line, target) != NULL)
			count++;
	}
	free(line);
	if (fclose(maps) != 0)
		return -1;
	return count;
}


int
postgamma_bootstrap_capture_host(PostgammaBootstrapHostSnapshot *snapshot)
{
	int			status;

	if (snapshot == NULL)
		return EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->pid = getpid();
	snapshot->descriptors = count_directory("/proc/self/fd");
	snapshot->threads = count_directory("/proc/self/task");
	snapshot->children = count_children();
	snapshot->mappings = count_mappings(&snapshot->sysv_mappings);
	status = read_process_umask(&snapshot->umask_value);
	if (status == 0)
		status = read_process_memory(
			&snapshot->virtual_memory_kib,
			&snapshot->resident_memory_kib);
	if (snapshot->descriptors < 0 || snapshot->threads < 0 ||
		snapshot->children < 0 || snapshot->mappings < 0 || status != 0)
		return EIO;
	return 0;
}


bool
postgamma_bootstrap_same_host_resources(const PostgammaBootstrapHostSnapshot *left,
								 const PostgammaBootstrapHostSnapshot *right)
{
	return left != NULL && right != NULL && left->pid == right->pid &&
		left->descriptors == right->descriptors &&
		left->threads == right->threads &&
		left->children == right->children &&
		left->mappings == right->mappings &&
		left->sysv_mappings == right->sysv_mappings &&
		left->umask_value == right->umask_value;
}
