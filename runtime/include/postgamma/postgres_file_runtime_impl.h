/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_POSTGRES_FILE_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_FILE_RUNTIME_IMPL_H
#ifdef __EMSCRIPTEN__
#include "postgamma/file_platform.h"
#include "postgamma/guc_runtime.h"
#include "postgamma/path_runtime.h"

static inline bool
postgamma_postgres_directory_owned(void)
{
    PostgammaExecutionContext *execution = postgamma_execution_context_current();
    PostgammaPathRuntimeTelemetry telemetry;
    if (execution == NULL || execution->instance == NULL ||
        postgamma_path_runtime_telemetry(execution->instance->path_runtime, &telemetry) != 0)
        return false;
    return postgamma_platform_directory_owned(telemetry.data_directory_fd, telemetry.generation);
}

static inline void
postgamma_postgres_directory_require_owner(void)
{
    if (!postgamma_postgres_directory_owned())
        elog(FATAL, "data directory requires an active postgamma instance claim");
}
#endif
#endif
