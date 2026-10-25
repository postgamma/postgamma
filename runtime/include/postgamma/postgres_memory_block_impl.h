/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_POSTGRES_MEMORY_BLOCK_IMPL_H
#define POSTGAMMA_POSTGRES_MEMORY_BLOCK_IMPL_H

/* Included after system headers, only in PostgreSQL's four context allocators. */
#include "postgamma/memory_block_runtime.h"
#ifdef POSTGAMMA_PRIVATE_MEMORY_BLOCKS
#define malloc(size) postgamma_context_block_malloc(size)
#define realloc(pointer, size) postgamma_context_block_realloc(pointer, size)
#define free(pointer) postgamma_memory_block_free(pointer)
#endif
#endif
