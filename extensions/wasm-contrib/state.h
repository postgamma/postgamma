/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#ifndef POSTGAMMA_WASM_CONTRIB_STATE_H
#define POSTGAMMA_WASM_CONTRIB_STATE_H
#include "postgres.h"
#include "postgamma/backend_state_runtime.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include <errno.h>

/* A carrier may run different logical backends on consecutive quantums.
 * Extension GUC addresses must therefore follow the role, not pthread TLS. */
static inline void *
postgamma_contrib_role_state_address(const char *identifier,
    const volatile void *initial, size_t size, size_t alignment)
{
    void *value = postgamma_extension_role_state_address(identifier, initial, size, alignment);
    if (!value)
        ereport(ERROR, (errcode(errno == ENOMEM ? ERRCODE_OUT_OF_MEMORY : ERRCODE_INTERNAL_ERROR),
            errmsg("could not bind contrib role state: %s", identifier)));
    return value;
}
#endif
