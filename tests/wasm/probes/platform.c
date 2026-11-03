/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "platform.h"

#include <emscripten.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

static char report[32768];
static size_t used;
static unsigned checks, failures;
static char failed_at[4096];
static size_t failed_used;
static pthread_mutex_t check_lock = PTHREAD_MUTEX_INITIALIZER;

void
probe_check(bool condition, const char *expression, const char *file, int line)
{
    pthread_mutex_lock(&check_lock);
    checks++;
    if (!condition) {
        const char *name = strrchr(file, '/');
        int written;
        fprintf(stderr, "%s:%d: failed: %s\n", file, line, expression);
        written = snprintf(failed_at + failed_used, sizeof(failed_at) - failed_used,
                           "%s\"%s:%d\"", failures ? "," : "", name ? name + 1 : file, line);
        if (written < 0 || (size_t) written >= sizeof(failed_at) - failed_used)
            abort();
        failed_used += (size_t) written;
        failures++;
    }
    pthread_mutex_unlock(&check_lock);
}

void
probe_begin(void)
{
    used = checks = failures = failed_used = 0;
    failed_at[0] = '\0';
    probe_report("{");
}

void
probe_report(const char *format, ...)
{
    va_list args;
    int written;
    va_start(args, format);
    written = vsnprintf(report + used, sizeof(report) - used, format, args);
    va_end(args);
    if (written < 0 || (size_t) written >= sizeof(report) - used)
        abort();
    used += (size_t) written;
}

const char *
probe_end(void)
{
    probe_report("\"checks\":%u,\"failures\":%u,\"failedChecks\":[%s]}",
                 checks, failures, failed_at);
    return report;
}

static int add(int a, int b) { return a + b; }
static double scale(double a, double b) { return a * b; }

EMSCRIPTEN_KEEPALIVE uint64_t
probe_u64(uint64_t value)
{
    return value ^ UINT64_C(0xfedcba9876543210);
}

EMSCRIPTEN_KEEPALIVE int64_t
probe_i64(int64_t value)
{
    return value + 17;
}

EMSCRIPTEN_KEEPALIVE const char *
probe_sync(void)
{
    struct layout { char a; uint64_t b; void *c; double d; };
    uint32_t endian = UINT32_C(0x01020304);
    /* Volatile dispatch keeps these as typed indirect calls under optimization. */
    int (*volatile integer_callback)(int, int) = add;
    double (*volatile float_callback)(double, double) = scale;
    void *allocation = malloc(37);

    probe_begin();
    CHECK(sizeof(void *) == 4 && sizeof(long) == 4 && sizeof(size_t) == 4);
    CHECK(sizeof(int) == 4 && sizeof(long long) == 8 && sizeof(off_t) == 8);
    CHECK(sizeof(uintptr_t) == sizeof(void *) && sizeof(ptrdiff_t) == 4);
    CHECK(*(unsigned char *) &endian == 4);
    CHECK(alignof(uint64_t) == 8 && alignof(double) == 8);
    CHECK(offsetof(struct layout, b) == 8 && offsetof(struct layout, d) == 24);
    CHECK(sizeof(struct layout) == 32);
    CHECK(allocation != NULL && (uintptr_t) allocation % alignof(max_align_t) == 0);
    CHECK(integer_callback(17, 25) == 42 && float_callback(1.5, 2.0) == 3.0);
    probe_report("\"abi\":{\"pointer\":%zu,\"long\":%zu,\"size_t\":%zu,"
                 "\"ptrdiff_t\":%zu,\"off_t\":%zu,\"time_t\":%zu,\"pthread_t\":%zu,"
                 "\"longLong\":%zu,\"longDouble\":%zu,\"wchar_t\":%zu,\"functionPointer\":%zu,"
                 "\"uint64Alignment\":%zu,\"doubleAlignment\":%zu,\"maxAlignment\":%zu,"
                 "\"layoutOffsets\":[%zu,%zu,%zu,%zu],\"layoutSize\":%zu,"
                 "\"littleEndian\":%s,\"integerCallback\":%d,\"floatCallback\":%.1f},",
                 sizeof(void *), sizeof(long), sizeof(size_t), sizeof(ptrdiff_t), sizeof(off_t),
                 sizeof(time_t), sizeof(pthread_t), sizeof(long long), sizeof(long double),
                 sizeof(wchar_t), sizeof(integer_callback), alignof(uint64_t), alignof(double),
                 alignof(max_align_t), offsetof(struct layout, a), offsetof(struct layout, b),
                 offsetof(struct layout, c), offsetof(struct layout, d), sizeof(struct layout),
                 *(unsigned char *) &endian == 4 ? "true" : "false",
                 integer_callback(17, 25), float_callback(1.5, 2.0));
    free(allocation);
    probe_libc();
    return probe_end();
}
