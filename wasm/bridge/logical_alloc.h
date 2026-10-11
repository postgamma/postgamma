/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#ifndef POSTGAMMA_WASM_LOGICAL_ALLOC_H
#define POSTGAMMA_WASM_LOGICAL_ALLOC_H
#include <stdlib.h>
#include <string.h>
void postgamma_wasm_logical_alloc_begin(void);
void postgamma_wasm_logical_alloc_end(void);
void *postgamma_wasm_logical_malloc(size_t size);
void *postgamma_wasm_logical_calloc(size_t count, size_t size);
void *postgamma_wasm_logical_realloc(void *pointer, size_t size);
void postgamma_wasm_logical_free(void *pointer);
char *postgamma_wasm_logical_strdup(const char *value);
char *postgamma_wasm_logical_strndup(const char *value, size_t size);
#ifndef POSTGAMMA_WASM_LOGICAL_ALLOC_IMPLEMENTATION
#define malloc postgamma_wasm_logical_malloc
#define calloc postgamma_wasm_logical_calloc
#define realloc postgamma_wasm_logical_realloc
#define free postgamma_wasm_logical_free
#define strdup postgamma_wasm_logical_strdup
#define strndup postgamma_wasm_logical_strndup
#endif
#endif
