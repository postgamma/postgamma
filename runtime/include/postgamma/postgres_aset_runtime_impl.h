/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_aset_runtime_impl.h
 *    Thread-exit bridge for PostgreSQL's file-local AllocSet freelists.
 *
 * AllocSetDelete() normally retains a bounded number of deleted contexts in
 * process-lifetime freelists.  A role thread has a shorter lifetime than the
 * host process, so those cached contexts must be released before its
 * virtualized role state is destroyed.
 *
 * This implementation header is included exactly once, by aset.c in the
 * generated tree.  Keeping the traversal beside PostgreSQL's private
 * AllocSetFreeList definition avoids exposing allocator internals through the
 * version-independent runtime.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_ASET_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_ASET_RUNTIME_IMPL_H

#define POSTGAMMA_ASET_STATE(name) \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE( \
		"internal:src/backend/utils/mmgr/aset.c:" \
		"src/backend/utils/mmgr/aset.c::" #name, name)

void
postgamma_shutdown_allocset_freelists(void)
{
	for (int index = 0;
		 index < lengthof(POSTGAMMA_ASET_STATE(context_freelists));
		 index++)
	{
		AllocSetFreeList *freelist =
			&POSTGAMMA_ASET_STATE(context_freelists)[index];

		while (freelist->first_free != NULL)
		{
			AllocSetContext *oldset = freelist->first_free;

			freelist->first_free =
				(AllocSetContext *) oldset->header.nextchild;
			freelist->num_free--;
			VALGRIND_DESTROY_MEMPOOL(oldset);
			free(oldset);
		}
		Assert(freelist->num_free == 0);
	}
}

#undef POSTGAMMA_ASET_STATE

#endif /* POSTGAMMA_POSTGRES_ASET_RUNTIME_IMPL_H */
