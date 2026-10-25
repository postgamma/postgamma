/* Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hold stack discovery at a barrier: unrelated registry work must progress,
 * while the reserved slot cannot run, complete, or be reclaimed prematurely.
 */
#define _GNU_SOURCE
#include "postgamma/backend_execution_runtime.h"
#include "../check.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>

static pthread_mutex_t Gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t Changed = PTHREAD_COND_INITIALIZER;
static bool Entered;
static bool Released;
static _Atomic bool StallNext;
static int StackError;
static _Atomic int Calls;
static _Atomic uint64_t Signals;
static PostgammaBackendRegistry *Registry;
static PostgammaBackendHandle DelayedHandle;
static int LaunchStatus;
static _Thread_local bool PauseCaller;
static bool PauseWaiting;
static PostgammaCompatPid PausePid;
static int PauseStatus;
static bool Async;
static bool Launched;
static _Atomic int FailedCleanups;

int postgamma_test_pthread_getattr_np(pthread_t thread, pthread_attr_t *attributes);
int __real_postgamma_condition_wait(PostgammaCondition *condition, PostgammaMutex *mutex);
int __wrap_postgamma_condition_wait(PostgammaCondition *condition, PostgammaMutex *mutex);

int
__wrap_postgamma_condition_wait(PostgammaCondition *condition, PostgammaMutex *mutex)
{
	if (PauseCaller)
	{
		CHECK(pthread_mutex_lock(&Gate) == 0);
		PauseWaiting = true;
		CHECK(pthread_cond_broadcast(&Changed) == 0);
		CHECK(pthread_mutex_unlock(&Gate) == 0);
	}
	return __real_postgamma_condition_wait(condition, mutex);
}

static void *pause_backend(void *unused)
{
	(void) unused;
	PauseCaller = true;
	PauseStatus = postgamma_backend_pause(Registry, PausePid);
	return NULL;
}

int
postgamma_test_pthread_getattr_np(pthread_t thread, pthread_attr_t *attributes)
{
	if (atomic_exchange(&StallNext, false))
	{
		CHECK(pthread_mutex_lock(&Gate) == 0);
		Entered = true;
		CHECK(pthread_cond_broadcast(&Changed) == 0);
		while (!Released)
			CHECK(pthread_cond_wait(&Changed, &Gate) == 0);
		CHECK(pthread_mutex_unlock(&Gate) == 0);
		if (StackError != 0)
			return StackError;
	}
	return pthread_getattr_np(thread, attributes);
}

static PostgammaBackendExitStatus
run_backend(const PostgammaBackendStartInfo *info, const void *data,
			size_t size, void *argument)
{
	PostgammaBackendSlotState state;
	uint64_t signals;
	(void) data;
	(void) size;
	(void) argument;
	CHECK(postgamma_backend_registry_lookup(Registry, info->handle.id, NULL, &state) == 0);
	CHECK(state == POSTGAMMA_BACKEND_SLOT_RUNNING && info->stack_status == 0);
	CHECK(postgamma_backend_wait_while_paused(Registry, info->handle.id) == 0);
	CHECK(postgamma_backend_take_pending_signals(Registry, info->handle.id, &signals) == 0);
	atomic_fetch_or(&Signals, signals);
	atomic_fetch_add(&Calls, 1);
	return (PostgammaBackendExitStatus) {POSTGAMMA_BACKEND_EXIT_NORMAL, 0};
}

static int
startup_failed(const PostgammaBackendStartInfo *info, void *argument)
{
	(void) argument;
	CHECK(info->stack_status == StackError && StackError != 0);
	CHECK(info->handle.id == DelayedHandle.id);
	atomic_fetch_add(&FailedCleanups, 1);
	return 0;
}

static void *launch(void *unused)
{
	PostgammaBackendLaunchRequest request = {
		.backend_type = 1, .execution_class = POSTGAMMA_BACKEND_CLASS_PARALLEL,
		.main_function = run_backend, .native_wake_signal = SIGUSR1};
	(void) unused;
	if (Async)
		request.startup_failure_function = startup_failed;
	LaunchStatus = postgamma_backend_launch(Registry, &request, &DelayedHandle);
	CHECK(pthread_mutex_lock(&Gate) == 0);
	Launched = true;
	CHECK(pthread_cond_broadcast(&Changed) == 0);
	CHECK(pthread_mutex_unlock(&Gate) == 0);
	return NULL;
}

static void join_one(PostgammaBackendHandle handle, PostgammaBackendExitKind kind)
{
	PostgammaBackendCompletion completion;
	struct pollfd fd = {.fd = postgamma_backend_registry_wake_fd(Registry), .events = POLLIN};
	for (;;)
	{
		int status = postgamma_backend_completion_pop(Registry, &completion);
		if (status == 0) break;
		CHECK(status == EAGAIN && poll(&fd, 1, 5000) == 1);
	}
	CHECK(completion.handle.id == handle.id);
	CHECK(completion.exit_status.kind == kind);
	CHECK(completion.exit_status.code == (kind == POSTGAMMA_BACKEND_EXIT_NORMAL ? 0 : 1));
	CHECK(postgamma_backend_completion_join(Registry, &completion) == 0);
}

