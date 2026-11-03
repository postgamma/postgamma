/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/locale_platform.h"
#include <errno.h>
#include <string.h>

bool postgamma_platform_locale_supported(const char *name)
{
    return name == NULL || name[0] == '\0' ||
        strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0;
}

locale_t postgamma_platform_newlocale(int mask, const char *name, locale_t base)
{
    if (name == NULL || !postgamma_platform_locale_supported(name))
    {
        errno = name == NULL ? EINVAL : ENOENT;
        return (locale_t) 0;
    }
    /* Empty names resolve to the selected C profile, independent of JS environment. */
    return newlocale(mask, name[0] == '\0' ? "C" : name, base);
}
