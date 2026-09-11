/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include "wait-clock.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define SLOTS 3
#define TRACE_CAPACITY 96

enum Stage { STARTING, GATE_START, GATE_CHECK, WAITING, GATE_TIMEOUT, FINISHED, JOINED };
enum Action { NOTIFY = 1, CANCEL, CLOSE, ARM, DISARM, TIMER_EVENT, POKE };
enum TraceKind { TRACE_START, TRACE_GATE, TRACE_WAIT, TRACE_RETURN, TRACE_ACTION,
                 TRACE_STALE, TRACE_FINISH, TRACE_JOIN };

struct Trace {
    uint64_t ns;
    uint32_t sequence, timer;
    int kind;
    int64_t value;
};

struct Request {
    uint32_t id, flags, payload;
    int stage, gate_mode, outcome, error;
    bool active, done, gate_used, own_armed, timer_armed;
    uint64_t start_ns, entry_ns, finish_ns, own_ns;
    int64_t timer_pg_us, finish_pg_us;
    unsigned waits, timed_out, mismatch, interrupted, completions, stale, poke_woken;
    int64_t last_budget_ms;
    unsigned trace_count, trace_dropped;
    struct Trace trace[TRACE_CAPACITY];
};

struct Slot {
    pthread_mutex_t lock;
    pthread_t thread;
    _Atomic uint32_t sequence, gate;
    uint32_t last_id, timer_generation;
    bool in_use;
    struct Request request;
};

static struct Slot slots[SLOTS];
static bool initialized;
static char report[24576];

/* All mutable request fields and trace entries are protected by the slot lock. */
static void
trace(struct Slot *s, int kind, int64_t value)
{
    struct ProbeClock now = {0};
    struct Request *r = &s->request;
    int rc = probe_clock_now(&now);
    if (rc) r->error = rc;
    if (r->trace_count == TRACE_CAPACITY) {
        r->trace_dropped++;
        return;
    }
    r->trace[r->trace_count++] = (struct Trace) {
        now.monotonic_ns, atomic_load_explicit(&s->sequence, memory_order_relaxed),
        s->timer_generation, kind, value,
    };
}

static void
pause_at_gate(struct Slot *s, int stage)
{
    pthread_mutex_lock(&s->lock);
    s->request.stage = stage;
    s->request.gate_used = true;
    trace(s, TRACE_GATE, stage);
    pthread_mutex_unlock(&s->lock);
    while (atomic_load_explicit(&s->gate, memory_order_acquire) == 0) {
        int rc = emscripten_futex_wait(&s->gate, 0, INFINITY);
        if (rc != 0 && rc != -EWOULDBLOCK && rc != -EINTR) {
            pthread_mutex_lock(&s->lock);
            s->request.error = -rc;
            pthread_mutex_unlock(&s->lock);
            return;
        }
    }
}

static void *
wait_main(void *argument)
{
    struct Slot *s = argument;
    struct Request *r = &s->request;
    struct ProbeClock now = {0};
    probe_clock_now(&now);
    pthread_mutex_lock(&s->lock);
    r->entry_ns = now.monotonic_ns;
    int gate_mode = r->gate_mode;
    pthread_mutex_unlock(&s->lock);
    if (gate_mode == GATE_START)
        pause_at_gate(s, GATE_START);
    for (;;) {
        struct ProbeBudget budget;
        uint32_t expected;
        bool gate_check;
        int rc, reason = PROBE_PENDING;
        pthread_mutex_lock(&s->lock);
        rc = probe_clock_now(&now);
        if (rc) r->error = rc;
        budget = probe_wait_budget(now, r->own_armed, r->own_ns, r->timer_armed, r->timer_pg_us);
        if (r->error) reason = PROBE_ERROR;
        else if (r->flags & 4) reason = PROBE_CLOSED;
        else if (r->flags & 2) reason = PROBE_CANCELLED;
        else if (r->flags & 1) reason = PROBE_NOTIFIED;
        else if (budget.milliseconds == 0) reason = budget.reason;
        if (reason != PROBE_PENDING) {
            r->outcome = reason;
            r->finish_ns = now.monotonic_ns;
            r->finish_pg_us = now.pg_us;
            r->completions++;
            r->active = false;
            r->done = true;
            r->stage = FINISHED;
            trace(s, TRACE_FINISH, reason);
            pthread_mutex_unlock(&s->lock);
            return NULL;
        }
        /* Capture the sequence while checking predicates. An event after this
         * point either wakes the futex or changes its expected value. */
        expected = atomic_load_explicit(&s->sequence, memory_order_acquire);
        gate_check = r->gate_mode == GATE_CHECK && !r->gate_used;
        pthread_mutex_unlock(&s->lock);
        if (gate_check)
            pause_at_gate(s, GATE_CHECK);
        pthread_mutex_lock(&s->lock);
        r->stage = WAITING;
        r->waits++;
        r->last_budget_ms = budget.milliseconds;
        trace(s, TRACE_WAIT, budget.milliseconds);
        pthread_mutex_unlock(&s->lock);
        rc = emscripten_futex_wait(&s->sequence, expected,
                                  budget.milliseconds < 0 ? INFINITY : (double) budget.milliseconds);
        pthread_mutex_lock(&s->lock);
        trace(s, TRACE_RETURN, rc);
        if (rc == -ETIMEDOUT) r->timed_out++;
        else if (rc == -EWOULDBLOCK) r->mismatch++;
        else if (rc == -EINTR) r->interrupted++;
        else if (rc != 0) r->error = -rc;
        bool gate_timeout = rc == -ETIMEDOUT && r->gate_mode == GATE_TIMEOUT && !r->gate_used;
        pthread_mutex_unlock(&s->lock);
        if (gate_timeout)
            pause_at_gate(s, GATE_TIMEOUT);
        /* A timeout is a wake hint. Recheck current predicates and timer
         * generation instead of completing from a stale captured deadline. */
    }
}

