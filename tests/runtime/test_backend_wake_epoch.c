/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "postgamma/backend_execution_runtime.h"
#include "../check.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>

typedef struct WakeTest
{
	PostgammaBackendRegistry *registry;
	unsigned kind;
	unsigned calls;
	unsigned cleaned;
} WakeTest;

static PostgammaBackendExitStatus
run_quantum(const PostgammaBackendStartInfo *info, const void *data,
	size_t size, void *argument)
{
	WakeTest *test = argument;
	unsigned call = test->calls++;
	uint64_t epoch;
	uint64_t pending;

	(void) data;
	CHECK(size == 0 && call < 3);
	CHECK(postgamma_backend_take_pending_signals(test->registry, info->handle.id, &pending) == 0);
	CHECK(pending == (test->kind == 0 && call > 0 ? UINT64_C(1) << (SIGUSR1 - 1) : 0));
	if (call == 2)
		return (PostgammaBackendExitStatus) {POSTGAMMA_BACKEND_EXIT_NORMAL, 0};
	CHECK(postgamma_backend_schedule_epoch(test->registry, info->handle.id, &epoch) == 0);
	/* Exercise arrival both before the idle decision and before returning. */
	if (call == 1)
		CHECK(postgamma_backend_prepare_yield(test->registry, info->handle.id, epoch, false) == 0);
	if (test->kind == 0)
		CHECK(postgamma_backend_signal(test->registry, info->handle.compat_pid, SIGUSR1) == 0);
	else if (test->kind == 1)
		CHECK(postgamma_backend_wake(test->registry, info->handle.compat_pid) == 0);
	else
		CHECK(postgamma_backend_schedule(test->registry, info->handle.id) == 0);
	if (call == 0)
		CHECK(postgamma_backend_prepare_yield(test->registry, info->handle.id, epoch, false) == EAGAIN);
	return (PostgammaBackendExitStatus) {POSTGAMMA_BACKEND_EXIT_YIELD, 0};
}

static int
cleanup(const PostgammaBackendStartInfo *info, void *argument)
{
	WakeTest *test = argument;

	CHECK(info->handle.id != 0);
	test->cleaned++;
	return 0;
}

int
main(void)
{
	for (unsigned kind = 0; kind < 3; kind++)
	{
		WakeTest test = {.kind = kind};
		PostgammaBackendProviderConfig config = {
			.kind = POSTGAMMA_BACKEND_PROVIDER_POOLED,
			.worker_count = 2, .queue_capacity = 2, .execution_token_count = 2};
		PostgammaBackendLaunchRequest request = {
			.execution_class = POSTGAMMA_BACKEND_CLASS_CLIENT,
			.main_function = run_quantum, .main_argument = &test,
			.cleanup_function = cleanup, .cleanup_argument = &test};
		PostgammaBackendHandle handle;
		PostgammaBackendCompletion completion;
		struct pollfd fd;
		uint64_t deadline;
		int status;

		CHECK(postgamma_backend_registry_create_with_provider(&test.registry, 2, &config) == 0);
		fd = (struct pollfd) {.fd = postgamma_backend_registry_wake_fd(test.registry), .events = POLLIN};
		CHECK(postgamma_backend_launch(test.registry, &request, &handle) == 0);
		deadline = postgamma_monotonic_now_ns() + UINT64_C(5000000000);
		while ((status = postgamma_backend_completion_pop(test.registry, &completion)) == EAGAIN)
		{
			CHECK(postgamma_monotonic_now_ns() < deadline);
			CHECK(poll(&fd, 1, 5000) == 1);
		}
		CHECK(status == 0 && completion.handle.id == handle.id);
		CHECK(completion.exit_status.kind == POSTGAMMA_BACKEND_EXIT_NORMAL);
		CHECK(postgamma_backend_completion_join(test.registry, &completion) == 0);
		CHECK(test.calls == 3 && test.cleaned == 1);
		CHECK(postgamma_backend_registry_active_count(test.registry) == 0);
		CHECK(postgamma_backend_registry_destroy(test.registry) == 0);
	}
	puts("backend wake epoch: signals, wakes and scheduling preserve pending work");
	return 0;
}
