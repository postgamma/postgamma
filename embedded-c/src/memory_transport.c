/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "postgamma/private/memory_transport.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define POSTGAMMA_MEMORY_TRANSPORT_MAGIC UINT64_C(0x50474D454D545250)
#define POSTGAMMA_MEMORY_ENDPOINT_MAGIC UINT64_C(0x50474D454D454E44)
#define POSTGAMMA_MEMORY_FRONTEND 0U
#define POSTGAMMA_MEMORY_BACKEND 1U

typedef struct PostgammaMemoryQueue
{
	unsigned char *bytes;
	size_t		head;
	size_t		length;
	uint64_t	total_written;
	uint64_t	total_read;
	bool		writer_closed;
	bool		reader_closed;
} PostgammaMemoryQueue;

typedef struct PostgammaMemoryTransport PostgammaMemoryTransport;

struct PostgammaMemoryEndpoint
{
	uint64_t	magic;
	PostgammaMemoryTransport *transport;
	unsigned int side;
	unsigned int references;
	PostgammaMemoryNotifyFunction notify;
	void	   *notify_argument;
};

struct PostgammaMemoryTransport
{
	uint64_t	magic;
	uint64_t	generation;
	size_t		capacity;
	size_t		allocation_size;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	PostgammaMemoryQueue queues[2];
	PostgammaMemoryEndpoint endpoints[2];
	uint64_t	cancel_epoch;
	uint64_t	wait_calls;
	uint64_t	wait_wakeups;
	uint64_t	wait_timeouts;
	uint64_t	wait_cancellations;
	uint64_t	simultaneous_full_observations;
	uint32_t	pin_reasons;
	uint32_t	carrier_retained;
	PostgammaMemoryResultPolicy result_policy;
	bool		aborted;
	PostgammaMemoryAbortReason abort_reason;
};

static _Atomic uint64_t PostgammaActiveMemoryTransports;
static _Atomic uint64_t PostgammaMemoryEndpointReferences;
static _Atomic uint64_t PostgammaMemoryAllocatedBytes;


static uint32_t postgamma_memory_ready_events(
	PostgammaMemoryEndpoint *endpoint, uint32_t requested);


static PostgammaMemoryIoResult
postgamma_memory_io_result(PostgammaMemoryStatus status, size_t bytes,
						   PostgammaMemoryAbortReason reason)
{
	return (PostgammaMemoryIoResult) {
		.status = status,
		.bytes = bytes,
		.abort_reason = reason,
	};
}


static PostgammaMemoryWaitResult
postgamma_memory_wait_result(PostgammaMemoryStatus status, uint32_t events,
							 uint64_t cancel_epoch,
							 PostgammaMemoryAbortReason reason)
{
	return (PostgammaMemoryWaitResult) {
		.status = status,
		.events = events,
		.cancel_epoch = cancel_epoch,
		.abort_reason = reason,
	};
}


static PostgammaMemoryStatus
postgamma_memory_lock_endpoint(PostgammaMemoryEndpoint *endpoint,
						   uint64_t generation,
						   PostgammaMemoryTransport **transport)
{
	PostgammaMemoryTransport *owner;

	if (endpoint == NULL || transport == NULL)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	if (endpoint->magic != POSTGAMMA_MEMORY_ENDPOINT_MAGIC ||
		endpoint->transport == NULL)
		return POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION;
	owner = endpoint->transport;
	if (pthread_mutex_lock(&owner->mutex) != 0)
		return POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
	if (owner->magic != POSTGAMMA_MEMORY_TRANSPORT_MAGIC ||
		endpoint->references == 0 || endpoint->side > POSTGAMMA_MEMORY_BACKEND)
	{
		(void) pthread_mutex_unlock(&owner->mutex);
		return POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION;
	}
	if (owner->generation != generation)
	{
		(void) pthread_mutex_unlock(&owner->mutex);
		return POSTGAMMA_MEMORY_STATUS_STALE_GENERATION;
	}
	*transport = owner;
	return POSTGAMMA_MEMORY_STATUS_OK;
}


