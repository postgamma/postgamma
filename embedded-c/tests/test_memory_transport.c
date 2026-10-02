#define _GNU_SOURCE

#include "postgamma/private/memory_transport.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define TEST_GENERATION UINT64_C(41)
#define NANOSECONDS_PER_SECOND INT64_C(1000000000)
#define PROCESS_BASELINE_TIMEOUT_NS (2 * NANOSECONDS_PER_SECOND)
#define PROCESS_BASELINE_SAMPLE_NS INT64_C(1000000)
#define PROCESS_BASELINE_STABLE_SAMPLES 3

typedef struct WaitFixture
{
	PostgammaMemoryEndpoint *endpoint;
	pthread_barrier_t *barrier;
	uint64_t	generation;
	uint64_t	cancel_epoch;
	uint32_t	events;
	int64_t		deadline;
	PostgammaMemoryWaitResult result;
} WaitFixture;

typedef struct CloseFixture
{
	PostgammaMemoryEndpoint *endpoint;
	pthread_barrier_t *barrier;
	PostgammaMemoryStatus status;
} CloseFixture;

typedef struct PumpFixture
{
	PostgammaMemoryEndpoint *endpoint;
	pthread_barrier_t *barrier;
	const unsigned char *send;
	size_t		send_length;
	size_t		send_offset;
	unsigned char *received;
	size_t		received_capacity;
	size_t		received_length;
	int64_t		deadline;
	PostgammaMemoryStatus status;
} PumpFixture;

typedef struct TestMetrics
{
	size_t		copy_bytes;
	size_t		control_bytes;
	size_t		duplex_capacity;
	uint64_t	duplex_wait_calls;
	uint64_t	duplex_wait_wakeups;
	uint64_t	duplex_timeouts;
	bool		both_queues_saturated;
} TestMetrics;


typedef struct NotifyFixture
{
	unsigned int calls;
	uint32_t	last_events;
} NotifyFixture;


static void
record_notification(void *argument, uint32_t events)
{
	NotifyFixture *fixture = argument;

	fixture->calls++;
	fixture->last_events = events;
}


