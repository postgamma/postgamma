/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include "postgamma/shmem_platform.h"
#include "postgamma/thread_runtime.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

struct SharedTest {
    uint32_t id;
    void *address;
    unsigned owner;
    _Atomic unsigned stage;
};

static void *
attached(void *argument)
{
    struct SharedTest *test = argument;
    assert(postgamma_platform_shmem_attach(&test->owner, 7, test,
                                          test->id, test->address) == 0);
    assert(postgamma_platform_shmem_attach(&test->owner, 7, test,
                                          test->id, test->address) == EALREADY);
    atomic_store_explicit(&test->stage, 1, memory_order_release);
    while (atomic_load_explicit(&test->stage, memory_order_acquire) != 2) {
        struct timespec delay = {0, 100000};
        nanosleep(&delay, NULL);
    }
    assert(*(uint64_t *) test->address == UINT64_C(0xfedcba9876543210));
    assert(postgamma_platform_shmem_in_use(test->id));
    assert(postgamma_platform_shmem_detach(&test->owner, 7, test, test->id) == 0);
    return NULL;
}

void
postgamma_runtime_shmem_test(void)
{
    uint32_t previous = 0;
    size_t baseline = postgamma_platform_shmem_count();
    uint32_t failed_id = 99;
    void *failed_address = (void *) (uintptr_t) 1;
    assert(postgamma_platform_shmem_create(&previous, 7, &failed_id,
        SIZE_MAX, &failed_id, &failed_address) == ENOMEM);
    assert(failed_id == 0 && failed_address == NULL);
    assert(postgamma_platform_shmem_count() == baseline);
    for (unsigned round = 0; round < 32; round++) {
        struct SharedTest test = {0};
        PostgammaThread *thread;
        assert(postgamma_platform_shmem_create(&test.owner, 7, &previous,
            1024 * 1024, &test.id, &test.address) == 0);
        assert(test.id > previous);
        previous = test.id;
        assert((uintptr_t) test.address % 8 == 0);
        assert(((unsigned char *) test.address)[1024 * 1024 - 1] == 0);
        assert(postgamma_platform_shmem_attach(&test.owner, 8, &test,
            test.id, test.address) == ESTALE);
        assert(postgamma_platform_shmem_attach(&previous, 7, &test,
            test.id, test.address) == ESTALE);
        assert(postgamma_platform_shmem_attach(&test.owner, 7, &test,
            test.id, (char *) test.address + 8) == EINVAL);
        assert(postgamma_thread_create(&thread, NULL, attached, &test) == 0);
        while (atomic_load_explicit(&test.stage, memory_order_acquire) != 1) {
            struct timespec delay = {0, 100000};
            nanosleep(&delay, NULL);
        }
        assert(postgamma_platform_shmem_remove(&test.owner, 8, test.id) == ESTALE);
        assert(postgamma_platform_shmem_remove(&test.owner, 7, test.id) == 0);
        assert(postgamma_platform_shmem_attach(&test.owner, 7, &thread,
            test.id, test.address) == ESTALE);
        assert(postgamma_platform_shmem_detach(&test.owner, 7, &previous, test.id) == 0);
        /* Removal and owner detach do not invalidate an attached logical role. */
        *(uint64_t *) test.address = UINT64_C(0xfedcba9876543210);
        atomic_store_explicit(&test.stage, 2, memory_order_release);
        assert(postgamma_thread_join(thread, NULL) == 0);
        assert(postgamma_thread_destroy(thread) == 0);
        assert(!postgamma_platform_shmem_in_use(test.id));
        assert(postgamma_platform_shmem_count() == baseline);
        assert(postgamma_platform_shmem_attach(&test.owner, 7, &test,
            test.id, test.address) == ESTALE);
    }
}
