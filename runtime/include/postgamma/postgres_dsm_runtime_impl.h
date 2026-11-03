/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_POSTGRES_DSM_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_DSM_RUNTIME_IMPL_H
#include "postgamma/dsm_platform.h"
#include "postgamma/instance_runtime.h"

bool
dsm_impl_op(dsm_op op, dsm_handle handle, Size request_size,
    void **impl_private, void **mapped_address, Size *mapped_size, int elevel)
{
    PostgammaExecutionContext *execution = postgamma_execution_context_current();
    PostgammaInstanceRuntime *owner = execution == NULL ? NULL :
        postgamma_instance_context_runtime(execution->instance);
    PostgammaDsmMapping *mapping = *impl_private;
    int status = EINVAL;
    uint64 generation;
    if (owner == NULL)
        elog(FATAL, "dynamic shared memory requires an active postgamma instance");
    generation = postgamma_instance_runtime_generation(owner);
    if (POSTGAMMA_GUC_VALUE(dynamic_shared_memory_type) != DSM_IMPL_MMAP)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("WebAssembly requires dynamic_shared_memory_type=mmap")));
    switch (op)
    {
        case DSM_OP_CREATE:
            status = postgamma_platform_dsm_create(owner, generation, handle, request_size,
                &mapping, mapped_address, mapped_size);
            break;
        case DSM_OP_ATTACH:
            status = postgamma_platform_dsm_attach(owner, generation, handle,
                &mapping, mapped_address, mapped_size);
            break;
        case DSM_OP_DETACH:
        case DSM_OP_DESTROY:
            status = 0;
            if (op == DSM_OP_DESTROY)
                status = postgamma_platform_dsm_destroy(owner, generation, handle);
            if (mapping != NULL)
            {
                int detached = postgamma_platform_dsm_detach(&mapping);
                if (status == 0) status = detached;
            }
            *mapped_address = NULL;
            *mapped_size = 0;
            break;
    }
    *impl_private = mapping;
    if (status == 0) return true;
    if (op == DSM_OP_CREATE && status == EEXIST) return false;
    errno = status;
    ereport(elevel, (status == ENOMEM ? errcode(ERRCODE_OUT_OF_MEMORY) : errcode_for_file_access(),
        errmsg("dynamic shared memory operation %d failed for handle %u: %m", (int) op, handle)));
    return false;
}

void
dsm_impl_pin_segment(dsm_handle handle, void *impl_private, void **pm_handle)
{
    /* Like POSIX shm, segments survive zero mappings until DSM explicitly destroys them.
     * PostgreSQL's shared control-slot refcount implements pin/unpin; no OS handle is needed. */
    (void) handle;
    (void) impl_private;
    *pm_handle = NULL;
}

void
dsm_impl_unpin_segment(dsm_handle handle, void **impl_private)
{
    (void) handle;
    Assert(*impl_private == NULL);
}
#endif