static PostgammaMemoryStatus
postgamma_memory_unlock(PostgammaMemoryTransport *transport)
{
	return pthread_mutex_unlock(&transport->mutex) == 0 ?
		POSTGAMMA_MEMORY_STATUS_OK : POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
}


static void
postgamma_memory_broadcast(PostgammaMemoryTransport *transport)
{
	(void) pthread_cond_broadcast(&transport->condition);
}


static void
postgamma_memory_notify_endpoint(PostgammaMemoryEndpoint *endpoint)
{
	if (endpoint->notify != NULL)
		endpoint->notify(
			endpoint->notify_argument,
			postgamma_memory_ready_events(
				endpoint, POSTGAMMA_MEMORY_WAIT_ALL));
}


static void
postgamma_memory_notify_both(PostgammaMemoryTransport *transport)
{
	postgamma_memory_notify_endpoint(&transport->endpoints[0]);
	postgamma_memory_notify_endpoint(&transport->endpoints[1]);
}


static PostgammaMemoryQueue *
postgamma_memory_outbound(PostgammaMemoryEndpoint *endpoint)
{
	return &endpoint->transport->queues[endpoint->side];
}


static PostgammaMemoryQueue *
postgamma_memory_inbound(PostgammaMemoryEndpoint *endpoint)
{
	return &endpoint->transport->queues[1U - endpoint->side];
}


static size_t
postgamma_memory_queue_write(PostgammaMemoryQueue *queue, size_t capacity,
						 const unsigned char *source, size_t length)
{
	size_t		available = capacity - queue->length;
	size_t		amount = length < available ? length : available;
	size_t		tail = (queue->head + queue->length) % capacity;
	size_t		first = amount < capacity - tail ? amount : capacity - tail;

	memcpy(queue->bytes + tail, source, first);
	memcpy(queue->bytes, source + first, amount - first);
	queue->length += amount;
	queue->total_written += amount;
	return amount;
}


static size_t
postgamma_memory_queue_read(PostgammaMemoryQueue *queue, size_t capacity,
						unsigned char *destination, size_t length)
{
	size_t		amount = length < queue->length ? length : queue->length;
	size_t		first = amount < capacity - queue->head ?
		amount : capacity - queue->head;

	memcpy(destination, queue->bytes + queue->head, first);
	memcpy(destination + first, queue->bytes, amount - first);
	queue->head = (queue->head + amount) % capacity;
	queue->length -= amount;
	queue->total_read += amount;
	return amount;
}


static uint32_t
postgamma_memory_ready_events(PostgammaMemoryEndpoint *endpoint,
						  uint32_t requested)
{
	PostgammaMemoryQueue *inbound = postgamma_memory_inbound(endpoint);
	PostgammaMemoryQueue *outbound = postgamma_memory_outbound(endpoint);
	PostgammaMemoryTransport *transport = endpoint->transport;
	uint32_t	ready = 0;

	if ((requested & POSTGAMMA_MEMORY_WAIT_READABLE) != 0 &&
		inbound->length != 0)
		ready |= POSTGAMMA_MEMORY_WAIT_READABLE;
	if ((requested & POSTGAMMA_MEMORY_WAIT_WRITABLE) != 0 &&
		outbound->length < transport->capacity &&
		!outbound->writer_closed && !outbound->reader_closed)
		ready |= POSTGAMMA_MEMORY_WAIT_WRITABLE;
	if ((requested & POSTGAMMA_MEMORY_WAIT_PEER_CLOSED) != 0 &&
		(inbound->writer_closed || outbound->reader_closed))
		ready |= POSTGAMMA_MEMORY_WAIT_PEER_CLOSED;
	return ready;
}


