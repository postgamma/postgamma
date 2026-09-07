/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "postgamma/private/public_runtime.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define PGM_EVENT_SEVERITY_CAPACITY 32U
#define PGM_EVENT_SQLSTATE_CAPACITY 6U
#define PGM_EVENT_MESSAGE_CAPACITY 2048U
#define PGM_EVENT_DETAIL_CAPACITY 2048U
#define PGM_EVENT_HINT_CAPACITY 1024U
#define PGM_EVENT_CHANNEL_CAPACITY 256U
#define PGM_EVENT_PAYLOAD_CAPACITY 8192U
#define PGM_EVENT_PUMP_BUDGET 64U


typedef union PostgammaPublicEventCallback
{
	pgm_log_callback log;
	pgm_notice_callback notice;
	pgm_notification_callback notification;
} PostgammaPublicEventCallback;

typedef struct PostgammaPublicLogPayload
{
	int32_t		virtual_backend_pid;
	char		severity[PGM_EVENT_SEVERITY_CAPACITY];
	char		sqlstate[PGM_EVENT_SQLSTATE_CAPACITY];
	char		message[PGM_EVENT_MESSAGE_CAPACITY];
	char		detail[PGM_EVENT_DETAIL_CAPACITY];
} PostgammaPublicLogPayload;

typedef struct PostgammaPublicNoticePayload
{
	char		sqlstate[PGM_EVENT_SQLSTATE_CAPACITY];
	char		severity[PGM_EVENT_SEVERITY_CAPACITY];
	char		message[PGM_EVENT_MESSAGE_CAPACITY];
	char		detail[PGM_EVENT_DETAIL_CAPACITY];
	char		hint[PGM_EVENT_HINT_CAPACITY];
} PostgammaPublicNoticePayload;

typedef struct PostgammaPublicNotificationPayload
{
	int32_t		virtual_backend_pid;
	char		channel[PGM_EVENT_CHANNEL_CAPACITY];
	char		payload[PGM_EVENT_PAYLOAD_CAPACITY];
} PostgammaPublicNotificationPayload;

struct pgm_event
{
	uint64_t	magic;
	pgm_event_kind kind;
	pgm_connection_id connection_id;
	pgm_request_id request_id;
	PostgammaPublicEventCallback callback;
	void	   *callback_user_data;
	union
	{
		PostgammaPublicLogPayload log;
		PostgammaPublicNoticePayload notice;
		PostgammaPublicNotificationPayload notification;
		pgm_event_overflow_record overflow;
	} payload;
};

struct PostgammaPublicEventRouter
{
	pgm_instance *instance;
	PostgammaMutex *mutex;
	PostgammaWakeTarget *wake_target;
	pgm_event  *events;
	size_t		capacity;
	size_t		head;
	size_t		count;
	pgm_log_callback log_callback;
	void	   *log_user_data;
	pgm_connection *connections;
	pgm_connection *ready_head;
	pgm_connection *ready_tail;
	_Atomic(pgm_connection *) pending_head;
	_Atomic uint64_t dropped[3];
	uint64_t	reported_dropped[3];
};

static _Thread_local PostgammaPublicCallbackFrame *PostgammaCallbackFrame;


static bool copy_event_text(
	char *target, size_t capacity, const char *source);
static int event_drop_index(pgm_event_kind kind);
static void record_drop(
	PostgammaPublicEventRouter *router, pgm_event_kind kind);
static void enqueue_event(
	PostgammaPublicEventRouter *router, const pgm_event *event);
static void preserve_level_wake_locked(PostgammaPublicEventRouter *router);
static bool overflow_pending_locked(PostgammaPublicEventRouter *router);
static pgm_event *take_event_locked(
	PostgammaPublicEventRouter *router, bool callbacks_only,
	pgm_connection_id connection_id, pgm_request_id request_id,
	bool *out_of_memory);
static pgm_event *take_overflow_locked(
	PostgammaPublicEventRouter *router, bool *out_of_memory);
static void remove_ready_locked(
	PostgammaPublicEventRouter *router, pgm_connection *connection);
static void enqueue_ready_locked(
	PostgammaPublicEventRouter *router, pgm_connection *connection);
static void collect_pending_locked(PostgammaPublicEventRouter *router);
static bool request_progress_ready(pgm_request *request);
static pgm_connection *take_ready_connection(
	PostgammaPublicEventRouter *router);
static void finish_ready_connection(
	PostgammaPublicEventRouter *router, pgm_connection *connection);
static void pump_ready_connections(
	PostgammaPublicEventRouter *router, size_t budget);
static void invoke_event_callback(
	pgm_instance *instance, const pgm_event *event);
static size_t dispatch_matching(
	PostgammaPublicEventRouter *router, size_t maximum_events,
	pgm_connection_id connection_id, pgm_request_id request_id,
	bool *out_of_memory);
static bool event_matches_callback(
	const pgm_event *event, pgm_connection_id connection_id,
	pgm_request_id request_id);
static int poll_waitable(int descriptor, uint64_t deadline_ns);


