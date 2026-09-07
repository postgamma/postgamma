/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "postgres_fe.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/protocol.h"
#include "port/pg_bswap.h"
#include "postgamma/private/libpq_memory_adapter.h"


#define POSTGAMMA_PRIVATE_LIBPQ_MAGIC UINT64_C(0x50474C494250514D)
#define POSTGAMMA_PRIVATE_LIBPQ_OPERATION_MAGIC UINT64_C(0x50474C514F504552)

_Static_assert(
	(int) POSTGAMMA_PRIVATE_TRANSACTION_IDLE == (int) PQTRANS_IDLE,
	"private and PostgreSQL idle transaction states must match");
_Static_assert(
	(int) POSTGAMMA_PRIVATE_TRANSACTION_ACTIVE == (int) PQTRANS_ACTIVE,
	"private and PostgreSQL active transaction states must match");
_Static_assert(
	(int) POSTGAMMA_PRIVATE_TRANSACTION_INTRANS == (int) PQTRANS_INTRANS,
	"private and PostgreSQL in-transaction states must match");
_Static_assert(
	(int) POSTGAMMA_PRIVATE_TRANSACTION_INERROR == (int) PQTRANS_INERROR,
	"private and PostgreSQL failed-transaction states must match");
_Static_assert(
	(int) POSTGAMMA_PRIVATE_TRANSACTION_UNKNOWN == (int) PQTRANS_UNKNOWN,
	"private and PostgreSQL unknown transaction states must match");

typedef enum PostgammaPrivateLibpqOperationPhase
{
	POSTGAMMA_PRIVATE_LIBPQ_OPERATION_ACTIVE = 0,
	POSTGAMMA_PRIVATE_LIBPQ_OPERATION_COMPLETE
} PostgammaPrivateLibpqOperationPhase;

typedef enum PostgammaPrivateLibpqCopyPhase
{
	POSTGAMMA_PRIVATE_LIBPQ_COPY_NONE = 0,
	POSTGAMMA_PRIVATE_LIBPQ_COPY_ACTIVE,
	POSTGAMMA_PRIVATE_LIBPQ_COPY_ENDED
} PostgammaPrivateLibpqCopyPhase;

typedef enum PostgammaPrivateSubmissionKind
{
	POSTGAMMA_PRIVATE_SUBMISSION_QUERY = 0,
	POSTGAMMA_PRIVATE_SUBMISSION_SCRIPT,
	POSTGAMMA_PRIVATE_SUBMISSION_PREPARED
} PostgammaPrivateSubmissionKind;

struct PostgammaPrivateLibpqOperation
{
	uint64_t	magic;
	PostgammaPrivateLibpqConnection *connection;
	uint64_t	request_generation;
	PostgammaPrivateLibpqOperationPhase phase;
	PostgammaPrivateLibpqStatus terminal_status;
	PostgammaPrivateResultPolicy result_policy;
	uint32_t	wait_events;
	PostgammaPrivateCopyDirection copy_direction;
	PostgammaPrivateLibpqCopyPhase copy_phase;
	size_t		copy_out_remaining;
	bool		result_policy_active;
	bool		copy_finish_sent;
	bool		copy_remote_terminated;
};

struct PostgammaPrivateLibpqConnection
{
	uint64_t	magic;
	uint64_t	generation;
	size_t		queue_capacity;
	pthread_mutex_t mutex;
	PostgammaMemoryEndpoint *endpoint;
	PGconn     *postgres_connection;
	PostgammaPrivateLibpqOperation *active_operation;
	bool		connect_operation;
	int64_t		operation_deadline_ns;
	PostgammaPrivateCancelCallback cancel_callback;
	void       *cancel_argument;
	PostgammaPrivateNoticeCallback notice_callback;
	void       *notice_argument;
	PostgammaPrivateNotificationCallback notification_callback;
	void       *notification_argument;
	uint64_t	secure_read_calls;
	uint64_t	secure_write_calls;
	uint64_t	socket_wait_calls;
	uint64_t	bytes_read;
	uint64_t	bytes_written;
	uint64_t	notice_count;
	_Atomic uint64_t cancel_dispatches;
	_Atomic int backend_pid;
	_Atomic uint64_t active_request_generation;
	char		last_notice_sqlstate[6];
	char		last_notice_message[128];
};

static _Thread_local PostgammaPrivateLibpqConnection *
	PostgammaCurrentPrivateLibpqConnection;

static PostgammaPrivateLibpqStatus postgamma_private_operation_start_internal(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	PostgammaPrivateSubmissionKind submission_kind,
	const char *query_or_statement,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	const PostgammaPrivateResultPolicy *result_policy,
	PostgammaPrivateLibpqOperation **operation);


static bool
postgamma_private_libpq_valid(
	const PostgammaPrivateLibpqConnection *connection)
{
	return connection != NULL &&
		connection->magic == POSTGAMMA_PRIVATE_LIBPQ_MAGIC;
}


static bool
postgamma_private_operation_valid(
	const PostgammaPrivateLibpqOperation *operation)
{
	return operation != NULL &&
		operation->magic == POSTGAMMA_PRIVATE_LIBPQ_OPERATION_MAGIC &&
		postgamma_private_libpq_valid(operation->connection);
}


static bool
postgamma_private_result_policy_valid(
	const PostgammaPrivateResultPolicy *policy)
{
	return policy != NULL &&
		(policy->delivery_mode == POSTGAMMA_MEMORY_DELIVERY_MATERIALIZED ||
		 policy->delivery_mode == POSTGAMMA_MEMORY_DELIVERY_CHUNKED) &&
		policy->target_chunk_rows <= (uint32_t) INT_MAX &&
		policy->result_buffer_limit != 0 && policy->maximum_value_size != 0 &&
		policy->maximum_value_size <= policy->result_buffer_limit &&
		((policy->delivery_mode == POSTGAMMA_MEMORY_DELIVERY_MATERIALIZED &&
		  policy->target_chunk_rows == 0) ||
		 (policy->delivery_mode == POSTGAMMA_MEMORY_DELIVERY_CHUNKED &&
		  policy->target_chunk_rows != 0));
}


static PostgammaPrivateLibpqStatus
postgamma_private_memory_status(PostgammaMemoryStatus status)
{
	switch (status)
	{
		case POSTGAMMA_MEMORY_STATUS_OK:
			return POSTGAMMA_PRIVATE_LIBPQ_OK;
		case POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT:
			return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
		case POSTGAMMA_MEMORY_STATUS_NO_MEMORY:
			return POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;
		case POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION:
			return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
		default:
			return POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
	}
}


static PostgammaPrivateLibpqStatus
postgamma_private_clear_result_policy(
	PostgammaPrivateLibpqConnection *connection)
{
	PostgammaMemoryResultPolicy policy = POSTGAMMA_MEMORY_RESULT_POLICY_INIT;

	return postgamma_private_memory_status(
		postgamma_memory_endpoint_set_result_policy(
			connection->endpoint, connection->generation, &policy));
}


static void
postgamma_private_copy(char *destination, size_t capacity, const char *source)
{
	if (capacity == 0)
		return;
	if (source == NULL)
		source = "";
	(void) snprintf(destination, capacity, "%s", source);
}


