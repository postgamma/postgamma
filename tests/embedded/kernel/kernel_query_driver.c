#include "postgamma/embedded_kernel.h"
#include "postgamma/private/data_directory_lock.h"
#include "postgamma/private/kernel_supervisor_adapter.h"
#include "postgamma/private/libpq_memory_adapter.h"
#include "postgamma/private/memory_transport.h"
#include "postgamma/private/server_transport_adapter.h"
#include "postgamma/private/supervisor.h"
#include "postgamma/thread_runtime.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#define TEST_GENERATION UINT64_C(620001)
#define TEST_CONNECTION_ID UINT64_C(1)
#define TEST_QUEUE_CAPACITY (64U * 1024U)
#define TEST_TIMEOUT_NS UINT64_C(30000000000)
#define TEST_CANCEL_TIMEOUT_NS UINT64_C(2000000000)
#define TEST_POLL_INTERVAL_NS 1000000L
#define TEST_TRANSACTION_IDLE ((char) 0)
#define TEST_TRANSACTION_IN_PROGRESS ((char) 2)


typedef struct KernelDriver
{
	PostgammaKernelBootOptions options;
	PostgammaKernelHostProvider host;
	PostgammaKernelResult result;
	const PostgammaKernelEntrypoints *entrypoints;
	PostgammaThreadStackInfo supervisor_stack;
	int			supervisor_stack_status;
} KernelDriver;


typedef struct CancelQueryDriver
{
	PostgammaPrivateLibpqConnection *connection;
	PostgammaPrivateOwnedResult *result;
	PostgammaPrivateLibpqStatus status;
} CancelQueryDriver;


typedef enum DriverMode
{
	DRIVER_MODE_QUERY = 0,
	DRIVER_MODE_CRASH_WRITER,
	DRIVER_MODE_CRASH_VERIFIER
} DriverMode;


static uint64_t supervisor_deadline(void);
static bool libpq_deadline(int64_t *deadline);
static int run_kernel(PostgammaSupervisor *supervisor, void *argument);
static int cancel_via_supervisor(
	void *argument, uint64_t connection_generation,
	uint64_t request_generation, int backend_pid);
static void *run_cancel_query(void *argument);
static bool result_is_one(const PostgammaPrivateQueryResult *result);
static bool result_is_division_by_zero(
	const PostgammaPrivateQueryResult *result);
static bool query_returns_value(
	PostgammaPrivateLibpqConnection *connection,
	const char *query, const char *expected,
	PostgammaPrivateQueryResult *result);
static bool query_returns_value_in_state(
	PostgammaPrivateLibpqConnection *connection,
	const char *query, const char *expected, char transaction_status,
	PostgammaPrivateQueryResult *result);
static bool query_returns_command(
	PostgammaPrivateLibpqConnection *connection,
	const char *query, const char *expected_command,
	char transaction_status, PostgammaPrivateQueryResult *result);
static int run_crash_writer(
	PostgammaPrivateLibpqConnection *connection, int backend_pid);
static int run_crash_verifier(
	PostgammaPrivateLibpqConnection *connection);
static bool memory_telemetry_equal(
	const PostgammaMemoryGlobalTelemetry *left,
	const PostgammaMemoryGlobalTelemetry *right);
static int wait_for_memory_baseline(
	const PostgammaMemoryGlobalTelemetry *baseline);
static int wait_and_destroy_ticket(
	PostgammaSupervisorTicket **ticket, int *operation_status);
static int stop_supervisor(PostgammaSupervisor *supervisor);
static int fail(const char *operation, int status);


