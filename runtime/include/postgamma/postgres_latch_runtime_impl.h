/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_latch_runtime_impl.h
 *    Thread-exit bridge for PostgreSQL's file-local latch wait set.
 *
 * This implementation header is included exactly once, by latch.c in the
 * generated tree.  Identifier lookup keeps handwritten cleanup code free of
 * generated slot numbers while preserving access to file-local state.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_LATCH_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_LATCH_RUNTIME_IMPL_H

#define POSTGAMMA_LATCH_WAIT_SET \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE( \
		"internal:src/backend/storage/ipc/latch.c:" \
		"src/backend/storage/ipc/latch.c::LatchWaitSet", LatchWaitSet)

void
postgamma_shutdown_latch_wait_set(void)
{
	if (POSTGAMMA_LATCH_WAIT_SET == NULL)
		return;
	FreeWaitEventSet(POSTGAMMA_LATCH_WAIT_SET);
	POSTGAMMA_LATCH_WAIT_SET = NULL;
}

#undef POSTGAMMA_LATCH_WAIT_SET

#endif /* POSTGAMMA_POSTGRES_LATCH_RUNTIME_IMPL_H */
