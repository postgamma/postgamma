#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>


#define TEST_CONNECTION_COUNT 1000
#define TEST_EXECUTOR_WORKER_COUNT 4
#define TEST_WAITER_COUNT 4
#define TEST_TIMEOUT_MS INT64_C(60000)
#define TEST_PARALLEL_ROWS 200000
#define TEST_REQUIRED_NOFILE ((rlim_t) 16384)


static int fail_status(const char *operation, pgm_status status, pgm_error *error);
static bool value_equals(
	const pgm_result *result, size_t row, size_t column, const char *expected);
static pgm_status execute_command(
	pgm_connection *connection, const char *sql, pgm_error **error);
static int count_native_threads(void);
static int count_open_file_descriptors(void);
static int prepare_file_descriptor_limit(rlim_t required, rlim_t *effective);
static void report_phase(const char *phase, size_t progress);


int
main(int argument_count, char **arguments)
{
	const pgm_setting settings[] =
	{
		{"max_connections", "1010"},
		{"shared_buffers", "16MB"},
		{"max_worker_processes", "4"},
		{"max_parallel_workers", "4"},
		{"autovacuum", "off"},
	};
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_instance *instance = NULL;
	pgm_connection **connections = NULL;
	pgm_request **requests = NULL;
	pgm_result  *result = NULL;
	pgm_error   *error = NULL;
	pgm_status	status = PGM_STATUS_OK;
	pgm_request_state request_state = PGM_REQUEST_PENDING;
	struct timespec settle = {0, 100000000L};
	int			threads_after_instance;
	int			threads_after_connections;
	int			threads_during_storm;
	int			threads_after_storm;
	int			fds_after_instance;
	int			fds_after_connections;
	rlim_t		nofile_soft_limit;
	bool		passed = false;

	if (argument_count != 5 || strcmp(arguments[4], "create") != 0)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT create\n",
			arguments[0]);
		return 2;
	}
	if (prepare_file_descriptor_limit(
			TEST_REQUIRED_NOFILE, &nofile_soft_limit) != 0)
	{
		fprintf(stderr, "could not prepare the session execution descriptor envelope\n");
		return 1;
	}
	connections = calloc(TEST_CONNECTION_COUNT, sizeof(*connections));
	requests = calloc(TEST_CONNECTION_COUNT, sizeof(*requests));
	if (connections == NULL || requests == NULL)
	{
		fprintf(stderr, "could not allocate session executor fixtures\n");
		goto cleanup;
	}
	instance_options.path = arguments[1];
	instance_options.create = UINT32_C(1);
	instance_options.executable_path = arguments[2];
	instance_options.resource_root = arguments[3];
	instance_options.settings = settings;
	instance_options.setting_count = sizeof(settings) / sizeof(settings[0]);
	instance_options.executor_worker_count =
		(uint32_t) TEST_EXECUTOR_WORKER_COUNT;
	instance_options.execution_queue_capacity = UINT32_C(1100);
	status = pgm_instance_open(&instance_options, &instance, &error);
	if (status != PGM_STATUS_OK)
		goto cleanup;
	report_phase("instance-ready", 0);
	threads_after_instance = count_native_threads();
	fds_after_instance = count_open_file_descriptors();
	if (threads_after_instance <= 0 || fds_after_instance <= 0)
	{
		status = PGM_STATUS_INTERNAL_ERROR;
		goto cleanup;
	}
	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-session-executor";
	for (size_t index = 0; index < TEST_CONNECTION_COUNT; index++)
	{
		status = pgm_connection_open(
			instance, &connection_options, &connections[index], &error);
		if (status != PGM_STATUS_OK)
			goto cleanup;
	}
	report_phase("connections-open", TEST_CONNECTION_COUNT);
	threads_after_connections = count_native_threads();
	fds_after_connections = count_open_file_descriptors();
	if (threads_after_connections <= 0 ||
		threads_after_connections > threads_after_instance + 2 ||
		threads_after_connections >= TEST_CONNECTION_COUNT / 10 ||
		fds_after_connections <= fds_after_instance)
	{
		fprintf(stderr,
			"idle sessions changed native threads from %d to %d\n",
			threads_after_instance, threads_after_connections);
		status = PGM_STATUS_INTERNAL_ERROR;
		goto cleanup;
	}

	for (size_t index = 0; index < TEST_CONNECTION_COUNT; index++)
	{
		status = pgm_execute_async(
			connections[index], "SELECT 1 FROM pg_sleep(0.05)",
			NULL, 0, UINT16_C(0),
			&requests[index], &error);
		if (status != PGM_STATUS_OK)
			goto cleanup;
	}
	report_phase("storm-submitted", TEST_CONNECTION_COUNT);
	threads_during_storm = count_native_threads();
	if (threads_during_storm <= 0 ||
		threads_during_storm > threads_after_instance + 2)
	{
		fprintf(stderr,
			"request storm changed native threads from %d to %d\n",
			threads_after_instance, threads_during_storm);
		status = PGM_STATUS_INTERNAL_ERROR;
		goto cleanup;
	}
	for (size_t index = 0; index < TEST_CONNECTION_COUNT; index++)
	{
		status = pgm_request_wait(
			requests[index], TEST_TIMEOUT_MS, &result, &error);
		if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "1"))
			goto cleanup;
		pgm_result_free(result);
		result = NULL;
		pgm_request_free(requests[index]);
		requests[index] = NULL;
		if ((index + 1) % 100 == 0)
			report_phase("storm-completed", index + 1);
	}
	report_phase("storm-finished", TEST_CONNECTION_COUNT);
	threads_after_storm = count_native_threads();
	if (threads_after_storm <= 0 ||
		threads_after_storm > threads_after_instance + 2)
	{
		status = PGM_STATUS_INTERNAL_ERROR;
		goto cleanup;
	}

	status = execute_command(
		connections[0],
		"CREATE TABLE postgamma_holder_progress(id int primary key, value int)",
		&error);
	if (status == PGM_STATUS_OK)
		report_phase("holder-table-created", 1);
	if (status == PGM_STATUS_OK)
		status = execute_command(
			connections[0],
			"INSERT INTO postgamma_holder_progress VALUES (1, 0)", &error);
	if (status == PGM_STATUS_OK)
		report_phase("holder-row-inserted", 1);
	if (status == PGM_STATUS_OK)
		status = execute_command(connections[0], "BEGIN", &error);
	if (status == PGM_STATUS_OK)
		report_phase("holder-transaction-begun", 1);
	if (status == PGM_STATUS_OK)
		status = execute_command(
			connections[0],
			"UPDATE postgamma_holder_progress SET value = value + 1 WHERE id = 1",
			&error);
	if (status != PGM_STATUS_OK)
		goto cleanup;
	report_phase("holder-locked", 1);
	for (size_t index = 0; index < TEST_WAITER_COUNT; index++)
	{
		status = pgm_execute_async(
			connections[index + 1],
			"UPDATE postgamma_holder_progress SET value = value + 1 WHERE id = 1",
			NULL, 0, UINT16_C(0), &requests[index], &error);
		if (status != PGM_STATUS_OK)
			goto cleanup;
	}
	report_phase("waiters-submitted", TEST_WAITER_COUNT);
	(void) nanosleep(&settle, NULL);
	status = execute_command(connections[0], "COMMIT", &error);
	if (status != PGM_STATUS_OK)
		goto cleanup;
	report_phase("holder-committed", 1);
	for (size_t index = 0; index < TEST_WAITER_COUNT; index++)
	{
		status = pgm_request_wait(
			requests[index], TEST_TIMEOUT_MS, &result, &error);
		if (status != PGM_STATUS_OK ||
			pgm_result_kind(result) != PGM_RESULT_COMMAND_OK)
			goto cleanup;
		pgm_result_free(result);
		result = NULL;
		pgm_request_free(requests[index]);
		requests[index] = NULL;
	}
	report_phase("waiters-finished", TEST_WAITER_COUNT);
	status = pgm_execute(
		connections[0],
		"SELECT value FROM postgamma_holder_progress WHERE id = 1",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "5"))
		goto cleanup;
	pgm_result_free(result);
	result = NULL;

	/*
	 * Explicit transactions retain their carriers in the first pooled
	 * implementation.  Exercise the documented capacity cliff, then prove
	 * that releasing one pinned carrier lets queued work continue.
	 */
	for (size_t index = 0; index < TEST_EXECUTOR_WORKER_COUNT; index++)
	{
		status = execute_command(connections[index], "BEGIN", &error);
		if (status != PGM_STATUS_OK)
			goto cleanup;
		report_phase("pinning-cliff-transaction-open", index + 1);
	}
	status = pgm_execute_async(
		connections[TEST_EXECUTOR_WORKER_COUNT], "SELECT 6202",
		NULL, 0, UINT16_C(0), &requests[0], &error);
	if (status != PGM_STATUS_OK)
		goto cleanup;
	(void) nanosleep(&settle, NULL);
	status = pgm_request_progress(requests[0], &request_state, &error);
	if (status != PGM_STATUS_OK || request_state != PGM_REQUEST_RUNNING)
	{
		fprintf(stderr,
			"request passed the fully pinned executor unexpectedly: state=%d\n",
			(int) request_state);
		if (status == PGM_STATUS_OK)
			status = PGM_STATUS_INTERNAL_ERROR;
		goto cleanup;
	}
	report_phase("pinning-cliff-queued", 1);
	status = execute_command(connections[0], "COMMIT", &error);
	if (status != PGM_STATUS_OK)
		goto cleanup;
	status = pgm_request_wait(
		requests[0], TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "6202"))
		goto cleanup;
	pgm_result_free(result);
	result = NULL;
	pgm_request_free(requests[0]);
	requests[0] = NULL;
	for (size_t index = 1; index < TEST_EXECUTOR_WORKER_COUNT; index++)
	{
		status = execute_command(connections[index], "COMMIT", &error);
		if (status != PGM_STATUS_OK)
			goto cleanup;
	}
	report_phase("pinning-cliff-released", TEST_EXECUTOR_WORKER_COUNT);

	status = execute_command(
		connections[0], "SET max_parallel_workers_per_gather = 4", &error);
	if (status == PGM_STATUS_OK)
		status = execute_command(
			connections[0], "SET min_parallel_table_scan_size = 0", &error);
	if (status == PGM_STATUS_OK)
		status = execute_command(
			connections[0], "SET parallel_setup_cost = 0", &error);
	if (status == PGM_STATUS_OK)
		status = execute_command(
			connections[0], "SET parallel_tuple_cost = 0", &error);
	if (status == PGM_STATUS_OK)
		status = execute_command(
			connections[0],
			"CREATE TABLE postgamma_parallel_budget AS "
			"SELECT generate_series(1, 200000)::bigint AS value", &error);
	if (status == PGM_STATUS_OK)
		status = execute_command(
			connections[0], "ANALYZE postgamma_parallel_budget", &error);
	if (status != PGM_STATUS_OK)
		goto cleanup;
	report_phase("parallel-fixture-ready", TEST_PARALLEL_ROWS);
	status = pgm_execute(
		connections[0], "SELECT sum(value) FROM postgamma_parallel_budget",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK ||
		!value_equals(result, 0, 0, "20000100000"))
		goto cleanup;
	pgm_result_free(result);
	result = NULL;
	report_phase("parallel-query-finished", 1);

	for (size_t index = 0; index < TEST_CONNECTION_COUNT; index++)
	{
		status = pgm_connection_close(
			connections[index], TEST_TIMEOUT_MS, &error);
		if (status != PGM_STATUS_OK)
			goto cleanup;
		connections[index] = NULL;
	}
	report_phase("connections-closed", TEST_CONNECTION_COUNT);
	status = pgm_instance_close(
		instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto cleanup;
	instance = NULL;
	passed = true;

cleanup:
	if (!passed && status != PGM_STATUS_OK)
		(void) fail_status("session executor gate", status, error);
	if (!passed)
		return 1;
	pgm_error_free(error);
	error = NULL;
	pgm_result_free(result);
	if (requests != NULL)
	{
		for (size_t index = 0; index < TEST_CONNECTION_COUNT; index++)
		{
			if (requests[index] != NULL)
				pgm_request_free(requests[index]);
		}
	}
	if (connections != NULL)
	{
		for (size_t index = 0; index < TEST_CONNECTION_COUNT; index++)
		{
			if (connections[index] != NULL)
			{
				(void) pgm_connection_close(
					connections[index], TEST_TIMEOUT_MS, NULL);
			}
		}
	}
	if (instance != NULL)
		(void) pgm_instance_close(
			instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, NULL);
	free(requests);
	free(connections);
	if (!passed)
		return 1;
	printf(
		"POSTGAMMA_SESSION_EXECUTOR connections=%d idle_thread_growth=%d "
		"idle_fd_growth=%d nofile_soft=%" PRIuMAX " "
		"request_thread_growth=%d request_storm=%d holder_progress=true "
		"saturated_waiters=%d pinning_cliff=true pinned_transactions=%d "
		"queued_at_cliff=true parallel_query=true executor_workers=%d "
		"request_threads=0 phase=closed\n",
		TEST_CONNECTION_COUNT,
		threads_after_connections - threads_after_instance,
		fds_after_connections - fds_after_instance,
		(uintmax_t) nofile_soft_limit,
		threads_during_storm - threads_after_instance,
		TEST_CONNECTION_COUNT, TEST_WAITER_COUNT,
		TEST_EXECUTOR_WORKER_COUNT, TEST_EXECUTOR_WORKER_COUNT);
	return 0;
}


