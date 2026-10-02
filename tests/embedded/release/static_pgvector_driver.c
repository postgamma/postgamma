#define _POSIX_C_SOURCE 200809L

#include <postgamma/postgamma.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


#define TEST_TIMEOUT_MS INT64_C(30000)


static bool bundled_pgvector_is_exact(void);
static bool execute_command(pgm_connection *connection, const char *sql);
static bool execute_value(
	pgm_connection *connection, const char *sql, const char *expected);
static void report_error(
	const char *operation, pgm_status status, const pgm_error *error);


int
main(int argument_count, char **arguments)
{
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_instance *instance = NULL;
	pgm_connection *connection = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	bool		passed = false;

	if (argument_count != 4)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT\n",
			arguments[0]);
		return 2;
	}
	if (!bundled_pgvector_is_exact())
	{
		fprintf(stderr, "bundled pgvector identity is invalid\n");
		return 1;
	}

	instance_options.path = arguments[1];
	instance_options.create = UINT32_C(1);
	instance_options.executable_path = arguments[2];
	instance_options.resource_root = arguments[3];
	instance_options.executor_worker_count = UINT32_C(2);
	status = pgm_instance_open(&instance_options, &instance, &error);
	if (status != PGM_STATUS_OK)
		goto cleanup;

	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-static-pgvector-test";
	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto cleanup;
	if (!execute_command(connection, "CREATE EXTENSION vector") ||
		!execute_command(
			connection,
			"CREATE TABLE static_vectors("
			"id int PRIMARY KEY, embedding vector(3) NOT NULL)") ||
		!execute_command(
			connection,
			"INSERT INTO static_vectors VALUES "
			"(1, '[1,1,1]'), (2, '[4,4,4]'), (3, '[8,8,8]')") ||
		!execute_command(
			connection,
			"CREATE INDEX static_vectors_hnsw ON static_vectors "
			"USING hnsw (embedding vector_l2_ops)") ||
		!execute_value(
			connection,
			"SELECT id::text FROM static_vectors "
			"ORDER BY embedding <-> '[1,1,1]' LIMIT 1",
			"1"))
		goto cleanup;

	status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto cleanup;
	connection = NULL;
	status = pgm_instance_close(
		instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto cleanup;
	instance = NULL;
	passed = true;
	printf(
		"POSTGAMMA_STATIC_PGVECTOR version=0.8.6 hnsw=true "
		"nearest=1 phase=closed\n");

cleanup:
	if (!passed)
		report_error("static pgvector consumer", status, error);
	pgm_error_free(error);
	if (connection != NULL)
		(void) pgm_connection_close(connection, TEST_TIMEOUT_MS, NULL);
	if (instance != NULL)
		(void) pgm_instance_close(
			instance, PGM_SHUTDOWN_IMMEDIATE, TEST_TIMEOUT_MS, NULL);
	return passed ? 0 : 1;
}


static bool
bundled_pgvector_is_exact(void)
{
	for (size_t index = 0; index < pgm_bundled_extension_count(); index++)
	{
		pgm_bundled_extension_info info = PGM_BUNDLED_EXTENSION_INFO_INIT;

		if (pgm_bundled_extension_get(index, &info) != PGM_STATUS_OK)
			return false;
		if (info.sql_name != NULL && strcmp(info.sql_name, "vector") == 0)
			return info.postgresql_major == UINT32_C(19) &&
				info.version != NULL && strcmp(info.version, "0.8.6") == 0;
	}
	return false;
}


static bool
execute_command(pgm_connection *connection, const char *sql)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	bool		passed = status == PGM_STATUS_OK && result != NULL &&
		pgm_result_kind(result) == PGM_RESULT_COMMAND_OK;

	if (!passed)
		report_error("static pgvector SQL command", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return passed;
}


static bool
execute_value(
	pgm_connection *connection, const char *sql, const char *expected)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	pgm_status	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	const size_t expected_size = strlen(expected);
	bool		passed = status == PGM_STATUS_OK && result != NULL &&
		pgm_result_kind(result) == PGM_RESULT_TUPLES_OK &&
		pgm_result_row_count(result) == 1 &&
		pgm_result_column_count(result) == 1 &&
		pgm_result_value(result, 0, 0, &value, NULL) == PGM_STATUS_OK &&
		value.is_null == 0 && value.format == PGM_FORMAT_TEXT &&
		value.size == expected_size &&
		memcmp(value.data, expected, expected_size) == 0;

	if (!passed)
		report_error("static pgvector SQL query", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return passed;
}


static void
report_error(
	const char *operation, pgm_status status, const pgm_error *error)
{
	fprintf(stderr, "%s failed: status=%s message=%s detail=%s\n",
		operation, pgm_status_name(status),
		error != NULL ? pgm_error_message(error) : "",
		error != NULL ? pgm_error_detail(error) : "");
}
