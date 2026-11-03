/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "opfs.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CALLERS 3
#define OPERATIONS 5
#define SAMPLES 64
#define WARMUP 4
#define BLOCK 8192

struct Sample { double start, entered, left, end; int events; };
struct Bench { int slot, operation, group; };
static struct Sample samples[2 * OPERATIONS][CALLERS][SAMPLES + WARMUP];
static _Atomic int active_group[CALLERS], active_sample[CALLERS];
static _Atomic int observing;
static pthread_barrier_t barrier;
static em_proxying_queue *queue;
static unsigned char buffers[CALLERS][BLOCK];
static char json[49152];
_Atomic int opfs_counts[9], opfs_owner, opfs_owner_errors;
_Atomic int opfs_fault, opfs_faults_seen, opfs_gate, opfs_gate_entered;
static _Atomic int shared_view_errors;

EM_JS(double, opfs_now, (void), { return performance.timeOrigin + performance.now(); });

/* Separate calibration, not an extra copy inserted into the SDK I/O path. */
EM_JS(void, copy_block, (int slot), {
    const state = Module['opfsCopyCalibration'] ||= Array.from({length: 3}, () =>
        ({source: new Uint8Array(8192).fill(42), target: new Uint8Array(8192)}));
    state[slot].target.set(state[slot].source);
});

EMSCRIPTEN_KEEPALIVE void
opfs_event(int operation, int slot, double entered, double left, int shared)
{
    int owner = (int) (uintptr_t) pthread_self();
    int previous = atomic_load(&opfs_owner);
    if (previous == 0) atomic_store(&opfs_owner, owner);
    else if (previous != owner) atomic_fetch_add(&opfs_owner_errors, 1);
    atomic_fetch_add(&opfs_counts[operation], 1);
    if (!shared) atomic_fetch_add(&shared_view_errors, 1);
    if (operation == OP_DIR_BEGIN) atomic_store(&opfs_stage, 4);
    if (slot >= 0 && slot < CALLERS && atomic_load(&observing) && atomic_load(&active_sample[slot]) >= 0) {
        struct Sample *s = &samples[atomic_load(&active_group[slot])][slot]
                                  [atomic_load(&active_sample[slot])];
        s->entered = entered;
        s->left = left;
        s->events++;
    }
}

EMSCRIPTEN_KEEPALIVE int
opfs_take_fault(int operation)
{
    int fault = atomic_load(&opfs_fault);
    int matches = (operation == OP_READ && (fault == SHORT_READ || fault == READ_TYPE_ERROR)) ||
                  (operation == OP_WRITE && (fault == SHORT_WRITE || fault == WRITE_QUOTA || fault == WRITE_GATE)) ||
                  (operation == OP_FLUSH && fault == FLUSH_ERROR) ||
                  (operation == OP_DIR_BEGIN && fault == DIR_DELAY);
    if (!matches) return 0;
    atomic_store(&opfs_fault, 0);
    atomic_fetch_add(&opfs_faults_seen, 1);
    return fault;
}

EMSCRIPTEN_KEEPALIVE void
opfs_wait_gate(void)
{
    atomic_store(&opfs_gate_entered, 1);
    double deadline = opfs_now() + 5000;
    while (!atomic_load(&opfs_gate) && opfs_now() < deadline)
        emscripten_futex_wait(&opfs_gate, 0, 50);
    if (!atomic_load(&opfs_gate)) atomic_fetch_add(&opfs_failures, 1);
}

static void
proxy_baseline(void *argument)
{
    struct Bench *b = argument;
    struct Sample *s = &samples[b->group][b->slot][atomic_load(&active_sample[b->slot])];
    s->entered = opfs_now();
    if ((int) (uintptr_t) pthread_self() != atomic_load(&opfs_owner))
        atomic_fetch_add(&opfs_owner_errors, 1);
    if (b->operation == 4) copy_block(b->slot);
    s->left = opfs_now();
    s->events++;
}