PostgammaMemoryStatus
postgamma_memory_transport_create(uint64_t generation, size_t capacity,
						  PostgammaMemoryEndpoint **frontend,
						  PostgammaMemoryEndpoint **backend)
{
	PostgammaMemoryTransport *transport;
	pthread_condattr_t attributes;
	bool		mutex_initialized = false;
	bool		attributes_initialized = false;
	bool		condition_initialized = false;
	size_t		allocation_size;
	int			status;

	if (frontend == NULL || backend == NULL || frontend == backend)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	*frontend = NULL;
	*backend = NULL;
	if (generation == 0 || capacity == 0)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	if (capacity > (SIZE_MAX - sizeof(*transport)) / 2)
		return POSTGAMMA_MEMORY_STATUS_OVERFLOW;
	allocation_size = sizeof(*transport) + 2 * capacity;
	transport = calloc(1, sizeof(*transport));
	if (transport == NULL)
		return POSTGAMMA_MEMORY_STATUS_NO_MEMORY;
	transport->queues[0].bytes = malloc(capacity);
	transport->queues[1].bytes = malloc(capacity);
	if (transport->queues[0].bytes == NULL || transport->queues[1].bytes == NULL)
	{
		free(transport->queues[1].bytes);
		free(transport->queues[0].bytes);
		free(transport);
		return POSTGAMMA_MEMORY_STATUS_NO_MEMORY;
	}
	status = pthread_mutex_init(&transport->mutex, NULL);
	if (status == 0)
		mutex_initialized = true;
	if (status == 0)
	{
		status = pthread_condattr_init(&attributes);
		if (status == 0)
			attributes_initialized = true;
	}
	if (status == 0)
		status = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (status == 0)
	{
		status = pthread_cond_init(&transport->condition, &attributes);
		if (status == 0)
			condition_initialized = true;
	}
	if (attributes_initialized)
		(void) pthread_condattr_destroy(&attributes);
	if (status != 0)
	{
		if (condition_initialized)
			(void) pthread_cond_destroy(&transport->condition);
		if (mutex_initialized)
			(void) pthread_mutex_destroy(&transport->mutex);
		free(transport->queues[1].bytes);
		free(transport->queues[0].bytes);
		free(transport);
		return POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
	}
	transport->magic = POSTGAMMA_MEMORY_TRANSPORT_MAGIC;
	transport->generation = generation;
	transport->capacity = capacity;
	transport->allocation_size = allocation_size;
	transport->endpoints[0] = (PostgammaMemoryEndpoint) {
		.magic = POSTGAMMA_MEMORY_ENDPOINT_MAGIC,
		.transport = transport,
		.side = POSTGAMMA_MEMORY_FRONTEND,
		.references = 1,
	};
	transport->endpoints[1] = (PostgammaMemoryEndpoint) {
		.magic = POSTGAMMA_MEMORY_ENDPOINT_MAGIC,
		.transport = transport,
		.side = POSTGAMMA_MEMORY_BACKEND,
		.references = 1,
	};
	atomic_fetch_add_explicit(
		&PostgammaActiveMemoryTransports, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(
		&PostgammaMemoryEndpointReferences, 2, memory_order_relaxed);
	atomic_fetch_add_explicit(
		&PostgammaMemoryAllocatedBytes, allocation_size, memory_order_relaxed);
	*frontend = &transport->endpoints[0];
	*backend = &transport->endpoints[1];
	return POSTGAMMA_MEMORY_STATUS_OK;
}


PostgammaMemoryStatus
postgamma_memory_endpoint_retain(PostgammaMemoryEndpoint *endpoint,
						 uint64_t generation,
						 PostgammaMemoryEndpoint **retained)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryStatus status;

	if (retained == NULL)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	*retained = NULL;
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	if (endpoint->references == UINT_MAX)
		status = POSTGAMMA_MEMORY_STATUS_OVERFLOW;
	else
	{
		endpoint->references++;
		atomic_fetch_add_explicit(
			&PostgammaMemoryEndpointReferences, 1, memory_order_relaxed);
		*retained = endpoint;
	}
	if (postgamma_memory_unlock(transport) != POSTGAMMA_MEMORY_STATUS_OK)
		return POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
	return status;
}


