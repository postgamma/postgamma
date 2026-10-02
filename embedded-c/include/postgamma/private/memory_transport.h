/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_MEMORY_TRANSPORT_H
#define POSTGAMMA_PRIVATE_MEMORY_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POSTGAMMA_MEMORY_WAIT_READABLE UINT32_C(0x01)
#define POSTGAMMA_MEMORY_WAIT_WRITABLE UINT32_C(0x02)
#define POSTGAMMA_MEMORY_WAIT_PEER_CLOSED UINT32_C(0x04)
#define POSTGAMMA_MEMORY_WAIT_ALL UINT32_C(0x07)
#define POSTGAMMA_MEMORY_NO_DEADLINE INT64_MAX

#define POSTGAMMA_MEMORY_DELIVERY_MATERIALIZED UINT32_C(0)
#define POSTGAMMA_MEMORY_DELIVERY_CHUNKED UINT32_C(1)

typedef enum PostgammaMemoryStatus
{
	POSTGAMMA_MEMORY_STATUS_OK = 0,
	POSTGAMMA_MEMORY_STATUS_PROGRESS,
	POSTGAMMA_MEMORY_STATUS_RETRY,
	POSTGAMMA_MEMORY_STATUS_EOF,
	POSTGAMMA_MEMORY_STATUS_TIMEOUT,
	POSTGAMMA_MEMORY_STATUS_CANCELLED,
	POSTGAMMA_MEMORY_STATUS_STALE_GENERATION,
	POSTGAMMA_MEMORY_STATUS_ABORTED,
	POSTGAMMA_MEMORY_STATUS_PEER_CLOSED,
	POSTGAMMA_MEMORY_STATUS_LOCAL_CLOSED,
	POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT,
	POSTGAMMA_MEMORY_STATUS_NO_MEMORY,
	POSTGAMMA_MEMORY_STATUS_OVERFLOW,
	POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION,
	POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR
} PostgammaMemoryStatus;

typedef enum PostgammaMemoryAbortReason
{
	POSTGAMMA_MEMORY_ABORT_NONE = 0,
	POSTGAMMA_MEMORY_ABORT_CANCELLED,
	POSTGAMMA_MEMORY_ABORT_PEER_FAILURE,
	POSTGAMMA_MEMORY_ABORT_DEADLINE,
	POSTGAMMA_MEMORY_ABORT_PROTOCOL,
	POSTGAMMA_MEMORY_ABORT_SHUTDOWN,
	POSTGAMMA_MEMORY_ABORT_INTERNAL
} PostgammaMemoryAbortReason;

typedef struct PostgammaMemoryEndpoint PostgammaMemoryEndpoint;

/*
 * Notification callbacks run while the transport mutex is held.  They must
 * be nonblocking and must not call back into the same memory transport.
 * postgamma_memory_endpoint_set_notify() takes the same mutex, so successfully
 * clearing a callback is a synchronous quiescence point: no invocation of the
 * removed callback remains in flight when the function returns.
 */
typedef void (*PostgammaMemoryNotifyFunction) (
	void *argument, uint32_t events);

typedef struct PostgammaMemoryIoResult
{
	PostgammaMemoryStatus status;
	size_t		bytes;
	PostgammaMemoryAbortReason abort_reason;
} PostgammaMemoryIoResult;

typedef struct PostgammaMemoryWaitResult
{
	PostgammaMemoryStatus status;
	uint32_t	events;
	uint64_t	cancel_epoch;
	PostgammaMemoryAbortReason abort_reason;
} PostgammaMemoryWaitResult;

typedef struct PostgammaMemoryQueueTelemetry
{
	size_t		pending_bytes;
	uint64_t	total_bytes_written;
	uint64_t	total_bytes_read;
	bool		writer_closed;
	bool		reader_closed;
} PostgammaMemoryQueueTelemetry;