EMSCRIPTEN_KEEPALIVE int
probe_wait_init(void)
{
    if (initialized) return EALREADY;
    for (unsigned i = 0; i < SLOTS; i++) {
        int rc = pthread_mutex_init(&slots[i].lock, NULL);
        if (rc) return rc;
    }
    initialized = true;
    return 0;
}

EMSCRIPTEN_KEEPALIVE int
probe_wait_start(unsigned index, uint32_t id, int own_ms, int timer_ms, int gate_mode)
{
    struct ProbeClock now;
    int rc;
    if (!initialized || index >= SLOTS || id == 0 || own_ms < -1 || timer_ms < -1 ||
        (gate_mode != 0 && gate_mode != GATE_START && gate_mode != GATE_CHECK && gate_mode != GATE_TIMEOUT))
        return EINVAL;
    rc = probe_clock_now(&now);
    if (rc) return rc;
    struct Slot *s = &slots[index];
    pthread_mutex_lock(&s->lock);
    if (s->in_use || id <= s->last_id || s->timer_generation == UINT32_MAX) {
        pthread_mutex_unlock(&s->lock);
        return EBUSY;
    }
    memset(&s->request, 0, sizeof(s->request));
    s->last_id = id;
    s->timer_generation++;
    s->request = (struct Request) {
        .id = id, .active = true, .gate_mode = gate_mode, .start_ns = now.monotonic_ns,
        .own_armed = own_ms >= 0, .own_ns = now.monotonic_ns + (uint64_t) (own_ms >= 0 ? own_ms : 0) * 1000000,
        .timer_armed = timer_ms >= 0, .timer_pg_us = now.pg_us + (int64_t) (timer_ms >= 0 ? timer_ms : 0) * 1000,
        .last_budget_ms = -1,
    };
    atomic_store_explicit(&s->gate, 0, memory_order_release);
    s->in_use = true;
    trace(s, TRACE_START, gate_mode);
    pthread_mutex_unlock(&s->lock);
    rc = pthread_create(&s->thread, NULL, wait_main, s);
    if (rc) {
        pthread_mutex_lock(&s->lock);
        s->in_use = s->request.active = false;
        s->request.error = rc;
        pthread_mutex_unlock(&s->lock);
    }
    return rc;
}

EMSCRIPTEN_KEEPALIVE int
probe_wait_action(unsigned index, uint32_t id, int action, uint32_t timer_generation, int value)
{
    struct ProbeClock now;
    bool change = action != POKE;
    int rc;
    if (index >= SLOTS || action < NOTIFY || action > POKE || (action == ARM && value < 0))
        return -EINVAL;
    struct Slot *s = &slots[index];
    pthread_mutex_lock(&s->lock);
    if (!s->in_use || !s->request.active || s->request.id != id ||
        (action == TIMER_EVENT && (!s->request.timer_armed || s->timer_generation != timer_generation))) {
        s->request.stale++;
        trace(s, TRACE_STALE, action);
        pthread_mutex_unlock(&s->lock);
        return -ESTALE;
    }
    if (action == ARM || action == DISARM) {
        rc = probe_clock_now(&now);
        if (rc || s->timer_generation == UINT32_MAX) {
            pthread_mutex_unlock(&s->lock);
            return -(rc ? rc : EOVERFLOW);
        }
        s->timer_generation++;
        s->request.timer_armed = action == ARM;
        s->request.timer_pg_us = now.pg_us + (int64_t) value * 1000;
    } else if (action == NOTIFY) {
        s->request.flags |= 1;
        s->request.payload = (uint32_t) value;
    } else if (action == CANCEL) s->request.flags |= 2;
    else if (action == CLOSE) s->request.flags |= 4;
    if (change)
        atomic_fetch_add_explicit(&s->sequence, 1, memory_order_release);
    trace(s, TRACE_ACTION, action);
    pthread_mutex_unlock(&s->lock);
    rc = emscripten_futex_wake(&s->sequence, INT_MAX);
    if (action == POKE && rc > 0) {
        pthread_mutex_lock(&s->lock);
        s->request.poke_woken += (unsigned) rc;
        pthread_mutex_unlock(&s->lock);
    }
    return rc;
}

