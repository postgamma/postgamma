#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define TEST_TIMEOUT_MS INT64_C(30000)
#define CONCURRENT_OPERATIONS 4


static int report_failure(
	const char *operation, pgm_status status, const pgm_error *error);
static bool execute_command(pgm_connection *connection, const char *sql);
static bool execute_text(
	pgm_connection *connection, const char *sql,
	char *output, size_t output_capacity);


int
main(int argument_count, char **arguments)
{
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_checkpoint_options checkpoint_options = PGM_CHECKPOINT_OPTIONS_INIT;
	pgm_instance_telemetry telemetry = PGM_INSTANCE_TELEMETRY_INIT;
	pgm_instance *instance = NULL;
	pgm_connection *connection = NULL;
	pgm_operation *operations[CONCURRENT_OPERATIONS] = {NULL};
	pgm_operation *canceled = NULL;
	pgm_operation *unexpected = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	char		before_lsn[64] = "";
	char		after_lsn[64] = "";
	int			waitables[CONCURRENT_OPERATIONS];
	size_t		completed = 0;
	bool		passed = false;

	if (argument_count != 5 || strcmp(arguments[4], "create") != 0)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT create\n",
			arguments[0]);
		return 2;
	}
	if ((pgm_capabilities() & PGM_CAP_MANAGEMENT_OPERATIONS) == 0)
	{
		fprintf(stderr, "management operation capability is not advertised\n");
		goto fail;
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
	connection_options.application_name = "postgamma-c-api-management";
	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (!execute_text(
			connection,
			"SELECT checkpoint_lsn::text FROM pg_control_checkpoint()",
			before_lsn, sizeof(before_lsn)) ||
		!execute_command(
			connection, "CREATE TABLE management_checkpoint(i int)") ||
		!execute_command(
			connection,
			"INSERT INTO management_checkpoint SELECT generate_series(1, 1000)"))
		goto fail;
	status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection = NULL;

	{
		pgm_checkpoint_options invalid = PGM_CHECKPOINT_OPTIONS_INIT;

		invalid.flags = UINT32_C(1);
		status = pgm_instance_checkpoint_async(
			instance, &invalid, &unexpected, &error);
		if (status != PGM_STATUS_INVALID_ARGUMENT || unexpected != NULL ||
			error == NULL || pgm_error_status(error) != status)
		{
			fprintf(stderr, "reserved checkpoint flags were not rejected\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
	}

	status = pgm_instance_checkpoint_async(
		instance, &checkpoint_options, &canceled, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_operation_cancel(canceled, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	{
		pgm_operation_state state = PGM_OPERATION_PENDING;

		status = pgm_operation_progress(canceled, &state, &error);
		if (status != PGM_STATUS_CANCELED || state != PGM_OPERATION_CANCELED)
		{
			fprintf(stderr, "pending checkpoint cancellation is invalid\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
	}
	pgm_operation_free(canceled);
	canceled = NULL;

	for (size_t index = 0; index < CONCURRENT_OPERATIONS; index++)
	{
		pgm_operation_state state = PGM_OPERATION_PENDING;

		waitables[index] = -1;
		status = pgm_instance_checkpoint_async(
			instance, &checkpoint_options, &operations[index], &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = pgm_operation_waitable(
			operations[index], &waitables[index], &error);
		if (status != PGM_STATUS_OK || waitables[index] < 0)
			goto fail;
		status = pgm_operation_progress(operations[index], &state, &error);
		if (status != PGM_STATUS_OK || state != PGM_OPERATION_RUNNING)
		{
			fprintf(stderr, "checkpoint did not enter running state\n");
			goto fail;
		}
	}
	status = pgm_operation_cancel(operations[0], &error);
	if (status != PGM_STATUS_BUSY)
	{
		fprintf(stderr, "dispatched checkpoint cancellation was not rejected\n");
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	status = pgm_instance_get_telemetry(instance, &telemetry, &error);
	if (status != PGM_STATUS_OK || telemetry.connection_count != 0)
	{
		fprintf(stderr, "checkpoint created a hidden SQL connection\n");
		goto fail;
	}
	status = pgm_instance_close(
		instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_BUSY)
	{
		fprintf(stderr, "instance close ignored active operations\n");
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;

	while (completed < CONCURRENT_OPERATIONS)
	{
		struct pollfd descriptors[CONCURRENT_OPERATIONS];
		int poll_status;

		for (size_t index = 0; index < CONCURRENT_OPERATIONS; index++)
		{
			descriptors[index].fd = operations[index] != NULL ?
				waitables[index] : -1;
			descriptors[index].events = POLLIN;
			descriptors[index].revents = 0;
		}
		poll_status = poll(descriptors, CONCURRENT_OPERATIONS, 30000);
		if (poll_status <= 0)
		{
			fprintf(stderr, "checkpoint operation waitable timed out\n");
			goto fail;
		}
		for (size_t index = 0; index < CONCURRENT_OPERATIONS; index++)
		{
			pgm_operation_state state = PGM_OPERATION_RUNNING;

			if (operations[index] == NULL ||
				(descriptors[index].revents & POLLIN) == 0)
				continue;
			status = pgm_operation_progress(
				operations[index], &state, &error);
			if (status != PGM_STATUS_OK || state != PGM_OPERATION_COMPLETED)
			{
				fprintf(stderr, "checkpoint operation did not complete\n");
				goto fail;
			}
			pgm_operation_free(operations[index]);
			operations[index] = NULL;
			completed++;
		}
	}

	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (!execute_text(
			connection,
			"SELECT checkpoint_lsn::text FROM pg_control_checkpoint()",
			after_lsn, sizeof(after_lsn)) || strcmp(before_lsn, after_lsn) == 0)
	{
		fprintf(stderr, "checkpoint did not advance the control-file LSN\n");
		goto fail;
	}
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
		"POSTGAMMA_KERNEL_3_MANAGEMENT checkpoint=true operations=%d "
		"caller_driven=true waitable=true pending_cancel=true "
		"running_cancel=busy hidden_connections=0 lsn_advanced=true "
		"close_gate=true subprocesses=0 phase=closed\n",
		CONCURRENT_OPERATIONS);

fail:
	if (!passed)
		(void) report_failure("Arrow management management driver", status, error);
	pgm_error_free(error);
	if (canceled != NULL)
		pgm_operation_free(canceled);
	for (size_t index = 0; index < CONCURRENT_OPERATIONS; index++)
		if (operations[index] != NULL)
			pgm_operation_free(operations[index]);
	if (connection != NULL)
		(void) pgm_connection_close(connection, TEST_TIMEOUT_MS, NULL);
	if (instance != NULL)
		(void) pgm_instance_close(
			instance, PGM_SHUTDOWN_IMMEDIATE, TEST_TIMEOUT_MS, NULL);
	return passed ? 0 : 1;
}


static int
report_failure(
	const char *operation, pgm_status status, const pgm_error *error)
{
	fprintf(stderr, "%s failed: status=%s message=%s detail=%s\n",
		operation, pgm_status_name(status),
		error != NULL ? pgm_error_message(error) : "",
		error != NULL ? pgm_error_detail(error) : "");
	return 1;
}


static bool
execute_command(pgm_connection *connection, const char *sql)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	bool		ok = status == PGM_STATUS_OK && result != NULL &&
		pgm_result_kind(result) == PGM_RESULT_COMMAND_OK;

	if (!ok)
		(void) report_failure("execute checkpoint fixture", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}


static bool
execute_text(
	pgm_connection *connection, const char *sql,
	char *output, size_t output_capacity)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	pgm_status	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	bool		ok = status == PGM_STATUS_OK && result != NULL &&
		pgm_result_kind(result) == PGM_RESULT_TUPLES_OK &&
		pgm_result_row_count(result) == 1 &&
		pgm_result_column_count(result) == 1 &&
		pgm_result_value(result, 0, 0, &value, &error) == PGM_STATUS_OK &&
		value.is_null == 0 && value.size < output_capacity;

	if (ok)
	{
		memcpy(output, value.data, value.size);
		output[value.size] = '\0';
	}
	else
		(void) report_failure("read checkpoint LSN", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}
