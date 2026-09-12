/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include "postgamma/thread_runtime.h"
#include <assert.h>
#include <emscripten.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define ROUNDS 8
static pthread_t supervisor;
static _Atomic int finished;
static _Atomic unsigned destructors;
static pthread_key_t key;
static _Thread_local unsigned marker = 17;

static void
destroy_tls(void *value)
{
    assert(value == (void *) (uintptr_t) 42);
    atomic_fetch_add(&destructors, 1);
}

static void *
child(void *argument)
{
    PostgammaThreadStackInfo info;
    size_t limit;
    volatile char local = 0;
    assert(marker == 17);
    marker = 23;
    assert(postgamma_thread_current_stack_info(&info) == 0);
    assert((uintptr_t) &local >= info.stack_low_address);
    assert((uintptr_t) &local < info.stack_high_address);
    assert(info.configured_guard_size == 0 && info.native_guard_size == 0);
    assert(info.native_stack_size >= info.configured_stack_size);
    assert(postgamma_thread_current_stack_limit(&limit) == 0);
    assert(limit + POSTGAMMA_THREAD_STACK_RUNTIME_SLOP == info.usable_stack_size);
    assert(pthread_setspecific(key, (void *) (uintptr_t) 42) == 0);
    return argument;
}

static void *
run(void *unused)
{
    PostgammaThreadStackInfo info;
    PostgammaThread *thread = NULL;
    PostgammaThreadAttributes attributes = {0};
    (void) unused;
    assert(postgamma_thread_current_stack_info(&info) == ENOENT);
    assert(pthread_key_create(&key, destroy_tls) == 0);
    attributes.guard_size = 4096;
    assert(postgamma_thread_create(&thread, &attributes, child, NULL) == ENOTSUP);
    assert(thread == NULL);
    attributes.guard_size = 0;
    attributes.stack_size = 1024;
    assert(postgamma_thread_create(&thread, &attributes, child, NULL) == EINVAL);
    attributes.stack_size = 0;
    for (unsigned round = 0; round < ROUNDS; round++) {
        for (unsigned role = 0; role < POSTGAMMA_THREAD_ROLE_COUNT; role++) {
            void *result = NULL;
            attributes.role = (PostgammaThreadRole) role;
            int status = postgamma_thread_create(&thread, &attributes, child,
                                                 (void *) (uintptr_t) 42);
            if (status) fprintf(stderr, "create: round=%u role=%u status=%d\n", round, role, status);
            assert(status == 0);
            assert(!postgamma_thread_is_current(thread));
            assert(postgamma_thread_destroy(thread) == EBUSY);
            assert(postgamma_thread_join(thread, &result) == 0);
            assert(result == (void *) (uintptr_t) 42);
            assert(postgamma_thread_join(thread, NULL) == EINVAL);
            assert(postgamma_thread_destroy(thread) == 0);
            assert(postgamma_thread_current_stack_info(&info) == ENOENT);
            assert(marker == 17);
        }
    }
    assert(atomic_load(&destructors) == ROUNDS * POSTGAMMA_THREAD_ROLE_COUNT);
    assert(pthread_key_delete(key) == 0);
    atomic_store_explicit(&finished, 1, memory_order_release);
    return NULL;
}

EMSCRIPTEN_KEEPALIVE int
postgamma_runtime_test_start(void)
{
    PostgammaThread *thread = NULL;
    /* Blocking shared runtime entry points reject the control Worker. */
    assert(postgamma_thread_create(&thread, NULL, child, NULL) == EWOULDBLOCK);
    return pthread_create(&supervisor, NULL, run, NULL);
}

EMSCRIPTEN_KEEPALIVE int
postgamma_runtime_test_poll(void)
{
    if (!atomic_load_explicit(&finished, memory_order_acquire))
        return 0;
    /* tryjoin never blocks the control Worker during pthread exit cleanup. */
    int status = pthread_tryjoin_np(supervisor, NULL);
    if (status == EBUSY)
        return 0;
    assert(status == 0);
    return (int) atomic_load(&destructors);
}