int
main(int argument_count, char **arguments)
{
	PostgammaMemoryGlobalTelemetry memory_before;
	PostgammaMemoryGlobalTelemetry memory_after;
	PostgammaDataDirectoryLockOptions lock_options =
		POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT;
	PostgammaDataDirectoryLockTelemetry lock_telemetry;
	PostgammaSupervisorOptions supervisor_options =
		POSTGAMMA_SUPERVISOR_OPTIONS_INIT;
	PostgammaPrivateLibpqOptions libpq_options;
	PostgammaPrivateLibpqTelemetry libpq_telemetry;
	const PostgammaKernelSetting instance_settings[] =
	{
		{"max_connections", "5"},
	};
	PostgammaKernelConnectRequest connect_request =
		POSTGAMMA_KERNEL_CONNECT_REQUEST_INIT;
	PostgammaPrivateQueryResult query_result;
	PostgammaDataDirectoryLock *data_lock = NULL;
	PostgammaSupervisor *supervisor = NULL;
	PostgammaSupervisorTicket *connect_ticket = NULL;
	PostgammaPrivateLibpqConnection *connection = NULL;
	PostgammaPrivateOwnedResult *owned_result = NULL;
	PostgammaThread *cancel_thread = NULL;
	PostgammaMemoryEndpoint *backend_endpoint = NULL;
	PostgammaSupervisorTelemetry supervisor_telemetry;
	const PostgammaKernelEntrypoints *entrypoints;
	KernelDriver driver;
	CancelQueryDriver cancel_driver;
	PostgammaPrivateLibpqStatus libpq_status;
	int64_t		deadline;
	int			operation_status = 0;
	int			backend_pid = 0;
	int			status = 0;
	char		error_sqlstate[sizeof(query_result.sqlstate)] = {0};
	bool		connected = false;
	bool		query_one_passed = false;
	bool		error_passed = false;
	bool		recovery_query_passed = false;
	bool		extended_parameters_passed = false;
	bool		dynamic_result_passed = false;
	bool		binary_result_passed = false;
	bool		cancellation_passed = false;
	bool		settings_precedence_passed = false;
	bool		safety_policy_passed = false;
	bool		crash_recovery_passed = false;
	bool		dedicated_executor = false;
	DriverMode	mode = DRIVER_MODE_QUERY;
	int			path_argument = 1;

	if (argument_count == 5 && strcmp(arguments[1], "dedicated-query") == 0)
	{
		dedicated_executor = true;
		path_argument = 2;
	}
	else if (argument_count == 5 && strcmp(arguments[1], "crash-writer") == 0)
	{
		mode = DRIVER_MODE_CRASH_WRITER;
		path_argument = 2;
	}
	else if (argument_count == 5 &&
		strcmp(arguments[1], "crash-verifier") == 0)
	{
		mode = DRIVER_MODE_CRASH_VERIFIER;
		path_argument = 2;
	}
	else if (argument_count != 4)
	{
		fprintf(stderr,
			"usage: %s [dedicated-query|crash-writer|crash-verifier]"
			" DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT\n",
			arguments[0]);
		return 2;
	}
	memset(&driver, 0, sizeof(driver));
	memset(&cancel_driver, 0, sizeof(cancel_driver));
	memset(&lock_telemetry, 0, sizeof(lock_telemetry));
	memset(&libpq_options, 0, sizeof(libpq_options));
	memset(&libpq_telemetry, 0, sizeof(libpq_telemetry));
	memset(&query_result, 0, sizeof(query_result));
	memset(&supervisor_telemetry, 0, sizeof(supervisor_telemetry));
	postgamma_memory_global_telemetry(&memory_before);
	entrypoints = postgamma_embedded_kernel_entrypoints();
	if (entrypoints == NULL ||
		entrypoints->struct_size != sizeof(*entrypoints) ||
		entrypoints->abi_version != POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION ||
		entrypoints->instance_main == NULL ||
		(entrypoints->capabilities &
		 POSTGAMMA_KERNEL_CAP_SERVER_LIFECYCLE) == 0 ||
		(entrypoints->capabilities &
		 POSTGAMMA_KERNEL_CAP_MEMORY_PROTOCOL) == 0)
	{
		return fail("validate kernel entrypoints", EPROTO);
	}

	driver.options = (PostgammaKernelBootOptions)
		POSTGAMMA_KERNEL_BOOT_OPTIONS_INIT;
	if (dedicated_executor)
	{
		driver.options.executor_kind = POSTGAMMA_KERNEL_EXECUTOR_DEDICATED;
		driver.options.executor_worker_count = 0;
		driver.options.execution_queue_capacity = 0;
	}
	driver.host = (PostgammaKernelHostProvider)
		POSTGAMMA_KERNEL_HOST_PROVIDER_INIT;
	driver.result = (PostgammaKernelResult)
		POSTGAMMA_KERNEL_RESULT_INIT;
	driver.entrypoints = entrypoints;
	lock_options.generation = TEST_GENERATION;
	lock_options.path = arguments[path_argument];
	status = postgamma_data_directory_lock_acquire(
		&lock_options, &data_lock);
	if (status != 0)
		goto cleanup;
	status = postgamma_data_directory_lock_telemetry(
		data_lock, &lock_telemetry);
	if (status != 0)
		goto cleanup;

	supervisor_options.generation = TEST_GENERATION;
	supervisor_options.queue_capacity = 16;
	supervisor_options.callbacks.run = run_kernel;
	supervisor_options.callback_argument = &driver;
	status = postgamma_supervisor_create(
		&supervisor_options, &supervisor);
	if (status != 0)
		goto cleanup;
	status = postgamma_kernel_supervisor_provider_init(
		supervisor, &driver.host);
	if (status != 0)
		goto cleanup;
	driver.options.generation = TEST_GENERATION;
	driver.options.data_directory_fd =
		lock_telemetry.directory_descriptor;
	driver.options.logical_umask = 0077;
	driver.options.data_directory = lock_telemetry.canonical_path;
	driver.options.executable_path = arguments[path_argument + 1];
	driver.options.resource_root = arguments[path_argument + 2];
	driver.options.settings = instance_settings;
	driver.options.setting_count =
		sizeof(instance_settings) / sizeof(instance_settings[0]);
	driver.options.host = &driver.host;
	status = postgamma_supervisor_start(supervisor, supervisor_deadline());
	if (status != 0)
		goto cleanup;

	libpq_options = (PostgammaPrivateLibpqOptions) {
		.generation = TEST_GENERATION,
		.queue_capacity = TEST_QUEUE_CAPACITY,
		.user = "postgamma",
		.database = "postgres",
		.application_name = mode == DRIVER_MODE_QUERY ?
			"postgamma-kernel-query-gate" : "postgamma-kernel-crash-oracle",
		.cancel_callback = cancel_via_supervisor,
		.cancel_argument = supervisor,
	};
	libpq_status = postgamma_private_libpq_create(
		&libpq_options, &connection, &backend_endpoint);
	if (libpq_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		fprintf(stderr, "private libpq create failed: %s\n",
			postgamma_private_libpq_status_name(libpq_status));
		status = EPROTO;
		goto cleanup;
	}
	status = postgamma_memory_server_connect_init(
		TEST_GENERATION, backend_endpoint, &connect_request);
	if (status != 0)
		goto cleanup;
	connect_request.connection_id = TEST_CONNECTION_ID;
	status = postgamma_supervisor_submit(
		supervisor, TEST_GENERATION, POSTGAMMA_SUPERVISOR_CONTROL_CONNECT,
		&connect_request, &connect_ticket);
	if (status != 0)
		goto cleanup;
	status = wait_and_destroy_ticket(&connect_ticket, &operation_status);
	if (status == 0 && operation_status != 0)
		status = operation_status;
	if (status != 0)
		goto cleanup;
	backend_pid = connect_request.backend_pid;
	if (backend_pid <= 0)
	{
		status = EPROTO;
		goto cleanup;
	}
	if (postgamma_memory_endpoint_release(
			&backend_endpoint, TEST_GENERATION) != POSTGAMMA_MEMORY_STATUS_OK)
	{
		status = EPROTO;
		goto cleanup;
	}
	connect_request.transport = NULL;

	if (!libpq_deadline(&deadline))
	{
		status = EOVERFLOW;
		goto cleanup;
	}
	libpq_status = postgamma_private_libpq_connect(connection, deadline);
	if (libpq_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		(void) postgamma_private_libpq_telemetry(
			connection, &libpq_telemetry);
		fprintf(stderr, "private libpq connect failed: %s: %s\n",
			postgamma_private_libpq_status_name(libpq_status),
			libpq_telemetry.last_error);
		status = EPROTO;
		goto cleanup;
	}
	connected = true;

	if (mode == DRIVER_MODE_CRASH_WRITER)
	{
		status = run_crash_writer(connection, backend_pid);
		goto cleanup;
	}
	if (mode == DRIVER_MODE_CRASH_VERIFIER)
	{
		status = run_crash_verifier(connection);
		if (status != 0)
			goto cleanup;
		crash_recovery_passed = true;
	}
	else
	{
		if (!libpq_deadline(&deadline) ||
			postgamma_private_libpq_query(
				connection, "SELECT 1", deadline, &query_result) !=
				POSTGAMMA_PRIVATE_LIBPQ_OK || !result_is_one(&query_result))
		{
			fprintf(stderr, "initial SELECT 1 result is invalid\n");
			status = EPROTO;
			goto cleanup;
		}
		query_one_passed = true;
		{
			static const char text_value[] = "embedded";
			const PostgammaPrivateParameter parameters[] =
			{
				{
					.type_oid = UINT32_C(25),
					.format = 0,
					.is_null = false,
					.data = text_value,
					.length = sizeof(text_value) - 1,
				},
				{
					.type_oid = UINT32_C(25),
					.format = 0,
					.is_null = true,
					.data = NULL,
					.length = 0,
				},
			};
			const unsigned char expected_binary[] = {0, 0, 0, 42};
			const PostgammaPrivateParameter binary_parameter =
			{
				.type_oid = UINT32_C(23),
				.format = 0,
				.is_null = false,
				.data = "42",
				.length = 2,
			};

			if (!libpq_deadline(&deadline) ||
				postgamma_private_libpq_query_params(
					connection, UINT64_C(1),
					"SELECT g::int4 AS ordinal, $1::text AS payload,"
					" $2::text IS NULL AS missing"
					" FROM generate_series(1, 3) AS g ORDER BY g",
					parameters, sizeof(parameters) / sizeof(parameters[0]),
					0, deadline, &owned_result) != POSTGAMMA_PRIVATE_LIBPQ_OK ||
				owned_result == NULL || owned_result->rows != 3 ||
				owned_result->columns != 3 ||
				owned_result->status != POSTGAMMA_PRIVATE_RESULT_TUPLES_OK ||
				strcmp(owned_result->command_status, "SELECT 3") != 0 ||
				strcmp(owned_result->fields[0].name, "ordinal") != 0 ||
				owned_result->fields[0].type_oid != UINT32_C(23) ||
				strcmp(owned_result->fields[1].name, "payload") != 0 ||
				owned_result->fields[1].type_oid != UINT32_C(25) ||
				strcmp(owned_result->fields[2].name, "missing") != 0 ||
				owned_result->fields[2].type_oid != UINT32_C(16) ||
				owned_result->values[0].is_null ||
				strcmp((const char *) owned_result->values[0].data, "1") != 0 ||
				owned_result->values[1].is_null ||
				strcmp((const char *) owned_result->values[1].data,
					"embedded") != 0 ||
				owned_result->values[2].is_null ||
				strcmp((const char *) owned_result->values[2].data, "t") != 0 ||
				strcmp((const char *) owned_result->values[6].data, "3") != 0)
			{
				fprintf(stderr, "extended parameter result is invalid\n");
				status = EPROTO;
				goto cleanup;
			}
			extended_parameters_passed = true;
			dynamic_result_passed = true;
			postgamma_private_libpq_result_free(owned_result);
			owned_result = NULL;
			if (!libpq_deadline(&deadline) ||
				postgamma_private_libpq_query_params(
					connection, UINT64_C(2), "SELECT $1::int4 AS answer",
					&binary_parameter, 1, 1, deadline, &owned_result) !=
					POSTGAMMA_PRIVATE_LIBPQ_OK ||
				owned_result == NULL || owned_result->rows != 1 ||
				owned_result->columns != 1 ||
				owned_result->fields[0].format != 1 ||
				owned_result->values[0].is_null ||
				owned_result->values[0].length != sizeof(expected_binary) ||
				memcmp(owned_result->values[0].data, expected_binary,
					sizeof(expected_binary)) != 0)
			{
				fprintf(stderr, "binary parameter result is invalid\n");
				status = EPROTO;
				goto cleanup;
			}
			binary_result_passed = true;
			postgamma_private_libpq_result_free(owned_result);
			owned_result = NULL;
		}
		{
			PostgammaThreadAttributes attributes =
			{
				.name = "pgm-cancel-test",
				.role = POSTGAMMA_THREAD_ROLE_FRONTEND,
				.stack_size = 0,
				.guard_size = 0,
			};
			struct timespec pause = {0, TEST_POLL_INTERVAL_NS};
			uint64_t cancel_started = postgamma_monotonic_now_ns();
			uint64_t cancel_deadline;

			if (cancel_started == 0 ||
				cancel_started > UINT64_MAX - TEST_CANCEL_TIMEOUT_NS)
			{
				status = EIO;
				goto cleanup;
			}
			cancel_deadline = cancel_started + TEST_CANCEL_TIMEOUT_NS;

			cancel_driver.connection = connection;
			status = postgamma_thread_create(
				&cancel_thread, &attributes, run_cancel_query, &cancel_driver);
			if (status != 0)
				goto cleanup;
			while (postgamma_private_libpq_active_request_generation(connection) !=
				UINT64_C(3))
			{
				uint64_t now = postgamma_monotonic_now_ns();

				if (now == 0 ||
					(cancel_deadline != POSTGAMMA_SUPERVISOR_NO_DEADLINE &&
					 now >= cancel_deadline))
				{
					status = ETIMEDOUT;
					goto cleanup;
				}
				(void) nanosleep(&pause, NULL);
			}
			for (;;)
			{
				if (postgamma_private_libpq_cancel(
						connection, UINT64_C(3)) !=
					POSTGAMMA_PRIVATE_LIBPQ_OK)
				{
					status = EPROTO;
					goto cleanup;
				}
				(void) nanosleep(&pause, NULL);
				if (postgamma_private_libpq_active_request_generation(
						connection) != UINT64_C(3))
					break;
				if (postgamma_monotonic_now_ns() >= cancel_deadline)
				{
					status = ETIMEDOUT;
					goto cleanup;
				}
			}
			status = postgamma_thread_join(cancel_thread, NULL);
			if (status == 0)
				status = postgamma_thread_destroy(cancel_thread);
			if (status != 0)
				goto cleanup;
			cancel_thread = NULL;
			if (cancel_driver.status != POSTGAMMA_PRIVATE_LIBPQ_OK ||
				cancel_driver.result == NULL ||
				cancel_driver.result->status !=
					POSTGAMMA_PRIVATE_RESULT_FATAL_ERROR ||
				strcmp(cancel_driver.result->sqlstate, "57014") != 0 ||
				strstr(cancel_driver.result->message, "canceling statement") == NULL)
			{
				fprintf(stderr, "cancelled query result is invalid\n");
				status = EPROTO;
				goto cleanup;
			}
			postgamma_private_libpq_result_free(cancel_driver.result);
			cancel_driver.result = NULL;
			if (!query_returns_value(
					connection, "SELECT 1", "1", &query_result))
			{
				fprintf(stderr, "connection did not recover after cancellation\n");
				status = EPROTO;
				goto cleanup;
			}
			cancellation_passed = true;
		}
		if (!query_returns_value(
				connection, "SHOW shared_buffers", "24MB", &query_result) ||
			!query_returns_value(
				connection,
				"SELECT source FROM pg_settings WHERE name = 'shared_buffers'",
				"configuration file", &query_result) ||
			!query_returns_value(
				connection, "SHOW max_connections", "5", &query_result) ||
			!query_returns_value(
				connection,
				"SELECT source FROM pg_settings WHERE name = 'max_connections'",
				"command line", &query_result) ||
			!query_returns_value(
				connection, "SHOW max_worker_processes", "4", &query_result) ||
			!query_returns_value(
				connection,
				"SELECT source FROM pg_settings"
				" WHERE name = 'max_worker_processes'",
				"default", &query_result))
		{
			fprintf(stderr, "embedded setting precedence is invalid\n");
			status = EPROTO;
			goto cleanup;
		}
		settings_precedence_passed = true;
		if (!query_returns_value(
				connection, "SHOW restart_after_crash", "off", &query_result) ||
			!query_returns_value(
				connection,
				"SELECT source FROM pg_settings"
				" WHERE name = 'restart_after_crash'",
				"override", &query_result))
		{
			fprintf(stderr, "embedded safety policy is not an override\n");
			status = EPROTO;
			goto cleanup;
		}
		safety_policy_passed = true;
		if (!libpq_deadline(&deadline) ||
			postgamma_private_libpq_query(
				connection, "SELECT 1 / 0", deadline, &query_result) !=
				POSTGAMMA_PRIVATE_LIBPQ_OK ||
			!result_is_division_by_zero(&query_result))
		{
			fprintf(stderr,
				"division-by-zero result is invalid: status=%d sqlstate=%s"
				" severity=%s message=%s\n",
				(int) query_result.status, query_result.sqlstate,
				query_result.severity, query_result.message);
			status = EPROTO;
			goto cleanup;
		}
		memcpy(error_sqlstate, query_result.sqlstate, sizeof(error_sqlstate));
		error_passed = true;
		if (!libpq_deadline(&deadline) ||
			postgamma_private_libpq_query(
				connection, "SELECT 1", deadline, &query_result) !=
				POSTGAMMA_PRIVATE_LIBPQ_OK || !result_is_one(&query_result))
		{
			fprintf(stderr, "SELECT 1 did not recover after the SQL error\n");
			status = EPROTO;
			goto cleanup;
		}
		recovery_query_passed = true;
	}
	libpq_status = postgamma_private_libpq_telemetry(
		connection, &libpq_telemetry);
	if (libpq_status != POSTGAMMA_PRIVATE_LIBPQ_OK ||
		libpq_telemetry.backend_pid != backend_pid ||
		libpq_telemetry.secure_read_calls == 0 ||
		libpq_telemetry.secure_write_calls == 0 ||
		libpq_telemetry.socket_wait_calls == 0 ||
		libpq_telemetry.network_connect_calls != 0 ||
		libpq_telemetry.optional_security_calls != 0 ||
		(mode == DRIVER_MODE_QUERY &&
		 libpq_telemetry.cancel_dispatches == UINT64_C(0)))
	{
		fprintf(stderr,
			"private libpq telemetry is invalid: pid=%d reads=%" PRIu64
			" writes=%" PRIu64 " waits=%" PRIu64
			" network=%" PRIu64 " security=%" PRIu64 "\n",
			libpq_telemetry.backend_pid,
			libpq_telemetry.secure_read_calls,
			libpq_telemetry.secure_write_calls,
			libpq_telemetry.socket_wait_calls,
			libpq_telemetry.network_connect_calls,
			libpq_telemetry.optional_security_calls);
		status = EPROTO;
		goto cleanup;
	}
	libpq_status = postgamma_private_libpq_close(&connection);
	if (libpq_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		status = EPROTO;
		goto cleanup;
	}
	status = wait_for_memory_baseline(&memory_before);
	if (status != 0)
		goto cleanup;
	status = stop_supervisor(supervisor);
	if (status != 0)
		goto cleanup;
	status = postgamma_supervisor_telemetry(
		supervisor, &supervisor_telemetry);
	if (status != 0)
		goto cleanup;
	if (supervisor_telemetry.state != POSTGAMMA_SUPERVISOR_STATE_CLOSED ||
		supervisor_telemetry.threads_started != 1 ||
		supervisor_telemetry.threads_joined != 1 ||
		supervisor_telemetry.active_tickets != 0 ||
		driver.supervisor_stack_status != 0 ||
		driver.supervisor_stack.role != POSTGAMMA_THREAD_ROLE_SUPERVISOR ||
		driver.supervisor_stack.configured_stack_size <=
			driver.supervisor_stack.configured_guard_size ||
		driver.supervisor_stack.native_stack_size <
			driver.supervisor_stack.configured_stack_size ||
		driver.supervisor_stack.native_guard_size <
			driver.supervisor_stack.configured_guard_size ||
		driver.supervisor_stack.usable_stack_size !=
			driver.supervisor_stack.native_stack_size -
				driver.supervisor_stack.native_guard_size ||
		driver.supervisor_stack.stack_high_address <=
			driver.supervisor_stack.stack_low_address ||
		driver.supervisor_stack.stack_high_address -
			driver.supervisor_stack.stack_low_address !=
			driver.supervisor_stack.native_stack_size ||
		driver.result.status != 0 || driver.result.postgres_exit_code != 0 ||
		driver.result.cleanup_status != 0 ||
		driver.result.fault_point != POSTGAMMA_KERNEL_FAULT_NONE)
	{
		status = EPROTO;
		goto cleanup;
	}
	status = postgamma_supervisor_destroy(supervisor);
	if (status != 0)
		goto cleanup;
	supervisor = NULL;
	status = postgamma_data_directory_lock_release(data_lock);
	if (status != 0)
		goto cleanup;
	data_lock = NULL;
	postgamma_memory_global_telemetry(&memory_after);
	if (!memory_telemetry_equal(&memory_before, &memory_after))
		return fail("validate memory transport cleanup", EPROTO);
	if (mode == DRIVER_MODE_CRASH_VERIFIER)
	{
		printf(
			"POSTGAMMA_KERNEL_CRASH_RECOVERY generation=%" PRIu64
			" host_pid=%ld backend_pid=%d committed_rows=1"
			" uncommitted_rows=0 recovery_complete=%s"
			" transport=memory network_calls=%" PRIu64
			" active_transports_after_close=%" PRIu64
			" endpoint_references_after_close=%" PRIu64
			" phase=%s state=%s\n",
			TEST_GENERATION, (long) getpid(), backend_pid,
			crash_recovery_passed ? "true" : "false",
			libpq_telemetry.network_connect_calls,
			memory_after.active_transports,
			memory_after.endpoint_references,
			driver.result.phase,
			postgamma_supervisor_state_name(supervisor_telemetry.state));
		return 0;
	}

	printf(
		"POSTGAMMA_KERNEL_QUERY generation=%" PRIu64
		" backend_pid=%d executor=%s connected=%s select_one=%s"
			" error_sqlstate=%s error_recovery=%s"
			" extended_params=%s dynamic_result=%s binary_result=%s"
			" cancellation=%s cancel_dispatches=%" PRIu64
		" settings_precedence=%s safety_policy=%s"
		" transport=memory network_calls=%" PRIu64
		" secure_reads=%" PRIu64 " secure_writes=%" PRIu64
		" socket_waits=%" PRIu64
		" supervisor_configured_stack_bytes=%zu"
		" supervisor_configured_guard_bytes=%zu"
		" supervisor_native_stack_bytes=%zu"
		" supervisor_native_guard_bytes=%zu"
		" supervisor_usable_stack_bytes=%zu"
		" supervisor_actual_bounds=true"
		" active_transports_after_close=%" PRIu64
		" endpoint_references_after_close=%" PRIu64
		" phase=%s state=%s\n",
		TEST_GENERATION, backend_pid,
		dedicated_executor ? "dedicated" : "pooled",
		connected ? "true" : "false",
		query_one_passed ? "true" : "false", error_sqlstate,
			error_passed && recovery_query_passed ? "true" : "false",
			extended_parameters_passed ? "true" : "false",
			dynamic_result_passed ? "true" : "false",
			binary_result_passed ? "true" : "false",
			cancellation_passed ? "true" : "false",
			libpq_telemetry.cancel_dispatches,
		settings_precedence_passed ? "true" : "false",
		safety_policy_passed ? "true" : "false",
		libpq_telemetry.network_connect_calls,
		libpq_telemetry.secure_read_calls,
		libpq_telemetry.secure_write_calls,
		libpq_telemetry.socket_wait_calls,
		driver.supervisor_stack.configured_stack_size,
		driver.supervisor_stack.configured_guard_size,
		driver.supervisor_stack.native_stack_size,
		driver.supervisor_stack.native_guard_size,
		driver.supervisor_stack.usable_stack_size,
		memory_after.active_transports,
		memory_after.endpoint_references,
		driver.result.phase,
		postgamma_supervisor_state_name(supervisor_telemetry.state));
	return 0;

cleanup:
	if (status == 0)
		status = EIO;
	if (cancel_thread != NULL)
	{
		(void) postgamma_private_libpq_cancel(connection, UINT64_C(3));
		(void) postgamma_thread_join(cancel_thread, NULL);
		(void) postgamma_thread_destroy(cancel_thread);
		cancel_thread = NULL;
	}
	postgamma_private_libpq_result_free(cancel_driver.result);
	cancel_driver.result = NULL;
	if (connect_ticket != NULL)
	{
		int destroy_status =
			postgamma_supervisor_ticket_destroy(connect_ticket);

		if (destroy_status != 0 && destroy_status != EBUSY)
			status = destroy_status;
	}
	postgamma_private_libpq_result_free(owned_result);
	if (connection != NULL)
		(void) postgamma_private_libpq_close(&connection);
	if (backend_endpoint != NULL)
	{
		(void) postgamma_memory_endpoint_abort(
			backend_endpoint, TEST_GENERATION,
			POSTGAMMA_MEMORY_ABORT_SHUTDOWN);
		(void) postgamma_memory_endpoint_release(
			&backend_endpoint, TEST_GENERATION);
	}
	if (supervisor != NULL)
	{
		PostgammaSupervisorState state =
			postgamma_supervisor_state(supervisor);

		if (state == POSTGAMMA_SUPERVISOR_STATE_READY)
			(void) stop_supervisor(supervisor);
		else if (state != POSTGAMMA_SUPERVISOR_STATE_NEW &&
			state != POSTGAMMA_SUPERVISOR_STATE_CLOSED)
		{
			(void) postgamma_supervisor_fail(supervisor, status);
			(void) postgamma_supervisor_join(
				supervisor, supervisor_deadline());
		}
		(void) postgamma_supervisor_destroy(supervisor);
	}
	if (data_lock != NULL)
		(void) postgamma_data_directory_lock_release(data_lock);
	if (driver.result.diagnostic[0] != '\0')
		fprintf(stderr, "kernel phase=%s: %s\n",
			driver.result.phase, driver.result.diagnostic);
	return fail("embedded query lifecycle", status);
}


