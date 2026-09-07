#define _POSIX_C_SOURCE 200809L

#ifdef POSTGAMMA_CONFORMANCE_REFERENCE
#include "libpq-fe.h"
#else
#include "postgamma/postgamma.h"
#endif

#include "postgamma_conformance_cases.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define TEST_TIMEOUT_MS INT64_C(30000)
#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)


typedef struct conformance_observation
{
	const char *kind;
	char		sqlstate[6];
	size_t		rows;
	size_t		columns;
	uint64_t	digest;
} conformance_observation;


static void hash_bytes(uint64_t *digest, const void *data, size_t size);
static void hash_size(uint64_t *digest, uint64_t value);
static void hash_string(uint64_t *digest, const char *value);
static bool execute_case(
	void *connection, const postgamma_conformance_case *test,
	conformance_observation *observation);


static void
hash_bytes(uint64_t *digest, const void *data, size_t size)
{
	const unsigned char *bytes = data;

	for (size_t index = 0; index < size; index++)
	{
		*digest ^= bytes[index];
		*digest *= FNV_PRIME;
	}
}


static void
hash_size(uint64_t *digest, uint64_t value)
{
	unsigned char bytes[8];

	for (size_t index = 0; index < sizeof(bytes); index++)
		bytes[index] = (unsigned char) (value >> (index * 8));
	hash_bytes(digest, bytes, sizeof(bytes));
}


static void
hash_string(uint64_t *digest, const char *value)
{
	size_t size = value != NULL ? strlen(value) : 0;

	hash_size(digest, size);
	if (size != 0)
		hash_bytes(digest, value, size);
}


#ifdef POSTGAMMA_CONFORMANCE_REFERENCE

typedef struct conformance_context
{
	PGconn *connections[2];
} conformance_context;


static bool
open_reference_connection(
	PGconn **output, const char *socket_directory, const char *port,
	const char *application_name)
{
	const char *keywords[] =
	{
		"host", "port", "dbname", "user", "application_name", NULL,
	};
	const char *values[] =
	{
		socket_directory, port, "postgres", "postgamma", application_name, NULL,
	};
	PGconn *connection = PQconnectdbParams(keywords, values, 0);

	if (connection == NULL || PQstatus(connection) != CONNECTION_OK ||
		PQserverVersion(connection) / 10000 != 19)
	{
		fprintf(
			stderr, "reference connection failed: %s",
			connection != NULL ? PQerrorMessage(connection) : "allocation failure\n");
		if (connection != NULL)
			PQfinish(connection);
		return false;
	}
	*output = connection;
	return true;
}


static bool
open_context(int argument_count, char **arguments, conformance_context *context)
{
	if (argument_count != 3)
	{
		fprintf(stderr, "usage: %s SOCKET_DIRECTORY PORT\n", arguments[0]);
		return false;
	}
	return open_reference_connection(
		&context->connections[0], arguments[1], arguments[2],
		"postgamma-conformance-primary") &&
		open_reference_connection(
			&context->connections[1], arguments[1], arguments[2],
			"postgamma-conformance-secondary");
}


static void
close_context(conformance_context *context)
{
	for (size_t index = 0; index < 2; index++)
	{
		if (context->connections[index] != NULL)
		{
			PQfinish(context->connections[index]);
			context->connections[index] = NULL;
		}
	}
}


static bool
hash_reference_result(PGresult *result, conformance_observation *observation)
{
	ExecStatusType status = PQresultStatus(result);
	const char *kind = status == PGRES_COMMAND_OK ? "command" : "tuples";
	uint64_t digest = FNV_OFFSET;
	int rows = PQntuples(result);
	int columns = PQnfields(result);

	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
		return false;
	observation->kind = kind;
	observation->rows = (size_t) rows;
	observation->columns = (size_t) columns;
	hash_string(&digest, kind);
	hash_string(&digest, PQcmdStatus(result));
	hash_size(&digest, (uint64_t) rows);
	hash_size(&digest, (uint64_t) columns);
	for (int column = 0; column < columns; column++)
	{
		hash_string(&digest, PQfname(result, column));
		hash_size(&digest, (uint64_t) PQftype(result, column));
		hash_size(&digest, (uint64_t) (int64_t) PQfsize(result, column));
		hash_size(&digest, (uint64_t) (int64_t) PQfmod(result, column));
		hash_size(&digest, (uint64_t) PQfformat(result, column));
	}
	for (int row = 0; row < rows; row++)
	{
		for (int column = 0; column < columns; column++)
		{
			int is_null = PQgetisnull(result, row, column);
			size_t size = is_null != 0 ? 0 :
				(size_t) PQgetlength(result, row, column);

			hash_size(&digest, (uint64_t) is_null);
			hash_size(&digest, size);
			if (size != 0)
				hash_bytes(&digest, PQgetvalue(result, row, column), size);
		}
	}
	observation->digest = digest;
	return true;
}


