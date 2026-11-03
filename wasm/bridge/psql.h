/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#ifndef POSTGAMMA_WASM_PSQL_H
#define POSTGAMMA_WASM_PSQL_H
#include "postgamma/postgamma.h"
#include <stddef.h>
/* One upstream psql invocation per module. The terminal owns its instance;
 * closing/restarting the demo destroys the module, including frontend globals. */
int postgamma_wasm_psql_start(pgm_instance *instance);
size_t postgamma_wasm_psql_read(void *data, size_t capacity, unsigned *state, int *code);
int postgamma_wasm_psql_write(const void *data, size_t size);
void postgamma_wasm_psql_interrupt(void);
int postgamma_wasm_psql_close(void);
#endif
