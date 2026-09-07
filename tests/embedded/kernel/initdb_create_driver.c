#define _POSIX_C_SOURCE 200809L

#include "postgamma/private/initdb_host.h"
#include "postgamma/private/postgres_static_modules.h"
#include "postgamma/static_module_provider.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


#define TEST_PATH_CAPACITY 4096


typedef struct CreateThread
{
	_Atomic uint32_t *ready;
	_Atomic bool *start;
	const char *data_directory;
	const char *executable_path;
	const char *resource_root;
	uint64_t	generation;
	PostgammaInitdbResult result;
	int			status;
} CreateThread;

typedef struct FaultContext
{
	PostgammaInitdbFaultPoint target;
	bool		pause_at_target;
} FaultContext;


static void *run_create(void *argument);
static int run_single(
	const char *mode, const char *data_directory,
	const char *executable_path, const char *resource_root);
static int run_concurrent(
	const char *data_directory, const char *executable_path,
	const char *resource_root);
static int run_concurrent_independent(
	const char *root, const char *executable_path,
	const char *resource_root);
static int run_concurrent_paths(
	const char *mode, const char *data_directories[2],
	const char *executable_path, const char *resource_root,
	int expected_created_count);
static int run_lock_timeout(
	const char *data_directory, const char *executable_path,
	const char *resource_root);
static int run_lock_handoff(
	const char *data_directory, const char *executable_path,
	const char *resource_root);
static int run_fault(
	PostgammaInitdbFaultPoint point, bool pause_at_target,
	const char *data_directory, const char *executable_path,
	const char *resource_root);
static int inject_fault(
	void *argument, PostgammaInitdbFaultPoint point);
static const char *fault_point_name(PostgammaInitdbFaultPoint point);
static PostgammaInitdbFaultPoint parse_fault_point(const char *mode);


int
main(int argument_count, char **arguments)
{
	PostgammaInitdbFaultPoint fault_point;
	int			status;

	if (argument_count != 5)
	{
		fprintf(stderr,
			"usage: %s MODE DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT\n",
			arguments[0]);
		return 2;
	}
	status = postgamma_static_module_provider_install(
		postgamma_product_static_module_provider());
	if (status != 0)
	{
		fprintf(stderr, "could not install static module provider: %d\n", status);
		return 1;
	}
	if (strcmp(arguments[1], "create") == 0 ||
		strcmp(arguments[1], "existing") == 0 ||
		strcmp(arguments[1], "contended") == 0 ||
		strcmp(arguments[1], "reject") == 0 ||
		strcmp(arguments[1], "reject-umask") == 0 ||
		strcmp(arguments[1], "host-stream") == 0)
		return run_single(
			arguments[1], arguments[2], arguments[3], arguments[4]);
	if (strcmp(arguments[1], "concurrent") == 0)
		return run_concurrent(arguments[2], arguments[3], arguments[4]);
	if (strcmp(arguments[1], "concurrent-independent") == 0)
		return run_concurrent_independent(
			arguments[2], arguments[3], arguments[4]);
	if (strcmp(arguments[1], "lock-timeout") == 0)
		return run_lock_timeout(
			arguments[2], arguments[3], arguments[4]);
	if (strcmp(arguments[1], "lock-handoff") == 0)
		return run_lock_handoff(
			arguments[2], arguments[3], arguments[4]);
	fault_point = parse_fault_point(arguments[1]);
	if (fault_point != POSTGAMMA_INITDB_FAULT_NONE)
		return run_fault(
			fault_point, strncmp(arguments[1], "crash-", 6) == 0,
			arguments[2], arguments[3], arguments[4]);
	fprintf(stderr, "unknown initdb create mode: %s\n", arguments[1]);
	return 2;
}


static void *
run_create(void *argument)
{
	CreateThread *thread = argument;
	PostgammaInitdbOptions options = POSTGAMMA_INITDB_OPTIONS_INIT;

	thread->result = (PostgammaInitdbResult) POSTGAMMA_INITDB_RESULT_INIT;
	options.generation = thread->generation;
	options.data_directory = thread->data_directory;
	options.executable_path = thread->executable_path;
	options.resource_root = thread->resource_root;
	options.username = "postgamma";
	(void) atomic_fetch_add_explicit(
		thread->ready, UINT32_C(1), memory_order_release);
	while (!atomic_load_explicit(thread->start, memory_order_acquire))
		sched_yield();
	thread->status = postgamma_initdb_create(&options, &thread->result);
	return NULL;
}


