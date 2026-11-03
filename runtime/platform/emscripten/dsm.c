/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/dsm_platform.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct DsmSegment {
    const void *owner;
    uint64_t generation;
    uint32_t handle;
    size_t size, references;
    void *address;
    bool removed;
    struct DsmSegment *next;
} DsmSegment;

struct PostgammaDsmMapping { DsmSegment *segment; };
static pthread_mutex_t dsm_lock = PTHREAD_MUTEX_INITIALIZER;
static DsmSegment *dsm_segments;

static DsmSegment *
lookup(const void *owner, uint64_t generation, uint32_t handle)
{
    for (DsmSegment *s = dsm_segments; s; s = s->next)
        if (s->owner == owner && s->generation == generation && s->handle == handle) return s;
    return NULL;
}

static void
reap(void)
{
    DsmSegment **link = &dsm_segments;
    while (*link) {
        DsmSegment *s = *link;
        if (s->removed && s->references == 0) {
            *link = s->next;
            free(s->address);
            free(s);
        } else link = &s->next;
    }
}

int
postgamma_platform_dsm_create(const void *owner, uint64_t generation, uint32_t handle,
    size_t size, PostgammaDsmMapping **mapping, void **address, size_t *mapped_size)
{
    if (!owner || !generation || !handle || !size || !mapping || *mapping ||
        !address || !mapped_size) return EINVAL;
    DsmSegment *s = calloc(1, sizeof(*s));
    PostgammaDsmMapping *m = malloc(sizeof(*m));
    void *data = calloc(1, size);
    if (!s || !m || !data) { free(s); free(m); free(data); return ENOMEM; }
    pthread_mutex_lock(&dsm_lock);
    if (lookup(owner, generation, handle)) {
        pthread_mutex_unlock(&dsm_lock);
        free(s); free(m); free(data);
        return EEXIST;
    }
    s->owner = owner;
    s->generation = generation;
    s->handle = handle;
    s->size = size;
    s->address = data;
    s->references = 1;
    s->next = dsm_segments;
    dsm_segments = s;
    m->segment = s;
    *mapping = m;
    *address = data;
    *mapped_size = size;
    pthread_mutex_unlock(&dsm_lock);
    return 0;
}

int
postgamma_platform_dsm_attach(const void *owner, uint64_t generation, uint32_t handle,
    PostgammaDsmMapping **mapping, void **address, size_t *mapped_size)
{
    int status = 0;
    if (!owner || !generation || !handle || !mapping || *mapping || !address || !mapped_size) return EINVAL;
    pthread_mutex_lock(&dsm_lock);
    DsmSegment *s = lookup(owner, generation, handle);
    if (!s || s->removed) status = ENOENT;
    else if (s->references == SIZE_MAX) status = EOVERFLOW;
    else {
        PostgammaDsmMapping *m = malloc(sizeof(*m));
        if (!m) status = ENOMEM;
        else {
            m->segment = s;
            s->references++;
            *mapping = m;
            *address = s->address;
            *mapped_size = s->size;
        }
    }
    pthread_mutex_unlock(&dsm_lock);
    return status;
}

int
postgamma_platform_dsm_detach(PostgammaDsmMapping **mapping)
{
    if (!mapping || !*mapping) return EINVAL;
    pthread_mutex_lock(&dsm_lock);
    (*mapping)->segment->references--;
    free(*mapping);
    *mapping = NULL;
    reap();
    pthread_mutex_unlock(&dsm_lock);
    return 0;
}

int
postgamma_platform_dsm_destroy(const void *owner, uint64_t generation, uint32_t handle)
{
    int status = 0;
    pthread_mutex_lock(&dsm_lock);
    DsmSegment *s = lookup(owner, generation, handle);
    if (!s) status = ENOENT;
    else { s->removed = true; reap(); }
    pthread_mutex_unlock(&dsm_lock);
    return status;
}

size_t
postgamma_platform_dsm_count(void)
{
    size_t count = 0;
    pthread_mutex_lock(&dsm_lock);
    for (DsmSegment *s = dsm_segments; s; s = s->next) count++;
    pthread_mutex_unlock(&dsm_lock);
    return count;
}
