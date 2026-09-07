/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "postgamma/private/public_runtime.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>


#define PGM_PROTOCOL_OPERATION_TIMEOUT_NS INT64_C(30000000000)

_Static_assert(
	PGM_TRANSACTION_IDLE == POSTGAMMA_PRIVATE_TRANSACTION_IDLE,
	"public and private idle transaction states must match");
_Static_assert(
	PGM_TRANSACTION_ACTIVE == POSTGAMMA_PRIVATE_TRANSACTION_ACTIVE,
	"public and private active transaction states must match");
_Static_assert(
	PGM_TRANSACTION_INTRANS == POSTGAMMA_PRIVATE_TRANSACTION_INTRANS,
	"public and private in-transaction states must match");
_Static_assert(
	PGM_TRANSACTION_INERROR == POSTGAMMA_PRIVATE_TRANSACTION_INERROR,
	"public and private failed-transaction states must match");
_Static_assert(
	PGM_TRANSACTION_UNKNOWN == POSTGAMMA_PRIVATE_TRANSACTION_UNKNOWN,
	"public and private unknown transaction states must match");
_Static_assert(
	PGM_PIN_TRANSACTION == POSTGAMMA_KERNEL_PIN_TRANSACTION,
	"public and kernel transaction pin bits must match");
_Static_assert(
	PGM_PIN_ADVISORY_LOCK == POSTGAMMA_KERNEL_PIN_ADVISORY_LOCK,
	"public and kernel advisory-lock pin bits must match");
_Static_assert(
	PGM_PIN_PORTAL == POSTGAMMA_KERNEL_PIN_PORTAL,
	"public and kernel portal pin bits must match");
_Static_assert(
	PGM_PIN_COPY == POSTGAMMA_KERNEL_PIN_COPY,
	"public and kernel COPY pin bits must match");
_Static_assert(
	PGM_PIN_OTHER == POSTGAMMA_KERNEL_PIN_OTHER,
	"public and kernel fallback pin bits must match");


static pgm_status validate_execute_options(
	const pgm_execute_options *options, pgm_error **error);
static pgm_status validate_script_options(
	const pgm_script_options *options, pgm_error **error);
static pgm_status transfer_ready_result_locked(
	pgm_request *request, pgm_result **result,
	pgm_availability *availability, pgm_error **error);
static void fill_public_column(
	const PostgammaPrivateResultField *source, pgm_column *column);


