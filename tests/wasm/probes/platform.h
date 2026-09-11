/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#ifndef POSTGAMMA_PLATFORM_PROBE_H
#define POSTGAMMA_PLATFORM_PROBE_H

#include <stdbool.h>
#include <stddef.h>

void probe_check(bool condition, const char *expression, const char *file, int line);
void probe_begin(void);
void probe_report(const char *format, ...) __attribute__((format(printf, 1, 2)));
const char *probe_end(void);
void probe_libc(void);

#define CHECK(condition) probe_check((condition), #condition, __FILE__, __LINE__)

#endif
