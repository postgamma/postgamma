/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#ifndef POSTGAMMA_VIEW_FIXTURE_INLINE_H
#define POSTGAMMA_VIEW_FIXTURE_INLINE_H

static inline int *
inline_state_address(int enabled)
{
	return enabled ? &POSTGAMMA_BACKEND_STATE_VALUE(0, outer) : NULL;
}

static inline int
inline_state_read(void)
{
	return *inline_state_address(1);
}

static inline int *
inline_static_identity(int enabled)
{
	static int value;
	if (enabled)
		value = POSTGAMMA_BACKEND_STATE_VALUE(0, outer);
	return &value;
}

static inline __attribute__((nonnull(1))) int
inline_annotated_read(const int *enabled)
{
	return *enabled ? POSTGAMMA_BACKEND_STATE_VALUE(0, outer) : 0;
}

static inline const char *
inline_builtin_identity(int enabled)
{
	if (enabled)
		POSTGAMMA_BACKEND_STATE_VALUE(0, outer)++;
	return __builtin_FUNCTION();
}

static inline int
inline_line_identity(void)
{
	return POSTGAMMA_BACKEND_STATE_VALUE(0, outer) + __LINE__;
}

#endif
