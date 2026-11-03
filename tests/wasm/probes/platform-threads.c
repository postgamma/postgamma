/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "platform.h"

#include <emscripten.h>
#include <emscripten/stack.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define THREADS 3
#define INCREMENTS 10000
#define COUNTER_START UINT64_C(0x20000000000001)

static _Thread_local int tls_value = 41;
static _Thread_local unsigned char tls_zero[64];
static pthread_t threads[THREADS];
static pthread_key_t key;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_barrier_t errno_barrier;
static sem_t wake, timeout_sem;
static atomic_int ready, done, published, destructors;
static _Atomic uint32_t counter32;
static _Atomic uint64_t counter64;
static bool released;
static unsigned round_number;
static int payload, capacity_rc;
static locale_t locales[2];

struct thread_result {
    unsigned id;
    uintptr_t tls_address, errno_address;
    uintptr_t base, end, entry, lowest, attr_start;
    size_t attr_size, guard_size, free_at_entry, free_at_peak;
    unsigned requested_stack, frame_checksum, locale_width;
    int initial_tls, final_tls, wait_rc, wait_errno, timed_rc, timed_errno;
    double timeout_ms;
};

static struct thread_result results[THREADS];

static double
monotonic_ms(void)
{
    struct timespec time;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return time.tv_sec * 1000.0 + time.tv_nsec / 1000000.0;
}

static void
destroy_tls(void *value)
{
    struct thread_result *result = value;
    CHECK(result >= results && result < results + THREADS);
    CHECK(tls_value == 100 + (int) result->id + (int) round_number * 10);
    atomic_fetch_add(&destructors, 1);
}

static __attribute__((noinline)) unsigned
stack_frames(unsigned depth, struct thread_result *result)
{
    volatile unsigned char frame[2048];
    uintptr_t current;
    unsigned value;
    for (size_t i = 0; i < sizeof(frame); i++)
        frame[i] = (unsigned char) (depth + i);
    current = emscripten_stack_get_current();
    if (current < result->lowest) {
        result->lowest = current;
        result->free_at_peak = emscripten_stack_get_free();
    }
    value = depth ? stack_frames(depth - 1, result) : 0;
    /* Read after recursion so the frames must coexist. */
    return value + frame[depth] + frame[sizeof(frame) - 1];
}

static int callback(int value) { return value * 3 + 1; }

