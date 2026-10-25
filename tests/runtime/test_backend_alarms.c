/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "postgamma/backend_execution_runtime.h"
#include "../check.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define ALARM_BIT (UINT64_C(1) << (SIGALRM - 1))
#define MS UINT64_C(1000000)

static _Thread_local bool FailOwner;
static _Thread_local bool ForbidLocks;
static _Thread_local bool FailNotify;
static volatile sig_atomic_t NativeAlarms;
static _Atomic unsigned OwnersStarted;

int __real_postgamma_thread_create(PostgammaThread **thread,
	const PostgammaThreadAttributes *attributes, PostgammaThreadMain main_function,
	void *argument);
int __wrap_postgamma_thread_create(PostgammaThread **thread,
	const PostgammaThreadAttributes *attributes, PostgammaThreadMain main_function,
	void *argument);
int __real_postgamma_mutex_lock(PostgammaMutex *mutex);
int __wrap_postgamma_mutex_lock(PostgammaMutex *mutex);
int __real_postgamma_condition_signal(PostgammaCondition *condition);
int __wrap_postgamma_condition_signal(PostgammaCondition *condition);

int
__wrap_postgamma_condition_signal(PostgammaCondition *condition)
{
	int status = __real_postgamma_condition_signal(condition);
	/* A spurious wake is harmless even if the caller receives an error. */
	return FailNotify ? EIO : status;
}

int
__wrap_postgamma_thread_create(PostgammaThread **thread,
	const PostgammaThreadAttributes *attributes, PostgammaThreadMain main_function,
	void *argument)
{
	if (FailOwner && attributes->name != NULL && strcmp(attributes->name, "pg-alarm") == 0)
		return EAGAIN;
	if (attributes->name != NULL && strcmp(attributes->name, "pg-alarm") == 0)
		atomic_fetch_add(&OwnersStarted, 1);
	return __real_postgamma_thread_create(thread, attributes, main_function, argument);
}

int
__wrap_postgamma_mutex_lock(PostgammaMutex *mutex)
{
	CHECK(!ForbidLocks);
	return __real_postgamma_mutex_lock(mutex);
}

static void
native_alarm(int signal_number)
{
	(void) signal_number;
	NativeAlarms++;
}

static void
sleep_ms(unsigned milliseconds)
{
	struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
	while (nanosleep(&delay, &delay) != 0)
		CHECK(errno == EINTR);
}

typedef struct AlarmTest
{
	PostgammaBackendRegistry *registry;
	unsigned iteration;
	unsigned quantums;
	bool yielding;
} AlarmTest;

static uint64_t
take(AlarmTest *test, const PostgammaBackendStartInfo *info)
{
	uint64_t pending;
	CHECK(postgamma_backend_take_pending_signals(test->registry, info->handle.id, &pending) == 0);
	return pending;
}

static void
await_alarm(AlarmTest *test, const PostgammaBackendStartInfo *info)
{
	const PostgammaBackendInterruptState *interrupts =
		postgamma_backend_borrow_interrupts(test->registry, info->handle.id);
	uint64_t deadline = postgamma_monotonic_now_ns() + 5000 * MS;
	struct pollfd fd = {.fd = postgamma_wake_target_fd(info->wake_target), .events = POLLIN};
	uint64_t wake_count;
	CHECK(interrupts != NULL);
	while (!postgamma_backend_interrupts_pending(interrupts))
	{
		CHECK(postgamma_monotonic_now_ns() < deadline);
		CHECK(poll(&fd, 1, 5000) == 1);
		CHECK(postgamma_wake_target_drain(info->wake_target, &wake_count) == 0);
	}
}

