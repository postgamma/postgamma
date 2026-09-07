/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include "postgamma/private/public_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define PGM_LOGICAL_CHANNEL_DEFAULT (256U * 1024U)
#define PGM_LOGICAL_QUANTUM_DEFAULT (64U * 1024U)
#define PGM_LOGICAL_CHANNEL_MIN 4096U
#define PGM_LOGICAL_CHANNEL_MAX (64U * 1024U * 1024U)
#define PGM_MANAGEMENT_CLOSE_TIMEOUT_MS INT64_C(30000)
#define PGM_OPERATION_PROGRESS_ACTIVE UINT32_C(0x01)
#define PGM_OPERATION_RELEASE_REQUESTED UINT32_C(0x02)


static _Atomic bool PostgammaLogicalOperationReserved;


static pgm_status logical_status_from_errno(int status);
static char *duplicate_text(const char *text);
static void logical_tracker_completion(
	void *argument, uint64_t generation, int operation_status);
static void archive_endpoint_notify(void *argument, uint32_t events);
static pgm_status create_logical_operation(
	pgm_instance *instance, pgm_operation_kind kind,
	const char *database, const char *user,
	size_t channel_capacity, size_t progress_quantum,
	pgm_stream_read_callback stream_read,
	pgm_stream_write_callback stream_write, void *stream_user_data,
	pgm_operation **operation, pgm_error **error);
static int destroy_logical_operation(pgm_operation *operation);
static void logical_worker_completed(
	void *argument, PostgammaLogicalToolWorker *worker,
	const PostgammaLogicalToolResult *result);
static pgm_status progress_logical_operation(
	pgm_operation *operation, pgm_operation_state *state,
	pgm_error **error);
static pgm_status progress_maintenance_operation(
	pgm_operation *operation, pgm_operation_state *state,
	pgm_error **error);
static pgm_status start_logical_worker_locked(pgm_operation *operation);
static void fail_logical_operation_locked(
	pgm_operation *operation, pgm_status status, const char *message);
static void preserve_logical_wake_locked(
	pgm_operation *operation, bool callback_again);
static char *maintenance_query(
	pgm_maintenance_kind kind, const char *database);
static void cleanup_maintenance_handles(
	pgm_request *request, pgm_connection *connection);
static bool acquire_progress_owner(pgm_operation *operation);
static void release_progress_owner(pgm_operation *operation);
static void release_logical_operation_owned(pgm_operation *operation);
static void snapshot_operation_state(
	pgm_operation *operation, pgm_operation_state *state);


bool
postgamma_logical_operation_kind(pgm_operation_kind kind)
{
	return kind == PGM_OPERATION_LOGICAL_DUMP ||
		kind == PGM_OPERATION_LOGICAL_RESTORE ||
		kind == PGM_OPERATION_MAINTENANCE;
}


pgm_status
pgm_instance_logical_dump_async(
	pgm_instance *instance,
	const pgm_logical_dump_options *options,
	pgm_operation **operation,
	pgm_error **error)
{
	size_t channel_capacity;
	size_t progress_quantum;
	bool expected = false;
	pgm_status status;

	if (error != NULL)
		*error = NULL;
	if (operation == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"logical dump operation output is required");
	*operation = NULL;
	if (!instance_is_valid(instance) || options == NULL ||
		options->struct_size < sizeof(*options) ||
		options->database == NULL || options->database[0] == '\0' ||
		options->user == NULL || options->user[0] == '\0' ||
		options->write == NULL ||
		(options->flags & ~PGM_LOGICAL_FLAGS_ALL) != 0 ||
		(options->flags & (PGM_LOGICAL_SCHEMA_ONLY |
			PGM_LOGICAL_DATA_ONLY)) ==
		(PGM_LOGICAL_SCHEMA_ONLY | PGM_LOGICAL_DATA_ONLY) ||
		(options->flags & (PGM_LOGICAL_CLEAN |
			PGM_LOGICAL_DATA_ONLY)) ==
		(PGM_LOGICAL_CLEAN | PGM_LOGICAL_DATA_ONLY))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid logical dump operation arguments");
	if (!process_is_valid(instance->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"instance belongs to a different host process");
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	channel_capacity = options->channel_capacity != 0 ?
		options->channel_capacity : PGM_LOGICAL_CHANNEL_DEFAULT;
	progress_quantum = options->progress_quantum != 0 ?
		options->progress_quantum : PGM_LOGICAL_QUANTUM_DEFAULT;
	if (channel_capacity < PGM_LOGICAL_CHANNEL_MIN ||
		channel_capacity > PGM_LOGICAL_CHANNEL_MAX ||
		progress_quantum == 0 || progress_quantum > channel_capacity)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"logical dump channel limits are invalid");
	if (!atomic_compare_exchange_strong_explicit(
			&PostgammaLogicalOperationReserved, &expected, true,
			memory_order_acq_rel, memory_order_acquire))
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"another logical tool is active in this process");
	status = create_logical_operation(
		instance, PGM_OPERATION_LOGICAL_DUMP,
		options->database, options->user,
		channel_capacity, progress_quantum, NULL, options->write,
		options->user_data, operation, error);
	if (status != PGM_STATUS_OK)
		atomic_store_explicit(
			&PostgammaLogicalOperationReserved, false, memory_order_release);
	else
	{
		(*operation)->logical_reserved = true;
		(*operation)->flags = options->flags;
	}
	return status;
}


