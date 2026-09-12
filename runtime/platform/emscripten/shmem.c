/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/shmem_platform.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

/* Storage lives until removal is requested and the last logical role detaches. */
typedef struct Attachment {
    const void *execution;
    struct Attachment *next;
} Attachment;

typedef struct Segment {
    uint32_t id;
    const void *owner;
    uint64_t generation;
    void *address;
    bool removed;
    Attachment *attachments;
    struct Segment *next;
} Segment;

static pthread_mutex_t segment_lock = PTHREAD_MUTEX_INITIALIZER;
static Segment *segments;
static uint32_t next_id = 1;

static Segment *
lookup(const void *owner, uint64_t generation, uint32_t id)
{
    for (Segment *s = segments; s; s = s->next)
        if (s->id == id && s->owner == owner && s->generation == generation)
            return s;
    return NULL;
}

static void
reap(void)
{
    Segment **link = &segments;
    while (*link) {
        Segment *s = *link;
        if (s->removed && !s->attachments) {
            *link = s->next;
            free(s->address);
            free(s);
        } else link = &s->next;
    }
}

int
postgamma_platform_shmem_create(const void *owner, uint64_t generation,
    const void *attacher, size_t size, uint32_t *id, void **address)
{
    if (!owner || !generation || !attacher || !size || !id || !address) return EINVAL;
    *id = 0;
    *address = NULL;
    Segment *s = calloc(1, sizeof(*s));
    Attachment *a = malloc(sizeof(*a));
    void *data = calloc(1, size);
    if (!s || !a || !data) { free(s); free(a); free(data); return ENOMEM; }
    a->execution = attacher;
    a->next = NULL;
    s->owner = owner;
    s->generation = generation;
    s->address = data;
    s->attachments = a;
    pthread_mutex_lock(&segment_lock);
    /* Never reuse an ID, including after an instance context address is reused. */
    if (next_id == 0) {
        pthread_mutex_unlock(&segment_lock);
        free(s); free(a); free(data);
        return EOVERFLOW;
    }
    s->id = next_id++;
    s->next = segments;
    segments = s;
    *id = s->id;
    *address = data;
    pthread_mutex_unlock(&segment_lock);
    return 0;
}

int
postgamma_platform_shmem_attach(const void *owner, uint64_t generation,
    const void *attacher, uint32_t id, void *expected_address)
{
    int status = 0;
    if (!owner || !generation || !attacher || !id || !expected_address) return EINVAL;
    pthread_mutex_lock(&segment_lock);
    Segment *s = lookup(owner, generation, id);
    if (!s || s->removed) status = ESTALE;
    else if (s->address != expected_address) status = EINVAL;
    else {
        for (Attachment *a = s->attachments; a; a = a->next)
            if (a->execution == attacher) { status = EALREADY; break; }
        if (!status) {
            Attachment *a = malloc(sizeof(*a));
            if (!a) status = ENOMEM;
            else {
                a->execution = attacher;
                a->next = s->attachments;
                s->attachments = a;
            }
        }
    }
    pthread_mutex_unlock(&segment_lock);
    return status;
}

int
postgamma_platform_shmem_detach(const void *owner, uint64_t generation,
    const void *attacher, uint32_t id)
{
    int status = 0;
    pthread_mutex_lock(&segment_lock);
    Segment *s = lookup(owner, generation, id);
    if (!s) status = ESTALE;
    else {
        Attachment **link = &s->attachments;
        while (*link && (*link)->execution != attacher) link = &(*link)->next;
        if (!*link) status = ENOENT;
        else { Attachment *a = *link; *link = a->next; free(a); }
        reap();
    }
    pthread_mutex_unlock(&segment_lock);
    return status;
}

int
postgamma_platform_shmem_remove(const void *owner, uint64_t generation, uint32_t id)
{
    int status = 0;
    pthread_mutex_lock(&segment_lock);
    Segment *s = lookup(owner, generation, id);
    if (!s) status = ESTALE;
    else { s->removed = true; reap(); }
    pthread_mutex_unlock(&segment_lock);
    return status;
}

bool
postgamma_platform_shmem_in_use(uint32_t id)
{
    bool found = false;
    pthread_mutex_lock(&segment_lock);
    for (Segment *s = segments; s; s = s->next)
        if (s->id == id) { found = s->attachments != NULL; break; }
    pthread_mutex_unlock(&segment_lock);
    return found;
}

size_t
postgamma_platform_shmem_count(void)
{
    size_t count = 0;
    pthread_mutex_lock(&segment_lock);
    for (Segment *s = segments; s; s = s->next) count++;
    pthread_mutex_unlock(&segment_lock);
    return count;
}
