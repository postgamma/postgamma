/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#ifndef POSTGAMMA_VIEW_FIXTURE_LATE_H
#define POSTGAMMA_VIEW_FIXTURE_LATE_H

typedef struct LateStateResult
{
	int value;
} LateStateResult;

static inline LateStateResult
late_state_read(void)
{
	return (LateStateResult) {POSTGAMMA_BACKEND_STATE_VALUE(0, outer)};
}

#endif
