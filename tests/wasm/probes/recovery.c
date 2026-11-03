/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "recovery.h"
#include "recovery-pg-try.h"

#include <emscripten.h>
#include <emscripten/stack.h>
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 3
#define CASES 20
#define REPETITIONS 1000
enum Stage { IDLE, STARTING, WAITING, RECOVERED, DONE, JOINED };

struct Case {
    const char *name;
    unsigned checks, failures, first_line;
};

struct Session {
    unsigned id, round, count, callback_path, catches, cleanups, allocations;
    unsigned logical_mask, cleanup_attempts, repetitions;
    size_t heap_before, heap_after;
    int error;
    pthread_t owner;
    sem_t wake;
    pthread_mutex_t resource_lock;
    _Atomic unsigned stage;
    _Atomic uintptr_t boundary;
    void *resource;
    uintptr_t base, end, minimum_sp, tls_address, final_sp;
    struct Case cases[CASES];
};

_Thread_local sigjmp_buf *PG_exception_stack;
_Thread_local ErrorContextCallback *error_context_stack;
static _Thread_local struct Session *current;
static _Thread_local unsigned tls_marker;
static struct Session sessions[THREADS + 1];
static pthread_t threads[THREADS];
static bool initialized, active;
static char report[24576];

void
recovery_check(bool condition, const char *expression, int line)
{
    struct Case *test = &current->cases[current->count - 1];
    test->checks++;
    if (!condition) {
        if (test->failures++ == 0) {
            test->first_line = (unsigned) line;
            fprintf(stderr, "recovery: slot=%u case=%s line=%d failed: %s\n",
                    current->id, test->name, line, expression);
        }
    }
}

static void
begin(const char *name)
{
    if (current->count == CASES) abort();
    current->cases[current->count++].name = name;
}

void
recovery_measure_stack(void)
{
    uintptr_t sp = emscripten_stack_get_current();
    CHECK(sp >= current->end && sp < current->base);
    if (sp < current->minimum_sp) current->minimum_sp = sp;
}

void
recovery_set_error(int code)
{
    CHECK(pthread_equal(current->owner, pthread_self()));
    CHECK(tls_marker == 100 * current->round + current->id + 1);
    current->error = code;
}

_Noreturn void
pg_re_throw(void)
{
    uintptr_t target = (uintptr_t) PG_exception_stack;
    bool owned = current && pthread_equal(current->owner, pthread_self()) &&
                 target >= current->end && target + sizeof(sigjmp_buf) <= current->base;
    CHECK(owned);
    if (!owned) abort();
    recovery_jump(*PG_exception_stack, 1);
}

static void
callback(void *arg)
{
    unsigned tag = (unsigned) (uintptr_t) arg;
    CHECK(tag == 1 || tag == 2);
    current->callback_path = 10 * current->callback_path + tag;
}

static void
acquire_resource(void)
{
    CHECK(current->resource == NULL);
    CHECK(pthread_mutex_lock(&current->resource_lock) == 0);
    current->resource = malloc(64);
    CHECK(current->resource != NULL);
    if (!current->resource) abort();
    memset(current->resource, (int) current->id + 1, 64);
    current->allocations++;
}

static void
release_resource(void)
{
    CHECK(current->resource != NULL);
    if (!current->resource) abort();
    CHECK(((unsigned char *) current->resource)[63] == current->id + 1);
    free(current->resource);
    current->resource = NULL;
    current->cleanups++;
    CHECK(pthread_mutex_unlock(&current->resource_lock) == 0);
}

static void
plain_jump(int value)
{
    sigjmp_buf env;
    volatile int marker = 3;
    void (*volatile jump)(sigjmp_buf, int) = recovery_jump;
    begin(value == 0 ? "longjmp-zero-becomes-one" : value > 0 ? "longjmp-positive" : "longjmp-negative");
    switch (setjmp(env)) {
        case 0: marker = 9; jump(env, value); abort();
        case 1: CHECK(value == 0); break;
        case 17: CHECK(value == 17); break;
        case -11: CHECK(value == -11); break;
        default: CHECK(false);
    }
    CHECK(marker == 9);
}

static void
signal_mask(int save)
{
    sigjmp_buf env;
    sigset_t original, first, changed, empty, seen;
    begin(save ? "sigsetjmp-one-does-not-save-sdk-mask" : "sigsetjmp-zero-leaves-sdk-mask");
    sigemptyset(&first); sigaddset(&first, SIGUSR1);
    sigemptyset(&changed); sigaddset(&changed, SIGUSR2);
    sigemptyset(&empty);
    CHECK(sigprocmask(SIG_SETMASK, &first, &original) == 0);
    if (sigsetjmp(env, save) == 0) {
        CHECK(sigprocmask(SIG_SETMASK, &changed, NULL) == 0);
        recovery_jump(env, 1);
    }
    /* The SDK aliases sigsetjmp to setjmp. Query with an empty SIG_BLOCK set
     * because this SDK's pthread_sigmask does not guard a NULL set pointer. */
    CHECK(sigprocmask(SIG_BLOCK, &empty, &seen) == 0);
    CHECK(sigismember(&seen, SIGUSR1) == 0 && sigismember(&seen, SIGUSR2) == 1);
    CHECK(sigprocmask(SIG_SETMASK, &original, NULL) == 0);
}

