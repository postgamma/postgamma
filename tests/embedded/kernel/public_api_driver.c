#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define TEST_CONNECTION_COUNT 4
#define TEST_TIMEOUT_MS INT64_C(30000)


typedef struct CallbackState
{
	_Atomic uint32_t notice_count;
	_Atomic uint32_t notification_count;
	char		notice_message[128];
	char		notification_channel[64];
	char		notification_payload[64];
} CallbackState;


static int fail_status(const char *operation, pgm_status status, pgm_error *error);
static bool value_equals(
	const pgm_result *result, size_t row, size_t column,
	const void *expected, size_t expected_size);
static bool binary_int4_equals(
	const pgm_result *result, size_t row, size_t column, uint32_t expected);
static void notice_callback(void *argument, const pgm_notice *notice);
static void notification_callback(
	void *argument, const pgm_notification *notification);
static uint64_t monotonic_milliseconds(void);


int
main(int argument_count, char **arguments)
{
	const pgm_setting instance_settings[] =
	{
		{"max_connections", "12"},
	};
	const pgm_setting connection_settings[] =
	{
		{"work_mem", "6MB"},
	};
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_parameter parameters[3] =
	{
		PGM_PARAMETER_INIT,
		PGM_PARAMETER_INIT,
		PGM_PARAMETER_INIT,
	};
	pgm_instance *instance = NULL;
	pgm_connection *connections[TEST_CONNECTION_COUNT] = {NULL};
	pgm_request *requests[TEST_CONNECTION_COUNT] = {NULL};
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	CallbackState callbacks;
	pgm_column column = PGM_COLUMN_INIT;
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	pgm_status	status;
	pgm_request_state request_state;
	int			waitable_fd = -1;
	uint64_t	cancel_started_ms;
	uint64_t	cancel_elapsed_ms = 0;
	uint64_t	timeout_started_ms;
	uint64_t	timeout_elapsed_ms = 0;
	bool		passed = false;

	if (argument_count != 4 && argument_count != 5)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT [create]\n",
			arguments[0]);
		return 2;
	}
	if (argument_count == 5 && strcmp(arguments[4], "create") != 0)
	{
		fprintf(stderr, "the optional mode must be create\n");
		return 2;
	}
	memset(&callbacks, 0, sizeof(callbacks));
	if (pgm_abi_version() != PGM_ABI_VERSION ||
		strcmp(pgm_postgresql_version(), "19") != 0 ||
		pgm_build_id()[0] == '\0')
	{
		fprintf(stderr, "public ABI identity is invalid\n");
		return 1;
	}
	instance_options.path = arguments[1];
	instance_options.create = argument_count == 5 ? UINT32_C(1) : UINT32_C(0);
	instance_options.executable_path = arguments[2];
	instance_options.resource_root = arguments[3];
	instance_options.settings = instance_settings;
	instance_options.setting_count =
		sizeof(instance_settings) / sizeof(instance_settings[0]);
	if (instance_options.create != 0)
	{
		pgm_instance_options rejected_options = instance_options;
		pgm_instance *rejected_instance = NULL;

		rejected_options.logical_umask = UINT32_C(0027);
		status = pgm_instance_open(
			&rejected_options, &rejected_instance, &error);
		if (status != PGM_STATUS_UNSUPPORTED || rejected_instance != NULL ||
			error == NULL || pgm_error_status(error) != status ||
			strstr(pgm_error_message(error), "logical_umask 0077") == NULL)
		{
			fprintf(stderr, "creation umask rejection validation failed\n");
			pgm_error_free(error);
			return 1;
		}
		pgm_error_free(error);
		error = NULL;
	}
	status = pgm_instance_open(&instance_options, &instance, &error);
	if (status != PGM_STATUS_OK)
		return fail_status("instance open", status, error);
	{
		pgm_connection *invalid_connection = NULL;

		status = pgm_connection_open(
			instance, NULL, &invalid_connection, &error);
		if (status != PGM_STATUS_INVALID_ARGUMENT || invalid_connection != NULL ||
			error == NULL || pgm_error_status(error) != status)
		{
			fprintf(stderr, "public error contract validation failed\n");
			if (status == PGM_STATUS_OK)
				status = PGM_STATUS_INTERNAL_ERROR;
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
	}

	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-public-api-gate";
	connection_options.settings = connection_settings;
	connection_options.setting_count =
		sizeof(connection_settings) / sizeof(connection_settings[0]);
	connection_options.notice_callback = notice_callback;
	connection_options.notification_callback = notification_callback;
	connection_options.user_data = &callbacks;
	status = pgm_connection_open(
		instance, &connection_options, &connections[0], &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	for (size_t index = 1; index < TEST_CONNECTION_COUNT; index++)
	{
		pgm_connection_options options = PGM_CONNECTION_OPTIONS_INIT;

		options.user = "postgamma";
		options.database = "postgres";
		options.application_name = "postgamma-public-concurrency-gate";
		status = pgm_connection_open(
			instance, &options, &connections[index], &error);
		if (status != PGM_STATUS_OK)
			goto fail;
	}

	parameters[0].type_oid = UINT32_C(25);
	parameters[0].data = "alpha";
	parameters[0].size = 5;
	parameters[1].type_oid = UINT32_C(23);
	parameters[1].data = "42";
	parameters[1].size = 2;
	parameters[2].type_oid = UINT32_C(25);
	parameters[2].is_null = UINT16_C(1);
	status = pgm_execute(
		connections[0],
		"SELECT $1::text AS label, $2::int4 AS count, "
		"$3::text AS missing",
		parameters, 3, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (pgm_result_kind(result) != PGM_RESULT_TUPLES_OK ||
		pgm_result_row_count(result) != 1 ||
		pgm_result_column_count(result) != 3 ||
		pgm_result_column(result, 1, &column, &error) != PGM_STATUS_OK ||
		strcmp(column.name, "count") != 0 || column.type_oid != UINT32_C(23) ||
		!value_equals(result, 0, 0, "alpha", 5) ||
		!value_equals(result, 0, 1, "42", 2) ||
		pgm_result_value(result, 0, 2, &value, &error) != PGM_STATUS_OK ||
		value.is_null != UINT16_C(1) || value.data != NULL || value.size != 0)
	{
		fprintf(stderr, "typed public result validation failed\n");
		status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "SHOW work_mem", NULL, 0, UINT16_C(0),
		TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "6MB", 3))
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "SET application_name = 'postgamma-migrated'",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "SHOW application_name", NULL, 0, UINT16_C(0),
		TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK ||
		!value_equals(result, 0, 0, "postgamma-migrated", 18))
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0],
		"CREATE TEMP TABLE postgamma_migration_state(value int)",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0],
		"INSERT INTO postgamma_migration_state VALUES (11), (22)",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0],
		"PREPARE postgamma_migration_plan(int) AS "
		"SELECT sum(value) + $1 FROM postgamma_migration_state",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "EXECUTE postgamma_migration_plan(9)",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "42", 2))
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "BEGIN", NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0],
		"DECLARE postgamma_migration_cursor CURSOR WITH HOLD FOR "
		"SELECT value FROM postgamma_migration_state ORDER BY value",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "FETCH NEXT FROM postgamma_migration_cursor",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "11", 2))
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "COMMIT", NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "FETCH NEXT FROM postgamma_migration_cursor",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "22", 2))
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "CLOSE postgamma_migration_cursor", NULL, 0,
		UINT16_C(0), TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "SELECT pg_advisory_lock(6202)", NULL, 0,
		UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "SELECT pg_advisory_unlock(6202)", NULL, 0,
		UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "t", 1))
		goto fail;
	pgm_result_free(result);
	result = NULL;

	status = pgm_execute(
		connections[0], "SELECT 1 / 0", NULL, 0, UINT16_C(0),
		TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_POSTGRES_ERROR || error == NULL ||
		strcmp(pgm_error_sqlstate(error), "22012") != 0 ||
		strcmp(pgm_error_severity(error), "ERROR") != 0 ||
		strstr(pgm_error_message(error), "division by zero") == NULL)
	{
		fprintf(stderr, "public error mapping validation failed\n");
		if (status == PGM_STATUS_OK)
			status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	status = pgm_execute(
		connections[0],
		"COPY (SELECT 1) TO PROGRAM 'exit 87'",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_POSTGRES_ERROR || result != NULL || error == NULL ||
		pgm_error_message(error) == NULL ||
		strstr(pgm_error_message(error), "not supported") == NULL)
	{
		fprintf(stderr, "embedded shell-process rejection failed\n");
		if (status == PGM_STATUS_OK)
			status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;

	status = pgm_execute(
		connections[0], "SELECT 16909060::int4", NULL, 0, UINT16_C(1),
		TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (!binary_int4_equals(result, 0, 0, UINT32_C(16909060)))
	{
		fprintf(stderr, "binary public result validation failed\n");
		status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	pgm_result_free(result);
	result = NULL;

	status = pgm_execute(
		connections[0],
		"DROP TABLE IF EXISTS postgamma_public_notice_missing",
		NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	if (atomic_load_explicit(
			&callbacks.notice_count, memory_order_acquire) != UINT32_C(1) ||
		strstr(callbacks.notice_message, "does not exist") == NULL)
	{
		fprintf(stderr, "notice callback validation failed\n");
		status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}

	status = pgm_execute(
		connections[0], "LISTEN postgamma_public", NULL, 0, UINT16_C(0),
		TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connections[0], "NOTIFY postgamma_public, 'ready'", NULL, 0,
		UINT16_C(0), TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	if (atomic_load_explicit(
			&callbacks.notification_count, memory_order_acquire) != UINT32_C(1) ||
		strcmp(callbacks.notification_channel, "postgamma_public") != 0 ||
		strcmp(callbacks.notification_payload, "ready") != 0)
	{
		fprintf(stderr, "notification callback validation failed\n");
		status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}

	cancel_started_ms = monotonic_milliseconds();
	status = pgm_execute_async(
		connections[0], "SELECT pg_sleep(30)", NULL, 0, UINT16_C(0),
		&requests[0], &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_request_poll(requests[0], &request_state, &error);
	if (status != PGM_STATUS_OK ||
		(request_state != PGM_REQUEST_PENDING &&
		 request_state != PGM_REQUEST_RUNNING))
	{
		fprintf(stderr, "public request poll validation failed\n");
		if (status == PGM_STATUS_OK)
			status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	status = pgm_request_waitable(requests[0], &waitable_fd, &error);
	if (status != PGM_STATUS_OK || waitable_fd < 0)
	{
		fprintf(stderr, "public request waitable validation failed\n");
		if (status == PGM_STATUS_OK)
			status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	status = pgm_request_progress(requests[0], &request_state, &error);
	if (status != PGM_STATUS_OK ||
		(request_state != PGM_REQUEST_PENDING &&
		 request_state != PGM_REQUEST_RUNNING))
	{
		fprintf(stderr, "caller-driven request progress validation failed\n");
		if (status == PGM_STATUS_OK)
			status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	status = pgm_request_cancel(requests[0], &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_request_wait(
		requests[0], TEST_TIMEOUT_MS, &result, &error);
	cancel_elapsed_ms = monotonic_milliseconds() - cancel_started_ms;
	if (status != PGM_STATUS_CANCELED || error == NULL ||
		strcmp(pgm_error_sqlstate(error), "57014") != 0 ||
		cancel_elapsed_ms > UINT64_C(2000))
	{
		fprintf(stderr, "public cancellation validation failed\n");
		if (status == PGM_STATUS_OK)
			status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	status = pgm_connection_close(
		connections[0], TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_BUSY || error == NULL ||
		pgm_error_status(error) != status ||
		strstr(pgm_error_message(error), "free the connection request") == NULL)
	{
		fprintf(stderr, "request ownership contract validation failed\n");
		if (status == PGM_STATUS_OK)
			connections[0] = NULL;
		else
			status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	pgm_request_free(requests[0]);
	requests[0] = NULL;
	timeout_started_ms = monotonic_milliseconds();
	status = pgm_execute(
		connections[0], "SELECT pg_sleep(30)", NULL, 0, UINT16_C(0),
		INT64_C(10), &result, &error);
	timeout_elapsed_ms = monotonic_milliseconds() - timeout_started_ms;
	if (status != PGM_STATUS_TIMEOUT || result != NULL || error == NULL ||
		pgm_error_status(error) != status ||
		timeout_elapsed_ms > UINT64_C(2000))
	{
		fprintf(stderr, "bounded synchronous timeout validation failed\n");
		if (status == PGM_STATUS_OK)
			status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;

	for (size_t index = 0; index < TEST_CONNECTION_COUNT; index++)
	{
		char	parameter_text[16];
		pgm_parameter parameter = PGM_PARAMETER_INIT;

		(void) snprintf(parameter_text, sizeof(parameter_text), "%zu", index + 1);
		parameter.type_oid = UINT32_C(23);
		parameter.data = parameter_text;
		parameter.size = strlen(parameter_text);
		status = pgm_execute_async(
			connections[index],
			"SELECT pg_sleep(0.05), $1::int4 AS worker",
			&parameter, 1, UINT16_C(0), &requests[index], &error);
		if (status != PGM_STATUS_OK)
			goto fail;
	}
	for (size_t index = 0; index < TEST_CONNECTION_COUNT; index++)
	{
		char	expected[16];

		status = pgm_request_wait(
			requests[index], TEST_TIMEOUT_MS, &result, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		(void) snprintf(expected, sizeof(expected), "%zu", index + 1);
		if (!value_equals(result, 0, 1, expected, strlen(expected)))
		{
			fprintf(stderr, "concurrent public result validation failed\n");
			status = PGM_STATUS_INTERNAL_ERROR;
			goto fail;
		}
		pgm_result_free(result);
		result = NULL;
		pgm_request_free(requests[index]);
		requests[index] = NULL;
	}

	status = pgm_execute(
		connections[0], "SELECT 1", NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "1", 1))
		goto fail;
	pgm_result_free(result);
	result = NULL;
	for (size_t index = 0; index < TEST_CONNECTION_COUNT; index++)
	{
		status = pgm_connection_close(
			connections[index], TEST_TIMEOUT_MS, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		connections[index] = NULL;
	}
	status = pgm_instance_close(
		instance, PGM_SHUTDOWN_FAST, INT64_MAX, &error);
	if (status != PGM_STATUS_INTERNAL_ERROR || error == NULL ||
		pgm_error_status(error) != status)
	{
		fprintf(stderr, "retryable instance close validation failed\n");
		if (status == PGM_STATUS_OK)
			instance = NULL;
		else
			status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	status = pgm_instance_close(
		instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	instance = NULL;
	passed = true;

fail:
	if (!passed && status != PGM_STATUS_OK)
		(void) fail_status("public API operation", status, error);
	pgm_error_free(error);
	error = NULL;
	pgm_result_free(result);
	for (size_t index = 0; index < TEST_CONNECTION_COUNT; index++)
	{
		if (requests[index] != NULL)
			pgm_request_free(requests[index]);
		if (connections[index] != NULL)
		{
			pgm_status close_status = pgm_connection_close(
				connections[index], TEST_TIMEOUT_MS, &error);

			if (close_status != PGM_STATUS_OK)
			{
				(void) fail_status("connection close", close_status, error);
				passed = false;
			}
			pgm_error_free(error);
			error = NULL;
		}
	}
	if (instance != NULL)
	{
		pgm_status close_status = pgm_instance_close(
			instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);

		if (close_status != PGM_STATUS_OK)
		{
			(void) fail_status("instance close", close_status, error);
			passed = false;
		}
		pgm_error_free(error);
	}
	if (!passed)
		return 1;
	printf(
		"POSTGAMMA_KERNEL_PUBLIC_API abi=%" PRIu32
		" capabilities=%" PRIu64
		" postgres=%s typed=true binary=true error=true settings=true "
		"notice=true notification=true "
		"cancel=true timeout=true timeout_cleanup_bounded=true "
		"caller_driven=true waitable=true request_threads=0 "
		"provider=pooled executor_workers=%d "
		"session_state=true guc_state=true temp_state=true prepared_state=true "
		"transaction_state=true portal_state=true "
		"holdable_cursor_migration=true advisory_pinning=true "
		"shell_process_rejected=true "
		"cancel_elapsed_ms=%" PRIu64
		" timeout_elapsed_ms=%" PRIu64
		" concurrent_connections=%d error_contract=true ownership=true "
		"creation_umask=true close_retry=true recovery=true phase=closed\n",
		pgm_abi_version(), pgm_capabilities(), pgm_postgresql_version(),
		TEST_CONNECTION_COUNT,
		cancel_elapsed_ms,
		timeout_elapsed_ms,
		TEST_CONNECTION_COUNT);
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
	const pgm_result *result, size_t row, size_t column,
	const void *expected, size_t expected_size)
{
	pgm_value_view value = PGM_VALUE_VIEW_INIT;

	return pgm_result_value(result, row, column, &value, NULL) ==
			PGM_STATUS_OK &&
		value.is_null == UINT16_C(0) && value.format == UINT16_C(0) &&
		value.size == expected_size &&
		memcmp(value.data, expected, expected_size) == 0;
}


static bool
binary_int4_equals(
	const pgm_result *result, size_t row, size_t column, uint32_t expected)
{
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	const unsigned char *bytes;
	uint32_t	decoded;

	if (pgm_result_value(result, row, column, &value, NULL) != PGM_STATUS_OK ||
		value.is_null != UINT16_C(0) || value.format != UINT16_C(1) ||
		value.size != 4 || value.data == NULL)
		return false;
	bytes = value.data;
	decoded = ((uint32_t) bytes[0] << 24) |
		((uint32_t) bytes[1] << 16) |
		((uint32_t) bytes[2] << 8) |
		(uint32_t) bytes[3];
	return decoded == expected;
}


static void
notice_callback(void *argument, const pgm_notice *notice)
{
	CallbackState *state = argument;

	if (state == NULL || notice == NULL ||
		notice->struct_size < sizeof(*notice))
		return;
	(void) snprintf(
		state->notice_message, sizeof(state->notice_message), "%s",
		notice->message != NULL ? notice->message : "");
	(void) atomic_fetch_add_explicit(
		&state->notice_count, UINT32_C(1), memory_order_release);
}


static void
notification_callback(
	void *argument, const pgm_notification *notification)
{
	CallbackState *state = argument;

	if (state == NULL || notification == NULL ||
		notification->struct_size < sizeof(*notification))
		return;
	(void) snprintf(
		state->notification_channel,
		sizeof(state->notification_channel), "%s",
		notification->channel != NULL ? notification->channel : "");
	(void) snprintf(
		state->notification_payload,
		sizeof(state->notification_payload), "%s",
		notification->payload != NULL ? notification->payload : "");
	(void) atomic_fetch_add_explicit(
		&state->notification_count, UINT32_C(1), memory_order_release);
}


static uint64_t
monotonic_milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
		return 0;
	return (uint64_t) now.tv_sec * UINT64_C(1000) +
		(uint64_t) now.tv_nsec / UINT64_C(1000000);
}
