/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only GUC check hook: postmaster settings must remain valid throughout
 * parallel worker GUC restoration, not just before and after it.
 */
#include "postgres.h"

#include <limits.h>

#include "fmgr.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);

static bool
check_expected_buffers(int *newval, void **extra, GucSource source)
{
	const char *actual = GetConfigOption("shared_buffers", false, false);

	if (*newval != atoi(actual))
	{
		GUC_check_errdetail("shared_buffers changed during GUC restore: expected %d, got %s",
						   *newval, actual);
		return false;
	}
	return true;
}

void
_PG_init(void)
{
	MemoryContext context;
	int		   *value;
	int			buffers = atoi(GetConfigOption("shared_buffers", false, false));
	char		setting[32];

	/* No process-global or carrier-local mutable state in this test module. */
#ifdef POSTGAMMA_BACKEND_STATE_GLOBAL_TopMemoryContext
	context = POSTGAMMA_BACKEND_STATE_GLOBAL_TopMemoryContext;
#else
	context = TopMemoryContext;
#endif
	value = MemoryContextAllocZero(context, sizeof(*value));
	DefineCustomIntVariable("postgamma_guc_restore.expected_buffers",
							"Check shared_buffers during parallel GUC restoration.",
							NULL, value, buffers, 1, INT_MAX, PGC_USERSET, 0,
							check_expected_buffers, NULL, NULL);

	/*
	 * RestoreLibraryState loads us before RestoreGUCState in each worker.
	 * Make this GUC non-default there so the reset pass calls our check hook
	 * after the pre-existing postmaster GUCs.  Its boot value records the real
	 * buffer pool size.  This detects even a temporary reset without relying
	 * on concurrent eviction, a cache miss, or an out-of-bounds access.
	 */
	snprintf(setting, sizeof(setting), "%d", buffers);
	SetConfigOption("postgamma_guc_restore.expected_buffers", setting,
					PGC_USERSET, PGC_S_DYNAMIC_DEFAULT);
}