PostgammaMemoryStatus
postgamma_memory_endpoint_set_notify(
	PostgammaMemoryEndpoint *endpoint, uint64_t generation,
	PostgammaMemoryNotifyFunction notify, void *notify_argument)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryStatus status;

	if (notify == NULL && notify_argument != NULL)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	endpoint->notify = notify;
	endpoint->notify_argument = notify_argument;
	if (notify != NULL)
		postgamma_memory_notify_endpoint(endpoint);
	return postgamma_memory_unlock(transport);
}


PostgammaMemoryStatus
postgamma_memory_endpoint_release(PostgammaMemoryEndpoint **endpoint,
						  uint64_t generation)
{
	PostgammaMemoryEndpoint *released;
	PostgammaMemoryTransport *transport;
	PostgammaMemoryQueue *outbound;
	PostgammaMemoryQueue *inbound;
	PostgammaMemoryStatus status;
	bool		destroy;
	int			condition_status = 0;
	int			mutex_status = 0;

	if (endpoint == NULL || *endpoint == NULL)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	released = *endpoint;
	status = postgamma_memory_lock_endpoint(released, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	released->references--;
	atomic_fetch_sub_explicit(
		&PostgammaMemoryEndpointReferences, 1, memory_order_relaxed);
	if (released->references == 0)
	{
		outbound = postgamma_memory_outbound(released);
		inbound = postgamma_memory_inbound(released);
		outbound->writer_closed = true;
		inbound->reader_closed = true;
		released->notify = NULL;
		released->notify_argument = NULL;
		postgamma_memory_broadcast(transport);
		postgamma_memory_notify_endpoint(
			&transport->endpoints[1U - released->side]);
	}
	destroy = transport->endpoints[0].references == 0 &&
		transport->endpoints[1].references == 0;
	*endpoint = NULL;
	status = postgamma_memory_unlock(transport);
	if (!destroy)
		return status;
	transport->magic = 0;
	transport->endpoints[0].magic = 0;
	transport->endpoints[1].magic = 0;
	condition_status = pthread_cond_destroy(&transport->condition);
	mutex_status = pthread_mutex_destroy(&transport->mutex);
	atomic_fetch_sub_explicit(
		&PostgammaActiveMemoryTransports, 1, memory_order_relaxed);
	atomic_fetch_sub_explicit(
		&PostgammaMemoryAllocatedBytes,
		transport->allocation_size,
		memory_order_relaxed);
	free(transport->queues[1].bytes);
	free(transport->queues[0].bytes);
	free(transport);
	return status == POSTGAMMA_MEMORY_STATUS_OK && condition_status == 0 &&
		mutex_status == 0 ? POSTGAMMA_MEMORY_STATUS_OK :
		POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION;
}


PostgammaMemoryIoResult
postgamma_memory_endpoint_read(PostgammaMemoryEndpoint *endpoint,
						   uint64_t generation, void *buffer, size_t length)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryQueue *queue;
	PostgammaMemoryStatus status;
	PostgammaMemoryAbortReason reason = POSTGAMMA_MEMORY_ABORT_NONE;
	size_t		amount;

	if (buffer == NULL || length == 0)
		return postgamma_memory_io_result(
			POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT, 0,
			POSTGAMMA_MEMORY_ABORT_NONE);
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return postgamma_memory_io_result(
			status, 0, POSTGAMMA_MEMORY_ABORT_NONE);
	queue = postgamma_memory_inbound(endpoint);
	if (transport->aborted)
		status = POSTGAMMA_MEMORY_STATUS_ABORTED;
	else if (queue->length == 0)
		status = queue->writer_closed ? POSTGAMMA_MEMORY_STATUS_EOF :
			POSTGAMMA_MEMORY_STATUS_RETRY;
	else
	{
		amount = postgamma_memory_queue_read(
			queue, transport->capacity, buffer, length);
		postgamma_memory_broadcast(transport);
		postgamma_memory_notify_endpoint(
			&transport->endpoints[1U - endpoint->side]);
		(void) postgamma_memory_unlock(transport);
		return postgamma_memory_io_result(
			POSTGAMMA_MEMORY_STATUS_PROGRESS, amount,
			POSTGAMMA_MEMORY_ABORT_NONE);
	}
	if (transport->aborted)
		reason = transport->abort_reason;
	(void) postgamma_memory_unlock(transport);
	return postgamma_memory_io_result(status, 0, reason);
}


