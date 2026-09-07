#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define TEST_TIMEOUT_MS INT64_C(30000)
#define IDENTIFIER_CAPACITY 64


static int exercise_cluster(
	const char *path, const char *executable_path, const char *resource_root,
	bool create, bool test_modules, char identifier[IDENTIFIER_CAPACITY]);
static int execute_text(
	pgm_connection *connection, const char *sql,
	char *value, size_t value_capacity);
static int execute_command(pgm_connection *connection, const char *sql);
static int report_error(
	const char *operation, pgm_status status, const pgm_error *error);


int
main(int argument_count, char **arguments)
{
	char		cluster_a[4096];
	char		cluster_b[4096];
	char		identifier_a[IDENTIFIER_CAPACITY];
	char		identifier_b[IDENTIFIER_CAPACITY];
	char		reopened_identifier[IDENTIFIER_CAPACITY];
	int			count;

	if (argument_count != 4)
	{
		fprintf(stderr,
			"usage: %s CLUSTER_ROOT EXECUTABLE_PATH RESOURCE_ROOT\n",
			arguments[0]);
		return 2;
	}
	count = snprintf(cluster_a, sizeof(cluster_a), "%s/cluster-a", arguments[1]);
	if (count < 0 || (size_t) count >= sizeof(cluster_a))
		return 2;
	count = snprintf(cluster_b, sizeof(cluster_b), "%s/cluster-b", arguments[1]);
	if (count < 0 || (size_t) count >= sizeof(cluster_b))
		return 2;
	if (exercise_cluster(
			cluster_a, arguments[2], arguments[3], true, true,
			identifier_a) != 0 ||
		exercise_cluster(
			cluster_b, arguments[2], arguments[3], true, false,
			identifier_b) != 0 ||
		exercise_cluster(
			cluster_a, arguments[2], arguments[3], false, false,
			reopened_identifier) != 0)
		return 1;
	if (identifier_a[0] == '\0' || identifier_b[0] == '\0' ||
		strcmp(identifier_a, identifier_b) == 0 ||
		strcmp(identifier_a, reopened_identifier) != 0)
	{
		fprintf(stderr,
			"cluster identity contract failed: first=%s second=%s reopened=%s\n",
			identifier_a, identifier_b, reopened_identifier);
		return 1;
	}
	printf(
		"POSTGAMMA_CLUSTER_CREATE clusters=2 unique_identifiers=true "
		"same_process=true reopen=true plpgsql=true snowball=true "
		"checksums=true phase=closed\n");
	return 0;
}


static int
exercise_cluster(
	const char *path, const char *executable_path, const char *resource_root,
	bool create, bool test_modules, char identifier[IDENTIFIER_CAPACITY])
{
	const pgm_setting settings[] =
	{
		{"max_connections", "8"},
	};
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_instance *instance = NULL;
	pgm_connection *connection = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_OK;
	char		checksums[8];
	bool		passed = false;

	identifier[0] = '\0';
	instance_options.create = create ? UINT32_C(1) : UINT32_C(0);
	instance_options.path = path;
	instance_options.executable_path = executable_path;
	instance_options.resource_root = resource_root;
	instance_options.settings = settings;
	instance_options.setting_count = sizeof(settings) / sizeof(settings[0]);
	status = pgm_instance_open(&instance_options, &instance, &error);
	if (status != PGM_STATUS_OK)
		goto finish;
	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-cluster-create";
	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto finish;
	if (execute_text(
			connection,
			"SELECT system_identifier::text FROM pg_control_system()",
			identifier, IDENTIFIER_CAPACITY) != 0 ||
		execute_text(
			connection, "SHOW data_checksums",
			checksums, sizeof(checksums)) != 0 ||
		strcmp(checksums, "on") != 0)
	{
		fprintf(stderr, "cluster checksum policy validation failed\n");
		goto finish;
	}
	if (test_modules)
	{
		char		lexeme[64];

		if (execute_command(
				connection,
				"DO $$ BEGIN PERFORM 1; END $$") != 0 ||
			execute_text(
				connection,
				"SELECT ts_lexize('english_stem', 'running')::text",
				lexeme, sizeof(lexeme)) != 0 ||
			strcmp(lexeme, "{run}") != 0)
		{
			fprintf(stderr, "bundled module validation failed\n");
			goto finish;
		}
	}
	passed = true;

finish:
	if (!passed && status != PGM_STATUS_OK)
		(void) report_error("cluster operation", status, error);
	pgm_error_free(error);
	error = NULL;
	if (connection != NULL)
	{
		status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
		if (status != PGM_STATUS_OK)
		{
			(void) report_error("connection close", status, error);
			passed = false;
		}
		pgm_error_free(error);
		error = NULL;
	}
	if (instance != NULL)
	{
		status = pgm_instance_close(
			instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);
		if (status != PGM_STATUS_OK)
		{
			(void) report_error("instance close", status, error);
			passed = false;
		}
		pgm_error_free(error);
	}
	return passed ? 0 : 1;
}


static int
execute_text(
	pgm_connection *connection, const char *sql,
	char *value, size_t value_capacity)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_value_view view = PGM_VALUE_VIEW_INIT;
	pgm_status	status;
	int			result_status = 1;

	status = pgm_execute(
		connection, sql, NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK)
	{
		(void) report_error("text query", status, error);
		goto finish;
	}
	if (pgm_result_kind(result) != PGM_RESULT_TUPLES_OK ||
		pgm_result_row_count(result) != 1 ||
		pgm_result_column_count(result) != 1 ||
		pgm_result_value(result, 0, 0, &view, &error) != PGM_STATUS_OK ||
		view.is_null != UINT16_C(0) || view.format != UINT16_C(0) ||
		view.data == NULL || view.size >= value_capacity)
	{
		fprintf(stderr, "text query returned an invalid result\n");
		goto finish;
	}
	memcpy(value, view.data, view.size);
	value[view.size] = '\0';
	result_status = 0;

finish:
	pgm_error_free(error);
	pgm_result_free(result);
	return result_status;
}


static int
execute_command(pgm_connection *connection, const char *sql)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	int			result_status = 1;

	status = pgm_execute(
		connection, sql, NULL, 0, UINT16_C(0), TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK)
		(void) report_error("command", status, error);
	else if (pgm_result_kind(result) == PGM_RESULT_COMMAND_OK)
		result_status = 0;
	else
		fprintf(stderr, "command returned an invalid result kind\n");
	pgm_error_free(error);
	pgm_result_free(result);
	return result_status;
}


static int
report_error(
	const char *operation, pgm_status status, const pgm_error *error)
{
	fprintf(stderr, "%s failed: status=%s sqlstate=%s message=%s detail=%s\n",
		operation, pgm_status_name(status), pgm_error_sqlstate(error),
		pgm_error_message(error), pgm_error_detail(error));
	return 1;
}
