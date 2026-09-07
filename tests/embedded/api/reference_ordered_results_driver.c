#define _POSIX_C_SOURCE 200809L

#include "libpq-fe.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void emit_hex(const void *data, size_t size);
static const char *result_kind_name(ExecStatusType status);
static void emit_result_case(
	const char *name, size_t sequence, PGresult *result);
static void emit_description_case(const char *name, const PGresult *result);
static void emit_transaction_case(
	size_t sequence, PGTransactionStatusType transaction_status);
static bool result_status_is(
	const PGresult *result, ExecStatusType expected_status);
static bool result_value_is(
	const PGresult *result, int expected_format,
	const void *expected, size_t expected_size);
static bool close_prepared(PGconn *connection, const char *name);


int
main(int argument_count, char **arguments)
{
	PGconn	   *connection = NULL;
	PGresult   *result = NULL;
	PGcancelConn *cancel = NULL;
	bool		passed = false;
	size_t		ordered_result_count = 0;
	size_t		prepared_execution_count = 0;

	if (argument_count != 3)
	{
		fprintf(stderr, "usage: %s SOCKET_DIRECTORY PORT\n", arguments[0]);
		return 2;
	}
	connection = PQsetdbLogin(
		arguments[1], arguments[2], NULL, NULL, "postgres", "postgamma", NULL);
	if (connection == NULL || PQstatus(connection) != CONNECTION_OK)
		goto fail;
	if (PQserverVersion(connection) / 10000 != 19)
	{
		fprintf(stderr, "reference server is not PostgreSQL 19\n");
		goto fail;
	}

	{
		const Oid parameter_types[] = {25};
		const char *parameter_values[] = {"alpha"};
		const int parameter_lengths[] = {5};
		const int parameter_formats[] = {0};

		result = PQexecParams(
			connection, "SELECT $1::text AS payload", 1, parameter_types,
			parameter_values, parameter_lengths, parameter_formats, 0);
		if (!result_status_is(result, PGRES_TUPLES_OK) ||
			!result_value_is(result, 0, "alpha", 5))
			goto fail;
		emit_result_case("extended", 0, result);
		PQclear(result);
		result = NULL;
	}

	{
		const ExecStatusType expected_statuses[] =
		{
			PGRES_TUPLES_OK,
			PGRES_COMMAND_OK,
			PGRES_TUPLES_OK,
		};
		const char *sql =
			"SELECT 1 AS first_value; "
			"SET application_name = 'postgamma-script'; "
			"SELECT 3 AS third_value;";

		if (PQsendQuery(connection, sql) != 1)
			goto fail;
		for (size_t index = 0;
			 index < sizeof(expected_statuses) / sizeof(expected_statuses[0]);
			 index++)
		{
			result = PQgetResult(connection);
			if (!result_status_is(result, expected_statuses[index]))
				goto fail;
			if (index == 0 && !result_value_is(result, 0, "1", 1))
				goto fail;
			if (index == 2 && !result_value_is(result, 0, "3", 1))
				goto fail;
			emit_result_case("script", index, result);
			ordered_result_count++;
			PQclear(result);
			result = NULL;
		}
		result = PQgetResult(connection);
		if (result != NULL)
			goto fail;
	}

	if (PQsendQuery(connection, "SELECT 10; SELECT 1 / 0; SELECT 30") != 1)
		goto fail;
	result = PQgetResult(connection);
	if (!result_status_is(result, PGRES_TUPLES_OK) ||
		!result_value_is(result, 0, "10", 2))
		goto fail;
	emit_result_case("script_error", 0, result);
	ordered_result_count++;
	PQclear(result);
	result = PQgetResult(connection);
	if (!result_status_is(result, PGRES_FATAL_ERROR) ||
		strcmp(PQresultErrorField(result, PG_DIAG_SQLSTATE), "22012") != 0)
		goto fail;
	emit_result_case("script_error", 1, result);
	ordered_result_count++;
	PQclear(result);
	result = NULL;
	result = PQgetResult(connection);
	if (result != NULL)
		goto fail;

	result = PQexecParams(
		connection, "SELECT 40", 0, NULL, NULL, NULL, NULL, 0);
	if (!result_status_is(result, PGRES_TUPLES_OK) ||
		!result_value_is(result, 0, "40", 2))
		goto fail;
	emit_result_case("reuse_after_error", 0, result);
	PQclear(result);
	result = NULL;

	{
		const Oid parameter_types[] = {23};
		const unsigned char binary_value[] = {0, 0, 0, 42};
		const char *parameter_values[] = {(const char *) binary_value};
		const int parameter_lengths[] = {4};
		const int parameter_formats[] = {1};

		result = PQexecParams(
			connection, "SELECT $1::int4 AS binary_value", 1,
			parameter_types, parameter_values, parameter_lengths,
			parameter_formats, 1);
		if (!result_status_is(result, PGRES_TUPLES_OK) ||
			!result_value_is(result, 1, binary_value, sizeof(binary_value)))
			goto fail;
		emit_result_case("binary", 0, result);
		PQclear(result);
		result = NULL;
	}

	result = PQprepare(
		connection, "api_inferred",
		"SELECT $1::int4 + $2::int4 AS total", 0, NULL);
	if (!result_status_is(result, PGRES_COMMAND_OK))
		goto fail;
	PQclear(result);
	result = PQdescribePrepared(connection, "api_inferred");
	if (!result_status_is(result, PGRES_COMMAND_OK) || PQnparams(result) != 2 ||
		PQparamtype(result, 0) != 23 || PQparamtype(result, 1) != 23 ||
		PQnfields(result) != 1 || PQftype(result, 0) != 23 ||
		strcmp(PQfname(result, 0), "total") != 0)
		goto fail;
	emit_description_case("prepared_inferred", result);
	PQclear(result);
	result = NULL;

	{
		const char *parameter_values[] = {"1", "2"};
		const int parameter_lengths[] = {1, 1};
		const int parameter_formats[] = {0, 0};

		result = PQexecPrepared(
			connection, "api_inferred", 2, parameter_values,
			parameter_lengths, parameter_formats, 0);
		if (!result_status_is(result, PGRES_TUPLES_OK) ||
			!result_value_is(result, 0, "3", 1))
			goto fail;
		emit_result_case("prepared_execute", 0, result);
		prepared_execution_count++;
		PQclear(result);
		result = NULL;
	}

	{
		const unsigned char binary_left[] = {0, 0, 0, 2};
		const unsigned char binary_right[] = {0, 0, 0, 3};
		const unsigned char binary_expected[] = {0, 0, 0, 5};
		const char *parameter_values[] =
		{
			(const char *) binary_left,
			(const char *) binary_right,
		};
		const int parameter_lengths[] = {4, 4};
		const int parameter_formats[] = {1, 1};

		result = PQexecPrepared(
			connection, "api_inferred", 2, parameter_values,
			parameter_lengths, parameter_formats, 1);
		if (!result_status_is(result, PGRES_TUPLES_OK) ||
			!result_value_is(
				result, 1, binary_expected, sizeof(binary_expected)))
			goto fail;
		emit_result_case("prepared_execute", 1, result);
		prepared_execution_count++;
		PQclear(result);
		result = NULL;
	}
	if (!close_prepared(connection, "api_inferred"))
		goto fail;

	{
		const Oid explicit_type[] = {23};
		const char *parameter_values[] = {"7"};
		const int parameter_lengths[] = {1};
		const int parameter_formats[] = {0};

		result = PQprepare(
			connection, "api_explicit", "SELECT $1 AS explicit_value", 1,
			explicit_type);
		if (!result_status_is(result, PGRES_COMMAND_OK))
			goto fail;
		PQclear(result);
		result = PQdescribePrepared(connection, "api_explicit");
		if (!result_status_is(result, PGRES_COMMAND_OK) ||
			PQnparams(result) != 1 || PQparamtype(result, 0) != 23 ||
			PQnfields(result) != 1 || PQftype(result, 0) != 23 ||
			strcmp(PQfname(result, 0), "explicit_value") != 0)
			goto fail;
		emit_description_case("prepared_explicit", result);
		PQclear(result);
		result = PQexecPrepared(
			connection, "api_explicit", 1, parameter_values,
			parameter_lengths, parameter_formats, 0);
		if (!result_status_is(result, PGRES_TUPLES_OK) ||
			!result_value_is(result, 0, "7", 1))
			goto fail;
		emit_result_case("prepared_explicit_execute", 0, result);
		prepared_execution_count++;
		PQclear(result);
		result = NULL;
		if (!close_prepared(connection, "api_explicit"))
			goto fail;
	}

	result = PQexec(connection, "BEGIN");
	if (!result_status_is(result, PGRES_COMMAND_OK))
		goto fail;
	PQclear(result);
	result = NULL;
	if (PQtransactionStatus(connection) != PQTRANS_INTRANS)
		goto fail;
	emit_transaction_case(0, PQtransactionStatus(connection));
	result = PQexec(connection, "COMMIT");
	if (!result_status_is(result, PGRES_COMMAND_OK))
		goto fail;
	PQclear(result);
	result = NULL;
	if (PQtransactionStatus(connection) != PQTRANS_IDLE)
		goto fail;
	emit_transaction_case(1, PQtransactionStatus(connection));

	result = PQexec(connection, "SELECT pg_advisory_lock(6301)");
	if (!result_status_is(result, PGRES_TUPLES_OK))
		goto fail;
	PQclear(result);
	result = PQexec(connection, "SELECT pg_advisory_unlock(6301)");
	if (!result_status_is(result, PGRES_TUPLES_OK))
		goto fail;
	PQclear(result);
	result = NULL;

	if (PQsendQueryParams(
			connection, "SELECT pg_sleep(30)", 0, NULL, NULL, NULL, NULL, 0) != 1)
		goto fail;
	cancel = PQcancelCreate(connection);
	if (cancel == NULL || PQcancelBlocking(cancel) != 1)
		goto fail;
	PQcancelFinish(cancel);
	cancel = NULL;
	result = PQgetResult(connection);
	if (!result_status_is(result, PGRES_FATAL_ERROR) ||
		strcmp(PQresultErrorField(result, PG_DIAG_SQLSTATE), "57014") != 0)
		goto fail;
	emit_result_case("cancel", 0, result);
	ordered_result_count++;
	PQclear(result);
	result = NULL;
	result = PQgetResult(connection);
	if (result != NULL)
		goto fail;

	result = PQexecParams(
		connection, "SELECT 99 AS after_cancel", 0, NULL, NULL, NULL, NULL, 0);
	if (!result_status_is(result, PGRES_TUPLES_OK) ||
		!result_value_is(result, 0, "99", 2))
		goto fail;
	emit_result_case("reuse_after_cancel", 0, result);
	PQclear(result);
	result = NULL;
	passed = true;

fail:
	if (cancel != NULL)
		PQcancelFinish(cancel);
	if (result != NULL)
		PQclear(result);
	if (!passed)
	{
		fprintf(
			stderr, "reference ordered-results driver failed: %s",
			connection != NULL ? PQerrorMessage(connection) :
			"could not allocate a libpq connection\n");
	}
	if (connection != NULL)
		PQfinish(connection);
	if (!passed)
		return 1;
	printf(
		"POSTGAMMA_API_REFERENCE postgres=19 ordered_results=%zu "
		"prepared_executions=%zu text_binary=true reuse_after_error=true "
		"cancel=true reuse_after_cancel=true phase=closed\n",
		ordered_result_count, prepared_execution_count);
	return 0;
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
result_kind_name(ExecStatusType status)
{
	switch (status)
	{
		case PGRES_EMPTY_QUERY:
			return "empty";
		case PGRES_COMMAND_OK:
			return "command";
		case PGRES_TUPLES_OK:
			return "tuples";
		case PGRES_COPY_OUT:
			return "copy_out";
		case PGRES_COPY_IN:
			return "copy_in";
		case PGRES_BAD_RESPONSE:
		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
			return "error";
		case PGRES_COPY_BOTH:
			return "copy_both";
		case PGRES_SINGLE_TUPLE:
			return "single_tuple";
		case PGRES_PIPELINE_SYNC:
			return "pipeline_sync";
		case PGRES_PIPELINE_ABORTED:
			return "pipeline_aborted";
		case PGRES_TUPLES_CHUNK:
			return "tuples_chunk";
		default:
			return "unknown";
	}
}


static void
emit_result_case(
	const char *name, size_t sequence, PGresult *result)
{
	const char *command = PQcmdStatus(result);
	const char *column = "";
	const char *sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
	const char *severity =
		PQresultErrorField(result, PG_DIAG_SEVERITY_NONLOCALIZED);
	const char *message = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);
	const char *detail = PQresultErrorField(result, PG_DIAG_MESSAGE_DETAIL);
	const void *value = NULL;
	size_t		value_size = 0;
	int		rows = PQntuples(result);
	int		columns = PQnfields(result);
	Oid		table_oid = 0;
	int		table_column = 0;
	Oid		type_oid = 0;
	int		type_size = 0;
	int		modifier = 0;
	int		format = 0;
	int		is_null = 0;

	if (columns > 0)
	{
		column = PQfname(result, 0);
		table_oid = PQftable(result, 0);
		table_column = PQftablecol(result, 0);
		type_oid = PQftype(result, 0);
		type_size = PQfsize(result, 0);
		modifier = PQfmod(result, 0);
		format = PQfformat(result, 0);
	}
	if (rows > 0 && columns > 0)
	{
		is_null = PQgetisnull(result, 0, 0);
		if (!is_null)
		{
			value = PQgetvalue(result, 0, 0);
			value_size = (size_t) PQgetlength(result, 0, 0);
		}
	}
	if (command == NULL)
		command = "";
	if (column == NULL)
		column = "";
	if (sqlstate == NULL)
		sqlstate = "";
	if (severity == NULL)
		severity = "";
	if (message == NULL)
		message = "";
	if (detail == NULL)
		detail = "";
	printf(
		"POSTGAMMA_API_CASE name=%s sequence=%zu kind=%s rows=%d "
		"columns=%d command=",
		name, sequence, result_kind_name(PQresultStatus(result)), rows, columns);
	emit_hex(command, strlen(command));
	printf(" column=");
	emit_hex(column, strlen(column));
	printf(
		" table_oid=%u table_column=%d oid=%u type_size=%d modifier=%d "
		"format=%d null=%d value=",
		(unsigned int) table_oid, table_column, (unsigned int) type_oid,
		type_size, modifier, format, is_null);
	if (value != NULL)
		emit_hex(value, value_size);
	printf(" sqlstate=");
	emit_hex(sqlstate, strlen(sqlstate));
	printf(" severity=");
	emit_hex(severity, strlen(severity));
	printf(" message=");
	emit_hex(message, strlen(message));
	printf(" detail=");
	emit_hex(detail, strlen(detail));
	(void) putchar('\n');
}