EMSCRIPTEN_KEEPALIVE int
probe_wait_release_gate(unsigned index, uint32_t id)
{
    if (index >= SLOTS) return -EINVAL;
    struct Slot *s = &slots[index];
    pthread_mutex_lock(&s->lock);
    if (!s->request.active || s->request.id != id) {
        pthread_mutex_unlock(&s->lock);
        return -ESTALE;
    }
    atomic_store_explicit(&s->gate, 1, memory_order_release);
    pthread_mutex_unlock(&s->lock);
    return emscripten_futex_wake(&s->gate, INT_MAX);
}

EMSCRIPTEN_KEEPALIVE const char *
probe_wait_snapshot(unsigned index, int with_trace)
{
    if (index >= SLOTS) return "{\"error\":1}";
    struct Slot *s = &slots[index];
    struct ProbeClock now = {0};
    pthread_mutex_lock(&s->lock);
    struct Request *r = &s->request;
    probe_clock_now(&now);
    size_t used = (size_t) snprintf(report, sizeof(report),
        "{\"id\":%u,\"timer\":%u,\"sequence\":%u,\"stage\":%d,\"active\":%s,\"done\":%s,"
        "\"outcome\":%d,\"error\":%d,\"payload\":%u,\"waits\":%u,\"timedOut\":%u,"
        "\"mismatch\":%u,\"interrupted\":%u,\"completions\":%u,\"stale\":%u,\"pokeWoken\":%u,"
        "\"lastBudgetMs\":%" PRId64 ",\"timerArmed\":%s,\"timerPgUs\":\"%" PRId64 "\","
        "\"ownNs\":\"%" PRIu64 "\",\"startNs\":\"%" PRIu64 "\",\"entryNs\":\"%" PRIu64
        "\",\"finishNs\":\"%" PRIu64 "\",\"finishPgUs\":\"%" PRId64 "\",\"snapshotNs\":\"%" PRIu64
        "\",\"traceDropped\":%u,\"trace\":[",
        r->id, s->timer_generation, atomic_load_explicit(&s->sequence, memory_order_acquire),
        r->stage, r->active ? "true" : "false", r->done ? "true" : "false", r->outcome, r->error,
        r->payload, r->waits, r->timed_out, r->mismatch, r->interrupted, r->completions, r->stale,
        r->poke_woken, r->last_budget_ms, r->timer_armed ? "true" : "false", r->timer_pg_us,
        r->own_ns, r->start_ns, r->entry_ns, r->finish_ns, r->finish_pg_us, now.monotonic_ns, r->trace_dropped);
    if (with_trace) for (unsigned i = 0; i < r->trace_count; i++) {
        struct Trace *t = &r->trace[i];
        used += (size_t) snprintf(report + used, sizeof(report) - used,
            "%s{\"ns\":\"%" PRIu64 "\",\"kind\":%d,\"sequence\":%u,\"timer\":%u,\"value\":%" PRId64 "}",
            i ? "," : "", t->ns, t->kind, t->sequence, t->timer, t->value);
    }
    snprintf(report + used, sizeof(report) - used, "]}");
    pthread_mutex_unlock(&s->lock);
    return report;
}

EMSCRIPTEN_KEEPALIVE int
probe_wait_join(unsigned index, uint32_t id)
{
    if (index >= SLOTS) return EINVAL;
    struct Slot *s = &slots[index];
    pthread_mutex_lock(&s->lock);
    bool ready = s->in_use && s->request.id == id && s->request.done;
    pthread_mutex_unlock(&s->lock);
    if (!ready) return EBUSY;
    int rc = pthread_join(s->thread, NULL);
    if (rc) return rc;
    pthread_mutex_lock(&s->lock);
    s->in_use = false;
    s->request.stage = JOINED;
    trace(s, TRACE_JOIN, 0);
    pthread_mutex_unlock(&s->lock);
    return 0;
}
