/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_elog_runtime_impl.h
 *    Thread-exit bridge for PostgreSQL's file-local syslog state.
 *
 * assign_syslog_ident() owns a malloc allocation outside PostgreSQL memory
 * contexts.  A role thread must close and release that process-lifetime
 * state explicitly.
 *
 * This implementation header is included exactly once, by elog.c in the
 * generated tree.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_ELOG_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_ELOG_RUNTIME_IMPL_H

#define POSTGAMMA_ELOG_STATE(name) \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE( \
		"internal:src/backend/utils/error/elog.c:" \
		"src/backend/utils/error/elog.c::" #name, name)

void
postgamma_shutdown_error_support(void)
{
#ifdef HAVE_SYSLOG
	if (POSTGAMMA_ELOG_STATE(openlog_done))
	{
		closelog();
		POSTGAMMA_ELOG_STATE(openlog_done) = false;
	}
	free(POSTGAMMA_ELOG_STATE(syslog_ident));
	POSTGAMMA_ELOG_STATE(syslog_ident) = NULL;
#endif
}

#undef POSTGAMMA_ELOG_STATE

#endif /* POSTGAMMA_POSTGRES_ELOG_RUNTIME_IMPL_H */
