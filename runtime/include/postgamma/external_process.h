/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_EXTERNAL_PROCESS_H
#define POSTGAMMA_EXTERNAL_PROCESS_H
#include <stdio.h>
#include <sys/types.h>
#include "postgamma/instance_runtime.h"

typedef struct PostgammaExternalPipe PostgammaExternalPipe;
/* Session-owned children. runtime is borrowed until session destruction.
 * dispatch runs on the current carrier and may exit the session nonlocally;
 * cleanup must then reap children before the pipe records are released.
 */
typedef struct PostgammaExternalProcessState
{
    PostgammaInstanceRuntime *runtime;
    void (*dispatch)(void);
    pid_t system_child_pid;
    PostgammaExternalPipe *external_pipes;
} PostgammaExternalProcessState;

int postgamma_external_system(PostgammaExternalProcessState *state, const char *command);
FILE *postgamma_external_popen(PostgammaExternalProcessState *state, const char *command, const char *mode);
int postgamma_external_pclose(PostgammaExternalProcessState *state, FILE *stream);
void postgamma_cleanup_external_children(PostgammaExternalProcessState *state);
void postgamma_release_external_pipes(PostgammaExternalProcessState *state);
#endif