PostgammaMemoryIoResult
postgamma_memory_endpoint_write(PostgammaMemoryEndpoint *endpoint,
							uint64_t generation, const void *buffer,
							size_t length)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryQueue *queue;
	PostgammaMemoryStatus status;
	PostgammaMemoryAbortReason reason = POSTGAMMA_MEMORY_ABORT_NONE;
	size_t		amount;

	if (buffer == NULL || length == 0)
		return postgamma_memory_io_result(
			POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT, 0,
			POSTGAMMA_MEMORY_ABORT_NONE);
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return postgamma_memory_io_result(
			status, 0, POSTGAMMA_MEMORY_ABORT_NONE);
	queue = postgamma_memory_outbound(endpoint);
	if (transport->aborted)
		status = POSTGAMMA_MEMORY_STATUS_ABORTED;
	else if (queue->writer_closed)
		status = POSTGAMMA_MEMORY_STATUS_LOCAL_CLOSED;
	else if (queue->reader_closed)
		status = POSTGAMMA_MEMORY_STATUS_PEER_CLOSED;
	else if (queue->length == transport->capacity)
		status = POSTGAMMA_MEMORY_STATUS_RETRY;
	else
	{
		amount = postgamma_memory_queue_write(
			queue, transport->capacity, buffer, length);
		if (transport->queues[POSTGAMMA_MEMORY_FRONTEND].length ==
				transport->capacity &&
			transport->queues[POSTGAMMA_MEMORY_BACKEND].length ==
				transport->capacity &&
			transport->simultaneous_full_observations != UINT64_MAX)
			transport->simultaneous_full_observations++;
		postgamma_memory_broadcast(transport);
		postgamma_memory_notify_endpoint(
			&transport->endpoints[1U - endpoint->side]);
		(void) postgamma_memory_unlock(transport);
		return postgamma_memory_io_result(
			POSTGAMMA_MEMORY_STATUS_PROGRESS, amount,
			POSTGAMMA_MEMORY_ABORT_NONE);
	}
	if (transport->aborted)
		reason = transport->abort_reason;
	(void) postgamma_memory_unlock(transport);
	return postgamma_memory_io_result(status, 0, reason);
}


