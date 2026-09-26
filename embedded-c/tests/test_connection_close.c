#define _POSIX_C_SOURCE 200809L

#include "postgamma/private/public_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_GENERATION UINT64_C(41)

/* Keep the real public close, event router and memory transport. Only the
 * libpq frontend is a fixture; its backend reference is released by the test. */
struct PostgammaPrivateLibpqConnection
{
	PostgammaMemoryEndpoint *frontend;
};

static unsigned int notify_calls;
static unsigned int close_calls;

PostgammaPrivateLibpqStatus
postgamma_private_libpq_set_notify(
	PostgammaPrivateLibpqConnection *connection,
	PostgammaMemoryNotifyFunction notify, void *argument)
{
	notify_calls++;
	if (connection == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	return postgamma_memory_endpoint_set_notify(
		connection->frontend, TEST_GENERATION, notify, argument) ==
		POSTGAMMA_MEMORY_STATUS_OK ? POSTGAMMA_PRIVATE_LIBPQ_OK :
		POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
}

PostgammaPrivateLibpqStatus
postgamma_private_libpq_close(PostgammaPrivateLibpqConnection **connection)
{
	assert(connection != NULL && *connection != NULL);
	assert(postgamma_memory_endpoint_release(
		&(*connection)->frontend, TEST_GENERATION) == POSTGAMMA_MEMORY_STATUS_OK);
	free(*connection);
	*connection = NULL;
	close_calls++;
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}

int
main(void)
{
	pgm_instance instance = {0};
	pgm_connection *connection = calloc(1, sizeof(*connection));
	PostgammaMemoryEndpoint *backend = NULL;
	PostgammaMemoryGlobalTelemetry before, after;
	pgm_error *error = NULL;
	pgm_status status;
	unsigned int notifications_after_close;

	postgamma_memory_global_telemetry(&before);
	assert(connection != NULL);
	instance.magic = PGM_INSTANCE_MAGIC;
	instance.owner_pid = getpid();
	instance.generation = TEST_GENERATION;
	assert(postgamma_mutex_create(&instance.mutex) == 0);
	assert(postgamma_public_event_router_create(&instance, 8, NULL, NULL) == 0);
	connection->magic = PGM_CONNECTION_MAGIC;
	connection->owner_pid = instance.owner_pid;
	connection->instance = &instance;
	connection->private_connection = calloc(1, sizeof(*connection->private_connection));
	assert(connection->private_connection != NULL);
	assert(postgamma_mutex_create(&connection->mutex) == 0);
	atomic_init(&connection->event_pending_next, NULL);
	atomic_init(&connection->event_ready_pending, false);
	assert(postgamma_memory_transport_create(TEST_GENERATION, 64,
		&connection->private_connection->frontend, &backend) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_retain(backend, TEST_GENERATION,
		&connection->backend_monitor) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_public_event_register_connection(connection) == 0);
	instance.connection_count = 1;
	connection->registered_with_instance = true;

	/* Before frontend teardown, a busy event pump still permits a clean retry. */
	connection->event_pump_references = 1;
	status = pgm_connection_close(connection, 0, &error);
	assert(status == PGM_STATUS_BUSY && pgm_error_status(error) == status);
	assert(connection->private_connection != NULL && !connection->closing);
	assert(notify_calls == 2 && close_calls == 0);
	pgm_error_free(error);
	connection->event_pump_references = 0;

	/* A retained backend makes each timeout deterministic, even on a fast host. */
	for (unsigned int attempt = 0; attempt < 3; attempt++)
	{
		status = pgm_connection_close(connection, attempt == 0 ? 0 : 1, &error);
		assert(status == PGM_STATUS_TIMEOUT && pgm_error_status(error) == status);
		assert(connection->private_connection == NULL && !connection->closing);
		assert(instance.connection_count == 1 && connection->registered_with_instance);
		assert(close_calls == 1);
		pgm_error_free(error);
	}
	notifications_after_close = notify_calls;
	assert(postgamma_memory_endpoint_release(&backend, TEST_GENERATION) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	/* A nonblocking retry must also be able to finish, not merely time out. */
	status = pgm_connection_close(connection, 0, &error);
	assert(status == PGM_STATUS_OK && error == NULL);
	assert(instance.connection_count == 0 && close_calls == 1);
	assert(notify_calls == notifications_after_close);
	assert(postgamma_public_event_router_destroy(&instance) == 0);
	assert(postgamma_mutex_destroy(instance.mutex) == 0);
	postgamma_memory_global_telemetry(&after);
	assert(after.active_transports == before.active_transports);
	assert(after.endpoint_references == before.endpoint_references);
	assert(after.allocated_bytes == before.allocated_bytes);
	puts("connection close: busy retry, three timeouts, final retry and resource release passed");
	return 0;
}