static uint64_t
supervisor_deadline(void)
{
	uint64_t	now = postgamma_monotonic_now_ns();

	if (now == 0 || now > UINT64_MAX - TEST_TIMEOUT_NS)
		return POSTGAMMA_SUPERVISOR_NO_DEADLINE;
	return now + TEST_TIMEOUT_NS;
}


static bool
libpq_deadline(int64_t *deadline)
{
	int64_t		now;

	if (deadline == NULL ||
		postgamma_memory_clock_now(&now) != POSTGAMMA_MEMORY_STATUS_OK ||
		now > INT64_MAX - (int64_t) TEST_TIMEOUT_NS)
		return false;
	*deadline = now + (int64_t) TEST_TIMEOUT_NS;
	return true;
}


static int
run_kernel(PostgammaSupervisor *supervisor, void *argument)
{
	KernelDriver *driver = argument;

	(void) supervisor;
	driver->supervisor_stack_status = postgamma_thread_current_stack_info(
		&driver->supervisor_stack);
	if (driver->supervisor_stack_status != 0)
		return driver->supervisor_stack_status;
	return driver->entrypoints->instance_main(
		&driver->options, &driver->result);
}


static int
cancel_via_supervisor(
	void *argument, uint64_t connection_generation,
	uint64_t request_generation, int backend_pid)
{
	PostgammaSupervisor *supervisor = argument;
	PostgammaKernelCancelRequest request =
		POSTGAMMA_KERNEL_CANCEL_REQUEST_INIT;
	PostgammaSupervisorTicket *ticket = NULL;
	int			operation_status = 0;
	int			status;

	if (supervisor == NULL || connection_generation == 0 ||
		request_generation == 0 || backend_pid <= 0)
		return EINVAL;
	request.generation = connection_generation;
	request.request_generation = request_generation;
	request.backend_pid = backend_pid;
	status = postgamma_supervisor_submit(
		supervisor, connection_generation,
		POSTGAMMA_SUPERVISOR_CONTROL_CANCEL, &request, &ticket);
	if (status == 0)
		status = wait_and_destroy_ticket(&ticket, &operation_status);
	if (status == 0 && operation_status != 0)
		status = operation_status;
	if (status == 0 && request.dispatched != 1)
		status = EPROTO;
	if (ticket != NULL)
		(void) postgamma_supervisor_ticket_destroy(ticket);
	return status;
}


