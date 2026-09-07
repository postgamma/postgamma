#define _GNU_SOURCE

#include "postgamma/private/data_directory_lock.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


#define LOCK_REPETITIONS 100


static int probe_directory(const char *path, bool expect_busy);
static void run_external_probe(
	const char *program, const char *path, bool expect_busy);
static void read_lock_metadata(const char *path);


static int
probe_directory(const char *path, bool expect_busy)
{
	PostgammaDataDirectoryLockOptions options =
		POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT;
	PostgammaDataDirectoryLock *lock = NULL;
	int			status;

	options.generation = UINT64_C(999);
	options.path = path;
	status = postgamma_data_directory_lock_acquire(&options, &lock);
	if (expect_busy)
		return status == EBUSY && lock == NULL ? EXIT_SUCCESS : EXIT_FAILURE;
	if (status != 0 || lock == NULL)
		return EXIT_FAILURE;
	return postgamma_data_directory_lock_release(lock) == 0 ?
		EXIT_SUCCESS : EXIT_FAILURE;
}


static void
run_external_probe(const char *program, const char *path, bool expect_busy)
{
	pid_t		child = fork();
	int			wait_status;

	assert(child >= 0);
	if (child == 0)
	{
		execl(program, program,
			  expect_busy ? "--expect-busy" : "--expect-free",
			  path, (char *) NULL);
		_exit(127);
	}
	assert(waitpid(child, &wait_status, 0) == child);
	assert(WIFEXITED(wait_status));
	assert(WEXITSTATUS(wait_status) == EXIT_SUCCESS);
}


static void
read_lock_metadata(const char *path)
{
	char		buffer[256];
	ssize_t		length;
	int			descriptor = open(path, O_RDONLY);

	assert(descriptor >= 0);
	length = read(descriptor, buffer, sizeof(buffer) - 1);
	assert(length > 0);
	assert(close(descriptor) == 0);
	buffer[length] = '\0';
	assert(strstr(buffer, "postgamma_embedded_lock_v1") != NULL);
	assert(strstr(buffer, "generation=71") != NULL);
	assert(strstr(buffer, "pid") == NULL);
}


int
main(int argc, char **argv)
{
	PostgammaDataDirectoryLockOptions options =
		POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT;
	PostgammaDataDirectoryLock *lock;
	PostgammaDataDirectoryLock *duplicate = NULL;
	PostgammaDataDirectoryLockTelemetry telemetry;
	char		temporary[] = "/tmp/postgamma-lock-XXXXXX";
	char		alias[sizeof(temporary) + 16];
	char		lock_path[sizeof(temporary) + 32];
	char		pid_path[sizeof(temporary) + 32];
	char	   *program;
	char	   *directory;
	char	   *cwd_before;
	char	   *cwd_after;

	if (argc == 3 && strcmp(argv[1], "--expect-busy") == 0)
		return probe_directory(argv[2], true);
	if (argc == 3 && strcmp(argv[1], "--expect-free") == 0)
		return probe_directory(argv[2], false);
	assert(argc == 1);
	program = realpath(argv[0], NULL);
	assert(program != NULL);
	cwd_before = getcwd(NULL, 0);
	assert(cwd_before != NULL);
	directory = mkdtemp(temporary);
	assert(directory != NULL);
	assert(snprintf(alias, sizeof(alias), "%s-alias", directory) > 0);
	assert(symlink(directory, alias) == 0);
	assert(snprintf(
		lock_path, sizeof(lock_path), "%s/%s", directory,
		POSTGAMMA_DATA_DIRECTORY_LOCK_FILE) > 0);
	assert(snprintf(
		pid_path, sizeof(pid_path), "%s/postmaster.pid", directory) > 0);

	options.generation = UINT64_C(71);
	options.path = directory;
	assert(postgamma_data_directory_lock_acquire(&options, &lock) == 0);
	assert(postgamma_data_directory_lock_active_count() == 1);
	assert(postgamma_data_directory_lock_telemetry(lock, &telemetry) == 0);
	assert(telemetry.generation == UINT64_C(71));
	assert(telemetry.device != 0);
	assert(telemetry.inode != 0);
	assert(telemetry.directory_descriptor >= 0);
	assert(telemetry.lock_descriptor >= 0);
	assert(strcmp(telemetry.canonical_path, directory) == 0);
	assert(strcmp(telemetry.lock_path, lock_path) == 0);
	assert(access(lock_path, F_OK) == 0);
	assert(access(pid_path, F_OK) != 0);
	assert(errno == ENOENT);
	read_lock_metadata(lock_path);

	options.generation = UINT64_C(72);
	options.path = alias;
	assert(postgamma_data_directory_lock_acquire(
		&options, &duplicate) == EBUSY);
	assert(duplicate == NULL);
	run_external_probe(program, directory, true);
	assert(postgamma_data_directory_lock_release(lock) == 0);
	assert(postgamma_data_directory_lock_active_count() == 0);
	run_external_probe(program, directory, false);

	options.path = directory;
	for (uint64_t iteration = 0; iteration < LOCK_REPETITIONS; iteration++)
	{
		options.generation = UINT64_C(1000) + iteration;
		assert(postgamma_data_directory_lock_acquire(
			&options, &lock) == 0);
		assert(postgamma_data_directory_lock_release(lock) == 0);
	}
	assert(postgamma_data_directory_lock_active_count() == 0);
	assert(access(pid_path, F_OK) != 0);
	assert(errno == ENOENT);
	cwd_after = getcwd(NULL, 0);
	assert(cwd_after != NULL);
	assert(strcmp(cwd_before, cwd_after) == 0);
	free(cwd_after);
	free(cwd_before);
	free(program);
	assert(unlink(alias) == 0);
	assert(unlink(lock_path) == 0);
	assert(rmdir(directory) == 0);
	return 0;
}
