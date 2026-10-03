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

/* The Wasm driver serializes connection use and retains its lifetime. Pump
 * only idle libpq input, never an active request that its consumer has paused. */
void postgamma_public_wasm_consume_idle(pgm_connection *connection);

/* Take one owned queued event without pumping requests or rearming descriptor
 * wakes. The Wasm driver waits on its own epoch, not the instance waitable. */
pgm_status postgamma_public_wasm_next_event(
    pgm_instance *instance, pgm_event **event, pgm_error **error);

#endif