PostgammaMemoryWaitResult
postgamma_memory_endpoint_wait(PostgammaMemoryEndpoint *endpoint,
						   uint64_t generation, uint32_t events,
						   uint64_t observed_cancel_epoch,
						   int64_t deadline_ns)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryQueue *inbound;
	PostgammaMemoryQueue *outbound;
	PostgammaMemoryStatus status;
	uint32_t	ready;
	int			wait_status;

	if (events == 0 || (events & ~POSTGAMMA_MEMORY_WAIT_ALL) != 0 ||
		deadline_ns < 0)
		return postgamma_memory_wait_result(
			POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT, 0,
			observed_cancel_epoch, POSTGAMMA_MEMORY_ABORT_NONE);
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return postgamma_memory_wait_result(
			status, 0, observed_cancel_epoch, POSTGAMMA_MEMORY_ABORT_NONE);
	inbound = postgamma_memory_inbound(endpoint);
	outbound = postgamma_memory_outbound(endpoint);
	transport->wait_calls++;
	for (;;)
	{
		if (transport->aborted)
		{
			status = POSTGAMMA_MEMORY_STATUS_ABORTED;
			break;
		}
		if (transport->cancel_epoch != observed_cancel_epoch)
		{
			transport->wait_cancellations++;
			status = POSTGAMMA_MEMORY_STATUS_CANCELLED;
			break;
		}
		ready = postgamma_memory_ready_events(endpoint, events);
		if (ready != 0)
		{
			PostgammaMemoryWaitResult result =
				postgamma_memory_wait_result(
					POSTGAMMA_MEMORY_STATUS_OK, ready,
					transport->cancel_epoch, POSTGAMMA_MEMORY_ABORT_NONE);

			(void) postgamma_memory_unlock(transport);
			return result;
		}
		if ((events & POSTGAMMA_MEMORY_WAIT_READABLE) != 0 &&
			inbound->length == 0 && inbound->writer_closed)
		{
			status = POSTGAMMA_MEMORY_STATUS_EOF;
			break;
		}
		if ((events & POSTGAMMA_MEMORY_WAIT_WRITABLE) != 0 &&
			outbound->reader_closed)
		{
			status = POSTGAMMA_MEMORY_STATUS_PEER_CLOSED;
			break;
		}
		if ((events & POSTGAMMA_MEMORY_WAIT_WRITABLE) != 0 &&
			outbound->writer_closed)
		{
			status = POSTGAMMA_MEMORY_STATUS_LOCAL_CLOSED;
			break;
		}
		if (deadline_ns == POSTGAMMA_MEMORY_NO_DEADLINE)
			wait_status = pthread_cond_wait(
				&transport->condition, &transport->mutex);
		else
		{
			struct timespec deadline = {
				.tv_sec = (time_t) (deadline_ns / INT64_C(1000000000)),
				.tv_nsec = (long) (deadline_ns % INT64_C(1000000000)),
			};

			wait_status = pthread_cond_timedwait(
				&transport->condition, &transport->mutex, &deadline);
		}
		if (wait_status == 0)
		{
			transport->wait_wakeups++;
			continue;
		}
		if (wait_status == ETIMEDOUT)
		{
			transport->wait_timeouts++;
			status = POSTGAMMA_MEMORY_STATUS_TIMEOUT;
			break;
		}
		status = POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
		break;
	}
	{
		PostgammaMemoryWaitResult result = postgamma_memory_wait_result(
			status, 0, transport->cancel_epoch,
			transport->aborted ? transport->abort_reason :
			POSTGAMMA_MEMORY_ABORT_NONE);

		(void) postgamma_memory_unlock(transport);
		return result;
	}
}


PostgammaMemoryStatus
postgamma_memory_endpoint_ready(PostgammaMemoryEndpoint *endpoint,
								uint64_t generation, uint32_t *events)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryStatus status;

	if (events == NULL)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	*events = 0;
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	if (transport->aborted)
		status = POSTGAMMA_MEMORY_STATUS_ABORTED;
	else
		*events = postgamma_memory_ready_events(
			endpoint, POSTGAMMA_MEMORY_WAIT_ALL);
	if (postgamma_memory_unlock(transport) != POSTGAMMA_MEMORY_STATUS_OK)
		return POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
	return status;
}


PostgammaMemoryStatus
postgamma_memory_endpoint_half_close_write(PostgammaMemoryEndpoint *endpoint,
								   uint64_t generation)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryQueue *queue;
	PostgammaMemoryStatus status;

	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	queue = postgamma_memory_outbound(endpoint);
	if (transport->aborted)
		status = POSTGAMMA_MEMORY_STATUS_ABORTED;
	else if (queue->writer_closed)
		status = POSTGAMMA_MEMORY_STATUS_LOCAL_CLOSED;
	else
	{
		queue->writer_closed = true;
		postgamma_memory_broadcast(transport);
		postgamma_memory_notify_both(transport);
	}
	if (postgamma_memory_unlock(transport) != POSTGAMMA_MEMORY_STATUS_OK)
		return POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
	return status;
}


PostgammaMemoryStatus
postgamma_memory_endpoint_abort(PostgammaMemoryEndpoint *endpoint,
							uint64_t generation,
							PostgammaMemoryAbortReason reason)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryStatus status;

	if (reason == POSTGAMMA_MEMORY_ABORT_NONE)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	if (transport->aborted)
		status = POSTGAMMA_MEMORY_STATUS_ABORTED;
	else
	{
		transport->aborted = true;
		transport->abort_reason = reason;
		postgamma_memory_broadcast(transport);
		postgamma_memory_notify_both(transport);
	}
	if (postgamma_memory_unlock(transport) != POSTGAMMA_MEMORY_STATUS_OK)
		return POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
	return status;
}


