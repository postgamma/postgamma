/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_fd_runtime_impl.h
 *    Thread-exit bridge for PostgreSQL's file-local descriptor cache.
 *
 * A process exit normally releases every remaining VFD and malloc allocation.
 * A backend thread must do that work explicitly before its role state is
 * destroyed.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_FD_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_FD_RUNTIME_IMPL_H

#define POSTGAMMA_FD_STATE(name) \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE( \
		"internal:src/backend/storage/file/fd.c:" \
		"src/backend/storage/file/fd.c::" #name, name)

void
postgamma_shutdown_file_access(bool preserve_temporary_files)
{
	Index		i;

	while (POSTGAMMA_FD_STATE(numAllocatedDescs) > 0)
		FreeDesc(&POSTGAMMA_FD_STATE(allocatedDescs)[0]);
	free(POSTGAMMA_FD_STATE(allocatedDescs));
	POSTGAMMA_FD_STATE(allocatedDescs) = NULL;
	POSTGAMMA_FD_STATE(maxAllocatedDescs) = 0;

	if (POSTGAMMA_FD_STATE(VfdCache) == NULL)
		return;
	for (i = 1; i < POSTGAMMA_FD_STATE(SizeVfdCache); i++)
	{
		if (POSTGAMMA_FD_STATE(VfdCache)[i].fileName == NULL)
			continue;
		POSTGAMMA_FD_STATE(VfdCache)[i].resowner = NULL;
		if (!preserve_temporary_files)
			FileClose(i);
		else
		{
			/*
			 * SIGQUIT and SIGKILL let the kernel close a process's file
			 * descriptors without running FD_DELETE_AT_CLOSE.  A role thread
			 * must close its host descriptors explicitly, but it must not
			 * unlink crash artifacts that PostgreSQL startup is responsible
			 * for handling according to remove_temp_files_after_crash.
			 */
			if (!FileIsNotOpen(i))
				LruDelete(i);
			FreeVfd(i);
		}
	}
	free(POSTGAMMA_FD_STATE(VfdCache));
	POSTGAMMA_FD_STATE(VfdCache) = NULL;
	POSTGAMMA_FD_STATE(SizeVfdCache) = 0;
}

#undef POSTGAMMA_FD_STATE

#endif /* POSTGAMMA_POSTGRES_FD_RUNTIME_IMPL_H */
