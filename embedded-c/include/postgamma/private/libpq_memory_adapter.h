/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_LIBPQ_MEMORY_ADAPTER_H
#define POSTGAMMA_PRIVATE_LIBPQ_MEMORY_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "postgamma/private/memory_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PostgammaPrivateLibpqStatus
{
	POSTGAMMA_PRIVATE_LIBPQ_OK = 0,
	POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT,
	POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY,
	POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT,
	POSTGAMMA_PRIVATE_LIBPQ_CANCELLED,
	POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR,
	POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR,
	POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR,
	POSTGAMMA_PRIVATE_LIBPQ_COPY_TERMINATED,
	POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION
} PostgammaPrivateLibpqStatus;

typedef enum PostgammaPrivateResultStatus
{
	POSTGAMMA_PRIVATE_RESULT_NONE = 0,
	POSTGAMMA_PRIVATE_RESULT_EMPTY_QUERY,
	POSTGAMMA_PRIVATE_RESULT_COMMAND_OK,
	POSTGAMMA_PRIVATE_RESULT_TUPLES_OK,
	POSTGAMMA_PRIVATE_RESULT_COPY_OUT,
	POSTGAMMA_PRIVATE_RESULT_COPY_IN,
	POSTGAMMA_PRIVATE_RESULT_COPY_BOTH,
	POSTGAMMA_PRIVATE_RESULT_BAD_RESPONSE,
	POSTGAMMA_PRIVATE_RESULT_NONFATAL_ERROR,
	POSTGAMMA_PRIVATE_RESULT_FATAL_ERROR,
	POSTGAMMA_PRIVATE_RESULT_SINGLE_TUPLE,
	POSTGAMMA_PRIVATE_RESULT_TUPLES_CHUNK,
	POSTGAMMA_PRIVATE_RESULT_PIPELINE_SYNC,
	POSTGAMMA_PRIVATE_RESULT_PIPELINE_ABORTED
} PostgammaPrivateResultStatus;

typedef struct PostgammaPrivateLibpqConnection
	PostgammaPrivateLibpqConnection;
typedef struct PostgammaPrivateLibpqOperation
	PostgammaPrivateLibpqOperation;

typedef enum PostgammaPrivateLibpqProgress
{
	POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_WAITING = 0,
	POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_RESULT_READY,
	POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_COMPLETE
} PostgammaPrivateLibpqProgress;

typedef enum PostgammaPrivateCopyDirection
{
	POSTGAMMA_PRIVATE_COPY_NONE = 0,
	POSTGAMMA_PRIVATE_COPY_IN,
	POSTGAMMA_PRIVATE_COPY_OUT
} PostgammaPrivateCopyDirection;

typedef enum PostgammaPrivateCopyProgress
{
	POSTGAMMA_PRIVATE_COPY_PROGRESS = 0,
	POSTGAMMA_PRIVATE_COPY_AGAIN,
	POSTGAMMA_PRIVATE_COPY_END
} PostgammaPrivateCopyProgress;

typedef enum PostgammaPrivateTransactionStatus
{
	POSTGAMMA_PRIVATE_TRANSACTION_IDLE = 0,
	POSTGAMMA_PRIVATE_TRANSACTION_ACTIVE,
	POSTGAMMA_PRIVATE_TRANSACTION_INTRANS,
	POSTGAMMA_PRIVATE_TRANSACTION_INERROR,
	POSTGAMMA_PRIVATE_TRANSACTION_UNKNOWN
} PostgammaPrivateTransactionStatus;

typedef int (*PostgammaPrivateCancelCallback) (
	void *argument,
	uint64_t connection_generation,
	uint64_t request_generation,
	int backend_pid);
typedef void (*PostgammaPrivateNoticeCallback) (
	void *argument,
	const char *sqlstate,
	const char *severity,
	const char *message,
	const char *detail,
	const char *hint);
typedef void (*PostgammaPrivateNotificationCallback) (
	void *argument,
	int backend_pid,
	const char *channel,
	const char *payload);

typedef struct PostgammaPrivateLibpqOptions
{
	uint64_t	generation;
	size_t		queue_capacity;
	const char *user;
	const char *database;
	const char *application_name;
	const char *startup_options;
	PostgammaPrivateCancelCallback cancel_callback;
	void       *cancel_argument;
	PostgammaPrivateNoticeCallback notice_callback;
	void       *notice_argument;
	PostgammaPrivateNotificationCallback notification_callback;
	void       *notification_argument;
} PostgammaPrivateLibpqOptions;