static int
run_single(
	const char *mode, const char *data_directory,
	const char *executable_path, const char *resource_root)
{
	PostgammaInitdbOptions options = POSTGAMMA_INITDB_OPTIONS_INIT;
	PostgammaInitdbResult result = POSTGAMMA_INITDB_RESULT_INIT;
	char		host_buffer[4096];
	struct stat stream_status;
	FILE	   *host_stream = NULL;
	bool		host_stream_test = strcmp(mode, "host-stream") == 0;
	bool		expect_created = strcmp(mode, "create") == 0 || host_stream_test;
	bool		accept_either = strcmp(mode, "contended") == 0;
	bool		expect_rejected = strcmp(mode, "reject") == 0 ||
		strcmp(mode, "reject-umask") == 0;
	int			status;

	if (host_stream_test)
	{
		host_stream = tmpfile();
		if (host_stream == NULL ||
			setvbuf(host_stream, host_buffer, _IOFBF, sizeof(host_buffer)) != 0 ||
			fputs("host-owned-buffer", host_stream) == EOF ||
			fstat(fileno(host_stream), &stream_status) != 0 ||
			stream_status.st_size != 0)
		{
			fprintf(stderr, "could not prepare the host stream isolation probe\n");
			if (host_stream != NULL)
				(void) fclose(host_stream);
			return 1;
		}
	}
	options.generation = UINT64_C(8101);
	options.data_directory = data_directory;
	options.executable_path = executable_path;
	options.resource_root = resource_root;
	options.username = "postgamma";
	if (strcmp(mode, "reject-umask") == 0)
		options.logical_umask = 0027;
	status = postgamma_initdb_create(&options, &result);
	if (host_stream_test &&
		(fstat(fileno(host_stream), &stream_status) != 0 ||
		 stream_status.st_size != 0))
	{
		fprintf(stderr,
			"in-process initdb flushed a host-owned stdio stream\n");
		(void) fclose(host_stream);
		return 1;
	}
	if (host_stream != NULL && fclose(host_stream) != 0)
	{
		fprintf(stderr, "could not close the host stream isolation probe\n");
		return 1;
	}
	if (expect_rejected)
	{
		if (status == 0 || result.status == 0 || result.created)
		{
			fprintf(stderr,
				"initdb rejection unexpectedly succeeded: status=%d result=%d "
				"created=%d phase=%s\n",
				status, result.status, result.created, result.phase);
			return 1;
		}
		printf(
			"POSTGAMMA_INITDB mode=%s created=false status=%d "
			"phase=%s\n",
			mode, status, result.phase);
		return 0;
	}
	if (status != 0 || result.status != 0 ||
		(!accept_either && result.created != expect_created))
	{
		fprintf(stderr,
			"initdb %s failed: status=%d result=%d pg=%d created=%d "
			"phase=%s message=%s\n",
			mode, status, result.status, result.postgres_exit_code,
			result.created, result.phase, result.message);
		return 1;
	}
	printf(
		"POSTGAMMA_INITDB mode=%s created=%s phase=%s\n",
		mode, result.created ? "true" : "false", result.phase);
	return 0;
}


static int
run_concurrent(
	const char *data_directory, const char *executable_path,
	const char *resource_root)
{
	const char *data_directories[2] =
	{
		data_directory,
		data_directory,
	};

	return run_concurrent_paths(
		"concurrent", data_directories, executable_path, resource_root, 1);
}


static int
run_concurrent_independent(
	const char *root, const char *executable_path,
	const char *resource_root)
{
	char		parent_a[TEST_PATH_CAPACITY];
	char		parent_b[TEST_PATH_CAPACITY];
	char		target_a[TEST_PATH_CAPACITY];
	char		target_b[TEST_PATH_CAPACITY];
	const char *data_directories[2] = {target_a, target_b};
	int			count;

	if (mkdir(root, 0700) != 0 && errno != EEXIST)
		return 1;
	count = snprintf(parent_a, sizeof(parent_a), "%s/parent-a", root);
	if (count < 0 || (size_t) count >= sizeof(parent_a))
		return 1;
	count = snprintf(parent_b, sizeof(parent_b), "%s/parent-b", root);
	if (count < 0 || (size_t) count >= sizeof(parent_b))
		return 1;
	if ((mkdir(parent_a, 0700) != 0 && errno != EEXIST) ||
		(mkdir(parent_b, 0700) != 0 && errno != EEXIST))
		return 1;
	count = snprintf(target_a, sizeof(target_a), "%s/cluster", parent_a);
	if (count < 0 || (size_t) count >= sizeof(target_a))
		return 1;
	count = snprintf(target_b, sizeof(target_b), "%s/cluster", parent_b);
	if (count < 0 || (size_t) count >= sizeof(target_b))
		return 1;
	return run_concurrent_paths(
		"concurrent-independent", data_directories,
		executable_path, resource_root, 2);
}


