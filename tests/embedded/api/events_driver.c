#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include <dirent.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define TEST_TIMEOUT_MS INT64_C(30000)
#define EVENT_QUEUE_CAPACITY 64U
#define SCALE_CONNECTIONS 1000U
#define TRACE_CONNECTIONS 32U


typedef struct CallbackState
{
	pgm_instance *instance;
	pthread_t	host_thread;
	pgm_connection_id expected_connection_id;
	pgm_request_id expected_request_id;
	pgm_status	reentrant_status;
	size_t		log_count;
	size_t		connection_notice_count;
	size_t		request_notice_count;
	size_t		notification_count;
	bool		wrong_thread;
	bool		routed_log;
	bool		request_notice_fields;
	bool		connection_notice_fields;
	bool		notification_fields;
	char		last_notification_payload[128];
} CallbackState;


static int report_failure(
	const char *operation, pgm_status status, const pgm_error *error);
static bool execute_command(pgm_connection *connection, const char *sql);
static bool execute_text(
	pgm_connection *connection, const char *sql, const char *expected);
static bool execute_notice(
	pgm_connection *connection, const char *sql,
	pgm_notice_callback callback, CallbackState *state,
	pgm_request_id *request_id);
static bool diagnostic_contract(pgm_connection *connection);
static bool drain_instance(pgm_instance *instance, size_t *drained);
static bool wait_for_notification_event(
	pgm_instance *instance, pgm_connection_id connection_id,
	const char *payload, size_t *polled_notifications);
static bool wait_for_overflow_event(
	pgm_instance *instance, uint64_t *reported_drops);
static bool dispatch_all(pgm_instance *instance, size_t *dispatched);
static uint64_t monotonic_milliseconds(void);
static size_t open_file_descriptor_count(void);
static void observe_callback(CallbackState *state);
static void receive_log(void *argument, const pgm_log_record *record);
static void receive_connection_notice(
	void *argument, const pgm_notice *notice);
static void receive_request_notice(void *argument, const pgm_notice *notice);
static void receive_notification(
	void *argument, const pgm_notification *notification);