static bool
execute_case(
	void *opaque, const postgamma_conformance_case *test,
	conformance_observation *observation)
{
	PGconn *connection = opaque;
	PGresult *result = PQexec(connection, test->sql);
	ExecStatusType status;
	const char *sqlstate;
	bool passed = false;

	if (result == NULL)
	{
		fprintf(stderr, "reference case %s returned no result\n", test->name);
		return false;
	}
	status = PQresultStatus(result);
	if (strcmp(test->expected_kind, "error") == 0)
	{
		sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
		if (status != PGRES_FATAL_ERROR || sqlstate == NULL ||
			strcmp(sqlstate, test->expected_sqlstate) != 0)
			goto done;
		observation->kind = "error";
		(void) snprintf(
			observation->sqlstate, sizeof(observation->sqlstate), "%s", sqlstate);
		observation->digest = FNV_OFFSET;
		hash_string(&observation->digest, observation->kind);
		hash_string(&observation->digest, observation->sqlstate);
		passed = true;
	}
	else if (hash_reference_result(result, observation) &&
		strcmp(observation->kind, test->expected_kind) == 0)
		passed = true;

done:
	if (!passed)
	{
		fprintf(
			stderr, "reference case %s failed: expected %s/%s, status=%d: %s",
			test->name, test->expected_kind, test->expected_sqlstate,
			(int) status, PQresultErrorMessage(result));
	}
	PQclear(result);
	return passed;
}

#define CONFORMANCE_MARKER "POSTGAMMA_EMBEDDED_CONFORMANCE_REFERENCE"

#else

typedef struct conformance_context
{
	pgm_instance *instance;
	pgm_connection *connections[2];
} conformance_context;


static bool
open_embedded_connection(
	pgm_instance *instance, pgm_connection **output,
	const char *application_name, pgm_error **error)
{
	pgm_connection_options options = PGM_CONNECTION_OPTIONS_INIT;

	options.user = "postgamma";
	options.database = "postgres";
	options.application_name = application_name;
	return pgm_connection_open(instance, &options, output, error) == PGM_STATUS_OK;
}


static bool
open_context(int argument_count, char **arguments, conformance_context *context)
{
	pgm_instance_options options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_error *error = NULL;
	pgm_status status;
	bool opened;

	if (argument_count != 5 || strcmp(arguments[4], "create") != 0)
	{
		fprintf(
			stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT create\n",
			arguments[0]);
		return false;
	}
	options.path = arguments[1];
	options.executable_path = arguments[2];
	options.resource_root = arguments[3];
	options.create = UINT32_C(1);
	options.executor_worker_count = UINT32_C(4);
	status = pgm_instance_open(&options, &context->instance, &error);
	opened = status == PGM_STATUS_OK &&
		open_embedded_connection(
			context->instance, &context->connections[0],
			"postgamma-conformance-primary", &error) &&
		open_embedded_connection(
			context->instance, &context->connections[1],
			"postgamma-conformance-secondary", &error);
	if (!opened)
	{
		fprintf(
			stderr, "embedded conformance open failed: status=%d sqlstate=%s "
			"message=%s\n",
			(int) status, pgm_error_sqlstate(error), pgm_error_message(error));
		pgm_error_free(error);
		return false;
	}
	pgm_error_free(error);
	return true;
}


static void
close_context(conformance_context *context)
{
	for (size_t index = 0; index < 2; index++)
	{
		if (context->connections[index] != NULL)
		{
			(void) pgm_connection_close(
				context->connections[index], TEST_TIMEOUT_MS, NULL);
			context->connections[index] = NULL;
		}
	}
	if (context->instance != NULL)
	{
		(void) pgm_instance_close(
			context->instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, NULL);
		context->instance = NULL;
	}
}