typedef struct PostgammaPrivateQueryResult
{
	PostgammaPrivateResultStatus status;
	int			rows;
	int			columns;
	char		value[64];
	char		command_status[64];
	char		sqlstate[6];
	char		severity[32];
	char		message[256];
	char		detail[256];
	char		transaction_status;
} PostgammaPrivateQueryResult;

typedef struct PostgammaPrivateParameter
{
	uint32_t	type_oid;
	uint16_t	format;
	bool		is_null;
	const void *data;
	size_t		length;
} PostgammaPrivateParameter;

typedef struct PostgammaPrivateResultPolicy
{
	uint32_t	delivery_mode;
	uint32_t	target_chunk_rows;
	size_t		result_buffer_limit;
	size_t		maximum_value_size;
} PostgammaPrivateResultPolicy;

#define POSTGAMMA_PRIVATE_RESULT_POLICY_INIT \
	{POSTGAMMA_MEMORY_DELIVERY_MATERIALIZED, UINT32_C(0), 0, 0}

typedef struct PostgammaPrivateResultField
{
	char	   *name;
	uint32_t	table_oid;
	int32_t		table_column;
	uint32_t	type_oid;
	int32_t		type_size;
	int32_t		type_modifier;
	uint16_t	format;
} PostgammaPrivateResultField;

typedef struct PostgammaPrivateResultValue
{
	bool		is_null;
	size_t		length;
	unsigned char *data;
} PostgammaPrivateResultValue;

typedef struct PostgammaPrivateOwnedResult
{
	PostgammaPrivateResultStatus status;
	size_t		rows;
	size_t		columns;
	PostgammaPrivateResultField *fields;
	PostgammaPrivateResultValue *values;
	char		command_status[64];
	char		sqlstate[6];
	char		severity[32];
	char		message[256];
	char		detail[256];
	char	   *diagnostics[17];
	char		transaction_status;
} PostgammaPrivateOwnedResult;

typedef struct PostgammaPrivatePreparedDescription
{
	size_t		parameter_count;
	uint32_t   *parameter_types;
	size_t		column_count;
	PostgammaPrivateResultField *columns;
} PostgammaPrivatePreparedDescription;

typedef struct PostgammaPrivateLibpqTelemetry
{
	uint64_t	generation;
	uint64_t	secure_read_calls;
	uint64_t	secure_write_calls;
	uint64_t	socket_wait_calls;
	uint64_t	bytes_read;
	uint64_t	bytes_written;
	uint64_t	notice_count;
	uint64_t	cancel_dispatches;
	uint64_t	network_connect_calls;
	uint64_t	optional_security_calls;
	int			backend_pid;
	int			connection_status;
	char		last_notice_sqlstate[6];
	char		last_notice_message[128];
	char		last_error[256];
} PostgammaPrivateLibpqTelemetry;

PostgammaPrivateLibpqStatus postgamma_private_libpq_create(
	const PostgammaPrivateLibpqOptions *options,
	PostgammaPrivateLibpqConnection **connection,
	PostgammaMemoryEndpoint **backend_endpoint);
PostgammaPrivateLibpqStatus postgamma_private_libpq_connect(
	PostgammaPrivateLibpqConnection *connection,
	int64_t deadline_ns);
/*
 * Borrow the private PGconn for one transformed frontend-tool invocation.
 * The borrow owns the connection mutex and memory-transport TLS binding until
 * the same thread releases it.  The native pointer never crosses a public ABI.
 */
PostgammaPrivateLibpqStatus postgamma_private_libpq_tool_borrow(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	void **native_connection);
PostgammaPrivateLibpqStatus postgamma_private_libpq_tool_release(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation);
PostgammaPrivateLibpqStatus postgamma_private_libpq_query(
	PostgammaPrivateLibpqConnection *connection,
	const char *query,
	int64_t deadline_ns,
	PostgammaPrivateQueryResult *result);
