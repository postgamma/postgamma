/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/file_platform.h"
#include "postgamma/private/data_directory_lock.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define main postgamma_runtime_path_test
#include "../../runtime/test_path_runtime.c"
#undef main

static void *compete(void *argument)
{
    PostgammaDataDirectoryLockOptions options = POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT;
    PostgammaDataDirectoryLock *lock = NULL;
    options.path = argument;
    options.generation = 200;
    assert(postgamma_data_directory_lock_acquire(&options, &lock) == EBUSY);
    assert(lock == NULL);
    return NULL;
}

void postgamma_runtime_filesystem_test(void)
{
    char template[] = "/tmp/postgamma-identity-XXXXXX";
    char *root = mkdtemp(template);
    char alias[256], moved[256], metadata[256];
    PostgammaDataDirectoryLockOptions options = POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT;
    PostgammaDataDirectoryLock *lock = NULL, *replacement = NULL;
    PostgammaDataDirectoryLockTelemetry telemetry;
    pthread_t thread;
    int owner = 0, other = 0, fd;
    assert(root);
    snprintf(alias, sizeof(alias), "%s-alias", root);
    snprintf(moved, sizeof(moved), "%s-moved", root);
    assert(symlink(root, alias) == 0);
    options.path = root;
    options.generation = 100;
    assert(postgamma_data_directory_lock_acquire(&options, &lock) == 0);
    assert(postgamma_data_directory_lock_telemetry(lock, &telemetry) == 0);
    assert(telemetry.device == 0 && telemetry.inode == 0 && telemetry.lock_command == 0);
    fd = telemetry.directory_descriptor;
    assert(postgamma_platform_directory_owned(fd, 100));
    assert(!postgamma_platform_directory_owned(fd, 101));
    assert(pthread_create(&thread, NULL, compete, alias) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(rename(root, moved) == 0);
    assert(postgamma_platform_directory_match(fd, AT_FDCWD, moved) == 0);
    assert(pthread_create(&thread, NULL, compete, moved) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(mkdir(root, 0700) == 0);
    assert(postgamma_platform_directory_match(fd, AT_FDCWD, root) == ESTALE);
    PostgammaPathRuntimeOptions paths = POSTGAMMA_PATH_RUNTIME_OPTIONS_INIT;
    PostgammaPathRuntime *path = NULL;
    paths.generation = 100;
    paths.data_directory_fd = fd;
    paths.canonical_data_directory = root;
    assert(postgamma_path_runtime_create(&path, &paths) == ESTALE);
    assert(path == NULL);
    options.generation = 101;
    assert(postgamma_data_directory_lock_acquire(&options, &replacement) == 0);
    assert(postgamma_data_directory_lock_active_count() == 2);
    assert(postgamma_data_directory_lock_release(replacement) == 0);
    assert(postgamma_data_directory_lock_release(lock) == 0);
    options.path = moved;
    options.generation = 102;
    assert(postgamma_data_directory_lock_acquire(&options, &lock) == 0);
    assert(postgamma_data_directory_lock_release(lock) == 0);
    fd = open(moved, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    assert(postgamma_platform_directory_claim(fd, 103, &owner) == 0);
    assert(postgamma_platform_file_release(104, &owner) == ESTALE);
    assert(postgamma_platform_file_release(103, &other) == ENOENT);
    assert(postgamma_platform_directory_owned(fd, 103));
    assert(postgamma_platform_file_release(103, &owner) == 0);
    assert(!postgamma_platform_directory_owned(fd, 103));
    assert(close(fd) == 0);
    snprintf(metadata, sizeof(metadata), "%s/creation-lock", root);
    fd = open(metadata, O_CREAT | O_RDWR, 0600);
    int duplicate = open(metadata, O_RDWR);
    assert(fd >= 0 && duplicate >= 0);
    assert(postgamma_platform_directory_claim(fd, 104, &owner) == ENOTDIR);
    assert(postgamma_platform_file_claim(fd, 104, &owner) == 0);
    assert(postgamma_platform_file_claim(duplicate, 105, &other) == EBUSY);
    assert(close(fd) == 0);
    assert(postgamma_platform_file_claim(duplicate, 105, &other) == EBUSY);
    assert(postgamma_platform_file_release(104, &owner) == 0);
    assert(postgamma_platform_file_claim(duplicate, 105, &other) == 0);
    assert(postgamma_platform_file_release(105, &other) == 0);
    assert(close(duplicate) == 0 && unlink(metadata) == 0);
    snprintf(metadata, sizeof(metadata), "%s/.postgamma.lock", root);
    assert(unlink(metadata) == 0);
    snprintf(metadata, sizeof(metadata), "%s/.postgamma.lock", moved);
    assert(unlink(metadata) == 0);
    assert(unlink(alias) == 0 && rmdir(root) == 0 && rmdir(moved) == 0);
    assert(postgamma_data_directory_lock_active_count() == 0);
    assert(postgamma_runtime_path_test() == 0);
}