static void *
run_cancel_query(void *argument)
{
	CancelQueryDriver *driver = argument;
	int64_t		deadline;

	if (driver == NULL || driver->connection == NULL ||
		!libpq_deadline(&deadline))
	{
		if (driver != NULL)
			driver->status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
		return NULL;
	}
	driver->status = postgamma_private_libpq_query_params(
		driver->connection, UINT64_C(3), "SELECT pg_sleep(30)",
		NULL, 0, 0, deadline, &driver->result);
	return NULL;
}


static bool
result_is_one(const PostgammaPrivateQueryResult *result)
{
	return result != NULL &&
		result->status == POSTGAMMA_PRIVATE_RESULT_TUPLES_OK &&
		result->rows == 1 && result->columns == 1 &&
		strcmp(result->value, "1") == 0 &&
		strcmp(result->command_status, "SELECT 1") == 0 &&
		result->transaction_status == 0;
}


static bool
result_is_division_by_zero(const PostgammaPrivateQueryResult *result)
{
	return result != NULL &&
		result->status == POSTGAMMA_PRIVATE_RESULT_FATAL_ERROR &&
		strcmp(result->sqlstate, "22012") == 0 &&
		strcmp(result->severity, "ERROR") == 0 &&
		strstr(result->message, "division by zero") != NULL &&
		result->transaction_status == 0;
}


