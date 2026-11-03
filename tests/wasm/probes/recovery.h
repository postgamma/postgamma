/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#ifndef POSTGAMMA_RECOVERY_PROBE_H
#define POSTGAMMA_RECOVERY_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>

/* Only the callback layout and TLS roots are supplied by the probe. The
 * PG_TRY family is extracted unchanged from the target PostgreSQL header. */
typedef struct ErrorContextCallback {
    struct ErrorContextCallback *previous;
    void (*callback)(void *);
    void *arg;
} ErrorContextCallback;

extern _Thread_local sigjmp_buf *PG_exception_stack;
extern _Thread_local ErrorContextCallback *error_context_stack;
_Noreturn void pg_re_throw(void);
void recovery_check(bool condition, const char *expression, int line);
void recovery_measure_stack(void);
void recovery_set_error(int code);
_Noreturn void recovery_throw(int code);
_Noreturn void recovery_jump(sigjmp_buf env, int value);

#define CHECK(condition) recovery_check((condition), #condition, __LINE__)

#endif
