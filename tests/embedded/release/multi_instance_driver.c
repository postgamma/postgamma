#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#define INSTANCE_COUNT 2
#define CONNECTIONS_PER_INSTANCE 4
#define TEST_TIMEOUT_MS INT64_C(60000)
#define ACTIVE_OBSERVATION_TIMEOUT_MS UINT64_C(5000)
#define ACTIVE_OBSERVATION_INTERVAL_NS UINT64_C(10000000)


typedef struct CallbackState
{
	const char *expected_payload;
	_Atomic uint32_t notice_count;
	_Atomic uint32_t notification_count;
	_Atomic uint32_t wrong_notification_count;
} CallbackState;


typedef struct InstanceFixture
{
	const char *name;
	const char *data_directory;
	const char *work_mem;
	const char *marker;
	pgm_instance *instance;
	pgm_connection *connections[CONNECTIONS_PER_INSTANCE];
	CallbackState callbacks;
	char		system_identifier[64];
	int32_t		virtual_backend_pid;
} InstanceFixture;


typedef struct QueryThread
{
	pgm_connection *connection;
	const char *sql;
	const char *expected;
	_Atomic uint32_t entered;
	pgm_status	status;
	bool		matched;
} QueryThread;


static int fail_status(const char *operation, pgm_status status, pgm_error *error);
static bool value_equals(
	const pgm_result *result, size_t row, size_t column, const char *expected);
static bool execute_value(
	pgm_connection *connection, const char *sql, const char *expected);
static bool execute_text(
	pgm_connection *connection, const char *sql, char *output, size_t capacity);
static bool execute_contains(
	pgm_connection *connection, const char *sql, const char *expected);
static bool execute_command(pgm_connection *connection, const char *sql);
static bool execute_expected_error(
	pgm_connection *connection, const char *sql, const char *sqlstate);
static bool open_instance(
	InstanceFixture *fixture, const char *executable_path,
	const char *resource_root, bool create);
static bool open_connections(InstanceFixture *fixture, size_t count);
static bool close_connections(InstanceFixture *fixture);
static bool close_instance(InstanceFixture *fixture);
static bool prepare_instance(InstanceFixture *fixture);
static bool prove_extension_metadata(void);
static bool prove_extension_isolation(
	InstanceFixture fixtures[INSTANCE_COUNT]);
static bool prove_vector_guc_isolation(
	InstanceFixture fixtures[INSTANCE_COUNT]);
static bool prove_extension_startup_failure(
	const InstanceFixture *fixture, const char *executable_path,
	const char *resource_root);
static bool prove_callbacks(InstanceFixture fixtures[INSTANCE_COUNT]);
static bool run_concurrent_queries(
	InstanceFixture fixtures[INSTANCE_COUNT], const char *sql,
	const char *expected[INSTANCE_COUNT], bool require_active_overlap);
static bool observe_both_active(InstanceFixture fixtures[INSTANCE_COUNT]);
static bool prove_cancel_isolation(
	InstanceFixture fixtures[INSTANCE_COUNT], bool *peer_in_flight);
static bool checkpoint_instance(pgm_instance *instance);
static void notice_callback(void *argument, const pgm_notice *notice);
static void notification_callback(
	void *argument, const pgm_notification *notification);
static void *query_thread_main(void *argument);
static uint64_t monotonic_milliseconds(void);


