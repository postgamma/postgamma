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
#include <stdlib.h>
#include <unistd.h>


static pgm_status management_status_from_errno(int status);
static void checkpoint_operation_completed(
	void *argument, uint64_t generation, int operation_status);
static int destroy_operation(pgm_operation *operation);


pgm_status
pgm_instance_checkpoint_async(
	pgm_instance *instance,
	const pgm_checkpoint_options *options,
	pgm_operation **operation,
	pgm_error **error)
{
	pgm_operation *created = NULL;
	int			status;

	if (error != NULL)
		*error = NULL;
	if (operation == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"checkpoint operation output is required");
	*operation = NULL;
	if (!instance_is_valid(instance) || options == NULL ||
		options->struct_size < sizeof(*options) || options->flags != 0)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid checkpoint operation arguments");
	if (!process_is_valid(instance->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"instance belongs to a different host process");
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
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
	created->magic = PGM_OPERATION_MAGIC;
	created->owner_pid = getpid();
	created->instance = instance;
	created->kind = PGM_OPERATION_CHECKPOINT;
	created->state = PGM_OPERATION_PENDING;
	created->phase = PGM_OPERATION_PHASE_PENDING;
	created->operation_status = PGM_STATUS_OK;
	atomic_init(&created->lifecycle_state, UINT32_C(0));
	created->checkpoint_request = (PostgammaKernelCheckpointRequest)
		POSTGAMMA_KERNEL_CHECKPOINT_REQUEST_INIT;
	created->checkpoint_request.generation = instance->generation;
	status = postgamma_mutex_create(&created->mutex);
	if (status != 0)
		goto fail_locked;
	status = postgamma_instance_checkpoint_tracker_create(
		(PostgammaInstanceRuntime *) instance->kernel_result.runtime_handle,
		instance->generation, checkpoint_operation_completed, created,
		&created->tracker);
	if (status != 0)
		goto fail_locked;
	created->checkpoint_request.tracker = created->tracker;
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
		if (created->tracker != NULL)
			(void) postgamma_instance_checkpoint_tracker_destroy(
				created->tracker);
		if (created->mutex != NULL)
			(void) postgamma_mutex_destroy(created->mutex);
		created->magic = 0;
		free(created);
	}
	return return_simple_error(
		error, management_status_from_errno(status),
		"could not create the checkpoint operation");
}


