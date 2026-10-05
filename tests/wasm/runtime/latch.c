/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <emscripten.h>
#include <emscripten/threading.h>
#include "pg-latch.inc"
typedef uint32_t uint32;
typedef double instr_time;
typedef struct {int fd, pos; void *user_data; uint32 events;} WaitEvent;
typedef struct {Latch *latch; int latch_pos; WaitEvent events[1];} WaitEventSet;
#define Assert assert
#define PGINVALID_SOCKET (-1)
#define WL_LATCH_SET 1
#define pg_memory_barrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define INSTR_TIME_SET_CURRENT(v) ((v) = emscripten_get_now())
#define INSTR_TIME_SET_ZERO(v) ((v) = 0)
#define INSTR_TIME_SUBTRACT(v, before) ((v) -= (before))
#define INSTR_TIME_GET_MILLISEC(v) (v)
static _Thread_local int MyProcPid;
static _Thread_local bool waiting;
static Latch latch;
static _Atomic int start, done, epoch, pending, completed, failed;
static int limit;
static void
wake(void)
{
    atomic_store(&pending, 1);
    atomic_fetch_add(&epoch, 1);
    emscripten_futex_wake(&epoch, INT_MAX);
}
static void WakeupMyProc(void) {wake();}
static void WakeupOtherProc(int pid) {(void) pid; wake();}
#include "pg-latch-set.inc"
#include "pg-latch-reset.inc"
static long postgamma_backend_adjust_wait_timeout(long t) {return t;}
static int postgamma_backend_filter_wake_events(WaitEvent *e, int n) {(void) e; return n;}
static bool postgamma_backend_timeout_wait_expired(int n) {(void) n; return false;}
static void postgamma_dispatch_pending_signals(void) {}
static void pgstat_report_wait_start(uint32 info) {(void) info;}
static void pgstat_report_wait_end(void) {}
static int
WaitEventSetWaitBlock(WaitEventSet *set, int timeout, WaitEvent *events, int count)
{
    (void) set; (void) events; (void) count;
    assert(waiting);
    int expected = atomic_load(&epoch);
    if (atomic_exchange(&pending, 0)) return 0;
    int rc = emscripten_futex_wait(&epoch, expected, timeout);
    return rc == -ETIMEDOUT ? -1 : 0;
}
#include "pg-latch-wait.inc"
static void *
sender(void *arg)
{
    (void) arg;
    MyProcPid = 456;
    for (int i = 1; i <= limit; i++) {
        for (;;) {
            int ticket = atomic_load(&start);
            if (ticket >= i) break;
            emscripten_futex_wait(&start, ticket, 1000);
        }
        SetLatch(&latch);
        atomic_store(&done, i);
        emscripten_futex_wake(&done, INT_MAX);
    }
    return NULL;
}
static void *
waiter(void *arg)
{
    (void) arg;
    MyProcPid = 123;
    WaitEventSet set = {.latch = &latch};
    WaitEvent event;
    for (int i = 1; i <= limit; i++) {
        ResetLatch(&latch);
        atomic_store(&pending, 0);
        atomic_store(&start, i);
        emscripten_futex_wake(&start, INT_MAX);
        int rc = WaitEventSetWait(&set, 1000, &event, 1, 0);
        for (;;) {
            int ack = atomic_load(&done);
            if (ack >= i) break;
            emscripten_futex_wait(&done, ack, 1000);
        }
        if (rc != 1) {
            atomic_fetch_add(&failed, 1);
            /* Release the producer so both native fixture threads retire. */
            atomic_store(&start, limit);
            emscripten_futex_wake(&start, INT_MAX);
            break;
        }
        atomic_store(&completed, i);
    }
    return NULL;
}
/* Exercise the actual adapted PostgreSQL handshake on two native Wasm threads.
 * The mock wake primitive keeps readiness durable and isolates the shared flags.
 * The supervisor joins both threads before the runtime suite continues. */
void
postgamma_runtime_latch_test(void)
{
    limit = 200000;
    latch.owner_pid = 123;
    _Static_assert(sizeof(Latch) == 16, "Latch ABI");
    _Static_assert(_Alignof(Latch) == 4, "Latch alignment");
    _Static_assert(_Generic(&latch.is_set, _Atomic(sig_atomic_t) *: 1, default: 0), "atomic is_set");
    _Static_assert(_Generic(&latch.maybe_sleeping, _Atomic(sig_atomic_t) *: 1, default: 0), "atomic sleeping");
    _Static_assert(__atomic_always_lock_free(sizeof(sig_atomic_t), 0), "lock-free flags");
    pthread_t a, b;
    assert(pthread_create(&a, NULL, sender, NULL) == 0);
    assert(pthread_create(&b, NULL, waiter, NULL) == 0);
    assert(pthread_join(a, NULL) == 0);
    assert(pthread_join(b, NULL) == 0);
    assert(atomic_load(&failed) == 0);
    assert(atomic_load(&completed) == limit);
    puts("PASS real PostgreSQL latch: 200000 concurrent handoffs");
}