int
main(int argument_count, char **arguments)
{
	InstanceFixture fixtures[INSTANCE_COUNT] =
	{
		{
			.name = "a",
			.work_mem = "5MB",
			.marker = "101",
			.callbacks = {.expected_payload = "from-a"},
		},
		{
			.name = "b",
			.work_mem = "9MB",
			.marker = "202",
			.callbacks = {.expected_payload = "from-b"},
		},
	};
	const char *parallel_expected[INSTANCE_COUNT] =
	{
		"20000100000",
		"20000100000",
	};
	const char *delay_expected[INSTANCE_COUNT] = {"101", "202"};
	const char *vector_expected[INSTANCE_COUNT] = {"1", "1"};
	const char *vector_parallel_expected[INSTANCE_COUNT] =
	{
		"600000",
		"600000",
	};
	pgm_instance_telemetry telemetry[INSTANCE_COUNT] =
	{
		PGM_INSTANCE_TELEMETRY_INIT,
		PGM_INSTANCE_TELEMETRY_INIT,
	};
	pgm_connection_status_snapshot connection_status =
		PGM_CONNECTION_STATUS_SNAPSHOT_INIT;
	char reopened_system_identifier[64] = "";
	bool first_closed = false;
	bool first_reopened = false;
	bool cancel_peer_in_flight = false;
	bool passed = false;
	uint32_t checkpoints = 0;
	uint32_t instances_opened = 0;
	uint32_t connections_opened = 0;

	if (argument_count != 5)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY_A DATA_DIRECTORY_B "
			"EXECUTABLE_PATH RESOURCE_ROOT\n",
			arguments[0]);
		return 2;
	}
	fixtures[0].data_directory = arguments[1];
	fixtures[1].data_directory = arguments[2];
	if (!prove_extension_metadata())
		goto cleanup;

	for (size_t index = 0; index < INSTANCE_COUNT; index++)
	{
		if (!open_instance(
				&fixtures[index], arguments[3], arguments[4], true))
			goto cleanup;
		instances_opened++;
	}
	if ((pgm_capabilities() & PGM_CAP_MULTIPLE_INSTANCES) == 0)
	{
		fprintf(stderr,
			"multi-instance capability is not advertised after its release gate\n");
		goto cleanup;
	}

	for (size_t index = 0; index < INSTANCE_COUNT; index++)
	{
		if (!open_connections(&fixtures[index], CONNECTIONS_PER_INSTANCE))
			goto cleanup;
		connections_opened += CONNECTIONS_PER_INSTANCE;
		if (!prepare_instance(&fixtures[index]))
			goto cleanup;
		if (pgm_connection_get_status(
				fixtures[index].connections[0], &connection_status,
				NULL) != PGM_STATUS_OK ||
			connection_status.virtual_backend_pid <= 0)
		{
			fprintf(stderr, "instance %s has no virtual backend identity\n",
				fixtures[index].name);
			goto cleanup;
		}
		fixtures[index].virtual_backend_pid =
			connection_status.virtual_backend_pid;
	}
	if (strcmp(fixtures[0].system_identifier,
			fixtures[1].system_identifier) == 0)
	{
		fprintf(stderr, "two clusters share a PostgreSQL system identifier\n");
		goto cleanup;
	}
	if (!execute_value(
			fixtures[0].connections[0], "SHOW work_mem", fixtures[0].work_mem) ||
		!execute_value(
			fixtures[1].connections[0], "SHOW work_mem", fixtures[1].work_mem))
		goto cleanup;
	if (!prove_callbacks(fixtures))
		goto cleanup;
	if (!prove_extension_isolation(fixtures))
		goto cleanup;
	if (!prove_vector_guc_isolation(fixtures))
		goto cleanup;

	if (!run_concurrent_queries(
			fixtures,
			"SELECT marker::text FROM postgamma_release_identity, pg_sleep(1)",
			delay_expected, true))
		goto cleanup;
	if (!run_concurrent_queries(
			fixtures,
			"SELECT sum(postgamma_sdk_probe_parallel_value(value))::text "
			"FROM postgamma_release_parallel",
			parallel_expected, false))
		goto cleanup;
	if (!run_concurrent_queries(
			fixtures,
			"SELECT id::text FROM postgamma_release_vector_hnsw "
			"ORDER BY embedding <-> '[1,1,1]' LIMIT 1",
			vector_expected, false))
		goto cleanup;
	if (!run_concurrent_queries(
			fixtures,
			"SELECT sum(vector_dims("
			"ARRAY[value::real, 1::real, 2::real]::vector))::text "
			"FROM postgamma_release_parallel",
			vector_parallel_expected, false))
		goto cleanup;
	for (size_t index = 0; index < INSTANCE_COUNT; index++)
	{
		if (!execute_value(
				fixtures[index].connections[0],
				"SELECT (postgamma_sdk_probe_parallel_calls() > 0)::text",
				"true"))
			goto cleanup;
	}
	if (!prove_cancel_isolation(fixtures, &cancel_peer_in_flight))
		goto cleanup;

	for (size_t index = 0; index < INSTANCE_COUNT; index++)
	{
		if (!checkpoint_instance(fixtures[index].instance))
			goto cleanup;
		checkpoints++;
		if (pgm_instance_get_telemetry(
				fixtures[index].instance, &telemetry[index], NULL) !=
			PGM_STATUS_OK ||
			telemetry[index].connection_count != CONNECTIONS_PER_INSTANCE ||
			telemetry[index].executor_worker_count != UINT32_C(4))
		{
			fprintf(stderr, "instance %s telemetry is not isolated\n",
				fixtures[index].name);
			goto cleanup;
		}
	}

	if (!close_connections(&fixtures[0]) || !close_instance(&fixtures[0]))
		goto cleanup;
	first_closed = true;
	if (!prove_extension_startup_failure(
			&fixtures[0], arguments[3], arguments[4]))
		goto cleanup;
	if (!execute_value(
			fixtures[1].connections[0],
			"SELECT marker::text FROM postgamma_release_identity",
			fixtures[1].marker))
	{
		fprintf(stderr, "instance B stopped after instance A closed\n");
		goto cleanup;
	}

	if (!open_instance(&fixtures[0], arguments[3], arguments[4], false))
		goto cleanup;
	instances_opened++;
	first_reopened = true;
	if (!open_connections(&fixtures[0], 1))
		goto cleanup;
	connections_opened++;
	if (!execute_value(
			fixtures[0].connections[0],
			"SELECT marker::text FROM postgamma_release_identity",
			fixtures[0].marker) ||
		!execute_value(
			fixtures[0].connections[0],
			"SELECT extversion FROM pg_extension WHERE extname = 'vector'",
			"0.8.6") ||
		!execute_value(
			fixtures[0].connections[0],
			"SELECT count(*)::text FROM pg_class WHERE relname IN "
			"('postgamma_release_vector_hnsw_idx', "
			"'postgamma_release_vector_ivf_idx')",
			"2") ||
		!execute_value(
			fixtures[0].connections[0],
			"SELECT id::text FROM postgamma_release_vector_ivf "
			"ORDER BY embedding <-> '[1,1,1]' LIMIT 1",
			"1") ||
		!execute_text(
			fixtures[0].connections[0],
			"SELECT system_identifier::text FROM pg_control_system()",
			reopened_system_identifier, sizeof(reopened_system_identifier)) ||
		strcmp(reopened_system_identifier, fixtures[0].system_identifier) != 0 ||
		!execute_value(
			fixtures[1].connections[0],
			"SELECT marker::text FROM postgamma_release_identity",
			fixtures[1].marker))
	{
		fprintf(stderr, "close/reopen isolation validation failed\n");
		goto cleanup;
	}
	if (!close_connections(&fixtures[0]) || !close_instance(&fixtures[0]))
		goto cleanup;
	if (!close_connections(&fixtures[1]) || !close_instance(&fixtures[1]))
		goto cleanup;
	passed = true;