static void
emit_description_case(const char *name, const PGresult *result)
{
	const char *column = PQfname(result, 0);

	printf(
		"POSTGAMMA_API_CASE name=%s sequence=0 kind=description parameters=",
		name);
	for (int index = 0; index < PQnparams(result); index++)
	{
		if (index != 0)
			(void) putchar(',');
		printf("%u", (unsigned int) PQparamtype(result, index));
	}
	printf(" column=");
	if (column != NULL)
		emit_hex(column, strlen(column));
	printf(
		" table_oid=%u table_column=%d oid=%u type_size=%d modifier=%d "
		"format=%d\n",
		(unsigned int) PQftable(result, 0), PQftablecol(result, 0),
		(unsigned int) PQftype(result, 0), PQfsize(result, 0),
		PQfmod(result, 0), PQfformat(result, 0));
}


static void
emit_transaction_case(
	size_t sequence, PGTransactionStatusType transaction_status)
{
	const char *name = "unknown";

	switch (transaction_status)
	{
		case PQTRANS_IDLE:
			name = "idle";
			break;
		case PQTRANS_ACTIVE:
			name = "active";
			break;
		case PQTRANS_INTRANS:
			name = "intrans";
			break;
		case PQTRANS_INERROR:
			name = "inerror";
			break;
		case PQTRANS_UNKNOWN:
			break;
		default:
			break;
	}
	printf(
		"POSTGAMMA_API_CASE name=transaction sequence=%zu kind=status "
		"value=%s\n",
		sequence, name);
}


static bool
result_status_is(const PGresult *result, ExecStatusType expected_status)
{
	return result != NULL && PQresultStatus(result) == expected_status;
}


static bool
result_value_is(
	const PGresult *result, int expected_format,
	const void *expected, size_t expected_size)
{
	return result != NULL && PQntuples(result) == 1 && PQnfields(result) == 1 &&
		PQfformat(result, 0) == expected_format &&
		PQgetisnull(result, 0, 0) == 0 &&
		(size_t) PQgetlength(result, 0, 0) == expected_size &&
		memcmp(PQgetvalue(result, 0, 0), expected, expected_size) == 0;
}


static bool
close_prepared(PGconn *connection, const char *name)
{
	PGresult   *result = PQclosePrepared(connection, name);
	bool		closed = result_status_is(result, PGRES_COMMAND_OK);

	if (result != NULL)
		PQclear(result);
	return closed;
}
