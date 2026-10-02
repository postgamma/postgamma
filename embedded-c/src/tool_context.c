/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "postgamma/private/tool_context.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>


#define POSTGAMMA_TOOL_CONTEXT_MAGIC UINT64_C(0x5047544F4F4C4354)

typedef struct PostgammaToolCleanup
{
	PostgammaToolCleanupFunction function;
	void	   *argument;
} PostgammaToolCleanup;

struct PostgammaToolContext
{
	uint64_t	magic;
	uint64_t	generation;
	PostgammaToolKind kind;
	PostgammaToolPhase phase;
	char	   *data_directory;
	char	   *resource_root;
	char	   *bootstrap_input;
	PostgammaToolCleanup *cleanups;
	size_t		cleanup_count;
	size_t		cleanup_capacity;
	size_t		cleanup_registered;
	size_t		cleanup_completed;
	void	   *kernel_context;
	int			exit_code;
	bool		exit_recorded;
	unsigned int active_bindings;
};

static _Thread_local PostgammaToolContext *PostgammaCurrentToolContext;


static bool
postgamma_tool_context_valid(const PostgammaToolContext *context)
{
	return context != NULL && context->magic == POSTGAMMA_TOOL_CONTEXT_MAGIC;
}


static char *
postgamma_tool_copy_string(const char *value)
{
	if (value == NULL || value[0] == '\0')
		return NULL;
	return strdup(value);
}


int
postgamma_tool_context_create(const PostgammaToolOptions *options,
							  PostgammaToolContext **context)
{
	PostgammaToolContext *created;

	if (context == NULL)
		return EINVAL;
	*context = NULL;
	if (options == NULL || options->generation == 0 ||
		(options->kind != POSTGAMMA_TOOL_BOOTSTRAP &&
		 options->kind != POSTGAMMA_TOOL_SINGLE_USER) ||
		options->data_directory == NULL || options->data_directory[0] == '\0' ||
		options->resource_root == NULL || options->resource_root[0] == '\0' ||
		options->bootstrap_input == NULL || options->bootstrap_input[0] == '\0')
		return EINVAL;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->cleanups = calloc(8, sizeof(*created->cleanups));
	if (created->cleanups == NULL)
	{
		free(created);
		return ENOMEM;
	}
	created->cleanup_capacity = 8;
	created->data_directory =
		postgamma_tool_copy_string(options->data_directory);
	created->resource_root = postgamma_tool_copy_string(options->resource_root);
	created->bootstrap_input =
		postgamma_tool_copy_string(options->bootstrap_input);
	if (created->data_directory == NULL || created->resource_root == NULL ||
		created->bootstrap_input == NULL)
	{
		free(created->bootstrap_input);
		free(created->resource_root);
		free(created->data_directory);
		free(created->cleanups);
		free(created);
		return ENOMEM;
	}
	created->magic = POSTGAMMA_TOOL_CONTEXT_MAGIC;
	created->generation = options->generation;
	created->kind = options->kind;
	created->phase = POSTGAMMA_TOOL_PHASE_CREATED;
	*context = created;
	return 0;
}


int
postgamma_tool_context_destroy(PostgammaToolContext *context)
{
	if (!postgamma_tool_context_valid(context))
		return EINVAL;
	if (context == PostgammaCurrentToolContext ||
		context->active_bindings != 0 || context->kernel_context != NULL ||
		context->cleanup_count != 0 ||
		(context->phase != POSTGAMMA_TOOL_PHASE_CREATED &&
		 context->phase != POSTGAMMA_TOOL_PHASE_COMPLETE &&
		 context->phase != POSTGAMMA_TOOL_PHASE_FAILED))
		return EBUSY;
	context->magic = 0;
	free(context->cleanups);
	free(context->bootstrap_input);
	free(context->resource_root);
	free(context->data_directory);
	free(context);
	return 0;
}


int
postgamma_tool_context_bind(PostgammaToolContext *context,
							PostgammaToolContext **previous)
{
	if (!postgamma_tool_context_valid(context) || previous == NULL ||
		context->phase != POSTGAMMA_TOOL_PHASE_CREATED ||
		context->active_bindings != 0)
		return EINVAL;
	*previous = PostgammaCurrentToolContext;
	PostgammaCurrentToolContext = context;
	context->active_bindings = 1;
	return 0;
}


int
postgamma_tool_context_restore(PostgammaToolContext *expected_current,
							   PostgammaToolContext *previous)
{
	if (!postgamma_tool_context_valid(expected_current) ||
		PostgammaCurrentToolContext != expected_current ||
		expected_current->active_bindings != 1 ||
		(previous != NULL && !postgamma_tool_context_valid(previous)))
		return EINVAL;
	expected_current->active_bindings = 0;
	PostgammaCurrentToolContext = previous;
	return 0;
}


PostgammaToolContext *
postgamma_tool_context_current(void)
{
	return PostgammaCurrentToolContext;
}


int
postgamma_tool_context_begin(PostgammaToolContext *context)
{
	if (!postgamma_tool_context_valid(context) ||
		context != PostgammaCurrentToolContext ||
		context->phase != POSTGAMMA_TOOL_PHASE_CREATED)
		return EINVAL;
	context->phase = POSTGAMMA_TOOL_PHASE_RUNNING;
	return 0;
}