cleanup:
	for (size_t index = 0; index < INSTANCE_COUNT; index++)
	{
		(void) close_connections(&fixtures[index]);
		(void) close_instance(&fixtures[index]);
	}
	if (!passed)
		return 1;
	printf(
		"POSTGAMMA_RELEASE_MULTI_INSTANCE "
		"host_pid=%jd clusters=2 instances_opened=%u instances_live_peak=2 "
		"connections_opened=%u concurrent_connections=8 "
		"distinct_system_identifiers=true data_isolation=true "
		"guc_isolation=true temp_isolation=true notice_routing=true "
		"notification_routing=true simultaneous_active=true "
		"parallel_query=true cancel_peer_in_flight=%s "
		"cancel_isolation=true checkpoints=%u "
		"first_closed=%s first_reopened=%s virtual_pids_valid=true "
		"extension_sdk=true bundled_extensions=2 vector_extension=true "
		"vector_hnsw=true vector_ivfflat=true vector_guc_isolation=true "
		"vector_concurrency=true vector_parallel_query=true "
		"vector_persistence=true "
		"extension_instances=3 extension_sessions=9 "
		"extension_session_resets=2 "
		"extension_session_mobility=true extension_parallel_workers=true "
		"extension_descriptor_rejections=17 "
		"extension_resources=true extension_failure_recovered=true "
		"unbundled_rejected=true capability_advertised=true phase=closed\n",
		(intmax_t) getpid(), instances_opened, connections_opened,
		cancel_peer_in_flight ? "true" : "false", checkpoints,
		first_closed ? "true" : "false",
		first_reopened ? "true" : "false");
	return 0;
}


static int
fail_status(const char *operation, pgm_status status, pgm_error *error)
{
	fprintf(stderr,
		"%s failed: status=%s sqlstate=%s message=%s detail=%s\n",
		operation, pgm_status_name(status), pgm_error_sqlstate(error),
		pgm_error_message(error), pgm_error_detail(error));
	return 1;
}


static bool
value_equals(
	const pgm_result *result, size_t row, size_t column, const char *expected)
{
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	size_t		expected_size = strlen(expected);

	return result != NULL &&
		pgm_result_value(result, row, column, &value, NULL) == PGM_STATUS_OK &&
		value.is_null == UINT16_C(0) && value.size == expected_size &&
		memcmp(value.data, expected, expected_size) == 0;
}


static bool
execute_value(
	pgm_connection *connection, const char *sql, const char *expected)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	bool		matched;

	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK)
	{
		(void) fail_status(sql, status, error);
		pgm_error_free(error);
		return false;
	}
	matched = value_equals(result, 0, 0, expected);
	if (!matched)
		fprintf(stderr, "query returned an unexpected value: %s\n", sql);
	pgm_result_free(result);
	return matched;
}


static bool
execute_text(
	pgm_connection *connection, const char *sql, char *output, size_t capacity)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	pgm_status	status;
	bool		copied = false;

	if (output == NULL || capacity == 0)
		return false;
	output[0] = '\0';
	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK)
	{
		(void) fail_status(sql, status, error);
		pgm_error_free(error);
		return false;
	}
	if (pgm_result_value(result, 0, 0, &value, NULL) == PGM_STATUS_OK &&
		value.is_null == UINT16_C(0) && value.size < capacity)
	{
		memcpy(output, value.data, value.size);
		output[value.size] = '\0';
		copied = true;
	}
	pgm_result_free(result);
	return copied;
}


static bool
execute_contains(
	pgm_connection *connection, const char *sql, const char *expected)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	size_t		expected_size = strlen(expected);
	bool		matched = false;

	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK)
	{
		(void) fail_status(sql, status, error);
		pgm_error_free(error);
		return false;
	}
	for (size_t row = 0;
		 row < pgm_result_row_count(result) && !matched; row++)
	{
		for (size_t column = 0;
			 column < pgm_result_column_count(result) && !matched; column++)
		{
			pgm_value_view value = PGM_VALUE_VIEW_INIT;

			if (pgm_result_value(
					result, row, column, &value, NULL) != PGM_STATUS_OK ||
				value.is_null != UINT16_C(0) || value.size < expected_size)
				continue;
			for (size_t offset = 0;
				 offset + expected_size <= value.size; offset++)
			{
				if (memcmp(
						(const unsigned char *) value.data + offset,
						expected, expected_size) == 0)
				{
					matched = true;
					break;
				}
			}
		}
	}
	if (!matched)
		fprintf(stderr,
			"query result does not contain '%s': %s\n", expected, sql);
	pgm_result_free(result);
	return matched;
}


static bool
execute_command(pgm_connection *connection, const char *sql)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;

	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_OK)
	{
		(void) fail_status(sql, status, error);
		pgm_error_free(error);
		return false;
	}
	pgm_result_free(result);
	return true;
}


static bool
execute_expected_error(
	pgm_connection *connection, const char *sql, const char *sqlstate)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	bool		matched;

	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	matched = status == PGM_STATUS_POSTGRES_ERROR && result == NULL &&
		error != NULL && pgm_error_sqlstate(error) != NULL &&
		strcmp(pgm_error_sqlstate(error), sqlstate) == 0;
	if (!matched)
		(void) fail_status(sql, status, error);
	pgm_result_free(result);
	pgm_error_free(error);
	return matched;
}