static void
finally_case(unsigned mode)
{
    ErrorContextCallback outer = {NULL, callback, (void *) 1};
    ErrorContextCallback inner = {&outer, callback, (void *) 2};
    void (*volatile raise_error)(int) = recovery_throw;
    unsigned cleaned = current->cleanups, caught = current->catches;
    begin(mode == 0 ? "finally-success" : mode == 1 ? "finally-rethrow" : "error-in-finally");
    current->callback_path = 0;
    error_context_stack = &outer;
    PG_TRY(outer);
    {
        PG_TRY(inner);
        {
            acquire_resource();
            error_context_stack = &inner;
            if (mode) raise_error(100 + (int) current->id);
        }
        PG_FINALLY(inner);
        {
            CHECK(error_context_stack == &outer);
            release_resource();
            if (mode == 2) raise_error(200 + (int) current->id);
        }
        PG_END_TRY(inner);
        CHECK(mode == 0);
    }
    PG_CATCH(outer);
    {
        CHECK(mode != 0 && PG_exception_stack == NULL && error_context_stack == &outer);
        CHECK(current->error == (mode == 2 ? 200 : 100) + (int) current->id);
        current->catches++;
    }
    PG_END_TRY(outer);
    CHECK(current->cleanups == cleaned + 1 && current->catches == caught + (mode != 0));
    CHECK(current->callback_path == (mode == 0 ? 0 : mode == 1 ? 21 : 211));
    CHECK(PG_exception_stack == NULL && error_context_stack == &outer);
    error_context_stack = NULL;
}

static void
catch_case(bool replace_error)
{
    ErrorContextCallback outer = {NULL, callback, (void *) 1};
    ErrorContextCallback inner = {&outer, callback, (void *) 2};
    unsigned cleaned = current->cleanups, caught = current->catches;
    begin(replace_error ? "new-error-in-catch" : "nested-catch-rethrow");
    error_context_stack = &outer;
    current->callback_path = 0;
    PG_TRY(outer);
    {
        PG_TRY(inner);
        {
            acquire_resource();
            error_context_stack = &inner;
            recovery_throw(300 + (int) current->id);
        }
        PG_CATCH(inner);
        {
            CHECK(PG_exception_stack != NULL && error_context_stack == &outer);
            current->catches++;
            release_resource();
            if (replace_error) recovery_throw(400 + (int) current->id);
            PG_RE_THROW();
        }
        PG_END_TRY(inner);
        CHECK(false);
    }
    PG_CATCH(outer);
    {
        CHECK(PG_exception_stack == NULL && error_context_stack == &outer);
        CHECK(current->error == (replace_error ? 400 : 300) + (int) current->id);
        current->catches++;
    }
    PG_END_TRY(outer);
    CHECK(current->cleanups == cleaned + 1 && current->catches == caught + 2);
    CHECK(current->callback_path == (replace_error ? 211 : 21));
    error_context_stack = NULL;
}

static void
top_level_cleanup(void)
{
    sigjmp_buf env;
    ErrorContextCallback context = {NULL, callback, (void *) 1};
    begin("error-during-top-level-cleanup");
    current->cleanup_attempts = 0;
    current->callback_path = 0;
    if (sigsetjmp(env, 1) != 0) {
        error_context_stack = NULL;
        CHECK(PG_exception_stack == &env);
        current->cleanup_attempts++;
        if (current->cleanup_attempts == 1) {
            CHECK(current->error == 501);
            release_resource();
            recovery_throw(502);
        }
        CHECK(current->cleanup_attempts == 2 && current->error == 502);
        CHECK(current->resource == NULL && current->callback_path == 1);
    } else {
        PG_exception_stack = &env;
        error_context_stack = &context;
        acquire_resource();
        recovery_throw(501);
    }
    PG_exception_stack = NULL;
    CHECK(error_context_stack == NULL);
}

static void
logical_mask(void)
{
    unsigned saved = current->logical_mask;
    begin("explicit-logical-mask-restoration");
    PG_TRY();
    {
        current->logical_mask = 0xff;
        recovery_throw(601);
    }
    PG_CATCH();
    {
        CHECK(current->logical_mask == 0xff);
        current->logical_mask = saved;
    }
    PG_END_TRY();
    CHECK(current->logical_mask == saved);
}

