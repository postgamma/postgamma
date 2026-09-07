/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "postgamma/private/public_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>


#define PGM_COPY_CANCEL_REDISPATCH_NS UINT64_C(10000000)


static pgm_io_state public_copy_progress(
	PostgammaPrivateCopyProgress progress);
static pgm_status copy_lock(pgm_copy *copy, pgm_error **error);
static void copy_release(pgm_copy *copy);
static void copy_dispatch_and_release(pgm_copy *copy);
static pgm_status copy_private_error(
	pgm_error **error, PostgammaPrivateLibpqStatus status,
	const char *operation);
static pgm_status set_finish_message(
	pgm_copy *copy, const char *message, size_t message_size,
	pgm_error **error);
static pgm_status wait_for_copy_progress(
	pgm_copy *copy, uint64_t deadline_ns, pgm_error **error);


pgm_status
pgm_result_take_copy(
	pgm_result **result, pgm_copy **copy, pgm_error **error)
{
	pgm_result *source;
	pgm_request *request;
	pgm_copy   *created = NULL;
	PostgammaPrivateCopyDirection direction;
	PostgammaPrivateLibpqStatus private_status;
	pgm_result_status result_status;
	int			status;

	if (error != NULL)
		*error = NULL;
	if (copy == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"COPY output parameter is required");
	*copy = NULL;
	if (result == NULL || !result_is_valid(*result))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"a valid COPY-start result is required");
	source = *result;
	result_status = pgm_result_kind(source);
	if ((result_status != PGM_RESULT_COPY_IN &&
		 result_status != PGM_RESULT_COPY_OUT) || source->lease == NULL ||
		source->lease->owner == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"result does not own an active COPY transfer");
	request = source->lease->owner;
	if (!request_is_valid(request) || !process_is_valid(request->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"COPY result belongs to a different host process");
	if (public_instance_reentrant(request->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return return_simple_error(
			error, PGM_STATUS_OUT_OF_MEMORY,
			"could not allocate COPY handle");
	status = postgamma_mutex_create(&created->mutex);
	if (status != 0)
	{
		free(created);
		return return_simple_error(
			error, PGM_STATUS_OUT_OF_MEMORY,
			"could not initialize COPY handle");
	}
	status = postgamma_mutex_lock(request->mutex);
	if (status != 0)
	{
		(void) postgamma_mutex_destroy(created->mutex);
		free(created);
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the COPY request");
	}
	if (request->active_copy != NULL || request->free_pending ||
		request->result_lease != source->lease ||
		!atomic_load_explicit(
			&source->lease->outstanding, memory_order_acquire))
	{
		(void) postgamma_mutex_unlock(request->mutex);
		(void) postgamma_mutex_destroy(created->mutex);
		free(created);
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"COPY request already has an active transfer");
	}
	private_status = postgamma_private_libpq_copy_direction(
		request->operation, &direction);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK ||
		(direction != POSTGAMMA_PRIVATE_COPY_IN &&
		 direction != POSTGAMMA_PRIVATE_COPY_OUT) ||
		(direction == POSTGAMMA_PRIVATE_COPY_IN) !=
		(result_status == PGM_RESULT_COPY_IN))
	{
		(void) postgamma_mutex_unlock(request->mutex);
		(void) postgamma_mutex_destroy(created->mutex);
		free(created);
		return copy_private_error(
			error,
			private_status == POSTGAMMA_PRIVATE_LIBPQ_OK ?
			POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION : private_status,
			"take COPY ownership");
	}
	created->magic = PGM_COPY_MAGIC;
	created->owner_pid = request->owner_pid;
	atomic_init(&created->io_owner_active, false);
	created->request = request;
	created->lease = source->lease;
	created->direction = direction;
	created->state = POSTGAMMA_PUBLIC_COPY_ACTIVE;
	request->active_copy = created;
	(void) postgamma_mutex_unlock(request->mutex);

	postgamma_private_libpq_result_free(source->private_result);
	source->private_result = NULL;
	source->lease = NULL;
	source->magic = 0;
	free(source);
	*result = NULL;
	*copy = created;
	return PGM_STATUS_OK;
}


