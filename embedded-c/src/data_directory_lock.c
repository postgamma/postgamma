/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE

#include "postgamma/private/data_directory_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


#define POSTGAMMA_DATA_DIRECTORY_LOCK_MAGIC \
	UINT64_C(0x50474d4449524c4b)


struct PostgammaDataDirectoryLock
{
	uint64_t	magic;
	uint64_t	generation;
	dev_t		device;
	ino_t		inode;
	int			directory_descriptor;
	int			descriptor;
	int			lock_command;
	char	   *canonical_path;
	char	   *lock_path;
	PostgammaDataDirectoryLock *next;
};


static pthread_mutex_t PostgammaDataDirectoryRegistryMutex =
	PTHREAD_MUTEX_INITIALIZER;
static PostgammaDataDirectoryLock *PostgammaDataDirectoryRegistry;
static size_t PostgammaDataDirectoryActiveCount;


static bool lock_is_valid(const PostgammaDataDirectoryLock *lock);
static int open_directory(const char *path);
static char *join_lock_path(const char *directory);
static bool registry_contains(dev_t device, ino_t inode);
static int acquire_os_lock(int descriptor, int *lock_command);
static int release_os_lock(int descriptor, int lock_command);
static int write_lock_metadata(int descriptor, uint64_t generation);
static int close_preserving_status(int descriptor, int status);


int
postgamma_data_directory_lock_acquire(
	const PostgammaDataDirectoryLockOptions *options,
	PostgammaDataDirectoryLock **lock)
{
	PostgammaDataDirectoryLock *created;
	struct stat directory_status;
	struct stat lock_status;
	char	   *canonical_path;
	char	   *lock_path;
	int			directory_descriptor;
	int			descriptor = -1;
	int			open_flags = O_RDWR | O_CREAT;
	int			status;

	if (options == NULL || lock == NULL ||
		options->struct_size != sizeof(*options) ||
		options->generation == 0 || options->path == NULL ||
		options->path[0] == '\0')
		return EINVAL;
	*lock = NULL;
	errno = 0;
	canonical_path = realpath(options->path, NULL);
	if (canonical_path == NULL)
		return errno != 0 ? errno : EINVAL;
	lock_path = join_lock_path(canonical_path);
	if (lock_path == NULL)
	{
		free(canonical_path);
		return ENOMEM;
	}
	directory_descriptor = open_directory(canonical_path);
	if (directory_descriptor < 0)
	{
		status = errno;
		free(lock_path);
		free(canonical_path);
		return status;
	}
	if (fstat(directory_descriptor, &directory_status) != 0)
	{
		status = errno;
		(void) close(directory_descriptor);
		free(lock_path);
		free(canonical_path);
		return status;
	}
	if (!S_ISDIR(directory_status.st_mode))
	{
		(void) close(directory_descriptor);
		free(lock_path);
		free(canonical_path);
		return ENOTDIR;
	}
	created = calloc(1, sizeof(*created));
	if (created == NULL)
	{
		(void) close(directory_descriptor);
		free(lock_path);
		free(canonical_path);
		return ENOMEM;
	}
	created->generation = options->generation;
	created->device = directory_status.st_dev;
	created->inode = directory_status.st_ino;
	created->directory_descriptor = -1;
	created->descriptor = -1;
	created->canonical_path = canonical_path;
	created->lock_path = lock_path;

	status = pthread_mutex_lock(&PostgammaDataDirectoryRegistryMutex);
	if (status != 0)
		goto fail;
	if (registry_contains(created->device, created->inode))
	{
		status = EBUSY;
		goto fail_locked;
	}
#ifdef O_CLOEXEC
	open_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	open_flags |= O_NOFOLLOW;
#endif
	descriptor = openat(
		directory_descriptor, POSTGAMMA_DATA_DIRECTORY_LOCK_FILE,
		open_flags, S_IRUSR | S_IWUSR);
	if (descriptor < 0)
	{
		status = errno;
		goto fail_locked;
	}
#ifndef O_CLOEXEC
	if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0)
	{
		status = errno;
		goto fail_locked;
	}
