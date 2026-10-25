/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/backend_execution_runtime.h"
#include "../check.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>

static _Thread_local bool ForbidLocks;
static _Thread_local unsigned LockCalls;

int __real_postgamma_mutex_lock(PostgammaMutex *mutex);
int __wrap_postgamma_mutex_lock(PostgammaMutex *mutex);

int
__wrap_postgamma_mutex_lock(PostgammaMutex *mutex)
{
	CHECK(!ForbidLocks);
	LockCalls++;
	return __real_postgamma_mutex_lock(mutex);
}

typedef struct PollState
{
	PostgammaBackendRegistry *registry;
	const PostgammaBackendInterruptState *interrupts;
	_Atomic int phase;
	_Atomic int cleaned;
} PollState;

static void
wait_phase(PollState *state, int expected)
{
	uint64_t deadline = postgamma_monotonic_now_ns() + UINT64_C(5000000000);

	while (atomic_load_explicit(&state->phase, memory_order_acquire) != expected)
	{
		CHECK(postgamma_monotonic_now_ns() < deadline);
		sched_yield();
	}
}

static PostgammaBackendExitStatus
poll_backend(const PostgammaBackendStartInfo *info, const void *data,
			 size_t length, void *argument)
{
	PollState *state = argument;
	uint64_t pending;
	uint64_t wake_count;
	struct pollfd descriptor = {
		.fd = postgamma_wake_target_fd(info->wake_target), .events = POLLIN};
	PostgammaBackendExitStatus result = {POSTGAMMA_BACKEND_EXIT_NORMAL, 0};

	(void) data;
	(void) length;
	ForbidLocks = true;
	state->interrupts = postgamma_backend_borrow_interrupts(state->registry, info->handle.id);
	CHECK(state->interrupts != NULL);
	CHECK(postgamma_backend_borrow_interrupts(NULL, info->handle.id) == NULL);
	CHECK(postgamma_backend_borrow_interrupts(
		state->registry, info->handle.id + (UINT64_C(1) << 32)) == NULL);
	for (int index = 0; index < 100; index++)
	{
		CHECK(!postgamma_backend_interrupts_pending(state->interrupts));
		CHECK(!postgamma_backend_interrupt_poll_required(state->registry, info->handle.id));
		CHECK(postgamma_backend_take_pending_signals(
			state->registry, info->handle.id, &pending) == 0);
		CHECK(pending == 0);
		CHECK(postgamma_backend_wait_while_paused(state->registry, info->handle.id) == 0);
	}
	atomic_store_explicit(&state->phase, 1, memory_order_release);
	/* A signal must also wake a blocked execution without a native signal. */
	CHECK(poll(&descriptor, 1, 5000) == 1);
	CHECK((descriptor.revents & POLLIN) != 0);
	CHECK(postgamma_wake_target_drain(info->wake_target, &wake_count) == 0);
	CHECK(wake_count > 0);
	wait_phase(state, 2);
	CHECK(postgamma_backend_interrupts_pending(state->interrupts));
	CHECK(postgamma_backend_interrupt_poll_required(state->registry, info->handle.id));
	CHECK(postgamma_backend_take_pending_signals(
		state->registry, info->handle.id, &pending) == 0);
	CHECK(pending == (UINT64_C(1) << 9));
	CHECK(postgamma_backend_take_pending_signals(
		state->registry, info->handle.id, &pending) == 0);
	CHECK(pending == 0);
	CHECK(!postgamma_backend_interrupts_pending(state->interrupts));
	CHECK(!postgamma_backend_interrupt_poll_required(state->registry, info->handle.id));
	CHECK(postgamma_backend_interrupt_poll_required(
		state->registry, info->handle.id + (UINT64_C(1) << 32)));
	ForbidLocks = false;
	LockCalls = 0;
	CHECK(postgamma_backend_take_pending_signals(
		state->registry, info->handle.id + (UINT64_C(1) << 32), &pending) == ESTALE);
	CHECK(LockCalls > 0);
	atomic_store_explicit(&state->phase, 3, memory_order_release);
	{
		uint64_t deadline = postgamma_monotonic_now_ns() + UINT64_C(5000000000);

		while (!postgamma_backend_interrupts_pending(state->interrupts))
		{
			CHECK(postgamma_monotonic_now_ns() < deadline);
			sched_yield();
		}
	}
	CHECK(postgamma_backend_wait_while_paused(state->registry, info->handle.id) == 0);
	/* Resuming a pause must not acknowledge a signal posted while paused. */
	CHECK(postgamma_backend_interrupts_pending(state->interrupts));
	CHECK(postgamma_backend_take_pending_signals(
		state->registry, info->handle.id, &pending) == 0);
	CHECK(pending == (UINT64_C(1) << 63));
	CHECK(!postgamma_backend_interrupts_pending(state->interrupts));
	/* Requeueing blocked signals must republish the executor's fast check. */
	CHECK(postgamma_backend_requeue_pending_signals(
		state->registry, info->handle.id, pending) == 0);
	CHECK(postgamma_backend_interrupts_pending(state->interrupts));
	CHECK(postgamma_backend_take_pending_signals(
		state->registry, info->handle.id, &pending) == 0);
	CHECK(pending == (UINT64_C(1) << 63));
	CHECK(!postgamma_backend_interrupts_pending(state->interrupts));
	return result;
}

