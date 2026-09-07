#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define TEST_TIMEOUT_MS INT64_C(30000)


static int report_failure(
	const char *operation, pgm_status status, const pgm_error *error);
static bool value_equals(
	const pgm_result *result, size_t row, size_t column,
	const void *expected, size_t expected_size);
static bool value_format_equals(
	const pgm_result *result, size_t row, size_t column,
	uint16_t expected_format, const void *expected, size_t expected_size);
static bool emit_result_case(
	const char *name, size_t sequence, const pgm_result *result,
	const pgm_error *error);
static void emit_description_case(
	const char *name, const uint32_t *parameter_types,
	size_t parameter_count, const pgm_column *column);
static void emit_transaction_case(
	size_t sequence, pgm_transaction_status transaction_status);
static pgm_status execute_one(
	pgm_connection *connection, const char *sql,
	const pgm_parameter *parameters, size_t parameter_count,
	uint16_t result_format, pgm_result **result, pgm_error **error);
static pgm_status next_result(
	pgm_request *request, pgm_result **result,
	pgm_availability *availability, pgm_error **error);


int
main(int argument_count, char **arguments)
{
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_execute_options execute_options = PGM_EXECUTE_OPTIONS_INIT;
	pgm_script_options script_options = PGM_SCRIPT_OPTIONS_INIT;
	pgm_prepare_options prepare_options = PGM_PREPARE_OPTIONS_INIT;
	pgm_instance *instance = NULL;
	pgm_connection *connection = NULL;
	pgm_connection *second_connection = NULL;
	pgm_statement *statement = NULL;
	pgm_request *request = NULL;
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_transaction_status transaction_status = PGM_TRANSACTION_UNKNOWN;
	pgm_connection_status_snapshot snapshot =
		PGM_CONNECTION_STATUS_SNAPSHOT_INIT;
	pgm_statement_description description = PGM_STATEMENT_DESCRIPTION_INIT;
	pgm_column column = PGM_COLUMN_INIT;
	uint32_t	parameter_types[2] = {0, 0};
	pgm_connection_id first_connection_id;
	pgm_request_id first_request_id;
	pgm_request_id second_request_id;
	size_t		ordered_result_count = 0;
	size_t		prepared_execution_count = 0;
	bool		passed = false;

	if (argument_count != 5 || strcmp(arguments[4], "create") != 0)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT create\n",
			arguments[0]);
		return 2;
	}
	if ((pgm_capabilities() & PGM_CAP_PREPARED_STATEMENTS) == 0)
	{
		fprintf(stderr, "prepared-statement capability is not advertised\n");
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
	connection_options.application_name = "postgamma-c-api-ordered";
	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_connection_open(
		instance, &connection_options, &second_connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	first_connection_id = pgm_connection_identity(connection);
	if (first_connection_id == 0 ||
		pgm_connection_identity(second_connection) == 0 ||
		first_connection_id == pgm_connection_identity(second_connection))
	{
		fprintf(stderr, "connection identities are not unique and nonzero\n");
		goto fail;
	}
	status = pgm_connection_transaction_status(
		connection, &transaction_status, &error);
	if (status != PGM_STATUS_OK || transaction_status != PGM_TRANSACTION_IDLE)
		goto fail;
	status = pgm_connection_get_status(connection, &snapshot, &error);
	if (status != PGM_STATUS_OK ||
		snapshot.connection_id != first_connection_id ||
		snapshot.virtual_backend_pid <= 0 || snapshot.active_request_id != 0 ||
		snapshot.pin_reasons != 0 || snapshot.carrier_retained != 0)
	{
		fprintf(stderr, "initial connection status snapshot is invalid\n");
		goto fail;
	}

	{
		const char sql_with_nul[] = {'S', 'E', 'L', 'E', 'C', 'T', ' ', '1',
			'\0', 'x'};

		status = pgm_execute_async_ex(
			connection, sql_with_nul, sizeof(sql_with_nul), &execute_options,
			&request, &error);
		if (status != PGM_STATUS_INVALID_ARGUMENT || request != NULL ||
			error == NULL || pgm_error_status(error) != status)
		{
			fprintf(stderr, "explicit SQL length did not reject an embedded NUL\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
	}

	{
		pgm_parameter parameter = PGM_PARAMETER_INIT;
		const char *sql = "SELECT $1::text AS payload";

		parameter.type_oid = UINT32_C(25);
		parameter.data = "alpha";
		parameter.size = 5;
		execute_options.parameters = &parameter;
		execute_options.parameter_count = 1;
		status = pgm_execute_async_ex(
			connection, sql, strlen(sql), &execute_options, &request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		first_request_id = pgm_request_identity(request);
		if (first_request_id == 0)
		{
			fprintf(stderr, "request identity is zero\n");
			goto fail;
		}
		status = pgm_connection_get_status(connection, &snapshot, &error);
		if (status != PGM_STATUS_OK ||
			snapshot.active_request_id != first_request_id)
		{
			fprintf(stderr, "active request identity is not observable\n");
			goto fail;
		}
		status = next_result(request, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
			pgm_result_kind(result) != PGM_RESULT_TUPLES_OK ||
			!value_equals(result, 0, 0, "alpha", 5))
			goto fail;
		if (!emit_result_case("extended", 0, result, NULL))
			goto fail;
		{
			pgm_result *outstanding = result;

			status = next_result(request, &result, &availability, &error);
			if (status != PGM_STATUS_BUSY || error == NULL ||
				pgm_error_status(error) != PGM_STATUS_BUSY)
			{
				fprintf(stderr, "outstanding result ownership was not enforced\n");
				pgm_result_free(outstanding);
				goto fail;
			}
			pgm_error_free(error);
			error = NULL;
			pgm_result_free(outstanding);
		}
		result = NULL;
		status = next_result(request, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_END ||
			result != NULL)
			goto fail;
		pgm_request_free(request);
		request = NULL;
		execute_options.parameters = NULL;
		execute_options.parameter_count = 0;
	}

	{
		const char *sql =
			"SELECT 1 AS first_value; "
			"SET application_name = 'postgamma-script'; "
			"SELECT 3 AS third_value;";
		const pgm_result_status expected_kinds[] =
		{
			PGM_RESULT_TUPLES_OK,
			PGM_RESULT_COMMAND_OK,
			PGM_RESULT_TUPLES_OK,
		};

		status = pgm_execute_script_async(
			connection, sql, strlen(sql), &script_options, &request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		second_request_id = pgm_request_identity(request);
		if (second_request_id == 0 || second_request_id == first_request_id)
		{
			fprintf(stderr, "request identities are not unique\n");
			goto fail;
		}
		status = pgm_request_wait(
			request, TEST_TIMEOUT_MS, &result, &error);
		if (status != PGM_STATUS_UNSUPPORTED || result != NULL)
		{
			fprintf(stderr, "script request was accepted by single-result wait\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
		for (size_t index = 0;
			 index < sizeof(expected_kinds) / sizeof(expected_kinds[0]); index++)
		{
			status = next_result(request, &result, &availability, &error);
			if (status != PGM_STATUS_OK ||
				availability != PGM_AVAILABILITY_READY ||
				pgm_result_kind(result) != expected_kinds[index])
				goto fail;
			if (index == 0 && !value_equals(result, 0, 0, "1", 1))
				goto fail;
			if (index == 2 && !value_equals(result, 0, 0, "3", 1))
				goto fail;
			if (!emit_result_case("script", index, result, NULL))
				goto fail;
			ordered_result_count++;
			pgm_result_free(result);
			result = NULL;
		}
		status = next_result(request, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_END)
			goto fail;
		pgm_request_free(request);
		request = NULL;
	}

	{
		const char *sql = "SELECT 10; SELECT 1 / 0; SELECT 30";

		status = pgm_execute_script_async(
			connection, sql, strlen(sql), &script_options, &request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = next_result(request, &result, &availability, &error);
		if (status != PGM_STATUS_OK ||
			pgm_result_kind(result) != PGM_RESULT_TUPLES_OK ||
			!value_equals(result, 0, 0, "10", 2))
			goto fail;
		if (!emit_result_case("script_error", 0, result, NULL))
			goto fail;
		ordered_result_count++;
		pgm_result_free(result);
		result = NULL;
		status = next_result(request, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
			pgm_result_kind(result) != PGM_RESULT_ERROR || error == NULL ||
			strcmp(pgm_error_sqlstate(error), "22012") != 0)
			goto fail;
		if (!emit_result_case("script_error", 1, result, error))
			goto fail;
		ordered_result_count++;
		pgm_error_free(error);
		error = NULL;
		pgm_result_free(result);
		result = NULL;
		status = next_result(request, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_END)
			goto fail;
		pgm_request_free(request);
		request = NULL;
		status = execute_one(
			connection, "SELECT 40", NULL, 0, PGM_FORMAT_TEXT,
			&result, &error);
		if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "40", 2))
			goto fail;
		if (!emit_result_case("reuse_after_error", 0, result, NULL))
			goto fail;
		pgm_result_free(result);
		result = NULL;
	}

	{
		const unsigned char binary_value[] = {0, 0, 0, 42};
		pgm_parameter parameter = PGM_PARAMETER_INIT;
		const char *sql = "SELECT $1::int4 AS binary_value";
		pgm_value_view value = PGM_VALUE_VIEW_INIT;

		parameter.type_oid = UINT32_C(23);
		parameter.format = PGM_FORMAT_BINARY;
		parameter.data = binary_value;
		parameter.size = sizeof(binary_value);
		execute_options.parameters = &parameter;
		execute_options.parameter_count = 1;
		execute_options.result_format = PGM_FORMAT_BINARY;
		status = pgm_execute_async_ex(
			connection, sql, strlen(sql), &execute_options, &request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = next_result(request, &result, &availability, &error);
		if (status != PGM_STATUS_OK ||
			pgm_result_value(result, 0, 0, &value, &error) != PGM_STATUS_OK ||
			value.format != PGM_FORMAT_BINARY || value.size != 4 ||
			memcmp(value.data, binary_value, 4) != 0)
			goto fail;
		if (!emit_result_case("binary", 0, result, NULL))
			goto fail;
		pgm_result_free(result);
		result = NULL;
		status = next_result(request, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_END)
			goto fail;
		pgm_request_free(request);
		request = NULL;
		execute_options.parameters = NULL;
		execute_options.parameter_count = 0;
		execute_options.result_format = PGM_FORMAT_TEXT;
	}

	{
		const char *sql = "SELECT $1::int4 + $2::int4 AS total";
		const unsigned char binary_left[] = {0, 0, 0, 2};
		const unsigned char binary_right[] = {0, 0, 0, 3};
		const unsigned char binary_expected[] = {0, 0, 0, 5};

		status = pgm_statement_prepare(
			connection, sql, strlen(sql), &prepare_options, &statement, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = pgm_statement_describe(statement, &description, &error);
		if (status != PGM_STATUS_OK || description.parameter_count != 2 ||
			description.column_count != 1)
			goto fail;
		for (size_t index = 0; index < 2; index++)
		{
			status = pgm_statement_parameter_type(
				statement, index, &parameter_types[index], &error);
			if (status != PGM_STATUS_OK ||
				parameter_types[index] != UINT32_C(23))
				goto fail;
		}
		status = pgm_statement_column(statement, 0, &column, &error);
		if (status != PGM_STATUS_OK || strcmp(column.name, "total") != 0 ||
			column.type_oid != UINT32_C(23))
			goto fail;
		emit_description_case(
			"prepared_inferred", parameter_types, 2, &column);
		for (int execution = 0; execution < 2; execution++)
		{
			pgm_parameter parameters[2] =
			{
				PGM_PARAMETER_INIT,
				PGM_PARAMETER_INIT,
			};

			for (size_t index = 0; index < 2; index++)
				parameters[index].type_oid = UINT32_C(23);
			if (execution == 0)
			{
				parameters[0].data = "1";
				parameters[0].size = 1;
				parameters[1].data = "2";
				parameters[1].size = 1;
				execute_options.result_format = PGM_FORMAT_TEXT;
			}
			else
			{
				parameters[0].format = PGM_FORMAT_BINARY;
				parameters[0].data = binary_left;
				parameters[0].size = sizeof(binary_left);
				parameters[1].format = PGM_FORMAT_BINARY;
				parameters[1].data = binary_right;
				parameters[1].size = sizeof(binary_right);
				execute_options.result_format = PGM_FORMAT_BINARY;
			}
			execute_options.parameters = parameters;
			execute_options.parameter_count = 2;
			status = pgm_statement_execute_async(
				statement, &execute_options, &request, &error);
			if (status != PGM_STATUS_OK)
				goto fail;
			status = next_result(request, &result, &availability, &error);
			if (status != PGM_STATUS_OK ||
				(execution == 0 && !value_format_equals(
					result, 0, 0, PGM_FORMAT_TEXT, "3", 1)) ||
				(execution == 1 && !value_format_equals(
					result, 0, 0, PGM_FORMAT_BINARY,
					binary_expected, sizeof(binary_expected))))
				goto fail;
			if (!emit_result_case(
					"prepared_execute", (size_t) execution, result, NULL))
				goto fail;
			prepared_execution_count++;
			pgm_result_free(result);
			result = NULL;
			status = next_result(request, &result, &availability, &error);
			if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_END)
				goto fail;
			pgm_request_free(request);
			request = NULL;
		}
		execute_options.parameters = NULL;
		execute_options.parameter_count = 0;
		execute_options.result_format = PGM_FORMAT_TEXT;
		status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
		if (status != PGM_STATUS_BUSY)
		{
			fprintf(stderr, "connection closed while a statement was alive\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
		status = pgm_statement_close(statement, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		statement = NULL;

		{
			const uint32_t explicit_type = UINT32_C(23);
			pgm_parameter parameter = PGM_PARAMETER_INIT;

			prepare_options.parameter_type_oids = &explicit_type;
			prepare_options.parameter_count = 1;
			status = pgm_statement_prepare(
				connection, "SELECT $1 AS explicit_value",
				strlen("SELECT $1 AS explicit_value"),
				&prepare_options, &statement, &error);
			if (status != PGM_STATUS_OK)
				goto fail;
			status = pgm_statement_describe(statement, &description, &error);
			if (status != PGM_STATUS_OK || description.parameter_count != 1 ||
				description.column_count != 1)
				goto fail;
			status = pgm_statement_parameter_type(
				statement, 0, &parameter_types[0], &error);
			if (status != PGM_STATUS_OK ||
				parameter_types[0] != explicit_type)
				goto fail;
			status = pgm_statement_column(statement, 0, &column, &error);
			if (status != PGM_STATUS_OK ||
				strcmp(column.name, "explicit_value") != 0 ||
				column.type_oid != explicit_type)
				goto fail;
			emit_description_case(
				"prepared_explicit", parameter_types, 1, &column);
			parameter.type_oid = explicit_type;
			parameter.data = "7";
			parameter.size = 1;
			execute_options.parameters = &parameter;
			execute_options.parameter_count = 1;
			status = pgm_statement_execute_async(
				statement, &execute_options, &request, &error);
			if (status != PGM_STATUS_OK)
				goto fail;
			status = next_result(request, &result, &availability, &error);
			if (status != PGM_STATUS_OK ||
				!value_format_equals(
					result, 0, 0, PGM_FORMAT_TEXT, "7", 1) ||
				!emit_result_case(
					"prepared_explicit_execute", 0, result, NULL))
				goto fail;
			prepared_execution_count++;
			pgm_result_free(result);
			result = NULL;
			status = next_result(request, &result, &availability, &error);
			if (status != PGM_STATUS_OK ||
				availability != PGM_AVAILABILITY_END)
				goto fail;
			pgm_request_free(request);
			request = NULL;
			execute_options.parameters = NULL;
			execute_options.parameter_count = 0;
			status = pgm_statement_close(statement, &error);
			if (status != PGM_STATUS_OK)
				goto fail;
			statement = NULL;
			prepare_options.parameter_type_oids = NULL;
			prepare_options.parameter_count = 0;
		}
	}

	status = execute_one(
		connection, "BEGIN", NULL, 0, PGM_FORMAT_TEXT, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_connection_get_status(connection, &snapshot, &error);
	if (status != PGM_STATUS_OK ||
		snapshot.transaction_status != PGM_TRANSACTION_INTRANS ||
		(snapshot.pin_reasons & PGM_PIN_TRANSACTION) == 0 ||
		snapshot.carrier_retained != 1)
	{
		fprintf(stderr, "transaction pin state is not observable\n");
		goto fail;
	}
	emit_transaction_case(0, snapshot.transaction_status);
	status = execute_one(
		connection, "COMMIT", NULL, 0, PGM_FORMAT_TEXT, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_connection_transaction_status(
		connection, &transaction_status, &error);
	if (status != PGM_STATUS_OK || transaction_status != PGM_TRANSACTION_IDLE)
		goto fail;
	emit_transaction_case(1, transaction_status);
	status = execute_one(
		connection, "SELECT pg_advisory_lock(6301)", NULL, 0,
		PGM_FORMAT_TEXT, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_connection_get_status(connection, &snapshot, &error);
	if (status != PGM_STATUS_OK ||
		(snapshot.pin_reasons & PGM_PIN_ADVISORY_LOCK) == 0 ||
		snapshot.carrier_retained != 1)
	{
		fprintf(stderr,
			"advisory-lock pin state is not observable: transaction=%d "
			"pin_reasons=%" PRIu32 " carrier=%" PRIu32 "\n",
			(int) snapshot.transaction_status, snapshot.pin_reasons,
			snapshot.carrier_retained);
		goto fail;
	}
	status = execute_one(
		connection, "SELECT pg_advisory_unlock(6301)", NULL, 0,
		PGM_FORMAT_TEXT, &result, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	status = pgm_connection_get_status(connection, &snapshot, &error);
	if (status != PGM_STATUS_OK || snapshot.pin_reasons != 0 ||
		snapshot.carrier_retained != 0)
	{
		fprintf(stderr, "session remained pinned after advisory unlock\n");
		goto fail;
	}

	status = pgm_execute_async_ex(
		connection, "SELECT pg_sleep(30)", strlen("SELECT pg_sleep(30)"),
		&execute_options, &request, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_request_cancel(request, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = next_result(request, &result, &availability, &error);
	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
		pgm_result_kind(result) != PGM_RESULT_ERROR || error == NULL ||
		strcmp(pgm_error_sqlstate(error), "57014") != 0 ||
		!emit_result_case("cancel", 0, result, error))
		goto fail;
	ordered_result_count++;
	pgm_error_free(error);
	error = NULL;
	pgm_result_free(result);
	result = NULL;
	status = next_result(request, &result, &availability, &error);
	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_END)
		goto fail;
	pgm_request_free(request);
	request = NULL;
	status = execute_one(
		connection, "SELECT 99 AS after_cancel", NULL, 0, PGM_FORMAT_TEXT,
		&result, &error);
	if (status != PGM_STATUS_OK || !value_equals(result, 0, 0, "99", 2) ||
		!emit_result_case("reuse_after_cancel", 0, result, NULL))
		goto fail;
	pgm_result_free(result);
	result = NULL;

	status = pgm_connection_close(second_connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	second_connection = NULL;
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

fail:
	if (!passed)
		(void) report_failure("ordered results ordered-results gate", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	if (request != NULL)
		pgm_request_free(request);
	if (statement != NULL)
		(void) pgm_statement_close(statement, NULL);
	if (second_connection != NULL)
		(void) pgm_connection_close(second_connection, TEST_TIMEOUT_MS, NULL);
	if (connection != NULL)
		(void) pgm_connection_close(connection, TEST_TIMEOUT_MS, NULL);
	if (instance != NULL)
		(void) pgm_instance_close(
			instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, NULL);
	if (!passed)
		return 1;
	printf(
		"POSTGAMMA_API_ORDERED_RESULTS postgres=19 connections=2 "
		"ordered_results=%zu prepared_executions=%zu "
		"text_binary=true sql_length=true ownership=true "
		"transaction_pin=true advisory_pin=true reuse_after_error=true "
		"cancel=true reuse_after_cancel=true prepared_capability=true "
		"phase=closed\n",
		ordered_result_count, prepared_execution_count);
	return 0;
}


static int
report_failure(
	const char *operation, pgm_status status, const pgm_error *error)
{
	fprintf(stderr,
		"%s failed: status=%s sqlstate=%s message=%s detail=%s\n",
		operation, pgm_status_name(status), pgm_error_sqlstate(error),
		pgm_error_message(error), pgm_error_detail(error));
	return 1;
}


static bool
value_equals(
	const pgm_result *result, size_t row, size_t column,
	const void *expected, size_t expected_size)
{
	return value_format_equals(
		result, row, column, PGM_FORMAT_TEXT, expected, expected_size);
}


static bool
value_format_equals(
	const pgm_result *result, size_t row, size_t column,
	uint16_t expected_format, const void *expected, size_t expected_size)
{
	pgm_value_view value = PGM_VALUE_VIEW_INIT;

	return pgm_result_value(result, row, column, &value, NULL) ==
			PGM_STATUS_OK && value.is_null == 0 &&
		value.format == expected_format && value.size == expected_size &&
		memcmp(value.data, expected, expected_size) == 0;
}


static void
emit_hex(const void *data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	const unsigned char *bytes = data;

	for (size_t index = 0; index < size; index++)
	{
		(void) putchar(digits[bytes[index] >> 4]);
		(void) putchar(digits[bytes[index] & 15]);
	}
}


static const char *
result_kind_name(pgm_result_status kind)
{
	switch (kind)
	{
		case PGM_RESULT_EMPTY_QUERY:
			return "empty";
		case PGM_RESULT_COMMAND_OK:
			return "command";
		case PGM_RESULT_TUPLES_OK:
			return "tuples";
		case PGM_RESULT_COPY_OUT:
			return "copy_out";
		case PGM_RESULT_COPY_IN:
			return "copy_in";
		case PGM_RESULT_ERROR:
			return "error";
		case PGM_RESULT_TUPLES_CHUNK:
			return "tuples_chunk";
		default:
			return "unknown";
	}
}


static bool
emit_result_case(
	const char *name, size_t sequence, const pgm_result *result,
	const pgm_error *error)
{
	pgm_column column = PGM_COLUMN_INIT;
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	const char *command;
	const char *sqlstate;
	const char *severity;
	const char *message;
	const char *detail;
	size_t		rows;
	size_t		columns;

	if (name == NULL || result == NULL)
		return false;
	rows = pgm_result_row_count(result);
	columns = pgm_result_column_count(result);
	if (columns > 0 &&
		pgm_result_column(result, 0, &column, NULL) != PGM_STATUS_OK)
		return false;
	if (rows > 0 && columns > 0 &&
		pgm_result_value(result, 0, 0, &value, NULL) != PGM_STATUS_OK)
		return false;
	command = pgm_result_command_status(result);
	sqlstate = pgm_error_sqlstate(error);
	severity = pgm_error_severity(error);
	message = pgm_error_message(error);
	detail = pgm_error_detail(error);
	printf(
		"POSTGAMMA_API_CASE name=%s sequence=%zu kind=%s rows=%zu "
		"columns=%zu command=",
		name, sequence, result_kind_name(pgm_result_kind(result)), rows, columns);
	emit_hex(command, strlen(command));
	printf(" column=");
	if (column.name != NULL)
		emit_hex(column.name, strlen(column.name));
	printf(
		" table_oid=%" PRIu32 " table_column=%" PRId32
		" oid=%" PRIu32 " type_size=%" PRId32 " modifier=%" PRId32
		" format=%u null=%u value=",
		column.table_oid, column.table_column, column.type_oid,
		column.type_size, column.type_modifier, (unsigned int) value.format,
		(unsigned int) value.is_null);
	if (value.data != NULL)
		emit_hex(value.data, value.size);
	printf(" sqlstate=");
	emit_hex(sqlstate, strlen(sqlstate));
	printf(" severity=");
	emit_hex(severity, strlen(severity));
	printf(" message=");
	emit_hex(message, strlen(message));
	printf(" detail=");
	emit_hex(detail, strlen(detail));
	(void) putchar('\n');
	return true;
}


static void
emit_description_case(
	const char *name, const uint32_t *parameter_types,
	size_t parameter_count, const pgm_column *column)
{
	printf(
		"POSTGAMMA_API_CASE name=%s sequence=0 kind=description parameters=",
		name);
	for (size_t index = 0; index < parameter_count; index++)
	{
		if (index != 0)
			(void) putchar(',');
		printf("%" PRIu32, parameter_types[index]);
	}
	printf(" column=");
	if (column->name != NULL)
		emit_hex(column->name, strlen(column->name));
	printf(
		" table_oid=%" PRIu32 " table_column=%" PRId32
		" oid=%" PRIu32 " type_size=%" PRId32 " modifier=%" PRId32
		" format=%u\n",
		column->table_oid, column->table_column, column->type_oid,
		column->type_size, column->type_modifier, (unsigned int) column->format);
}


static void
emit_transaction_case(
	size_t sequence, pgm_transaction_status transaction_status)
{
	const char *name = "unknown";

	switch (transaction_status)
	{
		case PGM_TRANSACTION_IDLE:
			name = "idle";
			break;
		case PGM_TRANSACTION_ACTIVE:
			name = "active";
			break;
		case PGM_TRANSACTION_INTRANS:
			name = "intrans";
			break;
		case PGM_TRANSACTION_INERROR:
			name = "inerror";
			break;
		case PGM_TRANSACTION_UNKNOWN:
			break;
		default:
			break;
	}
	printf(
		"POSTGAMMA_API_CASE name=transaction sequence=%zu kind=status "
		"value=%s\n",
		sequence, name);
}


static pgm_status
execute_one(
	pgm_connection *connection, const char *sql,
	const pgm_parameter *parameters, size_t parameter_count,
	uint16_t result_format, pgm_result **result, pgm_error **error)
{
	return pgm_execute(
		connection, sql, parameters, parameter_count, result_format,
		TEST_TIMEOUT_MS, result, error);
}


static pgm_status
next_result(
	pgm_request *request, pgm_result **result,
	pgm_availability *availability, pgm_error **error)
{
	return pgm_request_next_result(
		request, TEST_TIMEOUT_MS, result, availability, error);
}
