/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_IO_PLATFORM_REDIRECT_H
#define POSTGAMMA_IO_PLATFORM_REDIRECT_H
#ifdef __EMSCRIPTEN__
#include "postgamma/io_platform.h"
#define pipe postgamma_platform_pipe
#define read postgamma_platform_read
#define write postgamma_platform_write
#define close postgamma_platform_close
#define poll postgamma_platform_poll
#endif
#endif
