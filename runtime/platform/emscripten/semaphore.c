/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/semaphore_platform.h"
#include "postgamma/io_platform.h"
#include "postgamma/thread_runtime.h"
#include <errno.h>

int
postgamma_platform_semaphore_init(sem_t *sem, unsigned value)
{
    /* Shared linear memory contains threads of one process, not OS processes. */
    int status = sem_init(sem, 0, value);
    return status < 0 ? errno : status;
}

int
postgamma_platform_semaphore_post(sem_t *sem)
{
    int status = sem_post(sem);
    if (status == 0) postgamma_platform_notify();
    return status;
}

int
postgamma_platform_semaphore_wait(sem_t *sem, long timeout_ms)
{
    uint64_t start = postgamma_monotonic_now_ns();
    uint64_t duration = timeout_ms < 0 ? 0 : (uint64_t) timeout_ms * UINT64_C(1000000);
    for (;;) {
        uint32_t sequence = postgamma_platform_notification_sequence();
        if (sem_trywait(sem) == 0) return 0;
        if (errno != EAGAIN && errno != EINTR) return -1;
        uint64_t elapsed = postgamma_monotonic_now_ns() - start;
        if (timeout_ms >= 0 && elapsed >= duration) { errno = ETIMEDOUT; return -1; }
        double remaining = timeout_ms < 0 ? __builtin_inf() : (double) (duration - elapsed) / 1000000;
        if (postgamma_platform_notification_wait(sequence, remaining) < 0) return -1;
    }
}