static void *
thread_main(void *argument)
{
    struct thread_result *r = argument;
    pthread_attr_t attr;
    void *stack_start = NULL;
    int expected_tls = 100 + (int) r->id + (int) round_number * 10;
    int expected_errno = 120 + (int) r->id;
    int (*volatile indirect)(int) = callback;
    locale_t previous;

    r->initial_tls = tls_value;
    r->tls_address = (uintptr_t) &tls_value;
    r->errno_address = (uintptr_t) &errno;
    CHECK(tls_value == 41);
    for (size_t i = 0; i < sizeof(tls_zero); i++)
        CHECK(tls_zero[i] == 0);
    memset(tls_zero, (int) r->id + 1, sizeof(tls_zero));
    tls_value = expected_tls;
    CHECK(pthread_setspecific(key, r) == 0);
    CHECK(pthread_getspecific(key) == r);
    CHECK(indirect((int) r->id) == (int) r->id * 3 + 1);

    CHECK(pthread_getattr_np(pthread_self(), &attr) == 0);
    CHECK(pthread_attr_getstack(&attr, &stack_start, &r->attr_size) == 0);
    CHECK(pthread_attr_getguardsize(&attr, &r->guard_size) == 0);
    CHECK(pthread_attr_destroy(&attr) == 0);
    r->attr_start = (uintptr_t) stack_start;
    r->base = emscripten_stack_get_base();
    r->end = emscripten_stack_get_end();
    r->entry = r->lowest = emscripten_stack_get_current();
    r->free_at_entry = emscripten_stack_get_free();
    r->frame_checksum = stack_frames(11, r);
    CHECK(r->base > r->entry && r->entry > r->lowest && r->lowest > r->end);
    CHECK(r->attr_start == r->end && r->attr_start + r->attr_size == r->base);
    CHECK(r->attr_size == r->requested_stack);
    CHECK(r->entry - r->lowest >= 12 * 2048);
    CHECK(r->free_at_peak > 32768 && r->free_at_peak < r->free_at_entry);

    previous = uselocale(locales[r->id % 2]);
    r->locale_width = MB_CUR_MAX;
    CHECK(r->locale_width == (r->id % 2 ? 4 : 1));
    for (unsigned i = 0; i < INCREMENTS; i++) {
        atomic_fetch_add_explicit(&counter32, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&counter64, 1, memory_order_relaxed);
    }

    if (r->id == 2) {
        struct timespec deadline;
        double start = monotonic_ms();
        CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
        deadline.tv_nsec += 50000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        errno = 0;
        r->timed_rc = sem_timedwait(&timeout_sem, &deadline);
        r->timed_errno = errno;
        r->timeout_ms = monotonic_ms() - start;
        CHECK(r->timed_rc == -1 && r->timed_errno == ETIMEDOUT);
        CHECK(r->timeout_ms >= 49.0);
        /* An expired absolute deadline also returns immediately. */
        deadline.tv_sec = deadline.tv_nsec = 0;
        errno = 0;
        CHECK(sem_timedwait(&timeout_sem, &deadline) == -1 && errno == ETIMEDOUT);
    }
    errno = expected_errno;
    if (r->id == 1) {
        CHECK(pthread_mutex_lock(&wait_lock) == 0);
        atomic_fetch_add_explicit(&ready, 1, memory_order_release);
        while (!released)
            CHECK(pthread_cond_wait(&condition, &wait_lock) == 0);
        CHECK(pthread_mutex_unlock(&wait_lock) == 0);
    } else {
        atomic_fetch_add_explicit(&ready, 1, memory_order_release);
        r->wait_rc = sem_wait(&wake);
        r->wait_errno = errno;
        CHECK(r->wait_rc == 0);
    }
    CHECK(atomic_load_explicit(&published, memory_order_acquire) == 1 && payload == 2026);
    /* Successful semaphore calls may change errno. Synchronize fresh TLS markers. */
    errno = expected_errno;
    {
        int rc = pthread_barrier_wait(&errno_barrier);
        CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
    }
    CHECK(errno == expected_errno);
    CHECK(tls_value == expected_tls && pthread_getspecific(key) == r);
    CHECK(pthread_equal(pthread_self(), threads[r->id]));
    CHECK(MB_CUR_MAX == r->locale_width && uselocale((locale_t) 0) == locales[r->id % 2]);
    for (size_t i = 0; i < sizeof(tls_zero); i++)
        CHECK(tls_zero[i] == r->id + 1);
    r->final_tls = tls_value;
    CHECK(uselocale(previous) == locales[r->id % 2]);
    atomic_fetch_add_explicit(&done, 1, memory_order_release);
    return (void *) (uintptr_t) (0x100 + r->id);
}

static void *unused_thread(void *value) { return value; }

