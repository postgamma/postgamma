/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#ifndef POSTGAMMA_WAIT_CLOCK_PROBE_H
#define POSTGAMMA_WAIT_CLOCK_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#define PROBE_PG_EPOCH_US INT64_C(946684800000000)
#define PROBE_MAX_WAIT_MS UINT64_C(2147483647)

enum ProbeWaitReason {
    PROBE_PENDING, PROBE_NOTIFIED, PROBE_TIMER, PROBE_CANCELLED,
    PROBE_CLOSED, PROBE_OWN_TIMEOUT, PROBE_ERROR
};

struct ProbeClock {
    uint64_t monotonic_ns;
    int64_t pg_us;
    uint64_t sample_span_ns;
};

struct ProbeBudget {
    int64_t milliseconds; /* -1 is infinite; finite waits round up. */
    int reason;
    bool clamped;
};

int probe_clock_now(struct ProbeClock *sample);
struct ProbeBudget probe_wait_budget(struct ProbeClock now, bool own_armed,
                                    uint64_t own_ns, bool timer_armed, int64_t timer_pg_us);

#endif