bool
public_instance_reentrant(const pgm_instance *instance)
{
	PostgammaPublicCallbackFrame *frame = PostgammaCallbackFrame;

	while (frame != NULL)
	{
		if (frame->instance == instance)
			return true;
		frame = frame->previous;
	}
	return false;
}


void
postgamma_public_callback_enter(
	PostgammaPublicCallbackFrame *frame, pgm_instance *instance)
{
	if (frame == NULL || !instance_is_valid(instance))
		return;
	frame->instance = instance;
	frame->previous = PostgammaCallbackFrame;
	PostgammaCallbackFrame = frame;
}


void
postgamma_public_callback_leave(PostgammaPublicCallbackFrame *frame)
{
	if (frame == NULL || PostgammaCallbackFrame != frame)
		return;
	PostgammaCallbackFrame = frame->previous;
	frame->instance = NULL;
	frame->previous = NULL;
}


int
postgamma_public_event_router_create(
	pgm_instance *instance, size_t capacity,
	pgm_log_callback log_callback, void *log_user_data)
{
	PostgammaPublicEventRouter *router;
	int			status;

	if (!instance_is_valid(instance) || capacity == 0 ||
		capacity > SIZE_MAX / sizeof(*router->events))
		return EINVAL;
	router = calloc(1, sizeof(*router));
	if (router == NULL)
		return ENOMEM;
	router->events = calloc(capacity, sizeof(*router->events));
	if (router->events == NULL)
	{
		free(router);
		return ENOMEM;
	}
	status = postgamma_mutex_create(&router->mutex);
	if (status == 0)
		status = postgamma_wake_target_create(&router->wake_target);
	if (status != 0)
	{
		if (router->mutex != NULL)
			(void) postgamma_mutex_destroy(router->mutex);
		free(router->events);
		free(router);
		return status;
	}
	router->instance = instance;
	router->capacity = capacity;
	router->log_callback = log_callback;
	router->log_user_data = log_user_data;
	atomic_init(&router->pending_head, NULL);
	for (size_t index = 0; index < 3; index++)
		atomic_init(&router->dropped[index], UINT64_C(0));
	instance->event_router = router;
	return 0;
}


int
postgamma_public_event_router_destroy(pgm_instance *instance)
{
	PostgammaPublicEventRouter *router;
	int			status;

	if (!instance_is_valid(instance) || instance->event_router == NULL)
		return EINVAL;
	router = instance->event_router;
	status = postgamma_mutex_lock(router->mutex);
	if (status != 0)
		return status;
	collect_pending_locked(router);
	if (router->connections != NULL || router->ready_head != NULL ||
		atomic_load_explicit(&router->pending_head, memory_order_acquire) != NULL)
	{
		(void) postgamma_mutex_unlock(router->mutex);
		return EBUSY;
	}
	(void) postgamma_mutex_unlock(router->mutex);
	status = postgamma_wake_target_destroy(router->wake_target);
	if (status == 0)
		status = postgamma_mutex_destroy(router->mutex);
	if (status != 0)
		return status;
	free(router->events);
	free(router);
	instance->event_router = NULL;
	return 0;
}


int
postgamma_public_event_register_connection(pgm_connection *connection)
{
	PostgammaPublicEventRouter *router;
	int			status;

	if (!connection_is_valid(connection) ||
		connection->instance->event_router == NULL)
		return EINVAL;
	router = connection->instance->event_router;
	status = postgamma_mutex_lock(router->mutex);
	if (status != 0)
		return status;
	if (connection->event_registered)
	{
		(void) postgamma_mutex_unlock(router->mutex);
		return EALREADY;
	}
	connection->event_registry_next = router->connections;
	router->connections = connection;
	connection->event_registered = true;
	collect_pending_locked(router);
	return postgamma_mutex_unlock(router->mutex);
}


int
postgamma_public_event_begin_connection_close(pgm_connection *connection)
{
	PostgammaPublicEventRouter *router;
	pgm_connection **link;
	int			status;

	if (!connection_is_valid(connection) ||
		connection->instance->event_router == NULL)
		return EINVAL;
	router = connection->instance->event_router;
	status = postgamma_mutex_lock(router->mutex);
	if (status != 0)
		return status;
	if (!connection->event_registered)
		return postgamma_mutex_unlock(router->mutex);
	if (connection->event_pump_references != 0)
	{
		(void) postgamma_mutex_unlock(router->mutex);
		return EBUSY;
	}
	connection->event_registered = false;
	collect_pending_locked(router);
	remove_ready_locked(router, connection);
	link = &router->connections;
	while (*link != NULL && *link != connection)
		link = &(*link)->event_registry_next;
	if (*link != connection)
	{
		(void) postgamma_mutex_unlock(router->mutex);
		return EPROTO;
	}
	*link = connection->event_registry_next;
	connection->event_registry_next = NULL;
	atomic_store_explicit(
		&connection->event_ready_pending, false, memory_order_release);
	atomic_store_explicit(
		&connection->event_pending_next, NULL, memory_order_release);
	return postgamma_mutex_unlock(router->mutex);
}


