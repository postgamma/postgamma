/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

struct probe_state
{
    pthread_t parent;
    int value;
};

static void *
probe_thread(void *argument)
{
    struct probe_state *state = argument;

    if (pthread_equal(state->parent, pthread_self()))
        state->value = -1;
    else
        state->value *= 2;
    return NULL;
}

EMSCRIPTEN_KEEPALIVE int
postgamma_probe(int value)
{
    struct probe_state state = {pthread_self(), value};
    pthread_t thread;

    if (emscripten_is_main_browser_thread())
        return -2;
    if (pthread_create(&thread, NULL, probe_thread, &state) != 0)
        return -3;
    if (pthread_join(thread, NULL) != 0)
        return -4;
    return state.value;
}
