/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include <postgamma/postgamma.h>

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define QUICKSTART_TIMEOUT_MS INT64_C(30000)


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
	pgm_checkpoint_options checkpoint_options = PGM_CHECKPOINT_OPTIONS_INIT;
	pgm_parameter parameters[3] =
	{
		PGM_PARAMETER_INIT,
		PGM_PARAMETER_INIT,
		PGM_PARAMETER_INIT,
	};
	pgm_instance *database = NULL;
	pgm_connection *connection = NULL;
	pgm_operation *checkpoint = NULL;
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	pgm_operation_state operation_state = PGM_OPERATION_PENDING;
	int			waitable = -1;
	bool		passed = false;
	const uint64_t required_capabilities =
		PGM_CAP_PREPARED_STATEMENTS | PGM_CAP_CHUNKED_RESULTS |
		PGM_CAP_COPY_IN | PGM_CAP_COPY_OUT | PGM_CAP_ARROW_C_DATA |
		PGM_CAP_NOTIFICATIONS | PGM_CAP_REQUEST_NOTICES |
		PGM_CAP_STATUS_TELEMETRY | PGM_CAP_INSTANCE_EVENTS |
		PGM_CAP_MANAGEMENT_OPERATIONS;

	if (argument_count != 5 || strcmp(arguments[4], "create") != 0)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT create\n",
			arguments[0]);
		return 2;
	}
	if (pgm_abi_version() != PGM_ABI_VERSION ||
		strcmp(pgm_postgresql_version(), "19") != 0 ||
		(pgm_capabilities() & required_capabilities) != required_capabilities)
	{
		fprintf(stderr, "installed SDK identity or capabilities are invalid\n");
		return 1;
	}

	instance_options.path = arguments[1];
	instance_options.create = UINT32_C(1);
	instance_options.executable_path = arguments[2];
	instance_options.resource_root = arguments[3];
	status = pgm_instance_open(&instance_options, &database, &error);
	if (status != PGM_STATUS_OK)
		goto fail;

	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-quickstart";
	status = pgm_connection_open(
		database, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (!execute_command(
			connection,
			"CREATE TABLE agent_runs("
			"id int PRIMARY KEY, name text NOT NULL, score double precision)"))
		goto fail;

	parameters[0].type_oid = UINT32_C(23);
	parameters[0].data = "7";
	parameters[0].size = 1;
	parameters[1].type_oid = UINT32_C(25);
	parameters[1].data = "planner";
	parameters[1].size = 7;
	parameters[2].type_oid = UINT32_C(701);
	parameters[2].data = "98.5";
	parameters[2].size = 4;
	status = pgm_execute(
		connection,
		"INSERT INTO agent_runs VALUES ($1, $2, $3) RETURNING id",
		parameters, 3, PGM_FORMAT_TEXT, QUICKSTART_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK ||
		pgm_result_kind(result) != PGM_RESULT_TUPLES_OK ||
		pgm_result_row_count(result) != 1 ||
		!value_equals(result, 0, 0, "7"))
		goto fail;
	pgm_result_free(result);
	result = NULL;

	status = pgm_execute(
		connection,
		"SELECT name, score::text FROM agent_runs WHERE id = 7",
		NULL, 0, PGM_FORMAT_TEXT, QUICKSTART_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK ||
		pgm_result_kind(result) != PGM_RESULT_TUPLES_OK ||
		pgm_result_row_count(result) != 1 ||
		pgm_result_column_count(result) != 2 ||
		!value_equals(result, 0, 0, "planner") ||
		!value_equals(result, 0, 1, "98.5"))
		goto fail;
	pgm_result_free(result);
	result = NULL;

	status = pgm_connection_close(connection, QUICKSTART_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection = NULL;

	status = pgm_instance_checkpoint_async(
		database, &checkpoint_options, &checkpoint, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_operation_waitable(checkpoint, &waitable, &error);
	if (status != PGM_STATUS_OK || waitable < 0)
		goto fail;
	status = pgm_operation_progress(checkpoint, &operation_state, &error);
	if (status != PGM_STATUS_OK || operation_state != PGM_OPERATION_RUNNING)
		goto fail;
	while (operation_state == PGM_OPERATION_RUNNING)
	{
		struct pollfd descriptor = {.fd = waitable, .events = POLLIN};
		int			poll_status;

		do
			poll_status = poll(&descriptor, 1, (int) QUICKSTART_TIMEOUT_MS);
		while (poll_status < 0 && errno == EINTR);
		if (poll_status <= 0 || (descriptor.revents & POLLIN) == 0)
		{
			fprintf(stderr, "checkpoint wait timed out\n");
			goto fail;
		}
		status = pgm_operation_progress(
			checkpoint, &operation_state, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
	}
	if (operation_state != PGM_OPERATION_COMPLETED)
		goto fail;
	pgm_operation_free(checkpoint);
	checkpoint = NULL;

	status = pgm_instance_close(
		database, PGM_SHUTDOWN_FAST, QUICKSTART_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	database = NULL;
	passed = true;
	printf(
		"POSTGAMMA_QUICKSTART abi=%" PRIu32 " capabilities=%" PRIu64
		" postgres=%s "
		"rows=1 name=planner score=98.5 checkpoint=true phase=closed\n",
		pgm_abi_version(), pgm_capabilities(), pgm_postgresql_version());

fail:
	if (!passed)
		(void) report_error("PostGamma quickstart", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	if (checkpoint != NULL)
		pgm_operation_free(checkpoint);
	if (connection != NULL)
		(void) pgm_connection_close(connection, QUICKSTART_TIMEOUT_MS, NULL);
	if (database != NULL)
		(void) pgm_instance_close(
			database, PGM_SHUTDOWN_IMMEDIATE, QUICKSTART_TIMEOUT_MS, NULL);
	return passed ? 0 : 1;
}


static bool
execute_command(pgm_connection *connection, const char *sql)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	bool		passed;

	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT,
		QUICKSTART_TIMEOUT_MS, &result, &error);
	passed = status == PGM_STATUS_OK && result != NULL &&
		pgm_result_kind(result) == PGM_RESULT_COMMAND_OK;
	if (!passed)
		(void) report_error("SQL command", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return passed;
}


static bool
value_equals(
	const pgm_result *result, size_t row, size_t column,
	const char *expected)
{
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	const size_t expected_size = strlen(expected);

	return pgm_result_value(result, row, column, &value, NULL) ==
			PGM_STATUS_OK &&
		value.is_null == 0 && value.format == PGM_FORMAT_TEXT &&
		value.size == expected_size &&
		memcmp(value.data, expected, expected_size) == 0;
}


static int
report_error(
	const char *operation, pgm_status status, const pgm_error *error)
{
	fprintf(stderr, "%s failed: status=%s message=%s detail=%s\n",
		operation, pgm_status_name(status),
		error != NULL ? pgm_error_message(error) : "",
		error != NULL ? pgm_error_detail(error) : "");
	return 1;
}