static int
fail_status(const char *operation, pgm_status status, pgm_error *error)
{
	fprintf(stderr, "%s failed: status=%s sqlstate=%s message=%s detail=%s\n",
		operation, pgm_status_name(status), pgm_error_sqlstate(error),
		pgm_error_message(error), pgm_error_detail(error));
	return 1;
}


static bool
value_equals(
	const pgm_result *result, size_t row, size_t column, const char *expected)
{
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	size_t		expected_size = strlen(expected);

	return pgm_result_value(result, row, column, &value, NULL) ==
			PGM_STATUS_OK &&
		value.is_null == UINT16_C(0) && value.format == UINT16_C(0) &&
		value.size == expected_size &&
		memcmp(value.data, expected, expected_size) == 0;
}


static pgm_status
execute_command(
	pgm_connection *connection, const char *sql, pgm_error **error)
{
	pgm_result  *result = NULL;
	pgm_status	status = pgm_execute(
		connection, sql, NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS,
		&result, error);

	pgm_result_free(result);
	return status;
}


static int
count_native_threads(void)
{
	DIR		   *directory = opendir("/proc/self/task");
	struct dirent *entry;
	int			count = 0;

	if (directory == NULL)
		return -1;
	while ((entry = readdir(directory)) != NULL)
	{
		const unsigned char *cursor = (const unsigned char *) entry->d_name;

		if (*cursor == '\0')
			continue;
		while (*cursor != '\0' && isdigit(*cursor))
			cursor++;
		if (*cursor == '\0')
			count++;
	}
	if (closedir(directory) != 0)
		return -1;
	return count;
}


static int
count_open_file_descriptors(void)
{
	DIR		   *directory = opendir("/proc/self/fd");
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
prepare_file_descriptor_limit(rlim_t required, rlim_t *effective)
{
	struct rlimit limit;

	if (effective == NULL || getrlimit(RLIMIT_NOFILE, &limit) != 0)
		return -1;
	if (limit.rlim_cur < required)
	{
		if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < required)
			return -1;
		limit.rlim_cur = required;
		if (setrlimit(RLIMIT_NOFILE, &limit) != 0)
			return -1;
	}
	*effective = limit.rlim_cur;
	return 0;
}


static void
report_phase(const char *phase, size_t progress)
{
	fprintf(stderr, "POSTGAMMA_SESSION_PHASE phase=%s progress=%zu\n",
		phase, progress);
	(void) fflush(stderr);
}
