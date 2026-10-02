/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_DATA_DIRECTORY_LOCK_H
#define POSTGAMMA_PRIVATE_DATA_DIRECTORY_LOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POSTGAMMA_DATA_DIRECTORY_LOCK_FILE ".postgamma.lock"

typedef struct PostgammaDataDirectoryLock PostgammaDataDirectoryLock;

typedef struct PostgammaDataDirectoryLockOptions
{
	uint32_t	struct_size;
	uint64_t	generation;
	const char *path;
} PostgammaDataDirectoryLockOptions;

#define POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT \
	{sizeof(PostgammaDataDirectoryLockOptions), UINT64_C(0), NULL}

typedef struct PostgammaDataDirectoryLockTelemetry
{
	uint64_t	generation;
	uint64_t	device;
	uint64_t	inode;
	int			directory_descriptor;
	int			lock_descriptor;
	int			lock_command;
	const char *canonical_path;
	const char *lock_path;
} PostgammaDataDirectoryLockTelemetry;

int postgamma_data_directory_lock_acquire(
	const PostgammaDataDirectoryLockOptions *options,
	PostgammaDataDirectoryLock **lock);
int postgamma_data_directory_lock_telemetry(
	const PostgammaDataDirectoryLock *lock,
	PostgammaDataDirectoryLockTelemetry *telemetry);
int postgamma_data_directory_lock_release(
	PostgammaDataDirectoryLock *lock);
size_t postgamma_data_directory_lock_active_count(void);

#ifdef __cplusplus
}
#endif

#endif
