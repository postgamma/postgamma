/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "wait-clock.h"

#include <emscripten.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

int
probe_clock_now(struct ProbeClock *sample)
{
    struct timespec before, wall, after;
    uint64_t begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &before) ||
        clock_gettime(CLOCK_REALTIME, &wall) || clock_gettime(CLOCK_MONOTONIC, &after))
        return errno;
    if (wall.tv_sec < 0 || wall.tv_sec > INT64_MAX / INT64_C(1000000) - 1)
        return EOVERFLOW;
    begin = (uint64_t) before.tv_sec * UINT64_C(1000000000) + before.tv_nsec;
    end = (uint64_t) after.tv_sec * UINT64_C(1000000000) + after.tv_nsec;
    if (end < begin)
        return EDOM;
    sample->monotonic_ns = begin + (end - begin) / 2;
    sample->sample_span_ns = end - begin;
    sample->pg_us = (int64_t) wall.tv_sec * INT64_C(1000000) + wall.tv_nsec / 1000 - PROBE_PG_EPOCH_US;
    return 0;
}

struct ProbeBudget
probe_wait_budget(struct ProbeClock now, bool own_armed, uint64_t own_ns,
                  bool timer_armed, int64_t timer_pg_us)
{
    struct ProbeBudget result = {.milliseconds = -1, .reason = PROBE_PENDING};
    uint64_t remaining = UINT64_MAX;
    if (own_armed) {
        remaining = own_ns > now.monotonic_ns ? own_ns - now.monotonic_ns : 0;
        result.reason = PROBE_OWN_TIMEOUT;
    }
    if (timer_armed) {
        /* Subtract only after the signed comparison, without signed overflow. */
        uint64_t us = timer_pg_us > now.pg_us ? (uint64_t) timer_pg_us - (uint64_t) now.pg_us : 0;
        uint64_t ns = us > UINT64_MAX / 1000 ? UINT64_MAX : us * 1000;
        if (!own_armed || ns <= remaining) {
            remaining = ns;
            result.reason = PROBE_TIMER;
        }
    }
    if (own_armed || timer_armed) {
        const uint64_t cap_ns = PROBE_MAX_WAIT_MS * UINT64_C(1000000);
        result.clamped = remaining > cap_ns;
        if (result.clamped)
            remaining = cap_ns;
        result.milliseconds = (int64_t) ((remaining + 999999) / 1000000);
    }
    return result;
}

EMSCRIPTEN_KEEPALIVE const char *
probe_wait_clocks(void)
{
    struct clock_case {
        const char *name;
        struct ProbeClock now;
        bool own, timer;
        uint64_t own_ns;
        int64_t pg_us, expected_ms;
        int reason;
    } cases[] = {
        {"infinite", {0}, false, false, 0, 0, -1, PROBE_PENDING},
        {"own-deadline", {100000000, 500000, 0}, true, false, 150000000, 0, 50, PROBE_OWN_TIMEOUT},
        {"virtual-deadline", {100000000, 500000, 0}, false, true, 0, 525000, 25, PROBE_TIMER},
        {"own-earlier", {100000000, 500000, 0}, true, true, 110000000, 530000, 10, PROBE_OWN_TIMEOUT},
        {"virtual-earlier", {100000000, 500000, 0}, true, true, 130000000, 510000, 10, PROBE_TIMER},
        {"expired-own", {100000000, 500000, 0}, true, false, 99999999, 0, 0, PROBE_OWN_TIMEOUT},
        {"expired-virtual", {100000000, 500000, 0}, false, true, 0, 499999, 0, PROBE_TIMER},
        {"round-up-nanosecond", {100000000, 500000, 0}, true, false, 100000001, 0, 1, PROBE_OWN_TIMEOUT},
        {"round-up-microsecond", {100000000, 500000, 0}, false, true, 0, 500001, 1, PROBE_TIMER},
        {"wall-forward-at-resample", {100000000, 600000, 0}, true, true, 140000000, 550000, 0, PROBE_TIMER},
        {"wall-backward-at-resample", {100000000, 400000, 0}, true, true, 140000000, 550000, 40, PROBE_OWN_TIMEOUT},
        {"maximum-monotonic-delta", {0}, true, false, UINT64_MAX, 0, PROBE_MAX_WAIT_MS, PROBE_OWN_TIMEOUT},
        {"maximum-signed-wall-delta", {0, INT64_MIN, 0}, false, true, 0, INT64_MAX, PROBE_MAX_WAIT_MS, PROBE_TIMER},
    };
    static char report[8192];
    struct ProbeClock live = {0};
    int rc = probe_clock_now(&live);
    unsigned failures = rc != 0;
    size_t used = (size_t) snprintf(report, sizeof(report),
        "{\"clockReturn\":%d,\"monotonicNs\":\"%" PRIu64 "\",\"pgUs\":\"%" PRId64
        "\",\"unixUs\":\"%" PRId64 "\",\"sampleSpanNs\":\"%" PRIu64 "\","
        "\"errors\":{\"stale\":%d,\"mismatch\":%d,\"timeout\":%d},\"cases\":[",
        rc, live.monotonic_ns, live.pg_us, live.pg_us + PROBE_PG_EPOCH_US, live.sample_span_ns,
        -ESTALE, -EWOULDBLOCK, -ETIMEDOUT);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct clock_case *c = &cases[i];
        struct ProbeBudget b = probe_wait_budget(c->now, c->own, c->own_ns, c->timer, c->pg_us);
        bool passed = b.milliseconds == c->expected_ms && b.reason == c->reason;
        failures += !passed;
        used += (size_t) snprintf(report + used, sizeof(report) - used,
            "%s{\"name\":\"%s\",\"waitMs\":%" PRId64 ",\"reason\":%d,\"clamped\":%s,\"passed\":%s}",
            i ? "," : "", c->name, b.milliseconds, b.reason, b.clamped ? "true" : "false", passed ? "true" : "false");
    }
    snprintf(report + used, sizeof(report) - used, "],\"failures\":%u}", failures);
    return report;
}