void
postgamma_public_event_cancel_connection_close(pgm_connection *connection)
{
	if (connection_is_valid(connection) && !connection->event_registered)
		(void) postgamma_public_event_register_connection(connection);
}


void
postgamma_public_event_finish_connection_close(pgm_connection *connection)
{
	PostgammaPublicEventRouter *router;

	if (!connection_is_valid(connection) ||
		connection->instance->event_router == NULL)
		return;
	router = connection->instance->event_router;
	if (postgamma_mutex_lock(router->mutex) != 0)
		return;
	for (size_t offset = 0; offset < router->count; offset++)
	{
		pgm_event *event =
			&router->events[(router->head + offset) % router->capacity];

		if (event->connection_id == connection->identity)
		{
			memset(&event->callback, 0, sizeof(event->callback));
			event->callback_user_data = NULL;
		}
	}
	(void) postgamma_mutex_unlock(router->mutex);
}


void
postgamma_public_event_forget_request(pgm_request *request)
{
	PostgammaPublicEventRouter *router;

	if (!request_is_valid(request) ||
		request->connection->instance->event_router == NULL)
		return;
	router = request->connection->instance->event_router;
	if (postgamma_mutex_lock(router->mutex) != 0)
		return;
	for (size_t offset = 0; offset < router->count; offset++)
	{
		pgm_event *event =
			&router->events[(router->head + offset) % router->capacity];

		if (event->request_id == request->generation)
		{
			memset(&event->callback, 0, sizeof(event->callback));
			event->callback_user_data = NULL;
		}
	}
	(void) postgamma_mutex_unlock(router->mutex);
}


void
postgamma_public_event_notify_connection(
	pgm_connection *connection, uint32_t events)
{
	PostgammaPublicEventRouter *router;
	pgm_connection *head;

	(void) events;
	if (!connection_is_valid(connection) ||
		connection->instance->event_router == NULL)
		return;
	router = connection->instance->event_router;
	if (!atomic_exchange_explicit(
			&connection->event_ready_pending, true, memory_order_acq_rel))
	{
		head = atomic_load_explicit(
			&router->pending_head, memory_order_acquire);
		do
		{
			atomic_store_explicit(
				&connection->event_pending_next, head, memory_order_relaxed);
		} while (!atomic_compare_exchange_weak_explicit(
			&router->pending_head, &head, connection,
			memory_order_release, memory_order_acquire));
	}
	(void) postgamma_wake_target_wake(router->wake_target);
}


int
postgamma_public_event_waitable_fd(pgm_instance *instance)
{
	if (!instance_is_valid(instance) || instance->event_router == NULL)
		return -1;
	return postgamma_wake_target_fd(instance->event_router->wake_target);
}


void
postgamma_public_event_refresh_connection(pgm_connection *connection)
{
	PostgammaPublicEventRouter *router;
	pgm_request *request = NULL;
	bool		ready;
	uint64_t	wake_count;

	if (!connection_is_valid(connection) ||
		connection->instance->event_router == NULL)
		return;
	router = connection->instance->event_router;
	/*
	 * Retire the wake state that led to this progress attempt before testing
	 * the transport again.  A producer may publish another pending record at
	 * any point after the drain; the second collection below preserves that
	 * record independently of the readiness snapshot.
	 */
	(void) postgamma_wake_target_drain(router->wake_target, &wake_count);
	if (postgamma_mutex_lock(router->mutex) != 0)
		return;
	collect_pending_locked(router);
	remove_ready_locked(router, connection);
	(void) postgamma_mutex_unlock(router->mutex);
	if (postgamma_mutex_lock(connection->mutex) == 0)
	{
		request = connection->active_request;
		(void) postgamma_mutex_unlock(connection->mutex);
	}
	ready = request_progress_ready(request);
	if (postgamma_mutex_lock(router->mutex) != 0)
		return;
	collect_pending_locked(router);
	if (connection->event_registered && ready)
		enqueue_ready_locked(router, connection);
	preserve_level_wake_locked(router);
	(void) postgamma_mutex_unlock(router->mutex);
}


void
postgamma_public_event_enqueue_notice(
	pgm_connection *connection,
	const char *sqlstate, const char *severity, const char *message,
	const char *detail, const char *hint)
{
	pgm_event	event;
	pgm_request *request;
	bool		complete = true;

	if (!connection_is_valid(connection) ||
		connection->instance->event_router == NULL)
		return;
	memset(&event, 0, sizeof(event));
	event.magic = PGM_EVENT_MAGIC;
	event.kind = PGM_EVENT_NOTICE;
	event.connection_id = connection->identity;
	request = connection->active_request;
	if (request_is_valid(request))
	{
		event.request_id = request->generation;
		event.callback.notice = request->notice_callback != NULL ?
			request->notice_callback : connection->notice_callback;
		event.callback_user_data = request->notice_callback != NULL ?
			request->notice_user_data : connection->callback_user_data;
	}
	else
	{
		event.callback.notice = connection->notice_callback;
		event.callback_user_data = connection->callback_user_data;
	}
	complete = copy_event_text(
		event.payload.notice.sqlstate,
		sizeof(event.payload.notice.sqlstate), sqlstate) && complete;
	complete = copy_event_text(
		event.payload.notice.severity,
		sizeof(event.payload.notice.severity), severity) && complete;
	complete = copy_event_text(
		event.payload.notice.message,
		sizeof(event.payload.notice.message), message) && complete;
	complete = copy_event_text(
		event.payload.notice.detail,
		sizeof(event.payload.notice.detail), detail) && complete;
	complete = copy_event_text(
		event.payload.notice.hint,
		sizeof(event.payload.notice.hint), hint) && complete;
	if (complete)
		enqueue_event(connection->instance->event_router, &event);
	else
		record_drop(connection->instance->event_router, event.kind);
}


