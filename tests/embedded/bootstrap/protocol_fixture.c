#define _POSIX_C_SOURCE 200809L

#include "tests/embedded/bootstrap/protocol_fixture.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define POSTGAMMA_BOOTSTRAP_FIXTURE_DEADLINE_NS INT64_C(15000000000)
#define POSTGAMMA_BOOTSTRAP_FIXTURE_MAX_PACKET (UINT32_C(1024) * UINT32_C(1024))
#define POSTGAMMA_BOOTSTRAP_COPY_LENGTH 4096U
#define POSTGAMMA_BOOTSTRAP_COPY_PREFIX 128U

typedef struct PostgammaBootstrapWireBuffer
{
	unsigned char bytes[1024];
	size_t		length;
} PostgammaBootstrapWireBuffer;

struct PostgammaBootstrapProtocolFixture
{
	uint64_t	generation;
	int64_t		deadline_ns;
	pthread_t	thread;
	PostgammaMemoryEndpoint *endpoint;
	PostgammaBootstrapProtocolFixtureTelemetry telemetry;
	int			status;
};


static uint32_t
postgamma_bootstrap_read_u32(const unsigned char *source)
{
	return ((uint32_t) source[0] << 24) |
		((uint32_t) source[1] << 16) |
		((uint32_t) source[2] << 8) |
		(uint32_t) source[3];
}


static void
postgamma_bootstrap_write_u16(unsigned char *destination, uint16_t value)
{
	destination[0] = (unsigned char) (value >> 8);
	destination[1] = (unsigned char) value;
}


static void
postgamma_bootstrap_write_u32(unsigned char *destination, uint32_t value)
{
	destination[0] = (unsigned char) (value >> 24);
	destination[1] = (unsigned char) (value >> 16);
	destination[2] = (unsigned char) (value >> 8);
	destination[3] = (unsigned char) value;
}


static int
postgamma_bootstrap_buffer_append(
	PostgammaBootstrapWireBuffer *buffer, const void *source, size_t length)
{
	if (length > sizeof(buffer->bytes) - buffer->length)
		return -1;
	memcpy(buffer->bytes + buffer->length, source, length);
	buffer->length += length;
	return 0;
}


static int
postgamma_bootstrap_buffer_u8(PostgammaBootstrapWireBuffer *buffer, unsigned char value)
{
	return postgamma_bootstrap_buffer_append(buffer, &value, sizeof(value));
}


static int
postgamma_bootstrap_buffer_u16(PostgammaBootstrapWireBuffer *buffer, uint16_t value)
{
	unsigned char encoded[2];

	postgamma_bootstrap_write_u16(encoded, value);
	return postgamma_bootstrap_buffer_append(buffer, encoded, sizeof(encoded));
}


static int
postgamma_bootstrap_buffer_u32(PostgammaBootstrapWireBuffer *buffer, uint32_t value)
{
	unsigned char encoded[4];

	postgamma_bootstrap_write_u32(encoded, value);
	return postgamma_bootstrap_buffer_append(buffer, encoded, sizeof(encoded));
}


static int
postgamma_bootstrap_buffer_string(PostgammaBootstrapWireBuffer *buffer, const char *value)
{
	return postgamma_bootstrap_buffer_append(buffer, value, strlen(value) + 1);
}


static void
postgamma_bootstrap_fixture_error(
	PostgammaBootstrapProtocolFixture *fixture, const char *detail)
{
	if (fixture->telemetry.detail[0] == '\0')
		(void) snprintf(
			fixture->telemetry.detail,
			sizeof(fixture->telemetry.detail), "%s", detail);
	fixture->status = -1;
}


static int
postgamma_bootstrap_wait(
	PostgammaBootstrapProtocolFixture *fixture, uint32_t events)
{
	PostgammaMemoryWaitResult result = postgamma_memory_endpoint_wait(
		fixture->endpoint, fixture->generation,
		events | POSTGAMMA_MEMORY_WAIT_PEER_CLOSED,
		0, fixture->deadline_ns);

	if (result.status == POSTGAMMA_MEMORY_STATUS_OK &&
		(result.events & events) != 0)
		return 0;
	postgamma_bootstrap_fixture_error(fixture, "memory transport wait failed");
	return -1;
}