static bool
prove_extension_metadata(void)
{
	uint64_t	capabilities = pgm_capabilities();
	uint64_t	required =
		PGM_EXTENSION_CAP_THREAD_SAFE |
		PGM_EXTENSION_CAP_MULTI_INSTANCE_SAFE |
		PGM_EXTENSION_CAP_SESSION_MOBILITY_SAFE |
		PGM_EXTENSION_CAP_PARALLEL_WORKER_SAFE |
		PGM_EXTENSION_CAP_INSTANCE_SHMEM |
		PGM_EXTENSION_CAP_FILESYSTEM_READ;
	bool		found_probe = false;
	bool		found_vector = false;

	if (pgm_abi_version() != PGM_ABI_VERSION ||
		PGM_ABI_VERSION != UINT32_C(65539) ||
		(capabilities &
			(PGM_CAP_MULTIPLE_INSTANCES | PGM_CAP_BUNDLED_EXTENSIONS)) !=
			(PGM_CAP_MULTIPLE_INSTANCES | PGM_CAP_BUNDLED_EXTENSIONS) ||
		(capabilities & PGM_CAP_NATIVE_EXTENSION_LOADING) != 0 ||
		pgm_bundled_extension_count() != 2)
	{
		fprintf(stderr, "bundled extension ABI metadata is incomplete\n");
		return false;
	}
	for (size_t index = 0; index < pgm_bundled_extension_count(); index++)
	{
		pgm_bundled_extension_info info = PGM_BUNDLED_EXTENSION_INFO_INIT;

		if (pgm_bundled_extension_get(index, &info) != PGM_STATUS_OK ||
			info.postgresql_major != UINT32_C(19) ||
			info.sdk_abi_version != UINT32_C(1) ||
			info.capabilities != required || info.id == NULL ||
			info.sql_name == NULL || info.version == NULL)
		{
			fprintf(stderr,
				"bundled extension ABI metadata entry is incomplete\n");
			return false;
		}
		if (strcmp(info.id, "postgamma_sdk_probe") == 0 &&
			strcmp(info.sql_name, "postgamma_sdk_probe") == 0 &&
			strcmp(info.version, "1.0") == 0)
			found_probe = true;
		else if (strcmp(info.id, "pgvector") == 0 &&
			strcmp(info.sql_name, "vector") == 0 &&
			strcmp(info.version, "0.8.6") == 0)
			found_vector = true;
		else
		{
			fprintf(stderr, "unexpected bundled extension metadata entry\n");
			return false;
		}
	}
	{
		pgm_bundled_extension_info info = PGM_BUNDLED_EXTENSION_INFO_INIT;

		if (pgm_bundled_extension_get(2, &info) !=
			PGM_STATUS_INVALID_ARGUMENT)
			return false;
	}
	return found_probe && found_vector;
}


static bool
prove_extension_isolation(InstanceFixture fixtures[INSTANCE_COUNT])
{
	char		expected[64];
	char		snapshot[512];

	for (size_t instance_index = 0;
		 instance_index < INSTANCE_COUNT; instance_index++)
	{
		InstanceFixture *fixture = &fixtures[instance_index];

		if (!execute_value(
				fixture->connections[0],
				"SELECT extname FROM pg_extension "
				"WHERE extname = 'postgamma_sdk_probe'",
				"postgamma_sdk_probe") ||
			!execute_value(
				fixture->connections[0],
				"SELECT postgamma_sdk_probe_resource()",
				"postgamma-sdk-probe-resource"))
			return false;
		for (size_t connection_index = 0;
			 connection_index < CONNECTIONS_PER_INSTANCE; connection_index++)
		{
			pgm_connection *connection =
				fixture->connections[connection_index];
			uint64_t identity = pgm_connection_identity(connection);
			uint64_t delta = (uint64_t) connection_index + UINT64_C(1);
			char		sql[96];

			if (identity == 0 || snprintf(
					expected, sizeof(expected), "%" PRIu64,
					identity * UINT64_C(1000) + delta) < 0 ||
				snprintf(
					sql, sizeof(sql),
					"SELECT postgamma_sdk_probe_session_add(%" PRIu64 ")",
					delta) < 0 ||
				!execute_value(
					connection, sql, expected))
				return false;
		}
		{
			pgm_connection *reset_connection =
				fixture->connections[CONNECTIONS_PER_INSTANCE - 1];
			uint64_t identity = pgm_connection_identity(reset_connection);

			if (snprintf(
					expected, sizeof(expected), "%" PRIu64,
					identity * UINT64_C(1000)) < 0 ||
				!execute_command(reset_connection, "DISCARD ALL") ||
				!execute_value(
					reset_connection,
					"SELECT postgamma_sdk_probe_session_add(0)", expected))
				return false;
		}
		if (!execute_text(
				fixture->connections[0],
				"SELECT postgamma_sdk_probe_snapshot()",
				snapshot, sizeof(snapshot)) ||
			strstr(snapshot, "startups=1") == NULL ||
			strstr(snapshot, "active_sessions=4") == NULL ||
			strstr(snapshot, "session_initializations=4") == NULL ||
			strstr(snapshot, "session_resets=1") == NULL)
		{
			fprintf(stderr,
				"extension lifecycle snapshot is incomplete for instance %s\n",
				fixture->name);
			return false;
		}
		if (!execute_value(
				fixture->connections[0],
				"SELECT count(*)::text FROM generate_series(1, 17) "
				"AS scenarios(scenario) WHERE "
				"postgamma_sdk_probe_descriptor_rejected(scenario)",
				"17"))
			return false;
	}
	if (!execute_value(
			fixtures[0].connections[0],
			"SELECT postgamma_sdk_probe_instance_add(11)::text", "11") ||
		!execute_value(
			fixtures[1].connections[0],
			"SELECT postgamma_sdk_probe_instance_add(22)::text", "22") ||
		!execute_value(
			fixtures[0].connections[1],
			"SELECT postgamma_sdk_probe_instance_add(1)::text", "12") ||
		!execute_value(
			fixtures[1].connections[1],
			"SELECT postgamma_sdk_probe_instance_add(1)::text", "23") ||
		!execute_expected_error(
			fixtures[0].connections[0],
			"LOAD '$libdir/postgamma_release_unbundled'", "0A000"))
		return false;
	return execute_value(fixtures[0].connections[0], "SELECT 1", "1") &&
		execute_value(fixtures[1].connections[0], "SELECT 1", "1");
}