static bool
query_returns_value(
	PostgammaPrivateLibpqConnection *connection,
	const char *query, const char *expected,
	PostgammaPrivateQueryResult *result)
{
	return query_returns_value_in_state(
		connection, query, expected, TEST_TRANSACTION_IDLE, result);
}


static bool
query_returns_value_in_state(
	PostgammaPrivateLibpqConnection *connection,
	const char *query, const char *expected, char transaction_status,
	PostgammaPrivateQueryResult *result)
{
	int64_t		deadline;
	PostgammaPrivateLibpqStatus query_status;

	if (connection == NULL || query == NULL || expected == NULL ||
		result == NULL || !libpq_deadline(&deadline))
		return false;
	query_status = postgamma_private_libpq_query(
		connection, query, deadline, result);
	if (query_status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
		result->status == POSTGAMMA_PRIVATE_RESULT_TUPLES_OK &&
		result->rows == 1 && result->columns == 1 &&
		strcmp(result->value, expected) == 0 &&
		result->transaction_status == transaction_status)
		return true;
	fprintf(stderr,
		"query value mismatch: query=%s expected=%s actual=%s"
		" query_status=%d result_status=%d rows=%d columns=%d"
		" expected_transaction_status=%d transaction_status=%d\n",
		query, expected, result->value, (int) query_status,
		(int) result->status, result->rows, result->columns,
		(int) transaction_status,
		result->transaction_status);
	return false;
}