int main(void)
{
	/* A regression to a blocking launch must fail rather than hang the gate. */
	alarm(30);
	for (int pooled = 0; pooled < 2; pooled++)
	for (int fail = 0; fail < 2; fail++)
	for (int asynchronous = 0; asynchronous < 2; asynchronous++)
	{
		PostgammaBackendProviderConfig config = {
			.kind = pooled ? POSTGAMMA_BACKEND_PROVIDER_POOLED : POSTGAMMA_BACKEND_PROVIDER_DEDICATED,
			.worker_count = pooled ? 2 : 0, .queue_capacity = pooled ? 2 : 0,
			.execution_token_count = pooled ? 2 : 0};
		PostgammaBackendHandle reserved;
		PostgammaBackendHandle other;
		PostgammaBackendSlotState state;
		PostgammaBackendTelemetry telemetry;
		PostgammaBackendLaunchRequest request = {
			.backend_type = 1, .execution_class = POSTGAMMA_BACKEND_CLASS_PARALLEL,
			.main_function = run_backend};
		pthread_t launcher;
		pthread_t pauser;
		pthread_attr_t host_attributes;
		CHECK(postgamma_backend_registry_create_with_provider(&Registry, 2, &config) == 0);
		Entered = Released = PauseWaiting = Launched = false;
		Async = asynchronous != 0;
		StackError = fail ? EIO : 0;
		atomic_store(&Calls, 0);
		atomic_store(&Signals, 0);
		atomic_store(&FailedCleanups, 0);
		atomic_store(&StallNext, true);
		/* Host and sanitizer stack queries must not consume the runtime gate. */
		CHECK(pthread_getattr_np(pthread_self(), &host_attributes) == 0);
		CHECK(pthread_attr_destroy(&host_attributes) == 0);
		CHECK(atomic_load(&StallNext));
		CHECK(pthread_create(&launcher, NULL, launch, NULL) == 0);
		CHECK(pthread_mutex_lock(&Gate) == 0);
		while (!Entered && !(Launched && LaunchStatus != 0))
			CHECK(pthread_cond_wait(&Changed, &Gate) == 0);
		CHECK(Entered);
		if (Async)
		{
			/* Return to the acceptor while the child cannot finish discovery. */
			while (!Launched) CHECK(pthread_cond_wait(&Changed, &Gate) == 0);
			CHECK(LaunchStatus == 0 && DelayedHandle.id != 0);
		}
		CHECK(pthread_mutex_unlock(&Gate) == 0);

		/* The first slot's generation is 1. Query through the public registry. */
		CHECK(postgamma_backend_registry_lookup(Registry, (UINT64_C(1) << 32) | 1,
			&reserved, &state) == 0);
		CHECK(state == POSTGAMMA_BACKEND_SLOT_STARTING);
		CHECK(postgamma_backend_registry_active_count(Registry) == 1);
		CHECK(postgamma_backend_registry_destroy(Registry) == EBUSY);
		CHECK(atomic_load(&Calls) == 0);
		CHECK(postgamma_backend_signal(Registry, reserved.compat_pid, SIGUSR1) == 0);
		CHECK(postgamma_backend_wake(Registry, reserved.compat_pid) == 0);
		CHECK(postgamma_backend_launch(Registry, &request, &other) == 0);
		join_one(other, POSTGAMMA_BACKEND_EXIT_NORMAL);
		CHECK(atomic_load(&Calls) == 1);
		CHECK(postgamma_backend_registry_active_count(Registry) == 1);

		PausePid = reserved.compat_pid;
		CHECK(pthread_create(&pauser, NULL, pause_backend, NULL) == 0);
		CHECK(pthread_mutex_lock(&Gate) == 0);
		while (!PauseWaiting) CHECK(pthread_cond_wait(&Changed, &Gate) == 0);
		Released = true;
		CHECK(pthread_cond_broadcast(&Changed) == 0);
		CHECK(pthread_mutex_unlock(&Gate) == 0);
		CHECK(pthread_join(launcher, NULL) == 0);
		CHECK(pthread_join(pauser, NULL) == 0);
		CHECK(LaunchStatus == (Async ? 0 : StackError));
		CHECK(PauseStatus == (fail ? ESRCH : 0));
		CHECK(atomic_load(&Calls) == 1);
		if (!fail)
		{
			CHECK(postgamma_backend_resume(Registry, DelayedHandle.compat_pid) == 0);
			join_one(DelayedHandle, POSTGAMMA_BACKEND_EXIT_NORMAL);
		}
		else if (Async)
			join_one(DelayedHandle, POSTGAMMA_BACKEND_EXIT_FATAL);
		CHECK(atomic_load(&FailedCleanups) == (Async && fail ? 1 : 0));
		CHECK(atomic_load(&Calls) == (fail ? 1 : 2));
		CHECK(atomic_load(&Signals) == (fail ? 0 : UINT64_C(1) << (SIGUSR1 - 1)));
		CHECK(postgamma_backend_registry_active_count(Registry) == 0);
		CHECK(postgamma_backend_registry_telemetry(Registry, &telemetry) == 0);
		CHECK(telemetry.execution_tokens_active == 0);
		/* Failed startup must release the slot and its execution token. */
		CHECK(postgamma_backend_launch(Registry, &request, &other) == 0);
		join_one(other, POSTGAMMA_BACKEND_EXIT_NORMAL);
		CHECK(postgamma_backend_registry_destroy(Registry) == 0);
	}
	alarm(0);
	return 0;
}