static int64_t
deadline_after(int64_t nanoseconds)
{
	int64_t		now;

	assert(postgamma_memory_clock_now(&now) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(nanoseconds > 0 && now <= INT64_MAX - nanoseconds);
	return now + nanoseconds;
}


static void
join_before(pthread_t thread, int seconds)
{
	struct timespec deadline;

	assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
	deadline.tv_sec += seconds;
	assert(pthread_timedjoin_np(thread, NULL, &deadline) == 0);
}


static int
count_directory(const char *path)
{
	DIR    *directory = opendir(path);
	struct dirent *entry;
	int		count = 0;

	assert(directory != NULL);
	while ((entry = readdir(directory)) != NULL)
	{
		if (strcmp(entry->d_name, ".") != 0 &&
			strcmp(entry->d_name, "..") != 0)
			count++;
	}
	assert(closedir(directory) == 0);
	return count;
}


static void
wait_for_process_baseline(int baseline_descriptors, int baseline_threads,
						  int *final_descriptors, int *final_threads)
{
	struct timespec delay = {0, PROCESS_BASELINE_SAMPLE_NS};
	int64_t		deadline = deadline_after(PROCESS_BASELINE_TIMEOUT_NS);
	int			stable_samples = 0;

	assert(final_descriptors != NULL && final_threads != NULL);
	for (;;)
	{
		int64_t		now;

		*final_descriptors = count_directory("/proc/self/fd");
		*final_threads = count_directory("/proc/self/task");
		if (*final_descriptors == baseline_descriptors &&
			*final_threads == baseline_threads)
		{
			stable_samples++;
			if (stable_samples == PROCESS_BASELINE_STABLE_SAMPLES)
				return;
		}
		else
			stable_samples = 0;
		assert(postgamma_memory_clock_now(&now) ==
			   POSTGAMMA_MEMORY_STATUS_OK);
		assert(now < deadline);
		assert(nanosleep(&delay, NULL) == 0);
	}
}


static void
release_pair(PostgammaMemoryEndpoint **frontend,
			 PostgammaMemoryEndpoint **backend, uint64_t generation)
{
	assert(postgamma_memory_endpoint_release(frontend, generation) ==
		   POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_release(backend, generation) ==
		   POSTGAMMA_MEMORY_STATUS_OK);
	assert(*frontend == NULL && *backend == NULL);
}


static void
assert_global_telemetry(const PostgammaMemoryGlobalTelemetry *expected)
{
	PostgammaMemoryGlobalTelemetry actual;

	postgamma_memory_global_telemetry(&actual);
	assert(actual.active_transports == expected->active_transports);
	assert(actual.endpoint_references == expected->endpoint_references);
	assert(actual.allocated_bytes == expected->allocated_bytes);
}


static void
test_partial_progress_wraparound(void)
{
	PostgammaMemoryEndpoint *frontend = NULL;
	PostgammaMemoryEndpoint *backend = NULL;
	PostgammaMemoryEndpoint *retained = NULL;
	PostgammaMemoryTelemetry telemetry;
	PostgammaMemoryIoResult io;
	unsigned char output[16] = {0};

	assert(postgamma_memory_transport_create(
		TEST_GENERATION, 7, &frontend, &backend) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	io = postgamma_memory_endpoint_write(
		frontend, TEST_GENERATION, "abcdef", 6);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS && io.bytes == 6);
	io = postgamma_memory_endpoint_write(frontend, TEST_GENERATION, "GH", 2);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS && io.bytes == 1);
	io = postgamma_memory_endpoint_write(frontend, TEST_GENERATION, "H", 1);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_RETRY && io.bytes == 0);
	io = postgamma_memory_endpoint_read(backend, TEST_GENERATION, output, 4);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS && io.bytes == 4);
	assert(memcmp(output, "abcd", 4) == 0);
	io = postgamma_memory_endpoint_write(
		frontend, TEST_GENERATION, "HIJK", 4);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS && io.bytes == 4);
	io = postgamma_memory_endpoint_read(backend, TEST_GENERATION, output, 8);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS && io.bytes == 7);
	assert(memcmp(output, "efGHIJK", 7) == 0);
	io = postgamma_memory_endpoint_read(backend, TEST_GENERATION, output, 1);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_RETRY);
	io = postgamma_memory_endpoint_write(
		frontend, TEST_GENERATION + 1, "x", 1);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_STALE_GENERATION);
	assert(postgamma_memory_endpoint_retain(
		frontend, TEST_GENERATION, &retained) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(retained == frontend);
	assert(postgamma_memory_endpoint_release(&retained, TEST_GENERATION) ==
		   POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_snapshot(
		frontend, TEST_GENERATION, &telemetry) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(telemetry.frontend_references == 1 &&
		   telemetry.backend_references == 1);
	assert(telemetry.frontend_to_backend.total_bytes_written == 11);
	assert(telemetry.frontend_to_backend.total_bytes_read == 11);
	assert(postgamma_memory_endpoint_half_close_write(
		frontend, TEST_GENERATION) == POSTGAMMA_MEMORY_STATUS_OK);
	io = postgamma_memory_endpoint_read(backend, TEST_GENERATION, output, 1);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_EOF);
	assert(postgamma_memory_endpoint_half_close_write(
		backend, TEST_GENERATION) == POSTGAMMA_MEMORY_STATUS_OK);
	io = postgamma_memory_endpoint_read(frontend, TEST_GENERATION, output, 1);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_EOF);
	release_pair(&frontend, &backend, TEST_GENERATION);
}


