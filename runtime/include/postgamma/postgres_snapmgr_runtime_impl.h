/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_snapmgr_runtime_impl.h
 *    Thread-exit bridge for PostgreSQL's static snapshot storage.
 *
 * GetSnapshotData() deliberately allocates the XID arrays of three static
 * SnapshotData objects with malloc() and reuses them until process exit.  A
 * role thread must release those arrays before its virtualized state is
 * destroyed.
 *
 * This implementation header is included exactly once, by snapmgr.c in the
 * generated tree, so PostgreSQL's private snapshot objects remain private.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_SNAPMGR_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_SNAPMGR_RUNTIME_IMPL_H

#define POSTGAMMA_SNAPMGR_STATE(name) \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE( \
		"internal:src/backend/utils/time/snapmgr.c:" \
		"src/backend/utils/time/snapmgr.c::" #name, name)

void
postgamma_shutdown_snapshot_storage(void)
{
	SnapshotData *snapshots[] = {
		&POSTGAMMA_SNAPMGR_STATE(CurrentSnapshotData),
		&POSTGAMMA_SNAPMGR_STATE(SecondarySnapshotData),
		&POSTGAMMA_SNAPMGR_STATE(CatalogSnapshotData),
	};

	for (size_t index = 0; index < lengthof(snapshots); index++)
	{
		free(snapshots[index]->xip);
		free(snapshots[index]->subxip);
		snapshots[index]->xip = NULL;
		snapshots[index]->subxip = NULL;
		snapshots[index]->xcnt = 0;
		snapshots[index]->subxcnt = 0;
	}
}

#undef POSTGAMMA_SNAPMGR_STATE

#endif /* POSTGAMMA_POSTGRES_SNAPMGR_RUNTIME_IMPL_H */
