/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_LOCALE_PLATFORM_H
#define POSTGAMMA_LOCALE_PLATFORM_H
#include <locale.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool postgamma_platform_locale_supported(const char *name);
locale_t postgamma_platform_newlocale(int mask, const char *name, locale_t base);
#ifdef __cplusplus
}
#endif
#endif
