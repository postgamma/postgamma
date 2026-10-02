/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_xlog_runtime_impl.h
 *    Thread-exit bridge for PostgreSQL's file-local WAL descriptor.
 *
 * A backend process relies on exit(2) to close openLogFile.  A backend role
 * thread must close it explicitly before its role-local state is destroyed.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_XLOG_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_XLOG_RUNTIME_IMPL_H

#define POSTGAMMA_XLOG_STATE(name) \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE( \
		"internal:src/backend/access/transam/xlog.c:" \
		"src/backend/access/transam/xlog.c::" #name, name)

void
postgamma_shutdown_xlog_file_access(void)
{
	if (POSTGAMMA_XLOG_STATE(openLogFile) >= 0)
	{
		if (close(POSTGAMMA_XLOG_STATE(openLogFile)) != 0)
			elog(LOG, "could not close role-local WAL file descriptor: %m");
		POSTGAMMA_XLOG_STATE(openLogFile) = -1;
		ReleaseExternalFD();
	}
}

#undef POSTGAMMA_XLOG_STATE

#endif /* POSTGAMMA_POSTGRES_XLOG_RUNTIME_IMPL_H */
