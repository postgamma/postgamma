/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_INITDB_HOST_H
#define POSTGAMMA_PRIVATE_INITDB_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "postgamma/private/data_directory_lock.h"


#ifdef __cplusplus
extern "C" {
#endif


typedef struct PostgammaInitdbSetting
{
	const char *name;
	const char *value;
} PostgammaInitdbSetting;

typedef enum PostgammaInitdbFaultPoint
{
	POSTGAMMA_INITDB_FAULT_NONE = 0,
	POSTGAMMA_INITDB_FAULT_TEMPORARY_CREATED,
	POSTGAMMA_INITDB_FAULT_BOOTSTRAP_COMPLETE,
	POSTGAMMA_INITDB_FAULT_POST_BOOTSTRAP_COMPLETE,
	POSTGAMMA_INITDB_FAULT_INITDB_COMPLETE,
	POSTGAMMA_INITDB_FAULT_PUBLISH_READY,
	POSTGAMMA_INITDB_FAULT_PUBLISHED,
	POSTGAMMA_INITDB_FAULT_OWNER_REMOVED,
	POSTGAMMA_INITDB_FAULT_POINT_COUNT
} PostgammaInitdbFaultPoint;

typedef enum PostgammaInitdbPhase
{
	POSTGAMMA_INITDB_PHASE_NONE = 0,
	POSTGAMMA_INITDB_PHASE_CHECK,
	POSTGAMMA_INITDB_PHASE_BOOTSTRAP,
	POSTGAMMA_INITDB_PHASE_POST_BOOTSTRAP,
	POSTGAMMA_INITDB_PHASE_COUNT
} PostgammaInitdbPhase;

typedef int (*PostgammaInitdbFaultCheck) (
	void *context, PostgammaInitdbFaultPoint point);

typedef struct PostgammaInitdbFaultProvider
{
	uint32_t	struct_size;
	void	   *context;
	PostgammaInitdbFaultCheck check;
} PostgammaInitdbFaultProvider;

#define POSTGAMMA_INITDB_FAULT_PROVIDER_INIT \
	{sizeof(PostgammaInitdbFaultProvider), NULL, NULL}

typedef struct PostgammaInitdbOptions
{
	uint32_t	struct_size;
	uint64_t	generation;
	uint64_t	deadline_ns;
	const char *data_directory;
	const char *executable_path;
	const char *resource_root;
	const char *username;
	const PostgammaInitdbSetting *settings;
	size_t		setting_count;
	mode_t		logical_umask;
	const PostgammaInitdbFaultProvider *faults;
	bool		require_missing;
} PostgammaInitdbOptions;

#define POSTGAMMA_INITDB_OPTIONS_INIT \
	{sizeof(PostgammaInitdbOptions), UINT64_C(0), UINT64_C(0), \
	 NULL, NULL, NULL, NULL, \
	 NULL, 0, 0077, NULL, false}

typedef struct PostgammaInitdbResult
{
	uint32_t	struct_size;
	int			status;
	int			postgres_exit_code;
	bool		created;
	char		phase[32];
	char		message[256];
} PostgammaInitdbResult;

#define POSTGAMMA_INITDB_RESULT_INIT \
	{sizeof(PostgammaInitdbResult), 0, 0, false, "validate", ""}

int postgamma_initdb_create(
	const PostgammaInitdbOptions *options,
	PostgammaInitdbResult *result);
int postgamma_initdb_create_locked(
	const PostgammaInitdbOptions *options,
	PostgammaInitdbResult *result,
	PostgammaDataDirectoryLock **data_lock);


#ifdef __cplusplus
}
#endif


#endif /* POSTGAMMA_PRIVATE_INITDB_HOST_H */
