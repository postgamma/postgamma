#define _POSIX_C_SOURCE 200809L

#include "libpq-fe.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct ExpectedResult
{
	ExecStatusType status;
	int		rows;
} ExpectedResult;


static bool send_extended(
	PGconn *connection, const char *sql, int result_format, int chunk_rows);
static bool send_script(PGconn *connection, const char *sql, int chunk_rows);
static bool consume_expected(
	PGconn *connection, const char *name, const ExpectedResult *expected,
	size_t expected_count, bool expect_sql_error);
static bool emit_case(
	const char *name, size_t sequence, PGresult *result);
static void emit_hex(const void *data, size_t size);
static const char *result_kind_name(ExecStatusType status);


int
main(int argument_count, char **arguments)
{
	const char *keywords[] = {"host", "port", "user", "dbname", NULL};
	const char *values[5];
	PGconn	   *connection = NULL;
	PGresult   *result = NULL;
	bool		passed = false;

	if (argument_count != 3)
	{
		fprintf(stderr, "usage: %s SOCKET_DIRECTORY PORT\n", arguments[0]);
		return 2;
	}
	values[0] = arguments[1];
	values[1] = arguments[2];
	values[2] = "postgamma";
	values[3] = "postgres";
	values[4] = NULL;
	connection = PQconnectdbParams(keywords, values, 0);
	if (connection == NULL || PQstatus(connection) != CONNECTION_OK)
		goto fail;

	{
		static const char sql[] =
			"WITH delay AS MATERIALIZED (SELECT pg_sleep(0.1)) "
			"SELECT g FROM delay, generate_series(1, 10) AS g";
		static const ExpectedResult expected[] =
		{
			{PGRES_TUPLES_CHUNK, 3},
			{PGRES_TUPLES_CHUNK, 3},
			{PGRES_TUPLES_CHUNK, 3},
			{PGRES_TUPLES_CHUNK, 1},
			{PGRES_TUPLES_OK, 0},
		};

		if (!send_extended(connection, sql, 0, 3) ||
			!consume_expected(
				connection, "basic", expected,
				sizeof(expected) / sizeof(expected[0]), false))
			goto fail;
	}

	{
		static const char sql[] =
			"SELECT g::int4 FROM generate_series(1, 5) AS g";
		static const ExpectedResult expected[] =
		{
			{PGRES_TUPLES_CHUNK, 2},
			{PGRES_TUPLES_CHUNK, 2},
			{PGRES_TUPLES_CHUNK, 1},
			{PGRES_TUPLES_OK, 0},
		};

		if (!send_extended(connection, sql, 1, 2) ||
			!consume_expected(
				connection, "binary", expected,
				sizeof(expected) / sizeof(expected[0]), false))
			goto fail;
	}

	{
		static const char sql[] =
			"SELECT g FROM generate_series(1, 4) AS g; "
			"SET application_name = 'postgamma-chunked_results-script'; "
			"SELECT g FROM generate_series(7, 9) AS g";
		static const ExpectedResult expected[] =
		{
			{PGRES_TUPLES_CHUNK, 2},
			{PGRES_TUPLES_CHUNK, 2},
			{PGRES_TUPLES_OK, 0},
			{PGRES_COMMAND_OK, 0},
			{PGRES_TUPLES_CHUNK, 2},
			{PGRES_TUPLES_CHUNK, 1},
			{PGRES_TUPLES_OK, 0},
		};

		if (!send_script(connection, sql, 2) ||
			!consume_expected(
				connection, "script", expected,
				sizeof(expected) / sizeof(expected[0]), false))
			goto fail;
	}

	{
		static const char sql[] =
			"SELECT 10 / (g - 8) FROM generate_series(1, 10) AS g";
		static const ExpectedResult expected[] =
		{
			{PGRES_TUPLES_CHUNK, 3},
			{PGRES_TUPLES_CHUNK, 3},
			{PGRES_FATAL_ERROR, 0},
		};

		if (!send_extended(connection, sql, 0, 3) ||
			!consume_expected(
				connection, "late_error", expected,
				sizeof(expected) / sizeof(expected[0]), true))
			goto fail;
	}

	result = PQexecParams(
		connection, "SELECT 42", 0, NULL, NULL, NULL, NULL, 0);
	if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK ||
		PQntuples(result) != 1 || PQnfields(result) != 1 ||
		PQgetisnull(result, 0, 0) || PQgetlength(result, 0, 0) != 2 ||
		memcmp(PQgetvalue(result, 0, 0), "42", 2) != 0 ||
		!emit_case("reuse_after_late_error", 0, result))
		goto fail;
	PQclear(result);
	result = NULL;
	passed = true;

