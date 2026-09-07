/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * path_runtime.c
 *    Instance-owned virtual working-directory and file-path runtime.
 *
 *-------------------------------------------------------------------------
 */

#define _GNU_SOURCE

#include "postgamma/path_runtime.h"

#include "postgamma/guc_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define POSTGAMMA_PATH_RUNTIME_MAGIC UINT64_C(0x5047504154485254)


struct PostgammaPathRuntime
{
	uint64_t	magic;
	uint64_t	generation;
	int			data_directory_fd;
	int			resource_root_fd;
	char	   *canonical_data_directory;
	char	   *canonical_resource_root;
	_Atomic unsigned int logical_umask;
	_Atomic uint64_t relative_operations;
	_Atomic uint64_t absolute_operations;
	_Atomic uint64_t virtual_chdir_calls;
	_Atomic uint64_t logical_umask_calls;
	_Atomic uint64_t resource_operations;
};


typedef struct PostgammaPathTarget
{
	PostgammaPathRuntime *runtime;
	int			directory_fd;
	const char *path;
} PostgammaPathTarget;


static bool path_runtime_is_valid(const PostgammaPathRuntime *runtime);
static PostgammaPathRuntime *current_path_runtime(void);
static PostgammaPathTarget resolve_target(const char *path);
static mode_t current_creation_mode(
	PostgammaPathRuntime *runtime, mode_t requested_mode);
static int duplicate_descriptor(int descriptor);
static int open_directory(const char *path);
static int open_resource_components(
	int root_descriptor, const char *logical_path, int flags);
static int open_with_mode(
	int directory_fd, const char *path, int flags, mode_t mode,
	PostgammaPathRuntime *runtime);
static int fopen_flags(const char *mode, int *flags, char fdopen_mode[4]);
static char *absolute_path(PostgammaPathRuntime *runtime, const char *path);


int
postgamma_path_runtime_create(
	PostgammaPathRuntime **runtime,
	const PostgammaPathRuntimeOptions *options)
{
	PostgammaPathRuntime *created;
	struct stat descriptor_status;
	struct stat path_status;
	int			descriptor;
	int			resource_descriptor = -1;
	const char *resource_root;

	if (runtime == NULL || options == NULL ||
		options->struct_size != sizeof(*options) || options->generation == 0 ||
		options->data_directory_fd < 0 ||
		options->canonical_data_directory == NULL ||
		options->canonical_data_directory[0] != '/' ||
		(options->logical_umask & ~0777) != 0)
		return EINVAL;
	*runtime = NULL;
	if (fstat(options->data_directory_fd, &descriptor_status) != 0)
		return errno != 0 ? errno : EIO;
	if (!S_ISDIR(descriptor_status.st_mode))
		return ENOTDIR;
	if (stat(options->canonical_data_directory, &path_status) != 0)
		return errno != 0 ? errno : EIO;
	if (descriptor_status.st_dev != path_status.st_dev ||
		descriptor_status.st_ino != path_status.st_ino)
		return ESTALE;
	descriptor = duplicate_descriptor(options->data_directory_fd);
	if (descriptor < 0)
		return errno != 0 ? errno : EIO;
	resource_root = options->canonical_resource_root != NULL ?
		options->canonical_resource_root : options->canonical_data_directory;
	if (resource_root[0] != '/')
	{
		(void) close(descriptor);
		return EINVAL;
	}
	resource_descriptor = open_directory(resource_root);
	if (resource_descriptor < 0)
	{
		int			saved_errno = errno;

		(void) close(descriptor);
		return saved_errno != 0 ? saved_errno : EIO;
	}
	created = calloc(1, sizeof(*created));
	if (created == NULL)
	{
		(void) close(descriptor);
		(void) close(resource_descriptor);
		return ENOMEM;
	}
	created->canonical_data_directory =
		strdup(options->canonical_data_directory);
	if (created->canonical_data_directory == NULL)
	{
		(void) close(descriptor);
		(void) close(resource_descriptor);
		free(created);
		return ENOMEM;
	}
	created->canonical_resource_root = strdup(resource_root);
	if (created->canonical_resource_root == NULL)
	{
		(void) close(descriptor);
		(void) close(resource_descriptor);
		free(created->canonical_data_directory);
		free(created);
		return ENOMEM;
	}
	created->magic = POSTGAMMA_PATH_RUNTIME_MAGIC;
	created->generation = options->generation;
	created->data_directory_fd = descriptor;
	created->resource_root_fd = resource_descriptor;
	atomic_init(
		&created->logical_umask, (unsigned int) options->logical_umask);
	atomic_init(&created->relative_operations, UINT64_C(0));
	atomic_init(&created->absolute_operations, UINT64_C(0));
	atomic_init(&created->virtual_chdir_calls, UINT64_C(0));
	atomic_init(&created->logical_umask_calls, UINT64_C(0));
	atomic_init(&created->resource_operations, UINT64_C(0));
	*runtime = created;
	return 0;
}


