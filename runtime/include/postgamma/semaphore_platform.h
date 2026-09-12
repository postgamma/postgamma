/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_SEMAPHORE_PLATFORM_H
#define POSTGAMMA_SEMAPHORE_PLATFORM_H
#include <semaphore.h>
int postgamma_platform_semaphore_init(sem_t *sem, unsigned value);
int postgamma_platform_semaphore_post(sem_t *sem);
int postgamma_platform_semaphore_wait(sem_t *sem, long timeout_ms);
#endif