static bool
query_returns_command(
	PostgammaPrivateLibpqConnection *connection,
	const char *query, const char *expected_command,
	char transaction_status, PostgammaPrivateQueryResult *result)
{
	int64_t		deadline;
	PostgammaPrivateLibpqStatus query_status;

	if (connection == NULL || query == NULL || expected_command == NULL ||
		result == NULL || !libpq_deadline(&deadline))
		return false;
	query_status = postgamma_private_libpq_query(
		connection, query, deadline, result);
	if (query_status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
		result->status == POSTGAMMA_PRIVATE_RESULT_COMMAND_OK &&
		strcmp(result->command_status, expected_command) == 0 &&
		result->transaction_status == transaction_status)
		return true;
	fprintf(stderr,
		"query command mismatch: query=%s expected=%s actual=%s"
		" query_status=%d result_status=%d"
		" expected_transaction_status=%d transaction_status=%d\n",
		query, expected_command, result->command_status, (int) query_status,
		(int) result->status, (int) transaction_status,
		result->transaction_status);
	return false;
}


static int
run_crash_writer(
	PostgammaPrivateLibpqConnection *connection, int backend_pid)
{
	PostgammaPrivateQueryResult result;
	struct timespec pause = {60, 0};

	memset(&result, 0, sizeof(result));
	if (!query_returns_value(
			connection, "SHOW synchronous_commit", "on", &result) ||
		!query_returns_command(
			connection,
			"CREATE TABLE postgamma_crash_oracle"
			" (id integer PRIMARY KEY, payload text NOT NULL)",
			"CREATE TABLE", TEST_TRANSACTION_IDLE, &result) ||
		!query_returns_command(
			connection,
			"INSERT INTO postgamma_crash_oracle"
			" VALUES (1, 'committed')",
			"INSERT 0 1", TEST_TRANSACTION_IDLE, &result) ||
		!query_returns_command(
			connection, "BEGIN", "BEGIN",
			TEST_TRANSACTION_IN_PROGRESS, &result) ||
		!query_returns_command(
			connection,
			"INSERT INTO postgamma_crash_oracle"
			" VALUES (2, 'uncommitted')",
			"INSERT 0 1", TEST_TRANSACTION_IN_PROGRESS, &result) ||
		!query_returns_value_in_state(
			connection,
			"SELECT count(*) FROM postgamma_crash_oracle",
			"2", TEST_TRANSACTION_IN_PROGRESS, &result))
		return EPROTO;
	printf(
		"POSTGAMMA_KERNEL_CRASH_READY generation=%" PRIu64
		" host_pid=%ld backend_pid=%d committed_ack=true"
		" uncommitted_visible=true synchronous_commit=on\n",
		TEST_GENERATION, (long) getpid(), backend_pid);
	if (fflush(stdout) != 0)
		return errno != 0 ? errno : EIO;
	for (;;)
	{
		while (nanosleep(&pause, &pause) != 0)
		{
			if (errno != EINTR)
				return errno != 0 ? errno : EIO;
		}
		pause.tv_sec = 60;
		pause.tv_nsec = 0;
	}
}


