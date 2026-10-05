#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"
#include "postgamma/private/logical_tool_host.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#define TEST_TIMEOUT_MS INT64_C(30000)


static bool execute_command(pgm_connection *connection, const char *sql);
static bool value_equals(
	const pgm_result *result, size_t row, size_t column,
	const char *expected);
static int report_error(
	const char *operation, pgm_status status, const pgm_error *error);


int
main(int argument_count, char **arguments)
{
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	PostgammaLogicalToolOptions tool_options =
		POSTGAMMA_LOGICAL_TOOL_OPTIONS_INIT;
	PostgammaLogicalToolResult tool_result =
		POSTGAMMA_LOGICAL_TOOL_RESULT_INIT;
	pgm_instance *instance = NULL;
	pgm_connection *connection = NULL;
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	struct stat archive_status;
	bool		passed = false;

	if (argument_count != 5)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT ARCHIVE\n",
			arguments[0]);
		return 2;
	}
	instance_options.path = arguments[1];
	instance_options.create = UINT32_C(1);
	instance_options.executable_path = arguments[2];
	instance_options.resource_root = arguments[3];
	status = pgm_instance_open(&instance_options, &instance, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-logical-management-logical-tool";
	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (!execute_command(
			connection,
			"CREATE TABLE logical_items(id int PRIMARY KEY, payload text NOT NULL)") ||
		!execute_command(
			connection,
			"INSERT INTO logical_items VALUES "
			"(1, 'alpha'), (2, 'beta'), (3, repeat('x', 4096))") ||
		!execute_command(
			connection,
			"SELECT lo_from_bytea(0, decode(repeat('2a', 8192), 'hex'))"))
		goto fail;
	status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection = NULL;

	tool_options.instance = instance;
	tool_options.database = "postgres";
	tool_options.user = "postgamma";
	tool_options.archive_path = arguments[4];
	if (postgamma_logical_tool_dump_file(
			&tool_options, &tool_result) != 0 ||
		tool_result.postgres_exit_code != 0 ||
		stat(arguments[4], &archive_status) != 0 ||
		archive_status.st_size <= 0)
	{
		fprintf(stderr,
			"first in-process dump failed: status=%d exit=%d message=%s\n",
			tool_result.status, tool_result.postgres_exit_code,
			tool_result.message);
		goto fail;
	}
	tool_result = (PostgammaLogicalToolResult)
		POSTGAMMA_LOGICAL_TOOL_RESULT_INIT;
	if (postgamma_logical_tool_dump_file(
			&tool_options, &tool_result) != 0 ||
		tool_result.postgres_exit_code != 0)
	{
		fprintf(stderr,
			"second in-process dump failed: status=%d exit=%d message=%s\n",
			tool_result.status, tool_result.postgres_exit_code,
			tool_result.message);
		goto fail;
	}

	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (!execute_command(connection, "DROP TABLE logical_items") ||
		!execute_command(
			connection,
			"SELECT lo_unlink(oid) FROM pg_largeobject_metadata"))
		goto fail;
	status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection = NULL;

	tool_result = (PostgammaLogicalToolResult)
		POSTGAMMA_LOGICAL_TOOL_RESULT_INIT;
	if (postgamma_logical_tool_restore_file(
			&tool_options, &tool_result) != 0 ||
		tool_result.postgres_exit_code != 0)
	{
		fprintf(stderr,
			"in-process restore failed: status=%d exit=%d message=%s\n",
			tool_result.status, tool_result.postgres_exit_code,
			tool_result.message);
		goto fail;
	}

	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_execute(
		connection,
		"SELECT count(*)::text, sum(id)::text, "
		"(SELECT count(*)::text FROM pg_largeobject_metadata) "
		"FROM logical_items",
		NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK || pgm_result_row_count(result) != 1 ||
		pgm_result_column_count(result) != 3 ||
		!value_equals(result, 0, 0, "3") ||
		!value_equals(result, 0, 1, "6") ||
		!value_equals(result, 0, 2, "1"))
	{
		fprintf(stderr, "logical dump/restore contents did not round trip\n");
		goto fail;
	}
	pgm_result_free(result);
	result = NULL;
	status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection = NULL;
	status = pgm_instance_close(
		instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	instance = NULL;
	passed = true;
	printf(
		"POSTGAMMA_LOGICAL_TOOL pid=%ld dumps=2 restores=1 "
		"rows=3 large_objects=1 archive_bytes=%jd subprocesses=0 phase=closed\n",
		(long) getpid(), (intmax_t) archive_status.st_size);

fail:
	if (!passed)
		(void) report_error("logical-management tool driver", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	if (connection != NULL)
		(void) pgm_connection_close(connection, TEST_TIMEOUT_MS, NULL);
	if (instance != NULL)
		(void) pgm_instance_close(
			instance, PGM_SHUTDOWN_IMMEDIATE, TEST_TIMEOUT_MS, NULL);
	return passed ? 0 : 1;
}


static bool
execute_command(pgm_connection *connection, const char *sql)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	bool		ok;

	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT,
		TEST_TIMEOUT_MS, &result, &error);
	ok = status == PGM_STATUS_OK && result != NULL &&
		(pgm_result_kind(result) == PGM_RESULT_COMMAND_OK ||
		 pgm_result_kind(result) == PGM_RESULT_TUPLES_OK);
	if (!ok)
		(void) report_error(sql, status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}


static bool
value_equals(
	const pgm_result *result, size_t row, size_t column,
	const char *expected)
{
	pgm_value_view value = PGM_VALUE_VIEW_INIT;

	return pgm_result_value(result, row, column, &value, NULL) ==
		PGM_STATUS_OK && value.is_null == 0 && value.size == strlen(expected) &&
		memcmp(value.data, expected, value.size) == 0;
}


static int
report_error(
	const char *operation, pgm_status status, const pgm_error *error)
{
	fprintf(stderr, "%s failed: status=%s message=%s\n",
		operation != NULL ? operation : "operation",
		pgm_status_name(status),
		error != NULL && pgm_error_message(error) != NULL ?
		pgm_error_message(error) : "none");
	return 1;
}
