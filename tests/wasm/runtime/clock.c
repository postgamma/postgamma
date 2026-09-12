/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <emscripten.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef int64_t TimestampTz;
typedef int64_t int64;
typedef uint64_t uint64;
#include "timer-type.inc"
static PostgammaVirtualTimer timer;
static bool supplied_clock;
static TimestampTz clock_value;
static PostgammaVirtualTimer *postgamma_current_virtual_timer(void) { return &timer; }
static TimestampTz GetCurrentTimestamp(void)
{
    struct timespec now;
    if (supplied_clock) return clock_value;
    assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
    return (int64_t) now.tv_sec * 1000000 + now.tv_nsec / 1000 - INT64_C(946684800000000);
}
#include "timer-wait.inc"

EM_JS(void, shift_wall_clock, (int milliseconds), {
    if (!globalThis.postgammaOriginalDateNow) globalThis.postgammaOriginalDateNow = Date.now;
    Date.now = () => globalThis.postgammaOriginalDateNow() + milliseconds;
});

void
postgamma_runtime_clock_test(void)
{
    struct timespec mono_before, mono_after;
    timer.timeout_armed = false;
    assert(postgamma_backend_adjust_wait_timeout(-1) == -1);
    timer.timeout_armed = true;
    timer.timeout_due_at = GetCurrentTimestamp() + 60000000;
    assert(postgamma_backend_adjust_wait_timeout(-1) == 1000);
    assert(timer.timeout_wait_shortened);
    assert(postgamma_backend_adjust_wait_timeout(20) == 20);
    assert(!timer.timeout_wait_shortened);
    assert(clock_gettime(CLOCK_MONOTONIC, &mono_before) == 0);
    /* Change the actual SDK CLOCK_REALTIME source in this owning pthread. */
    shift_wall_clock(120000);
    assert(postgamma_backend_adjust_wait_timeout(-1) == 0);
    assert(postgamma_backend_timeout_wait_expired(-1));
    shift_wall_clock(-120000);
    assert(postgamma_backend_adjust_wait_timeout(-1) == 1000);
    assert(postgamma_backend_adjust_wait_timeout(20) == 20);
    shift_wall_clock(0);
    assert(clock_gettime(CLOCK_MONOTONIC, &mono_after) == 0);
    assert(mono_after.tv_sec >= mono_before.tv_sec);
    assert(mono_after.tv_sec - mono_before.tv_sec < 5);
    /* Rearm/disarm use current state; an old timeout is not a completion. */
    timer.timeout_due_at = GetCurrentTimestamp() - 1000000;
    assert(postgamma_backend_adjust_wait_timeout(-1) == 0);
    timer.timeout_due_at = GetCurrentTimestamp() + 60000000;
    assert(postgamma_backend_adjust_wait_timeout(-1) == 1000);
    timer.timeout_armed = false;
    assert(postgamma_backend_adjust_wait_timeout(-1) == -1);
    assert(!postgamma_backend_timeout_wait_expired(-1));
    supplied_clock = true;
    clock_value = INT64_MIN;
    timer.timeout_due_at = INT64_MAX;
    timer.timeout_armed = true;
    assert(postgamma_backend_adjust_wait_timeout(-1) == 1000);
    clock_value = INT64_MAX - 1;
    assert(postgamma_backend_adjust_wait_timeout(-1) == 1);
    clock_value = INT64_MAX;
    assert(postgamma_backend_adjust_wait_timeout(-1) == 0);
    supplied_clock = false;
}
