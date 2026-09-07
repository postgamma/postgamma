/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_checkpoint_runtime.h
 *    PostgreSQL-local bridge for nonblocking embedded checkpoints.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_CHECKPOINT_RUNTIME_H
#define POSTGAMMA_POSTGRES_CHECKPOINT_RUNTIME_H

#include "postgamma/embedded_kernel.h"


int postgamma_postgres_checkpoint_start(
	PostgammaKernelCheckpointRequest *request);
void postgamma_postgres_checkpoint_observe(void);

#endif /* POSTGAMMA_POSTGRES_CHECKPOINT_RUNTIME_H */