int
postgamma_path_runtime_destroy(PostgammaPathRuntime *runtime)
{
	int			status = 0;

	if (!path_runtime_is_valid(runtime))
		return EINVAL;
	if (close(runtime->data_directory_fd) != 0)
		status = errno != 0 ? errno : EIO;
	if (close(runtime->resource_root_fd) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	runtime->magic = 0;
	free(runtime->canonical_data_directory);
	free(runtime->canonical_resource_root);
	memset(runtime, 0, sizeof(*runtime));
	free(runtime);
	return status;
}


uint64_t
postgamma_path_runtime_generation(const PostgammaPathRuntime *runtime)
{
	return path_runtime_is_valid(runtime) ? runtime->generation : 0;
}


int
postgamma_path_runtime_telemetry(
	PostgammaPathRuntime *runtime,
	PostgammaPathRuntimeTelemetry *telemetry)
{
	if (!path_runtime_is_valid(runtime) || telemetry == NULL)
		return EINVAL;
	memset(telemetry, 0, sizeof(*telemetry));
	telemetry->generation = runtime->generation;
	telemetry->data_directory_fd = runtime->data_directory_fd;
	telemetry->logical_umask = (mode_t) atomic_load_explicit(
		&runtime->logical_umask, memory_order_relaxed);
	telemetry->relative_operations = atomic_load_explicit(
		&runtime->relative_operations, memory_order_relaxed);
	telemetry->absolute_operations = atomic_load_explicit(
		&runtime->absolute_operations, memory_order_relaxed);
	telemetry->virtual_chdir_calls = atomic_load_explicit(
		&runtime->virtual_chdir_calls, memory_order_relaxed);
	telemetry->logical_umask_calls = atomic_load_explicit(
		&runtime->logical_umask_calls, memory_order_relaxed);
	telemetry->resource_operations = atomic_load_explicit(
		&runtime->resource_operations, memory_order_relaxed);
	telemetry->canonical_data_directory = runtime->canonical_data_directory;
	telemetry->canonical_resource_root = runtime->canonical_resource_root;
	return 0;
}


int
postgamma_path_resource_open(
	PostgammaPathRuntime *runtime, const char *logical_path, int flags)
{
	int			descriptor;

	if (!path_runtime_is_valid(runtime) ||
		!postgamma_path_resource_name_is_valid(logical_path) ||
		(flags & (O_CREAT | O_TRUNC | O_APPEND | O_WRONLY | O_RDWR)) != 0)
	{
		errno = EINVAL;
		return -1;
	}
	descriptor = open_resource_components(
		runtime->resource_root_fd, logical_path, flags);
	if (descriptor >= 0)
		atomic_fetch_add_explicit(
			&runtime->resource_operations, UINT64_C(1), memory_order_relaxed);
	return descriptor;
}


int
postgamma_path_open(const char *path, int flags, ...)
{
	PostgammaPathTarget target;
	mode_t		mode = 0;
	va_list		arguments;

	if ((flags & O_CREAT) != 0
#ifdef O_TMPFILE
		|| (flags & O_TMPFILE) == O_TMPFILE
#endif
		)
	{
		va_start(arguments, flags);
		mode = (mode_t) va_arg(arguments, int);
		va_end(arguments);
	}
	target = resolve_target(path);
	return open_with_mode(
		target.directory_fd, target.path, flags, mode, target.runtime);
}


int
postgamma_path_openat(int directory_fd, const char *path, int flags, ...)
{
	PostgammaPathTarget target;
	mode_t		mode = 0;
	va_list		arguments;

	if ((flags & O_CREAT) != 0
#ifdef O_TMPFILE
		|| (flags & O_TMPFILE) == O_TMPFILE
#endif
		)
	{
		va_start(arguments, flags);
		mode = (mode_t) va_arg(arguments, int);
		va_end(arguments);
	}
	if (directory_fd != AT_FDCWD || path == NULL || path[0] == '/')
		return open_with_mode(directory_fd, path, flags, mode, NULL);
	target = resolve_target(path);
	return open_with_mode(
		target.directory_fd, target.path, flags, mode, target.runtime);
}


FILE *
postgamma_path_fopen(const char *path, const char *mode)
{
	char		fdopen_mode[4];
	int			flags;
	int			descriptor;
	FILE	   *stream;

	if (fopen_flags(mode, &flags, fdopen_mode) != 0)
	{
		errno = EINVAL;
		return NULL;
	}
	descriptor = postgamma_path_open(path, flags, 0666);
	if (descriptor < 0)
		return NULL;
	stream = fdopen(descriptor, fdopen_mode);
	if (stream == NULL)
	{
		int			saved_errno = errno;

		(void) close(descriptor);
		errno = saved_errno;
	}
	return stream;
}


int
postgamma_path_stat(const char *path, struct stat *status_buffer)
{
	PostgammaPathTarget target = resolve_target(path);

	return fstatat(target.directory_fd, target.path, status_buffer, 0);
}


int
postgamma_path_lstat(const char *path, struct stat *status_buffer)
{
	PostgammaPathTarget target = resolve_target(path);

	return fstatat(
		target.directory_fd, target.path, status_buffer, AT_SYMLINK_NOFOLLOW);
}


int
postgamma_path_access(const char *path, int mode)
{
	PostgammaPathTarget target = resolve_target(path);

	return faccessat(target.directory_fd, target.path, mode, 0);
}


int
postgamma_path_unlink(const char *path)
{
	PostgammaPathTarget target = resolve_target(path);

	return unlinkat(target.directory_fd, target.path, 0);
}


int
postgamma_path_remove(const char *path)
{
	PostgammaPathTarget target = resolve_target(path);
	int			status;

	status = unlinkat(target.directory_fd, target.path, 0);
	if (status != 0 && (errno == EISDIR || errno == EPERM))
		status = unlinkat(target.directory_fd, target.path, AT_REMOVEDIR);
	return status;
}


int
postgamma_path_rename(const char *old_path, const char *new_path)
{
	PostgammaPathTarget old_target = resolve_target(old_path);
	PostgammaPathTarget new_target = resolve_target(new_path);

	return renameat(
		old_target.directory_fd, old_target.path,
		new_target.directory_fd, new_target.path);
}


int
postgamma_path_link(const char *old_path, const char *new_path)
{
	PostgammaPathTarget old_target = resolve_target(old_path);
	PostgammaPathTarget new_target = resolve_target(new_path);

	return linkat(
		old_target.directory_fd, old_target.path,
		new_target.directory_fd, new_target.path, 0);
}


int
postgamma_path_symlink(const char *target, const char *link_path)
{
	PostgammaPathTarget link_target = resolve_target(link_path);

	/* A symlink target is data and must retain its original spelling. */
	return symlinkat(target, link_target.directory_fd, link_target.path);
}


ssize_t
postgamma_path_readlink(
	const char *path, char *buffer, size_t buffer_size)
{
	PostgammaPathTarget target = resolve_target(path);

	return readlinkat(
		target.directory_fd, target.path, buffer, buffer_size);
}


int
postgamma_path_mkdir(const char *path, mode_t mode)
{
	PostgammaPathTarget target = resolve_target(path);
	mode_t		effective_mode = current_creation_mode(target.runtime, mode);
	int			status;

	status = mkdirat(target.directory_fd, target.path, effective_mode);
	if (status == 0 && target.runtime != NULL &&
		fchmodat(target.directory_fd, target.path, effective_mode, 0) != 0)
	{
		int			saved_errno = errno;

		(void) unlinkat(target.directory_fd, target.path, AT_REMOVEDIR);
		errno = saved_errno;
		return -1;
	}
	return status;
}


int
postgamma_path_rmdir(const char *path)
{
	PostgammaPathTarget target = resolve_target(path);

	return unlinkat(target.directory_fd, target.path, AT_REMOVEDIR);
}


int
postgamma_path_chmod(const char *path, mode_t mode)
{
	PostgammaPathTarget target = resolve_target(path);

	return fchmodat(target.directory_fd, target.path, mode, 0);
}


int
postgamma_path_chown(const char *path, uid_t owner, gid_t group)
{
	PostgammaPathTarget target = resolve_target(path);

	return fchownat(target.directory_fd, target.path, owner, group, 0);
}


int
postgamma_path_truncate(const char *path, off_t length)
{
	int			descriptor = postgamma_path_open(path, O_WRONLY);
	int			status;

	if (descriptor < 0)
		return -1;
	status = ftruncate(descriptor, length);
	if (close(descriptor) != 0 && status == 0)
		status = -1;
	return status;
}


DIR *
postgamma_path_opendir(const char *path)
{
	int			flags = O_RDONLY;
	int			descriptor;
	DIR		   *directory;

#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
	flags |= O_DIRECTORY;
#endif
	descriptor = postgamma_path_open(path, flags);
	if (descriptor < 0)
		return NULL;
	directory = fdopendir(descriptor);
	if (directory == NULL)
	{
		int			saved_errno = errno;

		(void) close(descriptor);
		errno = saved_errno;
	}
	return directory;
}


char *
postgamma_path_realpath(const char *path, char *resolved_path)
{
	PostgammaPathRuntime *runtime = current_path_runtime();
	char	   *joined;
	char	   *result;

	if (runtime == NULL || path == NULL || path[0] == '/')
		return realpath(path, resolved_path);
	atomic_fetch_add_explicit(
		&runtime->relative_operations, UINT64_C(1), memory_order_relaxed);
	joined = absolute_path(runtime, path);
	if (joined == NULL)
		return NULL;
	result = realpath(joined, resolved_path);
	free(joined);
	return result;
}


int
postgamma_path_chdir(const char *path)
{
	PostgammaPathRuntime *runtime = current_path_runtime();
	PostgammaPathTarget target;
	struct stat target_status;
	struct stat root_status;

	if (runtime == NULL)
		return chdir(path);
	target = resolve_target(path);
	if (fstatat(target.directory_fd, target.path, &target_status, 0) != 0 ||
		fstat(runtime->data_directory_fd, &root_status) != 0)
		return -1;
	if (target_status.st_dev != root_status.st_dev ||
		target_status.st_ino != root_status.st_ino)
	{
		errno = ENOTSUP;
		return -1;
	}
	atomic_fetch_add_explicit(
		&runtime->virtual_chdir_calls, UINT64_C(1), memory_order_relaxed);
	return 0;
}


char *
postgamma_path_getcwd(char *buffer, size_t size)
{
	PostgammaPathRuntime *runtime = current_path_runtime();
	size_t		length;

	if (runtime == NULL)
		return getcwd(buffer, size);
	length = strlen(runtime->canonical_data_directory) + 1;
	if (buffer == NULL)
	{
		if (size != 0 && size < length)
		{
			errno = ERANGE;
			return NULL;
		}
		buffer = malloc(size == 0 ? length : size);
		if (buffer == NULL)
			return NULL;
	}
	else if (size < length)
	{
		errno = ERANGE;
		return NULL;
	}
	memcpy(buffer, runtime->canonical_data_directory, length);
	return buffer;
}


mode_t
postgamma_path_umask(mode_t mask)
{
	PostgammaPathRuntime *runtime = current_path_runtime();
	unsigned int old_mask;

	if (runtime == NULL)
		return umask(mask);
	old_mask = atomic_exchange_explicit(
		&runtime->logical_umask, (unsigned int) mask & 0777,
		memory_order_relaxed);
	atomic_fetch_add_explicit(
		&runtime->logical_umask_calls, UINT64_C(1), memory_order_relaxed);
	return (mode_t) old_mask;
}


static bool
path_runtime_is_valid(const PostgammaPathRuntime *runtime)
{
	return runtime != NULL &&
		runtime->magic == POSTGAMMA_PATH_RUNTIME_MAGIC &&
		runtime->generation != 0 && runtime->data_directory_fd >= 0 &&
		runtime->resource_root_fd >= 0 &&
		runtime->canonical_data_directory != NULL &&
		runtime->canonical_resource_root != NULL;
}


static PostgammaPathRuntime *
current_path_runtime(void)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	PostgammaPathRuntime *runtime;

	if (execution == NULL)
		return NULL;
	runtime = postgamma_instance_context_path_runtime(execution->instance);
	return path_runtime_is_valid(runtime) ? runtime : NULL;
}


