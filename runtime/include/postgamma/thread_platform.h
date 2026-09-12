/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_THREAD_PLATFORM_H
#define POSTGAMMA_THREAD_PLATFORM_H

#include "postgamma/thread_runtime.h"

/* Target mechanisms used by the shared thread lifecycle implementation. */
bool postgamma_platform_thread_can_block(void);
int postgamma_platform_thread_stack_info(PostgammaThreadStackInfo *info);

#endif