static void
repeat_errors(void)
{
    uintptr_t sp = emscripten_stack_get_current();
    unsigned cleaned = current->cleanups, caught = current->catches;
    begin("repeated-errors-release-resources-and-stack");
    current->heap_before = mallinfo().uordblks;
    for (unsigned i = 0; i < REPETITIONS; i++) {
        PG_TRY();
        {
            acquire_resource();
            recovery_throw(700 + (int) current->id);
        }
        PG_CATCH();
        {
            CHECK(current->error == 700 + (int) current->id);
            CHECK(PG_exception_stack == NULL && error_context_stack == NULL);
            release_resource();
            current->catches++;
        }
        PG_END_TRY();
        current->repetitions++;
        CHECK(emscripten_stack_get_current() == sp);
        CHECK(current->resource == NULL);
    }
    CHECK(current->cleanups == cleaned + REPETITIONS && current->catches == caught + REPETITIONS);
    current->heap_after = mallinfo().uordblks;
    /* Only the control run is quiescent; concurrent threads share the heap. */
    if (current->id == THREADS) CHECK(current->heap_after == current->heap_before);
    current->final_sp = emscripten_stack_get_current();
}

static void
suite(void)
{
    plain_jump(0); plain_jump(17); plain_jump(-11);
    signal_mask(0); signal_mask(1);
    finally_case(0); finally_case(1); finally_case(2);
    catch_case(false); catch_case(true);
    top_level_cleanup(); logical_mask(); repeat_errors();
    begin("reuse-after-recovery");
    CHECK(current->resource == NULL && current->allocations == current->cleanups);
    CHECK(current->logical_mask == 0 && PG_exception_stack == NULL && error_context_stack == NULL);
    acquire_resource();
    CHECK(((unsigned char *) current->resource)[0] == current->id + 1);
    release_resource();
}

static void
bind(struct Session *session)
{
    current = session;
    current->owner = pthread_self();
    current->base = emscripten_stack_get_base();
    current->end = emscripten_stack_get_end();
    current->minimum_sp = current->base;
    current->tls_address = (uintptr_t) &PG_exception_stack;
    begin("fresh-thread-binding");
    CHECK(PG_exception_stack == NULL && error_context_stack == NULL && tls_marker == 0);
    CHECK(current->base > current->end);
    tls_marker = 100 * current->round + current->id + 1;
}

static void *
thread_main(void *arg)
{
    ErrorContextCallback context = {NULL, callback, (void *) 1};
    sigset_t mask, saved_mask, empty, observed;
    bind(arg);
    begin("concurrent-owning-thread-recovery");
    sigemptyset(&mask); sigemptyset(&empty);
    sigaddset(&mask, current->id == 0 ? SIGUSR1 : current->id == 1 ? SIGUSR2 : SIGTERM);
    CHECK(pthread_sigmask(SIG_SETMASK, &mask, &saved_mask) == 0);
    PG_TRY();
    {
        error_context_stack = &context;
        acquire_resource();
        atomic_store(&current->boundary, (uintptr_t) PG_exception_stack);
        atomic_store_explicit(&current->stage, WAITING, memory_order_release);
        CHECK(sem_wait(&current->wake) == 0);
        recovery_throw(800 + (int) current->id);
    }
    PG_CATCH();
    {
        CHECK(current->error == 800 + (int) current->id);
        CHECK(PG_exception_stack == NULL && error_context_stack == NULL);
        CHECK(current->callback_path == 1);
        CHECK(pthread_sigmask(SIG_BLOCK, &empty, &observed) == 0);
        CHECK(sigismember(&observed, SIGUSR1) == (current->id == 0));
        CHECK(sigismember(&observed, SIGUSR2) == (current->id == 1));
        CHECK(sigismember(&observed, SIGTERM) == (current->id == 2));
        release_resource();
        current->catches++;
    }
    PG_END_TRY();
    CHECK(pthread_sigmask(SIG_SETMASK, &saved_mask, NULL) == 0);
    atomic_store(&current->boundary, 0);
    atomic_store_explicit(&current->stage, RECOVERED, memory_order_release);
    CHECK(sem_wait(&current->wake) == 0);
    suite();
    atomic_store_explicit(&current->stage, DONE, memory_order_release);
    return NULL;
}

static void
reset_session(struct Session *s, unsigned id, unsigned round)
{
    memset(s, 0, sizeof(*s));
    s->id = id; s->round = round;
    atomic_init(&s->stage, STARTING);
    atomic_init(&s->boundary, 0);
    if (sem_init(&s->wake, 0, 0) || pthread_mutex_init(&s->resource_lock, NULL)) abort();
}

