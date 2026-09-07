/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_checkpoint_runtime_impl.h
 *    Checkpointer translation-unit implementation for embedded management.
 *
 * This header is included once, immediately after CheckpointerShmem is
 * declared in checkpointer.c.  It deliberately reuses PostgreSQL's own
 * checkpoint counters instead of copying the checkpoint algorithm.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_CHECKPOINT_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_CHECKPOINT_RUNTIME_IMPL_H

#include "postgamma/instance_runtime.h"
#include "postgamma/postgres_checkpoint_runtime.h"


#define POSTGAMMA_CHECKPOINTER_SHMEM() \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE( \
		"internal:src/backend/postmaster/checkpointer.c:" \
		"src/backend/postmaster/checkpointer.c::CheckpointerShmem", \
		CheckpointerShmem)


static PostgammaInstanceRuntime *
postgamma_checkpoint_current_instance_runtime(void)
{
	PostgammaExecutionContext *execution = postgamma_execution_context_current();

	return execution == NULL ? NULL :
		postgamma_instance_context_runtime(execution->instance);
}


int
postgamma_postgres_checkpoint_start(
	PostgammaKernelCheckpointRequest *request)
{
	PostgammaInstanceRuntime *runtime;
	CheckpointerShmemStruct *checkpointer_shmem;
	int			checkpoint_flags = 0;
	int32_t		baseline_started;
	int32_t		baseline_failed;
	int			status;

	if (request == NULL || request->struct_size != sizeof(*request) ||
		request->abi_version != POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION ||
		request->generation == 0 || request->tracker == NULL ||
		request->dispatched != 0 ||
		(request->flags & ~POSTGAMMA_KERNEL_CHECKPOINT_ALL) != 0)
		return EINVAL;
	runtime = postgamma_checkpoint_current_instance_runtime();
	if (runtime == NULL)
		return ENOENT;
	if (postgamma_instance_runtime_generation(runtime) != request->generation)
		return ESTALE;
	checkpointer_shmem = POSTGAMMA_CHECKPOINTER_SHMEM();
	if (checkpointer_shmem == NULL)
		return ENXIO;
	if ((request->flags & POSTGAMMA_KERNEL_CHECKPOINT_FAST) != 0)
		checkpoint_flags |= CHECKPOINT_FAST;
	if ((request->flags & POSTGAMMA_KERNEL_CHECKPOINT_FORCE) != 0)
		checkpoint_flags |= CHECKPOINT_FORCE;

	/* Arm the tracker and publish the request under the same PG spinlock. */
	SpinLockAcquire(&checkpointer_shmem->ckpt_lck);
	baseline_started = (int32_t) checkpointer_shmem->ckpt_started;
	baseline_failed = (int32_t) checkpointer_shmem->ckpt_failed;
	status = postgamma_instance_checkpoint_tracker_arm(
		(PostgammaCheckpointTracker *) request->tracker,
		request->generation, baseline_started, baseline_failed);
	if (status == 0)
		checkpointer_shmem->ckpt_flags |=
			(checkpoint_flags | CHECKPOINT_REQUESTED);
	SpinLockRelease(&checkpointer_shmem->ckpt_lck);
	if (status != 0)
		return status;

	/*
	 * Reuse PostgreSQL's nonblocking notification path.  Besides preserving
	 * upstream checkpointer-startup semantics, this keeps every role-local
	 * access in transformed PostgreSQL code instead of referring to an
	 * untransformed global from this support header.  The request bits were
	 * already published above, so OR-ing them a second time is harmless.
	 */
	RequestCheckpoint(checkpoint_flags | CHECKPOINT_REQUESTED);
	request->dispatched = UINT32_C(1);
	return 0;
}


void
postgamma_postgres_checkpoint_observe(void)
{
	PostgammaInstanceRuntime *runtime =
		postgamma_checkpoint_current_instance_runtime();
	CheckpointerShmemStruct *checkpointer_shmem;
	int32_t		checkpoint_done;
	int32_t		checkpoint_failed;
	int			status;

	if (runtime == NULL)
		return;
	checkpointer_shmem = POSTGAMMA_CHECKPOINTER_SHMEM();
	if (checkpointer_shmem == NULL)
		return;
	SpinLockAcquire(&checkpointer_shmem->ckpt_lck);
	checkpoint_done = (int32_t) checkpointer_shmem->ckpt_done;
	checkpoint_failed = (int32_t) checkpointer_shmem->ckpt_failed;
	SpinLockRelease(&checkpointer_shmem->ckpt_lck);
	status = postgamma_instance_checkpoint_observe(
		runtime, postgamma_instance_runtime_generation(runtime),
		checkpoint_done, checkpoint_failed);
	if (status != 0)
		elog(WARNING,
			 "could not publish embedded checkpoint completion: %s",
			 strerror(status));
}

#undef POSTGAMMA_CHECKPOINTER_SHMEM

#endif /* POSTGAMMA_POSTGRES_CHECKPOINT_RUNTIME_IMPL_H */