static PostgammaPathTarget
resolve_target(const char *path)
{
	PostgammaPathRuntime *runtime = current_path_runtime();
	PostgammaPathTarget target = {
		.runtime = NULL,
		.directory_fd = AT_FDCWD,
		.path = path,
	};

	if (runtime == NULL || path == NULL || path[0] == '/')
	{
		if (runtime != NULL)
			atomic_fetch_add_explicit(
				&runtime->absolute_operations, UINT64_C(1),
				memory_order_relaxed);
		return target;
	}
	target.runtime = runtime;
	target.directory_fd = runtime->data_directory_fd;
	atomic_fetch_add_explicit(
		&runtime->relative_operations, UINT64_C(1), memory_order_relaxed);
	return target;
}


static mode_t
current_creation_mode(PostgammaPathRuntime *runtime, mode_t requested_mode)
{
	mode_t		mask;

	if (runtime == NULL)
		return requested_mode;
	mask = (mode_t) atomic_load_explicit(
		&runtime->logical_umask, memory_order_relaxed);
	return requested_mode & ~mask;
}


static int
duplicate_descriptor(int descriptor)
{
	int			duplicated;

#ifdef F_DUPFD_CLOEXEC
	duplicated = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
	if (duplicated >= 0 || (errno != EINVAL && errno != ENOSYS))
		return duplicated;
#endif
	duplicated = dup(descriptor);
	if (duplicated >= 0 && fcntl(duplicated, F_SETFD, FD_CLOEXEC) != 0)
	{
		int			saved_errno = errno;

		(void) close(duplicated);
		errno = saved_errno;
		return -1;
	}
	return duplicated;
}


