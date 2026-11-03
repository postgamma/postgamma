/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include <emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

struct Slot { pthread_t thread; int expected, value; uintptr_t tls_address; _Atomic int state; };
static struct Slot slots[3];
static _Thread_local int local_value;
static _Atomic int release_gate;
static int count;
static char json[1024];

static void *
thread_main(void *argument)
{
    struct Slot *slot = argument;
    slot->value = local_value == 0 ? slot->expected : -1;
    local_value = slot->value;
    slot->tls_address = (uintptr_t) &local_value;
    atomic_store(&slot->state, 1);
    while (!atomic_load(&release_gate)) emscripten_futex_wait(&release_gate, 0, 1000);
    slot->value = local_value;
    atomic_store(&slot->state, 2);
    return NULL;
}

EMSCRIPTEN_KEEPALIVE int
deployment_start(int round)
{
    if (count) return -1;
    atomic_store(&release_gate, 0);
    for (int i = 0; i < 3; i++) {
        slots[i].expected = round * 100 + i;
        atomic_store(&slots[i].state, 0);
        int rc = pthread_create(&slots[i].thread, NULL, thread_main, &slots[i]);
        if (rc) return rc;
        count++;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE const char *
deployment_state(void)
{
    int states[3];
    for (int i = 0; i < 3; i++) states[i] = atomic_load(&slots[i].state);
    snprintf(json, sizeof(json), "{\"states\":[%d,%d,%d]}", states[0], states[1], states[2]);
    return json;
}

EMSCRIPTEN_KEEPALIVE void
deployment_release(void)
{
    atomic_store(&release_gate, 1);
    emscripten_futex_wake(&release_gate, 3);
}

EMSCRIPTEN_KEEPALIVE const char *
deployment_join(void)
{
    for (int i = 0; i < count; i++) if (atomic_load(&slots[i].state) != 2) return "{}";
    size_t used = 0;
    used += (size_t) snprintf(json, sizeof(json), "{\"threads\":[");
    for (int i = 0; i < count; i++) {
        int rc = pthread_join(slots[i].thread, NULL);
        used += (size_t) snprintf(json + used, sizeof(json) - used,
            "%s{\"done\":%s,\"value\":%d,\"expected\":%d,\"tlsAddress\":%u}",
            i ? "," : "", rc == 0 ? "true" : "false", slots[i].value, slots[i].expected,
            (unsigned) slots[i].tls_address);
    }
    snprintf(json + used, sizeof(json) - used, "]}");
    count = 0;
    return json;
}
