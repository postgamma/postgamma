/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L

/* Exercise the real queue under deterministic contention without exposing its
 * lock or adding test hooks to the production API. */
#include "../src/postgamma_events.c"

#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static PostgammaMutex *contended_mutex;
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t attempted_condition = PTHREAD_COND_INITIALIZER;
static bool attempted;
static _Thread_local bool producer;

int __real_postgamma_mutex_lock(PostgammaMutex *mutex);
int __real_postgamma_mutex_try_lock(PostgammaMutex *mutex);
int __wrap_postgamma_mutex_lock(PostgammaMutex *mutex);
int __wrap_postgamma_mutex_try_lock(PostgammaMutex *mutex);

static void
signal_attempt(void)
{
	assert(pthread_mutex_lock(&test_mutex) == 0);
	attempted = true;
	assert(pthread_cond_signal(&attempted_condition) == 0);
	assert(pthread_mutex_unlock(&test_mutex) == 0);
}

int
__wrap_postgamma_mutex_lock(PostgammaMutex *mutex)
{
	if (producer && mutex == contended_mutex)
		signal_attempt();
	return __real_postgamma_mutex_lock(mutex);
}

int
__wrap_postgamma_mutex_try_lock(PostgammaMutex *mutex)
{
	int status = __real_postgamma_mutex_try_lock(mutex);

	/* Signal after trylock, so the consumer cannot accidentally make the old
	 * drop-on-contention implementation pass by releasing its lock early. */
	if (producer && mutex == contended_mutex)
		signal_attempt();
	return status;
}

typedef struct ProducerArguments
{
	pgm_connection *connection;
	pgm_event_kind kind;
} ProducerArguments;

static void *
publish(void *argument)
{
	ProducerArguments *arguments = argument;

	producer = true;
	if (arguments->kind == PGM_EVENT_NOTIFICATION)
		postgamma_public_event_enqueue_notification(
			arguments->connection, 123, "contended", "delivered");
	else
		postgamma_public_event_enqueue_notice(arguments->connection,
			"00000", "NOTICE", "delivered", "", "");
	return NULL;
}

static void
publish_with_consumer_lock(pgm_connection *connection, pgm_event_kind kind)
{
	ProducerArguments arguments = {connection, kind};
	struct timespec deadline;
	pthread_t thread;
	pgm_event *event;
	bool out_of_memory = false;

	assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
	deadline.tv_sec += 5;
	assert(postgamma_mutex_lock(contended_mutex) == 0);
	assert(pthread_mutex_lock(&test_mutex) == 0);
	attempted = false;
	assert(pthread_create(&thread, NULL, publish, &arguments) == 0);
	while (!attempted)
		assert(pthread_cond_timedwait(
			&attempted_condition, &test_mutex, &deadline) == 0);
	assert(pthread_mutex_unlock(&test_mutex) == 0);
	assert(postgamma_mutex_unlock(contended_mutex) == 0);
	assert(pthread_join(thread, NULL) == 0);

	assert(postgamma_mutex_lock(contended_mutex) == 0);
	event = take_event_locked(connection->instance->event_router,
		false, 0, 0, &out_of_memory);
	assert(postgamma_mutex_unlock(contended_mutex) == 0);
	assert(!out_of_memory && event != NULL && pgm_event_type(event) == kind);
	assert(pgm_event_connection(event) == connection->identity);
	if (kind == PGM_EVENT_NOTIFICATION)
	{
		pgm_notification notification = PGM_NOTIFICATION_INIT;

		assert(pgm_event_notification(event, &notification, NULL) == PGM_STATUS_OK);
		assert(notification.virtual_backend_pid == 123);
		assert(strcmp(notification.channel, "contended") == 0);
		assert(strcmp(notification.payload, "delivered") == 0);
	}
	else
	{
		pgm_notice notice = PGM_NOTICE_INIT;

		assert(pgm_event_notice(event, &notice, NULL) == PGM_STATUS_OK);
		assert(strcmp(notice.message, "delivered") == 0);
	}
	pgm_event_free(event);
	assert(connection->instance->event_router->count == 0);
	assert(!overflow_pending_locked(connection->instance->event_router));
}

int
main(void)
{
	pgm_instance instance = {0};
	pgm_connection connection = {0};
	PostgammaKernelLogRecord log = {0};
	pgm_event *overflow;
	pgm_event_overflow_record record = PGM_EVENT_OVERFLOW_RECORD_INIT;
	bool out_of_memory = false;

	alarm(10);
	instance.magic = PGM_INSTANCE_MAGIC;
	instance.owner_pid = getpid();
	instance.generation = 41;
	assert(postgamma_public_event_router_create(&instance, 2, NULL, NULL) == 0);
	contended_mutex = instance.event_router->mutex;
	connection.magic = PGM_CONNECTION_MAGIC;
	connection.owner_pid = getpid();
	connection.instance = &instance;
	connection.identity = 7;
	publish_with_consumer_lock(&connection, PGM_EVENT_NOTIFICATION);
	publish_with_consumer_lock(&connection, PGM_EVENT_NOTICE);

	/* Full queues still report overflow; no unbounded queue or hidden loss. */
	for (unsigned int index = 0; index < 3; index++)
		postgamma_public_event_enqueue_notification(
			&connection, 123, "full", "payload");
	assert(instance.event_router->count == 2);
	overflow = take_overflow_locked(instance.event_router, &out_of_memory);
	assert(!out_of_memory && overflow != NULL);
	assert(pgm_event_overflow(overflow, &record, NULL) == PGM_STATUS_OK);
	assert(record.dropped_kind == PGM_EVENT_NOTIFICATION && record.dropped_count == 1);
	pgm_event_free(overflow);

	/* Kernel logging cannot block on an already-owned router lock. */
	log.struct_size = sizeof(log);
	assert(postgamma_mutex_lock(contended_mutex) == 0);
	assert(postgamma_public_event_enqueue_kernel_log(&instance, 41, &log) == 0);
	assert(postgamma_mutex_unlock(contended_mutex) == 0);
	assert(atomic_load(&instance.event_router->dropped[0]) == 1);
	assert(postgamma_public_event_router_destroy(&instance) == 0);
	assert(pthread_cond_destroy(&attempted_condition) == 0);
	assert(pthread_mutex_destroy(&test_mutex) == 0);
	puts("event contention: notification/notice delivery, full-queue overflow and nonblocking logs passed");
	return 0;
}
