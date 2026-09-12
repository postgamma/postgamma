/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_POSTGRES_SHMEM_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_SHMEM_RUNTIME_IMPL_H

#include "postgamma/instance_runtime.h"
#include "postgamma/shmem_platform.h"

#define PGM_SHMEM_STATE(name) POSTGAMMA_BACKEND_STATE_GLOBAL(name)

static PostgammaInstanceRuntime *
postgamma_shmem_owner(void)
{
    PostgammaExecutionContext *execution = postgamma_execution_context_current();
    PostgammaInstanceRuntime *runtime = execution == NULL ? NULL :
        postgamma_instance_context_runtime(execution->instance);
    if (runtime == NULL)
        elog(FATAL, "shared memory requires an active postgamma instance");
    return runtime;
}

static void
postgamma_shmem_delete(int code, Datum argument)
{
    PostgammaInstanceRuntime *owner = postgamma_shmem_owner();
    uint32 id = DatumGetUInt32(argument);
    int status;
    (void) code;
    if (id == 0) return;
    status = postgamma_platform_shmem_remove(owner,
        postgamma_instance_runtime_generation(owner), id);
    if (status != 0)
        elog(FATAL, "shared memory removal failed: %s", strerror(status));
    if (PGM_SHMEM_STATE(UsedShmemSegID) == id)
        PGSharedMemoryDetach();
}

PGShmemHeader *
PGSharedMemoryCreate(Size size, PGShmemHeader **shim)
{
    PostgammaInstanceRuntime *owner = postgamma_shmem_owner();
    uint32 id;
    void *address;
    PGShmemHeader *header;
    char identity[64];
    int status;
    if (shim == NULL || size <= MAXALIGN(sizeof(PGShmemHeader)) ||
        PGM_SHMEM_STATE(UsedShmemSegAddr) != NULL)
        elog(FATAL, "invalid shared memory creation state");
    if (POSTGAMMA_GUC_VALUE(huge_pages) == HUGE_PAGES_ON ||
        POSTGAMMA_GUC_VALUE(shared_memory_type) != SHMEM_TYPE_MMAP)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("WebAssembly requires shared_memory_type=mmap and huge_pages=off or try")));
    status = postgamma_platform_shmem_create(owner,
        postgamma_instance_runtime_generation(owner), postgamma_execution_context_current(),
        size, Max(PG_IO_ALIGN_SIZE, PG_CACHE_LINE_SIZE), &id, &address);
    if (status != 0)
        elog(FATAL, "shared memory allocation failed: %s", strerror(status));
    PGM_SHMEM_STATE(UsedShmemSegID) = id;
    PGM_SHMEM_STATE(UsedShmemSegAddr) = address;
    PG_TRY();
    {
        /* The immutable ID survives an explicit detach before exit. */
        on_shmem_exit(postgamma_shmem_delete, UInt32GetDatum(id));
    }
    PG_CATCH();
    {
        /* Registration can ERROR before it installs the cleanup callback. */
        (void) postgamma_platform_shmem_remove(owner,
            postgamma_instance_runtime_generation(owner), id);
        PGSharedMemoryDetach();
        PG_RE_THROW();
    }
    PG_END_TRY();
    header = address;
    header->magic = PGShmemMagic;
    header->creatorPID = 0; /* Compatibility field; identity is the instance generation. */
    header->totalsize = size;
    header->content_offset = MAXALIGN(sizeof(PGShmemHeader));
    header->dsm_control = 0;
    header->device = 0;
    header->inode = 0;
    *shim = header;
    snprintf(identity, sizeof(identity), "%lu %lu", (unsigned long) id, (unsigned long) id);
    AddToDataDirLockFile(LOCK_FILE_LINE_SHMEM_KEY, identity);
    SetConfigOption("huge_pages_status", "off", PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
    return header;
}

void
PGSharedMemoryReAttach(void)
{
    PostgammaInstanceRuntime *owner = postgamma_shmem_owner();
    int status = postgamma_platform_shmem_attach(owner,
        postgamma_instance_runtime_generation(owner), postgamma_execution_context_current(),
        PGM_SHMEM_STATE(UsedShmemSegID), PGM_SHMEM_STATE(UsedShmemSegAddr));
    if (status != 0)
        elog(FATAL, "shared memory attach failed: %s", strerror(status));
}

void
PGSharedMemoryDetach(void)
{
    PostgammaInstanceRuntime *owner = postgamma_shmem_owner();
    uint32 id = PGM_SHMEM_STATE(UsedShmemSegID);
    int status;
    if (id == 0) return;
    status = postgamma_platform_shmem_detach(owner,
        postgamma_instance_runtime_generation(owner), postgamma_execution_context_current(), id);
    /* A role without shared-memory access inherits IDs but never attaches. */
    if (status != 0 && status != ENOENT)
        elog(FATAL, "shared memory detach failed: %s", strerror(status));
    PGM_SHMEM_STATE(UsedShmemSegAddr) = NULL;
    PGM_SHMEM_STATE(UsedShmemSegID) = 0;
}

void
PGSharedMemoryNoReAttach(void)
{
    PGSharedMemoryDetach();
}

bool
PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2)
{
    return id1 == id2 && postgamma_platform_shmem_in_use((uint32) id2);
}

void
GetHugePageSize(Size *size, int *flags)
{
    if (size != NULL) *size = 0;
    if (flags != NULL) *flags = 0;
}

bool
check_huge_page_size(int *value, void **extra, GucSource source)
{
    (void) extra;
    (void) source;
    if (*value == 0) return true;
    GUC_check_errdetail("huge_page_size must be 0 on WebAssembly");
    return false;
}
#undef PGM_SHMEM_STATE
#endif
