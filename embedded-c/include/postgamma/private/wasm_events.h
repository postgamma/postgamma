/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_WASM_EVENTS_H
#define POSTGAMMA_PRIVATE_WASM_EVENTS_H

#include "postgamma/postgamma.h"
#include <stdbool.h>

/* Private readiness edge for an owned live request. No native layout or
 * pointer is exposed to JavaScript. The caller retains its parent connection. */
uint64_t postgamma_public_request_ready_sequence(pgm_request *request);

/* Non-consuming level check after a NEXT call returns AGAIN. A partial libpq
 * read can leave the transport readable without another producer edge. */
bool postgamma_public_request_progress_ready(pgm_request *request);

#endif