static bool
hash_embedded_result(
	pgm_result *result, conformance_observation *observation)
{
	pgm_result_status status = pgm_result_kind(result);
	const char *kind = status == PGM_RESULT_COMMAND_OK ? "command" : "tuples";
	uint64_t digest = FNV_OFFSET;
	size_t rows = pgm_result_row_count(result);
	size_t columns = pgm_result_column_count(result);

	if (status != PGM_RESULT_COMMAND_OK && status != PGM_RESULT_TUPLES_OK)
		return false;
	observation->kind = kind;
	observation->rows = rows;
	observation->columns = columns;
	hash_string(&digest, kind);
	hash_string(&digest, pgm_result_command_status(result));
	hash_size(&digest, rows);
	hash_size(&digest, columns);
	for (size_t column_index = 0; column_index < columns; column_index++)
	{
		pgm_column column = PGM_COLUMN_INIT;

		if (pgm_result_column(result, column_index, &column, NULL) != PGM_STATUS_OK)
			return false;
		hash_string(&digest, column.name);
		hash_size(&digest, column.type_oid);
		hash_size(&digest, (uint64_t) (int64_t) column.type_size);
		hash_size(&digest, (uint64_t) (int64_t) column.type_modifier);
		hash_size(&digest, column.format);
	}
	for (size_t row = 0; row < rows; row++)
	{
		for (size_t column = 0; column < columns; column++)
		{
			pgm_value_view value = PGM_VALUE_VIEW_INIT;

			if (pgm_result_value(result, row, column, &value, NULL) != PGM_STATUS_OK)
				return false;
			hash_size(&digest, value.is_null);
			hash_size(&digest, value.size);
			if (value.data != NULL && value.size != 0)
				hash_bytes(&digest, value.data, value.size);
		}
	}
	observation->digest = digest;
	return true;
}


static bool
execute_case(
	void *opaque, const postgamma_conformance_case *test,
	conformance_observation *observation)
{
	pgm_connection *connection = opaque;
	pgm_result *result = NULL;
	pgm_error *error = NULL;
	pgm_status status = pgm_execute(
		connection, test->sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	bool passed = false;

	if (strcmp(test->expected_kind, "error") == 0)
	{
		const char *sqlstate = pgm_error_sqlstate(error);

		if (status != PGM_STATUS_POSTGRES_ERROR || result != NULL ||
			strcmp(sqlstate, test->expected_sqlstate) != 0)
			goto done;
		observation->kind = "error";
		(void) snprintf(
			observation->sqlstate, sizeof(observation->sqlstate), "%s", sqlstate);
		observation->digest = FNV_OFFSET;
		hash_string(&observation->digest, observation->kind);
		hash_string(&observation->digest, observation->sqlstate);
		passed = true;
	}
	else if (status == PGM_STATUS_OK && result != NULL &&
		hash_embedded_result(result, observation) &&
		strcmp(observation->kind, test->expected_kind) == 0)
		passed = true;

done:
	if (!passed)
	{
		fprintf(
			stderr, "embedded case %s failed: expected %s/%s, status=%d "
			"sqlstate=%s message=%s\n",
			test->name, test->expected_kind, test->expected_sqlstate,
			(int) status, pgm_error_sqlstate(error), pgm_error_message(error));
	}
	pgm_error_free(error);
	pgm_result_free(result);
	return passed;
}

#define CONFORMANCE_MARKER "POSTGAMMA_EMBEDDED_CONFORMANCE_CANDIDATE"

#endif


int
main(int argument_count, char **arguments)
{
	conformance_context context = {0};
	bool passed = false;

	if (!open_context(argument_count, arguments, &context))
		goto done;
	for (size_t index = 0; index < POSTGAMMA_CONFORMANCE_CASE_COUNT; index++)
	{
		const postgamma_conformance_case *test =
			&postgamma_conformance_cases[index];
		conformance_observation observation = {0};
		size_t connection_index =
			strcmp(test->session, "secondary") == 0 ? 1 : 0;

		if (!execute_case(
				context.connections[connection_index], test, &observation))
			goto done;
		printf(
			"POSTGAMMA_EMBEDDED_CONFORMANCE_CASE ordinal=%zu name=%s "
			"category=%s session=%s kind=%s sqlstate=%s rows=%zu "
			"columns=%zu digest=%016" PRIx64 "\n",
			index, test->name, test->category, test->session, observation.kind,
			observation.sqlstate[0] != '\0' ? observation.sqlstate : "-",
			observation.rows, observation.columns, observation.digest);
	}
	passed = true;

done:
	close_context(&context);
	if (!passed)
		return 1;
	printf(
		CONFORMANCE_MARKER " postgres=19 cases=%zu connections=2 phase=closed\n",
		(size_t) POSTGAMMA_CONFORMANCE_CASE_COUNT);
	return 0;
}
