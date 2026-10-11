/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define POSTGAMMA_WASM_LOGICAL_ALLOC_IMPLEMENTATION
#include "logical_alloc.h"
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
/* Only the private pg_dump/restore closure is redirected here. Libpq and the
 * database retain their own allocation ownership. Untracked pointers returned
 * by libc/libpq can be freed normally, without reading before their base. */
#define BUCKETS 1024
typedef struct Allocation {void *pointer; struct Allocation *next;} Allocation;
static _Thread_local Allocation *allocations[BUCKETS];
static _Thread_local bool active;
static size_t bucket(void *pointer) {return ((uintptr_t)pointer >> 4) % BUCKETS;}
static Allocation **find(void *pointer)
{
    Allocation **entry = &allocations[bucket(pointer)];
    while (*entry && (*entry)->pointer != pointer) entry = &(*entry)->next;
    return entry;
}
static void *track(void *pointer)
{
    if (pointer && active) {
        Allocation *entry = malloc(sizeof(*entry));
        if (!entry) {free(pointer); errno = ENOMEM; return NULL;}
        size_t index = bucket(pointer);
        *entry = (Allocation){pointer, allocations[index]}; allocations[index] = entry;
    }
    return pointer;
}
void postgamma_wasm_logical_alloc_begin(void)
{
    if (active) abort();
    active = true;
}
void postgamma_wasm_logical_alloc_end(void)
{
    for (size_t i = 0; i < BUCKETS; i++) while (allocations[i]) {
        Allocation *entry = allocations[i]; allocations[i] = entry->next;
        free(entry->pointer); free(entry);
    }
    active = false;
}
void *postgamma_wasm_logical_malloc(size_t size) {return track(malloc(size));}
void *postgamma_wasm_logical_calloc(size_t count, size_t size) {return track(calloc(count, size));}
void postgamma_wasm_logical_free(void *pointer)
{
    if (!pointer) return;
    Allocation **slot = find(pointer), *entry = *slot;
    if (entry) {*slot = entry->next; free(entry);}
    free(pointer);
}
void *postgamma_wasm_logical_realloc(void *pointer, size_t size)
{
    if (!pointer) return postgamma_wasm_logical_malloc(size);
    if (!size) {postgamma_wasm_logical_free(pointer); return NULL;}
    Allocation **slot = find(pointer), *entry = *slot;
    void *replacement = realloc(pointer, size);
    if (!replacement) return NULL; /* Old ownership survives allocation failure. */
    if (entry) {
        *slot = entry->next;
        size_t index = bucket(replacement);
        entry->pointer = replacement; entry->next = allocations[index]; allocations[index] = entry;
    }
    // An external allocation resized here remains externally owned.
    return replacement;
}
char *postgamma_wasm_logical_strndup(const char *value, size_t size)
{
    size_t length = strnlen(value, size);
    char *copy = postgamma_wasm_logical_malloc(length + 1);
    if (copy) {memcpy(copy, value, length); copy[length] = 0;}
    return copy;
}
char *postgamma_wasm_logical_strdup(const char *value) {return postgamma_wasm_logical_strndup(value, strlen(value));}
