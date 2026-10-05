/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_TOOL_CONTEXT_H
#define POSTGAMMA_PRIVATE_TOOL_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PostgammaToolKind
{
	POSTGAMMA_TOOL_BOOTSTRAP = 1,
	POSTGAMMA_TOOL_SINGLE_USER = 2
} PostgammaToolKind;

typedef enum PostgammaToolPhase
{
	POSTGAMMA_TOOL_PHASE_CREATED = 0,
	POSTGAMMA_TOOL_PHASE_RUNNING,
	POSTGAMMA_TOOL_PHASE_EXITED,
	POSTGAMMA_TOOL_PHASE_CLEANING,
	POSTGAMMA_TOOL_PHASE_COMPLETE,
	POSTGAMMA_TOOL_PHASE_FAILED
} PostgammaToolPhase;

typedef struct PostgammaToolContext PostgammaToolContext;
typedef int (*PostgammaToolCleanupFunction) (void *argument);

typedef struct PostgammaToolOptions
{
	uint64_t	generation;
	PostgammaToolKind kind;
	const char *data_directory;
	const char *resource_root;
	const char *bootstrap_input;
} PostgammaToolOptions;

typedef struct PostgammaToolTelemetry
{
	uint64_t	generation;
	PostgammaToolKind kind;
	PostgammaToolPhase phase;
	int			exit_code;
	size_t		cleanup_registered;
	size_t		cleanup_completed;
	size_t		cleanup_remaining;
	unsigned int active_bindings;
	unsigned int active_kernel_contexts;
} PostgammaToolTelemetry;

int postgamma_tool_context_create(const PostgammaToolOptions *options,
								  PostgammaToolContext **context);
int postgamma_tool_context_destroy(PostgammaToolContext *context);
int postgamma_tool_context_bind(PostgammaToolContext *context,
								PostgammaToolContext **previous);
int postgamma_tool_context_restore(PostgammaToolContext *expected_current,
								   PostgammaToolContext *previous);
PostgammaToolContext *postgamma_tool_context_current(void);
int postgamma_tool_context_begin(PostgammaToolContext *context);
int postgamma_tool_context_record_exit(PostgammaToolContext *context,
								   int exit_code);
int postgamma_tool_context_register_cleanup(
	PostgammaToolContext *context,
	PostgammaToolCleanupFunction function,
	void *argument);
int postgamma_tool_context_unwind(PostgammaToolContext *context);
int postgamma_tool_context_set_kernel_context(PostgammaToolContext *context,
									  void *kernel_context);
int postgamma_tool_context_clear_kernel_context(PostgammaToolContext *context,
										void *expected_kernel_context);
const char *postgamma_tool_context_data_directory(
	const PostgammaToolContext *context);
const char *postgamma_tool_context_resource_root(
	const PostgammaToolContext *context);
const char *postgamma_tool_context_bootstrap_input(
	const PostgammaToolContext *context);
int postgamma_tool_context_telemetry(const PostgammaToolContext *context,
									PostgammaToolTelemetry *telemetry);
const char *postgamma_tool_phase_name(PostgammaToolPhase phase);

#ifdef __cplusplus
}
#endif

#endif