static PostgammaBackendExitStatus
run_alarm(const PostgammaBackendStartInfo *info, const void *data, size_t size, void *argument)
{
	AlarmTest *test = argument;
	PostgammaBackendRegistry *registry = test->registry;
	PostgammaBackendId id = info->handle.id;
	uint64_t epoch;
	(void) data;
	(void) size;
	test->quantums++;
	CHECK(postgamma_backend_borrow_interrupts(registry, id) != NULL);
	if (test->yielding)
	{
		if (test->quantums == 1)
		{
			CHECK(postgamma_backend_schedule_epoch(registry, id, &epoch) == 0);
			CHECK(postgamma_backend_set_alarm(registry, id, postgamma_monotonic_now_ns() + 20 * MS, 0) == 0);
			CHECK(postgamma_backend_prepare_yield(registry, id, epoch, false) == 0);
			return (PostgammaBackendExitStatus) {POSTGAMMA_BACKEND_EXIT_YIELD, 0};
		}
		CHECK(test->quantums == 2 && take(test, info) == ALARM_BIT);
		return (PostgammaBackendExitStatus) {POSTGAMMA_BACKEND_EXIT_NORMAL, 0};
	}
	/* A retired slot's timer must never be inherited by a new generation. */
	if (test->iteration > 0)
		sleep_ms(60);
	CHECK(take(test, info) == 0);
	if (test->iteration == 0)
	{
		CHECK(postgamma_backend_set_alarm(registry, id, postgamma_monotonic_now_ns() + 100 * MS, 0) == 0);
		ForbidLocks = true;
		for (int i = 0; i < 100; i++)
			CHECK(!postgamma_backend_interrupt_poll_required(registry, id));
		ForbidLocks = false;
		CHECK(postgamma_backend_set_alarm(registry, id, 0, 0) == 0);
		sleep_ms(130);
		CHECK(take(test, info) == 0);
		CHECK(postgamma_backend_set_alarm(registry, id, postgamma_monotonic_now_ns() + 5000 * MS, 0) == 0);
		FailNotify = true;
		CHECK(postgamma_backend_set_alarm(registry, id, 1, 0) == EIO);
		FailNotify = false;
		sleep_ms(20);
		CHECK(take(test, info) == 0);
		/* Rearming later may wake the owner early; it must not deliver early. */
		CHECK(postgamma_backend_set_alarm(registry, id, postgamma_monotonic_now_ns() + 30 * MS, 0) == 0);
		CHECK(postgamma_backend_set_alarm(registry, id, postgamma_monotonic_now_ns() + 500 * MS, 0) == 0);
		sleep_ms(60);
		CHECK(take(test, info) == 0);
	}
	/* Earlier deadlines must interrupt the owner's existing wait. */
	CHECK(postgamma_backend_set_alarm(registry, id, postgamma_monotonic_now_ns() + 2 * MS, 0) == 0);
	await_alarm(test, info);
	CHECK(take(test, info) == ALARM_BIT);
	CHECK(take(test, info) == 0);
	CHECK(postgamma_backend_set_alarm(registry, id, postgamma_monotonic_now_ns() + 10 * MS, 10 * MS) == 0);
	for (int i = 0; i < 3; i++)
	{
		await_alarm(test, info);
		CHECK(take(test, info) == ALARM_BIT);
	}
	/* Cancellation does not withdraw a signal already delivered. */
	await_alarm(test, info);
	CHECK(postgamma_backend_set_alarm(registry, id, 0, 0) == 0);
	CHECK(take(test, info) == ALARM_BIT);
	sleep_ms(30);
	CHECK(take(test, info) == 0);
	CHECK(postgamma_backend_set_alarm(registry, id, postgamma_monotonic_now_ns() + 40 * MS, 0) == 0);
	return (PostgammaBackendExitStatus) {POSTGAMMA_BACKEND_EXIT_NORMAL, 0};
}

static void
test_provider(PostgammaBackendProviderKind kind)
{
	PostgammaBackendRegistry *registry;
	PostgammaBackendProviderConfig config = {.kind = kind};
	PostgammaBackendId old_id = 0;
	unsigned owners_before = atomic_load(&OwnersStarted);
	if (kind == POSTGAMMA_BACKEND_PROVIDER_POOLED)
	{
		config.worker_count = 2;
		config.queue_capacity = 8;
		config.execution_token_count = 2;
	}
	FailOwner = true;
	CHECK(postgamma_backend_registry_create_with_provider(&registry, 8, &config) == EAGAIN);
	CHECK(registry == NULL && atomic_load(&OwnersStarted) == owners_before);
	FailOwner = false;
	CHECK(postgamma_backend_registry_create_with_provider(&registry, 8, &config) == 0);
	CHECK(atomic_load(&OwnersStarted) == owners_before + 1);
	for (unsigned i = 0; i < 5; i++)
	{
		AlarmTest test = {.registry = registry, .iteration = i,
			.yielding = i == 4 && kind == POSTGAMMA_BACKEND_PROVIDER_POOLED};
		PostgammaBackendLaunchRequest request = {.execution_class = POSTGAMMA_BACKEND_CLASS_CLIENT,
			.main_function = run_alarm, .main_argument = &test};
		PostgammaBackendHandle handle;
		PostgammaBackendCompletion completion;
		struct pollfd fd = {.fd = postgamma_backend_registry_wake_fd(registry), .events = POLLIN};
		int status;
		CHECK(postgamma_backend_launch(registry, &request, &handle) == 0);
		if (old_id != 0)
			CHECK(postgamma_backend_set_alarm(registry, old_id, 1, 0) == ESTALE);
		while ((status = postgamma_backend_completion_pop(registry, &completion)) == EAGAIN)
			CHECK(poll(&fd, 1, 5000) == 1);
		CHECK(status == 0 && completion.handle.id == handle.id);
		CHECK(postgamma_backend_completion_join(registry, &completion) == 0);
		CHECK(test.quantums == (test.yielding ? 2u : 1u));
		old_id = handle.id;
	}
	CHECK(postgamma_backend_registry_destroy(registry) == 0);
	CHECK(atomic_load(&OwnersStarted) == owners_before + 1);
}

