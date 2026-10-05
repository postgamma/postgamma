#include "postgamma/private/bootstrap_probe.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postgamma/private/libpq_memory_adapter.h"
#include "tests/embedded/bootstrap/protocol_fixture.h"


#define POSTGAMMA_BOOTSTRAP_LIBPQ_GENERATION UINT64_C(702001)
#define POSTGAMMA_BOOTSTRAP_REQUEST_GENERATION UINT64_C(702002)
#define POSTGAMMA_BOOTSTRAP_LIBPQ_CAPACITY 64U
#define POSTGAMMA_BOOTSTRAP_LIBPQ_TIMEOUT_NS INT64_C(12000000000)
#define POSTGAMMA_BOOTSTRAP_COPY_BYTES 4096U
#define POSTGAMMA_BOOTSTRAP_COPY_PREFIX 128U

typedef struct PostgammaBootstrapCancelObservation
{
	unsigned int calls;
	uint64_t	connection_generation;
	uint64_t	request_generation;
	int			backend_pid;
} PostgammaBootstrapCancelObservation;


static int
postgamma_bootstrap_cancel_callback(
	void *argument,
	uint64_t connection_generation,
	uint64_t request_generation,
	int backend_pid)
{
	PostgammaBootstrapCancelObservation *observation = argument;

	observation->calls++;
	observation->connection_generation = connection_generation;
	observation->request_generation = request_generation;
	observation->backend_pid = backend_pid;
	return 0;
}


static bool
postgamma_bootstrap_deadline(int64_t *deadline)
{
	int64_t		now;

	if (postgamma_memory_clock_now(&now) != POSTGAMMA_MEMORY_STATUS_OK ||
		now > INT64_MAX - POSTGAMMA_BOOTSTRAP_LIBPQ_TIMEOUT_NS)
		return false;
	*deadline = now + POSTGAMMA_BOOTSTRAP_LIBPQ_TIMEOUT_NS;
	return true;
}


static bool
postgamma_bootstrap_result_is_one(const PostgammaPrivateQueryResult *result)
{
	return result->status == POSTGAMMA_PRIVATE_RESULT_TUPLES_OK &&
		result->rows == 1 && result->columns == 1 &&
		strcmp(result->value, "1") == 0 &&
		strcmp(result->command_status, "SELECT 1") == 0 &&
		result->transaction_status == 0;
}


