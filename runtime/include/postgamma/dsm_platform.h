/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_DSM_PLATFORM_H
#define POSTGAMMA_DSM_PLATFORM_H
#include <stddef.h>
#include <stdint.h>

typedef struct PostgammaDsmMapping PostgammaDsmMapping;
int postgamma_platform_dsm_create(const void *owner, uint64_t generation, uint32_t handle,
    size_t size, PostgammaDsmMapping **mapping, void **address, size_t *mapped_size);
int postgamma_platform_dsm_attach(const void *owner, uint64_t generation, uint32_t handle,
    PostgammaDsmMapping **mapping, void **address, size_t *mapped_size);
int postgamma_platform_dsm_detach(PostgammaDsmMapping **mapping);
int postgamma_platform_dsm_destroy(const void *owner, uint64_t generation, uint32_t handle);
size_t postgamma_platform_dsm_count(void);
#endif