static int
run_crash_verifier(PostgammaPrivateLibpqConnection *connection)
{
	PostgammaPrivateQueryResult result;

	memset(&result, 0, sizeof(result));
	if (!query_returns_value(
			connection,
			"SELECT count(*) FROM postgamma_crash_oracle",
			"1", &result) ||
		!query_returns_value(
			connection,
			"SELECT count(*) FROM postgamma_crash_oracle"
			" WHERE id = 1 AND payload = 'committed'",
			"1", &result) ||
		!query_returns_value(
			connection,
			"SELECT count(*) FROM postgamma_crash_oracle WHERE id = 2",
			"0", &result) ||
		!query_returns_value(
			connection, "SELECT pg_is_in_recovery()", "f", &result))
		return EPROTO;
	return 0;
}


static bool
memory_telemetry_equal(
	const PostgammaMemoryGlobalTelemetry *left,
	const PostgammaMemoryGlobalTelemetry *right)
{
	return left != NULL && right != NULL &&
		left->active_transports == right->active_transports &&
		left->endpoint_references == right->endpoint_references &&
		left->allocated_bytes == right->allocated_bytes;
}


static int
wait_for_memory_baseline(const PostgammaMemoryGlobalTelemetry *baseline)
{
	struct timespec pause = {0, TEST_POLL_INTERVAL_NS};
	uint64_t	deadline = supervisor_deadline();

	for (;;)
	{
		PostgammaMemoryGlobalTelemetry current;

		postgamma_memory_global_telemetry(&current);
		if (memory_telemetry_equal(baseline, &current))
			return 0;
		if (deadline != POSTGAMMA_SUPERVISOR_NO_DEADLINE &&
			postgamma_monotonic_now_ns() >= deadline)
		{
			fprintf(stderr,
				"memory transport did not quiesce: transports=%" PRIu64
				" references=%" PRIu64 " bytes=%" PRIu64 "\n",
				current.active_transports, current.endpoint_references,
				current.allocated_bytes);
			return ETIMEDOUT;
		}
		while (nanosleep(&pause, &pause) != 0)
		{
			if (errno != EINTR)
				return errno != 0 ? errno : EIO;
		}
		pause.tv_sec = 0;
		pause.tv_nsec = TEST_POLL_INTERVAL_NS;
	}
}