PostgammaMemoryStatus
postgamma_memory_endpoint_cancel_waits(PostgammaMemoryEndpoint *endpoint,
								   uint64_t generation,
								   uint64_t *new_cancel_epoch)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryStatus status;

	if (new_cancel_epoch == NULL)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	if (transport->cancel_epoch == UINT64_MAX)
		status = POSTGAMMA_MEMORY_STATUS_OVERFLOW;
	else
	{
		transport->cancel_epoch++;
		*new_cancel_epoch = transport->cancel_epoch;
		postgamma_memory_broadcast(transport);
		postgamma_memory_notify_both(transport);
	}
	if (postgamma_memory_unlock(transport) != POSTGAMMA_MEMORY_STATUS_OK)
		return POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
	return status;
}


static PostgammaMemoryQueueTelemetry
postgamma_memory_queue_telemetry(const PostgammaMemoryQueue *queue)
{
	return (PostgammaMemoryQueueTelemetry) {
		.pending_bytes = queue->length,
		.total_bytes_written = queue->total_written,
		.total_bytes_read = queue->total_read,
		.writer_closed = queue->writer_closed,
		.reader_closed = queue->reader_closed,
	};
}


PostgammaMemoryStatus
postgamma_memory_endpoint_set_session_status(
	PostgammaMemoryEndpoint *endpoint, uint64_t generation,
	uint32_t pin_reasons, uint32_t carrier_retained)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryStatus status;

	if ((pin_reasons & ~UINT32_C(0x1f)) != 0 || carrier_retained > 1)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	if (endpoint->side != POSTGAMMA_MEMORY_BACKEND)
	{
		(void) postgamma_memory_unlock(transport);
		return POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION;
	}
	transport->pin_reasons = pin_reasons;
	transport->carrier_retained = carrier_retained;
	return postgamma_memory_unlock(transport);
}


PostgammaMemoryStatus
postgamma_memory_endpoint_set_result_policy(
	PostgammaMemoryEndpoint *endpoint, uint64_t generation,
	const PostgammaMemoryResultPolicy *policy)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryStatus status;
	bool		clear_policy;

	if (policy == NULL)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	clear_policy = policy->request_generation == 0 &&
		policy->delivery_mode == POSTGAMMA_MEMORY_DELIVERY_MATERIALIZED &&
		policy->target_chunk_rows == 0 && policy->result_buffer_limit == 0 &&
		policy->maximum_value_size == 0;
	if (!clear_policy &&
		(policy->request_generation == 0 ||
		 policy->result_buffer_limit == 0 || policy->maximum_value_size == 0 ||
		 policy->maximum_value_size > policy->result_buffer_limit ||
		 (policy->delivery_mode != POSTGAMMA_MEMORY_DELIVERY_MATERIALIZED &&
		  policy->delivery_mode != POSTGAMMA_MEMORY_DELIVERY_CHUNKED) ||
		 (policy->delivery_mode == POSTGAMMA_MEMORY_DELIVERY_MATERIALIZED &&
		  policy->target_chunk_rows != 0) ||
		 (policy->delivery_mode == POSTGAMMA_MEMORY_DELIVERY_CHUNKED &&
		  policy->target_chunk_rows == 0)))
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	if (endpoint->side != POSTGAMMA_MEMORY_FRONTEND)
	{
		(void) postgamma_memory_unlock(transport);
		return POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION;
	}
	transport->result_policy = *policy;
	postgamma_memory_broadcast(transport);
	postgamma_memory_notify_both(transport);
	return postgamma_memory_unlock(transport);
}


PostgammaMemoryStatus
postgamma_memory_endpoint_get_result_policy(
	PostgammaMemoryEndpoint *endpoint, uint64_t generation,
	PostgammaMemoryResultPolicy *policy)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryStatus status;

	if (policy == NULL)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	*policy = transport->result_policy;
	return postgamma_memory_unlock(transport);
}