EMSCRIPTEN_KEEPALIVE int
probe_threads_start(unsigned round)
{
    pthread_t extra;
    probe_begin();
    round_number = round;
    memset(results, 0, sizeof(results));
    atomic_store(&ready, 0);
    atomic_store(&done, 0);
    atomic_store(&published, 0);
    atomic_store(&destructors, 0);
    atomic_store(&counter32, 0);
    atomic_store(&counter64, COUNTER_START);
    released = false;
    payload = 0;
    CHECK(tls_value == 41 && MB_CUR_MAX == 1);
    CHECK(pthread_key_create(&key, destroy_tls) == 0);
    CHECK(pthread_barrier_init(&errno_barrier, NULL, THREADS) == 0);
    CHECK(sem_init(&wake, 0, 0) == 0 && sem_init(&timeout_sem, 0, 0) == 0);
    locales[0] = newlocale(LC_ALL_MASK, "C", (locale_t) 0);
    locales[1] = newlocale(LC_ALL_MASK, "C.UTF-8", (locale_t) 0);
    if (!locales[0] || !locales[1])
        return ENOMEM;
    for (unsigned i = 0; i < THREADS; i++) {
        pthread_attr_t attr;
        int rc;
        results[i].id = i;
        results[i].requested_stack = i == 0 ? 262144 : i == 1 ? 524288 : 1048576;
        CHECK(pthread_attr_init(&attr) == 0);
        if (i != 2)
            CHECK(pthread_attr_setstacksize(&attr, results[i].requested_stack) == 0);
        rc = pthread_create(&threads[i], &attr, thread_main, &results[i]);
        CHECK(pthread_attr_destroy(&attr) == 0);
        if (rc)
            return rc;
    }
    /* No thread can exit before release, so all precreated slots are occupied. */
    capacity_rc = pthread_create(&extra, NULL, unused_thread, NULL);
    CHECK(capacity_rc == EAGAIN);
    if (capacity_rc == 0) {
        pthread_join(extra, NULL);
        return EINVAL;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE int
probe_threads_state(void)
{
    return (atomic_load_explicit(&ready, memory_order_acquire) << 8) |
           atomic_load_explicit(&done, memory_order_acquire);
}

EMSCRIPTEN_KEEPALIVE void
probe_threads_release(void)
{
    payload = 2026;
    atomic_store_explicit(&published, 1, memory_order_release);
    CHECK(pthread_mutex_lock(&wait_lock) == 0);
    released = true;
    CHECK(pthread_cond_broadcast(&condition) == 0);
    CHECK(pthread_mutex_unlock(&wait_lock) == 0);
    CHECK(sem_post(&wake) == 0 && sem_post(&wake) == 0);
}

EMSCRIPTEN_KEEPALIVE const char *
probe_threads_finish(void)
{
    uint32_t expected32 = THREADS * INCREMENTS;
    uint64_t expected64 = COUNTER_START + THREADS * INCREMENTS;
    uint32_t wrong32 = 0;
    uint64_t wrong64 = 0;
    CHECK(atomic_load_explicit(&done, memory_order_acquire) == THREADS);
    for (unsigned i = 0; i < THREADS; i++) {
        void *value = NULL;
        CHECK(pthread_join(threads[i], &value) == 0);
        CHECK((uintptr_t) value == 0x100 + i);
        for (unsigned j = 0; j < i; j++) {
            CHECK(results[i].tls_address != results[j].tls_address);
            CHECK(results[i].errno_address != results[j].errno_address);
        }
        CHECK(results[i].tls_address != (uintptr_t) &tls_value);
    }
    CHECK(atomic_load(&destructors) == THREADS && tls_value == 41 && MB_CUR_MAX == 1);
    CHECK(atomic_load(&counter32) == expected32 && atomic_load(&counter64) == expected64);
    CHECK(atomic_compare_exchange_strong(&counter32, &expected32, 7));
    CHECK(atomic_compare_exchange_strong(&counter64, &expected64, COUNTER_START));
    CHECK(atomic_load(&counter32) == 7 && atomic_load(&counter64) == COUNTER_START);
    CHECK(!atomic_compare_exchange_strong(&counter32, &wrong32, 123) && wrong32 == 7);
    CHECK(!atomic_compare_exchange_strong(&counter64, &wrong64, 123) && wrong64 == COUNTER_START);
    CHECK(sem_destroy(&wake) == 0 && sem_destroy(&timeout_sem) == 0);
    CHECK(pthread_key_delete(key) == 0);
    CHECK(pthread_barrier_destroy(&errno_barrier) == 0);
    freelocale(locales[0]);
    freelocale(locales[1]);
    probe_report("\"round\":%u,\"capacityReturn\":%d,\"EAGAIN\":%d,\"destructors\":%d,"
                 "\"atomic32\":%" PRIu32 ",\"atomic64\":\"%" PRIu64 "\","
                 "\"atomic32LockFree\":%s,\"atomic64LockFree\":%s,\"threads\":[",
                 round_number, capacity_rc, EAGAIN, atomic_load(&destructors), expected32, expected64,
                 atomic_is_lock_free(&counter32) ? "true" : "false",
                 atomic_is_lock_free(&counter64) ? "true" : "false");
    for (unsigned i = 0; i < THREADS; i++) {
        struct thread_result *r = &results[i];
        probe_report("%s{\"id\":%u,\"wait\":\"%s\",\"tlsAddress\":%" PRIuPTR ","
                     "\"errnoAddress\":%" PRIuPTR ",\"initialTls\":%d,\"finalTls\":%d,"
                     "\"localeWidth\":%u,\"stackRequested\":%u,\"stackAttributeSize\":%zu,"
                     "\"guardAttributeSize\":%zu,\"stackBase\":%" PRIuPTR ",\"stackEnd\":%" PRIuPTR ","
                     "\"stackEntry\":%" PRIuPTR ",\"stackLowest\":%" PRIuPTR ","
                     "\"entryUsed\":%" PRIuPTR ",\"peakUsed\":%" PRIuPTR ","
                     "\"freeAtEntry\":%zu,\"freeAtPeak\":%zu,\"frameChecksum\":%u,"
                     "\"waitReturn\":%d,\"waitErrno\":%d,\"timedReturn\":%d,"
                     "\"timedErrno\":%d,\"timeoutMs\":%.3f}",
                     i ? "," : "", i, i == 1 ? "pthread_cond_wait" : "sem_wait",
                     r->tls_address, r->errno_address, r->initial_tls, r->final_tls, r->locale_width,
                     r->requested_stack, r->attr_size, r->guard_size, r->base, r->end, r->entry,
                     r->lowest, r->base - r->entry, r->base - r->lowest, r->free_at_entry,
                     r->free_at_peak, r->frame_checksum, r->wait_rc, r->wait_errno,
                     r->timed_rc, r->timed_errno, r->timeout_ms);
    }
    probe_report("],");
    return probe_end();
}