static int
wait_and_destroy_ticket(
	PostgammaSupervisorTicket **ticket, int *operation_status)
{
	int			status;

	if (ticket == NULL || *ticket == NULL || operation_status == NULL)
		return EINVAL;
	status = postgamma_supervisor_ticket_wait(
		*ticket, supervisor_deadline(), operation_status);
	if (status != 0)
		return status;
	status = postgamma_supervisor_ticket_destroy(*ticket);
	if (status == 0)
		*ticket = NULL;
	return status;
}


static int
stop_supervisor(PostgammaSupervisor *supervisor)
{
	PostgammaSupervisorTicket *ticket = NULL;
	int			operation_status = 0;
	int			status;

	if (supervisor == NULL)
		return EINVAL;
	status = postgamma_supervisor_request_shutdown(
		supervisor, TEST_GENERATION, POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST,
		&ticket);
	if (status != 0)
		return status;
	status = wait_and_destroy_ticket(&ticket, &operation_status);
	if (status == 0 && operation_status != 0)
		status = operation_status;
	if (status != 0)
		return status;
	return postgamma_supervisor_join(supervisor, supervisor_deadline());
}


static int
fail(const char *operation, int status)
{
	fprintf(stderr, "%s failed: %s (%d)\n",
		operation, strerror(status), status);
	return 1;
}
