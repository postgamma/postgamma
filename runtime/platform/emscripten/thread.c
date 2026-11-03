/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/thread_platform.h"

#include <emscripten/stack.h>
#include <emscripten/threading.h>
#include <errno.h>

bool
postgamma_platform_thread_can_block(void)
{
    /* The module's control Worker must keep servicing its JavaScript queue. */
    return !emscripten_is_main_runtime_thread();
}

int
postgamma_platform_thread_stack_info(PostgammaThreadStackInfo *info)
{
    uintptr_t low = emscripten_stack_get_end();
    uintptr_t high = emscripten_stack_get_base();
    uintptr_t current = emscripten_stack_get_current();
    size_t size;

    if (info == NULL)
        return EINVAL;
    if (info->configured_guard_size != 0)
        return ENOTSUP;
    /* An optimized leaf may use no linear-memory stack: SP can equal base. */
    if (high <= low || current < low || current > high)
        return ERANGE;
    size = high - low;
    if (size < info->configured_stack_size ||
        size < POSTGAMMA_THREAD_STACK_MINIMUM_USABLE)
        return ERANGE;
    info->stack_low_address = low;
    info->stack_high_address = high;
    info->native_stack_size = size;
    info->native_guard_size = 0;
    info->usable_stack_size = size;
    return 0;
}