pgm_bootstrap_private_libpq_result
pgm_bootstrap_probe_private_libpq(void)
{
	PostgammaMemoryGlobalTelemetry baseline;
	PostgammaMemoryGlobalTelemetry after;
	PostgammaPrivateLibpqOptions options;
	PostgammaPrivateLibpqConnection *connection = NULL;
	PostgammaMemoryEndpoint *backend_endpoint = NULL;
	PostgammaBootstrapProtocolFixture *fixture = NULL;
	PostgammaBootstrapProtocolFixtureTelemetry fixture_telemetry;
	PostgammaPrivateLibpqTelemetry telemetry;
	PostgammaPrivateQueryResult result;
	PostgammaBootstrapCancelObservation cancel = {0};
	unsigned char copy_data[POSTGAMMA_BOOTSTRAP_COPY_BYTES];
	PostgammaPrivateLibpqStatus status;
	const char *failure = NULL;
	int64_t		deadline = 0;
	unsigned int checks = 0;
	bool		fixture_joined = false;
	bool		connection_closed = false;
	size_t		index;

	memset(&fixture_telemetry, 0, sizeof(fixture_telemetry));
	memset(&telemetry, 0, sizeof(telemetry));
	memset(&result, 0, sizeof(result));
	postgamma_memory_global_telemetry(&baseline);
	checks++;
	if (baseline.active_transports != 0 || baseline.endpoint_references != 0 ||
		baseline.allocated_bytes != 0)
	{
		failure = "memory transport baseline is not empty";
		goto cleanup;
	}
	options = (PostgammaPrivateLibpqOptions) {
		.generation = POSTGAMMA_BOOTSTRAP_LIBPQ_GENERATION,
		.queue_capacity = POSTGAMMA_BOOTSTRAP_LIBPQ_CAPACITY,
		.user = "embedded_user",
		.database = "embedded_db",
		.application_name = "postgamma-bootstrap-private-libpq",
		.cancel_callback = postgamma_bootstrap_cancel_callback,
		.cancel_argument = &cancel,
	};
	status = postgamma_private_libpq_create(
		&options, &connection, &backend_endpoint);
	checks++;
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK || connection == NULL ||
		backend_endpoint == NULL)
	{
		failure = "private libpq connection creation failed";
		goto cleanup;
	}
	checks++;
	if (postgamma_bootstrap_protocol_fixture_start(
			&backend_endpoint, POSTGAMMA_BOOTSTRAP_LIBPQ_GENERATION, &fixture) != 0)
	{
		failure = "protocol fixture thread creation failed";
		goto cleanup;
	}
	checks++;
	if (!postgamma_bootstrap_deadline(&deadline))
	{
		failure = "cannot create the connection deadline";
		goto cleanup;
	}
	status = postgamma_private_libpq_connect(connection, deadline);
	checks++;
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		failure = "private libpq startup exchange failed";
		goto cleanup;
	}
	checks++;
	if (!postgamma_bootstrap_deadline(&deadline) ||
		postgamma_private_libpq_query(
			connection, "SELECT 1", deadline, &result) !=
		POSTGAMMA_PRIVATE_LIBPQ_OK || !postgamma_bootstrap_result_is_one(&result))
	{
		failure = "private libpq tuple result is invalid";
		goto cleanup;
	}
	checks++;
	if (!postgamma_bootstrap_deadline(&deadline) ||
		postgamma_private_libpq_query(
			connection, "SELECT 1 / 0", deadline, &result) !=
		POSTGAMMA_PRIVATE_LIBPQ_OK ||
		result.status != POSTGAMMA_PRIVATE_RESULT_FATAL_ERROR ||
		strcmp(result.sqlstate, "22012") != 0 ||
		strcmp(result.severity, "ERROR") != 0 ||
		strcmp(result.message, "division by zero") != 0 ||
		strcmp(result.detail, "bootstrap protocol fixture query error") != 0)
	{
		failure = "private libpq error fields are invalid";
		goto cleanup;
	}
	checks++;
	if (!postgamma_bootstrap_deadline(&deadline) ||
		postgamma_private_libpq_query(
			connection, "SELECT 1", deadline, &result) !=
		POSTGAMMA_PRIVATE_LIBPQ_OK || !postgamma_bootstrap_result_is_one(&result))
	{
		failure = "connection did not recover after an error result";
		goto cleanup;
	}
	for (index = 0; index < sizeof(copy_data); index++)
		copy_data[index] = (unsigned char) (index & 0xffU);
	checks++;
	if (!postgamma_bootstrap_deadline(&deadline) ||
		postgamma_private_libpq_copy_in(
			connection, copy_data, sizeof(copy_data), deadline, &result) !=
		POSTGAMMA_PRIVATE_LIBPQ_OK ||
		result.status != POSTGAMMA_PRIVATE_RESULT_FATAL_ERROR ||
		strcmp(result.sqlstate, "57014") != 0 ||
		strcmp(result.message, "fixture rejected COPY") != 0)
	{
		failure = "duplex COPY exchange did not return the fixture error";
		goto cleanup;
	}
	checks++;
	if (!postgamma_bootstrap_deadline(&deadline) ||
		postgamma_private_libpq_query(
			connection, "SELECT 1", deadline, &result) !=
		POSTGAMMA_PRIVATE_LIBPQ_OK || !postgamma_bootstrap_result_is_one(&result))
	{
		failure = "connection did not recover after the COPY error";
		goto cleanup;
	}
	status = postgamma_private_libpq_cancel(
		connection, POSTGAMMA_BOOTSTRAP_REQUEST_GENERATION);
	checks++;
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK || cancel.calls != 1 ||
		cancel.connection_generation != POSTGAMMA_BOOTSTRAP_LIBPQ_GENERATION ||
		cancel.request_generation != POSTGAMMA_BOOTSTRAP_REQUEST_GENERATION ||
		cancel.backend_pid != 4242)
	{
		failure = "direct cancellation dispatch is invalid";
		goto cleanup;
	}
	status = postgamma_private_libpq_telemetry(connection, &telemetry);
	checks++;
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK ||
		telemetry.secure_read_calls == 0 || telemetry.secure_write_calls == 0 ||
		telemetry.socket_wait_calls == 0 || telemetry.notice_count != 1 ||
		telemetry.cancel_dispatches != 1 || telemetry.network_connect_calls != 0 ||
		telemetry.optional_security_calls != 0 || telemetry.backend_pid != 4242 ||
		strcmp(telemetry.last_notice_sqlstate, "00000") != 0 ||
		strcmp(telemetry.last_notice_message, "fixture copy notice") != 0)
	{
		failure = "private libpq telemetry is invalid";
		goto cleanup;
	}

