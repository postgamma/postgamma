/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include <emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/stack.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "resource-policy.h"

static volatile unsigned char static_reserve[RESOURCE_STATIC];
static _Thread_local volatile unsigned char tls_reserve[RESOURCE_TLS];
static unsigned char *allocation;
/* Keep the malloc/free limit probe observable under allocation elision at O2. */
static void *(*volatile limit_allocate)(size_t) = malloc;
static _Atomic int gate, ready, done;
static unsigned failures, created;
static char json[16384];
extern unsigned char __heap_base;
struct Slot { pthread_t thread; uintptr_t base, end, tls; size_t heap_after; unsigned failures; };
static struct Slot slots[RESOURCE_THREADS];

static void *
thread_main(void *argument)
{
    size_t index = (size_t) argument;
    struct Slot *slot = &slots[index];
    slot->base = emscripten_stack_get_base();
    slot->end = emscripten_stack_get_end();
    slot->tls = (uintptr_t) tls_reserve;
    slot->failures = slot->base - slot->end != resource_stacks[index];
    for (size_t i = 0; i < RESOURCE_TLS; i += 4096) {
        if (tls_reserve[i]) slot->failures++;
        tls_reserve[i] = (unsigned char) (index + 1);
    }
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&gate)) emscripten_futex_wait(&gate, 0, 1000);
    for (size_t i = 0; i < RESOURCE_TLS; i += 4096)
        if (tls_reserve[i] != (unsigned char) (index + 1)) slot->failures++;
    for (size_t i = index * 4096; i < RESOURCE_ALLOCATION; i += RESOURCE_THREADS * 4096)
        if (allocation[i] != (unsigned char) (i / 4096)) slot->failures++;
    slot->heap_after = emscripten_get_heap_size();
    atomic_fetch_add(&done, 1);
    return NULL;
}

EMSCRIPTEN_KEEPALIVE const char *
resource_snapshot(void)
{
    snprintf(json, sizeof(json), "{\"heapBytes\":%zu,\"allocatorLiveBytes\":%zu,"
             "\"heapBase\":%u,\"controlStackBytes\":%u,\"pthreadCount\":%u,"
             "\"ready\":%d,\"done\":%d,\"controlTls\":%u,\"failures\":%u}",
             emscripten_get_heap_size(), (size_t) mallinfo().uordblks, (unsigned) (uintptr_t) &__heap_base,
             (unsigned) (emscripten_stack_get_base() - emscripten_stack_get_end()), RESOURCE_THREADS,
             atomic_load(&ready), atomic_load(&done), (unsigned) (uintptr_t) tls_reserve, failures);
    return json;
}

EMSCRIPTEN_KEEPALIVE int
resource_start(void)
{
    if (created) return EBUSY;
    atomic_store(&gate, 0); atomic_store(&ready, 0); atomic_store(&done, 0);
    for (size_t i = 0; i < RESOURCE_STATIC; i += 4096) static_reserve[i] = 42;
    for (size_t i = 0; i < RESOURCE_TLS; i += 4096) tls_reserve[i] = 99;
    for (size_t i = 0; i < RESOURCE_THREADS; i++) {
        pthread_attr_t attributes;
        int rc = pthread_attr_init(&attributes);
        if (rc) return rc;
        rc = pthread_attr_setstacksize(&attributes, resource_stacks[i]);
        if (!rc) rc = pthread_create(&slots[i].thread, &attributes, thread_main, (void *) i);
        pthread_attr_destroy(&attributes);
        if (rc) return rc;
        created++;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE const char *
resource_grow(void)
{
    size_t before = emscripten_get_heap_size();
    allocation = malloc(RESOURCE_ALLOCATION);
    if (!allocation) return "{\"error\":\"allocation failed\"}";
    for (size_t i = 0; i < RESOURCE_ALLOCATION; i += 4096) allocation[i] = (unsigned char) (i / 4096);
    /* Reject a request larger than the remaining address-space allowance. */
    errno = 0;
    void *excess = limit_allocate(RESOURCE_MAXIMUM);
    int allocation_errno = errno;
    int rejected = excess == NULL;
    free(excess);
    for (size_t i = 0; i < RESOURCE_STATIC; i += 4096) if (static_reserve[i] != 42) failures++;
    for (size_t i = 0; i < RESOURCE_TLS; i += 4096) if (tls_reserve[i] != 99) failures++;
    snprintf(json, sizeof(json), "{\"beforeBytes\":%zu,\"afterBytes\":%zu,\"requestedBytes\":%u,"
             "\"pointer\":%u,\"allocationRejected\":%s,\"errno\":%d,\"ENOMEM\":%d}",
             before, emscripten_get_heap_size(), RESOURCE_ALLOCATION, (unsigned) (uintptr_t) allocation,
             rejected ? "true" : "false", allocation_errno, ENOMEM);
    return json;
}

EMSCRIPTEN_KEEPALIVE void
resource_release(void)
{
    atomic_store(&gate, 1);
    emscripten_futex_wake(&gate, RESOURCE_THREADS);
}

EMSCRIPTEN_KEEPALIVE const char *
resource_join(void)
{
    if (atomic_load(&done) != RESOURCE_THREADS) return "{}";
    size_t used = (size_t) snprintf(json, sizeof(json), "{\"threads\":[");
    for (size_t i = 0; i < created; i++) {
        int rc = pthread_join(slots[i].thread, NULL);
        if (rc || slots[i].failures) failures++;
        used += (size_t) snprintf(json + used, sizeof(json) - used,
            "%s{\"stackBase\":%u,\"stackEnd\":%u,\"requestedStack\":%zu,\"tlsAddress\":%u,"
            "\"heapAfterBytes\":%zu,\"failures\":%u,\"joined\":%s}", i ? "," : "",
            (unsigned) slots[i].base, (unsigned) slots[i].end, resource_stacks[i],
            (unsigned) slots[i].tls, slots[i].heap_after, slots[i].failures, rc ? "false" : "true");
    }
    free(allocation); allocation = NULL; created = 0;
    snprintf(json + used, sizeof(json) - used, "],\"failures\":%u}", failures);
    return json;
}