pgm_status
pgm_copy_write(
	pgm_copy *copy, const void *data, size_t size, size_t *consumed,
	pgm_io_state *state, pgm_error **error)
{
	PostgammaPrivateCopyProgress private_progress;
	PostgammaPrivateLibpqStatus private_status;
	pgm_status	status;

	if (consumed != NULL)
		*consumed = 0;
	if (state != NULL)
		*state = PGM_IO_AGAIN;
	status = copy_lock(copy, error);
	if (status != PGM_STATUS_OK)
		return status;
	if (consumed == NULL || state == NULL || (size != 0 && data == NULL) ||
		copy->direction != POSTGAMMA_PRIVATE_COPY_IN ||
		copy->state != POSTGAMMA_PUBLIC_COPY_ACTIVE)
	{
		copy_release(copy);
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid COPY IN write arguments or state");
	}
	private_status = postgamma_private_libpq_copy_write(
		copy->request->operation, data, size, consumed, &private_progress);
	if (private_status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		*state = public_copy_progress(private_progress);
	copy_dispatch_and_release(copy);
	return private_status == POSTGAMMA_PRIVATE_LIBPQ_OK ? PGM_STATUS_OK :
		copy_private_error(error, private_status, "write COPY IN data");
}


pgm_status
pgm_copy_finish(
	pgm_copy *copy, const char *failure_message, size_t failure_message_size,
	pgm_io_state *state, pgm_error **error)
{
	PostgammaPrivateCopyProgress private_progress;
	PostgammaPrivateLibpqStatus private_status;
	pgm_status	status;

	if (state != NULL)
		*state = PGM_IO_AGAIN;
	status = copy_lock(copy, error);
	if (status != PGM_STATUS_OK)
		return status;
	if (state == NULL || copy->direction != POSTGAMMA_PRIVATE_COPY_IN ||
		(copy->state != POSTGAMMA_PUBLIC_COPY_ACTIVE &&
		 copy->state != POSTGAMMA_PUBLIC_COPY_FINISHING))
	{
		copy_release(copy);
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid COPY IN finish arguments or state");
	}
	status = set_finish_message(
		copy, failure_message, failure_message_size, error);
	if (status != PGM_STATUS_OK)
	{
		copy_release(copy);
		return status;
	}
	copy->state = POSTGAMMA_PUBLIC_COPY_FINISHING;
	private_status = postgamma_private_libpq_copy_finish(
		copy->request->operation, copy->finish_message, &private_progress);
	if (private_status == POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		*state = public_copy_progress(private_progress);
		if (private_progress == POSTGAMMA_PRIVATE_COPY_END)
		{
			copy->state = POSTGAMMA_PUBLIC_COPY_ENDED;
			copy->io_ended = true;
		}
	}
	copy_dispatch_and_release(copy);
	return private_status == POSTGAMMA_PRIVATE_LIBPQ_OK ? PGM_STATUS_OK :
		copy_private_error(error, private_status, "finish COPY IN");
}


pgm_status
pgm_copy_read(
	pgm_copy *copy, void *buffer, size_t capacity, size_t *produced,
	pgm_io_state *state, pgm_error **error)
{
	PostgammaPrivateCopyProgress private_progress;
	PostgammaPrivateLibpqStatus private_status;
	pgm_status	status;

	if (produced != NULL)
		*produced = 0;
	if (state != NULL)
		*state = PGM_IO_AGAIN;
	status = copy_lock(copy, error);
	if (status != PGM_STATUS_OK)
		return status;
	if (buffer == NULL || capacity == 0 || produced == NULL || state == NULL ||
		copy->direction != POSTGAMMA_PRIVATE_COPY_OUT ||
		copy->state != POSTGAMMA_PUBLIC_COPY_ACTIVE)
	{
		copy_release(copy);
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid COPY OUT read arguments or state");
	}
	private_status = postgamma_private_libpq_copy_read(
		copy->request->operation, buffer, capacity, produced, &private_progress);
	if (private_status == POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		*state = public_copy_progress(private_progress);
		if (private_progress == POSTGAMMA_PRIVATE_COPY_END)
		{
			copy->state = POSTGAMMA_PUBLIC_COPY_ENDED;
			copy->io_ended = true;
		}
	}
	copy_dispatch_and_release(copy);
	return private_status == POSTGAMMA_PRIVATE_LIBPQ_OK ? PGM_STATUS_OK :
		copy_private_error(error, private_status, "read COPY OUT data");
}


pgm_status
pgm_copy_abort(
	pgm_copy *copy, const char *message, size_t message_size,
	pgm_error **error)
{
	static const char default_message[] = "COPY aborted by host";
	PostgammaPrivateCopyProgress private_progress;
	PostgammaPrivateLibpqStatus private_status;
	pgm_status	status;

	status = copy_lock(copy, error);
	if (status != PGM_STATUS_OK)
		return status;
	if (copy->state == POSTGAMMA_PUBLIC_COPY_ABORTED ||
		copy->state == POSTGAMMA_PUBLIC_COPY_ENDED)
	{
		copy_release(copy);
		return PGM_STATUS_OK;
	}
	if (message == NULL && message_size == 0)
	{
		message = default_message;
		message_size = sizeof(default_message) - 1;
	}
	status = set_finish_message(copy, message, message_size, error);
	if (status != PGM_STATUS_OK)
	{
		copy_release(copy);
		return status;
	}
	copy->state = POSTGAMMA_PUBLIC_COPY_ABORTED;
	if (copy->direction == POSTGAMMA_PRIVATE_COPY_IN)
	{
		private_status = postgamma_private_libpq_copy_finish(
			copy->request->operation, copy->finish_message, &private_progress);
		if (private_status == POSTGAMMA_PRIVATE_LIBPQ_OK &&
			private_progress == POSTGAMMA_PRIVATE_COPY_END)
			copy->io_ended = true;
	}
	else
		private_status = postgamma_private_libpq_cancel(
			copy->request->connection->private_connection,
			copy->request->generation);
	copy_dispatch_and_release(copy);
	return private_status == POSTGAMMA_PRIVATE_LIBPQ_OK ? PGM_STATUS_OK :
		copy_private_error(error, private_status, "abort COPY");
}


pgm_status
pgm_copy_close(pgm_copy *copy, int64_t timeout_ms, pgm_error **error)
{
	static const char close_message[] = "COPY closed before completion";
	unsigned char discard[8192];
	uint64_t	deadline_ns;
	uint64_t	next_cancel_ns = 0;
	bool		destroy_request_after_close = false;
	pgm_request *request;
	PostgammaPrivateLibpqStatus private_status;
	pgm_status	status;
	int			lock_status;

	status = copy_lock(copy, error);
	if (status != PGM_STATUS_OK)
		return status;
	if (deadline_from_timeout(timeout_ms, &deadline_ns) != 0)
	{
		copy_release(copy);
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid COPY close timeout");
	}
	request = copy->request;
	if (!copy->io_ended && copy->direction == POSTGAMMA_PRIVATE_COPY_IN &&
		copy->state == POSTGAMMA_PUBLIC_COPY_ACTIVE)
	{
		status = set_finish_message(
			copy, close_message, sizeof(close_message) - 1, error);
		if (status != PGM_STATUS_OK)
			goto fail;
		copy->state = POSTGAMMA_PUBLIC_COPY_ABORTED;
	}
	while (!copy->io_ended)
	{
		PostgammaPrivateCopyProgress private_progress;

		if (deadline_ns != POSTGAMMA_SUPERVISOR_NO_DEADLINE &&
			postgamma_monotonic_now_ns() >= deadline_ns)
		{
			status = PGM_STATUS_TIMEOUT;
			goto fail;
		}
		if (copy->direction == POSTGAMMA_PRIVATE_COPY_IN)
		{
			private_status = postgamma_private_libpq_copy_finish(
				request->operation, copy->finish_message, &private_progress);
		}
		else
		{
			size_t produced;
			uint64_t now_ns = postgamma_monotonic_now_ns();

			if (copy->state == POSTGAMMA_PUBLIC_COPY_ACTIVE)
				copy->state = POSTGAMMA_PUBLIC_COPY_ABORTED;
			if (next_cancel_ns == 0 || now_ns >= next_cancel_ns)
			{
				private_status = postgamma_private_libpq_cancel(
					request->connection->private_connection,
					request->generation);
				if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
					goto private_fail;
				next_cancel_ns = now_ns + PGM_COPY_CANCEL_REDISPATCH_NS;
			}
			private_status = postgamma_private_libpq_copy_read(
				request->operation, discard, sizeof(discard), &produced,
				&private_progress);
		}
		if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
			goto private_fail;
		if (private_progress == POSTGAMMA_PRIVATE_COPY_END)
		{
			copy->io_ended = true;
			break;
		}
		if (private_progress == POSTGAMMA_PRIVATE_COPY_AGAIN)
		{
			status = wait_for_copy_progress(copy, deadline_ns, error);
			if (status != PGM_STATUS_OK)
				goto fail;
		}
	}
	private_status = postgamma_private_libpq_copy_release(request->operation);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		goto private_fail;
	lock_status = postgamma_mutex_lock(request->mutex);
	if (lock_status != 0)
	{
		status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	if (request->active_copy != copy)
	{
		(void) postgamma_mutex_unlock(request->mutex);
		status = PGM_STATUS_INTERNAL_ERROR;
		goto fail;
	}
	request->active_copy = NULL;
	destroy_request_after_close = request->free_pending;
	(void) postgamma_mutex_unlock(request->mutex);
	atomic_store_explicit(&copy->lease->outstanding, false, memory_order_release);
	result_lease_release(copy->lease);
	copy->lease = NULL;
	free(copy->finish_message);
	copy->finish_message = NULL;
	(void) postgamma_mutex_unlock(copy->mutex);
	postgamma_public_dispatch_progress_events(request);
	copy->magic = 0;
	(void) postgamma_mutex_destroy(copy->mutex);
	free(copy);
	if (destroy_request_after_close)
		request_destroy(request);
	return PGM_STATUS_OK;

private_fail:
	status = status_from_private(private_status);
	(void) copy_private_error(error, private_status, "close COPY");
fail:
	copy_dispatch_and_release(copy);
	if (status == PGM_STATUS_TIMEOUT && (error == NULL || *error == NULL))
		(void) return_simple_error(
			error, status, "COPY close timed out");
	else if (status == PGM_STATUS_INTERNAL_ERROR &&
			 (error == NULL || *error == NULL))
		(void) return_simple_error(
			error, status, "could not close COPY handle");
	return status;
}


static pgm_io_state
public_copy_progress(PostgammaPrivateCopyProgress progress)
{
	switch (progress)
	{
		case POSTGAMMA_PRIVATE_COPY_PROGRESS:
			return PGM_IO_PROGRESS;
		case POSTGAMMA_PRIVATE_COPY_AGAIN:
			return PGM_IO_AGAIN;
		case POSTGAMMA_PRIVATE_COPY_END:
			return PGM_IO_END;
	}
	return PGM_IO_AGAIN;
}


static pgm_status
copy_lock(pgm_copy *copy, pgm_error **error)
{
	bool		expected = false;
	int			status;

	if (error != NULL)
		*error = NULL;
	if (!copy_is_valid(copy))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT, "invalid COPY handle");
	if (!process_is_valid(copy->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"COPY handle belongs to a different host process");
	if (public_instance_reentrant(copy->request->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	if (!atomic_compare_exchange_strong_explicit(
			&copy->io_owner_active, &expected, true,
			memory_order_acquire, memory_order_relaxed))
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"COPY handle already has an active I/O owner");
	status = postgamma_mutex_lock(copy->mutex);
	if (status != 0)
	{
		atomic_store_explicit(
			&copy->io_owner_active, false, memory_order_release);
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock COPY handle");
	}
	return PGM_STATUS_OK;
}


static void
copy_release(pgm_copy *copy)
{
	(void) postgamma_mutex_unlock(copy->mutex);
	atomic_store_explicit(
		&copy->io_owner_active, false, memory_order_release);
}


static void
copy_dispatch_and_release(pgm_copy *copy)
{
	pgm_request *request = copy->request;

	(void) postgamma_mutex_unlock(copy->mutex);
	postgamma_public_dispatch_progress_events(request);
	atomic_store_explicit(
		&copy->io_owner_active, false, memory_order_release);
}


static pgm_status
copy_private_error(
	pgm_error **error, PostgammaPrivateLibpqStatus status,
	const char *operation)
{
	pgm_status	public_status = status_from_private(status);

	return_error(error, make_error(
		public_status, NULL, NULL, NULL, "could not %s: %s", operation,
		postgamma_private_libpq_status_name(status)));
	return public_status;
}


static pgm_status
set_finish_message(
	pgm_copy *copy, const char *message, size_t message_size,
	pgm_error **error)
{
	char	   *created = NULL;

	if ((message_size != 0 && message == NULL) ||
		(message != NULL && memchr(message, '\0', message_size) != NULL) ||
		message_size == SIZE_MAX)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"COPY failure message is invalid");
	if (copy->finish_message_set)
	{
		if (copy->finish_message_size != message_size ||
			(message_size != 0 &&
			 memcmp(copy->finish_message, message, message_size) != 0))
			return return_simple_error(
				error, PGM_STATUS_INVALID_ARGUMENT,
				"COPY finish message changed while retrying");
		return PGM_STATUS_OK;
	}
	if (message_size != 0)
	{
		created = malloc(message_size + 1);
		if (created == NULL)
			return return_simple_error(
				error, PGM_STATUS_OUT_OF_MEMORY,
				"could not retain COPY failure message");
		memcpy(created, message, message_size);
		created[message_size] = '\0';
	}
	copy->finish_message = created;
	copy->finish_message_size = message_size;
	copy->finish_message_set = true;
	return PGM_STATUS_OK;
}


static pgm_status
wait_for_copy_progress(
	pgm_copy *copy, uint64_t deadline_ns, pgm_error **error)
{
	int64_t		private_deadline =
		deadline_ns == POSTGAMMA_SUPERVISOR_NO_DEADLINE ?
		POSTGAMMA_MEMORY_NO_DEADLINE : (int64_t) deadline_ns;
	PostgammaPrivateLibpqStatus private_status =
		postgamma_private_libpq_operation_wait(
			copy->request->operation, private_deadline);

	if (private_status == POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT)
		return return_simple_error(
			error, PGM_STATUS_TIMEOUT, "COPY progress timed out");
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		return copy_private_error(error, private_status, "wait for COPY progress");
	return PGM_STATUS_OK;
}