static PostgammaPrivateLibpqStatus
postgamma_private_bind(PostgammaPrivateLibpqConnection *connection)
{
	if (!postgamma_private_libpq_valid(connection) ||
		PostgammaCurrentPrivateLibpqConnection != NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	PostgammaCurrentPrivateLibpqConnection = connection;
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}


static void
postgamma_private_restore(PostgammaPrivateLibpqConnection *connection)
{
	if (PostgammaCurrentPrivateLibpqConnection == connection)
		PostgammaCurrentPrivateLibpqConnection = NULL;
}


bool
postgamma_libpq_memory_is_bound(PGconn *connection)
{
	PostgammaPrivateLibpqConnection *current =
		PostgammaCurrentPrivateLibpqConnection;

	return postgamma_private_libpq_valid(current) &&
		current->postgres_connection == connection;
}


bool
postgamma_libpq_memory_connect_start(PGconn *connection)
{
	PostgammaPrivateLibpqConnection *current =
		PostgammaCurrentPrivateLibpqConnection;

	if (!postgamma_private_libpq_valid(current) || connection == NULL ||
		current->postgres_connection != NULL)
		return false;
	current->postgres_connection = connection;
	connection->inStart = connection->inCursor = connection->inEnd = 0;
	connection->outCount = 0;
	connection->whichhost = 0;
	connection->try_next_host = false;
	connection->try_next_addr = false;
	connection->pversion = connection->max_pversion;
	connection->send_appname = true;
	connection->failed_enc_methods = 0;
	connection->allowed_enc_methods = ENC_PLAINTEXT;
	connection->current_enc_method = ENC_PLAINTEXT;
	connection->nonblocking = true;
	connection->status = CONNECTION_MADE;
	return true;
}


static void
postgamma_private_transport_error(PGconn *connection, const char *operation,
								 PostgammaMemoryStatus status)
{
	libpq_append_conn_error(
		connection, "embedded memory transport %s failed: %s",
		operation, postgamma_memory_status_name(status));
}


bool
postgamma_libpq_memory_secure_read(PGconn *connection, void *buffer,
								  size_t length, ssize_t *result)
{
	PostgammaPrivateLibpqConnection *current =
		PostgammaCurrentPrivateLibpqConnection;
	PostgammaMemoryIoResult io;

	if (!postgamma_libpq_memory_is_bound(connection))
		return false;
	current->secure_read_calls++;
	io = postgamma_memory_endpoint_read(
		current->endpoint, current->generation, buffer, length);
	switch (io.status)
	{
		case POSTGAMMA_MEMORY_STATUS_PROGRESS:
			current->bytes_read += io.bytes;
			*result = (ssize_t) io.bytes;
			SOCK_ERRNO_SET(0);
			break;
		case POSTGAMMA_MEMORY_STATUS_RETRY:
			*result = -1;
			SOCK_ERRNO_SET(EAGAIN);
			break;
		case POSTGAMMA_MEMORY_STATUS_EOF:
			*result = 0;
			SOCK_ERRNO_SET(0);
			break;
		default:
			postgamma_private_transport_error(connection, "read", io.status);
			*result = -1;
			SOCK_ERRNO_SET(ECONNRESET);
			break;
	}
	return true;
}


bool
postgamma_libpq_memory_secure_write(PGconn *connection, const void *buffer,
								   size_t length, ssize_t *result)
{
	PostgammaPrivateLibpqConnection *current =
		PostgammaCurrentPrivateLibpqConnection;
	PostgammaMemoryIoResult io;

	if (!postgamma_libpq_memory_is_bound(connection))
		return false;
	current->secure_write_calls++;
	if (current->connect_operation)
	{
		const unsigned char *cursor = buffer;
		size_t		remaining = length;

		while (remaining != 0)
		{
			PostgammaMemoryWaitResult wait_result;

			io = postgamma_memory_endpoint_write(
				current->endpoint, current->generation, cursor, remaining);
			if (io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
			{
				current->bytes_written += io.bytes;
				cursor += io.bytes;
				remaining -= io.bytes;
				continue;
			}
			if (io.status != POSTGAMMA_MEMORY_STATUS_RETRY)
			{
				postgamma_private_transport_error(
					connection, "connect write", io.status);
				*result = -1;
				SOCK_ERRNO_SET(EPIPE);
				return true;
			}
			wait_result = postgamma_memory_endpoint_wait(
				current->endpoint, current->generation,
				POSTGAMMA_MEMORY_WAIT_WRITABLE |
					POSTGAMMA_MEMORY_WAIT_PEER_CLOSED,
				0, current->operation_deadline_ns);
			if (wait_result.status != POSTGAMMA_MEMORY_STATUS_OK ||
				(wait_result.events & POSTGAMMA_MEMORY_WAIT_WRITABLE) == 0)
			{
				postgamma_private_transport_error(
					connection, "connect write wait", wait_result.status);
				*result = -1;
				SOCK_ERRNO_SET(
					wait_result.status == POSTGAMMA_MEMORY_STATUS_TIMEOUT ?
					ETIMEDOUT : ECONNRESET);
				return true;
			}
		}
		*result = (ssize_t) length;
		SOCK_ERRNO_SET(0);
		return true;
	}
	io = postgamma_memory_endpoint_write(
		current->endpoint, current->generation, buffer, length);
	switch (io.status)
	{
		case POSTGAMMA_MEMORY_STATUS_PROGRESS:
			current->bytes_written += io.bytes;
			*result = (ssize_t) io.bytes;
			SOCK_ERRNO_SET(0);
			break;
		case POSTGAMMA_MEMORY_STATUS_RETRY:
			*result = -1;
			SOCK_ERRNO_SET(EAGAIN);
			break;
		default:
			postgamma_private_transport_error(connection, "write", io.status);
			*result = -1;
			SOCK_ERRNO_SET(EPIPE);
			break;
	}
	return true;
}


static PostgammaMemoryStatus
postgamma_private_deadline(int64_t end_time, int64_t *deadline)
{
	int64_t		now;
	struct timeval wall;
	int64_t		wall_microseconds;
	int64_t		remaining;
	PostgammaMemoryStatus status;

	if (end_time < 0)
	{
		*deadline = POSTGAMMA_MEMORY_NO_DEADLINE;
		return POSTGAMMA_MEMORY_STATUS_OK;
	}
	status = postgamma_memory_clock_now(&now);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	if (end_time == 0)
	{
		*deadline = now;
		return POSTGAMMA_MEMORY_STATUS_OK;
	}
	if (gettimeofday(&wall, NULL) != 0 || wall.tv_sec < 0 ||
		(uint64_t) wall.tv_sec > UINT64_C(9223372036854))
		return POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
	wall_microseconds = (int64_t) wall.tv_sec * INT64_C(1000000) +
		wall.tv_usec;
	remaining = end_time > wall_microseconds ?
		end_time - wall_microseconds : 0;
	if (remaining > (INT64_MAX - now) / INT64_C(1000))
		return POSTGAMMA_MEMORY_STATUS_OVERFLOW;
	*deadline = now + remaining * INT64_C(1000);
	return POSTGAMMA_MEMORY_STATUS_OK;
}


bool
postgamma_libpq_memory_socket_check(PGconn *connection, int for_read,
								   int for_write, int64_t end_time,
								   int *result)
{
	PostgammaPrivateLibpqConnection *current =
		PostgammaCurrentPrivateLibpqConnection;
	PostgammaMemoryWaitResult wait_result;
	PostgammaMemoryStatus status;
	uint32_t	events = POSTGAMMA_MEMORY_WAIT_PEER_CLOSED;
	int64_t		deadline;

	if (!postgamma_libpq_memory_is_bound(connection))
		return false;
	current->socket_wait_calls++;
	if (!for_read && !for_write)
	{
		*result = 0;
		return true;
	}
	if (for_read)
		events |= POSTGAMMA_MEMORY_WAIT_READABLE;
	if (for_write)
		events |= POSTGAMMA_MEMORY_WAIT_WRITABLE;
	status = postgamma_private_deadline(end_time, &deadline);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
	{
		postgamma_private_transport_error(connection, "deadline", status);
		*result = -1;
		return true;
	}
	wait_result = postgamma_memory_endpoint_wait(
		current->endpoint, current->generation, events, 0, deadline);
	switch (wait_result.status)
	{
		case POSTGAMMA_MEMORY_STATUS_OK:
		case POSTGAMMA_MEMORY_STATUS_EOF:
		case POSTGAMMA_MEMORY_STATUS_PEER_CLOSED:
			*result = 1;
			break;
		case POSTGAMMA_MEMORY_STATUS_TIMEOUT:
			*result = 0;
			break;
		case POSTGAMMA_MEMORY_STATUS_CANCELLED:
			SOCK_ERRNO_SET(EINTR);
			*result = -1;
			break;
		default:
			postgamma_private_transport_error(
				connection, "wait", wait_result.status);
			SOCK_ERRNO_SET(ECONNRESET);
			*result = -1;
			break;
	}
	return true;
}


static void
postgamma_private_notice_receiver(void *argument, const PGresult *result)
{
	PostgammaPrivateLibpqConnection *connection = argument;
	const char *sqlstate;
	const char *message;

	if (!postgamma_private_libpq_valid(connection))
		return;
	sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
	message = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);
	connection->notice_count++;
	postgamma_private_copy(
		connection->last_notice_sqlstate,
		sizeof(connection->last_notice_sqlstate), sqlstate);
	postgamma_private_copy(
		connection->last_notice_message,
		sizeof(connection->last_notice_message), message);
	if (connection->notice_callback != NULL)
		connection->notice_callback(
			connection->notice_argument, sqlstate,
			PQresultErrorField(result, PG_DIAG_SEVERITY_NONLOCALIZED),
			message, PQresultErrorField(result, PG_DIAG_MESSAGE_DETAIL),
			PQresultErrorField(result, PG_DIAG_MESSAGE_HINT));
}


static void
postgamma_private_dispatch_notifications(
	PostgammaPrivateLibpqConnection *connection)
{
	PGnotify   *notification;

	if (connection->notification_callback == NULL)
		return;
	while ((notification = PQnotifies(connection->postgres_connection)) != NULL)
	{
		connection->notification_callback(
			connection->notification_argument, notification->be_pid,
			notification->relname, notification->extra);
		PQfreemem(notification);
	}
}


static PostgammaPrivateLibpqStatus
postgamma_private_wait(PostgammaPrivateLibpqConnection *connection,
					   bool for_read, bool for_write, int64_t deadline_ns)
{
	int64_t		now;
	struct timeval wall;
	int64_t		end_time;
	int			status;

	if (deadline_ns == POSTGAMMA_MEMORY_NO_DEADLINE)
		end_time = -1;
	else
	{
		int64_t		wall_microseconds;
		int64_t		remaining_microseconds;

		if (postgamma_memory_clock_now(&now) != POSTGAMMA_MEMORY_STATUS_OK ||
			gettimeofday(&wall, NULL) != 0 || wall.tv_sec < 0 ||
			(uint64_t) wall.tv_sec > UINT64_C(9223372036854))
			return POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
		if (now >= deadline_ns)
			return POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT;
		wall_microseconds = (int64_t) wall.tv_sec * INT64_C(1000000) +
			wall.tv_usec;
		remaining_microseconds = (deadline_ns - now) / INT64_C(1000);
		if (remaining_microseconds > INT64_MAX - wall_microseconds)
			return POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
		end_time = wall_microseconds + remaining_microseconds;
	}
	status = pqWaitTimed(for_read ? 1 : 0, for_write ? 1 : 0,
		connection->postgres_connection, end_time);
	if (status == 0)
		return POSTGAMMA_PRIVATE_LIBPQ_OK;
	return status == 1 ? POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT :
		POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
}


static PostgammaPrivateResultStatus
postgamma_private_result_status(ExecStatusType status)
{
	switch (status)
	{
		case PGRES_EMPTY_QUERY:
			return POSTGAMMA_PRIVATE_RESULT_EMPTY_QUERY;
		case PGRES_COMMAND_OK:
			return POSTGAMMA_PRIVATE_RESULT_COMMAND_OK;
		case PGRES_TUPLES_OK:
			return POSTGAMMA_PRIVATE_RESULT_TUPLES_OK;
		case PGRES_COPY_OUT:
			return POSTGAMMA_PRIVATE_RESULT_COPY_OUT;
		case PGRES_COPY_IN:
			return POSTGAMMA_PRIVATE_RESULT_COPY_IN;
		case PGRES_COPY_BOTH:
			return POSTGAMMA_PRIVATE_RESULT_COPY_BOTH;
		case PGRES_BAD_RESPONSE:
			return POSTGAMMA_PRIVATE_RESULT_BAD_RESPONSE;
		case PGRES_NONFATAL_ERROR:
			return POSTGAMMA_PRIVATE_RESULT_NONFATAL_ERROR;
		case PGRES_FATAL_ERROR:
			return POSTGAMMA_PRIVATE_RESULT_FATAL_ERROR;
		case PGRES_SINGLE_TUPLE:
			return POSTGAMMA_PRIVATE_RESULT_SINGLE_TUPLE;
		case PGRES_TUPLES_CHUNK:
			return POSTGAMMA_PRIVATE_RESULT_TUPLES_CHUNK;
		case PGRES_PIPELINE_SYNC:
			return POSTGAMMA_PRIVATE_RESULT_PIPELINE_SYNC;
		case PGRES_PIPELINE_ABORTED:
			return POSTGAMMA_PRIVATE_RESULT_PIPELINE_ABORTED;
	}
	return POSTGAMMA_PRIVATE_RESULT_NONE;
}


static void
postgamma_private_capture_result(PGconn *connection, PGresult *source,
								PostgammaPrivateQueryResult *target)
{
	const char *value;

	memset(target, 0, sizeof(*target));
	target->status = postgamma_private_result_status(PQresultStatus(source));
	target->rows = PQntuples(source);
	target->columns = PQnfields(source);
	if (target->rows > 0 && target->columns > 0 && !PQgetisnull(source, 0, 0))
		postgamma_private_copy(
			target->value, sizeof(target->value), PQgetvalue(source, 0, 0));
	postgamma_private_copy(
		target->command_status, sizeof(target->command_status),
		PQcmdStatus(source));
	value = PQresultErrorField(source, PG_DIAG_SQLSTATE);
	postgamma_private_copy(target->sqlstate, sizeof(target->sqlstate), value);
	value = PQresultErrorField(source, PG_DIAG_SEVERITY_NONLOCALIZED);
	postgamma_private_copy(target->severity, sizeof(target->severity), value);
	value = PQresultErrorField(source, PG_DIAG_MESSAGE_PRIMARY);
	postgamma_private_copy(target->message, sizeof(target->message), value);
	value = PQresultErrorField(source, PG_DIAG_MESSAGE_DETAIL);
	postgamma_private_copy(target->detail, sizeof(target->detail), value);
	target->transaction_status = (char) PQtransactionStatus(connection);
}


void
postgamma_private_libpq_result_free(PostgammaPrivateOwnedResult *result)
{
	if (result == NULL)
		return;
	if (result->fields != NULL)
	{
		for (size_t column = 0; column < result->columns; column++)
			free(result->fields[column].name);
	}
	if (result->values != NULL && result->columns != 0)
	{
		for (size_t row = 0; row < result->rows; row++)
		{
			for (size_t column = 0; column < result->columns; column++)
				free(result->values[row * result->columns + column].data);
		}
	}
	free(result->values);
	free(result->fields);
	for (size_t field = 0; field < 17; field++)
		free(result->diagnostics[field]);
	free(result);
}


static char *
postgamma_private_duplicate(const char *source)
{
	size_t		length;
	char	   *copy;

	if (source == NULL)
		source = "";
	length = strlen(source);
	if (length == SIZE_MAX)
		return NULL;
	copy = malloc(length + 1);
	if (copy != NULL)
		memcpy(copy, source, length + 1);
	return copy;
}


static PostgammaPrivateLibpqStatus
postgamma_private_check_owned_result_budget(
	PGresult *source, const PostgammaPrivateResultPolicy *policy)
{
	ExecStatusType result_status;
	int			row_count;
	int			column_count;
	size_t		result_bytes = 0;

	if (policy == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_OK;
	if (!postgamma_private_result_policy_valid(policy))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	result_status = PQresultStatus(source);
	row_count = PQntuples(source);
	column_count = PQnfields(source);
	if (row_count < 0 || column_count < 0)
		return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
	if (policy->delivery_mode == POSTGAMMA_MEMORY_DELIVERY_CHUNKED &&
		result_status == PGRES_TUPLES_CHUNK &&
		(uint64_t) row_count > (uint64_t) policy->target_chunk_rows)
		return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
	for (int row = 0; row < row_count; row++)
	{
		size_t		row_bytes = 5 + sizeof(uint16_t);

		if ((size_t) column_count >
			(SIZE_MAX - row_bytes) / sizeof(uint32_t))
			return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
		row_bytes += (size_t) column_count * sizeof(uint32_t);
		for (int column = 0; column < column_count; column++)
		{
			int			length;

			if (PQgetisnull(source, row, column))
				continue;
			length = PQgetlength(source, row, column);
			if (length < 0 ||
				(size_t) length > policy->maximum_value_size ||
				(size_t) length > SIZE_MAX - row_bytes)
				return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
			row_bytes += (size_t) length;
		}
		if (row_bytes > policy->result_buffer_limit ||
			result_bytes > policy->result_buffer_limit - row_bytes)
			return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
		result_bytes += row_bytes;
	}
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}


static PostgammaPrivateLibpqStatus
postgamma_private_capture_owned_result(
	PGconn *connection, PGresult *source,
	const PostgammaPrivateResultPolicy *result_policy,
	PostgammaPrivateOwnedResult **target)
{
	PostgammaPrivateOwnedResult *created;
	PostgammaPrivateLibpqStatus budget_status;
	int			row_count;
	int			column_count;
	size_t		value_count;
	const char *value;
	static const int diagnostic_codes[17] =
	{
		PG_DIAG_SQLSTATE,
		PG_DIAG_SEVERITY_NONLOCALIZED,
		PG_DIAG_MESSAGE_PRIMARY,
		PG_DIAG_MESSAGE_DETAIL,
		PG_DIAG_MESSAGE_HINT,
		PG_DIAG_STATEMENT_POSITION,
		PG_DIAG_INTERNAL_POSITION,
		PG_DIAG_INTERNAL_QUERY,
		PG_DIAG_CONTEXT,
		PG_DIAG_SCHEMA_NAME,
		PG_DIAG_TABLE_NAME,
		PG_DIAG_COLUMN_NAME,
		PG_DIAG_DATATYPE_NAME,
		PG_DIAG_CONSTRAINT_NAME,
		PG_DIAG_SOURCE_FILE,
		PG_DIAG_SOURCE_LINE,
		PG_DIAG_SOURCE_FUNCTION,
	};

	if (connection == NULL || source == NULL || target == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*target = NULL;
	budget_status = postgamma_private_check_owned_result_budget(
		source, result_policy);
	if (budget_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		return budget_status;
	row_count = PQntuples(source);
	column_count = PQnfields(source);
	if (row_count < 0 || column_count < 0 ||
		(column_count != 0 &&
		 (size_t) row_count > SIZE_MAX / (size_t) column_count))
		return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
	value_count = (size_t) row_count * (size_t) column_count;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;
	created->status = postgamma_private_result_status(PQresultStatus(source));
	created->rows = (size_t) row_count;
	created->columns = (size_t) column_count;
	if (created->columns != 0)
	{
		created->fields = calloc(created->columns, sizeof(*created->fields));
		if (created->fields == NULL)
			goto no_memory;
	}
	if (value_count != 0)
	{
		created->values = calloc(value_count, sizeof(*created->values));
		if (created->values == NULL)
			goto no_memory;
	}
	for (size_t column = 0; column < created->columns; column++)
	{
		PostgammaPrivateResultField *field = &created->fields[column];

		field->name = postgamma_private_duplicate(
			PQfname(source, (int) column));
		if (field->name == NULL)
			goto no_memory;
		field->table_oid = (uint32_t) PQftable(source, (int) column);
		field->table_column = (int32_t) PQftablecol(source, (int) column);
		field->type_oid = (uint32_t) PQftype(source, (int) column);
		field->type_size = (int32_t) PQfsize(source, (int) column);
		field->type_modifier = (int32_t) PQfmod(source, (int) column);
		field->format = (uint16_t) PQfformat(source, (int) column);
	}
	for (size_t row = 0; row < created->rows; row++)
	{
		for (size_t column = 0; column < created->columns; column++)
		{
			PostgammaPrivateResultValue *cell =
				&created->values[row * created->columns + column];
			int			length;

			cell->is_null = PQgetisnull(source, (int) row, (int) column) != 0;
			if (cell->is_null)
				continue;
			length = PQgetlength(source, (int) row, (int) column);
			if (length < 0)
				goto protocol_error;
			cell->length = (size_t) length;
			cell->data = malloc(cell->length + 1);
			if (cell->data == NULL)
				goto no_memory;
			memcpy(cell->data, PQgetvalue(source, (int) row, (int) column),
				cell->length);
			cell->data[cell->length] = '\0';
		}
	}
	postgamma_private_copy(
		created->command_status, sizeof(created->command_status),
		PQcmdStatus(source));
	value = PQresultErrorField(source, PG_DIAG_SQLSTATE);
	postgamma_private_copy(created->sqlstate, sizeof(created->sqlstate), value);
	value = PQresultErrorField(source, PG_DIAG_SEVERITY_NONLOCALIZED);
	postgamma_private_copy(created->severity, sizeof(created->severity), value);
	value = PQresultErrorField(source, PG_DIAG_MESSAGE_PRIMARY);
	postgamma_private_copy(created->message, sizeof(created->message), value);
	value = PQresultErrorField(source, PG_DIAG_MESSAGE_DETAIL);
	postgamma_private_copy(created->detail, sizeof(created->detail), value);
	for (size_t field = 0; field < 17; field++)
	{
		value = PQresultErrorField(source, diagnostic_codes[field]);
		if (value != NULL)
		{
			created->diagnostics[field] = postgamma_private_duplicate(value);
			if (created->diagnostics[field] == NULL)
				goto no_memory;
		}
	}
	created->transaction_status = (char) PQtransactionStatus(connection);
	*target = created;
	return POSTGAMMA_PRIVATE_LIBPQ_OK;

no_memory:
	postgamma_private_libpq_result_free(created);
	return POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;

protocol_error:
	postgamma_private_libpq_result_free(created);
	return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
}


static PostgammaPrivateLibpqStatus
postgamma_private_first_result(PostgammaPrivateLibpqConnection *connection,
							  int64_t deadline_ns,
							  PostgammaPrivateQueryResult *result)
{
	PGconn     *postgres = connection->postgres_connection;

	for (;;)
	{
		int			flush_status;

		if (!PQconsumeInput(postgres))
			return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		postgamma_private_dispatch_notifications(connection);
		flush_status = PQflush(postgres);
		if (flush_status < 0)
			return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		if (!PQisBusy(postgres))
		{
			PGresult   *postgres_result = PQgetResult(postgres);

			if (postgres_result != NULL)
			{
				postgamma_private_capture_result(
					postgres, postgres_result, result);
				PQclear(postgres_result);
				return POSTGAMMA_PRIVATE_LIBPQ_OK;
			}
			return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
		}
		{
			PostgammaPrivateLibpqStatus status = postgamma_private_wait(
				connection, true, flush_status == 1, deadline_ns);

			if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
				return status;
		}
	}
}


static PostgammaPrivateLibpqStatus
postgamma_private_next_pgresult(
	PostgammaPrivateLibpqConnection *connection,
	int64_t deadline_ns,
	PGresult **result)
{
	PGconn     *postgres;

	if (connection == NULL || result == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*result = NULL;
	postgres = connection->postgres_connection;
	for (;;)
	{
		int			flush_status;

		if (!PQconsumeInput(postgres))
			return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		postgamma_private_dispatch_notifications(connection);
		flush_status = PQflush(postgres);
		if (flush_status < 0)
			return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		if (!PQisBusy(postgres))
		{
			*result = PQgetResult(postgres);
			return *result != NULL ? POSTGAMMA_PRIVATE_LIBPQ_OK :
				POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
		}
		{
			PostgammaPrivateLibpqStatus status = postgamma_private_wait(
				connection, true, flush_status == 1, deadline_ns);

			if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
				return status;
		}
	}
}


static PostgammaPrivateLibpqStatus
postgamma_private_drain_to_idle(PostgammaPrivateLibpqConnection *connection,
							   int64_t deadline_ns)
{
	PGconn     *postgres = connection->postgres_connection;

	for (;;)
	{
		int			flush_status;

		if (!PQconsumeInput(postgres))
			return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		postgamma_private_dispatch_notifications(connection);
		flush_status = PQflush(postgres);
		if (flush_status < 0)
			return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		if (!PQisBusy(postgres))
		{
			PGresult   *extra = PQgetResult(postgres);

			if (extra == NULL)
			{
				PostgammaPrivateLibpqStatus status;

				if (flush_status == 0)
					return POSTGAMMA_PRIVATE_LIBPQ_OK;
				status = postgamma_private_wait(
					connection, true, true, deadline_ns);
				if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
					return status;
				continue;
			}
			PQclear(extra);
			continue;
		}
		{
			PostgammaPrivateLibpqStatus status = postgamma_private_wait(
				connection, true, flush_status == 1, deadline_ns);

			if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
				return status;
		}
	}
}


static PostgammaPrivateLibpqStatus
postgamma_private_put_copy_data(
	PostgammaPrivateLibpqConnection *connection,
	const void *data, int length, int64_t deadline_ns)
{
	PGconn     *postgres = connection->postgres_connection;

	for (;;)
	{
		int			put_status = PQputCopyData(postgres, data, length);

		if (put_status == 1)
			return POSTGAMMA_PRIVATE_LIBPQ_OK;
		if (put_status < 0)
			return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		if (!PQconsumeInput(postgres))
			return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		postgamma_private_dispatch_notifications(connection);
		{
			int			flush_status = PQflush(postgres);
			PostgammaPrivateLibpqStatus status;

			if (flush_status < 0)
				return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
			if (flush_status == 0)
				return POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;
			status = postgamma_private_wait(
				connection, true, true, deadline_ns);
			if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
				return status;
		}
	}
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_create(const PostgammaPrivateLibpqOptions *options,
							   PostgammaPrivateLibpqConnection **connection,
							   PostgammaMemoryEndpoint **backend_endpoint)
{
	static const char *const keywords[] = {
		"host", "port", "user", "dbname", "password",
		"application_name", "options", "sslmode", "gssencmode", "channel_binding",
		"target_session_attrs", "load_balance_hosts", "max_protocol_version",
		NULL,
	};
	const char *values[14];
	PostgammaPrivateLibpqConnection *created;
	PostgammaMemoryEndpoint *frontend = NULL;
	PostgammaMemoryEndpoint *backend = NULL;
	PostgammaMemoryStatus transport_status;
	PostgammaPrivateLibpqStatus status;

	if (connection == NULL || backend_endpoint == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*connection = NULL;
	*backend_endpoint = NULL;
	if (options == NULL || options->generation == 0 ||
		options->queue_capacity == 0 || options->user == NULL ||
		options->user[0] == '\0' || options->database == NULL ||
		options->database[0] == '\0' || options->cancel_callback == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;
	created->magic = POSTGAMMA_PRIVATE_LIBPQ_MAGIC;
	created->generation = options->generation;
	created->queue_capacity = options->queue_capacity;
	created->cancel_callback = options->cancel_callback;
	created->cancel_argument = options->cancel_argument;
	created->notice_callback = options->notice_callback;
	created->notice_argument = options->notice_argument;
	created->notification_callback = options->notification_callback;
	created->notification_argument = options->notification_argument;
	if (pthread_mutex_init(&created->mutex, NULL) != 0)
	{
		free(created);
		return POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;
	}
	transport_status = postgamma_memory_transport_create(
		options->generation, options->queue_capacity, &frontend, &backend);
	if (transport_status != POSTGAMMA_MEMORY_STATUS_OK)
	{
		(void) pthread_mutex_destroy(&created->mutex);
		free(created);
		return transport_status == POSTGAMMA_MEMORY_STATUS_NO_MEMORY ?
			POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY :
			POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
	}
	created->endpoint = frontend;
	values[0] = "postgamma-memory.invalid";
	values[1] = "5432";
	values[2] = options->user;
	values[3] = options->database;
	values[4] = "unused";
	values[5] = options->application_name != NULL ?
		options->application_name : "postgamma-embedded";
	values[6] = options->startup_options != NULL ? options->startup_options : "";
	values[7] = "disable";
	values[8] = "disable";
	values[9] = "disable";
	values[10] = "any";
	values[11] = "disable";
	values[12] = "3.0";
	values[13] = NULL;
	status = postgamma_private_bind(created);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		created->postgres_connection =
			PQconnectStartParams(keywords, values, 0);
		postgamma_private_restore(created);
	}
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK ||
		created->postgres_connection == NULL ||
		PQstatus(created->postgres_connection) == CONNECTION_BAD)
	{
		if (created->postgres_connection != NULL)
			PQfinish(created->postgres_connection);
		created->postgres_connection = NULL;
		(void) postgamma_memory_endpoint_release(&frontend, options->generation);
		(void) postgamma_memory_endpoint_release(&backend, options->generation);
		(void) pthread_mutex_destroy(&created->mutex);
		created->magic = 0;
		free(created);
		return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	}
	(void) PQsetNoticeReceiver(
		created->postgres_connection, postgamma_private_notice_receiver, created);
	*connection = created;
	*backend_endpoint = backend;
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_connect(PostgammaPrivateLibpqConnection *connection,
								int64_t deadline_ns)
{
	PostgammaPrivateLibpqStatus status;

	if (!postgamma_private_libpq_valid(connection) || deadline_ns < 0)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	status = postgamma_private_bind(connection);
	connection->connect_operation = true;
	connection->operation_deadline_ns = deadline_ns;
	while (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		PostgresPollingStatusType poll_status =
			PQconnectPoll(connection->postgres_connection);

		if (poll_status == PGRES_POLLING_OK)
			break;
		if (poll_status == PGRES_POLLING_FAILED)
		{
			status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
			break;
		}
		status = postgamma_private_wait(
			connection, true, poll_status == PGRES_POLLING_WRITING,
			deadline_ns);
	}
	connection->connect_operation = false;
	connection->operation_deadline_ns = 0;
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		atomic_store_explicit(
			&connection->backend_pid,
			PQbackendPID(connection->postgres_connection),
			memory_order_release);
	postgamma_private_restore(connection);
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_tool_borrow(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	void **native_connection)
{
	PostgammaPrivateLibpqStatus status;

	if (native_connection == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*native_connection = NULL;
	if (!postgamma_private_libpq_valid(connection) ||
		request_generation == 0)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (connection->active_operation != NULL ||
		atomic_load_explicit(
			&connection->active_request_generation,
			memory_order_acquire) != 0)
	{
		(void) pthread_mutex_unlock(&connection->mutex);
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	}
	status = postgamma_private_bind(connection);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		(void) pthread_mutex_unlock(&connection->mutex);
		return status;
	}
	atomic_store_explicit(
		&connection->active_request_generation,
		request_generation, memory_order_release);
	*native_connection = connection->postgres_connection;
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_tool_release(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation)
{
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;

	if (connection == NULL || request_generation == 0)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	/*
	 * A successful tool borrow binds the connection to this thread while
	 * holding connection->mutex.  A different thread cannot safely release
	 * that ownership, but the owning thread must always restore and unlock,
	 * even when a generation or validity invariant has been violated.
	 */
	if (PostgammaCurrentPrivateLibpqConnection != connection)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (!postgamma_private_libpq_valid(connection) ||
		atomic_load_explicit(
			&connection->active_request_generation,
			memory_order_acquire) != request_generation)
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	atomic_store_explicit(
		&connection->active_request_generation,
		UINT64_C(0), memory_order_release);
	postgamma_private_restore(connection);
	if (pthread_mutex_unlock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_query(PostgammaPrivateLibpqConnection *connection,
							  const char *query, int64_t deadline_ns,
							  PostgammaPrivateQueryResult *result)
{
	PostgammaPrivateLibpqStatus status;

	if (!postgamma_private_libpq_valid(connection) || query == NULL ||
		query[0] == '\0' || deadline_ns < 0 || result == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	memset(result, 0, sizeof(*result));
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	status = postgamma_private_bind(connection);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
		!PQsendQuery(connection->postgres_connection, query))
		status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		status = postgamma_private_first_result(connection, deadline_ns, result);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		status = postgamma_private_drain_to_idle(connection, deadline_ns);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		result->transaction_status = (char)
			PQtransactionStatus(connection->postgres_connection);
	postgamma_private_restore(connection);
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_query_params(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *query,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	int64_t deadline_ns,
	PostgammaPrivateOwnedResult **result)
{
	PostgammaPrivateLibpqOperation *operation = NULL;
	PostgammaPrivateOwnedResult *current_result = NULL;
	PostgammaPrivateLibpqProgress progress;
	PostgammaPrivateLibpqStatus status;

	if (result == NULL ||
		(deadline_ns != POSTGAMMA_MEMORY_NO_DEADLINE && deadline_ns < 0))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*result = NULL;
	status = postgamma_private_libpq_operation_start(
		connection, request_generation, query, parameters, parameter_count,
		result_format, &operation);
	while (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		status = postgamma_private_libpq_operation_progress(
			operation, &progress, &current_result);
		if (progress == POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_RESULT_READY)
		{
			if (*result != NULL)
			{
				postgamma_private_libpq_result_free(current_result);
				status = POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
				break;
			}
			*result = current_result;
			current_result = NULL;
			continue;
		}
		if (progress == POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_COMPLETE)
			break;
		status = postgamma_private_libpq_operation_wait(operation, deadline_ns);
	}
	postgamma_private_libpq_result_free(current_result);
	postgamma_private_libpq_operation_free(&operation);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_operation_start(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *query,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	PostgammaPrivateLibpqOperation **operation)
{
	return postgamma_private_operation_start_internal(
		connection, request_generation, POSTGAMMA_PRIVATE_SUBMISSION_QUERY,
		query, parameters, parameter_count, result_format, NULL, operation);
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_operation_start_ex(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *query,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	const PostgammaPrivateResultPolicy *result_policy,
	PostgammaPrivateLibpqOperation **operation)
{
	return postgamma_private_operation_start_internal(
		connection, request_generation, POSTGAMMA_PRIVATE_SUBMISSION_QUERY,
		query, parameters, parameter_count, result_format, result_policy,
		operation);
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_operation_start_script(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *query,
	PostgammaPrivateLibpqOperation **operation)
{
	return postgamma_private_operation_start_internal(
		connection, request_generation, POSTGAMMA_PRIVATE_SUBMISSION_SCRIPT,
		query, NULL, 0, 0, NULL, operation);
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_operation_start_script_ex(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *query,
	const PostgammaPrivateResultPolicy *result_policy,
	PostgammaPrivateLibpqOperation **operation)
{
	return postgamma_private_operation_start_internal(
		connection, request_generation, POSTGAMMA_PRIVATE_SUBMISSION_SCRIPT,
		query, NULL, 0, 0, result_policy, operation);
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_operation_start_prepared(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *statement_name,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	PostgammaPrivateLibpqOperation **operation)
{
	return postgamma_private_operation_start_internal(
		connection, request_generation, POSTGAMMA_PRIVATE_SUBMISSION_PREPARED,
		statement_name, parameters, parameter_count, result_format, NULL,
		operation);
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_operation_start_prepared_ex(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *statement_name,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	const PostgammaPrivateResultPolicy *result_policy,
	PostgammaPrivateLibpqOperation **operation)
{
	return postgamma_private_operation_start_internal(
		connection, request_generation, POSTGAMMA_PRIVATE_SUBMISSION_PREPARED,
		statement_name, parameters, parameter_count, result_format,
		result_policy, operation);
}


static PostgammaPrivateLibpqStatus
postgamma_private_operation_start_internal(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	PostgammaPrivateSubmissionKind submission_kind,
	const char *query_or_statement,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	const PostgammaPrivateResultPolicy *result_policy,
	PostgammaPrivateLibpqOperation **operation)
{
	Oid		   *parameter_types = NULL;
	const char **parameter_values = NULL;
	int		   *parameter_lengths = NULL;
	int		   *parameter_formats = NULL;
	PostgammaPrivateLibpqOperation *created = NULL;
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	bool		locked = false;
	bool		result_policy_installed = false;

	if (operation == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*operation = NULL;
	if (!postgamma_private_libpq_valid(connection) ||
		request_generation == 0 || query_or_statement == NULL ||
		query_or_statement[0] == '\0' ||
		submission_kind < POSTGAMMA_PRIVATE_SUBMISSION_QUERY ||
		submission_kind > POSTGAMMA_PRIVATE_SUBMISSION_PREPARED ||
		result_format > 1 || parameter_count > INT_MAX ||
		(parameter_count != 0 && parameters == NULL) ||
		(result_policy != NULL &&
		 !postgamma_private_result_policy_valid(result_policy)) ||
		(submission_kind == POSTGAMMA_PRIVATE_SUBMISSION_SCRIPT &&
		 (parameter_count != 0 || result_format != 0)))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;
	if (parameter_count != 0)
	{
		parameter_types = calloc(parameter_count, sizeof(*parameter_types));
		parameter_values = calloc(parameter_count, sizeof(*parameter_values));
		parameter_lengths = calloc(parameter_count, sizeof(*parameter_lengths));
		parameter_formats = calloc(parameter_count, sizeof(*parameter_formats));
		if (parameter_types == NULL || parameter_values == NULL ||
			parameter_lengths == NULL || parameter_formats == NULL)
		{
			status = POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;
			goto done;
		}
	}
	for (size_t index = 0; index < parameter_count; index++)
	{
		const PostgammaPrivateParameter *parameter = &parameters[index];

		if (parameter->format > 1 || parameter->length > INT_MAX ||
			(parameter->is_null &&
			 (parameter->data != NULL || parameter->length != 0)) ||
			(!parameter->is_null && parameter->data == NULL))
		{
			status = POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
			goto done;
		}
		parameter_types[index] = (Oid) parameter->type_oid;
		parameter_values[index] = parameter->is_null ? NULL : parameter->data;
		parameter_lengths[index] = (int) parameter->length;
		parameter_formats[index] = (int) parameter->format;
	}
	if (pthread_mutex_lock(&connection->mutex) != 0)
	{
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
		goto done;
	}
	locked = true;
	if (connection->active_operation != NULL)
	{
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
		goto done;
	}
	if (result_policy != NULL)
	{
		PostgammaMemoryResultPolicy memory_policy =
			POSTGAMMA_MEMORY_RESULT_POLICY_INIT;
		PostgammaMemoryStatus memory_status;

		memory_policy.request_generation = request_generation;
		memory_policy.delivery_mode = result_policy->delivery_mode;
		memory_policy.target_chunk_rows = result_policy->target_chunk_rows;
		memory_policy.result_buffer_limit = result_policy->result_buffer_limit;
		memory_policy.maximum_value_size = result_policy->maximum_value_size;
		memory_status = postgamma_memory_endpoint_set_result_policy(
			connection->endpoint, connection->generation, &memory_policy);
		status = postgamma_private_memory_status(memory_status);
		if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
			goto done;
		result_policy_installed = true;
	}
	status = postgamma_private_bind(connection);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		int			sent;

		if (submission_kind == POSTGAMMA_PRIVATE_SUBMISSION_SCRIPT)
			sent = PQsendQuery(
				connection->postgres_connection, query_or_statement);
		else if (submission_kind == POSTGAMMA_PRIVATE_SUBMISSION_PREPARED)
			sent = PQsendQueryPrepared(
				connection->postgres_connection, query_or_statement,
				(int) parameter_count, parameter_values, parameter_lengths,
				parameter_formats, (int) result_format);
		else
			sent = PQsendQueryParams(
				connection->postgres_connection, query_or_statement,
				(int) parameter_count, parameter_types, parameter_values,
				parameter_lengths, parameter_formats, (int) result_format);
		if (!sent)
			status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		else if (result_policy != NULL &&
				 result_policy->delivery_mode ==
				 POSTGAMMA_MEMORY_DELIVERY_CHUNKED &&
				 !PQsetChunkedRowsMode(
					 connection->postgres_connection,
					 (int) result_policy->target_chunk_rows))
		{
			(void) postgamma_memory_endpoint_abort(
				connection->endpoint, connection->generation,
				POSTGAMMA_MEMORY_ABORT_PROTOCOL);
			status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
		}
	}
	postgamma_private_restore(connection);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		created->magic = POSTGAMMA_PRIVATE_LIBPQ_OPERATION_MAGIC;
		created->connection = connection;
		created->request_generation = request_generation;
		created->phase = POSTGAMMA_PRIVATE_LIBPQ_OPERATION_ACTIVE;
		created->terminal_status = POSTGAMMA_PRIVATE_LIBPQ_OK;
		if (result_policy != NULL)
		{
			created->result_policy = *result_policy;
			created->result_policy_active = true;
		}
		created->wait_events = POSTGAMMA_MEMORY_WAIT_READABLE;
		connection->active_operation = created;
		atomic_store_explicit(
			&connection->active_request_generation, request_generation,
			memory_order_release);
		*operation = created;
		created = NULL;
	}

done:
	if (locked && status != POSTGAMMA_PRIVATE_LIBPQ_OK &&
		result_policy_installed)
		(void) postgamma_private_clear_result_policy(connection);
	if (locked)
		(void) pthread_mutex_unlock(&connection->mutex);
	free(created);
	free(parameter_formats);
	free(parameter_lengths);
	free(parameter_values);
	free(parameter_types);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_operation_progress(
	PostgammaPrivateLibpqOperation *operation,
	PostgammaPrivateLibpqProgress *progress,
	PostgammaPrivateOwnedResult **result)
{
	PostgammaPrivateLibpqConnection *connection;
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	PGconn	   *postgres;
	int			flush_status = 0;

	if (progress == NULL || result == NULL ||
		!postgamma_private_operation_valid(operation))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*result = NULL;
	*progress = POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_WAITING;
	connection = operation->connection;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (operation->phase == POSTGAMMA_PRIVATE_LIBPQ_OPERATION_COMPLETE)
	{
		*progress = POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_COMPLETE;
		status = operation->terminal_status;
		(void) pthread_mutex_unlock(&connection->mutex);
		return status;
	}
	if (connection->active_operation != operation)
	{
		(void) pthread_mutex_unlock(&connection->mutex);
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	}
	if (operation->copy_phase == POSTGAMMA_PRIVATE_LIBPQ_COPY_ACTIVE)
	{
		operation->wait_events = POSTGAMMA_MEMORY_WAIT_READABLE |
			POSTGAMMA_MEMORY_WAIT_WRITABLE;
		(void) pthread_mutex_unlock(&connection->mutex);
		return POSTGAMMA_PRIVATE_LIBPQ_OK;
	}
	status = postgamma_private_bind(connection);
	postgres = connection->postgres_connection;
	for (unsigned int iteration = 0;
		 status == POSTGAMMA_PRIVATE_LIBPQ_OK && iteration < 64;
		 iteration++)
	{
		PGresult   *postgres_result;

		if (!PQconsumeInput(postgres))
		{
			status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
			break;
		}
		postgamma_private_dispatch_notifications(connection);
		flush_status = PQflush(postgres);
		if (flush_status < 0)
		{
			status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
			break;
		}
		operation->wait_events = POSTGAMMA_MEMORY_WAIT_READABLE |
			(flush_status == 1 ? POSTGAMMA_MEMORY_WAIT_WRITABLE : 0);
		if (PQisBusy(postgres))
			break;
		postgres_result = PQgetResult(postgres);
		if (postgres_result != NULL)
		{
			PostgammaPrivateResultStatus result_status =
				postgamma_private_result_status(PQresultStatus(postgres_result));

			status = postgamma_private_capture_owned_result(
				postgres, postgres_result,
				operation->result_policy_active ?
				&operation->result_policy : NULL,
				result);
			PQclear(postgres_result);
			if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
			{
				if (result_status == POSTGAMMA_PRIVATE_RESULT_COPY_IN)
				{
					operation->copy_direction = POSTGAMMA_PRIVATE_COPY_IN;
					operation->copy_phase = POSTGAMMA_PRIVATE_LIBPQ_COPY_ACTIVE;
				}
				else if (result_status == POSTGAMMA_PRIVATE_RESULT_COPY_OUT)
				{
					operation->copy_direction = POSTGAMMA_PRIVATE_COPY_OUT;
					operation->copy_phase = POSTGAMMA_PRIVATE_LIBPQ_COPY_ACTIVE;
				}
				else if (result_status == POSTGAMMA_PRIVATE_RESULT_COPY_BOTH)
				{
					postgamma_private_libpq_result_free(*result);
					*result = NULL;
					status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
				}
			}
			if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
				*progress = POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_RESULT_READY;
			break;
		}
		if (flush_status == 0)
		{
			operation->phase = POSTGAMMA_PRIVATE_LIBPQ_OPERATION_COMPLETE;
			break;
		}
	}
	postgamma_private_restore(connection);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		operation->terminal_status = status;
		operation->phase = POSTGAMMA_PRIVATE_LIBPQ_OPERATION_COMPLETE;
	}
	if (operation->phase == POSTGAMMA_PRIVATE_LIBPQ_OPERATION_COMPLETE)
	{
		PostgammaPrivateLibpqStatus clear_status;

		connection->active_operation = NULL;
		atomic_store_explicit(
			&connection->active_request_generation, UINT64_C(0),
			memory_order_release);
		clear_status = postgamma_private_clear_result_policy(connection);
		if (operation->terminal_status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
			clear_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
			operation->terminal_status = clear_status;
		*progress = POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_COMPLETE;
		status = operation->terminal_status;
	}
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_operation_wait(
	PostgammaPrivateLibpqOperation *operation, int64_t deadline_ns)
{
	PostgammaMemoryWaitResult wait_result;

	if (!postgamma_private_operation_valid(operation) ||
		(deadline_ns != POSTGAMMA_MEMORY_NO_DEADLINE && deadline_ns < 0))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	if (operation->phase == POSTGAMMA_PRIVATE_LIBPQ_OPERATION_COMPLETE)
		return operation->terminal_status;
	wait_result = postgamma_memory_endpoint_wait(
		operation->connection->endpoint,
		operation->connection->generation,
		operation->wait_events | POSTGAMMA_MEMORY_WAIT_PEER_CLOSED,
		0, deadline_ns);
	switch (wait_result.status)
	{
		case POSTGAMMA_MEMORY_STATUS_OK:
		case POSTGAMMA_MEMORY_STATUS_EOF:
		case POSTGAMMA_MEMORY_STATUS_PEER_CLOSED:
			return POSTGAMMA_PRIVATE_LIBPQ_OK;
		case POSTGAMMA_MEMORY_STATUS_TIMEOUT:
			return POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT;
		case POSTGAMMA_MEMORY_STATUS_CANCELLED:
			return POSTGAMMA_PRIVATE_LIBPQ_CANCELLED;
		default:
			return POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
	}
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_operation_ready(
	PostgammaPrivateLibpqOperation *operation, bool *ready)
{
	PostgammaPrivateLibpqConnection *connection;
	PostgammaMemoryStatus memory_status;
	uint32_t	events = 0;

	if (!postgamma_private_operation_valid(operation) || ready == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*ready = false;
	connection = operation->connection;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (operation->phase == POSTGAMMA_PRIVATE_LIBPQ_OPERATION_COMPLETE)
	{
		(void) pthread_mutex_unlock(&connection->mutex);
		return POSTGAMMA_PRIVATE_LIBPQ_OK;
	}
	if (connection->active_operation != operation)
	{
		(void) pthread_mutex_unlock(&connection->mutex);
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	}
	memory_status = postgamma_memory_endpoint_ready(
		connection->endpoint, connection->generation, &events);
	if (memory_status == POSTGAMMA_MEMORY_STATUS_OK)
		*ready = (events & (operation->wait_events |
			POSTGAMMA_MEMORY_WAIT_PEER_CLOSED)) != 0;
	(void) pthread_mutex_unlock(&connection->mutex);
	return memory_status == POSTGAMMA_MEMORY_STATUS_OK ?
		POSTGAMMA_PRIVATE_LIBPQ_OK : POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
}


static PostgammaPrivateLibpqStatus
postgamma_private_copy_input_pump_bound(
	PostgammaPrivateLibpqOperation *operation, int *flush_status,
	bool allow_remote_termination)
{
	PostgammaPrivateLibpqConnection *connection = operation->connection;
	PGconn	   *postgres = connection->postgres_connection;

	if (!PQconsumeInput(postgres))
		return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	postgamma_private_dispatch_notifications(connection);
	if (postgres->inEnd > postgres->inStart &&
		postgres->inBuffer[postgres->inStart] == PqMsg_ErrorResponse)
		operation->copy_remote_terminated = true;
	*flush_status = PQflush(postgres);
	if (*flush_status < 0)
		return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	operation->wait_events = POSTGAMMA_MEMORY_WAIT_READABLE |
		(*flush_status == 1 ? POSTGAMMA_MEMORY_WAIT_WRITABLE : 0);
	return operation->copy_remote_terminated && !allow_remote_termination ?
		POSTGAMMA_PRIVATE_LIBPQ_COPY_TERMINATED :
		POSTGAMMA_PRIVATE_LIBPQ_OK;
}


static PostgammaPrivateLibpqStatus
postgamma_private_copy_output_fill_bound(
	PostgammaPrivateLibpqOperation *operation, bool *read_bytes)
{
	PGconn	   *postgres = operation->connection->postgres_connection;
	int			read_status;

	*read_bytes = false;
	read_status = pqReadData(postgres);
	if (read_status < 0)
		return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	*read_bytes = read_status > 0;
	operation->wait_events = POSTGAMMA_MEMORY_WAIT_READABLE;
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}


static PostgammaPrivateLibpqStatus
postgamma_private_copy_control_bound(
	PostgammaPrivateLibpqOperation *operation,
	PostgammaPrivateCopyProgress *progress)
{
	PostgammaPrivateLibpqConnection *connection = operation->connection;
	PGconn	   *postgres = connection->postgres_connection;
	size_t		available = (size_t) (postgres->inEnd - postgres->inStart);
	uint32_t	wire_length;
	uint32_t	message_length;
	size_t		frame_size;
	int			saved_end;
	char	   *copy_buffer = NULL;
	int			copy_status;

	if (available < 5)
		return POSTGAMMA_PRIVATE_LIBPQ_OK;
	memcpy(&wire_length, postgres->inBuffer + postgres->inStart + 1,
		   sizeof(wire_length));
	message_length = pg_ntoh32(wire_length);
	if (message_length < 4 || message_length > INT_MAX - 1)
		return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
	frame_size = (size_t) message_length + 1;
	if (frame_size > operation->result_policy.result_buffer_limit)
		return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
	if (available < frame_size)
		return POSTGAMMA_PRIVATE_LIBPQ_OK;

	/*
	 * Hide subsequent frames while libpq handles one bounded control frame.
	 * Otherwise PQgetCopyData() can inspect the following CopyData header and
	 * grow its input buffer to the complete payload before returning.
	 */
	saved_end = postgres->inEnd;
	postgres->inEnd = postgres->inStart + (int) frame_size;
	copy_status = PQgetCopyData(postgres, &copy_buffer, 1);
	postgres->inEnd = saved_end;
	postgamma_private_dispatch_notifications(connection);
	if (copy_buffer != NULL)
		PQfreemem(copy_buffer);
	if (copy_status > 0)
		return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
	if (copy_status == -2)
		return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	if (copy_status == -1)
	{
		operation->copy_phase = POSTGAMMA_PRIVATE_LIBPQ_COPY_ENDED;
		*progress = POSTGAMMA_PRIVATE_COPY_END;
	}
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_copy_direction(
	PostgammaPrivateLibpqOperation *operation,
	PostgammaPrivateCopyDirection *direction)
{
	PostgammaPrivateLibpqConnection *connection;

	if (!postgamma_private_operation_valid(operation) || direction == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	connection = operation->connection;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	*direction = operation->copy_direction;
	(void) pthread_mutex_unlock(&connection->mutex);
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_copy_pump(
	PostgammaPrivateLibpqOperation *operation,
	PostgammaPrivateCopyProgress *progress)
{
	PostgammaPrivateLibpqConnection *connection;
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	int			flush_status = 0;
	bool		read_bytes = false;

	if (!postgamma_private_operation_valid(operation) || progress == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*progress = POSTGAMMA_PRIVATE_COPY_AGAIN;
	connection = operation->connection;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (connection->active_operation != operation ||
		operation->copy_phase == POSTGAMMA_PRIVATE_LIBPQ_COPY_NONE)
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	else if (operation->copy_phase == POSTGAMMA_PRIVATE_LIBPQ_COPY_ENDED)
		*progress = POSTGAMMA_PRIVATE_COPY_END;
	else
	{
		status = postgamma_private_bind(connection);
		if (status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
			operation->copy_direction == POSTGAMMA_PRIVATE_COPY_IN)
		{
			status = postgamma_private_copy_input_pump_bound(
				operation, &flush_status, true);
			if (status == POSTGAMMA_PRIVATE_LIBPQ_OK && flush_status == 0)
				*progress = POSTGAMMA_PRIVATE_COPY_PROGRESS;
		}
		else if (status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
				 operation->copy_direction == POSTGAMMA_PRIVATE_COPY_OUT)
		{
			status = postgamma_private_copy_output_fill_bound(
				operation, &read_bytes);
			if (status == POSTGAMMA_PRIVATE_LIBPQ_OK && read_bytes)
				*progress = POSTGAMMA_PRIVATE_COPY_PROGRESS;
		}
		postgamma_private_restore(connection);
	}
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_copy_write(
	PostgammaPrivateLibpqOperation *operation,
	const void *data, size_t size, size_t *consumed,
	PostgammaPrivateCopyProgress *progress)
{
	PostgammaPrivateLibpqConnection *connection;
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	int			flush_status = 0;
	size_t		chunk_size;
	int			put_status;

	if (!postgamma_private_operation_valid(operation) || consumed == NULL ||
		progress == NULL || (size != 0 && data == NULL))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*consumed = 0;
	*progress = POSTGAMMA_PRIVATE_COPY_AGAIN;
	connection = operation->connection;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (connection->active_operation != operation ||
		operation->copy_direction != POSTGAMMA_PRIVATE_COPY_IN ||
		operation->copy_phase != POSTGAMMA_PRIVATE_LIBPQ_COPY_ACTIVE ||
		operation->copy_finish_sent)
	{
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
		goto done;
	}
	status = postgamma_private_bind(connection);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		goto done;
	status = postgamma_private_copy_input_pump_bound(
		operation, &flush_status, false);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK || flush_status == 1)
		goto restore;
	if (size == 0)
	{
		*progress = POSTGAMMA_PRIVATE_COPY_PROGRESS;
		goto restore;
	}
	chunk_size = size;
	if (chunk_size > connection->queue_capacity)
		chunk_size = connection->queue_capacity;
	if (chunk_size > (size_t) INT_MAX)
		chunk_size = (size_t) INT_MAX;
	put_status = PQputCopyData(
		connection->postgres_connection, data, (int) chunk_size);
	if (put_status < 0)
		status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	else if (put_status == 1)
	{
		*consumed = chunk_size;
		*progress = POSTGAMMA_PRIVATE_COPY_PROGRESS;
		flush_status = PQflush(connection->postgres_connection);
		if (flush_status < 0)
			status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		else
			operation->wait_events = POSTGAMMA_MEMORY_WAIT_READABLE |
				(flush_status == 1 ? POSTGAMMA_MEMORY_WAIT_WRITABLE : 0);
	}

restore:
	postgamma_private_restore(connection);
done:
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_copy_finish(
	PostgammaPrivateLibpqOperation *operation, const char *failure_message,
	PostgammaPrivateCopyProgress *progress)
{
	PostgammaPrivateLibpqConnection *connection;
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	int			flush_status = 0;

	if (!postgamma_private_operation_valid(operation) || progress == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*progress = POSTGAMMA_PRIVATE_COPY_AGAIN;
	connection = operation->connection;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (connection->active_operation != operation ||
		operation->copy_direction != POSTGAMMA_PRIVATE_COPY_IN ||
		operation->copy_phase == POSTGAMMA_PRIVATE_LIBPQ_COPY_NONE)
	{
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
		goto done;
	}
	if (operation->copy_phase == POSTGAMMA_PRIVATE_LIBPQ_COPY_ENDED)
	{
		*progress = POSTGAMMA_PRIVATE_COPY_END;
		goto done;
	}
	status = postgamma_private_bind(connection);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		goto done;
	status = postgamma_private_copy_input_pump_bound(
		operation, &flush_status, true);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK || flush_status == 1)
		goto restore;
	if (!operation->copy_finish_sent)
	{
		if (PQputCopyEnd(connection->postgres_connection, failure_message) != 1)
		{
			status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
			goto restore;
		}
		operation->copy_finish_sent = true;
	}
	flush_status = PQflush(connection->postgres_connection);
	if (flush_status < 0)
		status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	else if (flush_status == 0)
	{
		operation->copy_phase = POSTGAMMA_PRIVATE_LIBPQ_COPY_ENDED;
		*progress = POSTGAMMA_PRIVATE_COPY_END;
	}
	else
		operation->wait_events = POSTGAMMA_MEMORY_WAIT_READABLE |
			POSTGAMMA_MEMORY_WAIT_WRITABLE;

restore:
	postgamma_private_restore(connection);
done:
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_copy_read(
	PostgammaPrivateLibpqOperation *operation,
	void *buffer, size_t capacity, size_t *produced,
	PostgammaPrivateCopyProgress *progress)
{
	PostgammaPrivateLibpqConnection *connection;
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	PGconn	   *postgres;
	unsigned char *target = buffer;
	unsigned int iterations = 0;

	if (!postgamma_private_operation_valid(operation) || buffer == NULL ||
		capacity == 0 || produced == NULL || progress == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*produced = 0;
	*progress = POSTGAMMA_PRIVATE_COPY_AGAIN;
	connection = operation->connection;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (connection->active_operation != operation ||
		operation->copy_direction != POSTGAMMA_PRIVATE_COPY_OUT ||
		operation->copy_phase == POSTGAMMA_PRIVATE_LIBPQ_COPY_NONE)
	{
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
		goto done;
	}
	if (operation->copy_phase == POSTGAMMA_PRIVATE_LIBPQ_COPY_ENDED)
	{
		*progress = POSTGAMMA_PRIVATE_COPY_END;
		goto done;
	}
	status = postgamma_private_bind(connection);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		goto done;
	postgres = connection->postgres_connection;
	while (*produced < capacity && iterations++ < 64)
	{
		size_t		available =
			(size_t) (postgres->inEnd - postgres->inStart);

		if (operation->copy_out_remaining != 0)
		{
			size_t amount;

			if (available == 0)
			{
				bool read_bytes;

				status = postgamma_private_copy_output_fill_bound(
					operation, &read_bytes);
				if (status != POSTGAMMA_PRIVATE_LIBPQ_OK || !read_bytes)
					break;
				available =
					(size_t) (postgres->inEnd - postgres->inStart);
			}
			amount = operation->copy_out_remaining;
			if (amount > available)
				amount = available;
			if (amount > capacity - *produced)
				amount = capacity - *produced;
			memcpy(target + *produced,
				   postgres->inBuffer + postgres->inStart, amount);
			postgres->inStart += (int) amount;
			postgres->inCursor = postgres->inStart;
			operation->copy_out_remaining -= amount;
			*produced += amount;
			continue;
		}

		if (available < 5)
		{
			bool read_bytes;

			status = postgamma_private_copy_output_fill_bound(
				operation, &read_bytes);
			if (status != POSTGAMMA_PRIVATE_LIBPQ_OK || !read_bytes)
				break;
			continue;
		}
		if (postgres->inBuffer[postgres->inStart] == PqMsg_CopyData)
		{
			uint32_t wire_length;
			uint32_t message_length;

			memcpy(&wire_length, postgres->inBuffer + postgres->inStart + 1,
				   sizeof(wire_length));
			message_length = pg_ntoh32(wire_length);
			if (message_length < 4)
			{
				status = POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
				break;
			}
			postgres->inStart += 5;
			postgres->inCursor = postgres->inStart;
			operation->copy_out_remaining =
				(size_t) message_length - 4;
			continue;
		}
		status = postgamma_private_copy_control_bound(operation, progress);
		if (status != POSTGAMMA_PRIVATE_LIBPQ_OK ||
			*progress == POSTGAMMA_PRIVATE_COPY_END)
			break;
		if ((size_t) (postgres->inEnd - postgres->inStart) == available)
		{
			bool read_bytes;

			status = postgamma_private_copy_output_fill_bound(
				operation, &read_bytes);
			if (status != POSTGAMMA_PRIVATE_LIBPQ_OK || !read_bytes)
				break;
		}
	}
	if (*produced != 0)
		*progress = POSTGAMMA_PRIVATE_COPY_PROGRESS;
	postgamma_private_restore(connection);
done:
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_copy_release(
	PostgammaPrivateLibpqOperation *operation)
{
	PostgammaPrivateLibpqConnection *connection;
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;

	if (!postgamma_private_operation_valid(operation))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	connection = operation->connection;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (connection->active_operation != operation ||
		operation->copy_phase != POSTGAMMA_PRIVATE_LIBPQ_COPY_ENDED)
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	else
	{
		operation->copy_phase = POSTGAMMA_PRIVATE_LIBPQ_COPY_NONE;
		operation->copy_direction = POSTGAMMA_PRIVATE_COPY_NONE;
		operation->copy_out_remaining = 0;
		operation->copy_finish_sent = false;
		operation->copy_remote_terminated = false;
		operation->wait_events = POSTGAMMA_MEMORY_WAIT_READABLE;
	}
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


void
postgamma_private_libpq_operation_free(
	PostgammaPrivateLibpqOperation **operation)
{
	PostgammaPrivateLibpqOperation *closing;
	PostgammaPrivateLibpqConnection *connection;

	if (operation == NULL || !postgamma_private_operation_valid(*operation))
		return;
	closing = *operation;
	connection = closing->connection;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return;
	if (connection->active_operation == closing)
	{
		connection->active_operation = NULL;
		atomic_store_explicit(
			&connection->active_request_generation, UINT64_C(0),
			memory_order_release);
		(void) postgamma_private_clear_result_policy(connection);
	}
	(void) pthread_mutex_unlock(&connection->mutex);
	closing->magic = 0;
	free(closing);
	*operation = NULL;
}


void
postgamma_private_libpq_prepared_description_free(
	PostgammaPrivatePreparedDescription *description)
{
	if (description == NULL)
		return;
	if (description->columns != NULL)
	{
		for (size_t column = 0; column < description->column_count; column++)
			free(description->columns[column].name);
	}
	free(description->columns);
	free(description->parameter_types);
	free(description);
}


static PostgammaPrivateLibpqStatus
postgamma_private_capture_description(
	PGresult *source,
	PostgammaPrivatePreparedDescription **description)
{
	PostgammaPrivatePreparedDescription *created;
	int			parameter_count;
	int			column_count;

	if (source == NULL || description == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*description = NULL;
	parameter_count = PQnparams(source);
	column_count = PQnfields(source);
	if (parameter_count < 0 || column_count < 0)
		return POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;
	created->parameter_count = (size_t) parameter_count;
	created->column_count = (size_t) column_count;
	if (created->parameter_count != 0)
	{
		created->parameter_types = calloc(
			created->parameter_count, sizeof(*created->parameter_types));
		if (created->parameter_types == NULL)
			goto no_memory;
	}
	if (created->column_count != 0)
	{
		created->columns = calloc(
			created->column_count, sizeof(*created->columns));
		if (created->columns == NULL)
			goto no_memory;
	}
	for (size_t parameter = 0;
		 parameter < created->parameter_count; parameter++)
		created->parameter_types[parameter] =
			(uint32_t) PQparamtype(source, (int) parameter);
	for (size_t column = 0; column < created->column_count; column++)
	{
		PostgammaPrivateResultField *field = &created->columns[column];

		field->name = postgamma_private_duplicate(PQfname(source, (int) column));
		if (field->name == NULL)
			goto no_memory;
		field->table_oid = (uint32_t) PQftable(source, (int) column);
		field->table_column = (int32_t) PQftablecol(source, (int) column);
		field->type_oid = (uint32_t) PQftype(source, (int) column);
		field->type_size = (int32_t) PQfsize(source, (int) column);
		field->type_modifier = (int32_t) PQfmod(source, (int) column);
		field->format = (uint16_t) PQfformat(source, (int) column);
	}
	*description = created;
	return POSTGAMMA_PRIVATE_LIBPQ_OK;

no_memory:
	postgamma_private_libpq_prepared_description_free(created);
	return POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;
}


static PostgammaPrivateLibpqStatus
postgamma_private_close_prepared_bound(
	PostgammaPrivateLibpqConnection *connection,
	const char *statement_name,
	int64_t deadline_ns,
	PostgammaPrivateOwnedResult **error_result)
{
	PGresult   *result = NULL;
	PostgammaPrivateLibpqStatus status;

	if (error_result != NULL)
		*error_result = NULL;
	if (!PQsendClosePrepared(connection->postgres_connection, statement_name))
		return POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	status = postgamma_private_next_pgresult(connection, deadline_ns, &result);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
		postgamma_private_result_status(PQresultStatus(result)) !=
			POSTGAMMA_PRIVATE_RESULT_COMMAND_OK)
	{
		if (error_result == NULL)
			status = POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
		else
			status = postgamma_private_capture_owned_result(
				connection->postgres_connection, result, NULL, error_result);
	}
	PQclear(result);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		status = postgamma_private_drain_to_idle(connection, deadline_ns);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_prepare(
	PostgammaPrivateLibpqConnection *connection,
	const char *statement_name,
	const char *query,
	const uint32_t *parameter_type_oids,
	size_t parameter_count,
	int64_t deadline_ns,
	PostgammaPrivatePreparedDescription **description,
	PostgammaPrivateOwnedResult **error_result)
{
	Oid		   *parameter_types = NULL;
	PGresult   *result = NULL;
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	bool		bound = false;
	bool		prepared = false;

	if (description == NULL || error_result == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*description = NULL;
	*error_result = NULL;
	if (!postgamma_private_libpq_valid(connection) || statement_name == NULL ||
		statement_name[0] == '\0' || query == NULL || query[0] == '\0' ||
		parameter_count > INT_MAX ||
		(parameter_count != 0 && parameter_type_oids == NULL) ||
		(deadline_ns != POSTGAMMA_MEMORY_NO_DEADLINE && deadline_ns < 0))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	if (parameter_count != 0)
	{
		parameter_types = calloc(parameter_count, sizeof(*parameter_types));
		if (parameter_types == NULL)
			return POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY;
		for (size_t index = 0; index < parameter_count; index++)
			parameter_types[index] = (Oid) parameter_type_oids[index];
	}
	if (pthread_mutex_lock(&connection->mutex) != 0)
	{
		free(parameter_types);
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	}
	if (connection->active_operation != NULL)
	{
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
		goto done;
	}
	status = postgamma_private_bind(connection);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		goto done;
	bound = true;
	if (!PQsendPrepare(
			connection->postgres_connection, statement_name, query,
			(int) parameter_count, parameter_types))
	{
		status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		goto done;
	}
	status = postgamma_private_next_pgresult(connection, deadline_ns, &result);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		goto done;
	if (postgamma_private_result_status(PQresultStatus(result)) !=
		POSTGAMMA_PRIVATE_RESULT_COMMAND_OK)
	{
		status = postgamma_private_capture_owned_result(
			connection->postgres_connection, result, NULL, error_result);
		PQclear(result);
		result = NULL;
		if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
			status = postgamma_private_drain_to_idle(connection, deadline_ns);
		goto done;
	}
	prepared = true;
	PQclear(result);
	result = NULL;
	status = postgamma_private_drain_to_idle(connection, deadline_ns);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		goto done;
	if (!PQsendDescribePrepared(connection->postgres_connection, statement_name))
	{
		status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
		goto done;
	}
	status = postgamma_private_next_pgresult(connection, deadline_ns, &result);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		goto done;
	if (postgamma_private_result_status(PQresultStatus(result)) !=
		POSTGAMMA_PRIVATE_RESULT_COMMAND_OK)
		status = postgamma_private_capture_owned_result(
			connection->postgres_connection, result, NULL, error_result);
	else
		status = postgamma_private_capture_description(result, description);
	PQclear(result);
	result = NULL;
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		status = postgamma_private_drain_to_idle(connection, deadline_ns);

done:
	if (result != NULL)
		PQclear(result);
	if (bound && prepared && *description == NULL)
	{
		PostgammaPrivateOwnedResult *close_error = NULL;

		(void) postgamma_private_close_prepared_bound(
			connection, statement_name, deadline_ns, &close_error);
		postgamma_private_libpq_result_free(close_error);
	}
	if (bound)
		postgamma_private_restore(connection);
	(void) pthread_mutex_unlock(&connection->mutex);
	free(parameter_types);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_close_prepared(
	PostgammaPrivateLibpqConnection *connection,
	const char *statement_name,
	int64_t deadline_ns,
	PostgammaPrivateOwnedResult **error_result)
{
	PostgammaPrivateLibpqStatus status;

	if (error_result == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*error_result = NULL;
	if (!postgamma_private_libpq_valid(connection) || statement_name == NULL ||
		statement_name[0] == '\0' ||
		(deadline_ns != POSTGAMMA_MEMORY_NO_DEADLINE && deadline_ns < 0))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (connection->active_operation != NULL)
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	else
		status = postgamma_private_bind(connection);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		status = postgamma_private_close_prepared_bound(
			connection, statement_name, deadline_ns, error_result);
		postgamma_private_restore(connection);
	}
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_transaction_status(
	PostgammaPrivateLibpqConnection *connection,
	PostgammaPrivateTransactionStatus *transaction_status,
	int *backend_pid)
{
	if (!postgamma_private_libpq_valid(connection) || transaction_status == NULL ||
		backend_pid == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	*transaction_status = (PostgammaPrivateTransactionStatus) PQtransactionStatus(
		connection->postgres_connection);
	*backend_pid = atomic_load_explicit(
		&connection->backend_pid, memory_order_acquire);
	(void) pthread_mutex_unlock(&connection->mutex);
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_set_notify(
	PostgammaPrivateLibpqConnection *connection,
	PostgammaMemoryNotifyFunction notify, void *notify_argument)
{
	PostgammaMemoryStatus status;

	if (!postgamma_private_libpq_valid(connection))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	status = postgamma_memory_endpoint_set_notify(
		connection->endpoint, connection->generation, notify, notify_argument);
	return status == POSTGAMMA_MEMORY_STATUS_OK ?
		POSTGAMMA_PRIVATE_LIBPQ_OK : POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_consume_idle(
	PostgammaPrivateLibpqConnection *connection)
{
	PostgammaPrivateLibpqStatus status;

	if (!postgamma_private_libpq_valid(connection))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (connection->active_operation != NULL ||
		connection->postgres_connection == NULL)
	{
		(void) pthread_mutex_unlock(&connection->mutex);
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	}
	status = postgamma_private_bind(connection);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
		!PQconsumeInput(connection->postgres_connection))
		status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		postgamma_private_dispatch_notifications(connection);
	postgamma_private_restore(connection);
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_parameter_status(
	PostgammaPrivateLibpqConnection *connection,
	const char *name,
	const char **value)
{
	if (!postgamma_private_libpq_valid(connection) || name == NULL ||
		name[0] == '\0' || value == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	*value = NULL;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (connection->postgres_connection != NULL)
		*value = PQparameterStatus(connection->postgres_connection, name);
	(void) pthread_mutex_unlock(&connection->mutex);
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_copy_in(PostgammaPrivateLibpqConnection *connection,
								const void *data, size_t length,
								int64_t deadline_ns,
								PostgammaPrivateQueryResult *result)
{
	PostgammaPrivateLibpqStatus status;
	PostgammaPrivateQueryResult begin_result;
	PGconn     *postgres;

	if (!postgamma_private_libpq_valid(connection) || data == NULL ||
		length == 0 || length > INT_MAX || deadline_ns < 0 || result == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	memset(result, 0, sizeof(*result));
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	status = postgamma_private_bind(connection);
	postgres = connection->postgres_connection;
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
		!PQsendQuery(postgres, "COPY fixture FROM STDIN"))
		status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		status = postgamma_private_first_result(
			connection, deadline_ns, &begin_result);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
		begin_result.status != POSTGAMMA_PRIVATE_RESULT_COPY_IN)
		status = POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		status = postgamma_private_put_copy_data(
			connection, data, (int) length, deadline_ns);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
		PQputCopyEnd(postgres, NULL) != 1)
		status = POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR;
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		status = postgamma_private_first_result(connection, deadline_ns, result);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		status = postgamma_private_drain_to_idle(connection, deadline_ns);
	if (status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		result->transaction_status = (char) PQtransactionStatus(postgres);
	postgamma_private_restore(connection);
	(void) pthread_mutex_unlock(&connection->mutex);
	return status;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_cancel(PostgammaPrivateLibpqConnection *connection,
								   uint64_t request_generation)
{
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	int			callback_status;
	int			backend_pid;

	if (!postgamma_private_libpq_valid(connection) || request_generation == 0)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	backend_pid = atomic_load_explicit(
		&connection->backend_pid, memory_order_acquire);
	if (backend_pid <= 0)
		status = POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR;
	else
	{
		callback_status = connection->cancel_callback(
			connection->cancel_argument,
			connection->generation,
			request_generation,
			backend_pid);
		if (callback_status != 0)
			status = POSTGAMMA_PRIVATE_LIBPQ_CANCELLED;
		else
			(void) atomic_fetch_add_explicit(
				&connection->cancel_dispatches, UINT64_C(1),
				memory_order_relaxed);
	}
	return status;
}


uint64_t
postgamma_private_libpq_active_request_generation(
	const PostgammaPrivateLibpqConnection *connection)
{
	if (!postgamma_private_libpq_valid(connection))
		return 0;
	return atomic_load_explicit(
		&connection->active_request_generation, memory_order_acquire);
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_telemetry(PostgammaPrivateLibpqConnection *connection,
								   PostgammaPrivateLibpqTelemetry *telemetry)
{
	if (!postgamma_private_libpq_valid(connection) || telemetry == NULL)
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	if (pthread_mutex_lock(&connection->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	*telemetry = (PostgammaPrivateLibpqTelemetry) {
		.generation = connection->generation,
		.secure_read_calls = connection->secure_read_calls,
		.secure_write_calls = connection->secure_write_calls,
		.socket_wait_calls = connection->socket_wait_calls,
		.bytes_read = connection->bytes_read,
		.bytes_written = connection->bytes_written,
		.notice_count = connection->notice_count,
		.cancel_dispatches = atomic_load_explicit(
			&connection->cancel_dispatches, memory_order_relaxed),
		.network_connect_calls = 0,
		.optional_security_calls = 0,
		.backend_pid = atomic_load_explicit(
			&connection->backend_pid, memory_order_acquire),
		.connection_status = connection->postgres_connection != NULL ?
			PQstatus(connection->postgres_connection) : CONNECTION_BAD,
	};
	postgamma_private_copy(
		telemetry->last_notice_sqlstate,
		sizeof(telemetry->last_notice_sqlstate),
		connection->last_notice_sqlstate);
	postgamma_private_copy(
		telemetry->last_notice_message,
		sizeof(telemetry->last_notice_message),
		connection->last_notice_message);
	postgamma_private_copy(
		telemetry->last_error, sizeof(telemetry->last_error),
		connection->postgres_connection != NULL ?
			PQerrorMessage(connection->postgres_connection) : "");
	(void) pthread_mutex_unlock(&connection->mutex);
	return POSTGAMMA_PRIVATE_LIBPQ_OK;
}


PostgammaPrivateLibpqStatus
postgamma_private_libpq_close(PostgammaPrivateLibpqConnection **connection)
{
	PostgammaPrivateLibpqConnection *closing;
	PostgammaPrivateLibpqStatus status = POSTGAMMA_PRIVATE_LIBPQ_OK;

	if (connection == NULL || !postgamma_private_libpq_valid(*connection))
		return POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT;
	closing = *connection;
	if (pthread_mutex_lock(&closing->mutex) != 0)
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	if (postgamma_private_bind(closing) != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		(void) pthread_mutex_unlock(&closing->mutex);
		return POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	}
	PQfinish(closing->postgres_connection);
	closing->postgres_connection = NULL;
	atomic_store_explicit(
		&closing->backend_pid, 0, memory_order_release);
	postgamma_private_restore(closing);
	if (closing->endpoint != NULL)
	{
		PostgammaMemoryStatus transport_status =
			postgamma_memory_endpoint_half_close_write(
				closing->endpoint, closing->generation);

		if (transport_status != POSTGAMMA_MEMORY_STATUS_OK &&
			transport_status != POSTGAMMA_MEMORY_STATUS_LOCAL_CLOSED &&
			status == POSTGAMMA_PRIVATE_LIBPQ_OK)
			status = POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
		transport_status = postgamma_memory_endpoint_release(
			&closing->endpoint, closing->generation);
		if (transport_status != POSTGAMMA_MEMORY_STATUS_OK &&
			status == POSTGAMMA_PRIVATE_LIBPQ_OK)
			status = POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR;
	}
	(void) pthread_mutex_unlock(&closing->mutex);
	if (pthread_mutex_destroy(&closing->mutex) != 0 &&
		status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		status = POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION;
	closing->magic = 0;
	free(closing);
	*connection = NULL;
	return status;
}


const char *
postgamma_private_libpq_status_name(PostgammaPrivateLibpqStatus status)
{
	switch (status)
	{
		case POSTGAMMA_PRIVATE_LIBPQ_OK:
			return "ok";
		case POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT:
			return "invalid-argument";
		case POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY:
			return "no-memory";
		case POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT:
			return "timeout";
		case POSTGAMMA_PRIVATE_LIBPQ_CANCELLED:
			return "cancelled";
		case POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR:
			return "transport-error";
		case POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR:
			return "protocol-error";
		case POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR:
			return "upstream-error";
		case POSTGAMMA_PRIVATE_LIBPQ_COPY_TERMINATED:
			return "copy-terminated";
		case POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION:
			return "contract-violation";
	}
	return "unknown";
}