static void
test_endpoint_notifications(void)
{
	PostgammaMemoryEndpoint *frontend = NULL;
	PostgammaMemoryEndpoint *backend = NULL;
	NotifyFixture frontend_notify = {0};
	NotifyFixture backend_notify = {0};
	PostgammaMemoryIoResult io;
	unsigned char byte;
	uint64_t	cancel_epoch;

	assert(postgamma_memory_transport_create(
		TEST_GENERATION, 2, &frontend, &backend) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_set_notify(
		frontend, TEST_GENERATION, record_notification,
		&frontend_notify) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_set_notify(
		backend, TEST_GENERATION, record_notification,
		&backend_notify) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(frontend_notify.calls == 1 && backend_notify.calls == 1);
	assert(frontend_notify.last_events == POSTGAMMA_MEMORY_WAIT_WRITABLE);
	assert(backend_notify.last_events == POSTGAMMA_MEMORY_WAIT_WRITABLE);

	io = postgamma_memory_endpoint_write(
		frontend, TEST_GENERATION, "x", 1);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS && io.bytes == 1);
	assert(backend_notify.calls == 2);
	assert((backend_notify.last_events & POSTGAMMA_MEMORY_WAIT_READABLE) != 0);
	io = postgamma_memory_endpoint_read(
		backend, TEST_GENERATION, &byte, sizeof(byte));
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS && byte == 'x');
	assert(frontend_notify.calls == 2);
	assert(postgamma_memory_endpoint_cancel_waits(
		frontend, TEST_GENERATION, &cancel_epoch) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	assert(cancel_epoch == 1);
	assert(frontend_notify.calls == 3 && backend_notify.calls == 3);
	assert(postgamma_memory_endpoint_half_close_write(
		frontend, TEST_GENERATION) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(frontend_notify.calls == 4 && backend_notify.calls == 4);
	assert(postgamma_memory_endpoint_set_notify(
		frontend, TEST_GENERATION, NULL, NULL) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_set_notify(
		backend, TEST_GENERATION, NULL, NULL) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	release_pair(&frontend, &backend, TEST_GENERATION);
}


static void
test_result_policy_handoff(void)
{
	PostgammaMemoryEndpoint *frontend = NULL;
	PostgammaMemoryEndpoint *backend = NULL;
	PostgammaMemoryResultPolicy policy = POSTGAMMA_MEMORY_RESULT_POLICY_INIT;
	PostgammaMemoryResultPolicy observed = POSTGAMMA_MEMORY_RESULT_POLICY_INIT;

	assert(postgamma_memory_transport_create(
		TEST_GENERATION, 8, &frontend, &backend) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_get_result_policy(
		backend, TEST_GENERATION, &observed) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(observed.request_generation == 0);
	policy.request_generation = UINT64_C(17);
	policy.delivery_mode = POSTGAMMA_MEMORY_DELIVERY_CHUNKED;
	policy.target_chunk_rows = UINT32_C(3);
	policy.result_buffer_limit = 4096;
	policy.maximum_value_size = 1024;
	assert(postgamma_memory_endpoint_set_result_policy(
		backend, TEST_GENERATION, &policy) ==
		POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION);
	assert(postgamma_memory_endpoint_set_result_policy(
		frontend, TEST_GENERATION, &policy) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_get_result_policy(
		backend, TEST_GENERATION, &observed) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(memcmp(&observed, &policy, sizeof(policy)) == 0);
	policy = (PostgammaMemoryResultPolicy)
		POSTGAMMA_MEMORY_RESULT_POLICY_INIT;
	assert(postgamma_memory_endpoint_set_result_policy(
		frontend, TEST_GENERATION, &policy) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_get_result_policy(
		backend, TEST_GENERATION, &observed) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(memcmp(&observed, &policy, sizeof(policy)) == 0);
	policy.request_generation = UINT64_C(17);
	policy.delivery_mode = POSTGAMMA_MEMORY_DELIVERY_CHUNKED;
	policy.target_chunk_rows = UINT32_C(3);
	policy.result_buffer_limit = 4096;
	policy.maximum_value_size = 4097;
	assert(postgamma_memory_endpoint_set_result_policy(
		frontend, TEST_GENERATION, &policy) ==
		POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT);
	assert(postgamma_memory_endpoint_get_result_policy(
		backend, TEST_GENERATION + 1, &observed) ==
		POSTGAMMA_MEMORY_STATUS_STALE_GENERATION);
	release_pair(&frontend, &backend, TEST_GENERATION);
}


static void *
wait_worker(void *argument)
{
	WaitFixture *fixture = argument;
	int			status;

	status = pthread_barrier_wait(fixture->barrier);
	assert(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
	fixture->result = postgamma_memory_endpoint_wait(
		fixture->endpoint,
		fixture->generation,
		fixture->events,
		fixture->cancel_epoch,
		fixture->deadline);
	return NULL;
}


static void
barrier_wait(pthread_barrier_t *barrier)
{
	int			status = pthread_barrier_wait(barrier);

	assert(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}


static void
test_wait_wakeup_timeout_cancel_and_abort(void)
{
	PostgammaMemoryEndpoint *frontend = NULL;
	PostgammaMemoryEndpoint *backend = NULL;
	pthread_barrier_t barrier;
	pthread_t	thread;
	WaitFixture fixture;
	PostgammaMemoryIoResult io;
	PostgammaMemoryWaitResult wait_result;
	PostgammaMemoryTelemetry telemetry;
	uint64_t	cancel_epoch;
	unsigned char byte;

	assert(postgamma_memory_transport_create(
		TEST_GENERATION, 1, &frontend, &backend) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
	fixture = (WaitFixture) {
		.endpoint = frontend,
		.barrier = &barrier,
		.generation = TEST_GENERATION,
		.events = POSTGAMMA_MEMORY_WAIT_READABLE,
		.deadline = POSTGAMMA_MEMORY_NO_DEADLINE,
	};
	assert(pthread_create(&thread, NULL, wait_worker, &fixture) == 0);
	barrier_wait(&barrier);
	io = postgamma_memory_endpoint_write(
		backend, TEST_GENERATION, "r", 1);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS && io.bytes == 1);
	join_before(thread, 2);
	assert(fixture.result.status == POSTGAMMA_MEMORY_STATUS_OK);
	assert((fixture.result.events & POSTGAMMA_MEMORY_WAIT_READABLE) != 0);
	io = postgamma_memory_endpoint_read(
		frontend, TEST_GENERATION, &byte, sizeof(byte));
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS && byte == 'r');

	io = postgamma_memory_endpoint_write(
		frontend, TEST_GENERATION, "w", 1);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS);
	fixture = (WaitFixture) {
		.endpoint = frontend,
		.barrier = &barrier,
		.generation = TEST_GENERATION,
		.events = POSTGAMMA_MEMORY_WAIT_WRITABLE,
		.deadline = POSTGAMMA_MEMORY_NO_DEADLINE,
	};
	assert(pthread_create(&thread, NULL, wait_worker, &fixture) == 0);
	barrier_wait(&barrier);
	io = postgamma_memory_endpoint_read(
		backend, TEST_GENERATION, &byte, sizeof(byte));
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS && byte == 'w');
	join_before(thread, 2);
	assert(fixture.result.status == POSTGAMMA_MEMORY_STATUS_OK);
	assert((fixture.result.events & POSTGAMMA_MEMORY_WAIT_WRITABLE) != 0);

	wait_result = postgamma_memory_endpoint_wait(
		frontend,
		TEST_GENERATION,
		POSTGAMMA_MEMORY_WAIT_READABLE,
		0,
		deadline_after(INT64_C(20000000)));
	assert(wait_result.status == POSTGAMMA_MEMORY_STATUS_TIMEOUT);
	fixture = (WaitFixture) {
		.endpoint = frontend,
		.barrier = &barrier,
		.generation = TEST_GENERATION,
		.events = POSTGAMMA_MEMORY_WAIT_READABLE,
		.deadline = POSTGAMMA_MEMORY_NO_DEADLINE,
	};
	assert(pthread_create(&thread, NULL, wait_worker, &fixture) == 0);
	barrier_wait(&barrier);
	assert(postgamma_memory_endpoint_cancel_waits(
		backend, TEST_GENERATION, &cancel_epoch) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	assert(cancel_epoch == 1);
	join_before(thread, 2);
	assert(fixture.result.status == POSTGAMMA_MEMORY_STATUS_CANCELLED);
	assert(fixture.result.cancel_epoch == cancel_epoch);

	fixture = (WaitFixture) {
		.endpoint = frontend,
		.barrier = &barrier,
		.generation = TEST_GENERATION,
		.cancel_epoch = cancel_epoch,
		.events = POSTGAMMA_MEMORY_WAIT_READABLE,
		.deadline = POSTGAMMA_MEMORY_NO_DEADLINE,
	};
	assert(pthread_create(&thread, NULL, wait_worker, &fixture) == 0);
	barrier_wait(&barrier);
	assert(postgamma_memory_endpoint_abort(
		backend, TEST_GENERATION, POSTGAMMA_MEMORY_ABORT_PROTOCOL) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	join_before(thread, 2);
	assert(fixture.result.status == POSTGAMMA_MEMORY_STATUS_ABORTED);
	assert(fixture.result.abort_reason == POSTGAMMA_MEMORY_ABORT_PROTOCOL);
	assert(postgamma_memory_endpoint_abort(
		frontend, TEST_GENERATION, POSTGAMMA_MEMORY_ABORT_SHUTDOWN) ==
		POSTGAMMA_MEMORY_STATUS_ABORTED);
	io = postgamma_memory_endpoint_read(
		frontend, TEST_GENERATION, &byte, sizeof(byte));
	assert(io.status == POSTGAMMA_MEMORY_STATUS_ABORTED);
	assert(io.abort_reason == POSTGAMMA_MEMORY_ABORT_PROTOCOL);
	assert(postgamma_memory_endpoint_snapshot(
		frontend, TEST_GENERATION, &telemetry) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(telemetry.wait_timeouts == 1);
	assert(telemetry.wait_cancellations == 1);
	assert(telemetry.aborted &&
		   telemetry.abort_reason == POSTGAMMA_MEMORY_ABORT_PROTOCOL);
	assert(pthread_barrier_destroy(&barrier) == 0);
	release_pair(&frontend, &backend, TEST_GENERATION);
}


static void *
close_worker(void *argument)
{
	CloseFixture *fixture = argument;

	barrier_wait(fixture->barrier);
	fixture->status = postgamma_memory_endpoint_half_close_write(
		fixture->endpoint, TEST_GENERATION);
	return NULL;
}


static void
test_concurrent_half_close(void)
{
	PostgammaMemoryEndpoint *frontend = NULL;
	PostgammaMemoryEndpoint *backend = NULL;
	pthread_barrier_t barrier;
	pthread_t	threads[2];
	CloseFixture fixtures[2];
	unsigned char byte;

	assert(postgamma_memory_transport_create(
		TEST_GENERATION, 7, &frontend, &backend) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	assert(pthread_barrier_init(&barrier, NULL, 3) == 0);
	fixtures[0] = (CloseFixture) {frontend, &barrier,
		POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR};
	fixtures[1] = (CloseFixture) {backend, &barrier,
		POSTGAMMA_MEMORY_STATUS_INTERNAL_ERROR};
	assert(pthread_create(&threads[0], NULL, close_worker, &fixtures[0]) == 0);
	assert(pthread_create(&threads[1], NULL, close_worker, &fixtures[1]) == 0);
	barrier_wait(&barrier);
	join_before(threads[0], 2);
	join_before(threads[1], 2);
	assert(fixtures[0].status == POSTGAMMA_MEMORY_STATUS_OK);
	assert(fixtures[1].status == POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_read(
		frontend, TEST_GENERATION, &byte, 1).status ==
		POSTGAMMA_MEMORY_STATUS_EOF);
	assert(postgamma_memory_endpoint_read(
		backend, TEST_GENERATION, &byte, 1).status ==
		POSTGAMMA_MEMORY_STATUS_EOF);
	assert(pthread_barrier_destroy(&barrier) == 0);
	release_pair(&frontend, &backend, TEST_GENERATION);
}


static void
test_final_endpoint_release_notifies_peer(void)
{
	PostgammaMemoryEndpoint *frontend = NULL;
	PostgammaMemoryEndpoint *backend = NULL;
	PostgammaMemoryIoResult io;
	PostgammaMemoryWaitResult wait_result;
	unsigned char byte;

	assert(postgamma_memory_transport_create(
		TEST_GENERATION, 7, &frontend, &backend) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	assert(postgamma_memory_endpoint_release(&backend, TEST_GENERATION) ==
		   POSTGAMMA_MEMORY_STATUS_OK);
	io = postgamma_memory_endpoint_write(
		frontend, TEST_GENERATION, "x", 1);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PEER_CLOSED);
	io = postgamma_memory_endpoint_read(
		frontend, TEST_GENERATION, &byte, sizeof(byte));
	assert(io.status == POSTGAMMA_MEMORY_STATUS_EOF);
	wait_result = postgamma_memory_endpoint_wait(
		frontend,
		TEST_GENERATION,
		POSTGAMMA_MEMORY_WAIT_PEER_CLOSED,
		0,
		POSTGAMMA_MEMORY_NO_DEADLINE);
	assert(wait_result.status == POSTGAMMA_MEMORY_STATUS_OK);
	assert(wait_result.events == POSTGAMMA_MEMORY_WAIT_PEER_CLOSED);
	assert(postgamma_memory_endpoint_release(&frontend, TEST_GENERATION) ==
		   POSTGAMMA_MEMORY_STATUS_OK);
}


static void *
pump_worker(void *argument)
{
	PumpFixture *fixture = argument;
	bool		write_closed = false;
	bool		read_eof = false;
	unsigned char extra;

	fixture->status = POSTGAMMA_MEMORY_STATUS_OK;
	barrier_wait(fixture->barrier);
	while (!read_eof || !write_closed)
	{
		bool		progress = false;

		for (;;)
		{
			PostgammaMemoryIoResult io;
			void       *target;
			size_t		available;

			if (fixture->received_length < fixture->received_capacity)
			{
				target = fixture->received + fixture->received_length;
				available = fixture->received_capacity -
					fixture->received_length;
			}
			else
			{
				target = &extra;
				available = 1;
			}
			io = postgamma_memory_endpoint_read(
				fixture->endpoint, TEST_GENERATION, target, available);
			if (io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
			{
				if (fixture->received_length == fixture->received_capacity)
				{
					fixture->status =
						POSTGAMMA_MEMORY_STATUS_CONTRACT_VIOLATION;
					return NULL;
				}
				fixture->received_length += io.bytes;
				progress = true;
				continue;
			}
			if (io.status == POSTGAMMA_MEMORY_STATUS_EOF)
			{
				read_eof = true;
				break;
			}
			if (io.status == POSTGAMMA_MEMORY_STATUS_RETRY)
				break;
			fixture->status = io.status;
			return NULL;
		}

		if (fixture->send_offset < fixture->send_length)
		{
			PostgammaMemoryIoResult io = postgamma_memory_endpoint_write(
				fixture->endpoint,
				TEST_GENERATION,
				fixture->send + fixture->send_offset,
				fixture->send_length - fixture->send_offset);

			if (io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
			{
				fixture->send_offset += io.bytes;
				progress = true;
			}
			else if (io.status != POSTGAMMA_MEMORY_STATUS_RETRY)
			{
				fixture->status = io.status;
				return NULL;
			}
		}
		if (fixture->send_offset == fixture->send_length && !write_closed)
		{
			fixture->status = postgamma_memory_endpoint_half_close_write(
				fixture->endpoint, TEST_GENERATION);
			if (fixture->status != POSTGAMMA_MEMORY_STATUS_OK)
				return NULL;
			write_closed = true;
			progress = true;
		}
		if (read_eof && write_closed)
			break;
		if (!progress)
		{
			uint32_t events = 0;
			PostgammaMemoryWaitResult wait_result;

			if (!read_eof)
				events |= POSTGAMMA_MEMORY_WAIT_READABLE |
					POSTGAMMA_MEMORY_WAIT_PEER_CLOSED;
			if (!write_closed)
				events |= POSTGAMMA_MEMORY_WAIT_WRITABLE;
			wait_result = postgamma_memory_endpoint_wait(
				fixture->endpoint,
				TEST_GENERATION,
				events,
				0,
				fixture->deadline);
			if (wait_result.status == POSTGAMMA_MEMORY_STATUS_OK)
				continue;
			if (wait_result.status == POSTGAMMA_MEMORY_STATUS_EOF)
			{
				read_eof = true;
				continue;
			}
			fixture->status = wait_result.status;
			return NULL;
		}
	}
	return NULL;
}


static void
test_duplex_saturation_progress(TestMetrics *metrics)
{
	const size_t copy_length = 4096;
	const size_t control_length = 257;
	const size_t capacity = 7;
	PostgammaMemoryEndpoint *frontend = NULL;
	PostgammaMemoryEndpoint *backend = NULL;
	PostgammaMemoryTelemetry telemetry;
	PostgammaMemoryIoResult io;
	pthread_barrier_t barrier;
	pthread_t	threads[2];
	PumpFixture frontend_pump;
	PumpFixture backend_pump;
	unsigned char *copy = malloc(copy_length);
	unsigned char *control = malloc(control_length);
	unsigned char *copy_received = calloc(copy_length, 1);
	unsigned char *control_received = calloc(control_length, 1);
	size_t		index;
	int64_t		deadline;

	assert(copy != NULL && control != NULL && copy_received != NULL &&
		   control_received != NULL);
	for (index = 0; index < copy_length; index++)
		copy[index] = (unsigned char) (index * 37U + 11U);
	for (index = 0; index < control_length; index++)
		control[index] = (unsigned char) ('a' + index % 23U);
	control[0] = 'N';
	control[128] = 'E';
	control[256] = 'Z';
	assert(postgamma_memory_transport_create(
		TEST_GENERATION, capacity, &frontend, &backend) ==
		POSTGAMMA_MEMORY_STATUS_OK);
	io = postgamma_memory_endpoint_write(
		frontend, TEST_GENERATION, copy, capacity);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS &&
		   io.bytes == capacity);
	io = postgamma_memory_endpoint_write(
		backend, TEST_GENERATION, control, capacity);
	assert(io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS &&
		   io.bytes == capacity);
	assert(postgamma_memory_endpoint_snapshot(
		frontend, TEST_GENERATION, &telemetry) == POSTGAMMA_MEMORY_STATUS_OK);
	metrics->both_queues_saturated =
		telemetry.frontend_to_backend.pending_bytes == capacity &&
		telemetry.backend_to_frontend.pending_bytes == capacity &&
		telemetry.simultaneous_full_observations != 0;
	assert(metrics->both_queues_saturated);
	assert(pthread_barrier_init(&barrier, NULL, 3) == 0);
	deadline = deadline_after(2 * NANOSECONDS_PER_SECOND);
	frontend_pump = (PumpFixture) {
		.endpoint = frontend,
		.barrier = &barrier,
		.send = copy,
		.send_length = copy_length,
		.send_offset = capacity,
		.received = control_received,
		.received_capacity = control_length,
		.deadline = deadline,
	};
	backend_pump = (PumpFixture) {
		.endpoint = backend,
		.barrier = &barrier,
		.send = control,
		.send_length = control_length,
		.send_offset = capacity,
		.received = copy_received,
		.received_capacity = copy_length,
		.deadline = deadline,
	};
	assert(pthread_create(
		&threads[0], NULL, pump_worker, &frontend_pump) == 0);
	assert(pthread_create(
		&threads[1], NULL, pump_worker, &backend_pump) == 0);
	barrier_wait(&barrier);
	join_before(threads[0], 3);
	join_before(threads[1], 3);
	assert(frontend_pump.status == POSTGAMMA_MEMORY_STATUS_OK);
	assert(backend_pump.status == POSTGAMMA_MEMORY_STATUS_OK);
	assert(frontend_pump.send_offset == copy_length);
	assert(backend_pump.send_offset == control_length);
	assert(frontend_pump.received_length == control_length);
	assert(backend_pump.received_length == copy_length);
	assert(memcmp(copy, copy_received, copy_length) == 0);
	assert(memcmp(control, control_received, control_length) == 0);
	assert(control_received[0] == 'N' && control_received[128] == 'E' &&
		   control_received[256] == 'Z');
	assert(postgamma_memory_endpoint_snapshot(
		frontend, TEST_GENERATION, &telemetry) == POSTGAMMA_MEMORY_STATUS_OK);
	assert(telemetry.frontend_to_backend.pending_bytes == 0);
	assert(telemetry.backend_to_frontend.pending_bytes == 0);
	assert(telemetry.frontend_to_backend.total_bytes_written == copy_length);
	assert(telemetry.frontend_to_backend.total_bytes_read == copy_length);
	assert(telemetry.backend_to_frontend.total_bytes_written == control_length);
	assert(telemetry.backend_to_frontend.total_bytes_read == control_length);
	assert(telemetry.wait_timeouts == 0);
	metrics->copy_bytes = copy_length;
	metrics->control_bytes = control_length;
	metrics->duplex_capacity = capacity;
	metrics->duplex_wait_calls = telemetry.wait_calls;
	metrics->duplex_wait_wakeups = telemetry.wait_wakeups;
	metrics->duplex_timeouts = telemetry.wait_timeouts;
	assert(pthread_barrier_destroy(&barrier) == 0);
	release_pair(&frontend, &backend, TEST_GENERATION);
	free(control_received);
	free(copy_received);
	free(control);
	free(copy);
}


static void
test_repeated_lifecycle(void)
{
	const size_t capacities[] = {1, 7, 64};
	size_t		iteration;

	for (iteration = 0; iteration < 600; iteration++)
	{
		PostgammaMemoryEndpoint *frontend = NULL;
		PostgammaMemoryEndpoint *backend = NULL;
		size_t capacity = capacities[iteration % 3];
		uint64_t generation = TEST_GENERATION + 1 + iteration;

		assert(postgamma_memory_transport_create(
			generation, capacity, &frontend, &backend) ==
			POSTGAMMA_MEMORY_STATUS_OK);
		release_pair(&frontend, &backend, generation);
	}
}


static void
write_report(const char *path,
			 const PostgammaMemoryGlobalTelemetry *baseline,
			 const PostgammaMemoryGlobalTelemetry *final,
			 int baseline_descriptors, int final_descriptors,
			 int baseline_threads, int final_threads,
			 const TestMetrics *metrics)
{
	FILE   *report = fopen(path, "w");

	assert(report != NULL);
	assert(fprintf(
		report,
		"{\n"
		"  \"schema_version\": 1,\n"
		"  \"kind\": \"postgamma.memory-transport-test\",\n"
		"  \"status\": \"pass\",\n"
		"  \"scenarios\": 9,\n"
		"  \"capacities\": [1, 7, 64],\n"
		"  \"repeated_lifecycles\": 600,\n"
		"  \"duplex_progress\": {\n"
		"    \"capacity\": %zu,\n"
		"    \"copy_bytes\": %zu,\n"
		"    \"control_bytes\": %zu,\n"
		"    \"both_queues_saturated\": %s,\n"
		"    \"wait_calls\": %llu,\n"
		"    \"wait_wakeups\": %llu,\n"
		"    \"timeouts\": %llu\n"
		"  },\n"
		"  \"global_baseline\": {\"active_transports\": %llu, "
		"\"endpoint_references\": %llu, \"allocated_bytes\": %llu},\n"
		"  \"global_final\": {\"active_transports\": %llu, "
		"\"endpoint_references\": %llu, \"allocated_bytes\": %llu},\n"
		"  \"process_baseline\": {\"descriptors\": %d, \"threads\": %d},\n"
		"  \"process_final\": {\"descriptors\": %d, \"threads\": %d}\n"
		"}\n",
		metrics->duplex_capacity,
		metrics->copy_bytes,
		metrics->control_bytes,
		metrics->both_queues_saturated ? "true" : "false",
		(unsigned long long) metrics->duplex_wait_calls,
		(unsigned long long) metrics->duplex_wait_wakeups,
		(unsigned long long) metrics->duplex_timeouts,
		(unsigned long long) baseline->active_transports,
		(unsigned long long) baseline->endpoint_references,
		(unsigned long long) baseline->allocated_bytes,
		(unsigned long long) final->active_transports,
		(unsigned long long) final->endpoint_references,
		(unsigned long long) final->allocated_bytes,
		baseline_descriptors,
		baseline_threads,
		final_descriptors,
		final_threads) > 0);
	assert(fclose(report) == 0);
}


static void *
warm_thread_runtime(void *argument)
{
	(void) argument;
	return NULL;
}


int
main(int argc, char **argv)
{
	PostgammaMemoryEndpoint *frontend = NULL;
	PostgammaMemoryEndpoint *backend = NULL;
	PostgammaMemoryGlobalTelemetry baseline;
	PostgammaMemoryGlobalTelemetry final;
	TestMetrics metrics = {0};
	int			baseline_descriptors;
	int			baseline_threads;
	int			final_descriptors;
	int			final_threads;
	pthread_t	warm_thread;

	assert(argc == 1 || (argc == 3 && strcmp(argv[1], "--report") == 0));
	assert(pthread_create(&warm_thread, NULL, warm_thread_runtime, NULL) == 0);
	join_before(warm_thread, 2);
	postgamma_memory_global_telemetry(&baseline);
	baseline_descriptors = count_directory("/proc/self/fd");
	baseline_threads = count_directory("/proc/self/task");
	assert(postgamma_memory_transport_create(
		0, 1, &frontend, &backend) ==
		POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT);
	assert(postgamma_memory_transport_create(
		TEST_GENERATION, 0, &frontend, &backend) ==
		POSTGAMMA_MEMORY_STATUS_INVALID_ARGUMENT);
	test_partial_progress_wraparound();
	test_endpoint_notifications();
	test_result_policy_handoff();
	test_wait_wakeup_timeout_cancel_and_abort();
	test_concurrent_half_close();
	test_final_endpoint_release_notifies_peer();
	test_duplex_saturation_progress(&metrics);
	test_repeated_lifecycle();
	postgamma_memory_global_telemetry(&final);
	wait_for_process_baseline(
		baseline_descriptors, baseline_threads,
		&final_descriptors, &final_threads);
	assert_global_telemetry(&baseline);
	assert(final.active_transports == baseline.active_transports);
	assert(final.endpoint_references == baseline.endpoint_references);
	assert(final.allocated_bytes == baseline.allocated_bytes);
	assert(final_descriptors == baseline_descriptors);
	assert(final_threads == baseline_threads);
	if (argc == 3)
		write_report(
			argv[2], &baseline, &final,
			baseline_descriptors, final_descriptors,
			baseline_threads, final_threads, &metrics);
	return 0;
}