typedef struct RaceTest
{
	PostgammaBackendRegistry *registry;
	_Atomic bool stop;
} RaceTest;

static PostgammaBackendExitStatus
race_backend(const PostgammaBackendStartInfo *info, const void *data, size_t size, void *argument)
{
	RaceTest *test = argument;
	(void) data;
	(void) size;
	while (!atomic_load_explicit(&test->stop, memory_order_acquire))
	{
		uint64_t pending;
		CHECK(postgamma_backend_take_pending_signals(test->registry, info->handle.id, &pending) == 0);
		CHECK((pending & ~ALARM_BIT) == 0);
		sched_yield();
	}
	return (PostgammaBackendExitStatus) {POSTGAMMA_BACKEND_EXIT_NORMAL, 0};
}

static void
test_retirement_race(PostgammaBackendProviderKind kind)
{
	RaceTest test = {0};
	PostgammaBackendProviderConfig config = {.kind = kind};
	PostgammaBackendLaunchRequest request = {.execution_class = POSTGAMMA_BACKEND_CLASS_CLIENT,
		.main_function = race_backend, .main_argument = &test};
	PostgammaBackendHandle handle;
	PostgammaBackendCompletion completion;
	uint64_t deadline;
	int status;
	if (kind == POSTGAMMA_BACKEND_PROVIDER_POOLED)
	{
		config.worker_count = 2;
		config.queue_capacity = 8;
		config.execution_token_count = 2;
	}
	CHECK(postgamma_backend_registry_create_with_provider(&test.registry, 8, &config) == 0);
	atomic_init(&test.stop, false);
	CHECK(postgamma_backend_launch(test.registry, &request, &handle) == 0);
	for (int i = 0; i < 10000; i++)
		CHECK(postgamma_backend_set_alarm(test.registry, handle.id, i % 2 == 0 ? 1 : 0, 0) == 0);
	atomic_store_explicit(&test.stop, true, memory_order_release);
	deadline = postgamma_monotonic_now_ns() + 5000 * MS;
	while ((status = postgamma_backend_set_alarm(test.registry, handle.id, 1, 0)) == 0)
		CHECK(postgamma_monotonic_now_ns() < deadline);
	CHECK(status == ESTALE);
	while ((status = postgamma_backend_completion_pop(test.registry, &completion)) == EAGAIN)
		CHECK(postgamma_monotonic_now_ns() < deadline);
	CHECK(status == 0 && completion.handle.id == handle.id);
	CHECK(postgamma_backend_completion_join(test.registry, &completion) == 0);
	CHECK(postgamma_backend_set_alarm(test.registry, handle.id, 1, 0) == ESTALE);
	CHECK(postgamma_backend_registry_destroy(test.registry) == 0);
}

int
main(void)
{
	struct sigaction action = {.sa_handler = native_alarm};
	CHECK(sigemptyset(&action.sa_mask) == 0);
	CHECK(sigaction(SIGALRM, &action, NULL) == 0);
	test_provider(POSTGAMMA_BACKEND_PROVIDER_DEDICATED);
	test_provider(POSTGAMMA_BACKEND_PROVIDER_POOLED);
	test_retirement_race(POSTGAMMA_BACKEND_PROVIDER_DEDICATED);
	test_retirement_race(POSTGAMMA_BACKEND_PROVIDER_POOLED);
	CHECK(NativeAlarms == 0);
	return 0;
}
