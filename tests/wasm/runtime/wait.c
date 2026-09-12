/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include "postgamma/io_platform.h"
#include "postgamma/thread_runtime.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

struct WaitTest {
    PostgammaWakeTarget *wake;
    pthread_barrier_t barrier;
    _Atomic unsigned consumed;
};

static void *
sender(void *argument)
{
    struct WaitTest *test = argument;
    for (unsigned i = 1; i <= 1000; i++) {
        pthread_barrier_wait(&test->barrier);
        assert(postgamma_wake_target_wake(test->wake) == 0);
        while (atomic_load_explicit(&test->consumed, memory_order_acquire) < i) {
            struct timespec delay = {0, 100000};
            nanosleep(&delay, NULL);
        }
    }
    pthread_barrier_wait(&test->barrier);
    struct timespec delay = {0, 100000000};
    nanosleep(&delay, NULL);
    assert(postgamma_wake_target_wake(test->wake) == 0);
    return NULL;
}

void
postgamma_runtime_wait_test(void)
{
    struct WaitTest test = {0};
    PostgammaThread *thread;
    struct pollfd fd;
    uint64_t count;
    size_t baseline = postgamma_platform_pipe_count();
    assert(postgamma_wake_target_create(&test.wake) == 0);
    fd = (struct pollfd) {.fd = postgamma_wake_target_fd(test.wake), .events = POLLIN};
    assert(postgamma_wake_target_drain(test.wake, &count) == 0 && count == 0);
    assert(postgamma_platform_poll(&fd, 1, 0) == 0);
    uint64_t before = postgamma_monotonic_now_ns();
    assert(postgamma_platform_poll(&fd, 1, 20) == 0);
    assert(postgamma_monotonic_now_ns() - before >= 19000000);
    assert(pthread_barrier_init(&test.barrier, NULL, 2) == 0);
    assert(postgamma_thread_create(&thread, NULL, sender, &test) == 0);
    for (unsigned i = 1; i <= 1000; i++) {
        pthread_barrier_wait(&test.barrier);
        assert(postgamma_platform_poll(&fd, 1, 1000) == 1);
        assert(fd.revents & POLLIN);
        assert(postgamma_wake_target_drain(test.wake, &count) == 0 && count == 1);
        atomic_store_explicit(&test.consumed, i, memory_order_release);
    }
    pthread_barrier_wait(&test.barrier);
    before = postgamma_monotonic_now_ns();
    assert(postgamma_platform_poll(&fd, 1, -1) == 1);
    assert(postgamma_monotonic_now_ns() - before >= 90000000);
    assert(postgamma_wake_target_drain(test.wake, &count) == 0 && count == 1);
    assert(postgamma_thread_join(thread, NULL) == 0);
    assert(postgamma_thread_destroy(thread) == 0);
    assert(pthread_barrier_destroy(&test.barrier) == 0);
    /* A saturated notification buffer remains readable and coalesces wakes. */
    for (unsigned i = 0; i < 5000; i++) assert(postgamma_wake_target_wake(test.wake) == 0);
    assert(postgamma_platform_poll(&fd, 1, 0) == 1);
    assert(postgamma_wake_target_drain(test.wake, &count) == 0 && count == 4096);
    assert(postgamma_wake_target_destroy(test.wake) == 0);
    assert(postgamma_platform_pipe_count() == baseline);

    int descriptors[2];
    char buffer[4] = {0};
    assert(postgamma_platform_pipe(descriptors) == 0);
    assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
    assert(postgamma_platform_read(descriptors[0], buffer, 1) == -1 && errno == EAGAIN);
    assert(postgamma_platform_write(descriptors[1], "abc", 3) == 3);
    assert(postgamma_platform_close(descriptors[1]) == 0);
    fd = (struct pollfd) {.fd = descriptors[0], .events = POLLIN};
    assert(postgamma_platform_poll(&fd, 1, 0) == 1);
    assert(fd.revents == (POLLIN | POLLHUP));
    assert(postgamma_platform_read(descriptors[0], buffer, 4) == 3);
    assert(strcmp(buffer, "abc") == 0);
    assert(postgamma_platform_read(descriptors[0], buffer, 1) == 0);
    assert(postgamma_platform_close(descriptors[0]) == 0);
    assert(postgamma_platform_pipe(descriptors) == 0);
    assert(postgamma_platform_close(descriptors[0]) == 0);
    assert(postgamma_platform_write(descriptors[1], buffer, 1) == -1 && errno == EPIPE);
    assert(postgamma_platform_close(descriptors[1]) == 0);
    assert(postgamma_platform_close(-1) == -1 && errno == EBADF);
    assert(postgamma_platform_pipe_count() == baseline);
}