#endif
	if (fstat(descriptor, &lock_status) != 0)
	{
		status = errno;
		goto fail_locked;
	}
	if (!S_ISREG(lock_status.st_mode))
	{
		status = EINVAL;
		goto fail_locked;
	}
	status = acquire_os_lock(descriptor, &created->lock_command);
	if (status != 0)
		goto fail_locked;
	status = write_lock_metadata(descriptor, created->generation);
	if (status != 0)
	{
		(void) release_os_lock(descriptor, created->lock_command);
		goto fail_locked;
	}
	created->descriptor = descriptor;
	created->directory_descriptor = directory_descriptor;
	created->magic = POSTGAMMA_DATA_DIRECTORY_LOCK_MAGIC;
	created->next = PostgammaDataDirectoryRegistry;
	PostgammaDataDirectoryRegistry = created;
	PostgammaDataDirectoryActiveCount++;
	status = pthread_mutex_unlock(&PostgammaDataDirectoryRegistryMutex);
	if (status != 0)
		return status;
	*lock = created;
	return 0;

fail_locked:
	if (descriptor >= 0)
		status = close_preserving_status(descriptor, status);
	{
		int			unlock_status = pthread_mutex_unlock(
			&PostgammaDataDirectoryRegistryMutex);

		if (unlock_status != 0)
			status = unlock_status;
	}
fail:
	(void) close(directory_descriptor);
	free(created->lock_path);
	free(created->canonical_path);
	free(created);
	return status;
}


int
postgamma_data_directory_lock_telemetry(
	const PostgammaDataDirectoryLock *lock,
	PostgammaDataDirectoryLockTelemetry *telemetry)
{
	if (!lock_is_valid(lock) || telemetry == NULL)
		return EINVAL;
	memset(telemetry, 0, sizeof(*telemetry));
	telemetry->generation = lock->generation;
	telemetry->device = (uint64_t) lock->device;
	telemetry->inode = (uint64_t) lock->inode;
	telemetry->directory_descriptor = lock->directory_descriptor;
	telemetry->lock_descriptor = lock->descriptor;
	telemetry->lock_command = lock->lock_command;
	telemetry->canonical_path = lock->canonical_path;
	telemetry->lock_path = lock->lock_path;
	return 0;
}


int
postgamma_data_directory_lock_release(PostgammaDataDirectoryLock *lock)
{
	PostgammaDataDirectoryLock **link;
	int			status;
	int			unlock_status;

	if (!lock_is_valid(lock))
		return EINVAL;
	status = pthread_mutex_lock(&PostgammaDataDirectoryRegistryMutex);
	if (status != 0)
		return status;
	for (link = &PostgammaDataDirectoryRegistry;
		 *link != NULL && *link != lock;
		 link = &(*link)->next)
		;
	if (*link == NULL || PostgammaDataDirectoryActiveCount == 0)
	{
		(void) pthread_mutex_unlock(&PostgammaDataDirectoryRegistryMutex);
		return EPROTO;
	}
	status = release_os_lock(lock->descriptor, lock->lock_command);
	status = close_preserving_status(lock->descriptor, status);
	status = close_preserving_status(lock->directory_descriptor, status);
	*link = lock->next;
	PostgammaDataDirectoryActiveCount--;
	unlock_status = pthread_mutex_unlock(
		&PostgammaDataDirectoryRegistryMutex);
	if (unlock_status != 0)
		status = unlock_status;
	lock->magic = 0;
	lock->descriptor = -1;
	lock->directory_descriptor = -1;
	free(lock->lock_path);
	free(lock->canonical_path);
	memset(lock, 0, sizeof(*lock));
	free(lock);
	return status;
}