cleanup:
	if (connection != NULL)
	{
		(void) postgamma_private_libpq_telemetry(connection, &telemetry);
		status = postgamma_private_libpq_close(&connection);
		connection_closed = status == POSTGAMMA_PRIVATE_LIBPQ_OK;
		if (!connection_closed && failure == NULL)
			failure = "private libpq close failed";
	}
	if (backend_endpoint != NULL)
	{
		(void) postgamma_memory_endpoint_abort(
			backend_endpoint, POSTGAMMA_BOOTSTRAP_LIBPQ_GENERATION,
			POSTGAMMA_MEMORY_ABORT_SHUTDOWN);
		(void) postgamma_memory_endpoint_release(
			&backend_endpoint, POSTGAMMA_BOOTSTRAP_LIBPQ_GENERATION);
	}
	if (fixture != NULL)
	{
		fixture_joined = postgamma_bootstrap_protocol_fixture_join(
			&fixture, &fixture_telemetry) == 0;
		if (!fixture_joined && failure == NULL)
			failure = "protocol fixture failed or could not join";
	}
	postgamma_memory_global_telemetry(&after);
	checks++;
	if (after.active_transports != baseline.active_transports ||
		after.endpoint_references != baseline.endpoint_references ||
		after.allocated_bytes != baseline.allocated_bytes)
		failure = "memory transport resources leaked";
	if (failure == NULL)
	{
		checks++;
		if (!connection_closed || !fixture_joined ||
			fixture_telemetry.startup_packets != 1 ||
			fixture_telemetry.query_packets != 5 ||
			fixture_telemetry.copy_packets != 1 ||
			fixture_telemetry.copy_bytes_before_response !=
				POSTGAMMA_BOOTSTRAP_COPY_PREFIX ||
			fixture_telemetry.copy_bytes_received != POSTGAMMA_BOOTSTRAP_COPY_BYTES ||
			!fixture_telemetry.startup_parameters_valid ||
			!fixture_telemetry.response_preceded_copy_completion ||
			!fixture_telemetry.simultaneous_queue_saturation ||
			!fixture_telemetry.frontend_half_close_observed)
			failure = "protocol fixture evidence is incomplete";
	}
	{
		pgm_bootstrap_private_libpq_result probe_result = {
		.status = failure == NULL ? PGM_BOOTSTRAP_PROBE_PASS : PGM_BOOTSTRAP_PROBE_FAIL,
		.checks = checks,
		.queue_capacity = POSTGAMMA_BOOTSTRAP_LIBPQ_CAPACITY,
		.startup_packets = fixture_telemetry.startup_packets,
		.query_packets = fixture_telemetry.query_packets,
		.copy_packets = fixture_telemetry.copy_packets,
		.copy_bytes_before_response = (unsigned int)
			fixture_telemetry.copy_bytes_before_response,
		.copy_bytes_received = (unsigned int)
			fixture_telemetry.copy_bytes_received,
		.simultaneous_queue_saturation =
			fixture_telemetry.simultaneous_queue_saturation ? 1U : 0U,
		.secure_read_calls = (unsigned int) telemetry.secure_read_calls,
		.secure_write_calls = (unsigned int) telemetry.secure_write_calls,
		.socket_wait_calls = (unsigned int) telemetry.socket_wait_calls,
		.notice_count = (unsigned int) telemetry.notice_count,
		.cancel_dispatches = (unsigned int) telemetry.cancel_dispatches,
		.network_connect_calls = (unsigned int) telemetry.network_connect_calls,
		.optional_security_calls = (unsigned int)
			telemetry.optional_security_calls,
		.backend_pid = (unsigned int) telemetry.backend_pid,
		.connection_generation = POSTGAMMA_BOOTSTRAP_LIBPQ_GENERATION,
		.request_generation = POSTGAMMA_BOOTSTRAP_REQUEST_GENERATION,
		.active_transports_after_close = after.active_transports,
		.endpoint_references_after_close = after.endpoint_references,
		.allocated_bytes_after_close = after.allocated_bytes,
		.observed_result_status = (unsigned int) result.status,
		.observed_rows = result.rows,
		.observed_columns = result.columns,
		.observed_transaction_status = result.transaction_status,
		.detail = failure == NULL ?
			"private PG19 libpq memory transport passed" : failure,
		};

		(void) snprintf(
			probe_result.observed_value,
			sizeof(probe_result.observed_value), "%s", result.value);
		(void) snprintf(
			probe_result.observed_command_status,
			sizeof(probe_result.observed_command_status), "%s",
			result.command_status);
		(void) snprintf(
			probe_result.last_error, sizeof(probe_result.last_error),
			"%s", telemetry.last_error);
		return probe_result;
	}
}