PostgammaMemoryStatus
postgamma_memory_endpoint_snapshot(PostgammaMemoryEndpoint *endpoint,
							   uint64_t generation,
							   PostgammaMemoryTelemetry *telemetry)
{
	PostgammaMemoryTransport *transport;
	PostgammaMemoryStatus status;

	if (telemetry == NULL)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	status = postgamma_memory_lock_endpoint(endpoint, generation, &transport);
	if (status != POSTGAMMA_MEMORY_STATUS_OK)
		return status;
	*telemetry = (PostgammaMemoryTelemetry) {
		.generation = transport->generation,
		.capacity = transport->capacity,
		.cancel_epoch = transport->cancel_epoch,
		.frontend_references = transport->endpoints[0].references,
		.backend_references = transport->endpoints[1].references,
		.wait_calls = transport->wait_calls,
		.wait_wakeups = transport->wait_wakeups,
		.wait_timeouts = transport->wait_timeouts,
		.wait_cancellations = transport->wait_cancellations,
		.simultaneous_full_observations =
			transport->simultaneous_full_observations,
		.pin_reasons = transport->pin_reasons,
		.carrier_retained = transport->carrier_retained,
		.aborted = transport->aborted,
		.abort_reason = transport->abort_reason,
		.frontend_to_backend =
			postgamma_memory_queue_telemetry(&transport->queues[0]),
		.backend_to_frontend =
			postgamma_memory_queue_telemetry(&transport->queues[1]),
	};
	return postgamma_memory_unlock(transport);
}


void
postgamma_memory_global_telemetry(PostgammaMemoryGlobalTelemetry *telemetry)
{
	if (telemetry == NULL)
		return;
	*telemetry = (PostgammaMemoryGlobalTelemetry) {
		.active_transports = atomic_load_explicit(
			&PostgammaActiveMemoryTransports, memory_order_relaxed),
		.endpoint_references = atomic_load_explicit(
			&PostgammaMemoryEndpointReferences, memory_order_relaxed),
		.allocated_bytes = atomic_load_explicit(
			&PostgammaMemoryAllocatedBytes, memory_order_relaxed),
	};
}


PostgammaMemoryStatus
postgamma_memory_clock_now(int64_t *now_ns)
{
	struct timespec now;

	if (now_ns == NULL)
		return POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
		(uint64_t) now.tv_sec >
		(uint64_t) INT64_MAX / UINT64_C(1000000000))
		return POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR;
	*now_ns = (int64_t) now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
	return POSTGAMMA_MEMORY_STATUS_OK;
}


const char *
postgamma_memory_status_name(PostgammaMemoryStatus status)
{
	switch (status)
	{
		case POSTGAMMA_MEMORY_STATUS_OK:
			return "ok";
		case POSTGAMMA_MEMORY_STATUS_PROGRESS:
			return "progress";
		case POSTGAMMA_MEMORY_STATUS_RETRY:
			return "retry";
		case POSTGAMMA_MEMORY_STATUS_EOF:
			return "eof";
		case POSTGAMMA_MEMORY_STATUS_TIMEOUT:
			return "timeout";
		case POSTGAMMA_MEMORY_STATUS_CANCELLED:
			return "cancelled";
		case POSTGAMMA_MEMORY_STATUS_STALE_GENERATION:
			return "stale-generation";
		case POSTGAMMA_MEMORY_STATUS_ABORTED:
			return "aborted";
		case POSTGAMMA_MEMORY_STATUS_PEER_CLOSED:
			return "peer-closed";
		case POSTGAMMA_MEMORY_STATUS_LOCAL_CLOSED:
			return "local-closed";
		case POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT:
			return "invalid-argument";
		case POSTGAMMA_MEMORY_STATUS_NO_MEMORY:
			return "no-memory";
		case POSTGAMMA_MEMORY_STATUS_OVERFLOW:
			return "overflow";
		case POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION:
			return "contract-violation";
		case POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR:
			return "internal-error";
	}
	return "unknown";
}