static int
open_directory(const char *path)
{
	int			flags = O_RDONLY;

#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
	flags |= O_DIRECTORY;
#endif
	return open(path, flags);
}


bool
postgamma_path_resource_name_is_valid(const char *path)
{
	const char *component = path;
	const char *cursor;

	if (path == NULL || path[0] == '\0' || path[0] == '/' ||
		strchr(path, '\\') != NULL)
		return false;
	for (cursor = path; ; cursor++)
	{
		unsigned char character = (unsigned char) *cursor;

		if ((character < 0x20 && character != '\0') || character == 0x7f)
			return false;
		if (*cursor != '/' && *cursor != '\0')
			continue;
		if (cursor == component ||
			(cursor - component == 1 && component[0] == '.') ||
			(cursor - component == 2 && component[0] == '.' &&
			 component[1] == '.'))
			return false;
		if (*cursor == '\0')
			break;
		component = cursor + 1;
	}
	return true;
}


static int
open_resource_components(
	int root_descriptor, const char *logical_path, int flags)
{
	char	   *copied = strdup(logical_path);
	char	   *save_pointer = NULL;
	char	   *component;
	int			current;

	if (copied == NULL)
		return -1;
	current = duplicate_descriptor(root_descriptor);
	if (current < 0)
	{
		free(copied);
		return -1;
	}
	component = strtok_r(copied, "/", &save_pointer);
	while (component != NULL)
	{
		char	   *next = strtok_r(NULL, "/", &save_pointer);
		int			open_flags = next == NULL ? flags : O_RDONLY;
		int			opened;

#ifdef O_CLOEXEC
		open_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
		open_flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
		if (next != NULL)
			open_flags |= O_DIRECTORY;
#endif
		opened = openat(current, component, open_flags);
		if (close(current) != 0 && opened >= 0)
		{
			int			saved_errno = errno;

			(void) close(opened);
			opened = -1;
			errno = saved_errno;
		}
		if (opened < 0)
		{
			free(copied);
			return -1;
		}
		current = opened;
		component = next;
	}
	free(copied);
	return current;
}


