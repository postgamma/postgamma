/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_FRONTEND_TOOL_RUNTIME_H
#define POSTGAMMA_PRIVATE_FRONTEND_TOOL_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

int postgamma_frontend_tool_execution_lock(void);
int postgamma_frontend_tool_execution_trylock(void);
int postgamma_frontend_tool_execution_unlock(void);
void postgamma_frontend_tool_getopt_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_PRIVATE_FRONTEND_TOOL_RUNTIME_H */
