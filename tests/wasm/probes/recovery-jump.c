/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "recovery.h"

#include <emscripten.h>

/* Keep a distinct translation unit and real frames between save and restore. */
__attribute__((noinline)) static _Noreturn void
descend(sigjmp_buf env, int value, unsigned depth)
{
    volatile unsigned char scratch[512];
    for (unsigned i = 0; i < sizeof(scratch); i++) scratch[i] = (unsigned char) depth;
    CHECK(scratch[17] == depth);
    recovery_measure_stack();
    if (depth) descend(env, value, depth - 1);
    siglongjmp(env, value);
}

_Noreturn void
recovery_jump(sigjmp_buf env, int value)
{
    descend(env, value, 8);
}

_Noreturn void
recovery_throw(int code)
{
    recovery_set_error(code);
    for (ErrorContextCallback *context = error_context_stack; context; context = context->previous)
        context->callback(context->arg);
    pg_re_throw();
}
