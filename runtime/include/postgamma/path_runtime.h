/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * path_runtime.h
 *    Instance-owned virtual working-directory and file-path runtime.
 *
 * Relative PostgreSQL server paths are resolved against an open data-directory
 * descriptor.  The host process working directory and umask are never changed.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_PATH_RUNTIME_H
#define POSTGAMMA_PATH_RUNTIME_H

#include <stdbool.h>
#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>


#ifdef __cplusplus
extern "C" {
#endif


typedef struct PostgammaPathRuntime PostgammaPathRuntime;

typedef struct PostgammaPathRuntimeOptions
{
	uint32_t	struct_size;
	uint64_t	generation;
	int			data_directory_fd;
	const char *canonical_data_directory;
	mode_t		logical_umask;
	const char *canonical_resource_root;
} PostgammaPathRuntimeOptions;

#define POSTGAMMA_PATH_RUNTIME_OPTIONS_INIT \
	{sizeof(PostgammaPathRuntimeOptions), UINT64_C(0), -1, NULL, 0077, NULL}

typedef struct PostgammaPathRuntimeTelemetry
{
	uint64_t	generation;
	int			data_directory_fd;
	mode_t		logical_umask;
	uint64_t	relative_operations;
	uint64_t	absolute_operations;
	uint64_t	virtual_chdir_calls;
	uint64_t	logical_umask_calls;
	uint64_t	resource_operations;
	const char *canonical_data_directory;
	const char *canonical_resource_root;
} PostgammaPathRuntimeTelemetry;

int postgamma_path_runtime_create(
	PostgammaPathRuntime **runtime,
	const PostgammaPathRuntimeOptions *options);
int postgamma_path_runtime_destroy(PostgammaPathRuntime *runtime);
uint64_t postgamma_path_runtime_generation(
	const PostgammaPathRuntime *runtime);
int postgamma_path_runtime_telemetry(
	PostgammaPathRuntime *runtime,
	PostgammaPathRuntimeTelemetry *telemetry);
bool postgamma_path_resource_name_is_valid(const char *logical_path);
int postgamma_path_resource_open(
	PostgammaPathRuntime *runtime, const char *logical_path, int flags);

int postgamma_path_open(const char *path, int flags, ...);
int postgamma_path_openat(int directory_fd, const char *path, int flags, ...);
FILE *postgamma_path_fopen(const char *path, const char *mode);
int postgamma_path_stat(const char *path, struct stat *status_buffer);
int postgamma_path_lstat(const char *path, struct stat *status_buffer);
int postgamma_path_access(const char *path, int mode);
int postgamma_path_unlink(const char *path);
int postgamma_path_remove(const char *path);
int postgamma_path_rename(const char *old_path, const char *new_path);
int postgamma_path_link(const char *old_path, const char *new_path);
int postgamma_path_symlink(const char *target, const char *link_path);
ssize_t postgamma_path_readlink(
	const char *path, char *buffer, size_t buffer_size);
int postgamma_path_mkdir(const char *path, mode_t mode);
int postgamma_path_rmdir(const char *path);
int postgamma_path_chmod(const char *path, mode_t mode);
int postgamma_path_chown(const char *path, uid_t owner, gid_t group);
int postgamma_path_truncate(const char *path, off_t length);
DIR *postgamma_path_opendir(const char *path);
char *postgamma_path_realpath(const char *path, char *resolved_path);
int postgamma_path_chdir(const char *path);
char *postgamma_path_getcwd(char *buffer, size_t size);
mode_t postgamma_path_umask(mode_t mask);

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_PATH_RUNTIME_H */
