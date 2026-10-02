/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * extension_runtime.h
 *    PostgreSQL-facing lifecycle runtime for bundled native extensions.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_EXTENSION_RUNTIME_H
#define POSTGAMMA_EXTENSION_RUNTIME_H

#include "postgamma/guc_runtime.h"
#include "postgamma/postgamma_extension.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int postgamma_extension_execution_destroy(
	PostgammaExecutionContext *execution);
int postgamma_extension_session_reset(
	PostgammaExecutionContext *execution);
void postgamma_extension_reset_current_session(void);
int postgamma_extension_instance_shutdown(
	PostgammaExecutionContext *execution);

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_EXTENSION_RUNTIME_H */
