/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include "postgamma/dsm_platform.h"
#include "postgamma/semaphore_platform.h"
#include "postgamma/thread_runtime.h"
#include "postgamma/io_platform.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

struct DsmTest { unsigned owner; sem_t sem; pthread_barrier_t barrier; _Atomic bool cancel; };

static void *
consumer(void *argument)
{
    struct DsmTest *test = argument;
    PostgammaDsmMapping *mapping = NULL;
    void *address = NULL;
    size_t size = 0;
    assert(postgamma_platform_dsm_attach(&test->owner, 4, 22, &mapping, &address, &size) == 0);
    assert(size == 65536 && *(uint64_t *) address == UINT64_C(0xfedcba9876543210));
    pthread_barrier_wait(&test->barrier);
    pthread_barrier_wait(&test->barrier);
    *(uint64_t *) address = 42;
    assert(postgamma_platform_dsm_detach(&mapping) == 0);
    assert(mapping == NULL);
    for (unsigned i = 0; i < 1000; i++) {
        pthread_barrier_wait(&test->barrier);
        assert(postgamma_platform_semaphore_post(&test->sem) == 0);
    }
    pthread_barrier_wait(&test->barrier);
    atomic_store_explicit(&test->cancel, true, memory_order_release);
    postgamma_platform_notify();
    return NULL;
}

void
postgamma_runtime_dsm_test(void)
{
    struct DsmTest test = {0};
    PostgammaDsmMapping *first = NULL, *second = NULL;
    void *address = NULL, *other_address = NULL;
    size_t size = 0, other_size = 0;
    size_t baseline = postgamma_platform_dsm_count();
    PostgammaThread *thread;
    assert(postgamma_platform_dsm_create(&test.owner, 4, 22, SIZE_MAX,
        &first, &address, &size) == ENOMEM);
    assert(first == NULL && postgamma_platform_dsm_count() == baseline);
    assert(postgamma_platform_dsm_create(&test.owner, 4, 22, 65536,
        &first, &address, &size) == 0);
    assert(size == 65536 && ((char *) address)[65535] == 0);
    assert(postgamma_platform_dsm_create(&test.owner, 4, 22, 65536,
        &second, &other_address, &other_size) == EEXIST);
    assert(postgamma_platform_dsm_attach(&test.owner, 5, 22,
        &second, &other_address, &other_size) == ENOENT);
    *(uint64_t *) address = UINT64_C(0xfedcba9876543210);
    assert(postgamma_platform_dsm_detach(&first) == 0);
    /* DSM's shared control refcount may keep a segment pinned with no mapping. */
    assert(postgamma_platform_dsm_count() == baseline + 1);
    assert(postgamma_platform_dsm_attach(&test.owner, 4, 22, &first, &address, &size) == 0);
    assert(pthread_barrier_init(&test.barrier, NULL, 2) == 0);
    sem_t unsupported;
    errno = 0;
    assert(sem_init(&unsupported, 1, 0) == ENOTSUP && errno == 0);
    assert(postgamma_platform_semaphore_init(&test.sem, 0) == 0);
    assert(postgamma_platform_semaphore_wait(&test.sem, 0) == -1 && errno == ETIMEDOUT);
    assert(postgamma_thread_create(&thread, NULL, consumer, &test) == 0);
    pthread_barrier_wait(&test.barrier);
    assert(postgamma_platform_dsm_destroy(&test.owner, 4, 22) == 0);
    assert(postgamma_platform_dsm_attach(&test.owner, 4, 22,
        &second, &other_address, &other_size) == ENOENT);
    pthread_barrier_wait(&test.barrier);
    for (unsigned i = 0; i < 1000; i++) {
        pthread_barrier_wait(&test.barrier);
        assert(postgamma_platform_semaphore_wait(&test.sem, 1000) == 0);
    }
    pthread_barrier_wait(&test.barrier);
    uint64_t before = postgamma_monotonic_now_ns();
    assert(postgamma_platform_semaphore_wait(&test.sem, 20) == -1 && errno == ETIMEDOUT);
    assert(atomic_load_explicit(&test.cancel, memory_order_acquire));
    assert(postgamma_monotonic_now_ns() - before >= 19000000);
    assert(postgamma_thread_join(thread, NULL) == 0);
    assert(postgamma_thread_destroy(thread) == 0);
    assert(*(uint64_t *) address == 42);
    assert(postgamma_platform_dsm_detach(&first) == 0);
    assert(postgamma_platform_dsm_count() == baseline);
    assert(postgamma_platform_dsm_detach(&first) == EINVAL);
    assert(sem_destroy(&test.sem) == 0);
    assert(pthread_barrier_destroy(&test.barrier) == 0);
}