size_t
postgamma_data_directory_lock_active_count(void)
{
	size_t		count = 0;

	if (pthread_mutex_lock(&PostgammaDataDirectoryRegistryMutex) != 0)
		return SIZE_MAX;
	count = PostgammaDataDirectoryActiveCount;
	if (pthread_mutex_unlock(&PostgammaDataDirectoryRegistryMutex) != 0)
		return SIZE_MAX;
	return count;
}


static bool
lock_is_valid(const PostgammaDataDirectoryLock *lock)
{
	return lock != NULL &&
		lock->magic == POSTGAMMA_DATA_DIRECTORY_LOCK_MAGIC &&
		lock->generation != 0 && lock->directory_descriptor >= 0 &&
		lock->descriptor >= 0 &&
		lock->canonical_path != NULL && lock->lock_path != NULL;
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


static char *
join_lock_path(const char *directory)
{
	size_t		directory_length = strlen(directory);
	size_t		file_length = strlen(POSTGAMMA_DATA_DIRECTORY_LOCK_FILE);
	bool		has_separator = directory_length != 0 &&
		directory[directory_length - 1] == '/';
	size_t		length;
	char	   *path;

	if (directory_length > SIZE_MAX - file_length - 2)
		return NULL;
	length = directory_length + (has_separator ? 0 : 1) + file_length + 1;
	path = malloc(length);
	if (path == NULL)
		return NULL;
	(void) snprintf(path, length, "%s%s%s", directory,
					has_separator ? "" : "/",
					POSTGAMMA_DATA_DIRECTORY_LOCK_FILE);
	return path;
}


static bool
registry_contains(dev_t device, ino_t inode)
{
	PostgammaDataDirectoryLock *current;

	for (current = PostgammaDataDirectoryRegistry;
		 current != NULL;
		 current = current->next)
	{
		if (current->device == device && current->inode == inode)
			return true;
	}
	return false;
}


static int
acquire_os_lock(int descriptor, int *lock_command)
{
	struct flock request;
	int			status;

	memset(&request, 0, sizeof(request));
	request.l_type = F_WRLCK;
	request.l_whence = SEEK_SET;
#ifdef F_OFD_SETLK
	do
		status = fcntl(descriptor, F_OFD_SETLK, &request);
	while (status != 0 && errno == EINTR);
	if (status == 0)
	{
		*lock_command = F_OFD_SETLK;
		return 0;
	}
	if (errno != EINVAL && errno != ENOTSUP)
		return errno == EACCES || errno == EAGAIN ? EBUSY : errno;
#endif
	do
		status = fcntl(descriptor, F_SETLK, &request);
	while (status != 0 && errno == EINTR);
	if (status != 0)
		return errno == EACCES || errno == EAGAIN ? EBUSY : errno;
	*lock_command = F_SETLK;
	return 0;
}


static int
release_os_lock(int descriptor, int lock_command)
{
	struct flock request;
	int			status;

	memset(&request, 0, sizeof(request));
	request.l_type = F_UNLCK;
	request.l_whence = SEEK_SET;
	do
		status = fcntl(descriptor, lock_command, &request);
	while (status != 0 && errno == EINTR);
	return status == 0 ? 0 : errno;
}


static int
write_lock_metadata(int descriptor, uint64_t generation)
{
	char		metadata[128];
	int			length;
	ssize_t		written = 0;

	length = snprintf(
		metadata, sizeof(metadata),
		"postgamma_embedded_lock_v1\ngeneration=%" PRIu64 "\n",
		generation);
	if (length < 0 || (size_t) length >= sizeof(metadata))
		return EOVERFLOW;
	while (written < length)
	{
		ssize_t		result = pwrite(
			descriptor, metadata + written,
			(size_t) length - (size_t) written, written);

		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0)
			return result == 0 ? EIO : errno;
		written += result;
	}
	if (ftruncate(descriptor, length) != 0)
		return errno;
	return 0;
}


static int
close_preserving_status(int descriptor, int status)
{
	if (close(descriptor) != 0 && status == 0)
		return errno;
	return status;
}