static int
cleanup_backend(const PostgammaBackendStartInfo *info, void *argument)
{
	PollState *state = argument;
	uint64_t pending;

	/* A borrow survives cleanup, but cannot be acquired outside main. */
	CHECK(postgamma_backend_borrow_interrupts(state->registry, info->handle.id) == NULL);
	ForbidLocks = true;
	CHECK(!postgamma_backend_interrupts_pending(state->interrupts));
	ForbidLocks = false;
	state->interrupts = NULL;
	/* The carrier must no longer retain this execution's fast binding. */
	LockCalls = 0;
	CHECK(postgamma_backend_interrupt_poll_required(state->registry, info->handle.id));
	CHECK(postgamma_backend_take_pending_signals(state->registry, info->handle.id, &pending) == 0);
	CHECK(LockCalls > 0 && pending == 0);
	atomic_fetch_add_explicit(&state->cleaned, 1, memory_order_release);
	return 0;
}

static void
test_provider(PostgammaBackendProviderKind kind)
{
	PostgammaBackendRegistry *registry;
	PostgammaBackendProviderConfig config = {.kind = kind};
	PostgammaBackendId previous_id = 0;

	if (kind == POSTGAMMA_BACKEND_PROVIDER_POOLED)
	{
		config.worker_count = 2;
		config.queue_capacity = 8;
		config.execution_token_count = 2;
	}
	CHECK(postgamma_backend_registry_create_with_provider(&registry, 8, &config) == 0);
	for (int iteration = 0; iteration < 32; iteration++)
	{
		PollState state = {.registry = registry};
		PostgammaBackendHandle handle;
		PostgammaBackendCompletion completion;
		PostgammaBackendLaunchRequest request = {
			.execution_class = POSTGAMMA_BACKEND_CLASS_CLIENT,
			.main_function = poll_backend, .main_argument = &state,
			.cleanup_function = cleanup_backend, .cleanup_argument = &state,
		};
		struct pollfd descriptor = {
			.fd = postgamma_backend_registry_wake_fd(registry), .events = POLLIN,
		};
		uint64_t pending;
		int status;

		atomic_init(&state.phase, 0);
		atomic_init(&state.cleaned, 0);
		CHECK(postgamma_backend_launch(registry, &request, &handle) == 0);
		CHECK(postgamma_backend_borrow_interrupts(registry, handle.id) == NULL);
		CHECK(handle.id != previous_id);
		if (previous_id != 0)
			CHECK(postgamma_backend_take_pending_signals(registry, previous_id, &pending) == ESTALE);
		wait_phase(&state, 1);
		CHECK(postgamma_backend_signal(registry, handle.compat_pid, 10) == 0);
		atomic_store_explicit(&state.phase, 2, memory_order_release);
		wait_phase(&state, 3);
		CHECK(postgamma_backend_pause(registry, handle.compat_pid) == 0);
		/* A signal drain must not acknowledge the independent pause request. */
		CHECK(postgamma_backend_take_pending_signals(registry, handle.id, &pending) == 0);
		CHECK(pending == 0 && postgamma_backend_interrupts_pending(state.interrupts));
		CHECK(postgamma_backend_signal(registry, handle.compat_pid, 64) == 0);
		CHECK(postgamma_backend_resume(registry, handle.compat_pid) == 0);
		while ((status = postgamma_backend_completion_pop(registry, &completion)) == EAGAIN)
			CHECK(poll(&descriptor, 1, 5000) == 1);
		CHECK(status == 0 && completion.handle.id == handle.id);
		CHECK(completion.exit_status.kind == POSTGAMMA_BACKEND_EXIT_NORMAL);
		CHECK(postgamma_backend_completion_join(registry, &completion) == 0);
		CHECK(atomic_load_explicit(&state.cleaned, memory_order_acquire) == 1);
		previous_id = handle.id;
	}
	CHECK(postgamma_backend_registry_destroy(registry) == 0);
}