fail:
	if (result != NULL)
		PQclear(result);
	if (!passed)
	{
		fprintf(stderr, "reference chunked-results driver failed: %s",
			connection != NULL ? PQerrorMessage(connection) :
			"could not allocate a libpq connection\n");
	}
	if (connection != NULL)
		PQfinish(connection);
	if (!passed)
		return 1;
	printf(
		"POSTGAMMA_STREAMING_REFERENCE postgres=19 chunked_sequences=5 "
		"common_streamed_rows=28 common_terminal_results=6 "
		"text_binary=true script=true late_error=true "
		"connection_reuse=true phase=closed\n");
	return 0;
}


static bool
send_extended(
	PGconn *connection, const char *sql, int result_format, int chunk_rows)
{
	return PQsendQueryParams(
			connection, sql, 0, NULL, NULL, NULL, NULL, result_format) == 1 &&
		PQsetChunkedRowsMode(connection, chunk_rows) == 1;
}


static bool
send_script(PGconn *connection, const char *sql, int chunk_rows)
{
	return PQsendQuery(connection, sql) == 1 &&
		PQsetChunkedRowsMode(connection, chunk_rows) == 1;
}


static bool
consume_expected(
	PGconn *connection, const char *name, const ExpectedResult *expected,
	size_t expected_count, bool expect_sql_error)
{
	PGresult   *result = NULL;
	bool		ok = false;

	for (size_t index = 0; index < expected_count; index++)
	{
		const char *sqlstate;

		result = PQgetResult(connection);
		if (result == NULL || PQresultStatus(result) != expected[index].status ||
			PQntuples(result) != expected[index].rows)
			goto done;
		sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
		if (expected[index].status == PGRES_FATAL_ERROR)
		{
			if (!expect_sql_error || sqlstate == NULL ||
				strcmp(sqlstate, "22012") != 0)
				goto done;
		}
		else if (sqlstate != NULL && sqlstate[0] != '\0')
			goto done;
		if (!emit_case(name, index, result))
			goto done;
		PQclear(result);
		result = NULL;
	}
	result = PQgetResult(connection);
	if (result != NULL)
		goto done;
	ok = true;

done:
	if (result != NULL)
		PQclear(result);
	return ok;
}


static bool
emit_case(
	const char *name, size_t sequence, PGresult *result)
{
	const char *command;
	const char *sqlstate;
	const void *first = NULL;
	const void *last = NULL;
	size_t		first_size = 0;
	size_t		last_size = 0;
	int		rows;
	int		columns;
	int		format = 0;

	if (name == NULL || result == NULL)
		return false;
	rows = PQntuples(result);
	columns = PQnfields(result);
	if (rows < 0 || columns < 0)
		return false;
	if (columns > 0)
		format = PQfformat(result, 0);
	if (rows > 0 && columns > 0)
	{
		if (PQgetisnull(result, 0, 0) || PQgetisnull(result, rows - 1, 0))
			return false;
		first = PQgetvalue(result, 0, 0);
		last = PQgetvalue(result, rows - 1, 0);
		first_size = (size_t) PQgetlength(result, 0, 0);
		last_size = (size_t) PQgetlength(result, rows - 1, 0);
	}
	command = PQcmdStatus(result);
	sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
	if (command == NULL)
		command = "";
	if (sqlstate == NULL)
		sqlstate = "";
	printf(
		"POSTGAMMA_STREAMING_CASE name=%s sequence=%zu kind=%s rows=%d "
		"columns=%d format=%d first=",
		name, sequence, result_kind_name(PQresultStatus(result)), rows, columns,
		format);
	if (first != NULL)
		emit_hex(first, first_size);
	printf(" last=");
	if (last != NULL)
		emit_hex(last, last_size);
	printf(" command=");
	emit_hex(command, strlen(command));
	printf(" sqlstate=");
	emit_hex(sqlstate, strlen(sqlstate));
	(void) putchar('\n');
	return true;
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
		case PGRES_COMMAND_OK:
			return "command";
		case PGRES_TUPLES_OK:
			return "tuples";
		case PGRES_TUPLES_CHUNK:
			return "tuples_chunk";
		case PGRES_FATAL_ERROR:
			return "error";
		default:
			return "other";
	}
}