static int
run_concurrent_paths(
	const char *mode, const char *data_directories[2],
	const char *executable_path, const char *resource_root,
	int expected_created_count)
{
	_Atomic uint32_t ready = UINT32_C(0);
	_Atomic bool start = false;
	pthread_t	threads[2];
	CreateThread states[2];
	size_t		created_threads = 0;
	int			created_count = 0;
	int			status;

	memset(states, 0, sizeof(states));
	for (size_t index = 0; index < 2; index++)
	{
		states[index].ready = &ready;
		states[index].start = &start;
		states[index].data_directory = data_directories[index];
		states[index].executable_path = executable_path;
		states[index].resource_root = resource_root;
		states[index].generation = UINT64_C(8201) + index;
		status = pthread_create(
			&threads[index], NULL, run_create, &states[index]);
		if (status != 0)
		{
			atomic_store_explicit(&start, true, memory_order_release);
			for (size_t joined = 0; joined < created_threads; joined++)
				(void) pthread_join(threads[joined], NULL);
			return 1;
		}
		created_threads++;
	}
	while (atomic_load_explicit(&ready, memory_order_acquire) != UINT32_C(2))
		sched_yield();
	atomic_store_explicit(&start, true, memory_order_release);
	for (size_t index = 0; index < 2; index++)
	{
		status = pthread_join(threads[index], NULL);
		if (status != 0 || states[index].status != 0 ||
			states[index].result.status != 0)
		{
			fprintf(stderr,
				"concurrent initdb worker %zu failed: join=%d status=%d "
				"result=%d phase=%s message=%s\n",
				index, status, states[index].status,
				states[index].result.status, states[index].result.phase,
				states[index].result.message);
			return 1;
		}
		if (states[index].result.created)
			created_count++;
	}
	if (created_count != expected_created_count)
	{
		fprintf(stderr,
			"%s initdb expected %d creators, observed %d\n",
			mode, expected_created_count, created_count);
		return 1;
	}
	printf(
		"POSTGAMMA_INITDB mode=%s created_count=%d "
		"phase=complete\n",
		mode, created_count);
	return 0;
}


