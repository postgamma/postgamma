/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * tool_state_runtime.h
 *    Runtime API for PostgreSQL frontend-tool state virtualization.
 *
 * PostgreSQL-dependent slots are generated from the frontend-tool inventory.
 * The handwritten runtime owns only invocation binding and storage mechanics.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_TOOL_STATE_RUNTIME_H
#define POSTGAMMA_TOOL_STATE_RUNTIME_H

#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

#include "postgamma/tool_state_layout.h"

typedef struct PostgammaToolStateTemplate
{
	PostgammaToolStateSlot slot;
	const volatile void *address;
	size_t		expected_matches;
} PostgammaToolStateTemplate;

typedef void (*PostgammaToolStateFailureFunction) (void *argument, int status);

int postgamma_tool_state_create(void **state);
void postgamma_tool_state_destroy(void **state);
int postgamma_tool_state_bind(
	void *state, PostgammaToolStateFailureFunction failure, void *argument);
int postgamma_tool_state_unbind(void *expected_state);
void *postgamma_tool_state_address(
	PostgammaToolStateSlot slot, const volatile void *template_address);
void *postgamma_tool_state_address_relocated(
	PostgammaToolStateSlot slot, const volatile void *template_address,
	const PostgammaToolStateTemplate *targets, size_t target_count);

#define POSTGAMMA_TOOL_STATE_VALUE(slot, original) \
	(*(__typeof__(&(original))) postgamma_tool_state_address((slot), &(original)))

#define POSTGAMMA_TOOL_STATE_RELOCATED_VALUE(original, bridge) \
	(*(__typeof__(&(original))) (bridge)())

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_TOOL_STATE_RUNTIME_H */