static int
open_with_mode(
	int directory_fd, const char *path, int flags, mode_t mode,
	PostgammaPathRuntime *runtime)
{
	mode_t		effective_mode = current_creation_mode(runtime, mode);
	int			descriptor;
	bool		creates = (flags & O_CREAT) != 0;

#ifdef O_TMPFILE
	if ((flags & O_TMPFILE) == O_TMPFILE)
	{
		descriptor = openat(directory_fd, path, flags, effective_mode);
		if (descriptor >= 0 && runtime != NULL &&
			fchmod(descriptor, effective_mode) != 0)
		{
			int			saved_errno = errno;

			(void) close(descriptor);
			errno = saved_errno;
			return -1;
		}
		return descriptor;
	}
#endif
	if (!creates)
		return openat(directory_fd, path, flags);
	if ((flags & O_EXCL) != 0 || runtime == NULL)
		return openat(directory_fd, path, flags, effective_mode);

	for (int attempt = 0; attempt < 4; attempt++)
	{
		descriptor = openat(
			directory_fd, path, flags | O_EXCL, effective_mode);
		if (descriptor >= 0)
		{
			if (fchmod(descriptor, effective_mode) != 0)
			{
				int			saved_errno = errno;

				(void) close(descriptor);
				(void) unlinkat(directory_fd, path, 0);
				errno = saved_errno;
				return -1;
			}
			return descriptor;
		}
		if (errno != EEXIST)
			return -1;
		descriptor = openat(
			directory_fd, path, flags & ~(O_CREAT | O_EXCL));
		if (descriptor >= 0 || errno != ENOENT)
			return descriptor;
	}
	errno = EAGAIN;
	return -1;
}


