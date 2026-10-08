/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
/* Parse the actual SDK headers with the host generator's target frontend. */
#include <stdint.h>
#include <errno.h>

enum PostgammaSdkErrno { postgamma_sdk_exists = EEXIST, postgamma_sdk_invalid = EINVAL };
_Static_assert(UINT8_C(255) == UINT8_MAX, "SDK uint8 constants");
_Static_assert(UINT16_C(65535) == UINT16_MAX, "SDK uint16 constants");
_Static_assert(UINT32_C(4294967295) == UINT32_MAX, "SDK uint32 constants");
_Static_assert(UINT64_C(18446744073709551615) == UINT64_MAX, "SDK uint64 constants");
_Static_assert(INT8_C(127) == INT8_MAX && INT16_C(32767) == INT16_MAX, "SDK narrow signed constants");
_Static_assert(INT32_C(2147483647) == INT32_MAX && INT64_C(9223372036854775807) == INT64_MAX, "SDK signed constants");
_Static_assert(INTMAX_C(9223372036854775807) == INTMAX_MAX, "SDK intmax constants");
_Static_assert(UINTMAX_C(18446744073709551615) == UINTMAX_MAX, "SDK uintmax constants");
_Static_assert(sizeof(void *) == 4 && sizeof(long) == 4, "SDK wasm32 data model");