#define RACE_ITERATIONS 5000

typedef struct RaceState
{
	PostgammaBackendRegistry *registry;
	PostgammaBackendHandle handle;
	_Atomic int acknowledged[2];
	uint64_t deadline;
} RaceState;

typedef struct SignalProducer
{
	RaceState *state;
	unsigned stream;
} SignalProducer;

static void *
publish_signals(void *argument)
{
	SignalProducer *producer = argument;
	RaceState *state = producer->state;
	unsigned stream = producer->stream;

	for (int iteration = 0; iteration < RACE_ITERATIONS; iteration++)
	{
		CHECK(postgamma_backend_signal(state->registry, state->handle.compat_pid,
			stream * 32 + iteration % 32 + 1) == 0);
		while (atomic_load_explicit(&state->acknowledged[stream],
								   memory_order_acquire) != iteration + 1)
		{
			CHECK(postgamma_monotonic_now_ns() < state->deadline);
			sched_yield();
		}
	}
	return NULL;
}

static PostgammaBackendExitStatus
race_backend(const PostgammaBackendStartInfo *info, const void *data,
			 size_t length, void *argument)
{
	RaceState *state = argument;
	const PostgammaBackendInterruptState *interrupts =
		postgamma_backend_borrow_interrupts(state->registry, info->handle.id);
	int received[2] = {0, 0};

	(void) data;
	(void) length;
	CHECK(interrupts != NULL);
	ForbidLocks = true;
	/* Disjoint signal sets avoid coalescing while publications race with drains. */
	while (received[0] + received[1] < 2 * RACE_ITERATIONS)
	{
		uint64_t pending = 0;

		while (pending == 0)
		{
			CHECK(postgamma_monotonic_now_ns() < state->deadline);
			if (postgamma_backend_interrupts_pending(interrupts))
				CHECK(postgamma_backend_take_pending_signals(
					state->registry, info->handle.id, &pending) == 0);
			if (pending == 0)
				sched_yield();
		}
		for (unsigned stream = 0; stream < 2; stream++)
		{
			uint64_t expected = UINT64_C(1) << (stream * 32 + received[stream] % 32);

			if ((pending & expected) != 0)
			{
				CHECK(received[stream] < RACE_ITERATIONS);
				pending &= ~expected;
				atomic_store_explicit(&state->acknowledged[stream],
					++received[stream], memory_order_release);
			}
		}
		CHECK(pending == 0);
	}
	ForbidLocks = false;
	return (PostgammaBackendExitStatus) {POSTGAMMA_BACKEND_EXIT_NORMAL, 0};
}

static void
test_publication_race(void)
{
	PostgammaBackendRegistry *registry;
	PostgammaBackendCompletion completion;
	RaceState state = {0};
	SignalProducer producers[2] = {{&state, 0}, {&state, 1}};
	pthread_t producer;
	PostgammaBackendLaunchRequest request = {
		.execution_class = POSTGAMMA_BACKEND_CLASS_CLIENT,
		.main_function = race_backend, .main_argument = &state
	};
	int status;

	CHECK(postgamma_backend_registry_create(&registry, 1) == 0);
	state.registry = registry;
	state.deadline = postgamma_monotonic_now_ns() + UINT64_C(60000000000);
	atomic_init(&state.acknowledged[0], 0);
	atomic_init(&state.acknowledged[1], 0);
	CHECK(postgamma_backend_launch(registry, &request, &state.handle) == 0);
	CHECK(pthread_create(&producer, NULL, publish_signals, &producers[1]) == 0);
	CHECK(publish_signals(&producers[0]) == NULL);
	CHECK(pthread_join(producer, NULL) == 0);
	while ((status = postgamma_backend_completion_pop(registry, &completion)) == EAGAIN)
	{
		CHECK(postgamma_monotonic_now_ns() < state.deadline);
		sched_yield();
	}
	CHECK(status == 0 && completion.exit_status.kind == POSTGAMMA_BACKEND_EXIT_NORMAL);
	CHECK(postgamma_backend_completion_join(registry, &completion) == 0);
	CHECK(postgamma_backend_registry_destroy(registry) == 0);
}

int
main(void)
{
	test_provider(POSTGAMMA_BACKEND_PROVIDER_DEDICATED);
	test_provider(POSTGAMMA_BACKEND_PROVIDER_POOLED);
	test_publication_race();
	return 0;
}
