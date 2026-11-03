/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_LOCALE_PLATFORM_REDIRECT_H
#define POSTGAMMA_LOCALE_PLATFORM_REDIRECT_H
#ifdef __EMSCRIPTEN__
#include "postgamma/locale_platform.h"
#define newlocale(mask, name, base) postgamma_platform_newlocale((mask), (name), (base))
#endif
#endif
