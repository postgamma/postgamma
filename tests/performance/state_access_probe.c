/* Test-only Callgrind phase control in the persistent benchmark backend. */
#include "postgres.h"
#include "fmgr.h"
#include <valgrind/callgrind.h>

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(postgamma_state_profile);

Datum
postgamma_state_profile(PG_FUNCTION_ARGS)
{
	switch (PG_GETARG_INT32(0))
	{
		case 0:
			CALLGRIND_START_INSTRUMENTATION;
			break;
		case 1:
			CALLGRIND_ZERO_STATS;
			break;
		case 2:
			CALLGRIND_DUMP_STATS_AT("measured");
			CALLGRIND_STOP_INSTRUMENTATION;
			break;
		default:
			elog(ERROR, "unknown state profiling phase");
	}
	PG_RETURN_VOID();
}