EMSCRIPTEN_KEEPALIVE int
probe_recovery_start(unsigned round)
{
    if (active || round == 0) return EINVAL;
    active = true;
    for (unsigned i = 0; i < THREADS; i++) {
        reset_session(&sessions[i], i, round);
        int rc = pthread_create(&threads[i], NULL, thread_main, &sessions[i]);
        if (rc) return rc;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE unsigned
probe_recovery_state(unsigned id)
{
    return id < THREADS ? atomic_load_explicit(&sessions[id].stage, memory_order_acquire) : IDLE;
}

EMSCRIPTEN_KEEPALIVE int
probe_recovery_foreign(unsigned id)
{
    if (id >= THREADS || probe_recovery_state(id) != WAITING || !atomic_load(&sessions[id].boundary))
        return EINVAL;
    /* Reject a foreign carrier before touching its live jump buffer. Actual
     * cross-thread longjmp would be undefined behavior and is never attempted. */
    if (!pthread_equal(sessions[id].owner, pthread_self())) return EPERM;
    return 0;
}

EMSCRIPTEN_KEEPALIVE int
probe_recovery_release(unsigned id)
{
    if (id >= THREADS || (probe_recovery_state(id) != WAITING && probe_recovery_state(id) != RECOVERED))
        return EINVAL;
    return sem_post(&sessions[id].wake);
}

EMSCRIPTEN_KEEPALIVE const char *
probe_recovery_report(unsigned id)
{
    struct Session *s = &sessions[id <= THREADS ? id : THREADS];
    size_t used = 0;
    unsigned failures = 0, checks = 0;
    if (id < THREADS && probe_recovery_state(id) != DONE && probe_recovery_state(id) != JOINED)
        return "{\"error\":\"thread is still running\"}";
    for (unsigned i = 0; i < s->count; i++) {
        failures += s->cases[i].failures;
        checks += s->cases[i].checks;
    }
#define APPEND(...) do { \
    int n = snprintf(report + used, sizeof(report) - used, __VA_ARGS__); \
    if (n < 0 || (size_t) n >= sizeof(report) - used) abort(); \
    used += (size_t) n; \
} while (0)
    APPEND("{\"slot\":%u,\"round\":%u,\"checks\":%u,\"failures\":%u,\"cases\":[",
           s->id, s->round, checks, failures);
    for (unsigned i = 0; i < s->count; i++) {
        struct Case *c = &s->cases[i];
        APPEND("%s{\"name\":\"%s\",\"checks\":%u,\"failures\":%u,\"firstLine\":%u}",
               i ? "," : "", c->name, c->checks, c->failures, c->first_line);
    }
    APPEND("],\"allocations\":%u,\"cleanups\":%u,\"catches\":%u,\"repetitions\":%u,"
           "\"heapBefore\":%zu,\"heapAfter\":%zu,"
           "\"stack\":{\"base\":%" PRIuPTR ",\"end\":%" PRIuPTR ",\"minimumSP\":%" PRIuPTR
           ",\"finalSP\":%" PRIuPTR "},\"tlsAddress\":%" PRIuPTR ",\"foreignErrno\":%d,\"nativeExceptions\":%s}",
           s->allocations, s->cleanups, s->catches, s->repetitions,
           s->heap_before, s->heap_after,
           s->base, s->end, s->minimum_sp, s->final_sp, s->tls_address, EPERM,
#ifdef __wasm_exception_handling__
           "true"
#else
           "false"
#endif
    );
#undef APPEND
    return report;
}

EMSCRIPTEN_KEEPALIVE const char *
probe_recovery_sync(void)
{
    if (active) return "{\"error\":\"threads active\"}";
    if (initialized) {
        sem_destroy(&sessions[THREADS].wake);
        pthread_mutex_destroy(&sessions[THREADS].resource_lock);
    }
    reset_session(&sessions[THREADS], THREADS, 1);
    tls_marker = 0;
    bind(&sessions[THREADS]);
    suite();
    initialized = true;
    return probe_recovery_report(THREADS);
}

EMSCRIPTEN_KEEPALIVE int
probe_recovery_join(void)
{
    for (unsigned i = 0; i < THREADS; i++) if (probe_recovery_state(i) != DONE) return EBUSY;
    for (unsigned i = 0; i < THREADS; i++) {
        int rc = pthread_join(threads[i], NULL);
        if (rc) return rc;
        if (sem_destroy(&sessions[i].wake) || pthread_mutex_destroy(&sessions[i].resource_lock)) abort();
        atomic_store(&sessions[i].stage, JOINED);
    }
    active = false;
    return 0;
}

EMSCRIPTEN_KEEPALIVE void
probe_recovery_trap(void)
{
    PG_TRY();
    {
        puts("recovery: trap armed inside PG_TRY");
        __builtin_trap();
    }
    PG_CATCH();
    {
        puts("recovery: trap incorrectly caught as ERROR");
    }
    PG_END_TRY();
}