pgm_status
pgm_instance_logical_restore_async(
	pgm_instance *instance,
	const pgm_logical_restore_options *options,
	pgm_operation **operation,
	pgm_error **error)
{
	size_t channel_capacity;
	size_t progress_quantum;
	bool expected = false;
	pgm_status status;

	if (error != NULL)
		*error = NULL;
	if (operation == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"logical restore operation output is required");
	*operation = NULL;
	if (!instance_is_valid(instance) || options == NULL ||
		options->struct_size < sizeof(*options) ||
		options->database == NULL || options->database[0] == '\0' ||
		options->user == NULL || options->user[0] == '\0' ||
		options->read == NULL ||
		(options->flags & ~PGM_LOGICAL_FLAGS_ALL) != 0 ||
		(options->flags & (PGM_LOGICAL_SCHEMA_ONLY |
			PGM_LOGICAL_DATA_ONLY)) ==
		(PGM_LOGICAL_SCHEMA_ONLY | PGM_LOGICAL_DATA_ONLY) ||
		(options->flags & (PGM_LOGICAL_CLEAN |
			PGM_LOGICAL_DATA_ONLY)) ==
		(PGM_LOGICAL_CLEAN | PGM_LOGICAL_DATA_ONLY))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid logical restore operation arguments");
	if (!process_is_valid(instance->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"instance belongs to a different host process");
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	channel_capacity = options->channel_capacity != 0 ?
		options->channel_capacity : PGM_LOGICAL_CHANNEL_DEFAULT;
	progress_quantum = options->progress_quantum != 0 ?
		options->progress_quantum : PGM_LOGICAL_QUANTUM_DEFAULT;
	if (channel_capacity < PGM_LOGICAL_CHANNEL_MIN ||
		channel_capacity > PGM_LOGICAL_CHANNEL_MAX ||
		progress_quantum == 0 || progress_quantum > channel_capacity)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"logical restore channel limits are invalid");
	if (!atomic_compare_exchange_strong_explicit(
			&PostgammaLogicalOperationReserved, &expected, true,
			memory_order_acq_rel, memory_order_acquire))
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"another logical tool is active in this process");
	status = create_logical_operation(
		instance, PGM_OPERATION_LOGICAL_RESTORE,
		options->database, options->user,
		channel_capacity, progress_quantum, options->read, NULL,
		options->user_data, operation, error);
	if (status != PGM_STATUS_OK)
		atomic_store_explicit(
			&PostgammaLogicalOperationReserved, false, memory_order_release);
	else
	{
		(*operation)->logical_reserved = true;
		(*operation)->flags = options->flags;
	}
	return status;
}