static bool
prove_vector_guc_isolation(InstanceFixture fixtures[INSTANCE_COUNT])
{
	if (!execute_command(
			fixtures[0].connections[0], "SET hnsw.ef_search = 61") ||
		!execute_command(
			fixtures[0].connections[0], "SET ivfflat.probes = 3") ||
		!execute_command(
			fixtures[1].connections[0], "SET hnsw.ef_search = 83") ||
		!execute_command(
			fixtures[1].connections[0], "SET ivfflat.probes = 7") ||
		!execute_value(
			fixtures[0].connections[0], "SHOW hnsw.ef_search", "61") ||
		!execute_value(
			fixtures[0].connections[0], "SHOW ivfflat.probes", "3") ||
		!execute_value(
			fixtures[1].connections[0], "SHOW hnsw.ef_search", "83") ||
		!execute_value(
			fixtures[1].connections[0], "SHOW ivfflat.probes", "7"))
		return false;
	for (size_t instance_index = 0;
		 instance_index < INSTANCE_COUNT; instance_index++)
	{
		for (size_t connection_index = 1;
			 connection_index < CONNECTIONS_PER_INSTANCE; connection_index++)
		{
			pgm_connection *connection =
				fixtures[instance_index].connections[connection_index];

			if (!execute_value(connection, "SHOW hnsw.ef_search", "40") ||
				!execute_value(connection, "SHOW ivfflat.probes", "1"))
			{
				fprintf(stderr,
					"pgvector GUC state crossed a session boundary\n");
				return false;
			}
		}
	}
	return true;
}


static bool
prove_extension_startup_failure(
	const InstanceFixture *fixture, const char *executable_path,
	const char *resource_root)
{
	pgm_setting settings[] =
	{
		{"max_connections", "12"},
		{"shared_buffers", "16MB"},
		{"max_worker_processes", "2"},
		{"max_parallel_workers", "2"},
		{"autovacuum", "off"},
		{"cluster_name", "postgamma-sdk-probe-fail"},
	};
	pgm_instance_options options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_instance *failed_instance = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;

	options.path = fixture->data_directory;
	options.executable_path = executable_path;
	options.resource_root = resource_root;
	options.settings = settings;
	options.setting_count = sizeof(settings) / sizeof(settings[0]);
	options.executor_worker_count = UINT32_C(4);
	status = pgm_instance_open(&options, &failed_instance, &error);
	if (status == PGM_STATUS_OK || failed_instance != NULL || error == NULL)
	{
		fprintf(stderr,
			"extension startup fault did not fail one instance open\n");
		if (failed_instance != NULL)
			(void) pgm_instance_close(
				failed_instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, NULL);
		pgm_error_free(error);
		return false;
	}
	pgm_error_free(error);
	return true;
}


static bool
open_instance(
	InstanceFixture *fixture, const char *executable_path,
	const char *resource_root, bool create)
{
	pgm_setting settings[] =
	{
		{"max_connections", "12"},
		{"shared_buffers", "16MB"},
		{"max_worker_processes", "2"},
		{"max_parallel_workers", "2"},
		{"autovacuum", "off"},
		{"work_mem", fixture->work_mem},
		{"cluster_name", fixture->name},
	};
	pgm_instance_options options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_error  *error = NULL;
	pgm_status	status;

	options.path = fixture->data_directory;
	options.create = create ? UINT32_C(1) : UINT32_C(0);
	options.executable_path = executable_path;
	options.resource_root = resource_root;
	options.settings = settings;
	options.setting_count = sizeof(settings) / sizeof(settings[0]);
	options.executor_worker_count = UINT32_C(4);
	status = pgm_instance_open(&options, &fixture->instance, &error);
	if (status != PGM_STATUS_OK)
	{
		(void) fail_status("open instance", status, error);
		pgm_error_free(error);
		return false;
	}
	return true;
}


static bool
open_connections(InstanceFixture *fixture, size_t count)
{
	for (size_t index = 0; index < count; index++)
	{
		pgm_connection_options options = PGM_CONNECTION_OPTIONS_INIT;
		pgm_error  *error = NULL;
		pgm_status	status;

		options.user = "postgamma";
		options.database = "postgres";
		options.application_name = index == 0 ?
			"postgamma-release-primary" : "postgamma-release-secondary";
		if (index == 0)
		{
			options.notice_callback = notice_callback;
			options.notification_callback = notification_callback;
			options.user_data = &fixture->callbacks;
		}
		status = pgm_connection_open(
			fixture->instance, &options, &fixture->connections[index], &error);
		if (status != PGM_STATUS_OK)
		{
			(void) fail_status("open connection", status, error);
			pgm_error_free(error);
			return false;
		}
	}
	return true;
}


static bool
close_connections(InstanceFixture *fixture)
{
	bool success = true;

	for (size_t index = 0; index < CONNECTIONS_PER_INSTANCE; index++)
	{
		pgm_error  *error = NULL;
		pgm_status	status;

		if (fixture->connections[index] == NULL)
			continue;
		status = pgm_connection_close(
			fixture->connections[index], TEST_TIMEOUT_MS, &error);
		if (status != PGM_STATUS_OK)
		{
			(void) fail_status("close connection", status, error);
			success = false;
		}
		else
			fixture->connections[index] = NULL;
		pgm_error_free(error);
	}
	return success;
}