static int
fopen_flags(const char *mode, int *flags, char fdopen_mode[4])
{
	bool		plus = false;
	bool		exclusive = false;
	bool		close_on_exec = false;
	size_t		output = 0;

	if (mode == NULL || flags == NULL || fdopen_mode == NULL ||
		(mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a'))
		return EINVAL;
	for (const char *option = mode + 1; *option != '\0'; option++)
	{
		switch (*option)
		{
			case '+':
				plus = true;
				break;
			case 'x':
				exclusive = true;
				break;
			case 'e':
				close_on_exec = true;
				break;
			case 'b':
				break;
			default:
				return EINVAL;
		}
	}
	switch (mode[0])
	{
		case 'r':
			*flags = plus ? O_RDWR : O_RDONLY;
			if (exclusive)
				return EINVAL;
			break;
		case 'w':
			*flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
			break;
		case 'a':
			*flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
			break;
		default:
			return EINVAL;
	}
	if (exclusive)
		*flags |= O_EXCL;
#ifdef O_CLOEXEC
	if (close_on_exec)
		*flags |= O_CLOEXEC;
#else
	(void) close_on_exec;
#endif
	fdopen_mode[output++] = mode[0];
	if (plus)
		fdopen_mode[output++] = '+';
	fdopen_mode[output] = '\0';
	return 0;
}


static char *
absolute_path(PostgammaPathRuntime *runtime, const char *path)
{
	size_t		root_length;
	size_t		path_length;
	bool		has_separator;
	size_t		length;
	char	   *result;

	if (!path_runtime_is_valid(runtime) || path == NULL)
	{
		errno = EINVAL;
		return NULL;
	}
	root_length = strlen(runtime->canonical_data_directory);
	path_length = strlen(path);
	has_separator = root_length != 0 &&
		runtime->canonical_data_directory[root_length - 1] == '/';
	if (root_length > SIZE_MAX - path_length - 2)
	{
		errno = ENAMETOOLONG;
		return NULL;
	}
	length = root_length + (has_separator ? 0 : 1) + path_length + 1;
	result = malloc(length);
	if (result == NULL)
		return NULL;
	(void) snprintf(
		result, length, "%s%s%s", runtime->canonical_data_directory,
		has_separator ? "" : "/", path);
	return result;
}