pgm_status
pgm_instance_maintenance_async(
	pgm_instance *instance,
	const pgm_maintenance_options *options,
	pgm_operation **operation,
	pgm_error **error)
{
	pgm_status status;

	if (error != NULL)
		*error = NULL;
	if (operation == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"maintenance operation output is required");
	*operation = NULL;
	if (!instance_is_valid(instance) || options == NULL ||
		options->struct_size < sizeof(*options) || options->flags != 0 ||
		options->reserved != 0 || options->database == NULL ||
		options->database[0] == '\0' || options->user == NULL ||
		options->user[0] == '\0' ||
		options->kind < PGM_MAINTENANCE_VACUUM ||
		options->kind > PGM_MAINTENANCE_REINDEX_DATABASE)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid maintenance operation arguments");
	if (!process_is_valid(instance->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"instance belongs to a different host process");
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = create_logical_operation(
		instance, PGM_OPERATION_MAINTENANCE,
		options->database, options->user, 0, 0, NULL, NULL, NULL,
		operation, error);
	if (status != PGM_STATUS_OK)
		return status;
	(*operation)->maintenance_sql = maintenance_query(
		options->kind, options->database);
	if ((*operation)->maintenance_sql == NULL)
	{
		(void) destroy_logical_operation(*operation);
		*operation = NULL;
		return return_simple_error(
			error, PGM_STATUS_OUT_OF_MEMORY,
			"could not create the maintenance command");
	}
	return PGM_STATUS_OK;
}


static pgm_status
create_logical_operation(
	pgm_instance *instance, pgm_operation_kind kind,
	const char *database, const char *user,
	size_t channel_capacity, size_t progress_quantum,
	pgm_stream_read_callback stream_read,
	pgm_stream_write_callback stream_write, void *stream_user_data,
	pgm_operation **operation, pgm_error **error)
{
	pgm_operation *created = NULL;
	PostgammaMemoryStatus memory_status;
	int status;

	status = postgamma_mutex_lock(instance->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the embedded instance");
	if (instance->closing || instance->kernel_result.runtime_handle == NULL)
	{
		(void) postgamma_mutex_unlock(instance->mutex);
		return return_simple_error(
			error, PGM_STATUS_INSTANCE_FAILED,
			"embedded instance is not accepting management operations");
	}
	created = calloc(1, sizeof(*created));
	if (created == NULL)
	{
		status = ENOMEM;
		goto fail_locked;
	}
	created->database = duplicate_text(database);
	created->user = duplicate_text(user);
	if (created->database == NULL || created->user == NULL)
	{
		status = ENOMEM;
		goto fail_locked;
	}
	created->magic = PGM_OPERATION_MAGIC;
	created->owner_pid = getpid();
	created->instance = instance;
	created->kind = kind;
	created->state = PGM_OPERATION_PENDING;
	created->phase = PGM_OPERATION_PHASE_PENDING;
	created->operation_status = PGM_STATUS_OK;
	created->channel_capacity = channel_capacity;
	created->progress_quantum = progress_quantum;
	created->stream_read = stream_read;
	created->stream_write = stream_write;
	created->stream_user_data = stream_user_data;
	created->archive_generation = instance->generation;
	atomic_init(&created->lifecycle_state, UINT32_C(0));
	status = postgamma_mutex_create(&created->mutex);
	if (status != 0)
		goto fail_locked;
	status = postgamma_instance_checkpoint_tracker_create(
		(PostgammaInstanceRuntime *) instance->kernel_result.runtime_handle,
		instance->generation, logical_tracker_completion, created,
		&created->tracker);
	if (status != 0)
		goto fail_locked;
	if (kind == PGM_OPERATION_LOGICAL_DUMP ||
		kind == PGM_OPERATION_LOGICAL_RESTORE)
	{
		created->stream_buffer = malloc(progress_quantum);
		if (created->stream_buffer == NULL)
		{
			status = ENOMEM;
			goto fail_locked;
		}
		memory_status = postgamma_memory_transport_create(
			created->archive_generation, channel_capacity,
			&created->archive_host_endpoint,
			&created->archive_worker_endpoint);
		if (memory_status != POSTGAMMA_MEMORY_STATUS_OK)
		{
			status = memory_status == POSTGAMMA_MEMORY_STATUS_NO_MEMORY ?
				ENOMEM : EIO;
			goto fail_locked;
		}
		memory_status = postgamma_memory_endpoint_set_notify(
			created->archive_host_endpoint,
			created->archive_generation,
			archive_endpoint_notify, created);
		if (memory_status != POSTGAMMA_MEMORY_STATUS_OK)
		{
			status = EIO;
			goto fail_locked;
		}
	}
	if (instance->operation_count == SIZE_MAX)
	{
		status = EOVERFLOW;
		goto fail_locked;
	}
	instance->operation_count++;
	created->registered_with_instance = true;
	(void) postgamma_mutex_unlock(instance->mutex);
	*operation = created;
	return PGM_STATUS_OK;

fail_locked:
	(void) postgamma_mutex_unlock(instance->mutex);
	if (created != NULL)
	{
		if (created->archive_host_endpoint != NULL)
		{
			(void) postgamma_memory_endpoint_set_notify(
				created->archive_host_endpoint,
				created->archive_generation, NULL, NULL);
			(void) postgamma_memory_endpoint_release(
				&created->archive_host_endpoint,
				created->archive_generation);
		}
		if (created->archive_worker_endpoint != NULL)
			(void) postgamma_memory_endpoint_release(
				&created->archive_worker_endpoint,
				created->archive_generation);
		if (created->tracker != NULL)
			(void) postgamma_instance_checkpoint_tracker_destroy(
				created->tracker);
		if (created->mutex != NULL)
			(void) postgamma_mutex_destroy(created->mutex);
		free(created->stream_buffer);
		free(created->database);
		free(created->user);
		created->magic = 0;
		free(created);
	}
	return return_simple_error(
		error, logical_status_from_errno(status),
		"could not create the management operation");
}


pgm_status
postgamma_logical_operation_progress(
	pgm_operation *operation, pgm_operation_state *state, pgm_error **error)
{
	if (operation->kind == PGM_OPERATION_MAINTENANCE)
		return progress_maintenance_operation(operation, state, error);
	return progress_logical_operation(operation, state, error);
}


static pgm_status
progress_logical_operation(
	pgm_operation *operation, pgm_operation_state *state,
	pgm_error **error)
{
	bool callback_again = false;
	pgm_status result_status = PGM_STATUS_OK;
	char diagnostic[sizeof(operation->diagnostic)] = {0};
	uint64_t wake_count = 0;
	int status;

	if (!acquire_progress_owner(operation))
	{
		snapshot_operation_state(operation, state);
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"another host thread is progressing this operation");
	}
	status = postgamma_instance_checkpoint_tracker_wake_drain(
		operation->tracker, &wake_count);
	if (status != 0 && status != EAGAIN)
	{
		result_status = PGM_STATUS_INTERNAL_ERROR;
		goto done_unlocked;
	}
	status = postgamma_mutex_lock(operation->mutex);
	if (status != 0)
	{
		result_status = PGM_STATUS_INTERNAL_ERROR;
		goto done_unlocked;
	}
	if (operation->free_pending)
	{
		result_status = PGM_STATUS_BUSY;
		(void) postgamma_mutex_unlock(operation->mutex);
		goto done_unlocked;
	}
	if (operation->state == PGM_OPERATION_PENDING)
	{
		result_status = start_logical_worker_locked(operation);
		if (result_status != PGM_STATUS_OK)
			fail_logical_operation_locked(
				operation, result_status,
				"could not start the logical-tool worker");
	}
	if (operation->state == PGM_OPERATION_RUNNING &&
		operation->kind == PGM_OPERATION_LOGICAL_DUMP)
	{
		if (operation->stream_buffer_offset ==
			operation->stream_buffer_size && !operation->stream_end)
		{
			PostgammaMemoryIoResult io;

			operation->stream_buffer_offset = 0;
			operation->stream_buffer_size = 0;
			io = postgamma_memory_endpoint_read(
				operation->archive_host_endpoint,
				operation->archive_generation,
				operation->stream_buffer,
				operation->progress_quantum);
			if (io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
				operation->stream_buffer_size = io.bytes;
			else if (io.status == POSTGAMMA_MEMORY_STATUS_EOF)
				operation->stream_end = true;
			else if (io.status != POSTGAMMA_MEMORY_STATUS_RETRY &&
				!(io.status == POSTGAMMA_MEMORY_STATUS_ABORTED &&
				  operation->cancel_requested))
				fail_logical_operation_locked(
					operation, PGM_STATUS_IO_ERROR,
					"logical dump archive channel failed");
		}
		if (operation->state == PGM_OPERATION_RUNNING &&
			operation->stream_buffer_offset < operation->stream_buffer_size)
		{
			const void *data = operation->stream_buffer +
				operation->stream_buffer_offset;
			size_t size = operation->stream_buffer_size -
				operation->stream_buffer_offset;
			size_t consumed = 0;
			pgm_io_state io_state;
			PostgammaPublicCallbackFrame frame;

			operation->phase = PGM_OPERATION_PHASE_HOST_IO;
			(void) postgamma_mutex_unlock(operation->mutex);
			postgamma_public_callback_enter(&frame, operation->instance);
			io_state = operation->stream_write(
				operation->stream_user_data, data, size, &consumed);
			postgamma_public_callback_leave(&frame);
			if (postgamma_mutex_lock(operation->mutex) != 0)
			{
				result_status = PGM_STATUS_INTERNAL_ERROR;
				goto done_unlocked;
			}
			if (io_state == PGM_IO_PROGRESS && consumed > 0 &&
				consumed <= size)
			{
				operation->stream_buffer_offset += consumed;
				operation->bytes_produced += consumed;
			}
			else if (io_state == PGM_IO_AGAIN && consumed == 0)
				callback_again = true;
			else
				fail_logical_operation_locked(
					operation, PGM_STATUS_INVALID_ARGUMENT,
					"logical dump write callback violated its contract");
		}
		if (operation->state == PGM_OPERATION_RUNNING &&
			operation->worker_done && operation->stream_end &&
			operation->stream_buffer_offset == operation->stream_buffer_size)
		{
			operation->state = PGM_OPERATION_COMPLETED;
			operation->phase = PGM_OPERATION_PHASE_TERMINAL;
			operation->operation_status = PGM_STATUS_OK;
		}
	}
	else if (operation->state == PGM_OPERATION_RUNNING &&
		operation->kind == PGM_OPERATION_LOGICAL_RESTORE)
	{
		if (operation->stream_buffer_offset < operation->stream_buffer_size)
		{
			PostgammaMemoryIoResult io = postgamma_memory_endpoint_write(
				operation->archive_host_endpoint,
				operation->archive_generation,
				operation->stream_buffer + operation->stream_buffer_offset,
				operation->stream_buffer_size -
				operation->stream_buffer_offset);

			if (io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
				operation->stream_buffer_offset += io.bytes;
			else if (io.status != POSTGAMMA_MEMORY_STATUS_RETRY &&
				!(io.status == POSTGAMMA_MEMORY_STATUS_ABORTED &&
				  operation->cancel_requested))
				fail_logical_operation_locked(
					operation, PGM_STATUS_IO_ERROR,
					"logical restore archive channel failed");
		}
		if (operation->state == PGM_OPERATION_RUNNING &&
			operation->stream_buffer_offset ==
			operation->stream_buffer_size && !operation->stream_end)
		{
			size_t produced = 0;
			pgm_io_state io_state;
			PostgammaPublicCallbackFrame frame;

			operation->stream_buffer_offset = 0;
			operation->stream_buffer_size = 0;
			operation->phase = PGM_OPERATION_PHASE_HOST_IO;
			(void) postgamma_mutex_unlock(operation->mutex);
			postgamma_public_callback_enter(&frame, operation->instance);
			io_state = operation->stream_read(
				operation->stream_user_data, operation->stream_buffer,
				operation->progress_quantum, &produced);
			postgamma_public_callback_leave(&frame);
			if (postgamma_mutex_lock(operation->mutex) != 0)
			{
				result_status = PGM_STATUS_INTERNAL_ERROR;
				goto done_unlocked;
			}
			if (io_state == PGM_IO_PROGRESS && produced > 0 &&
				produced <= operation->progress_quantum)
			{
				operation->stream_buffer_size = produced;
				operation->bytes_received += produced;
			}
			else if (io_state == PGM_IO_AGAIN && produced == 0)
				callback_again = true;
			else if (io_state == PGM_IO_END && produced == 0)
			{
				PostgammaMemoryStatus close_status =
					postgamma_memory_endpoint_half_close_write(
						operation->archive_host_endpoint,
						operation->archive_generation);

				if (close_status == POSTGAMMA_MEMORY_STATUS_OK)
					operation->stream_end = true;
				else
					fail_logical_operation_locked(
						operation, PGM_STATUS_IO_ERROR,
						"could not finish logical restore input");
			}
			else
				fail_logical_operation_locked(
					operation, PGM_STATUS_INVALID_ARGUMENT,
					"logical restore read callback violated its contract");
		}
	}
	preserve_logical_wake_locked(operation, callback_again);
	*state = operation->state;
	result_status = operation->operation_status;
	(void) snprintf(diagnostic, sizeof(diagnostic), "%s", operation->diagnostic);
	(void) postgamma_mutex_unlock(operation->mutex);

done_unlocked:
	release_progress_owner(operation);
	(void) wake_count;
	if (result_status != PGM_STATUS_OK)
		return return_simple_error(
			error, result_status,
			diagnostic[0] != '\0' ? diagnostic :
			"logical management operation failed");
	return PGM_STATUS_OK;
}


static pgm_status
start_logical_worker_locked(pgm_operation *operation)
{
	PostgammaLogicalToolOptions options = POSTGAMMA_LOGICAL_TOOL_OPTIONS_INIT;
	PostgammaLogicalToolKind kind;
	int status;

	options.instance = operation->instance;
	options.database = operation->database;
	options.user = operation->user;
	options.archive_path = POSTGAMMA_LOGICAL_TOOL_STREAM_PATH;
	options.flags = operation->flags;
	options.archive_endpoint = operation->archive_worker_endpoint;
	options.archive_generation = operation->archive_generation;
	kind = operation->kind == PGM_OPERATION_LOGICAL_DUMP ?
		POSTGAMMA_LOGICAL_TOOL_DUMP : POSTGAMMA_LOGICAL_TOOL_RESTORE;
	operation->phase = PGM_OPERATION_PHASE_STARTING;
	status = postgamma_logical_tool_start(
		kind, &options, logical_worker_completed, operation,
		&operation->logical_worker);
	if (status != 0)
		return logical_status_from_errno(status);
	operation->archive_worker_endpoint = NULL;
	operation->state = PGM_OPERATION_RUNNING;
	operation->phase = PGM_OPERATION_PHASE_DATABASE;
	return PGM_STATUS_OK;
}


static void
logical_worker_completed(
	void *argument, PostgammaLogicalToolWorker *worker,
	const PostgammaLogicalToolResult *result)
{
	pgm_operation *operation = argument;
	bool free_pending;

	if (!operation_is_valid(operation) || result == NULL ||
		postgamma_mutex_lock(operation->mutex) != 0)
		return;
	if (operation->logical_worker != worker)
	{
		(void) postgamma_mutex_unlock(operation->mutex);
		return;
	}
	operation->logical_worker = NULL;
	operation->worker_done = true;
	if (operation->state == PGM_OPERATION_FAILED)
	{
		/* Preserve the causal host callback or channel failure. */
	}
	else if (operation->cancel_requested || result->status == ECANCELED)
	{
		(void) snprintf(
			operation->diagnostic, sizeof(operation->diagnostic), "%s",
			result->message);
		operation->state = PGM_OPERATION_CANCELED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status = PGM_STATUS_CANCELED;
	}
	else if (result->status != 0)
	{
		(void) snprintf(
			operation->diagnostic, sizeof(operation->diagnostic), "%s",
			result->message);
		operation->state = PGM_OPERATION_FAILED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status = logical_status_from_errno(result->status);
	}
	else if (operation->kind == PGM_OPERATION_LOGICAL_RESTORE)
	{
		(void) snprintf(
			operation->diagnostic, sizeof(operation->diagnostic), "%s",
			result->message);
		operation->state = PGM_OPERATION_COMPLETED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status = PGM_STATUS_OK;
	}
	else if (operation->stream_end &&
		operation->stream_buffer_offset == operation->stream_buffer_size)
	{
		operation->state = PGM_OPERATION_COMPLETED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status = PGM_STATUS_OK;
	}
	else
		operation->phase = PGM_OPERATION_PHASE_HOST_IO;
	if (operation->logical_reserved)
	{
		operation->logical_reserved = false;
		atomic_store_explicit(
			&PostgammaLogicalOperationReserved, false, memory_order_release);
	}
	free_pending = operation->free_pending;
	if (!free_pending)
		(void) postgamma_instance_checkpoint_tracker_wake(operation->tracker);
	(void) postgamma_mutex_unlock(operation->mutex);
	if (free_pending)
		(void) destroy_logical_operation(operation);
}


static void
fail_logical_operation_locked(
	pgm_operation *operation, pgm_status status, const char *message)
{
	if (operation->state == PGM_OPERATION_FAILED ||
		operation->state == PGM_OPERATION_CANCELED)
		return;
	operation->state = PGM_OPERATION_FAILED;
	operation->phase = PGM_OPERATION_PHASE_TERMINAL;
	operation->operation_status = status;
	(void) snprintf(
		operation->diagnostic, sizeof(operation->diagnostic), "%s", message);
	if (operation->logical_worker != NULL)
		(void) postgamma_logical_tool_cancel(operation->logical_worker);
	else if (operation->archive_host_endpoint != NULL)
		(void) postgamma_memory_endpoint_abort(
			operation->archive_host_endpoint,
			operation->archive_generation,
			POSTGAMMA_MEMORY_ABORT_INTERNAL);
}


static void
preserve_logical_wake_locked(
	pgm_operation *operation, bool callback_again)
{
	uint32_t events = 0;
	uint32_t wanted;

	if (callback_again || operation->state != PGM_OPERATION_RUNNING)
		return;
	wanted = operation->kind == PGM_OPERATION_LOGICAL_DUMP ?
		POSTGAMMA_MEMORY_WAIT_READABLE : POSTGAMMA_MEMORY_WAIT_WRITABLE;
	if (operation->stream_buffer_offset < operation->stream_buffer_size ||
		operation->worker_done ||
		(postgamma_memory_endpoint_ready(
			operation->archive_host_endpoint,
			operation->archive_generation, &events) ==
		 POSTGAMMA_MEMORY_STATUS_OK && (events & wanted) != 0))
		(void) postgamma_instance_checkpoint_tracker_wake(operation->tracker);
}


static pgm_status
progress_maintenance_operation(
	pgm_operation *operation, pgm_operation_state *state,
	pgm_error **error)
{
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_request *request;
	pgm_result *result = NULL;
	pgm_error *request_error = NULL;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_status status = PGM_STATUS_OK;
	char diagnostic[sizeof(operation->diagnostic)] = {0};
	uint64_t wake_count = 0;
	int lock_status;

	if (!acquire_progress_owner(operation))
	{
		snapshot_operation_state(operation, state);
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"another host thread is progressing this operation");
	}
	(void) postgamma_instance_checkpoint_tracker_wake_drain(
		operation->tracker, &wake_count);
	lock_status = postgamma_mutex_lock(operation->mutex);
	if (lock_status != 0)
	{
		status = PGM_STATUS_INTERNAL_ERROR;
		goto done_unlocked;
	}
	if (operation->state == PGM_OPERATION_PENDING)
	{
		connection_options.database = operation->database;
		connection_options.user = operation->user;
		connection_options.application_name = "postgamma-maintenance";
		operation->phase = PGM_OPERATION_PHASE_STARTING;
		status = pgm_connection_open(
			operation->instance, &connection_options,
			&operation->management_connection, &request_error);
		if (status == PGM_STATUS_OK)
			status = pgm_execute_async(
				operation->management_connection,
				operation->maintenance_sql, NULL, 0, PGM_FORMAT_TEXT,
				&operation->management_request, &request_error);
		if (status != PGM_STATUS_OK)
		{
			operation->state = operation->cancel_requested ?
				PGM_OPERATION_CANCELED : PGM_OPERATION_FAILED;
			operation->phase = PGM_OPERATION_PHASE_TERMINAL;
			operation->operation_status = operation->cancel_requested ?
				PGM_STATUS_CANCELED : status;
			(void) snprintf(
				operation->diagnostic, sizeof(operation->diagnostic), "%s",
				request_error != NULL ? pgm_error_message(request_error) :
				"could not start the maintenance request");
			pgm_error_free(request_error);
			request_error = NULL;
			*state = operation->state;
			(void) snprintf(
				diagnostic, sizeof(diagnostic), "%s", operation->diagnostic);
			(void) postgamma_mutex_unlock(operation->mutex);
			goto done_unlocked;
		}
		operation->state = PGM_OPERATION_RUNNING;
		operation->phase = PGM_OPERATION_PHASE_DATABASE;
	}
	if (operation->state != PGM_OPERATION_RUNNING)
	{
		*state = operation->state;
		status = operation->operation_status;
		(void) snprintf(
			diagnostic, sizeof(diagnostic), "%s", operation->diagnostic);
		(void) postgamma_mutex_unlock(operation->mutex);
		goto done_unlocked;
	}
	request = operation->management_request;
	(void) postgamma_mutex_unlock(operation->mutex);
	status = pgm_request_next_result(
		request, 0, &result, &availability, &request_error);
	pgm_result_free(result);
	if (postgamma_mutex_lock(operation->mutex) != 0)
	{
		status = PGM_STATUS_INTERNAL_ERROR;
		goto done_unlocked;
	}
	if (status != PGM_STATUS_OK)
	{
		operation->state = operation->cancel_requested ?
			PGM_OPERATION_CANCELED : PGM_OPERATION_FAILED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status = operation->cancel_requested ?
			PGM_STATUS_CANCELED : status;
		(void) snprintf(
			operation->diagnostic, sizeof(operation->diagnostic), "%s",
			request_error != NULL ? pgm_error_message(request_error) :
			"maintenance request failed");
	}
	else if (availability == PGM_AVAILABILITY_END)
	{
		operation->state = operation->cancel_requested ?
			PGM_OPERATION_CANCELED : PGM_OPERATION_COMPLETED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status = operation->cancel_requested ?
			PGM_STATUS_CANCELED : PGM_STATUS_OK;
	}
	else if (availability == PGM_AVAILABILITY_READY)
	{
		operation->objects_completed++;
		(void) postgamma_instance_checkpoint_tracker_wake(operation->tracker);
	}
	pgm_error_free(request_error);
	request_error = NULL;
	*state = operation->state;
	status = operation->operation_status;
	(void) snprintf(diagnostic, sizeof(diagnostic), "%s", operation->diagnostic);
	(void) postgamma_mutex_unlock(operation->mutex);

done_unlocked:
	release_progress_owner(operation);
	(void) wake_count;
	if (status != PGM_STATUS_OK)
		return return_simple_error(
			error, status,
			diagnostic[0] != '\0' ? diagnostic :
			"maintenance operation failed");
	return PGM_STATUS_OK;
}


pgm_status
postgamma_logical_operation_waitable(
	pgm_operation *operation, int *file_descriptor, pgm_error **error)
{
	int descriptor;

	if (operation->kind == PGM_OPERATION_MAINTENANCE)
		descriptor = postgamma_public_event_waitable_fd(operation->instance);
	else
		descriptor = postgamma_instance_checkpoint_tracker_waitable_fd(
			operation->tracker);
	if (descriptor < 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"management operation waitable is unavailable");
	*file_descriptor = descriptor;
	return PGM_STATUS_OK;
}


pgm_status
postgamma_logical_operation_cancel(
	pgm_operation *operation, pgm_error **error)
{
	PostgammaLogicalToolWorker *worker = NULL;
	pgm_request *request = NULL;
	pgm_status status = PGM_STATUS_OK;

	if (postgamma_mutex_lock(operation->mutex) != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the management operation");
	if (operation->state == PGM_OPERATION_CANCELED)
	{
		(void) postgamma_mutex_unlock(operation->mutex);
		return PGM_STATUS_OK;
	}
	if (operation->state == PGM_OPERATION_COMPLETED ||
		operation->state == PGM_OPERATION_FAILED)
	{
		(void) postgamma_mutex_unlock(operation->mutex);
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"a terminal management operation cannot be canceled");
	}
	operation->cancel_requested = true;
	if (operation->state == PGM_OPERATION_PENDING)
	{
		operation->state = PGM_OPERATION_CANCELED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status = PGM_STATUS_CANCELED;
		if (operation->logical_reserved)
		{
			operation->logical_reserved = false;
			atomic_store_explicit(
				&PostgammaLogicalOperationReserved, false,
				memory_order_release);
		}
		(void) postgamma_instance_checkpoint_tracker_wake(operation->tracker);
		(void) postgamma_mutex_unlock(operation->mutex);
		return PGM_STATUS_OK;
	}
	worker = operation->logical_worker;
	request = operation->management_request;
	if (worker != NULL && postgamma_logical_tool_cancel(worker) != 0)
		status = PGM_STATUS_INTERNAL_ERROR;
	else if (worker == NULL && request == NULL &&
		(operation->kind == PGM_OPERATION_LOGICAL_DUMP ||
		 operation->kind == PGM_OPERATION_LOGICAL_RESTORE))
	{
		operation->state = PGM_OPERATION_CANCELED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status = PGM_STATUS_CANCELED;
		if (operation->archive_host_endpoint != NULL)
			(void) postgamma_memory_endpoint_abort(
				operation->archive_host_endpoint,
				operation->archive_generation,
				POSTGAMMA_MEMORY_ABORT_CANCELLED);
		(void) postgamma_instance_checkpoint_tracker_wake(operation->tracker);
	}
	(void) postgamma_mutex_unlock(operation->mutex);
	if (request != NULL)
		status = pgm_request_cancel(request, error);
	return status;
}


void
postgamma_logical_operation_free(pgm_operation *operation)
{
	uint32_t previous;
	uint32_t expected;

	if (!operation_is_valid(operation))
		return;
	previous = atomic_fetch_or_explicit(
		&operation->lifecycle_state, PGM_OPERATION_RELEASE_REQUESTED,
		memory_order_acq_rel);
	if ((previous & PGM_OPERATION_PROGRESS_ACTIVE) != 0)
		return;
	expected = PGM_OPERATION_RELEASE_REQUESTED;
	if (!atomic_compare_exchange_strong_explicit(
			&operation->lifecycle_state, &expected,
			PGM_OPERATION_RELEASE_REQUESTED |
				PGM_OPERATION_PROGRESS_ACTIVE,
			memory_order_acq_rel, memory_order_acquire))
		return;
	release_logical_operation_owned(operation);
}


static void
release_logical_operation_owned(pgm_operation *operation)
{
	PostgammaLogicalToolWorker *worker = NULL;
	pgm_request *request = NULL;
	pgm_connection *connection = NULL;

	if (postgamma_mutex_lock(operation->mutex) != 0)
		return;
	if (operation->logical_worker != NULL)
	{
		operation->free_pending = true;
		operation->cancel_requested = true;
		worker = operation->logical_worker;
		(void) postgamma_logical_tool_cancel(worker);
		(void) postgamma_mutex_unlock(operation->mutex);
		return;
	}
	request = operation->management_request;
	connection = operation->management_connection;
	operation->management_request = NULL;
	operation->management_connection = NULL;
	operation->state = operation->state == PGM_OPERATION_COMPLETED ?
		PGM_OPERATION_COMPLETED : PGM_OPERATION_CANCELED;
	operation->phase = PGM_OPERATION_PHASE_TERMINAL;
	(void) postgamma_mutex_unlock(operation->mutex);
	if (request != NULL)
		(void) pgm_request_cancel(request, NULL);
	cleanup_maintenance_handles(request, connection);
	(void) destroy_logical_operation(operation);
}


static bool
acquire_progress_owner(pgm_operation *operation)
{
	uint32_t expected = UINT32_C(0);

	return atomic_compare_exchange_strong_explicit(
		&operation->lifecycle_state, &expected,
		PGM_OPERATION_PROGRESS_ACTIVE,
		memory_order_acq_rel, memory_order_acquire);
}


static void
release_progress_owner(pgm_operation *operation)
{
	uint32_t previous;
	uint32_t expected;

	previous = atomic_fetch_and_explicit(
		&operation->lifecycle_state,
		~PGM_OPERATION_PROGRESS_ACTIVE, memory_order_acq_rel);
	if ((previous & PGM_OPERATION_RELEASE_REQUESTED) == 0)
		return;
	expected = PGM_OPERATION_RELEASE_REQUESTED;
	if (atomic_compare_exchange_strong_explicit(
			&operation->lifecycle_state, &expected,
			PGM_OPERATION_RELEASE_REQUESTED |
				PGM_OPERATION_PROGRESS_ACTIVE,
			memory_order_acq_rel, memory_order_acquire))
		release_logical_operation_owned(operation);
}


static void
snapshot_operation_state(
	pgm_operation *operation, pgm_operation_state *state)
{
	if (postgamma_mutex_lock(operation->mutex) != 0)
		return;
	*state = operation->state;
	(void) postgamma_mutex_unlock(operation->mutex);
}


pgm_status
pgm_operation_get_progress(
	pgm_operation *operation,
	pgm_operation_progress_snapshot *progress,
	pgm_error **error)
{
	if (error != NULL)
		*error = NULL;
	if (!operation_is_valid(operation) || progress == NULL ||
		progress->struct_size < sizeof(*progress))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid operation progress snapshot arguments");
	if (!process_is_valid(operation->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"management operation belongs to a different host process");
	if (public_instance_reentrant(operation->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	if (postgamma_mutex_lock(operation->mutex) != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the management operation");
	progress->kind = operation->kind;
	progress->state = operation->state;
	progress->phase = operation->phase;
	progress->bytes_received = operation->bytes_received;
	progress->bytes_produced = operation->bytes_produced;
	progress->total_bytes = operation->total_bytes;
	progress->objects_completed = operation->objects_completed;
	progress->objects_total = operation->objects_total;
	(void) postgamma_mutex_unlock(operation->mutex);
	return PGM_STATUS_OK;
}


static int
destroy_logical_operation(pgm_operation *operation)
{
	pgm_instance *instance;
	int status;

	if (!operation_is_valid(operation) || operation->logical_worker != NULL)
		return EBUSY;
	instance = operation->instance;
	if (operation->archive_host_endpoint != NULL)
	{
		(void) postgamma_memory_endpoint_set_notify(
			operation->archive_host_endpoint,
			operation->archive_generation, NULL, NULL);
		(void) postgamma_memory_endpoint_abort(
			operation->archive_host_endpoint,
			operation->archive_generation,
			POSTGAMMA_MEMORY_ABORT_SHUTDOWN);
		(void) postgamma_memory_endpoint_release(
			&operation->archive_host_endpoint,
			operation->archive_generation);
	}
	if (operation->archive_worker_endpoint != NULL)
		(void) postgamma_memory_endpoint_release(
			&operation->archive_worker_endpoint,
			operation->archive_generation);
	status = postgamma_instance_checkpoint_tracker_destroy(operation->tracker);
	if (status != 0)
		return status;
	operation->tracker = NULL;
	status = postgamma_mutex_destroy(operation->mutex);
	if (status != 0)
		return status;
	operation->mutex = NULL;
	if (operation->registered_with_instance)
	{
		status = postgamma_mutex_lock(instance->mutex);
		if (status != 0)
			return status;
		if (instance->operation_count == 0)
		{
			(void) postgamma_mutex_unlock(instance->mutex);
			return EPROTO;
		}
		instance->operation_count--;
		operation->registered_with_instance = false;
		(void) postgamma_mutex_unlock(instance->mutex);
	}
	if (operation->logical_reserved)
		atomic_store_explicit(
			&PostgammaLogicalOperationReserved, false, memory_order_release);
	operation->magic = 0;
	operation->instance = NULL;
	free(operation->maintenance_sql);
	free(operation->stream_buffer);
	free(operation->database);
	free(operation->user);
	free(operation);
	return 0;
}


static void
logical_tracker_completion(
	void *argument, uint64_t generation, int operation_status)
{
	(void) argument;
	(void) generation;
	(void) operation_status;
}


static void
archive_endpoint_notify(void *argument, uint32_t events)
{
	pgm_operation *operation = argument;

	(void) events;
	if (operation != NULL && operation->tracker != NULL)
		(void) postgamma_instance_checkpoint_tracker_wake(operation->tracker);
}


static char *
maintenance_query(pgm_maintenance_kind kind, const char *database)
{
	const char *fixed = NULL;
	char *query;
	size_t length;
	size_t offset;

	switch (kind)
	{
		case PGM_MAINTENANCE_VACUUM:
			fixed = "VACUUM";
			break;
		case PGM_MAINTENANCE_ANALYZE:
			fixed = "ANALYZE";
			break;
		case PGM_MAINTENANCE_VACUUM_ANALYZE:
			fixed = "VACUUM (ANALYZE)";
			break;
		case PGM_MAINTENANCE_REINDEX_DATABASE:
			break;
		default:
			return NULL;
	}
	if (fixed != NULL)
		return duplicate_text(fixed);
	if (strlen(database) > (SIZE_MAX - 32) / 2)
		return NULL;
	length = 20 + 2 * strlen(database);
	query = malloc(length);
	if (query == NULL)
		return NULL;
	offset = (size_t) snprintf(query, length, "REINDEX DATABASE \"");
	for (const char *cursor = database; *cursor != '\0'; cursor++)
	{
		if (*cursor == '"')
			query[offset++] = '"';
		query[offset++] = *cursor;
	}
	query[offset++] = '"';
	query[offset] = '\0';
	return query;
}


static void
cleanup_maintenance_handles(
	pgm_request *request, pgm_connection *connection)
{
	pgm_request_free(request);
	if (connection != NULL)
		(void) pgm_connection_close(
			connection, PGM_MANAGEMENT_CLOSE_TIMEOUT_MS, NULL);
}


static char *
duplicate_text(const char *text)
{
	size_t length;
	char *copy;

	if (text == NULL)
		return NULL;
	length = strlen(text);
	if (length == SIZE_MAX)
		return NULL;
	copy = malloc(length + 1);
	if (copy != NULL)
		memcpy(copy, text, length + 1);
	return copy;
}


static pgm_status
logical_status_from_errno(int status)
{
	switch (status)
	{
		case 0:
			return PGM_STATUS_OK;
		case EINVAL:
			return PGM_STATUS_INVALID_ARGUMENT;
		case EPROTO:
			return PGM_STATUS_POSTGRES_ERROR;
		case EBUSY:
		case EAGAIN:
		case EALREADY:
			return PGM_STATUS_BUSY;
		case ETIMEDOUT:
			return PGM_STATUS_TIMEOUT;
		case ECANCELED:
		case EINTR:
			return PGM_STATUS_CANCELED;
		case ENOMEM:
			return PGM_STATUS_OUT_OF_MEMORY;
		case ENOTSUP:
			return PGM_STATUS_UNSUPPORTED;
		case ESTALE:
			return PGM_STATUS_VERSION_MISMATCH;
		case EIO:
		case ENOENT:
		case EACCES:
		case EPERM:
			return PGM_STATUS_IO_ERROR;
	}
	return PGM_STATUS_INTERNAL_ERROR;
}