static bool
close_instance(InstanceFixture *fixture)
{
	pgm_error  *error = NULL;
	pgm_status	status;

	if (fixture->instance == NULL)
		return true;
	status = pgm_instance_close(
		fixture->instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
	{
		(void) fail_status("close instance", status, error);
		pgm_error_free(error);
		return false;
	}
	fixture->instance = NULL;
	return true;
}


static bool
prepare_instance(InstanceFixture *fixture)
{
	char sql[256];

	if (!execute_text(
			fixture->connections[0],
			"SELECT system_identifier::text FROM pg_control_system()",
			fixture->system_identifier, sizeof(fixture->system_identifier)) ||
		!execute_command(
			fixture->connections[0],
			"CREATE EXTENSION IF NOT EXISTS postgamma_sdk_probe") ||
		!execute_command(
			fixture->connections[0],
			"CREATE EXTENSION IF NOT EXISTS vector") ||
		!execute_value(
			fixture->connections[0],
			"SELECT (abs(('[1,2,3]'::vector <-> "
			"'[4,5,6]'::vector) - sqrt(27)) < 1e-12)::text",
			"true") ||
		!execute_command(
			fixture->connections[0],
			"CREATE TABLE postgamma_release_identity(marker int NOT NULL)"))
		return false;
	if (snprintf(
			sql, sizeof(sql),
			"INSERT INTO postgamma_release_identity VALUES (%s)",
			fixture->marker) < 0 ||
		!execute_command(fixture->connections[0], sql) ||
		!execute_command(
			fixture->connections[0],
			"CREATE TEMP TABLE postgamma_release_temp(marker int NOT NULL)"))
		return false;
	if (snprintf(
			sql, sizeof(sql),
			"INSERT INTO postgamma_release_temp VALUES (%s)", fixture->marker) < 0 ||
		!execute_command(fixture->connections[0], sql) ||
		!execute_value(
			fixture->connections[0],
			"SELECT marker::text FROM postgamma_release_identity",
			fixture->marker) ||
		!execute_value(
			fixture->connections[0],
			"SELECT marker::text FROM postgamma_release_temp", fixture->marker) ||
		!execute_command(
			fixture->connections[0],
			"SET max_parallel_workers_per_gather = 2") ||
		!execute_command(
			fixture->connections[0], "SET min_parallel_table_scan_size = 0") ||
		!execute_command(
			fixture->connections[0], "SET parallel_setup_cost = 0") ||
		!execute_command(
			fixture->connections[0], "SET parallel_tuple_cost = 0") ||
		!execute_command(
			fixture->connections[0],
			"CREATE TABLE postgamma_release_parallel AS "
			"SELECT generate_series(1, 200000)::bigint AS value") ||
		!execute_command(
			fixture->connections[0], "ANALYZE postgamma_release_parallel") ||
		!execute_command(
			fixture->connections[0],
			"CREATE TABLE postgamma_release_vector_hnsw "
			"(id int PRIMARY KEY, embedding vector(3) NOT NULL)") ||
		!execute_command(
			fixture->connections[0],
			"INSERT INTO postgamma_release_vector_hnsw "
			"SELECT i, ARRAY[i::real, (i % 17)::real, "
			"(i % 31)::real]::vector FROM generate_series(1, 2000) AS s(i)") ||
		!execute_command(
			fixture->connections[0],
			"CREATE TABLE postgamma_release_vector_ivf AS TABLE "
			"postgamma_release_vector_hnsw") ||
		!execute_command(
			fixture->connections[0],
			"CREATE INDEX postgamma_release_vector_hnsw_idx ON "
			"postgamma_release_vector_hnsw USING hnsw "
			"(embedding vector_l2_ops) WITH (m = 8, ef_construction = 32)") ||
		!execute_command(
			fixture->connections[0],
			"CREATE INDEX postgamma_release_vector_ivf_idx ON "
			"postgamma_release_vector_ivf USING ivfflat "
			"(embedding vector_l2_ops) WITH (lists = 10)") ||
		!execute_command(
			fixture->connections[0],
			"ANALYZE postgamma_release_vector_hnsw") ||
		!execute_command(
			fixture->connections[0],
			"ANALYZE postgamma_release_vector_ivf") ||
		!execute_command(fixture->connections[0], "SET enable_seqscan = off") ||
		!execute_contains(
			fixture->connections[0],
			"EXPLAIN (COSTS OFF) SELECT id FROM postgamma_release_vector_hnsw "
			"ORDER BY embedding <-> '[1,1,1]' LIMIT 1",
			"postgamma_release_vector_hnsw_idx") ||
		!execute_contains(
			fixture->connections[0],
			"EXPLAIN (COSTS OFF) SELECT id FROM postgamma_release_vector_ivf "
			"ORDER BY embedding <-> '[1,1,1]' LIMIT 1",
			"postgamma_release_vector_ivf_idx") ||
		!execute_value(
			fixture->connections[0],
			"SELECT id::text FROM postgamma_release_vector_hnsw "
			"ORDER BY embedding <-> '[1,1,1]' LIMIT 1", "1") ||
		!execute_value(
			fixture->connections[0],
			"SELECT id::text FROM postgamma_release_vector_ivf "
			"ORDER BY embedding <-> '[1,1,1]' LIMIT 1", "1") ||
		!execute_command(fixture->connections[0], "RESET enable_seqscan") ||
		!execute_contains(
			fixture->connections[0],
			"EXPLAIN (COSTS OFF) SELECT sum(vector_dims("
			"ARRAY[value::real, 1::real, 2::real]::vector)) "
			"FROM postgamma_release_parallel",
			"Gather"))
		return false;
	return true;
}


static bool
prove_callbacks(InstanceFixture fixtures[INSTANCE_COUNT])
{
	if (!execute_command(
			fixtures[0].connections[0],
			"DROP TABLE IF EXISTS postgamma_release_missing_a") ||
		!execute_command(
			fixtures[1].connections[0],
			"DROP TABLE IF EXISTS postgamma_release_missing_b") ||
		atomic_load_explicit(
			&fixtures[0].callbacks.notice_count, memory_order_acquire) == 0 ||
		atomic_load_explicit(
			&fixtures[1].callbacks.notice_count, memory_order_acquire) == 0 ||
		!execute_command(
			fixtures[0].connections[0], "LISTEN postgamma_release_channel") ||
		!execute_command(
			fixtures[1].connections[0], "LISTEN postgamma_release_channel") ||
		!execute_command(
			fixtures[0].connections[1],
			"NOTIFY postgamma_release_channel, 'from-a'") ||
		!execute_value(fixtures[0].connections[0], "SELECT 1", "1") ||
		!execute_value(fixtures[1].connections[0], "SELECT 1", "1"))
		return false;
	if (atomic_load_explicit(
			&fixtures[0].callbacks.notification_count,
			memory_order_acquire) != UINT32_C(1) ||
		atomic_load_explicit(
			&fixtures[1].callbacks.notification_count,
			memory_order_acquire) != UINT32_C(0))
	{
		fprintf(stderr, "notification from A crossed an instance boundary\n");
		return false;
	}
	if (!execute_command(
			fixtures[1].connections[1],
			"NOTIFY postgamma_release_channel, 'from-b'") ||
		!execute_value(fixtures[0].connections[0], "SELECT 1", "1") ||
		!execute_value(fixtures[1].connections[0], "SELECT 1", "1"))
		return false;
	if (atomic_load_explicit(
			&fixtures[0].callbacks.notification_count,
			memory_order_acquire) != UINT32_C(1) ||
		atomic_load_explicit(
			&fixtures[1].callbacks.notification_count,
			memory_order_acquire) != UINT32_C(1) ||
		atomic_load_explicit(
			&fixtures[0].callbacks.wrong_notification_count,
			memory_order_acquire) != UINT32_C(0) ||
		atomic_load_explicit(
			&fixtures[1].callbacks.wrong_notification_count,
			memory_order_acquire) != UINT32_C(0))
	{
		fprintf(stderr, "notification callback routing is not instance-local\n");
		return false;
	}
	return true;
}


static bool
run_concurrent_queries(
	InstanceFixture fixtures[INSTANCE_COUNT], const char *sql,
	const char *expected[INSTANCE_COUNT], bool require_active_overlap)
{
	QueryThread states[INSTANCE_COUNT];
	pthread_t threads[INSTANCE_COUNT];
	size_t	created = 0;
	bool		success = true;

	memset(states, 0, sizeof(states));
	for (size_t index = 0; index < INSTANCE_COUNT; index++)
	{
		states[index].connection = fixtures[index].connections[0];
		states[index].sql = sql;
		states[index].expected = expected[index];
		states[index].status = PGM_STATUS_INTERNAL_ERROR;
		atomic_init(&states[index].entered, UINT32_C(0));
		if (pthread_create(
				&threads[index], NULL, query_thread_main, &states[index]) != 0)
		{
			fprintf(stderr, "could not create concurrent query thread\n");
			success = false;
			break;
		}
		created++;
	}
	if (success && require_active_overlap && !observe_both_active(fixtures))
	{
		fprintf(stderr, "both instances were not observed active together\n");
		success = false;
	}
	for (size_t index = 0; index < created; index++)
	{
		if (pthread_join(threads[index], NULL) != 0)
			success = false;
		if (states[index].status != PGM_STATUS_OK || !states[index].matched)
			success = false;
	}
	return success;
}


static bool
observe_both_active(InstanceFixture fixtures[INSTANCE_COUNT])
{
	uint64_t deadline = monotonic_milliseconds() +
		ACTIVE_OBSERVATION_TIMEOUT_MS;
	struct timespec interval =
	{
		.tv_sec = 0,
		.tv_nsec = (long) ACTIVE_OBSERVATION_INTERVAL_NS,
	};

	while (monotonic_milliseconds() < deadline)
	{
		pgm_instance_telemetry telemetry[INSTANCE_COUNT] =
		{
			PGM_INSTANCE_TELEMETRY_INIT,
			PGM_INSTANCE_TELEMETRY_INIT,
		};
		bool both_entered = true;
		bool both_active = true;

		for (size_t index = 0; index < INSTANCE_COUNT; index++)
		{
			if (pgm_instance_get_telemetry(
					fixtures[index].instance, &telemetry[index], NULL) !=
				PGM_STATUS_OK)
				return false;
			if (telemetry[index].active_request_count == 0)
				both_active = false;
			if (telemetry[index].running_session_count == 0)
				both_entered = false;
		}
		if (both_active && both_entered)
			return true;
		(void) nanosleep(&interval, NULL);
	}
	return false;
}


static bool
prove_cancel_isolation(
	InstanceFixture fixtures[INSTANCE_COUNT], bool *peer_in_flight)
{
	pgm_request *requests[INSTANCE_COUNT] = {NULL, NULL};
	pgm_result *results[INSTANCE_COUNT] = {NULL, NULL};
	pgm_error  *errors[INSTANCE_COUNT] = {NULL, NULL};
	pgm_instance_telemetry peer_telemetry = PGM_INSTANCE_TELEMETRY_INIT;
	pgm_status	status;
	bool		completed[INSTANCE_COUNT] = {false, false};
	bool		success = false;

	if (peer_in_flight == NULL)
		return false;
	*peer_in_flight = false;
	status = pgm_execute_async(
		fixtures[0].connections[1], "SELECT 1 FROM pg_sleep(10)",
		NULL, 0, PGM_FORMAT_TEXT, &requests[0], &errors[0]);
	if (status != PGM_STATUS_OK)
	{
		(void) fail_status("start cancellable request", status, errors[0]);
		goto done;
	}
	status = pgm_execute_async(
		fixtures[1].connections[1],
		"SELECT marker::text FROM postgamma_release_identity, pg_sleep(5)",
		NULL, 0, PGM_FORMAT_TEXT, &requests[1], &errors[1]);
	if (status != PGM_STATUS_OK)
	{
		(void) fail_status("start peer request", status, errors[1]);
		goto done;
	}
	if (!observe_both_active(fixtures))
	{
		fprintf(stderr,
			"both instances were not active before request cancellation\n");
		goto done;
	}
	status = pgm_request_cancel(requests[0], &errors[0]);
	if (status != PGM_STATUS_OK)
	{
		(void) fail_status("cancel request", status, errors[0]);
		goto done;
	}
	pgm_error_free(errors[0]);
	errors[0] = NULL;
	status = pgm_instance_get_telemetry(
		fixtures[1].instance, &peer_telemetry, &errors[1]);
	if (status != PGM_STATUS_OK)
	{
		(void) fail_status(
			"observe peer after request cancellation", status, errors[1]);
		goto done;
	}
	if (peer_telemetry.active_request_count == 0 ||
		peer_telemetry.running_session_count == 0)
	{
		fprintf(stderr,
			"peer request was not in flight after request cancellation\n");
		goto done;
	}
	*peer_in_flight = true;
	status = pgm_request_wait(
		requests[0], TEST_TIMEOUT_MS, &results[0], &errors[0]);
	completed[0] = true;
	if (status != PGM_STATUS_CANCELED ||
		pgm_error_sqlstate(errors[0]) == NULL ||
		strcmp(pgm_error_sqlstate(errors[0]), "57014") != 0)
	{
		(void) fail_status("wait for canceled request", status, errors[0]);
		goto done;
	}
	status = pgm_request_wait(
		requests[1], TEST_TIMEOUT_MS, &results[1], &errors[1]);
	completed[1] = true;
	if (status != PGM_STATUS_OK ||
		!value_equals(results[1], 0, 0, fixtures[1].marker))
	{
		(void) fail_status("wait for active peer request", status, errors[1]);
		fprintf(stderr, "request cancellation crossed an instance boundary\n");
		goto done;
	}
	pgm_error_free(errors[1]);
	errors[1] = NULL;
	pgm_result_free(results[1]);
	results[1] = NULL;
	pgm_request_free(requests[1]);
	requests[1] = NULL;
	if (!execute_value(
			fixtures[1].connections[1],
			"SELECT marker::text FROM postgamma_release_identity",
			fixtures[1].marker))
	{
		fprintf(stderr, "peer instance was unusable after request cancellation\n");
		goto done;
	}
	success = true;

done:
	for (size_t index = 0; index < INSTANCE_COUNT; index++)
	{
		if (requests[index] != NULL && !completed[index])
		{
			pgm_error  *cleanup_error = NULL;
			pgm_result *cleanup_result = NULL;

			(void) pgm_request_cancel(requests[index], &cleanup_error);
			pgm_error_free(cleanup_error);
			cleanup_error = NULL;
			(void) pgm_request_wait(
				requests[index], TEST_TIMEOUT_MS,
				&cleanup_result, &cleanup_error);
			pgm_error_free(cleanup_error);
			pgm_result_free(cleanup_result);
		}
		pgm_error_free(errors[index]);
		pgm_result_free(results[index]);
		pgm_request_free(requests[index]);
	}
	return success;
}


static bool
checkpoint_instance(pgm_instance *instance)
{
	pgm_checkpoint_options options = PGM_CHECKPOINT_OPTIONS_INIT;
	pgm_operation *operation = NULL;
	pgm_operation_state state = PGM_OPERATION_PENDING;
	pgm_error  *error = NULL;
	pgm_status	status;
	int		waitable = -1;
	bool		success = false;

	status = pgm_instance_checkpoint_async(
		instance, &options, &operation, &error);
	if (status != PGM_STATUS_OK)
		goto done;
	status = pgm_operation_waitable(operation, &waitable, &error);
	if (status != PGM_STATUS_OK || waitable < 0)
		goto done;
	for (;;)
	{
		struct pollfd descriptor = {waitable, POLLIN, 0};

		status = pgm_operation_progress(operation, &state, &error);
		if (status != PGM_STATUS_OK)
			goto done;
		if (state == PGM_OPERATION_COMPLETED)
			break;
		if (state != PGM_OPERATION_RUNNING && state != PGM_OPERATION_PENDING)
			goto done;
		if (poll(&descriptor, 1, (int) TEST_TIMEOUT_MS) <= 0)
		{
			fprintf(stderr, "checkpoint waitable timed out\n");
			goto done;
		}
	}
	success = true;

done:
	if (!success && status != PGM_STATUS_OK)
		(void) fail_status("checkpoint instance", status, error);
	pgm_error_free(error);
	pgm_operation_free(operation);
	return success;
}


static void
notice_callback(void *argument, const pgm_notice *notice)
{
	CallbackState *state = argument;

	if (state == NULL || notice == NULL)
		return;
	atomic_fetch_add_explicit(
		&state->notice_count, UINT32_C(1), memory_order_relaxed);
}


static void
notification_callback(void *argument, const pgm_notification *notification)
{
	CallbackState *state = argument;

	if (state == NULL || notification == NULL)
		return;
	if (notification->payload == NULL ||
		strcmp(notification->payload, state->expected_payload) != 0)
	{
		atomic_fetch_add_explicit(
			&state->wrong_notification_count,
			UINT32_C(1), memory_order_relaxed);
	}
	atomic_fetch_add_explicit(
		&state->notification_count, UINT32_C(1), memory_order_relaxed);
}


static void *
query_thread_main(void *argument)
{
	QueryThread *state = argument;
	pgm_result *result = NULL;
	pgm_error  *error = NULL;

	atomic_store_explicit(&state->entered, UINT32_C(1), memory_order_release);
	state->status = pgm_execute(
		state->connection, state->sql, NULL, 0, PGM_FORMAT_TEXT,
		TEST_TIMEOUT_MS,
		&result, &error);
	if (state->status == PGM_STATUS_OK)
		state->matched = value_equals(result, 0, 0, state->expected);
	else
		(void) fail_status("concurrent query", state->status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return NULL;
}


static uint64_t
monotonic_milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t) now.tv_sec * UINT64_C(1000) +
		(uint64_t) now.tv_nsec / UINT64_C(1000000);
}