static void *
bench_caller(void *argument)
{
    struct Bench *b = argument;
    int fd = -1;
    char path[32];
    unsigned char *buffer = buffers[b->slot];
    atomic_store(&active_sample[b->slot], -1);
    snprintf(path, sizeof(path), "/opfs/perf-%d", b->slot);
    memset(buffer, b->slot + 1, BLOCK);
    if (b->operation >= 1 && b->operation <= 3) {
        fd = open(path, O_CREAT | O_RDWR, 0600);
        opfs_check("benchmark open", fd >= 0, fd, errno, "supported");
        if (fd >= 0) {
            opfs_check("benchmark prepare", pwrite(fd, buffer, BLOCK, 0) == BLOCK, 0, 0, "supported");
            opfs_check("benchmark prepare flush", fsync(fd) == 0, 0, 0, "supported");
        }
    }
    atomic_store(&active_group[b->slot], b->group);
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < SAMPLES + WARMUP; i++) {
        struct Sample *s = &samples[b->group][b->slot][i];
        if (i == WARMUP) pthread_barrier_wait(&barrier);
        if (b->operation == 3) {
            atomic_store(&active_sample[b->slot], -1);
            buffer[0] = (unsigned char) i;
            if (pwrite(fd, buffer, BLOCK, 0) != BLOCK)
                opfs_check("dirty flush preparation", 0, -1, errno, "supported");
        }
        atomic_store(&active_sample[b->slot], i);
        s->start = opfs_now();
        long rc;
        if (b->operation == 0 || b->operation == 4)
            rc = emscripten_proxy_sync(queue, (pthread_t) (uintptr_t) atomic_load(&opfs_owner),
                                      proxy_baseline, b);
        else if (b->operation == 1) rc = pread(fd, buffer, BLOCK, 0);
        else if (b->operation == 2) rc = pwrite(fd, buffer, BLOCK, 0);
        else rc = fsync(fd);
        s->end = opfs_now();
        if (rc != ((b->operation == 0 || b->operation == 4) ? 1 : b->operation == 3 ? 0 : BLOCK))
            opfs_check("benchmark I/O result", 0, rc, errno, "supported");
        if (b->operation == 1 && (buffer[0] != b->slot + 1 || buffer[BLOCK - 1] != b->slot + 1))
            opfs_check("benchmark read contents", 0, 0, 0, "supported");
    }
    atomic_store(&active_sample[b->slot], -1);
    pthread_barrier_wait(&barrier);
    if (fd >= 0) close(fd);
    return NULL;
}

void
opfs_benchmark(void)
{
    queue = em_proxying_queue_create();
    opfs_check("proxy queue allocated", queue != NULL, 0, 0, "supported");
    if (!queue) return;
    for (int multi = 0; multi < 2; multi++) {
        for (int op = 0; op < OPERATIONS; op++) {
            int callers = multi ? CALLERS : 1;
            struct Bench args[CALLERS];
            pthread_t threads[CALLERS];
            pthread_barrier_init(&barrier, NULL, callers);
            atomic_store(&observing, op >= 1 && op <= 3);
            for (int slot = 0; slot < callers; slot++) {
                args[slot] = (struct Bench){slot, op, multi * OPERATIONS + op};
                if (slot) {
                    int rc = pthread_create(&threads[slot], NULL, bench_caller, &args[slot]);
                    // Exhausting the precreated pool must fail, never shrink the measurement.
                    if (rc) __builtin_trap();
                }
            }
            bench_caller(&args[0]);
            for (int slot = 1; slot < callers; slot++) pthread_join(threads[slot], NULL);
            atomic_store(&observing, 0);
            pthread_barrier_destroy(&barrier);
            for (int slot = 0; slot < callers; slot++) for (int i = WARMUP; i < SAMPLES + WARMUP; i++) {
                struct Sample *s = &samples[multi * OPERATIONS + op][slot][i];
                opfs_check("one owner event per measured call",
                           s->events == 1 && s->entered <= s->left && s->start <= s->end,
                           s->events, 0, "measurement");
            }
        }
    }
    em_proxying_queue_destroy(queue);
    opfs_check("SDK access uses shared heap views", !atomic_load(&shared_view_errors),
               atomic_load(&shared_view_errors), 0, "measurement");
}

EMSCRIPTEN_KEEPALIVE const char *
opfs_metrics(int group)
{
    if (group < 0 || group >= 2 * OPERATIONS || atomic_load(&opfs_stage) != 7) return "{}";
    int callers = group < OPERATIONS ? 1 : CALLERS;
    size_t used = (size_t) snprintf(json, sizeof(json), "{\"callers\":%d,\"operation\":%d,\"samples\":[",
                                  callers, group % OPERATIONS);
    for (int slot = 0; slot < callers; slot++) for (int i = WARMUP; i < SAMPLES + WARMUP; i++) {
        struct Sample *s = &samples[group][slot][i];
        used += (size_t) snprintf(json + used, sizeof(json) - used,
            "%s[%d,%.3f,%.6f,%.6f,%.6f,%.6f]", slot || i > WARMUP ? "," : "", slot,
            s->start, s->end - s->start, s->entered - s->start, s->left - s->entered, s->end - s->left);
    }
    snprintf(json + used, sizeof(json) - used, "]}");
    return json;
}