pgm_status
pgm_operation_progress(
	pgm_operation *operation,
	pgm_operation_state *state,
	pgm_error **error)
{
	pgm_status	result_status = PGM_STATUS_OK;
	uint64_t	wake_count = 0;
	int			status;

	if (error != NULL)
		*error = NULL;
	if (!operation_is_valid(operation) || state == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid management operation progress arguments");
	*state = PGM_OPERATION_PENDING;
	if (!process_is_valid(operation->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"management operation belongs to a different host process");
	if (public_instance_reentrant(operation->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	if (postgamma_logical_operation_kind(operation->kind))
		return postgamma_logical_operation_progress(operation, state, error);
	status = postgamma_instance_checkpoint_tracker_wake_drain(
		operation->tracker, &wake_count);
	if (status != 0 && status != EAGAIN)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not drain the operation waitable");
	status = postgamma_mutex_lock(operation->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the management operation");
	if (operation->free_pending)
	{
		(void) postgamma_mutex_unlock(operation->mutex);
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"management operation is pending deferred release");
	}
	if (operation->state == PGM_OPERATION_PENDING)
	{
		status = postgamma_supervisor_submit(
			operation->instance->supervisor,
			operation->instance->generation,
			POSTGAMMA_SUPERVISOR_CONTROL_CHECKPOINT,
			&operation->checkpoint_request, &operation->ticket);
		if (status == 0)
		{
			operation->state = PGM_OPERATION_RUNNING;
			operation->phase = PGM_OPERATION_PHASE_DATABASE;
		}
		else
		{
			pgm_status failure = management_status_from_errno(status);

			(void) postgamma_mutex_unlock(operation->mutex);
			(void) postgamma_instance_checkpoint_tracker_finish(
				operation->tracker, operation->instance->generation, status);
			*state = PGM_OPERATION_FAILED;
			return return_simple_error(
				error, failure,
				"could not submit the checkpoint operation");
		}
	}
	*state = operation->state;
	result_status = operation->operation_status;
	(void) postgamma_mutex_unlock(operation->mutex);
	(void) wake_count;
	if (result_status != PGM_STATUS_OK)
		return return_simple_error(
			error, result_status,
			result_status == PGM_STATUS_CANCELED ?
			"checkpoint operation was canceled" :
			"checkpoint operation failed");
	return PGM_STATUS_OK;
}


pgm_status
pgm_operation_waitable(
	pgm_operation *operation,
	int *file_descriptor,
	pgm_error **error)
{
	int			waitable;

	if (error != NULL)
		*error = NULL;
	if (!operation_is_valid(operation) || file_descriptor == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid management operation waitable arguments");
	*file_descriptor = -1;
	if (!process_is_valid(operation->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"management operation belongs to a different host process");
	if (public_instance_reentrant(operation->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	if (postgamma_logical_operation_kind(operation->kind))
		return postgamma_logical_operation_waitable(
			operation, file_descriptor, error);
	waitable = postgamma_instance_checkpoint_tracker_waitable_fd(
		operation->tracker);
	if (waitable < 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"management operation waitable is unavailable");
	*file_descriptor = waitable;
	return PGM_STATUS_OK;
}


pgm_status
pgm_operation_cancel(
	pgm_operation *operation,
	pgm_error **error)
{
	int			status;

	if (error != NULL)
		*error = NULL;
	if (!operation_is_valid(operation))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid management operation cancellation arguments");
	if (!process_is_valid(operation->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"management operation belongs to a different host process");
	if (public_instance_reentrant(operation->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	if (postgamma_logical_operation_kind(operation->kind))
		return postgamma_logical_operation_cancel(operation, error);
	status = postgamma_mutex_lock(operation->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the management operation");
	if (operation->state == PGM_OPERATION_CANCELED)
	{
		(void) postgamma_mutex_unlock(operation->mutex);
		return PGM_STATUS_OK;
	}
	if (operation->state != PGM_OPERATION_PENDING)
	{
		(void) postgamma_mutex_unlock(operation->mutex);
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"a dispatched checkpoint cannot be canceled");
	}
	operation->state = PGM_OPERATION_CANCELED;
	operation->operation_status = PGM_STATUS_CANCELED;
	(void) postgamma_mutex_unlock(operation->mutex);
	status = postgamma_instance_checkpoint_tracker_finish(
		operation->tracker, operation->instance->generation, ECANCELED);
	if (status != 0 && status != EALREADY)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not cancel the checkpoint operation");
	return PGM_STATUS_OK;
}


void
pgm_operation_free(pgm_operation *operation)
{
	pgm_operation_state state;

	if (!operation_is_valid(operation) ||
		!process_is_valid(operation->owner_pid) ||
		public_instance_reentrant(operation->instance) ||
		postgamma_mutex_lock(operation->mutex) != 0)
		return;
	if (postgamma_logical_operation_kind(operation->kind))
	{
		(void) postgamma_mutex_unlock(operation->mutex);
		postgamma_logical_operation_free(operation);
		return;
	}
	state = operation->state;
	if (state == PGM_OPERATION_RUNNING)
	{
		operation->free_pending = true;
		(void) postgamma_mutex_unlock(operation->mutex);
		return;
	}
	if (state == PGM_OPERATION_PENDING)
	{
		operation->free_pending = true;
		operation->state = PGM_OPERATION_CANCELED;
		operation->operation_status = PGM_STATUS_CANCELED;
		(void) postgamma_mutex_unlock(operation->mutex);
		(void) postgamma_instance_checkpoint_tracker_finish(
			operation->tracker, operation->instance->generation, ECANCELED);
		return;
	}
	(void) postgamma_mutex_unlock(operation->mutex);
	(void) destroy_operation(operation);
}


bool
operation_is_valid(const pgm_operation *operation)
{
	return operation != NULL && operation->magic == PGM_OPERATION_MAGIC &&
		instance_is_valid(operation->instance) && operation->mutex != NULL &&
		operation->tracker != NULL;
}


static pgm_status
management_status_from_errno(int status)
{
	switch (status)
	{
		case 0:
			return PGM_STATUS_OK;
		case EINVAL:
			return PGM_STATUS_INVALID_ARGUMENT;
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


static void
checkpoint_operation_completed(
	void *argument, uint64_t generation, int operation_status)
{
	pgm_operation *operation = argument;
	bool		free_pending;

	if (!operation_is_valid(operation) ||
		operation->instance->generation != generation ||
		postgamma_mutex_lock(operation->mutex) != 0)
		return;
	if (operation_status == ECANCELED ||
		operation->state == PGM_OPERATION_CANCELED)
	{
		operation->state = PGM_OPERATION_CANCELED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status = PGM_STATUS_CANCELED;
	}
	else if (operation_status == 0)
	{
		operation->state = PGM_OPERATION_COMPLETED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status = PGM_STATUS_OK;
	}
	else
	{
		operation->state = PGM_OPERATION_FAILED;
		operation->phase = PGM_OPERATION_PHASE_TERMINAL;
		operation->operation_status =
			management_status_from_errno(operation_status);
	}
	if (operation->ticket != NULL &&
		postgamma_supervisor_ticket_destroy(operation->ticket) == 0)
		operation->ticket = NULL;
	free_pending = operation->free_pending;
	if (!free_pending)
		(void) postgamma_instance_checkpoint_tracker_wake(operation->tracker);
	(void) postgamma_mutex_unlock(operation->mutex);
	if (free_pending)
		(void) destroy_operation(operation);
}


static int
destroy_operation(pgm_operation *operation)
{
	pgm_instance *instance;
	int			status;

	if (!operation_is_valid(operation))
		return EINVAL;
	instance = operation->instance;
	if (operation->ticket != NULL)
	{
		status = postgamma_supervisor_ticket_destroy(operation->ticket);
		if (status != 0)
			return status;
		operation->ticket = NULL;
	}
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
	operation->magic = 0;
	operation->instance = NULL;
	free(operation);
	return 0;
}