int
main(int argument_count, char **arguments)
{
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_connection_options listener_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_instance *instance = NULL;
	pgm_connection *connection = NULL;
	pgm_connection *listener = NULL;
	pgm_connection **scale_connections = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	CallbackState callback_state;
	pgm_setting instance_settings[4];
	pgm_setting connection_settings[1];
	pgm_instance_telemetry telemetry = PGM_INSTANCE_TELEMETRY_INIT;
	pgm_instance_telemetry before_overflow = PGM_INSTANCE_TELEMETRY_INIT;
	pgm_event  *event = NULL;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	size_t		scale_count = SCALE_CONNECTIONS;
	size_t		opened_scale = 0;
	size_t		fd_before = 0;
	size_t		fd_after = 0;
	size_t		startup_polled = 0;
	size_t		dispatched = 0;
	size_t		polled_notifications = 0;
	uint64_t	overflow_reported = 0;
	int			instance_waitable = -1;
	int			request_waitable = -1;
	bool		trace_mode = false;
	bool		passed = false;

	if ((argument_count != 5 && argument_count != 6) ||
		strcmp(arguments[4], "create") != 0 ||
		(argument_count == 6 && strcmp(arguments[5], "trace") != 0))
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT "
			"create [trace]\n",
			arguments[0]);
		return 2;
	}
	trace_mode = argument_count == 6;
	if (trace_mode)
		scale_count = TRACE_CONNECTIONS;
	memset(&callback_state, 0, sizeof(callback_state));
	callback_state.host_thread = pthread_self();
	callback_state.reentrant_status = PGM_STATUS_INTERNAL_ERROR;

	if ((pgm_capabilities() &
		 (PGM_CAP_NOTIFICATIONS | PGM_CAP_REQUEST_NOTICES |
		  PGM_CAP_STATUS_TELEMETRY | PGM_CAP_INSTANCE_EVENTS)) !=
		(PGM_CAP_NOTIFICATIONS | PGM_CAP_REQUEST_NOTICES |
		 PGM_CAP_STATUS_TELEMETRY | PGM_CAP_INSTANCE_EVENTS))
	{
		fprintf(stderr, "runtime events capabilities are not advertised\n");
		goto fail;
	}

	{
		pgm_instance_options invalid = PGM_INSTANCE_OPTIONS_INIT;
		pgm_instance *unexpected = NULL;
		pgm_setting duplicates[2] = {
			{"max_connections", "16"},
			{"MAX_CONNECTIONS", "32"},
		};

		invalid.path = arguments[1];
		invalid.create = UINT32_C(1);
		invalid.executable_path = arguments[2];
		invalid.resource_root = arguments[3];
		invalid.settings = duplicates;
		invalid.setting_count = 2;
		status = pgm_instance_open(&invalid, &unexpected, &error);
		if (status != PGM_STATUS_INVALID_ARGUMENT || unexpected != NULL ||
			error == NULL || pgm_error_status(error) != status)
		{
			fprintf(stderr, "duplicate instance settings were not rejected\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
		invalid.settings = NULL;
		invalid.setting_count = 0;
		invalid.log_user_data = &callback_state;
		status = pgm_instance_open(&invalid, &unexpected, &error);
		if (status != PGM_STATUS_INVALID_ARGUMENT || unexpected != NULL)
		{
			fprintf(stderr, "orphan log user data was not rejected\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
	}

	{
		static char maximum_connections[32];

		(void) snprintf(
			maximum_connections, sizeof(maximum_connections), "%zu",
			scale_count + 16U);
		instance_settings[0] = (pgm_setting)
			{"max_connections", maximum_connections};
		instance_settings[1] = (pgm_setting)
			{"log_min_messages", "notice"};
		instance_settings[2] = (pgm_setting)
			{"search_path", "public"};
		instance_settings[3] = (pgm_setting)
			{"application_name", "instance-default"};
	}
	instance_options.path = arguments[1];
	instance_options.create = UINT32_C(1);
	instance_options.executable_path = arguments[2];
	instance_options.resource_root = arguments[3];
	instance_options.settings = instance_settings;
	instance_options.setting_count = 4;
	instance_options.event_queue_capacity = EVENT_QUEUE_CAPACITY;
	instance_options.log_callback = receive_log;
	instance_options.log_user_data = &callback_state;
	status = pgm_instance_open(&instance_options, &instance, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	callback_state.instance = instance;
	status = pgm_instance_waitable(instance, &instance_waitable, &error);
	if (status != PGM_STATUS_OK || instance_waitable < 0)
		goto fail;

	status = pgm_instance_next_event(
		instance, TEST_TIMEOUT_MS, &event, &availability, &error);
	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
		event == NULL || pgm_event_type(event) != PGM_EVENT_LOG ||
		callback_state.log_count != 0)
	{
		fprintf(stderr, "startup log was not available for polling\n");
		goto fail;
	}
	startup_polled++;
	pgm_event_free(event);
	event = NULL;
	if (!dispatch_all(instance, &dispatched) || callback_state.log_count == 0 ||
		callback_state.reentrant_status != PGM_STATUS_REENTRANT_CALL ||
		callback_state.wrong_thread ||
		!drain_instance(instance, &startup_polled))
	{
		fprintf(stderr, "log callback dispatch contract failed\n");
		goto fail;
	}

	connection_settings[0] = (pgm_setting) {"search_path", "pg_catalog"};
	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-c-api-events";
	connection_options.settings = connection_settings;
	connection_options.setting_count = 1;
	connection_options.notice_callback = receive_connection_notice;
	connection_options.notification_callback = receive_notification;
	connection_options.user_data = &callback_state;
	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	callback_state.expected_connection_id =
		pgm_connection_identity(connection);
	if (callback_state.expected_connection_id == 0)
	{
		fprintf(stderr, "connection identity is invalid\n");
		goto fail;
	}
	{
		const char *parameter_value = NULL;

		status = pgm_connection_parameter_status(
			connection, "application_name", &parameter_value, &error);
		if (status != PGM_STATUS_OK || parameter_value == NULL ||
			strcmp(parameter_value, "postgamma-c-api-events") != 0)
		{
			fprintf(stderr, "application_name parameter status is invalid\n");
			goto fail;
		}
		status = pgm_connection_parameter_status(
			connection, "server_version", &parameter_value, &error);
		if (status != PGM_STATUS_OK || parameter_value == NULL ||
			strncmp(parameter_value, "19", 2) != 0)
		{
			fprintf(stderr, "server_version parameter status is invalid\n");
			goto fail;
		}
	}
	if (!execute_text(connection, "SHOW search_path", "pg_catalog"))
		goto fail;

	{
		pgm_connection_options invalid = connection_options;
		pgm_connection *unexpected = NULL;
		pgm_setting duplicates[2] = {
			{"search_path", "public"},
			{"SEARCH_PATH", "pg_catalog"},
		};
		pgm_setting unknown[1] = {{"postgamma_missing_guc", "1"}};
		pgm_setting unsafe[1] = {{"max_connections", "2"}};

		invalid.settings = duplicates;
		invalid.setting_count = 2;
		status = pgm_connection_open(instance, &invalid, &unexpected, &error);
		if (status != PGM_STATUS_INVALID_ARGUMENT || unexpected != NULL)
		{
			fprintf(stderr, "duplicate connection settings were not rejected\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
		invalid.notice_callback = NULL;
		invalid.notification_callback = NULL;
		invalid.user_data = NULL;
		invalid.settings = unknown;
		invalid.setting_count = 1;
		status = pgm_connection_open(instance, &invalid, &unexpected, &error);
		if (status != PGM_STATUS_CONNECTION_FAILED || unexpected != NULL)
		{
			fprintf(stderr, "unknown connection setting did not fail closed\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
		invalid.settings = unsafe;
		status = pgm_connection_open(instance, &invalid, &unexpected, &error);
		if (status != PGM_STATUS_CONNECTION_FAILED || unexpected != NULL)
		{
			fprintf(stderr, "unsafe connection setting did not fail closed\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
	}

	listener_options.user = "postgamma";
	listener_options.database = "postgres";
	listener_options.application_name = "postgamma-c-api-listener";
	listener_options.notification_callback = receive_notification;
	listener_options.user_data = &callback_state;
	status = pgm_connection_open(
		instance, &listener_options, &listener, &error);
	if (status != PGM_STATUS_OK)
		goto fail;

	scale_connections = calloc(scale_count, sizeof(*scale_connections));
	if (scale_connections == NULL)
	{
		status = PGM_STATUS_OUT_OF_MEMORY;
		goto fail;
	}
	fd_before = open_file_descriptor_count();
	for (size_t index = 0; index < scale_count; index++)
	{
		pgm_connection_options scale_options = PGM_CONNECTION_OPTIONS_INIT;

		scale_options.user = "postgamma";
		scale_options.database = "postgres";
		scale_options.application_name = "postgamma-c-api-scale";
		status = pgm_connection_open(
			instance, &scale_options, &scale_connections[index], &error);
		if (status != PGM_STATUS_OK)
		{
			fprintf(stderr, "scale connection %zu failed\n", index);
			goto fail;
		}
		opened_scale++;
	}
	fd_after = open_file_descriptor_count();
	if (fd_before == SIZE_MAX || fd_after == SIZE_MAX || fd_after < fd_before)
	{
		fprintf(stderr, "could not measure host file descriptors\n");
		goto fail;
	}
	{
		pgm_request *request = NULL;
		pgm_result *result = NULL;

		status = pgm_execute_async(
			scale_connections[0], "SELECT 1", NULL, 0, PGM_FORMAT_TEXT,
			&request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = pgm_request_waitable(request, &request_waitable, &error);
		if (status != PGM_STATUS_OK || request_waitable != instance_waitable)
		{
			fprintf(stderr, "request did not reuse the instance waitable\n");
			pgm_request_free(request);
			goto fail;
		}
		status = pgm_request_wait(
			request, TEST_TIMEOUT_MS, &result, &error);
		if (status != PGM_STATUS_OK)
		{
			pgm_result_free(result);
			pgm_request_free(request);
			goto fail;
		}
		pgm_result_free(result);
		pgm_request_free(request);
	}
	for (size_t index = opened_scale; index > 0; index--)
	{
		status = pgm_connection_close(
			scale_connections[index - 1], TEST_TIMEOUT_MS, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		scale_connections[index - 1] = NULL;
		opened_scale--;
	}
	free(scale_connections);
	scale_connections = NULL;

	if (!execute_notice(
			connection,
			"DO $$ BEGIN RAISE NOTICE 'request-notice' "
			"USING DETAIL='request-detail', HINT='request-hint'; END $$",
			receive_request_notice, &callback_state,
			&callback_state.expected_request_id) ||
		callback_state.request_notice_count != 1 ||
		callback_state.connection_notice_count != 0 ||
		!callback_state.request_notice_fields)
	{
		fprintf(stderr, "request notice override contract failed\n");
		goto fail;
	}
	if (!execute_notice(
			connection,
			"DO $$ BEGIN RAISE NOTICE 'connection-notice' "
			"USING DETAIL='connection-detail'; END $$",
			NULL, &callback_state, NULL) ||
		callback_state.connection_notice_count != 1 ||
		!callback_state.connection_notice_fields ||
		!callback_state.routed_log)
	{
		fprintf(stderr, "connection notice or routed log contract failed\n");
		goto fail;
	}

	if (!diagnostic_contract(connection))
		goto fail;
	if (!dispatch_all(instance, &dispatched))
		goto fail;

	if (!execute_command(listener, "LISTEN postgamma_events") ||
		!execute_command(
			connection, "NOTIFY postgamma_events, 'polled-payload'"))
		goto fail;
	{
		struct pollfd descriptor = {instance_waitable, POLLIN, 0};

		if (poll(&descriptor, 1, 5000) <= 0 ||
			(descriptor.revents & POLLIN) == 0)
		{
			fprintf(stderr, "idle notification did not signal the waitable\n");
			goto fail;
		}
	}
	{
		size_t callback_count = callback_state.notification_count;

		if (!wait_for_notification_event(
				instance, pgm_connection_identity(listener), "polled-payload",
				&polled_notifications) ||
			callback_state.notification_count != callback_count)
		{
			fprintf(stderr, "polled notification was not exactly-once\n");
			goto fail;
		}
	}
	if (!execute_command(
			connection, "NOTIFY postgamma_events, 'dispatched-payload'"))
		goto fail;
	{
		uint64_t dispatch_deadline = monotonic_milliseconds();

		if (dispatch_deadline == UINT64_MAX ||
			UINT64_MAX - dispatch_deadline < (uint64_t) TEST_TIMEOUT_MS)
		{
			fprintf(stderr, "could not start the notification deadline\n");
			goto fail;
		}
		dispatch_deadline += (uint64_t) TEST_TIMEOUT_MS;
		while (callback_state.notification_count == 0)
		{
			struct pollfd descriptor = {instance_waitable, POLLIN, 0};
			uint64_t now;
			uint64_t remaining;
			int timeout;
			int poll_status;

			if (!dispatch_all(instance, &dispatched))
			{
				fprintf(stderr, "could not dispatch the idle notification\n");
				goto fail;
			}
			if (callback_state.notification_count != 0)
				break;
			now = monotonic_milliseconds();
			if (now == UINT64_MAX || now >= dispatch_deadline)
				break;
			remaining = dispatch_deadline - now;
			timeout = remaining > UINT64_C(100) ? 100 : (int) remaining;
			poll_status = poll(&descriptor, 1, timeout);
			if (poll_status < 0 ||
				(poll_status > 0 && (descriptor.revents & POLLIN) == 0))
			{
				fprintf(stderr, "could not wait for the idle notification\n");
				goto fail;
			}
		}
	}
	if (callback_state.notification_count != 1 ||
		!callback_state.notification_fields ||
		strcmp(
			callback_state.last_notification_payload,
			"dispatched-payload") != 0 ||
		!dispatch_all(instance, &dispatched) ||
		callback_state.notification_count != 1)
	{
		fprintf(stderr,
			"dispatched idle notification contract failed: "
			"count=%zu fields=%s payload=%s\n",
			callback_state.notification_count,
			callback_state.notification_fields ? "true" : "false",
			callback_state.last_notification_payload);
		goto fail;
	}

	status = pgm_instance_get_telemetry(
		instance, &before_overflow, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (!execute_command(
			connection,
			"DO $$ BEGIN FOR i IN 1..500 LOOP "
			"RAISE NOTICE 'overflow-notice-%', i; END LOOP; END $$") ||
		!execute_command(
			connection,
			"DO $$ BEGIN RAISE NOTICE '%', repeat('x', 3000); END $$"))
		goto fail;
	status = pgm_instance_get_telemetry(instance, &telemetry, &error);
	if (status != PGM_STATUS_OK ||
		(telemetry.dropped_log_count <= before_overflow.dropped_log_count &&
		 telemetry.dropped_notice_count <= before_overflow.dropped_notice_count) ||
		telemetry.event_queue_capacity != EVENT_QUEUE_CAPACITY ||
		telemetry.event_queue_depth > telemetry.event_queue_capacity)
	{
		fprintf(stderr, "event overflow telemetry is invalid\n");
		goto fail;
	}
	if (!wait_for_overflow_event(instance, &overflow_reported) ||
		overflow_reported == 0)
	{
		fprintf(stderr, "event overflow was not observable\n");
		goto fail;
	}
	{
		bool		quiescent = false;

		for (size_t attempt = 0; attempt < 300; attempt++)
		{
			status = pgm_instance_get_telemetry(instance, &telemetry, &error);
			if (status != PGM_STATUS_OK)
				goto fail;
			if (telemetry.connection_count == 2 &&
				telemetry.active_request_count == 0 &&
				telemetry.request_count >= 10 &&
				telemetry.completed_request_count != 0 &&
				telemetry.failed_request_count >= 2 &&
				telemetry.executor_worker_count == 4 &&
				telemetry.running_session_count == 0 &&
				telemetry.runnable_session_count == 0 &&
				telemetry.queued_request_count == 0 &&
				telemetry.parallel_tokens_in_use == 0)
			{
				quiescent = true;
				break;
			}
			(void) poll(NULL, 0, 10);
		}
		if (quiescent)
			goto telemetry_quiescent;
		fprintf(stderr,
			"instance telemetry did not reach a quiescent state: "
			"connections=%" PRIu64 " active=%" PRIu64
			" requests=%" PRIu64 " completed=%" PRIu64
			" failed=%" PRIu64 " workers=%" PRIu32
			" running=%" PRIu64 " runnable=%" PRIu64
			" queued=%" PRIu64 " parallel=%" PRIu64 "\n",
			telemetry.connection_count, telemetry.active_request_count,
			telemetry.request_count, telemetry.completed_request_count,
			telemetry.failed_request_count, telemetry.executor_worker_count,
			telemetry.running_session_count, telemetry.runnable_session_count,
			telemetry.queued_request_count, telemetry.parallel_tokens_in_use);
		goto fail;
	}

telemetry_quiescent:
	if (callback_state.wrong_thread ||
		callback_state.reentrant_status != PGM_STATUS_REENTRANT_CALL)
	{
		fprintf(stderr, "callback thread or reentry contract regressed\n");
		goto fail;
	}

	status = pgm_connection_close(listener, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	listener = NULL;
	status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection = NULL;
	status = pgm_instance_close(
		instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	instance = NULL;
	callback_state.instance = NULL;
	passed = true;

	printf(
		"POSTGAMMA_KERNEL_3_EVENTS diagnostics=17 object_diagnostics=true "
		"parameter_status=true precedence=true settings_fail_closed=true "
		"request_notice_override=true idle_notification_poll=true "
		"idle_notification_dispatch=true exact_once=true routed_log=true "
		"callback_host_thread=true reentrant_status=%d overflow=true "
		"shared_waitable=true routing=o1 connections=%zu fd_growth=%zu "
		"startup_polled=%zu "
		"event_capacity=%" PRIu64 " overflow_reported=%" PRIu64 " "
		"request_count=%" PRIu64 " completed_count=%" PRIu64 " "
		"failed_count=%" PRIu64 " trace=%s phase=closed\n",
		callback_state.reentrant_status, scale_count,
		fd_after - fd_before, startup_polled,
		telemetry.event_queue_capacity, overflow_reported,
		telemetry.request_count, telemetry.completed_request_count,
		telemetry.failed_request_count, trace_mode ? "true" : "false");

fail:
	if (!passed)
		(void) report_failure("runtime events events driver", status, error);
	pgm_event_free(event);
	pgm_error_free(error);
	if (scale_connections != NULL)
	{
		for (size_t index = opened_scale; index > 0; index--)
		{
			if (scale_connections[index - 1] != NULL)
				(void) pgm_connection_close(
					scale_connections[index - 1], TEST_TIMEOUT_MS, NULL);
		}
		free(scale_connections);
	}
	if (listener != NULL)
		(void) pgm_connection_close(listener, TEST_TIMEOUT_MS, NULL);
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
	fprintf(
		stderr, "%s failed: status=%s sqlstate=%s severity=%s message=%s "
		"detail=%s\n",
		operation, pgm_status_name(status),
		error != NULL ? pgm_error_sqlstate(error) : "",
		error != NULL ? pgm_error_severity(error) : "",
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
		(pgm_result_kind(result) == PGM_RESULT_COMMAND_OK ||
		 pgm_result_kind(result) == PGM_RESULT_TUPLES_OK);

	if (!ok)
		(void) report_failure("execute command", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}


static bool
execute_text(
	pgm_connection *connection, const char *sql, const char *expected)
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
		pgm_result_value(result, 0, 0, &value, &error) == PGM_STATUS_OK &&
		!value.is_null && value.size == strlen(expected) &&
		memcmp(value.data, expected, value.size) == 0;

	if (!ok)
		(void) report_failure("execute text", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}


static bool
execute_notice(
	pgm_connection *connection, const char *sql,
	pgm_notice_callback callback, CallbackState *state,
	pgm_request_id *request_id)
{
	pgm_execute_options options = PGM_EXECUTE_OPTIONS_INIT;
	pgm_request *request = NULL;
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;

	options.notice_callback = callback;
	options.notice_user_data = callback != NULL ? state : NULL;
	status = pgm_execute_async_ex(
		connection, sql, strlen(sql), &options, &request, &error);
	if (status != PGM_STATUS_OK)
		goto done;
	if (request_id != NULL)
		*request_id = pgm_request_identity(request);
	status = pgm_request_wait(
		request, TEST_TIMEOUT_MS, &result, &error);

done:
	if (status != PGM_STATUS_OK)
		(void) report_failure("execute notice", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	pgm_request_free(request);
	return status == PGM_STATUS_OK;
}


static bool
diagnostic_contract(pgm_connection *connection)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	bool		ok = false;

	if (!execute_command(
			connection,
			"CREATE TABLE public.event_diagnostic "
			"(value integer CONSTRAINT event_positive CHECK (value > 0))"))
		return false;
	status = pgm_execute(
		connection, "INSERT INTO public.event_diagnostic VALUES (-1)",
		NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_POSTGRES_ERROR || error == NULL ||
		strcmp(pgm_error_field(error, PGM_DIAG_SQLSTATE), "23514") != 0 ||
		strcmp(pgm_error_field(error, PGM_DIAG_SEVERITY), "ERROR") != 0 ||
		pgm_error_field(error, PGM_DIAG_MESSAGE) == NULL ||
		strcmp(pgm_error_field(error, PGM_DIAG_SCHEMA), "public") != 0 ||
		strcmp(
			pgm_error_field(error, PGM_DIAG_TABLE), "event_diagnostic") != 0 ||
		strcmp(
			pgm_error_field(error, PGM_DIAG_CONSTRAINT), "event_positive") != 0 ||
		pgm_error_field(error, PGM_DIAG_SOURCE_FILE) == NULL ||
		pgm_error_field(error, PGM_DIAG_SOURCE_LINE) == NULL ||
		pgm_error_field(error, PGM_DIAG_SOURCE_FUNCTION) == NULL)
	{
		(void) report_failure("constraint diagnostic", status, error);
		goto done;
	}
	for (pgm_diagnostic_field field = PGM_DIAG_SQLSTATE;
		 field <= PGM_DIAG_SOURCE_FUNCTION; field++)
		(void) pgm_error_field(error, field);
	if (pgm_error_field(error, PGM_DIAG_SOURCE_FUNCTION + 1) != NULL)
	{
		fprintf(stderr, "out-of-range diagnostic field was not rejected\n");
		goto done;
	}
	pgm_error_free(error);
	error = NULL;
	pgm_result_free(result);
	result = NULL;
	status = pgm_execute(
		connection, "SELEC 1", NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	if (status != PGM_STATUS_POSTGRES_ERROR || error == NULL ||
		strcmp(pgm_error_field(error, PGM_DIAG_SQLSTATE), "42601") != 0 ||
		pgm_error_field(error, PGM_DIAG_POSITION) == NULL)
	{
		(void) report_failure("syntax diagnostic", status, error);
		goto done;
	}
	ok = true;

done:
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}


static bool
drain_instance(pgm_instance *instance, size_t *drained)
{
	for (;;)
	{
		pgm_event  *event = NULL;
		pgm_error  *error = NULL;
		pgm_availability availability = PGM_AVAILABILITY_AGAIN;
		pgm_status	status = pgm_instance_next_event(
			instance, 0, &event, &availability, &error);

		if (status != PGM_STATUS_OK)
		{
			(void) report_failure("drain instance events", status, error);
			pgm_error_free(error);
			pgm_event_free(event);
			return false;
		}
		pgm_error_free(error);
		if (availability != PGM_AVAILABILITY_READY)
			return true;
		(*drained)++;
		pgm_event_free(event);
	}
}


static bool
wait_for_notification_event(
	pgm_instance *instance, pgm_connection_id connection_id,
	const char *payload, size_t *polled_notifications)
{
	for (size_t attempt = 0; attempt < 256; attempt++)
	{
		pgm_event  *event = NULL;
		pgm_error  *error = NULL;
		pgm_availability availability = PGM_AVAILABILITY_AGAIN;
		pgm_status	status = pgm_instance_next_event(
			instance, TEST_TIMEOUT_MS, &event, &availability, &error);

		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
			event == NULL)
		{
			(void) report_failure("wait for notification", status, error);
			pgm_error_free(error);
			pgm_event_free(event);
			return false;
		}
		pgm_error_free(error);
		if (pgm_event_type(event) == PGM_EVENT_NOTIFICATION)
		{
			pgm_notification notification = PGM_NOTIFICATION_INIT;
			bool matches = pgm_event_notification(
				event, &notification, NULL) == PGM_STATUS_OK &&
				pgm_event_connection(event) == connection_id &&
				pgm_event_request(event) == 0 &&
				notification.connection_id == connection_id &&
				notification.channel != NULL &&
				strcmp(notification.channel, "postgamma_events") == 0 &&
				notification.payload != NULL &&
				strcmp(notification.payload, payload) == 0;

			pgm_event_free(event);
			if (matches)
			{
				(*polled_notifications)++;
				return true;
			}
			continue;
		}
		pgm_event_free(event);
	}
	return false;
}


static bool
wait_for_overflow_event(pgm_instance *instance, uint64_t *reported_drops)
{
	for (size_t attempt = 0; attempt < 256; attempt++)
	{
		pgm_event  *event = NULL;
		pgm_error  *error = NULL;
		pgm_availability availability = PGM_AVAILABILITY_AGAIN;
		pgm_status	status = pgm_instance_next_event(
			instance, 0, &event, &availability, &error);

		if (status != PGM_STATUS_OK)
		{
			(void) report_failure("wait for overflow", status, error);
			pgm_error_free(error);
			pgm_event_free(event);
			return false;
		}
		pgm_error_free(error);
		if (availability != PGM_AVAILABILITY_READY)
			return *reported_drops != 0;
		if (pgm_event_type(event) == PGM_EVENT_OVERFLOW)
		{
			pgm_event_overflow_record overflow =
				PGM_EVENT_OVERFLOW_RECORD_INIT;

			if (pgm_event_overflow(event, &overflow, NULL) != PGM_STATUS_OK ||
				overflow.dropped_count == 0)
			{
				pgm_event_free(event);
				return false;
			}
			*reported_drops += overflow.dropped_count;
		}
		pgm_event_free(event);
	}
	return *reported_drops != 0;
}


static bool
dispatch_all(pgm_instance *instance, size_t *dispatched)
{
	for (size_t attempt = 0; attempt < 256; attempt++)
	{
		pgm_error  *error = NULL;
		size_t		batch = 0;
		pgm_status	status = pgm_instance_dispatch(
			instance, 0, &batch, &error);

		if (status != PGM_STATUS_OK)
		{
			(void) report_failure("dispatch instance events", status, error);
			pgm_error_free(error);
			return false;
		}
		pgm_error_free(error);
		*dispatched += batch;
		if (batch == 0)
			return true;
	}
	return false;
}


static uint64_t
monotonic_milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
		return UINT64_MAX;
	return (uint64_t) now.tv_sec * UINT64_C(1000) +
		(uint64_t) now.tv_nsec / UINT64_C(1000000);
}


static size_t
open_file_descriptor_count(void)
{
	DIR		   *directory = opendir("/proc/self/fd");
	struct dirent *entry;
	size_t		count = 0;

	if (directory == NULL)
		return SIZE_MAX;
	while ((entry = readdir(directory)) != NULL)
	{
		if (strcmp(entry->d_name, ".") != 0 &&
			strcmp(entry->d_name, "..") != 0)
			count++;
	}
	(void) closedir(directory);
	return count;
}


static void
observe_callback(CallbackState *state)
{
	if (!pthread_equal(pthread_self(), state->host_thread))
		state->wrong_thread = true;
	if (state->reentrant_status == PGM_STATUS_INTERNAL_ERROR &&
		state->instance != NULL)
	{
		pgm_instance_telemetry telemetry = PGM_INSTANCE_TELEMETRY_INIT;
		pgm_error  *error = NULL;

		state->reentrant_status = pgm_instance_get_telemetry(
			state->instance, &telemetry, &error);
		pgm_error_free(error);
	}
}


static void
receive_log(void *argument, const pgm_log_record *record)
{
	CallbackState *state = argument;

	observe_callback(state);
	state->log_count++;
	if (record != NULL &&
		record->connection_id == state->expected_connection_id &&
		record->request_id == state->expected_request_id &&
		record->connection_id != 0 && record->request_id != 0)
		state->routed_log = true;
}


static void
receive_connection_notice(void *argument, const pgm_notice *notice)
{
	CallbackState *state = argument;

	observe_callback(state);
	state->connection_notice_count++;
	if (notice != NULL && notice->connection_id == state->expected_connection_id &&
		notice->request_id != 0 && notice->sqlstate != NULL &&
		strcmp(notice->sqlstate, "00000") == 0 && notice->message != NULL &&
		strstr(notice->message, "connection-notice") != NULL &&
		notice->detail != NULL &&
		strcmp(notice->detail, "connection-detail") == 0)
		state->connection_notice_fields = true;
}


static void
receive_request_notice(void *argument, const pgm_notice *notice)
{
	CallbackState *state = argument;

	observe_callback(state);
	state->request_notice_count++;
	if (notice != NULL && notice->connection_id == state->expected_connection_id &&
		notice->request_id == state->expected_request_id &&
		notice->sqlstate != NULL && strcmp(notice->sqlstate, "00000") == 0 &&
		notice->message != NULL &&
		strstr(notice->message, "request-notice") != NULL &&
		notice->detail != NULL &&
		strcmp(notice->detail, "request-detail") == 0 &&
		notice->hint != NULL && strcmp(notice->hint, "request-hint") == 0)
		state->request_notice_fields = true;
}


static void
receive_notification(
	void *argument, const pgm_notification *notification)
{
	CallbackState *state = argument;

	observe_callback(state);
	state->notification_count++;
	if (notification != NULL && notification->connection_id != 0 &&
		notification->virtual_backend_pid > 0 &&
		notification->channel != NULL &&
		strcmp(notification->channel, "postgamma_events") == 0 &&
		notification->payload != NULL)
	{
		state->notification_fields = true;
		(void) snprintf(
			state->last_notification_payload,
			sizeof(state->last_notification_payload), "%s",
			notification->payload);
	}
}
