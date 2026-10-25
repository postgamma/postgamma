/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <pthread.h>

int postgamma_test_pthread_getattr_np(pthread_t thread, pthread_attr_t *attributes);

/* Redirect only the runtime call, leaving sanitizer stack discovery intact. */
#define pthread_getattr_np postgamma_test_pthread_getattr_np
#include "../../runtime/src/thread_runtime.c"