void
postgamma_public_event_enqueue_notification(
	pgm_connection *connection, int backend_pid,
	const char *channel, const char *payload)
{
	pgm_event	event;
	bool		complete = true;

	if (!connection_is_valid(connection) ||
		connection->instance->event_router == NULL)
		return;
	memset(&event, 0, sizeof(event));
	event.magic = PGM_EVENT_MAGIC;
	event.kind = PGM_EVENT_NOTIFICATION;
	event.connection_id = connection->identity;
	event.callback.notification = connection->notification_callback;
	event.callback_user_data = connection->callback_user_data;
	event.payload.notification.virtual_backend_pid = backend_pid;
	complete = copy_event_text(
		event.payload.notification.channel,
		sizeof(event.payload.notification.channel), channel) && complete;
	complete = copy_event_text(
		event.payload.notification.payload,
		sizeof(event.payload.notification.payload), payload) && complete;
	if (complete)
		enqueue_event(connection->instance->event_router, &event);
	else
		record_drop(connection->instance->event_router, event.kind);
}


int
postgamma_public_event_enqueue_kernel_log(
	void *argument, uint64_t generation,
	const PostgammaKernelLogRecord *record)
{
	pgm_instance *instance = argument;
	pgm_event	event;
	bool		complete = true;

	if (!instance_is_valid(instance) || instance->generation != generation ||
		instance->event_router == NULL || record == NULL ||
		record->struct_size != sizeof(*record))
		return EINVAL;
	memset(&event, 0, sizeof(event));
	event.magic = PGM_EVENT_MAGIC;
	event.kind = PGM_EVENT_LOG;
	event.connection_id = record->connection_id;
	event.request_id = record->request_id;
	event.callback.log = instance->event_router->log_callback;
	event.callback_user_data = instance->event_router->log_user_data;
	event.payload.log.virtual_backend_pid = record->virtual_backend_pid;
	complete = copy_event_text(
		event.payload.log.severity,
		sizeof(event.payload.log.severity), record->severity) && complete;
	complete = copy_event_text(
		event.payload.log.sqlstate,
		sizeof(event.payload.log.sqlstate), record->sqlstate) && complete;
	complete = copy_event_text(
		event.payload.log.message,
		sizeof(event.payload.log.message), record->message) && complete;
	complete = copy_event_text(
		event.payload.log.detail,
		sizeof(event.payload.log.detail), record->detail) && complete;
	if (complete)
		enqueue_event(instance->event_router, &event);
	else
		record_drop(instance->event_router, event.kind);
	return 0;
}