static int
run_lock_timeout(
	const char *data_directory, const char *executable_path,
	const char *resource_root)
{
	PostgammaInitdbOptions options = POSTGAMMA_INITDB_OPTIONS_INIT;
	PostgammaInitdbResult result = POSTGAMMA_INITDB_RESULT_INIT;
	char		lock_path[TEST_PATH_CAPACITY];
	const char *base = strrchr(data_directory, '/');
	struct timespec now;
	int			descriptor;
	int			count;
	int			status;

	if (base == NULL || base == data_directory || base[1] == '\0')
		return 1;
	count = snprintf(
		lock_path, sizeof(lock_path), "%.*s/.%s.postgamma-create.lock",
		(int) (base - data_directory), data_directory, base + 1);
	if (count < 0 || (size_t) count >= sizeof(lock_path))
		return 1;
	descriptor = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (descriptor < 0 || flock(descriptor, LOCK_EX) != 0)
	{
		if (descriptor >= 0)
			(void) close(descriptor);
		return 1;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
	{
		(void) close(descriptor);
		return 1;
	}
	options.generation = UINT64_C(8401);
	options.deadline_ns =
		(uint64_t) now.tv_sec * UINT64_C(1000000000) +
		(uint64_t) now.tv_nsec + UINT64_C(100000000);
	options.data_directory = data_directory;
	options.executable_path = executable_path;
	options.resource_root = resource_root;
	options.username = "postgamma";
	status = postgamma_initdb_create(&options, &result);
	(void) flock(descriptor, LOCK_UN);
	(void) close(descriptor);
	if (status != ETIMEDOUT || result.status != ETIMEDOUT || result.created)
	{
		fprintf(stderr,
			"creation lock deadline failed: status=%d result=%d created=%d "
			"phase=%s message=%s\n",
			status, result.status, result.created, result.phase, result.message);
		return 1;
	}
	printf(
		"POSTGAMMA_INITDB mode=lock-timeout created=false status=%d "
		"phase=%s\n",
		status, result.phase);
	return 0;
}


static int
run_lock_handoff(
	const char *data_directory, const char *executable_path,
	const char *resource_root)
{
	PostgammaInitdbOptions first_options = POSTGAMMA_INITDB_OPTIONS_INIT;
	PostgammaInitdbOptions second_options = POSTGAMMA_INITDB_OPTIONS_INIT;
	PostgammaInitdbResult first_result = POSTGAMMA_INITDB_RESULT_INIT;
	PostgammaInitdbResult second_result = POSTGAMMA_INITDB_RESULT_INIT;
	PostgammaDataDirectoryLock *first_lock = NULL;
	PostgammaDataDirectoryLock *second_lock = NULL;
	int			status;
	int			second_status;

	first_options.generation = UINT64_C(8501);
	first_options.data_directory = data_directory;
	first_options.executable_path = executable_path;
	first_options.resource_root = resource_root;
	first_options.username = "postgamma";
	status = postgamma_initdb_create_locked(
		&first_options, &first_result, &first_lock);
	if (status != 0 || first_result.status != 0 || !first_result.created ||
		first_lock == NULL || postgamma_data_directory_lock_active_count() != 1)
	{
		fprintf(stderr,
			"creation lock handoff failed: status=%d result=%d created=%d "
			"lock=%p phase=%s message=%s\n",
			status, first_result.status, first_result.created, (void *) first_lock,
			first_result.phase, first_result.message);
		if (first_lock != NULL)
			(void) postgamma_data_directory_lock_release(first_lock);
		return 1;
	}
	second_options = first_options;
	second_options.generation = UINT64_C(8502);
	second_status = postgamma_initdb_create_locked(
		&second_options, &second_result, &second_lock);
	status = postgamma_data_directory_lock_release(first_lock);
	if (second_status != EBUSY || second_result.status != EBUSY ||
		second_result.created || second_lock != NULL || status != 0 ||
		postgamma_data_directory_lock_active_count() != 0)
	{
		fprintf(stderr,
			"creation lock handoff contention failed: status=%d result=%d "
			"created=%d lock=%p release=%d active=%zu phase=%s message=%s\n",
			second_status, second_result.status, second_result.created,
			(void *) second_lock, status,
			postgamma_data_directory_lock_active_count(), second_result.phase,
			second_result.message);
		if (second_lock != NULL)
			(void) postgamma_data_directory_lock_release(second_lock);
		return 1;
	}
	printf(
		"POSTGAMMA_INITDB mode=lock-handoff created=true "
		"contender_status=%d active_locks=0 phase=complete\n",
		second_status);
	return 0;
}


static int
run_fault(
	PostgammaInitdbFaultPoint point, bool pause_at_target,
	const char *data_directory, const char *executable_path,
	const char *resource_root)
{
	PostgammaInitdbFaultProvider faults =
		POSTGAMMA_INITDB_FAULT_PROVIDER_INIT;
	PostgammaInitdbOptions options = POSTGAMMA_INITDB_OPTIONS_INIT;
	PostgammaInitdbResult result = POSTGAMMA_INITDB_RESULT_INIT;
	FaultContext context;
	struct stat target_status;
	bool		expect_published =
		point == POSTGAMMA_INITDB_FAULT_PUBLISHED ||
		point == POSTGAMMA_INITDB_FAULT_OWNER_REMOVED;
	int			status;

	context.target = point;
	context.pause_at_target = pause_at_target;
	faults.context = &context;
	faults.check = inject_fault;
	options.generation = UINT64_C(8301) + (uint64_t) point;
	options.data_directory = data_directory;
	options.executable_path = executable_path;
	options.resource_root = resource_root;
	options.username = "postgamma";
	options.faults = &faults;
	status = postgamma_initdb_create(&options, &result);
	if (pause_at_target)
	{
		fprintf(stderr, "crash pause unexpectedly returned: status=%d\n", status);
		return 1;
	}
	errno = 0;
	if (status != ECANCELED || result.status != ECANCELED ||
		result.created != expect_published ||
		(expect_published &&
		 (lstat(data_directory, &target_status) != 0 ||
		  !S_ISDIR(target_status.st_mode))) ||
		(!expect_published &&
		 (lstat(data_directory, &target_status) == 0 || errno != ENOENT)))
	{
		fprintf(stderr,
			"initdb fault violated its publication contract: point=%s status=%d "
			"result=%d "
			"created=%d phase=%s message=%s\n",
			fault_point_name(point), status, result.status, result.created,
			result.phase, result.message);
		return 1;
	}
	printf(
		"POSTGAMMA_INITDB mode=fault point=%s status=%d "
		"created=%s phase=%s\n",
		fault_point_name(point), status,
		expect_published ? "true" : "false", result.phase);
	return 0;
}


static int
inject_fault(void *argument, PostgammaInitdbFaultPoint point)
{
	FaultContext *context = argument;

	if (context == NULL || point != context->target)
		return 0;
	if (context->pause_at_target)
	{
		const struct timespec interval = {1, 0};

		printf(
			"POSTGAMMA_INITDB mode=crash-ready point=%s\n",
			fault_point_name(point));
		(void) fflush(stdout);
		for (;;)
			(void) nanosleep(&interval, NULL);
	}
	return ECANCELED;
}


static const char *
fault_point_name(PostgammaInitdbFaultPoint point)
{
	switch (point)
	{
		case POSTGAMMA_INITDB_FAULT_TEMPORARY_CREATED:
			return "temporary-created";
		case POSTGAMMA_INITDB_FAULT_BOOTSTRAP_COMPLETE:
			return "bootstrap-complete";
		case POSTGAMMA_INITDB_FAULT_POST_BOOTSTRAP_COMPLETE:
			return "post-bootstrap-complete";
		case POSTGAMMA_INITDB_FAULT_INITDB_COMPLETE:
			return "initdb-complete";
		case POSTGAMMA_INITDB_FAULT_PUBLISH_READY:
			return "publish-ready";
		case POSTGAMMA_INITDB_FAULT_PUBLISHED:
			return "published";
		case POSTGAMMA_INITDB_FAULT_OWNER_REMOVED:
			return "owner-removed";
		case POSTGAMMA_INITDB_FAULT_NONE:
		case POSTGAMMA_INITDB_FAULT_POINT_COUNT:
			break;
	}
	return "unknown";
}


static PostgammaInitdbFaultPoint
parse_fault_point(const char *mode)
{
	if (strcmp(mode, "fault-temporary") == 0)
		return POSTGAMMA_INITDB_FAULT_TEMPORARY_CREATED;
	if (strcmp(mode, "fault-bootstrap") == 0)
		return POSTGAMMA_INITDB_FAULT_BOOTSTRAP_COMPLETE;
	if (strcmp(mode, "fault-post-bootstrap") == 0)
		return POSTGAMMA_INITDB_FAULT_POST_BOOTSTRAP_COMPLETE;
	if (strcmp(mode, "fault-initdb") == 0)
		return POSTGAMMA_INITDB_FAULT_INITDB_COMPLETE;
	if (strcmp(mode, "fault-publish") == 0)
		return POSTGAMMA_INITDB_FAULT_PUBLISH_READY;
	if (strcmp(mode, "fault-published") == 0)
		return POSTGAMMA_INITDB_FAULT_PUBLISHED;
	if (strcmp(mode, "fault-owner-removed") == 0)
		return POSTGAMMA_INITDB_FAULT_OWNER_REMOVED;
	if (strcmp(mode, "crash-temporary") == 0)
		return POSTGAMMA_INITDB_FAULT_TEMPORARY_CREATED;
	if (strcmp(mode, "crash-bootstrap") == 0)
		return POSTGAMMA_INITDB_FAULT_BOOTSTRAP_COMPLETE;
	if (strcmp(mode, "crash-post-bootstrap") == 0)
		return POSTGAMMA_INITDB_FAULT_POST_BOOTSTRAP_COMPLETE;
	if (strcmp(mode, "crash-initdb") == 0)
		return POSTGAMMA_INITDB_FAULT_INITDB_COMPLETE;
	if (strcmp(mode, "crash-publish") == 0)
		return POSTGAMMA_INITDB_FAULT_PUBLISH_READY;
	if (strcmp(mode, "crash-published") == 0)
		return POSTGAMMA_INITDB_FAULT_PUBLISHED;
	if (strcmp(mode, "crash-owner-removed") == 0)
		return POSTGAMMA_INITDB_FAULT_OWNER_REMOVED;
	return POSTGAMMA_INITDB_FAULT_NONE;
}
