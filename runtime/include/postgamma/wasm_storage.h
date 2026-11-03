/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_WASM_STORAGE_H
#define POSTGAMMA_WASM_STORAGE_H
#ifdef __cplusplus
extern "C" {
#endif
/* Target-private storage lifetime; called on the host driver before/after PG. */
int postgamma_wasm_storage_mount(const char *name, const char *test_channel);
int postgamma_wasm_storage_close(void);
unsigned postgamma_wasm_storage_pending_writes(void);
/* Last observed I/O stack size, retained for diagnostics after shutdown. */
unsigned postgamma_wasm_storage_stack_bytes(void);
#ifdef __cplusplus
}
#endif
#endif
