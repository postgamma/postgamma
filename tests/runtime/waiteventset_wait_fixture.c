/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
/* Dependencies for the actual PostgreSQL wait loop emitted by the adapter. */
#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "postgamma/virtual_signal.h"
typedef int64_t TimestampTz;
typedef uint32_t uint32;
typedef int64_t instr_time;
typedef struct Latch { bool is_set, maybe_sleeping; } Latch;
typedef struct WaitEvent { int fd, pos; void *user_data; uint32 events; } WaitEvent;
typedef struct WaitEventSet { Latch *latch; int latch_pos; WaitEvent events[1]; } WaitEventSet;
#define Assert assert
#define PGINVALID_SOCKET (-1)
#define WL_LATCH_SET 1
#define WL_SOCKET_READABLE 2
#define WL_POSTMASTER_DEATH 16
#define pg_memory_barrier() ((void) 0)
#define INSTR_TIME_SET_CURRENT(time) ((time) = now_us)
#define INSTR_TIME_SET_ZERO(time) ((time) = 0)
#define INSTR_TIME_SUBTRACT(time, before) ((time) -= (before))
#define INSTR_TIME_GET_MILLISEC(time) ((time) / 1000)
static bool waiting;
static int64_t now_us;
static PostgammaVirtualTimer timer;
static PostgammaVirtualTimer *postgamma_current_virtual_timer(void) { return &timer; }
static TimestampTz GetCurrentTimestamp(void) { return now_us; }
#include "timer-wait.inc"
static unsigned polls, signals;
static bool rearm, periodic, ready_first;
static void pgstat_report_wait_start(uint32 info) { (void) info; }
static void pgstat_report_wait_end(void) {}
static void
postgamma_dispatch_pending_signals(void)
{
    if (postgamma_virtual_timer_due(&timer, now_us))
    {
        struct itimerval value = {0};

        signals++;
        if (rearm)
            value.it_value.tv_usec = periodic ? 1000 : 10000;
        // PostgreSQL's alarm handler can rearm or disarm the virtual timer.
        assert(postgamma_virtual_timer_set(&timer, ITIMER_REAL, &value, NULL, now_us) == 0);
    }
}
static int
postgamma_backend_filter_wake_events(WaitEvent *events, int count)
{
    (void) events;
    return count;
}
static int
WaitEventSetWaitBlock(WaitEventSet *set, int timeout, WaitEvent *events, int count)
{
    (void) set;
    assert(count == 1);
    polls++;
    if (ready_first || (!periodic && polls > 1))
    {
        *events = (WaitEvent) {.fd = 42, .events = WL_SOCKET_READABLE};
        return 1;
    }
    assert(timeout >= 0);
    now_us += (int64_t) timeout * 1000;
    return -1;
}
#include "wait.inc"
static void
reset(bool arm_again, bool tick_repeatedly)
{
    struct itimerval value = {0};

    now_us = 0;
    polls = signals = 0;
    rearm = arm_again;
    periodic = tick_repeatedly;
    ready_first = false;
    timer = (PostgammaVirtualTimer) {0};
    value.it_value.tv_usec = 1000;
    assert(postgamma_virtual_timer_set(&timer, ITIMER_REAL, &value, NULL, now_us) == 0);
}

int
main(void)
{
    Latch latch = {0};
    WaitEventSet set = {.latch = &latch};
    WaitEvent event;

    for (int has_latch = 0; has_latch < 2; has_latch++)
        for (int arms = 0; arms < 2; arms++)
        {
            reset(arms, false);
            set.latch = has_latch ? &latch : NULL;
            event = (WaitEvent) {.events = WL_POSTMASTER_DEATH};
            // A timer tick cannot turn an unbounded wait into a zero-event return.
            assert(WaitEventSetWait(&set, -1, &event, 1, 0) == 1);
            assert(event.events == WL_SOCKET_READABLE && event.fd == 42);
            assert(polls == 2 && signals == 1);
        }

    // Internal timer ticks cannot restart or prematurely end the caller's deadline.
    reset(true, true);
    set.latch = &latch;
    assert(WaitEventSetWait(&set, 5, &event, 1, 0) == 0);
    assert(now_us == 5000 && polls == 5 && signals == 5);

    reset(false, true);
    timer.timeout_due_at = 10000;
    assert(WaitEventSetWait(&set, 3, &event, 1, 0) == 0);
    assert(now_us == 3000 && polls == 1 && signals == 0);

    reset(true, false);
    ready_first = true;
    assert(WaitEventSetWait(&set, -1, &event, 1, 0) == 1);
    assert(polls == 1 && signals == 0);
    assert(!waiting && !timer.timeout_wait_shortened);
    puts("PASS real transformed wait: rearm/disarm, optional latch, infinite readiness and finite deadlines");
}