int
postgamma_tool_context_record_exit(PostgammaToolContext *context, int exit_code)
{
	if (!postgamma_tool_context_valid(context) ||
		context != PostgammaCurrentToolContext ||
		context->phase != POSTGAMMA_TOOL_PHASE_RUNNING ||
		context->exit_recorded)
		return EINVAL;
	context->exit_recorded = true;
	context->exit_code = exit_code;
	context->phase = POSTGAMMA_TOOL_PHASE_EXITED;
	return 0;
}


int
postgamma_tool_context_register_cleanup(
	PostgammaToolContext *context,
	PostgammaToolCleanupFunction function,
	void *argument)
{
	PostgammaToolCleanup *expanded;
	size_t		capacity;

	if (!postgamma_tool_context_valid(context) || function == NULL ||
		(context->phase != POSTGAMMA_TOOL_PHASE_CREATED &&
		 context->phase != POSTGAMMA_TOOL_PHASE_RUNNING))
		return EINVAL;
	if (context->cleanup_count == context->cleanup_capacity)
	{
		capacity = context->cleanup_capacity == 0 ? 8 :
			context->cleanup_capacity * 2;
		if (capacity < context->cleanup_capacity ||
			capacity > SIZE_MAX / sizeof(*expanded))
			return EOVERFLOW;
		expanded = realloc(context->cleanups, capacity * sizeof(*expanded));
		if (expanded == NULL)
			return ENOMEM;
		context->cleanups = expanded;
		context->cleanup_capacity = capacity;
	}
	context->cleanups[context->cleanup_count++] =
		(PostgammaToolCleanup) {function, argument};
	context->cleanup_registered++;
	return 0;
}


int
postgamma_tool_context_unwind(PostgammaToolContext *context)
{
	int			first_error = 0;

	if (!postgamma_tool_context_valid(context) ||
		context != PostgammaCurrentToolContext ||
		(context->phase != POSTGAMMA_TOOL_PHASE_CREATED &&
		 context->phase != POSTGAMMA_TOOL_PHASE_EXITED &&
		 context->phase != POSTGAMMA_TOOL_PHASE_RUNNING))
		return EINVAL;
	context->phase = POSTGAMMA_TOOL_PHASE_CLEANING;
	while (context->cleanup_count != 0)
	{
		PostgammaToolCleanup cleanup =
			context->cleanups[--context->cleanup_count];
		int			status = cleanup.function(cleanup.argument);

		context->cleanup_completed++;
		if (status != 0 && first_error == 0)
			first_error = status;
	}
	if (context->kernel_context != NULL && first_error == 0)
		first_error = EBUSY;
	context->phase = context->exit_recorded && context->exit_code == 0 &&
		first_error == 0 ? POSTGAMMA_TOOL_PHASE_COMPLETE :
		POSTGAMMA_TOOL_PHASE_FAILED;
	return first_error;
}


int
postgamma_tool_context_set_kernel_context(PostgammaToolContext *context,
								  void *kernel_context)
{
	if (!postgamma_tool_context_valid(context) || kernel_context == NULL ||
		context->kernel_context != NULL ||
		context->phase != POSTGAMMA_TOOL_PHASE_CREATED)
		return EINVAL;
	context->kernel_context = kernel_context;
	return 0;
}


int
postgamma_tool_context_clear_kernel_context(PostgammaToolContext *context,
									void *expected_kernel_context)
{
	if (!postgamma_tool_context_valid(context) ||
		expected_kernel_context == NULL ||
		context->kernel_context != expected_kernel_context)
		return EINVAL;
	context->kernel_context = NULL;
	return 0;
}


const char *
postgamma_tool_context_data_directory(const PostgammaToolContext *context)
{
	return postgamma_tool_context_valid(context) ? context->data_directory : NULL;
}


const char *
postgamma_tool_context_resource_root(const PostgammaToolContext *context)
{
	return postgamma_tool_context_valid(context) ? context->resource_root : NULL;
}


const char *
postgamma_tool_context_bootstrap_input(const PostgammaToolContext *context)
{
	return postgamma_tool_context_valid(context) ? context->bootstrap_input : NULL;
}


int
postgamma_tool_context_telemetry(const PostgammaToolContext *context,
								PostgammaToolTelemetry *telemetry)
{
	if (!postgamma_tool_context_valid(context) || telemetry == NULL)
		return EINVAL;
	*telemetry = (PostgammaToolTelemetry) {
		.generation = context->generation,
		.kind = context->kind,
		.phase = context->phase,
		.exit_code = context->exit_code,
		.cleanup_registered = context->cleanup_registered,
		.cleanup_completed = context->cleanup_completed,
		.cleanup_remaining = context->cleanup_count,
		.active_bindings = context->active_bindings,
		.active_kernel_contexts = context->kernel_context != NULL ? 1U : 0U,
	};
	return 0;
}


const char *
postgamma_tool_phase_name(PostgammaToolPhase phase)
{
	switch (phase)
	{
		case POSTGAMMA_TOOL_PHASE_CREATED:
			return "created";
		case POSTGAMMA_TOOL_PHASE_RUNNING:
			return "running";
		case POSTGAMMA_TOOL_PHASE_EXITED:
			return "exited";
		case POSTGAMMA_TOOL_PHASE_CLEANING:
			return "cleaning";
		case POSTGAMMA_TOOL_PHASE_COMPLETE:
			return "complete";
		case POSTGAMMA_TOOL_PHASE_FAILED:
			return "failed";
	}
	return "invalid";
}