pgm_status
pgm_connection_transaction_status(
	pgm_connection *connection,
	pgm_transaction_status *transaction_status,
	pgm_error **error)
{
	PostgammaPrivateTransactionStatus private_transaction;
	PostgammaPrivateLibpqStatus private_status;
	int			backend_pid;

	if (error != NULL)
		*error = NULL;
	if (!connection_is_valid(connection) || transaction_status == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid connection transaction status arguments");
	if (!process_is_valid(connection->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"connection belongs to a different host process");
	if (public_instance_reentrant(connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	private_status = postgamma_private_libpq_transaction_status(
		connection->private_connection, &private_transaction, &backend_pid);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK ||
		backend_pid != connection->backend_pid)
		return return_simple_error(
			error, private_status == POSTGAMMA_PRIVATE_LIBPQ_OK ?
			PGM_STATUS_CONNECTION_FAILED : status_from_private(private_status),
			"could not inspect the PostgreSQL transaction state");
	*transaction_status = (pgm_transaction_status) private_transaction;
	return PGM_STATUS_OK;
}


pgm_status
pgm_connection_get_status(
	pgm_connection *connection,
	pgm_connection_status_snapshot *snapshot,
	pgm_error **error)
{
	PostgammaMemoryTelemetry telemetry;
	pgm_transaction_status transaction_status;
	pgm_request_id active_request_id = 0;
	pgm_status	public_status;
	int			status;

	if (error != NULL)
		*error = NULL;
	if (!connection_is_valid(connection) || snapshot == NULL ||
		snapshot->struct_size < sizeof(*snapshot))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid connection status arguments");
	if (!process_is_valid(connection->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"connection belongs to a different host process");
	if (public_instance_reentrant(connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	public_status = pgm_connection_transaction_status(
		connection, &transaction_status, error);
	if (public_status != PGM_STATUS_OK)
		return public_status;
	if (postgamma_memory_endpoint_snapshot(
			connection->backend_monitor, connection->instance->generation,
			&telemetry) != POSTGAMMA_MEMORY_STATUS_OK)
		return return_simple_error(
			error, PGM_STATUS_CONNECTION_FAILED,
			"could not inspect the embedded session transport");
	status = postgamma_mutex_lock(connection->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the embedded connection");
	if (connection->active_request != NULL)
		active_request_id = connection->active_request->generation;
	(void) postgamma_mutex_unlock(connection->mutex);
	*snapshot = (pgm_connection_status_snapshot)
		PGM_CONNECTION_STATUS_SNAPSHOT_INIT;
	snapshot->transaction_status = transaction_status;
	snapshot->pin_reasons = telemetry.pin_reasons;
	snapshot->carrier_retained = telemetry.carrier_retained != 0 ||
		transaction_status == PGM_TRANSACTION_ACTIVE ? UINT32_C(1) : UINT32_C(0);
	snapshot->virtual_backend_pid = connection->backend_pid;
	snapshot->connection_id = connection->identity;
	snapshot->active_request_id = active_request_id;
	return PGM_STATUS_OK;
}


pgm_connection_id
pgm_connection_identity(const pgm_connection *connection)
{
	return connection_is_valid(connection) &&
		process_is_valid(connection->owner_pid) &&
		!public_instance_reentrant(connection->instance) ?
		connection->identity : 0;
}


pgm_status
pgm_connection_parameter_status(
	pgm_connection *connection,
	const char *name,
	const char **value,
	pgm_error **error)
{
	PostgammaPrivateLibpqStatus private_status;

	if (error != NULL)
		*error = NULL;
	if (value != NULL)
		*value = NULL;
	if (!connection_is_valid(connection) || name == NULL || name[0] == '\0' ||
		value == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid connection parameter-status arguments");
	if (!process_is_valid(connection->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"connection belongs to a different host process");
	if (public_instance_reentrant(connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	private_status = postgamma_private_libpq_parameter_status(
		connection->private_connection, name, value);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		return return_simple_error(
			error, status_from_private(private_status),
			"could not read the PostgreSQL parameter status");
	return PGM_STATUS_OK;
}


pgm_status
pgm_execute_async_ex(
	pgm_connection *connection,
	const char *sql,
	size_t sql_size,
	const pgm_execute_options *options,
	pgm_request **request,
	pgm_error **error)
{
	char	   *terminated_sql;
	pgm_status	status;

	if (error != NULL)
		*error = NULL;
	if (request == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"request output parameter is required");
	*request = NULL;
	if (!connection_is_valid(connection))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid embedded connection");
	status = validate_execute_options(options, error);
	if (status != PGM_STATUS_OK)
		return status;
	terminated_sql = duplicate_sql(sql, sql_size);
	if (terminated_sql == NULL)
		return return_simple_error(
			error, errno == ENOMEM ? PGM_STATUS_OUT_OF_MEMORY :
			PGM_STATUS_INVALID_ARGUMENT,
			"SQL must be nonempty, explicitly sized, and contain no NUL byte");
	status = request_start(
		connection, POSTGAMMA_PUBLIC_REQUEST_EXTENDED, NULL, terminated_sql,
		options->parameters, options->parameter_count, options->result_format,
		options->delivery_mode, options->target_chunk_rows,
		options->result_buffer_limit,
		options->notice_callback, options->notice_user_data,
		request, error);
	free(terminated_sql);
	return status;
}


pgm_status
pgm_execute_script_async(
	pgm_connection *connection,
	const char *sql,
	size_t sql_size,
	const pgm_script_options *options,
	pgm_request **request,
	pgm_error **error)
{
	char	   *terminated_sql;
	pgm_status	status;

	if (error != NULL)
		*error = NULL;
	if (request == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"request output parameter is required");
	*request = NULL;
	if (!connection_is_valid(connection))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid embedded connection");
	status = validate_script_options(options, error);
	if (status != PGM_STATUS_OK)
		return status;
	terminated_sql = duplicate_sql(sql, sql_size);
	if (terminated_sql == NULL)
		return return_simple_error(
			error, errno == ENOMEM ? PGM_STATUS_OUT_OF_MEMORY :
			PGM_STATUS_INVALID_ARGUMENT,
			"SQL must be nonempty, explicitly sized, and contain no NUL byte");
	status = request_start(
		connection, POSTGAMMA_PUBLIC_REQUEST_SCRIPT, NULL, terminated_sql,
		NULL, 0, PGM_FORMAT_TEXT, options->delivery_mode,
		options->target_chunk_rows, options->result_buffer_limit,
		options->notice_callback, options->notice_user_data,
		request, error);
	free(terminated_sql);
	return status;
}


pgm_status
pgm_statement_prepare(
	pgm_connection *connection,
	const char *sql,
	size_t sql_size,
	const pgm_prepare_options *options,
	pgm_statement **statement,
	pgm_error **error)
{
	PostgammaPrivatePreparedDescription *description = NULL;
	PostgammaPrivateOwnedResult *private_error = NULL;
	PostgammaPrivateLibpqStatus private_status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	pgm_statement *created = NULL;
	char	   *terminated_sql = NULL;
	uint64_t	statement_id;
	int64_t		deadline_ns;
	int			status = 0;

	if (error != NULL)
		*error = NULL;
	if (statement == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"statement output parameter is required");
	*statement = NULL;
	if (!connection_is_valid(connection) || options == NULL ||
		options->struct_size < sizeof(*options) || options->flags != 0 ||
		options->parameter_count > INT_MAX ||
		(options->parameter_count != 0 &&
		 options->parameter_type_oids == NULL))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid prepared statement arguments");
	if (!process_is_valid(connection->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"connection belongs to a different host process");
	if (public_instance_reentrant(connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	terminated_sql = duplicate_sql(sql, sql_size);
	if (terminated_sql == NULL)
		return return_simple_error(
			error, errno == ENOMEM ? PGM_STATUS_OUT_OF_MEMORY :
			PGM_STATUS_INVALID_ARGUMENT,
			"SQL must be nonempty, explicitly sized, and contain no NUL byte");
	status = postgamma_mutex_lock(connection->mutex);
	if (status != 0)
		goto fail;
	if (connection->closing || connection->failed ||
		connection->active_request != NULL)
	{
		(void) postgamma_mutex_unlock(connection->mutex);
		free(terminated_sql);
		return return_simple_error(
			error, connection->failed ? PGM_STATUS_CONNECTION_FAILED :
			PGM_STATUS_BUSY,
			"connection is not idle for statement preparation");
	}
	created = calloc(1, sizeof(*created));
	if (created == NULL)
	{
		status = ENOMEM;
		goto fail_locked;
	}
	created->magic = PGM_STATEMENT_MAGIC;
	created->owner_pid = connection->owner_pid;
	created->connection = connection;
	statement_id = atomic_fetch_add_explicit(
		&connection->instance->next_statement_id, UINT64_C(1),
		memory_order_relaxed);
	if (statement_id == 0 || statement_id == UINT64_MAX)
	{
		status = EOVERFLOW;
		goto fail_locked;
	}
	created->name = malloc(64);
	if (created->name == NULL)
	{
		status = ENOMEM;
		goto fail_locked;
	}
	(void) snprintf(
		created->name, 64, "pgm_%" PRIu64 "_%" PRIu64,
		(uint64_t) connection->identity, statement_id);
	status = private_deadline_after(
		PGM_PROTOCOL_OPERATION_TIMEOUT_NS, &deadline_ns);
	if (status != 0)
		goto fail_locked;
	private_status = postgamma_private_libpq_prepare(
		connection->private_connection, created->name, terminated_sql,
		options->parameter_type_oids, options->parameter_count, deadline_ns,
		&description, &private_error);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK || private_error != NULL ||
		description == NULL)
	{
		status = private_status == POSTGAMMA_PRIVATE_LIBPQ_OK ? EINVAL : EPROTO;
		goto fail_locked;
	}
	created->description = description;
	description = NULL;
	connection->statement_count++;
	created->registered_with_connection = true;
	(void) postgamma_mutex_unlock(connection->mutex);
	free(terminated_sql);
	*statement = created;
	return PGM_STATUS_OK;

fail_locked:
	(void) postgamma_mutex_unlock(connection->mutex);
fail:
	free(terminated_sql);
	postgamma_private_libpq_prepared_description_free(description);
	if (private_error != NULL)
	{
		return_error(error, error_from_result(
			PGM_STATUS_POSTGRES_ERROR, private_error));
		postgamma_private_libpq_result_free(private_error);
	}
	else
		return_error(error, make_error(
			private_status != POSTGAMMA_PRIVATE_LIBPQ_OK ?
			status_from_private(private_status) :
			status == ENOMEM ? PGM_STATUS_OUT_OF_MEMORY :
			PGM_STATUS_INTERNAL_ERROR,
			NULL, NULL, NULL, "could not prepare the embedded statement"));
	if (created != NULL)
	{
		free(created->name);
		created->magic = 0;
		free(created);
	}
	if (private_error != NULL)
		return PGM_STATUS_POSTGRES_ERROR;
	return private_status != POSTGAMMA_PRIVATE_LIBPQ_OK ?
		status_from_private(private_status) :
		status == ENOMEM ? PGM_STATUS_OUT_OF_MEMORY : PGM_STATUS_INTERNAL_ERROR;
}


pgm_status
pgm_statement_execute_async(
	pgm_statement *statement,
	const pgm_execute_options *options,
	pgm_request **request,
	pgm_error **error)
{
	pgm_status	status;

	if (error != NULL)
		*error = NULL;
	if (request == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"request output parameter is required");
	*request = NULL;
	if (!statement_is_valid(statement))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid prepared statement");
	if (!process_is_valid(statement->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"statement belongs to a different host process");
	status = validate_execute_options(options, error);
	if (status != PGM_STATUS_OK)
		return status;
	if (options->parameter_count != statement->description->parameter_count)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"prepared statement parameter count does not match its description");
	return request_start(
		statement->connection, POSTGAMMA_PUBLIC_REQUEST_PREPARED, statement,
		statement->name, options->parameters, options->parameter_count,
		options->result_format, options->delivery_mode,
		options->target_chunk_rows, options->result_buffer_limit,
		options->notice_callback, options->notice_user_data,
		request, error);
}


pgm_status
pgm_statement_describe(
	const pgm_statement *statement,
	pgm_statement_description *description,
	pgm_error **error)
{
	int			status;

	if (error != NULL)
		*error = NULL;
	if (!statement_is_valid(statement) || description == NULL ||
		description->struct_size < sizeof(*description))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid statement description arguments");
	if (!process_is_valid(statement->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"statement belongs to a different host process");
	if (public_instance_reentrant(statement->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = postgamma_mutex_lock(statement->connection->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the prepared statement connection");
	*description = (pgm_statement_description)
		PGM_STATEMENT_DESCRIPTION_INIT;
	description->parameter_count = statement->description->parameter_count;
	description->column_count = statement->description->column_count;
	(void) postgamma_mutex_unlock(statement->connection->mutex);
	return PGM_STATUS_OK;
}


pgm_status
pgm_statement_parameter_type(
	const pgm_statement *statement,
	size_t parameter_index,
	uint32_t *type_oid,
	pgm_error **error)
{
	int			status;

	if (error != NULL)
		*error = NULL;
	if (!statement_is_valid(statement) || type_oid == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid statement parameter type arguments");
	if (!process_is_valid(statement->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"statement belongs to a different host process");
	if (public_instance_reentrant(statement->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = postgamma_mutex_lock(statement->connection->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the prepared statement connection");
	if (parameter_index >= statement->description->parameter_count)
	{
		(void) postgamma_mutex_unlock(statement->connection->mutex);
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"prepared statement parameter index is out of range");
	}
	*type_oid = statement->description->parameter_types[parameter_index];
	(void) postgamma_mutex_unlock(statement->connection->mutex);
	return PGM_STATUS_OK;
}


pgm_status
pgm_statement_column(
	const pgm_statement *statement,
	size_t column_index,
	pgm_column *column,
	pgm_error **error)
{
	int			status;

	if (error != NULL)
		*error = NULL;
	if (!statement_is_valid(statement) || column == NULL ||
		column->struct_size < sizeof(*column))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid statement column arguments");
	if (!process_is_valid(statement->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"statement belongs to a different host process");
	if (public_instance_reentrant(statement->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = postgamma_mutex_lock(statement->connection->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the prepared statement connection");
	if (column_index >= statement->description->column_count)
	{
		(void) postgamma_mutex_unlock(statement->connection->mutex);
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"prepared statement column index is out of range");
	}
	fill_public_column(&statement->description->columns[column_index], column);
	(void) postgamma_mutex_unlock(statement->connection->mutex);
	return PGM_STATUS_OK;
}


pgm_status
pgm_statement_close(
	pgm_statement *statement,
	pgm_error **error)
{
	PostgammaPrivateOwnedResult *private_error = NULL;
	PostgammaPrivateLibpqStatus private_status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	pgm_connection *connection;
	int64_t		deadline_ns;
	int			status;

	if (error != NULL)
		*error = NULL;
	if (!statement_is_valid(statement))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid prepared statement");
	if (!process_is_valid(statement->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"statement belongs to a different host process");
	connection = statement->connection;
	if (public_instance_reentrant(connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = postgamma_mutex_lock(connection->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the prepared statement connection");
	if (statement->active_request != NULL || connection->active_request != NULL)
	{
		(void) postgamma_mutex_unlock(connection->mutex);
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"free the active request before closing its statement");
	}
	if (!connection->failed)
	{
		status = private_deadline_after(
			PGM_PROTOCOL_OPERATION_TIMEOUT_NS, &deadline_ns);
		if (status == 0)
			private_status = postgamma_private_libpq_close_prepared(
				connection->private_connection, statement->name, deadline_ns,
				&private_error);
		if (status != 0 || private_status != POSTGAMMA_PRIVATE_LIBPQ_OK ||
			private_error != NULL)
		{
			(void) postgamma_mutex_unlock(connection->mutex);
			if (private_error != NULL)
			{
				return_error(error, error_from_result(
					PGM_STATUS_POSTGRES_ERROR, private_error));
				postgamma_private_libpq_result_free(private_error);
				return PGM_STATUS_POSTGRES_ERROR;
			}
			return return_simple_error(
				error, private_status != POSTGAMMA_PRIVATE_LIBPQ_OK ?
				status_from_private(private_status) : PGM_STATUS_INTERNAL_ERROR,
				"could not close the prepared statement");
		}
	}
	if (!statement->registered_with_connection ||
		connection->statement_count == 0)
	{
		(void) postgamma_mutex_unlock(connection->mutex);
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"prepared statement ownership is inconsistent");
	}
	connection->statement_count--;
	statement->registered_with_connection = false;
	statement->magic = 0;
	(void) postgamma_mutex_unlock(connection->mutex);
	postgamma_private_libpq_prepared_description_free(statement->description);
	free(statement->name);
	free(statement);
	return PGM_STATUS_OK;
}


pgm_status
pgm_request_next_result(
	pgm_request *request,
	int64_t timeout_ms,
	pgm_result **result,
	pgm_availability *availability,
	pgm_error **error)
{
	uint64_t	deadline_ns;
	int			status;

	if (result != NULL)
		*result = NULL;
	if (error != NULL)
		*error = NULL;
	if (!request_is_valid(request) || result == NULL || availability == NULL ||
		timeout_ms < PGM_NO_TIMEOUT)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid next-result arguments");
	if (!process_is_valid(request->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"request belongs to a different host process");
	if (public_instance_reentrant(request->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	if (atomic_load_explicit(
			&request->result_lease->outstanding, memory_order_acquire))
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"free the outstanding result before requesting another");
	status = deadline_from_timeout(timeout_ms, &deadline_ns);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid next-result timeout");
	for (;;)
	{
		PostgammaPrivateLibpqStatus private_status;
		pgm_status	progress_status;
		int64_t		private_deadline;
		status = postgamma_mutex_lock(request->mutex);
		if (status != 0)
			return return_simple_error(
				error, PGM_STATUS_INTERNAL_ERROR,
				"could not lock the embedded request");
		progress_status = progress_request_locked(request);
		if (progress_status != PGM_STATUS_OK)
		{
			(void) postgamma_mutex_unlock(request->mutex);
			postgamma_public_dispatch_progress_events(request);
			return return_simple_error(
				error, progress_status,
				"could not progress the embedded request");
		}
		if (request->result != NULL)
		{
			pgm_status transfer_status = transfer_ready_result_locked(
				request, result, availability, error);

			(void) postgamma_mutex_unlock(request->mutex);
			postgamma_public_dispatch_progress_events(request);
			if (transfer_status != PGM_STATUS_OK)
				return return_simple_error(
					error, transfer_status,
					"could not transfer the next request result");
			return PGM_STATUS_OK;
		}
		if (request->state != PGM_REQUEST_PENDING &&
			request->state != PGM_REQUEST_RUNNING)
		{
			pgm_status terminal_status = request->operation_status;
			bool		has_protocol_result = request->result_count != 0;

			*availability = PGM_AVAILABILITY_END;
			if (!has_protocol_result && terminal_status != PGM_STATUS_OK &&
				error != NULL && request->error != NULL)
			{
				*error = request->error;
				request->error = NULL;
			}
			(void) postgamma_mutex_unlock(request->mutex);
			postgamma_public_dispatch_progress_events(request);
			if (!has_protocol_result && terminal_status != PGM_STATUS_OK)
			{
				if (error != NULL && *error == NULL)
					(void) return_simple_error(
						error, terminal_status, "embedded request failed");
				return terminal_status;
			}
			return PGM_STATUS_OK;
		}
		(void) postgamma_mutex_unlock(request->mutex);
		postgamma_public_dispatch_progress_events(request);
		if (timeout_ms == 0)
		{
			*availability = PGM_AVAILABILITY_AGAIN;
			return PGM_STATUS_OK;
		}
		if (deadline_ns != POSTGAMMA_SUPERVISOR_NO_DEADLINE &&
			postgamma_monotonic_now_ns() >= deadline_ns)
			return return_simple_error(
				error, PGM_STATUS_TIMEOUT,
				"next-result wait timed out");
		private_deadline = deadline_ns == POSTGAMMA_SUPERVISOR_NO_DEADLINE ?
			POSTGAMMA_MEMORY_NO_DEADLINE : (int64_t) deadline_ns;
		private_status = postgamma_private_libpq_operation_wait(
			request->operation, private_deadline);
		if (private_status == POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT)
			return return_simple_error(
				error, PGM_STATUS_TIMEOUT,
				"next-result wait timed out");
		if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
			return return_simple_error(
				error, status_from_private(private_status),
				"next-result wait failed");
	}
}


pgm_request_id
pgm_request_identity(const pgm_request *request)
{
	return request_is_valid(request) && process_is_valid(request->owner_pid) ?
		!public_instance_reentrant(request->connection->instance) ?
		request->generation : 0 : 0;
}


static pgm_status
validate_execute_options(
	const pgm_execute_options *options, pgm_error **error)
{
	if (options == NULL || options->struct_size < sizeof(*options) ||
		options->flags != 0 || options->reserved16 != 0 ||
		options->result_format > PGM_FORMAT_BINARY ||
		options->parameter_count > INT_MAX ||
		(options->parameter_count != 0 && options->parameters == NULL))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid execute options");
	if ((options->delivery_mode != PGM_DELIVERY_MATERIALIZED &&
		 options->delivery_mode != PGM_DELIVERY_CHUNKED) ||
		(options->delivery_mode == PGM_DELIVERY_MATERIALIZED &&
		 options->target_chunk_rows != 0) ||
		(options->delivery_mode == PGM_DELIVERY_CHUNKED &&
		 options->target_chunk_rows == 0))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"delivery mode and target_chunk_rows do not agree");
	if (options->notice_callback == NULL && options->notice_user_data != NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"request notice user data requires a notice callback");
	return PGM_STATUS_OK;
}


static pgm_status
validate_script_options(
	const pgm_script_options *options, pgm_error **error)
{
	if (options == NULL || options->struct_size < sizeof(*options) ||
		options->flags != 0)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid script options");
	if ((options->delivery_mode != PGM_DELIVERY_MATERIALIZED &&
		 options->delivery_mode != PGM_DELIVERY_CHUNKED) ||
		(options->delivery_mode == PGM_DELIVERY_MATERIALIZED &&
		 options->target_chunk_rows != 0) ||
		(options->delivery_mode == PGM_DELIVERY_CHUNKED &&
		 options->target_chunk_rows == 0))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"delivery mode and target_chunk_rows do not agree");
	if (options->notice_callback == NULL && options->notice_user_data != NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"request notice user data requires a notice callback");
	return PGM_STATUS_OK;
}


static pgm_status
transfer_ready_result_locked(
	pgm_request *request, pgm_result **result,
	pgm_availability *availability, pgm_error **error)
{
	pgm_status	status;
	bool		is_error;

	if (!request_is_valid(request) || request->result == NULL ||
		result == NULL || availability == NULL)
		return PGM_STATUS_INVALID_ARGUMENT;
	status = result_attach_lease(request->result, request->result_lease);
	if (status != PGM_STATUS_OK)
		return status;
	is_error = pgm_result_kind(request->result) == PGM_RESULT_ERROR;
	*result = request->result;
	request->result = NULL;
	*availability = PGM_AVAILABILITY_READY;
	if (is_error && error != NULL && request->error != NULL)
	{
		*error = request->error;
		request->error = NULL;
	}
	return PGM_STATUS_OK;
}


static void
fill_public_column(
	const PostgammaPrivateResultField *source, pgm_column *column)
{
	*column = (pgm_column) PGM_COLUMN_INIT;
	column->name = source->name;
	column->table_oid = source->table_oid;
	column->table_column = source->table_column;
	column->type_oid = source->type_oid;
	column->type_size = source->type_size;
	column->type_modifier = source->type_modifier;
	column->format = source->format;
}
