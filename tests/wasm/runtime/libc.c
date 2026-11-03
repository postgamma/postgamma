/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/locale_platform.h"
#include <assert.h>
#include <emscripten.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

EM_JS(void, watch_entropy, (void), {
    globalThis.postgammaEntropyCalls = 0;
    globalThis.postgammaEntropyOriginal = crypto.getRandomValues;
    crypto.getRandomValues = function(view) {
        globalThis.postgammaEntropyCalls++;
        return globalThis.postgammaEntropyOriginal.call(this, view);
    };
});
EM_JS(int, finish_entropy, (void), {
    crypto.getRandomValues = globalThis.postgammaEntropyOriginal;
    return globalThis.postgammaEntropyCalls;
});

void postgamma_runtime_libc_test(void)
{
    unsigned char first[512], second[256];
    int fd = open("/dev/urandom", O_RDONLY);
    assert(fd >= 0);
    watch_entropy();
    assert(read(fd, first, sizeof(first)) == sizeof(first));
    assert(getentropy(second, sizeof(second)) == 0);
    assert(finish_entropy() == 3);
    assert(memcmp(first, first + 256, 256) != 0);
    assert(memcmp(first, second, 256) != 0);
    assert(close(fd) == 0);
    locale_t c = postgamma_platform_newlocale(LC_ALL_MASK, "C", (locale_t) 0);
    assert(c != (locale_t) 0);
    errno = 0;
    assert(postgamma_platform_newlocale(LC_ALL_MASK, "de_DE.UTF-8", c) == (locale_t) 0);
    assert(errno == ENOENT);
    /* Rejected names must leave the caller's base locale live. */
    locale_t old = uselocale(c);
    assert(old != (locale_t) 0);
    assert(strcoll("z", "a") > 0);
    assert(uselocale(old) == c);
    freelocale(c);
}
