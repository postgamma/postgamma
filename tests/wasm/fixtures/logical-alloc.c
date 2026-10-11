/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define POSTGAMMA_WASM_LOGICAL_ALLOC_IMPLEMENTATION
#include "wasm/bridge/logical_alloc.h"
#include <assert.h>
int main(void)
{
    for (int round = 0; round < 40; round++) {
        char *external = malloc(32);
        assert(external); external[0] = 'x';
        postgamma_wasm_logical_alloc_begin();
        for (int i = 0; i < 1000; i++) {
            char *p = postgamma_wasm_logical_calloc(23, 1);
            assert(p && !p[22]); p[0] = 'a';
            p = postgamma_wasm_logical_realloc(p, 64 + i);
            assert(p && p[0] == 'a');
            if (i % 3) postgamma_wasm_logical_free(p);
            // Remaining buffers intentionally mimic pg_dump's process-exit cleanup.
        }
        char *text = postgamma_wasm_logical_strdup("snow");
        assert(text && strcmp(text, "snow") == 0);
        postgamma_wasm_logical_free(text);
        text = postgamma_wasm_logical_strndup("snow", 2);
        assert(text && strcmp(text, "sn") == 0);
        external = postgamma_wasm_logical_realloc(external, 100);
        assert(external);
        postgamma_wasm_logical_free(malloc(16));
        postgamma_wasm_logical_alloc_end();
        assert(external[0] == 'x'); free(external);
    }
    return 0;
}