pgm_status
pgm_instance_waitable(
	pgm_instance *instance, int *file_descriptor, pgm_error **error)
{
	int			descriptor;

	if (error != NULL)
		*error = NULL;
	if (!instance_is_valid(instance) || file_descriptor == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid instance waitable arguments");
	if (!process_is_valid(instance->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"instance belongs to a different host process");
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	descriptor = postgamma_public_event_waitable_fd(instance);
	if (descriptor < 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"instance waitable is unavailable");
	*file_descriptor = descriptor;
	return PGM_STATUS_OK;
}


pgm_status
pgm_instance_next_event(
	pgm_instance *instance, int64_t timeout_ms, pgm_event **event,
	pgm_availability *availability, pgm_error **error)
{
	PostgammaPublicEventRouter *router;
	uint64_t	deadline_ns;
	int			status;

	if (error != NULL)
		*error = NULL;
	if (event != NULL)
		*event = NULL;
	if (!instance_is_valid(instance) || event == NULL || availability == NULL ||
		timeout_ms < PGM_NO_TIMEOUT)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid instance event arguments");
	if (!process_is_valid(instance->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"instance belongs to a different host process");
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = deadline_from_timeout(timeout_ms, &deadline_ns);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid instance event timeout");
	router = instance->event_router;
	for (;;)
	{
		bool		out_of_memory = false;
		uint64_t wake_count;

		pump_ready_connections(router, PGM_EVENT_PUMP_BUDGET);
		status = postgamma_mutex_lock(router->mutex);
		if (status != 0)
			return return_simple_error(
				error, PGM_STATUS_INTERNAL_ERROR,
				"could not lock the instance event queue");
		*event = take_event_locked(
			router, false, 0, 0, &out_of_memory);
		if (*event == NULL)
			*event = take_overflow_locked(router, &out_of_memory);
		preserve_level_wake_locked(router);
		(void) postgamma_mutex_unlock(router->mutex);
		(void) postgamma_wake_target_drain(router->wake_target, &wake_count);
		if (postgamma_mutex_lock(router->mutex) == 0)
		{
			preserve_level_wake_locked(router);
			(void) postgamma_mutex_unlock(router->mutex);
		}
		if (*event != NULL)
		{
			*availability = PGM_AVAILABILITY_READY;
			return PGM_STATUS_OK;
		}
		if (out_of_memory)
			return return_simple_error(
				error, PGM_STATUS_OUT_OF_MEMORY,
				"could not allocate an owned instance event");
		if (timeout_ms == 0)
		{
			*availability = PGM_AVAILABILITY_AGAIN;
			return PGM_STATUS_OK;
		}
		status = poll_waitable(
			postgamma_wake_target_fd(router->wake_target), deadline_ns);
		if (status == ETIMEDOUT)
		{
			*availability = PGM_AVAILABILITY_AGAIN;
			return PGM_STATUS_OK;
		}
		if (status != 0)
			return return_simple_error(
				error, PGM_STATUS_IO_ERROR,
				"could not wait for an instance event");
	}
}


pgm_status
pgm_instance_dispatch(
	pgm_instance *instance, size_t maximum_events,
	size_t *dispatched_events, pgm_error **error)
{
	PostgammaPublicEventRouter *router;
	bool		out_of_memory = false;

	if (error != NULL)
		*error = NULL;
	if (dispatched_events != NULL)
		*dispatched_events = 0;
	if (!instance_is_valid(instance) || dispatched_events == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid instance dispatch arguments");
	if (!process_is_valid(instance->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"instance belongs to a different host process");
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	router = instance->event_router;
	pump_ready_connections(router, PGM_EVENT_PUMP_BUDGET);
	*dispatched_events = dispatch_matching(
		router, maximum_events == 0 ? SIZE_MAX : maximum_events, 0, 0,
		&out_of_memory);
	if (out_of_memory)
		return return_simple_error(
			error, PGM_STATUS_OUT_OF_MEMORY,
			"could not allocate an event for callback dispatch");
	return PGM_STATUS_OK;
}


pgm_event_kind
pgm_event_type(const pgm_event *event)
{
	return event != NULL && event->magic == PGM_EVENT_MAGIC ?
		event->kind : INT32_C(-1);
}


pgm_connection_id
pgm_event_connection(const pgm_event *event)
{
	return event != NULL && event->magic == PGM_EVENT_MAGIC ?
		event->connection_id : UINT64_C(0);
}


pgm_request_id
pgm_event_request(const pgm_event *event)
{
	return event != NULL && event->magic == PGM_EVENT_MAGIC ?
		event->request_id : UINT64_C(0);
}


pgm_status
pgm_event_log(
	const pgm_event *event, pgm_log_record *record, pgm_error **error)
{
	if (error != NULL)
		*error = NULL;
	if (event == NULL || event->magic != PGM_EVENT_MAGIC ||
		event->kind != PGM_EVENT_LOG || record == NULL ||
		record->struct_size < sizeof(*record))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT, "event is not a log record");
	*record = (pgm_log_record) PGM_LOG_RECORD_INIT;
	record->virtual_backend_pid = event->payload.log.virtual_backend_pid;
	record->connection_id = event->connection_id;
	record->request_id = event->request_id;
	record->severity = event->payload.log.severity;
	record->sqlstate = event->payload.log.sqlstate;
	record->message = event->payload.log.message;
	record->detail = event->payload.log.detail;
	return PGM_STATUS_OK;
}


pgm_status
pgm_event_notice(
	const pgm_event *event, pgm_notice *notice, pgm_error **error)
{
	if (error != NULL)
		*error = NULL;
	if (event == NULL || event->magic != PGM_EVENT_MAGIC ||
		event->kind != PGM_EVENT_NOTICE || notice == NULL ||
		notice->struct_size < sizeof(*notice))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT, "event is not a notice");
	*notice = (pgm_notice) PGM_NOTICE_INIT;
	notice->sqlstate = event->payload.notice.sqlstate;
	notice->severity = event->payload.notice.severity;
	notice->message = event->payload.notice.message;
	notice->detail = event->payload.notice.detail;
	notice->hint = event->payload.notice.hint;
	notice->connection_id = event->connection_id;
	notice->request_id = event->request_id;
	return PGM_STATUS_OK;
}


pgm_status
pgm_event_notification(
	const pgm_event *event, pgm_notification *notification,
	pgm_error **error)
{
	if (error != NULL)
		*error = NULL;
	if (event == NULL || event->magic != PGM_EVENT_MAGIC ||
		event->kind != PGM_EVENT_NOTIFICATION || notification == NULL ||
		notification->struct_size < sizeof(*notification))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"event is not a notification");
	*notification = (pgm_notification) PGM_NOTIFICATION_INIT;
	notification->virtual_backend_pid =
		event->payload.notification.virtual_backend_pid;
	notification->channel = event->payload.notification.channel;
	notification->payload = event->payload.notification.payload;
	notification->connection_id = event->connection_id;
	return PGM_STATUS_OK;
}


pgm_status
pgm_event_overflow(
	const pgm_event *event, pgm_event_overflow_record *overflow,
	pgm_error **error)
{
	if (error != NULL)
		*error = NULL;
	if (event == NULL || event->magic != PGM_EVENT_MAGIC ||
		event->kind != PGM_EVENT_OVERFLOW || overflow == NULL ||
		overflow->struct_size < sizeof(*overflow))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"event is not an overflow record");
	*overflow = event->payload.overflow;
	return PGM_STATUS_OK;
}


void
pgm_event_free(pgm_event *event)
{
	if (event == NULL || event->magic != PGM_EVENT_MAGIC)
		return;
	event->magic = 0;
	free(event);
}


void
postgamma_public_dispatch_progress_events(pgm_request *request)
{
	if (!request_is_valid(request))
		return;
	postgamma_public_event_refresh_connection(request->connection);
	postgamma_public_dispatch_connection_events(
		request->connection, request->generation);
}


void
postgamma_public_dispatch_connection_events(
	pgm_connection *connection, pgm_request_id request_id)
{
	if (!connection_is_valid(connection) ||
		connection->instance->event_router == NULL ||
		public_instance_reentrant(connection->instance))
		return;
	(void) dispatch_matching(
		connection->instance->event_router, SIZE_MAX,
		connection->identity, request_id, NULL);
}


int
postgamma_public_event_telemetry(
	pgm_instance *instance,
	uint64_t *queue_depth, uint64_t *queue_capacity,
	uint64_t *dropped_logs, uint64_t *dropped_notices,
	uint64_t *dropped_notifications)
{
	PostgammaPublicEventRouter *router;
	int			status;

	if (!instance_is_valid(instance) || instance->event_router == NULL ||
		queue_depth == NULL || queue_capacity == NULL ||
		dropped_logs == NULL || dropped_notices == NULL ||
		dropped_notifications == NULL)
		return EINVAL;
	router = instance->event_router;
	status = postgamma_mutex_lock(router->mutex);
	if (status != 0)
		return status;
	*queue_depth = router->count;
	*queue_capacity = router->capacity;
	(void) postgamma_mutex_unlock(router->mutex);
	*dropped_logs = atomic_load_explicit(
		&router->dropped[0], memory_order_relaxed);
	*dropped_notices = atomic_load_explicit(
		&router->dropped[1], memory_order_relaxed);
	*dropped_notifications = atomic_load_explicit(
		&router->dropped[2], memory_order_relaxed);
	return 0;
}


static bool
copy_event_text(char *target, size_t capacity, const char *source)
{
	int			written;

	if (capacity == 0)
		return false;
	if (source == NULL)
		source = "";
	written = snprintf(target, capacity, "%s", source);
	return written >= 0 && (size_t) written < capacity;
}


static int
event_drop_index(pgm_event_kind kind)
{
	if (kind == PGM_EVENT_LOG)
		return 0;
	if (kind == PGM_EVENT_NOTICE)
		return 1;
	if (kind == PGM_EVENT_NOTIFICATION)
		return 2;
	return -1;
}


static void
record_drop(PostgammaPublicEventRouter *router, pgm_event_kind kind)
{
	int			index = event_drop_index(kind);

	if (index >= 0)
		(void) atomic_fetch_add_explicit(
			&router->dropped[index], UINT64_C(1), memory_order_relaxed);
	(void) postgamma_wake_target_wake(router->wake_target);
}


static void
enqueue_event(PostgammaPublicEventRouter *router, const pgm_event *event)
{
	size_t		tail;
	int			status;

	status = postgamma_mutex_try_lock(router->mutex);
	if (status != 0)
	{
		record_drop(router, event->kind);
		return;
	}
	if (router->count == router->capacity)
	{
		(void) postgamma_mutex_unlock(router->mutex);
		record_drop(router, event->kind);
		return;
	}
	tail = (router->head + router->count) % router->capacity;
	router->events[tail] = *event;
	router->count++;
	(void) postgamma_mutex_unlock(router->mutex);
	(void) postgamma_wake_target_wake(router->wake_target);
}


static void
preserve_level_wake_locked(PostgammaPublicEventRouter *router)
{
	if (router->count != 0 || router->ready_head != NULL ||
		overflow_pending_locked(router) || atomic_load_explicit(
			&router->pending_head, memory_order_acquire) != NULL)
		(void) postgamma_wake_target_wake(router->wake_target);
}


static bool
overflow_pending_locked(PostgammaPublicEventRouter *router)
{
	for (size_t index = 0; index < 3; index++)
	{
		if (atomic_load_explicit(
				&router->dropped[index], memory_order_relaxed) !=
			router->reported_dropped[index])
			return true;
	}
	return false;
}


static pgm_event *
take_event_locked(
	PostgammaPublicEventRouter *router, bool callbacks_only,
	pgm_connection_id connection_id, pgm_request_id request_id,
	bool *out_of_memory)
{
	size_t		offset;
	pgm_event *result;

	for (offset = 0; offset < router->count; offset++)
	{
		pgm_event *candidate =
			&router->events[(router->head + offset) % router->capacity];

		if (!callbacks_only ||
			event_matches_callback(candidate, connection_id, request_id))
			break;
	}
	if (offset == router->count)
		return NULL;
	result = malloc(sizeof(*result));
	if (result == NULL)
	{
		if (out_of_memory != NULL)
			*out_of_memory = true;
		return NULL;
	}
	*result = router->events[(router->head + offset) % router->capacity];
	for (size_t index = offset; index + 1 < router->count; index++)
		router->events[(router->head + index) % router->capacity] =
			router->events[(router->head + index + 1) % router->capacity];
	router->count--;
	if (router->count == 0)
		router->head = 0;
	return result;
}


static pgm_event *
take_overflow_locked(
	PostgammaPublicEventRouter *router, bool *out_of_memory)
{
	pgm_event *event;

	for (size_t index = 0; index < 3; index++)
	{
		uint64_t dropped = atomic_load_explicit(
			&router->dropped[index], memory_order_relaxed);

		if (dropped == router->reported_dropped[index])
			continue;
		event = calloc(1, sizeof(*event));
		if (event == NULL)
		{
			if (out_of_memory != NULL)
				*out_of_memory = true;
			return NULL;
		}
		event->magic = PGM_EVENT_MAGIC;
		event->kind = PGM_EVENT_OVERFLOW;
		event->payload.overflow = (pgm_event_overflow_record)
			PGM_EVENT_OVERFLOW_RECORD_INIT;
		event->payload.overflow.dropped_kind = (pgm_event_kind) index;
		event->payload.overflow.dropped_count =
			dropped - router->reported_dropped[index];
		router->reported_dropped[index] = dropped;
		return event;
	}
	return NULL;
}


static void
remove_ready_locked(
	PostgammaPublicEventRouter *router, pgm_connection *connection)
{
	pgm_connection *previous;
	pgm_connection *next;

	if (!connection->event_ready_queued)
		return;
	previous = connection->event_ready_previous;
	next = connection->event_ready_next;
	if (previous == NULL)
		router->ready_head = next;
	else
		previous->event_ready_next = next;
	if (next == NULL)
		router->ready_tail = previous;
	else
		next->event_ready_previous = previous;
	connection->event_ready_previous = NULL;
	connection->event_ready_next = NULL;
	connection->event_ready_queued = false;
}


static void
enqueue_ready_locked(
	PostgammaPublicEventRouter *router, pgm_connection *connection)
{
	if (!connection->event_registered || connection->event_ready_queued)
		return;
	connection->event_ready_previous = router->ready_tail;
	connection->event_ready_next = NULL;
	if (router->ready_tail == NULL)
		router->ready_head = connection;
	else
		router->ready_tail->event_ready_next = connection;
	router->ready_tail = connection;
	connection->event_ready_queued = true;
}


static void
collect_pending_locked(PostgammaPublicEventRouter *router)
{
	pgm_connection *connection = atomic_exchange_explicit(
		&router->pending_head, NULL, memory_order_acq_rel);

	while (connection != NULL)
	{
		pgm_connection *next = atomic_load_explicit(
			&connection->event_pending_next, memory_order_acquire);

		atomic_store_explicit(
			&connection->event_pending_next, NULL, memory_order_relaxed);
		atomic_store_explicit(
			&connection->event_ready_pending, false, memory_order_release);
		if (connection->event_registered)
			enqueue_ready_locked(router, connection);
		connection = next;
	}
}


static bool
request_progress_ready(pgm_request *request)
{
	bool		ready = false;

	if (!request_is_valid(request) || request->mutex == NULL)
		return false;
	if (postgamma_mutex_lock(request->mutex) != 0)
		return false;
	ready = request->result != NULL ||
		(request->state != PGM_REQUEST_PENDING &&
		 request->state != PGM_REQUEST_RUNNING);
	if (!ready && request->operation != NULL)
		(void) postgamma_private_libpq_operation_ready(
			request->operation, &ready);
	(void) postgamma_mutex_unlock(request->mutex);
	return ready;
}


static pgm_connection *
take_ready_connection(PostgammaPublicEventRouter *router)
{
	pgm_connection *connection;

	if (postgamma_mutex_lock(router->mutex) != 0)
		return NULL;
	collect_pending_locked(router);
	connection = router->ready_head;
	if (connection != NULL)
	{
		remove_ready_locked(router, connection);
		connection->event_pump_references++;
	}
	(void) postgamma_mutex_unlock(router->mutex);
	return connection;
}


static void
finish_ready_connection(
	PostgammaPublicEventRouter *router, pgm_connection *connection)
{
	if (postgamma_mutex_lock(router->mutex) != 0)
		return;
	collect_pending_locked(router);
	if (connection->event_pump_references != 0)
		connection->event_pump_references--;
	preserve_level_wake_locked(router);
	(void) postgamma_mutex_unlock(router->mutex);
}


static void
pump_ready_connections(
	PostgammaPublicEventRouter *router, size_t budget)
{
	for (size_t index = 0; index < budget; index++)
	{
		pgm_connection *connection = take_ready_connection(router);
		pgm_request *request = NULL;
		bool		idle = false;
		bool		request_ready = false;

		if (connection == NULL)
			break;
		if (postgamma_mutex_try_lock(connection->mutex) == 0)
		{
			idle = !connection->closing &&
				connection->active_request == NULL &&
				connection->private_connection != NULL;
			request = connection->active_request;
			(void) postgamma_mutex_unlock(connection->mutex);
		}
		if (idle)
			(void) postgamma_private_libpq_consume_idle(
				connection->private_connection);
		else
			request_ready = request_progress_ready(request);
		if (request_ready)
			postgamma_public_event_notify_connection(
				connection, POSTGAMMA_MEMORY_WAIT_READABLE);
		finish_ready_connection(router, connection);
	}
}


static void
invoke_event_callback(pgm_instance *instance, const pgm_event *event)
{
	PostgammaPublicCallbackFrame frame = {instance, PostgammaCallbackFrame};

	PostgammaCallbackFrame = &frame;
	if (event->kind == PGM_EVENT_LOG && event->callback.log != NULL)
	{
		pgm_log_record record = PGM_LOG_RECORD_INIT;

		(void) pgm_event_log(event, &record, NULL);
		event->callback.log(event->callback_user_data, &record);
	}
	else if (event->kind == PGM_EVENT_NOTICE && event->callback.notice != NULL)
	{
		pgm_notice notice = PGM_NOTICE_INIT;

		(void) pgm_event_notice(event, &notice, NULL);
		event->callback.notice(event->callback_user_data, &notice);
	}
	else if (event->kind == PGM_EVENT_NOTIFICATION &&
		event->callback.notification != NULL)
	{
		pgm_notification notification = PGM_NOTIFICATION_INIT;

		(void) pgm_event_notification(event, &notification, NULL);
		event->callback.notification(
			event->callback_user_data, &notification);
	}
	PostgammaCallbackFrame = frame.previous;
}


static size_t
dispatch_matching(
	PostgammaPublicEventRouter *router, size_t maximum_events,
	pgm_connection_id connection_id, pgm_request_id request_id,
	bool *out_of_memory)
{
	size_t		dispatched = 0;

	while (dispatched < maximum_events)
	{
		pgm_event *event;
		bool		consume_all = connection_id == 0;

		if (postgamma_mutex_lock(router->mutex) != 0)
			break;
		event = take_event_locked(
			router, !consume_all, connection_id, request_id, out_of_memory);
		if (event == NULL && consume_all)
			event = take_overflow_locked(router, out_of_memory);
		preserve_level_wake_locked(router);
		(void) postgamma_mutex_unlock(router->mutex);
		if (event == NULL)
			break;
		invoke_event_callback(router->instance, event);
		pgm_event_free(event);
		dispatched++;
	}
	return dispatched;
}


static bool
event_matches_callback(
	const pgm_event *event, pgm_connection_id connection_id,
	pgm_request_id request_id)
{
	bool		has_callback =
		(event->kind == PGM_EVENT_LOG && event->callback.log != NULL) ||
		(event->kind == PGM_EVENT_NOTICE && event->callback.notice != NULL) ||
		(event->kind == PGM_EVENT_NOTIFICATION &&
		 event->callback.notification != NULL);

	if (!has_callback)
		return false;
	if (connection_id == 0)
		return true;
	if (event->connection_id != connection_id)
		return false;
	return event->request_id == 0 || event->request_id == request_id;
}


static int
poll_waitable(int descriptor, uint64_t deadline_ns)
{
	struct pollfd descriptor_state = {descriptor, POLLIN, 0};

	for (;;)
	{
		int timeout = -1;
		int result;

		if (deadline_ns != POSTGAMMA_SUPERVISOR_NO_DEADLINE)
		{
			uint64_t now = postgamma_monotonic_now_ns();
			uint64_t remaining;

			if (now >= deadline_ns)
				return ETIMEDOUT;
			remaining = deadline_ns - now;
			if (remaining / UINT64_C(1000000) > (uint64_t) INT_MAX)
				timeout = INT_MAX;
			else
			{
				timeout = (int) (remaining / UINT64_C(1000000));
				if (remaining % UINT64_C(1000000) != 0)
					timeout++;
			}
		}
		result = poll(&descriptor_state, 1, timeout);
		if (result > 0)
			return 0;
		if (result == 0)
			return ETIMEDOUT;
		if (errno != EINTR)
			return errno != 0 ? errno : EIO;
	}
}