static int
postgamma_bootstrap_read_exact(
	PostgammaBootstrapProtocolFixture *fixture, void *destination, size_t length)
{
	unsigned char *cursor = destination;
	size_t		remaining = length;

	while (remaining != 0)
	{
		PostgammaMemoryIoResult result = postgamma_memory_endpoint_read(
			fixture->endpoint, fixture->generation, cursor, remaining);

		if (result.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
		{
			cursor += result.bytes;
			remaining -= result.bytes;
			continue;
		}
		if (result.status == POSTGAMMA_MEMORY_STATUS_RETRY)
		{
			fixture->telemetry.read_retries++;
			if (postgamma_bootstrap_wait(
					fixture, POSTGAMMA_MEMORY_WAIT_READABLE) == 0)
				continue;
			return -1;
		}
		postgamma_bootstrap_fixture_error(fixture, "memory transport read failed");
		return -1;
	}
	return 0;
}


static void
postgamma_bootstrap_record_queue_saturation(
	PostgammaBootstrapProtocolFixture *fixture)
{
	PostgammaMemoryTelemetry telemetry;

	if (postgamma_memory_endpoint_snapshot(
			fixture->endpoint, fixture->generation, &telemetry) ==
		POSTGAMMA_MEMORY_STATUS_OK &&
		telemetry.simultaneous_full_observations != 0)
		fixture->telemetry.simultaneous_queue_saturation = true;
}


static int
postgamma_bootstrap_write_all(
	PostgammaBootstrapProtocolFixture *fixture, const void *source, size_t length)
{
	const unsigned char *cursor = source;
	size_t		remaining = length;

	while (remaining != 0)
	{
		PostgammaMemoryIoResult result = postgamma_memory_endpoint_write(
			fixture->endpoint, fixture->generation, cursor, remaining);

		if (result.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
		{
			cursor += result.bytes;
			remaining -= result.bytes;
			continue;
		}
		if (result.status == POSTGAMMA_MEMORY_STATUS_RETRY)
		{
			fixture->telemetry.write_retries++;
			postgamma_bootstrap_record_queue_saturation(fixture);
			if (postgamma_bootstrap_wait(
					fixture, POSTGAMMA_MEMORY_WAIT_WRITABLE) == 0)
				continue;
			return -1;
		}
		postgamma_bootstrap_fixture_error(fixture, "memory transport write failed");
		return -1;
	}
	return 0;
}


static int
postgamma_bootstrap_send_message(
	PostgammaBootstrapProtocolFixture *fixture, unsigned char type,
	const PostgammaBootstrapWireBuffer *payload)
{
	unsigned char header[5] = {0};

	header[0] = type;
	postgamma_bootstrap_write_u32(header + 1, (uint32_t) payload->length + 4U);
	if (postgamma_bootstrap_write_all(fixture, header, sizeof(header)) != 0)
		return -1;
	return postgamma_bootstrap_write_all(
		fixture, payload->bytes, payload->length);
}


static int
postgamma_bootstrap_send_authentication_ok(PostgammaBootstrapProtocolFixture *fixture)
{
	PostgammaBootstrapWireBuffer payload = {{0}, 0};

	if (postgamma_bootstrap_buffer_u32(&payload, 0) != 0)
		return -1;
	return postgamma_bootstrap_send_message(fixture, 'R', &payload);
}


static int
postgamma_bootstrap_send_parameter(
	PostgammaBootstrapProtocolFixture *fixture, const char *name, const char *value)
{
	PostgammaBootstrapWireBuffer payload = {{0}, 0};

	if (postgamma_bootstrap_buffer_string(&payload, name) != 0 ||
		postgamma_bootstrap_buffer_string(&payload, value) != 0)
		return -1;
	return postgamma_bootstrap_send_message(fixture, 'S', &payload);
}


static int
postgamma_bootstrap_send_backend_key(PostgammaBootstrapProtocolFixture *fixture)
{
	PostgammaBootstrapWireBuffer payload = {{0}, 0};

	if (postgamma_bootstrap_buffer_u32(&payload, 4242) != 0 ||
		postgamma_bootstrap_buffer_u32(&payload, UINT32_C(0x10203040)) != 0)
		return -1;
	return postgamma_bootstrap_send_message(fixture, 'K', &payload);
}


static int
postgamma_bootstrap_send_ready(PostgammaBootstrapProtocolFixture *fixture)
{
	PostgammaBootstrapWireBuffer payload = {{0}, 0};

	if (postgamma_bootstrap_buffer_u8(&payload, 'I') != 0)
		return -1;
	return postgamma_bootstrap_send_message(fixture, 'Z', &payload);
}


static int
postgamma_bootstrap_send_startup(PostgammaBootstrapProtocolFixture *fixture)
{
	return postgamma_bootstrap_send_authentication_ok(fixture) != 0 ||
		postgamma_bootstrap_send_parameter(
			fixture, "server_version", "19devel") != 0 ||
		postgamma_bootstrap_send_parameter(
			fixture, "server_encoding", "UTF8") != 0 ||
		postgamma_bootstrap_send_parameter(
			fixture, "client_encoding", "UTF8") != 0 ||
		postgamma_bootstrap_send_parameter(
			fixture, "standard_conforming_strings", "on") != 0 ||
		postgamma_bootstrap_send_backend_key(fixture) != 0 ||
		postgamma_bootstrap_send_ready(fixture) != 0 ? -1 : 0;
}


static int
postgamma_bootstrap_send_result(PostgammaBootstrapProtocolFixture *fixture)
{
	PostgammaBootstrapWireBuffer row = {{0}, 0};
	PostgammaBootstrapWireBuffer data = {{0}, 0};
	PostgammaBootstrapWireBuffer command = {{0}, 0};
	unsigned char value = '1';

	if (postgamma_bootstrap_buffer_u16(&row, 1) != 0 ||
		postgamma_bootstrap_buffer_string(&row, "one") != 0 ||
		postgamma_bootstrap_buffer_u32(&row, 0) != 0 ||
		postgamma_bootstrap_buffer_u16(&row, 0) != 0 ||
		postgamma_bootstrap_buffer_u32(&row, 23) != 0 ||
		postgamma_bootstrap_buffer_u16(&row, 4) != 0 ||
		postgamma_bootstrap_buffer_u32(&row, UINT32_MAX) != 0 ||
		postgamma_bootstrap_buffer_u16(&row, 0) != 0 ||
		postgamma_bootstrap_buffer_u16(&data, 1) != 0 ||
		postgamma_bootstrap_buffer_u32(&data, 1) != 0 ||
		postgamma_bootstrap_buffer_append(&data, &value, 1) != 0 ||
		postgamma_bootstrap_buffer_string(&command, "SELECT 1") != 0)
		return -1;
	return postgamma_bootstrap_send_message(fixture, 'T', &row) != 0 ||
		postgamma_bootstrap_send_message(fixture, 'D', &data) != 0 ||
		postgamma_bootstrap_send_message(fixture, 'C', &command) != 0 ||
		postgamma_bootstrap_send_ready(fixture) != 0 ? -1 : 0;
}


static int
postgamma_bootstrap_error_field(
	PostgammaBootstrapWireBuffer *payload, unsigned char field, const char *value)
{
	return postgamma_bootstrap_buffer_u8(payload, field) != 0 ||
		postgamma_bootstrap_buffer_string(payload, value) != 0 ? -1 : 0;
}


static int
postgamma_bootstrap_send_diagnostic(
	PostgammaBootstrapProtocolFixture *fixture, unsigned char type,
	const char *severity, const char *sqlstate,
	const char *message, const char *detail)
{
	PostgammaBootstrapWireBuffer payload = {{0}, 0};

	if (postgamma_bootstrap_error_field(&payload, 'S', severity) != 0 ||
		postgamma_bootstrap_error_field(&payload, 'V', severity) != 0 ||
		postgamma_bootstrap_error_field(&payload, 'C', sqlstate) != 0 ||
		postgamma_bootstrap_error_field(&payload, 'M', message) != 0 ||
		postgamma_bootstrap_error_field(&payload, 'D', detail) != 0 ||
		postgamma_bootstrap_buffer_u8(&payload, 0) != 0)
		return -1;
	return postgamma_bootstrap_send_message(fixture, type, &payload);
}


static int
postgamma_bootstrap_send_query_error(PostgammaBootstrapProtocolFixture *fixture)
{
	return postgamma_bootstrap_send_diagnostic(
			fixture, 'E', "ERROR", "22012", "division by zero",
			"bootstrap protocol fixture query error") != 0 ||
		postgamma_bootstrap_send_ready(fixture) != 0 ? -1 : 0;
}


static int
postgamma_bootstrap_send_copy_start(PostgammaBootstrapProtocolFixture *fixture)
{
	PostgammaBootstrapWireBuffer payload = {{0}, 0};

	if (postgamma_bootstrap_buffer_u8(&payload, 0) != 0 ||
		postgamma_bootstrap_buffer_u16(&payload, 0) != 0)
		return -1;
	return postgamma_bootstrap_send_message(fixture, 'G', &payload);
}


static int
postgamma_bootstrap_send_copy_rejection(PostgammaBootstrapProtocolFixture *fixture)
{
	return postgamma_bootstrap_send_diagnostic(
			fixture, 'N', "NOTICE", "00000", "fixture copy notice",
			"notice emitted while COPY input is still pending") != 0 ||
		postgamma_bootstrap_send_diagnostic(
			fixture, 'E', "ERROR", "57014", "fixture rejected COPY",
			"error emitted before the complete COPY payload arrived") != 0 ||
		postgamma_bootstrap_send_ready(fixture) != 0 ? -1 : 0;
}


static int
postgamma_bootstrap_read_header(
	PostgammaBootstrapProtocolFixture *fixture, unsigned char *type,
	uint32_t *payload_length)
{
	unsigned char header[5] = {0};
	uint32_t	wire_length;

	if (postgamma_bootstrap_read_exact(fixture, header, sizeof(header)) != 0)
		return -1;
	wire_length = postgamma_bootstrap_read_u32(header + 1);
	if (wire_length < 4 || wire_length > POSTGAMMA_BOOTSTRAP_FIXTURE_MAX_PACKET)
	{
		postgamma_bootstrap_fixture_error(fixture, "invalid frontend packet length");
		return -1;
	}
	*type = header[0];
	*payload_length = wire_length - 4;
	return 0;
}


static int
postgamma_bootstrap_read_query(
	PostgammaBootstrapProtocolFixture *fixture, const char *expected)
{
	unsigned char type;
	uint32_t	length;
	unsigned char payload[256] = {0};

	if (postgamma_bootstrap_read_header(fixture, &type, &length) != 0)
		return -1;
	if (type != 'Q' || length == 0 || length > sizeof(payload) ||
		postgamma_bootstrap_read_exact(fixture, payload, length) != 0)
	{
		postgamma_bootstrap_fixture_error(fixture, "invalid simple-query packet");
		return -1;
	}
	if (payload[length - 1] != '\0' || strlen((const char *) payload) + 1 != length ||
		strcmp((const char *) payload, expected) != 0)
	{
		postgamma_bootstrap_fixture_error(fixture, "unexpected simple-query text");
		return -1;
	}
	fixture->telemetry.query_packets++;
	return 0;
}


static bool
postgamma_bootstrap_startup_value(
	const unsigned char *payload, size_t length,
	const char *wanted_name, const char *wanted_value)
{
	size_t		offset = 4;

	while (offset < length && payload[offset] != '\0')
	{
		const unsigned char *name_end = memchr(
			payload + offset, '\0', length - offset);
		const unsigned char *value;
		const unsigned char *value_end;

		if (name_end == NULL)
			return false;
		value = name_end + 1;
		if (value >= payload + length)
			return false;
		value_end = memchr(value, '\0', (size_t) (payload + length - value));
		if (value_end == NULL)
			return false;
		if (strcmp((const char *) (payload + offset), wanted_name) == 0 &&
			strcmp((const char *) value, wanted_value) == 0)
			return true;
		offset = (size_t) (value_end - payload) + 1;
	}
	return false;
}


static int
postgamma_bootstrap_read_startup(PostgammaBootstrapProtocolFixture *fixture)
{
	unsigned char length_bytes[4] = {0};
	unsigned char *payload;
	uint32_t	wire_length;
	size_t		payload_length;
	bool		valid;

	if (postgamma_bootstrap_read_exact(
			fixture, length_bytes, sizeof(length_bytes)) != 0)
		return -1;
	wire_length = postgamma_bootstrap_read_u32(length_bytes);
	if (wire_length < 9 || wire_length > POSTGAMMA_BOOTSTRAP_FIXTURE_MAX_PACKET)
	{
		postgamma_bootstrap_fixture_error(fixture, "invalid startup packet length");
		return -1;
	}
	payload_length = wire_length - 4;
	payload = calloc(1, payload_length);
	if (payload == NULL)
	{
		postgamma_bootstrap_fixture_error(fixture, "cannot allocate startup packet");
		return -1;
	}
	if (postgamma_bootstrap_read_exact(fixture, payload, payload_length) != 0)
	{
		free(payload);
		return -1;
	}
	valid = postgamma_bootstrap_read_u32(payload) == UINT32_C(196608) &&
		payload[payload_length - 1] == '\0' &&
		postgamma_bootstrap_startup_value(
			payload, payload_length, "user", "embedded_user") &&
		postgamma_bootstrap_startup_value(
			payload, payload_length, "database", "embedded_db");
	free(payload);
	if (!valid)
	{
		postgamma_bootstrap_fixture_error(fixture, "startup parameters are invalid");
		return -1;
	}
	fixture->telemetry.startup_packets++;
	fixture->telemetry.startup_parameters_valid = true;
	return 0;
}


static int
postgamma_bootstrap_read_copy(PostgammaBootstrapProtocolFixture *fixture)
{
	unsigned char type;
	uint32_t	length;
	unsigned char buffer[256];
	size_t		remaining;

	if (postgamma_bootstrap_read_header(fixture, &type, &length) != 0)
		return -1;
	if (type != 'd' || length != POSTGAMMA_BOOTSTRAP_COPY_LENGTH)
	{
		postgamma_bootstrap_fixture_error(fixture, "invalid COPY data packet");
		return -1;
	}
	if (postgamma_bootstrap_read_exact(
			fixture, buffer, POSTGAMMA_BOOTSTRAP_COPY_PREFIX) != 0)
		return -1;
	fixture->telemetry.copy_bytes_before_response = POSTGAMMA_BOOTSTRAP_COPY_PREFIX;
	if (postgamma_bootstrap_send_copy_rejection(fixture) != 0)
		return -1;
	postgamma_bootstrap_record_queue_saturation(fixture);
	fixture->telemetry.response_preceded_copy_completion = true;
	remaining = length - POSTGAMMA_BOOTSTRAP_COPY_PREFIX;
	while (remaining != 0)
	{
		size_t amount = remaining < sizeof(buffer) ? remaining : sizeof(buffer);

		if (postgamma_bootstrap_read_exact(fixture, buffer, amount) != 0)
			return -1;
		remaining -= amount;
	}
	fixture->telemetry.copy_bytes_received = length;
	fixture->telemetry.copy_packets++;
	if (postgamma_bootstrap_read_header(fixture, &type, &length) != 0)
		return -1;
	if (type != 'c' || length != 0)
	{
		postgamma_bootstrap_fixture_error(fixture, "COPY Done packet is missing");
		return -1;
	}
	return 0;
}


static int
postgamma_bootstrap_wait_for_close(PostgammaBootstrapProtocolFixture *fixture)
{
	unsigned char byte;

	for (;;)
	{
		PostgammaMemoryIoResult result = postgamma_memory_endpoint_read(
			fixture->endpoint, fixture->generation, &byte, sizeof(byte));

		if (result.status == POSTGAMMA_MEMORY_STATUS_EOF)
		{
			fixture->telemetry.frontend_half_close_observed = true;
			return 0;
		}
		if (result.status == POSTGAMMA_MEMORY_STATUS_RETRY)
		{
			PostgammaMemoryWaitResult wait_result;

			fixture->telemetry.read_retries++;
			wait_result = postgamma_memory_endpoint_wait(
				fixture->endpoint, fixture->generation,
				POSTGAMMA_MEMORY_WAIT_READABLE |
					POSTGAMMA_MEMORY_WAIT_PEER_CLOSED,
				0, fixture->deadline_ns);
			if (wait_result.status != POSTGAMMA_MEMORY_STATUS_OK &&
				wait_result.status != POSTGAMMA_MEMORY_STATUS_EOF &&
				wait_result.status != POSTGAMMA_MEMORY_STATUS_PEER_CLOSED)
			{
				postgamma_bootstrap_fixture_error(
					fixture, "memory transport close wait failed");
				return -1;
			}
			continue;
		}
		postgamma_bootstrap_fixture_error(fixture, "unexpected data after final query");
		return -1;
	}
}


static void *
postgamma_bootstrap_fixture_main(void *argument)
{
	PostgammaBootstrapProtocolFixture *fixture = argument;
	PostgammaMemoryStatus close_status;

	fixture->status = postgamma_bootstrap_read_startup(fixture) != 0 ||
		postgamma_bootstrap_send_startup(fixture) != 0 ||
		postgamma_bootstrap_read_query(fixture, "SELECT 1") != 0 ||
		postgamma_bootstrap_send_result(fixture) != 0 ||
		postgamma_bootstrap_read_query(fixture, "SELECT 1 / 0") != 0 ||
		postgamma_bootstrap_send_query_error(fixture) != 0 ||
		postgamma_bootstrap_read_query(fixture, "SELECT 1") != 0 ||
		postgamma_bootstrap_send_result(fixture) != 0 ||
		postgamma_bootstrap_read_query(fixture, "COPY fixture FROM STDIN") != 0 ||
		postgamma_bootstrap_send_copy_start(fixture) != 0 ||
		postgamma_bootstrap_read_copy(fixture) != 0 ||
		postgamma_bootstrap_read_query(fixture, "SELECT 1") != 0 ||
		postgamma_bootstrap_send_result(fixture) != 0 ||
		postgamma_bootstrap_wait_for_close(fixture) != 0 ? -1 : 0;
	if (fixture->status == 0)
		(void) snprintf(
			fixture->telemetry.detail,
			sizeof(fixture->telemetry.detail),
			"PG19 libpq protocol fixture passed");
	else
		(void) postgamma_memory_endpoint_abort(
			fixture->endpoint, fixture->generation,
			POSTGAMMA_MEMORY_ABORT_PROTOCOL);
	close_status = postgamma_memory_endpoint_half_close_write(
		fixture->endpoint, fixture->generation);
	if (close_status != POSTGAMMA_MEMORY_STATUS_OK &&
		close_status != POSTGAMMA_MEMORY_STATUS_LOCAL_CLOSED)
		fixture->status = -1;
	if (postgamma_memory_endpoint_release(
			&fixture->endpoint, fixture->generation) !=
		POSTGAMMA_MEMORY_STATUS_OK)
		fixture->status = -1;
	return NULL;
}


int
postgamma_bootstrap_protocol_fixture_start(
	PostgammaMemoryEndpoint **backend_endpoint,
	uint64_t generation,
	PostgammaBootstrapProtocolFixture **fixture)
{
	PostgammaBootstrapProtocolFixture *created;
	int64_t		now;

	if (backend_endpoint == NULL || *backend_endpoint == NULL ||
		fixture == NULL || generation == 0)
		return -1;
	*fixture = NULL;
	if (postgamma_memory_clock_now(&now) != POSTGAMMA_MEMORY_STATUS_OK ||
		now > INT64_MAX - POSTGAMMA_BOOTSTRAP_FIXTURE_DEADLINE_NS)
		return -1;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return -1;
	created->generation = generation;
	created->deadline_ns = now + POSTGAMMA_BOOTSTRAP_FIXTURE_DEADLINE_NS;
	created->endpoint = *backend_endpoint;
	if (pthread_create(
			&created->thread, NULL, postgamma_bootstrap_fixture_main, created) != 0)
	{
		free(created);
		return -1;
	}
	*backend_endpoint = NULL;
	*fixture = created;
	return 0;
}


int
postgamma_bootstrap_protocol_fixture_join(
	PostgammaBootstrapProtocolFixture **fixture,
	PostgammaBootstrapProtocolFixtureTelemetry *telemetry)
{
	PostgammaBootstrapProtocolFixture *joining;
	int			status;

	if (fixture == NULL || *fixture == NULL || telemetry == NULL)
		return -1;
	joining = *fixture;
	status = pthread_join(joining->thread, NULL);
	*telemetry = joining->telemetry;
	if (status == 0)
		status = joining->status;
	free(joining);
	*fixture = NULL;
	return status;
}
