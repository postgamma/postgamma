/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include "postgamma/private/public_runtime.h"

/* Retain a real backend endpoint reference so connection close must wait.
 * The test releases it through the same driver that must continue serving SQL. */
static PostgammaMemoryEndpoint *close_gate_endpoint;
static uint64_t close_gate_generation;
static int close_gate(pgm_connection *connection, unsigned hold)
{
    if (hold) {
        if (close_gate_endpoint || !connection->backend_monitor) return EBUSY;
        close_gate_generation = connection->instance->generation;
        return postgamma_memory_endpoint_retain(connection->backend_monitor,
            close_gate_generation, &close_gate_endpoint) == POSTGAMMA_MEMORY_STATUS_OK ? 0 : EIO;
    }
    if (!close_gate_endpoint) return EINVAL;
    return postgamma_memory_endpoint_release(&close_gate_endpoint,
        close_gate_generation) == POSTGAMMA_MEMORY_STATUS_OK ? 0 : EIO;
}
