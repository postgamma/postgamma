/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include <emscripten.h>
#include <emscripten/stack.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static volatile unsigned sink;

static __attribute__((noinline)) unsigned
consume_stack(unsigned depth)
{
    volatile unsigned char frame[2048];
    unsigned value;
    for (unsigned i = 0; i < sizeof(frame); i++)
        frame[i] = (unsigned char) (depth + i);
    value = depth ? consume_stack(depth - 1) : 0;
    return value + frame[depth % sizeof(frame)] + frame[sizeof(frame) - 1];
}

static void
overflow(void)
{
    printf("stack-overflow: armed base=%" PRIuPTR " end=%" PRIuPTR "\n",
           emscripten_stack_get_base(), emscripten_stack_get_end());
    sink = consume_stack(128);
}

static void *
thread_main(void *argument)
{
    (void) argument;
    overflow();
    return NULL;
}

EMSCRIPTEN_KEEPALIVE int
probe_overflow(int in_thread)
{
    pthread_t thread;
    if (in_thread)
        return pthread_create(&thread, NULL, thread_main, NULL);
    overflow();
    return 0;
}