typedef struct PostgammaMemoryTelemetry
{
	uint64_t	generation;
	size_t		capacity;
	uint64_t	cancel_epoch;
	unsigned int frontend_references;
	unsigned int backend_references;
	uint64_t	wait_calls;
	uint64_t	wait_wakeups;
	uint64_t	wait_timeouts;
	uint64_t	wait_cancellations;
	uint64_t	simultaneous_full_observations;
	uint32_t	pin_reasons;
	uint32_t	carrier_retained;
	bool		aborted;
	PostgammaMemoryAbortReason abort_reason;
	PostgammaMemoryQueueTelemetry frontend_to_backend;
	PostgammaMemoryQueueTelemetry backend_to_frontend;
} PostgammaMemoryTelemetry;

typedef struct PostgammaMemoryGlobalTelemetry
{
	uint64_t	active_transports;
	uint64_t	endpoint_references;
	uint64_t	allocated_bytes;
} PostgammaMemoryGlobalTelemetry;

typedef struct PostgammaMemoryResultPolicy
{
	uint64_t	request_generation;
	uint32_t	delivery_mode;
	uint32_t	target_chunk_rows;
	size_t		result_buffer_limit;
	size_t		maximum_value_size;
} PostgammaMemoryResultPolicy;

#define POSTGAMMA_MEMORY_RESULT_POLICY_INIT \
	{UINT64_C(0), POSTGAMMA_MEMORY_DELIVERY_MATERIALIZED, UINT32_C(0), 0, 0}

PostgammaMemoryStatus postgamma_memory_transport_create(
	uint64_t generation,
	size_t capacity,
	PostgammaMemoryEndpoint **frontend,
	PostgammaMemoryEndpoint **backend);
PostgammaMemoryStatus postgamma_memory_endpoint_retain(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	PostgammaMemoryEndpoint **retained);
PostgammaMemoryStatus postgamma_memory_endpoint_set_notify(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	PostgammaMemoryNotifyFunction notify,
	void *notify_argument);
PostgammaMemoryStatus postgamma_memory_endpoint_release(
	PostgammaMemoryEndpoint **endpoint,
	uint64_t generation);
PostgammaMemoryIoResult postgamma_memory_endpoint_read(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	void *buffer,
	size_t length);
PostgammaMemoryIoResult postgamma_memory_endpoint_write(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	const void *buffer,
	size_t length);
PostgammaMemoryWaitResult postgamma_memory_endpoint_wait(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	uint32_t events,
	uint64_t observed_cancel_epoch,
	int64_t deadline_ns);
PostgammaMemoryStatus postgamma_memory_endpoint_ready(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	uint32_t *events);
PostgammaMemoryStatus postgamma_memory_endpoint_half_close_write(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation);
PostgammaMemoryStatus postgamma_memory_endpoint_abort(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	PostgammaMemoryAbortReason reason);
PostgammaMemoryStatus postgamma_memory_endpoint_cancel_waits(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	uint64_t *new_cancel_epoch);
PostgammaMemoryStatus postgamma_memory_endpoint_set_session_status(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	uint32_t pin_reasons,
	uint32_t carrier_retained);
PostgammaMemoryStatus postgamma_memory_endpoint_set_result_policy(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	const PostgammaMemoryResultPolicy *policy);
PostgammaMemoryStatus postgamma_memory_endpoint_get_result_policy(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	PostgammaMemoryResultPolicy *policy);
PostgammaMemoryStatus postgamma_memory_endpoint_snapshot(
	PostgammaMemoryEndpoint *endpoint,
	uint64_t generation,
	PostgammaMemoryTelemetry *telemetry);
void postgamma_memory_global_telemetry(
	PostgammaMemoryGlobalTelemetry *telemetry);
PostgammaMemoryStatus postgamma_memory_clock_now(int64_t *now_ns);
const char *postgamma_memory_status_name(PostgammaMemoryStatus status);

#ifdef __cplusplus
}
#endif

#endif