PostgammaPrivateLibpqStatus postgamma_private_libpq_query_params(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *query,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	int64_t deadline_ns,
	PostgammaPrivateOwnedResult **result);
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_start(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *query,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	PostgammaPrivateLibpqOperation **operation);
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_start_ex(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *query,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	const PostgammaPrivateResultPolicy *result_policy,
	PostgammaPrivateLibpqOperation **operation);
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_start_script(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *query,
	PostgammaPrivateLibpqOperation **operation);
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_start_script_ex(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *query,
	const PostgammaPrivateResultPolicy *result_policy,
	PostgammaPrivateLibpqOperation **operation);
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_start_prepared(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *statement_name,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	PostgammaPrivateLibpqOperation **operation);
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_start_prepared_ex(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation,
	const char *statement_name,
	const PostgammaPrivateParameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	const PostgammaPrivateResultPolicy *result_policy,
	PostgammaPrivateLibpqOperation **operation);
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_progress(
	PostgammaPrivateLibpqOperation *operation,
	PostgammaPrivateLibpqProgress *progress,
	PostgammaPrivateOwnedResult **result);
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_wait(
	PostgammaPrivateLibpqOperation *operation,
	int64_t deadline_ns);
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_ready(
	PostgammaPrivateLibpqOperation *operation,
	bool *ready);
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_direction(
	PostgammaPrivateLibpqOperation *operation,
	PostgammaPrivateCopyDirection *direction);
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_pump(
	PostgammaPrivateLibpqOperation *operation,
	PostgammaPrivateCopyProgress *progress);
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_write(
	PostgammaPrivateLibpqOperation *operation,
	const void *data,
	size_t size,
	size_t *consumed,
	PostgammaPrivateCopyProgress *progress);
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_finish(
	PostgammaPrivateLibpqOperation *operation,
	const char *failure_message,
	PostgammaPrivateCopyProgress *progress);
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_read(
	PostgammaPrivateLibpqOperation *operation,
	void *buffer,
	size_t capacity,
	size_t *produced,
	PostgammaPrivateCopyProgress *progress);
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_release(
	PostgammaPrivateLibpqOperation *operation);
void postgamma_private_libpq_operation_free(
	PostgammaPrivateLibpqOperation **operation);
PostgammaPrivateLibpqStatus postgamma_private_libpq_prepare(
	PostgammaPrivateLibpqConnection *connection,
	const char *statement_name,
	const char *query,
	const uint32_t *parameter_type_oids,
	size_t parameter_count,
	int64_t deadline_ns,
	PostgammaPrivatePreparedDescription **description,
	PostgammaPrivateOwnedResult **error_result);
PostgammaPrivateLibpqStatus postgamma_private_libpq_close_prepared(
	PostgammaPrivateLibpqConnection *connection,
	const char *statement_name,
	int64_t deadline_ns,
	PostgammaPrivateOwnedResult **error_result);
void postgamma_private_libpq_prepared_description_free(
	PostgammaPrivatePreparedDescription *description);
PostgammaPrivateLibpqStatus postgamma_private_libpq_transaction_status(
	PostgammaPrivateLibpqConnection *connection,
	PostgammaPrivateTransactionStatus *transaction_status,
	int *backend_pid);
PostgammaPrivateLibpqStatus postgamma_private_libpq_set_notify(
	PostgammaPrivateLibpqConnection *connection,
	PostgammaMemoryNotifyFunction notify,
	void *notify_argument);
PostgammaPrivateLibpqStatus postgamma_private_libpq_consume_idle(
	PostgammaPrivateLibpqConnection *connection);
PostgammaPrivateLibpqStatus postgamma_private_libpq_parameter_status(
	PostgammaPrivateLibpqConnection *connection,
	const char *name,
	const char **value);
void postgamma_private_libpq_result_free(
	PostgammaPrivateOwnedResult *result);
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_in(
	PostgammaPrivateLibpqConnection *connection,
	const void *data,
	size_t length,
	int64_t deadline_ns,
	PostgammaPrivateQueryResult *result);
PostgammaPrivateLibpqStatus postgamma_private_libpq_cancel(
	PostgammaPrivateLibpqConnection *connection,
	uint64_t request_generation);
uint64_t postgamma_private_libpq_active_request_generation(
	const PostgammaPrivateLibpqConnection *connection);
PostgammaPrivateLibpqStatus postgamma_private_libpq_telemetry(
	PostgammaPrivateLibpqConnection *connection,
	PostgammaPrivateLibpqTelemetry *telemetry);
PostgammaPrivateLibpqStatus postgamma_private_libpq_close(
	PostgammaPrivateLibpqConnection **connection);
const char *postgamma_private_libpq_status_name(
	PostgammaPrivateLibpqStatus status);

#ifdef __cplusplus
}
#endif

#endif
