/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include "postgres.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef Datum (*PostgammaLayoutCallback) (int64_t, const void *, size_t);

struct PostgammaTargetLayout
{
	char tag;
	void *pointer;
	int64_t integer;
	off_t offset;
	PostgammaLayoutCallback callback;
};

void *postgamma_layout_pointer;
long postgamma_layout_long;
Datum postgamma_layout_datum;
struct PostgammaTargetLayout postgamma_layout_record;
PostgammaLayoutCallback postgamma_layout_callback;
_Thread_local uint64_t postgamma_layout_tls = 42;

_Static_assert(sizeof(void *) == 4 && sizeof(long) == 4, "wasm32 data model");
_Static_assert(sizeof(Datum) == 8 && sizeof(size_t) == 4, "target PostgreSQL ABI");
_Static_assert(sizeof(off_t) == 8 && sizeof(int64_t) == 8, "64-bit file and integer ABI");
_Static_assert(sizeof(PostgammaLayoutCallback) == 4, "function table ABI");
_Static_assert(sizeof(struct PostgammaTargetLayout) == 32, "record size");
_Static_assert(_Alignof(struct PostgammaTargetLayout) == 8, "record alignment");
_Static_assert(offsetof(struct PostgammaTargetLayout, pointer) == 4, "pointer offset");
_Static_assert(offsetof(struct PostgammaTargetLayout, integer) == 8, "integer offset");
_Static_assert(offsetof(struct PostgammaTargetLayout, offset) == 16, "file offset");
_Static_assert(offsetof(struct PostgammaTargetLayout, callback) == 24, "callback offset");
