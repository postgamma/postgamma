/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * execution_context.c
 *    Current-execution-context runtime shared by generated state families.
 *
 *-------------------------------------------------------------------------
 */

#include "postgamma/guc_runtime.h"

#include <stdlib.h>
#include <string.h>


POSTGAMMA_THREAD_LOCAL PostgammaExecutionContext *PostgammaCurrentExecutionContext = NULL;


void
postgamma_runtime_contract_violation(void)
{
	abort();
}


int
postgamma_instance_context_init(PostgammaInstanceContext *context)
{
	void	   *backend_state = NULL;
	int			status;

	if (context == NULL)
		postgamma_runtime_contract_violation();
	memset(context, 0, sizeof(*context));
	status = postgamma_backend_state_context_init(
		POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE, &backend_state);
	if (status != 0)
		return status;
	context->magic = POSTGAMMA_INSTANCE_CONTEXT_MAGIC;
	context->postgres_backend_state = backend_state;
	context->owns_postgres_backend_state = true;
	return 0;
}


void
postgamma_instance_context_init_shared(
	PostgammaInstanceContext *context,
	PostgammaInstanceContext *owner)
{
	if (context == NULL || owner == NULL || context == owner ||
		owner->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC ||
		!owner->owns_postgres_backend_state ||
		owner->postgres_backend_state == NULL)
		postgamma_runtime_contract_violation();
	memset(context, 0, sizeof(*context));
	context->magic = POSTGAMMA_INSTANCE_CONTEXT_MAGIC;
	context->postgres_backend_state = owner->postgres_backend_state;
}


void
postgamma_instance_context_destroy(PostgammaInstanceContext *context)
{
	if (context == NULL || context->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC)
		postgamma_runtime_contract_violation();
	if (PostgammaCurrentExecutionContext != NULL &&
		PostgammaCurrentExecutionContext->instance == context)
		postgamma_runtime_contract_violation();
	if (context->runtime != NULL || context->path_runtime != NULL)
		postgamma_runtime_contract_violation();
	if (context->owns_postgres_backend_state)
		postgamma_backend_state_context_destroy(
			&context->postgres_backend_state);
	else
		context->postgres_backend_state = NULL;
	memset(context, 0, sizeof(*context));
}


void
postgamma_instance_context_attach_runtime(
	PostgammaInstanceContext *context,
	struct PostgammaInstanceRuntime *runtime)
{
	if (context == NULL || context->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC ||
		runtime == NULL || context->runtime != NULL)
		postgamma_runtime_contract_violation();
	context->runtime = runtime;
}


void
postgamma_instance_context_detach_runtime(
	PostgammaInstanceContext *context,
	struct PostgammaInstanceRuntime *runtime)
{
	if (context == NULL || context->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC ||
		runtime == NULL || context->runtime != runtime)
		postgamma_runtime_contract_violation();
	context->runtime = NULL;
}


struct PostgammaInstanceRuntime *
postgamma_instance_context_runtime(const PostgammaInstanceContext *context)
{
	if (context == NULL || context->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC)
		postgamma_runtime_contract_violation();
	return context->runtime;
}


void
postgamma_instance_context_attach_path_runtime(
	PostgammaInstanceContext *context,
	struct PostgammaPathRuntime *path_runtime)
{
	if (context == NULL || context->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC ||
		path_runtime == NULL || context->path_runtime != NULL)
		postgamma_runtime_contract_violation();
	context->path_runtime = path_runtime;
}


void
postgamma_instance_context_detach_path_runtime(
	PostgammaInstanceContext *context,
	struct PostgammaPathRuntime *path_runtime)
{
	if (context == NULL || context->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC ||
		path_runtime == NULL || context->path_runtime != path_runtime)
		postgamma_runtime_contract_violation();
	context->path_runtime = NULL;
}


struct PostgammaPathRuntime *
postgamma_instance_context_path_runtime(const PostgammaInstanceContext *context)
{
	if (context == NULL || context->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC)
		postgamma_runtime_contract_violation();
	return context->path_runtime;
}


int
postgamma_role_context_init(PostgammaRoleContext *context,
							PostgammaInstanceContext *instance)
{
	void	   *backend_state = NULL;
	int			status;

	if (context == NULL || instance == NULL ||
		instance->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC)
		postgamma_runtime_contract_violation();
	memset(context, 0, sizeof(*context));
	status = postgamma_backend_state_context_init(
		POSTGAMMA_BACKEND_STATE_OWNER_ROLE, &backend_state);
	if (status != 0)
		return status;
	context->magic = POSTGAMMA_ROLE_CONTEXT_MAGIC;
	context->instance = instance;
	context->postgres_backend_state = backend_state;
	return 0;
}


void
postgamma_role_context_destroy(PostgammaRoleContext *context)
{
	if (context == NULL || context->magic != POSTGAMMA_ROLE_CONTEXT_MAGIC)
		postgamma_runtime_contract_violation();
	if (PostgammaCurrentExecutionContext != NULL &&
		PostgammaCurrentExecutionContext->role == context)
		postgamma_runtime_contract_violation();
	postgamma_extension_role_state_destroy(&context->extension_role_state);
	postgamma_backend_state_context_destroy(&context->postgres_backend_state);
	memset(context, 0, sizeof(*context));
}


int
postgamma_execution_context_init(PostgammaExecutionContext *context,
								 PostgammaInstanceContext *instance,
								 PostgammaRoleContext *role)
{
	void	   *backend_state = NULL;
	int			status;

	if (context == NULL || instance == NULL || role == NULL ||
		instance->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC ||
		role->magic != POSTGAMMA_ROLE_CONTEXT_MAGIC ||
		role->instance != instance)
		postgamma_runtime_contract_violation();

	memset(context, 0, sizeof(*context));
	status = postgamma_backend_state_context_init(
		POSTGAMMA_BACKEND_STATE_OWNER_SESSION, &backend_state);
	if (status != 0)
		return status;
	context->magic = POSTGAMMA_EXECUTION_CONTEXT_MAGIC;
	context->instance = instance;
	context->role = role;
	context->postgres_backend_state = backend_state;
	return 0;
}


void
postgamma_execution_context_destroy(PostgammaExecutionContext *context)
{
	if (context == NULL || context->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC ||
		context == PostgammaCurrentExecutionContext ||
		context->postgres_guc_control != NULL || context->exit_handler != NULL ||
		context->exit_handler_argument != NULL ||
		context->postgres_extension_state != NULL)
		postgamma_runtime_contract_violation();
	postgamma_backend_state_context_destroy(&context->postgres_backend_state);
	memset(context, 0, sizeof(*context));
}


void
postgamma_execution_context_set_connection_id(
	PostgammaExecutionContext *context, uint64_t connection_id)
{
	if (context == NULL || context->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC ||
		context->connection_id != 0 || connection_id == 0)
		postgamma_runtime_contract_violation();
	context->connection_id = connection_id;
}


uint64_t
postgamma_execution_context_connection_id(
	const PostgammaExecutionContext *context)
{
	if (context == NULL || context->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC)
		postgamma_runtime_contract_violation();
	return context->connection_id;
}


PostgammaExecutionContext *
postgamma_execution_context_bind(PostgammaExecutionContext *context)
{
	PostgammaExecutionContext *previous = PostgammaCurrentExecutionContext;

	if (context == NULL || context->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC ||
		context->instance == NULL ||
		context->instance->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC ||
		context->role == NULL ||
		context->role->magic != POSTGAMMA_ROLE_CONTEXT_MAGIC ||
		context->role->instance != context->instance)
		postgamma_runtime_contract_violation();
	PostgammaCurrentExecutionContext = context;
	return previous;
}


void
postgamma_execution_context_restore(PostgammaExecutionContext *expected_current,
									PostgammaExecutionContext *previous)
{
	if (PostgammaCurrentExecutionContext != expected_current)
		postgamma_runtime_contract_violation();
	if (previous != NULL &&
		(previous->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC ||
		 previous->instance == NULL || previous->role == NULL ||
		 previous->role->instance != previous->instance))
		postgamma_runtime_contract_violation();
	PostgammaCurrentExecutionContext = previous;
}


PostgammaExecutionContext *
postgamma_execution_context_current(void)
{
	return PostgammaCurrentExecutionContext;
}


void
postgamma_execution_context_set_exit_handler(
	PostgammaExecutionContext *context,
	PostgammaExecutionExitHandler handler,
	void *argument)
{
	if (context == NULL || context->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC ||
		(handler != NULL &&
		 (argument == NULL || context->exit_handler != NULL)) ||
		(handler == NULL &&
		 (argument != NULL || context->exit_handler == NULL)))
		postgamma_runtime_contract_violation();
	context->exit_handler = handler;
	context->exit_handler_argument = argument;
}


bool
postgamma_execution_context_dispatch_exit(int code)
{
	PostgammaExecutionContext *context = PostgammaCurrentExecutionContext;
	PostgammaExecutionExitHandler handler;
	void	   *argument;

	if (context == NULL || context->exit_handler == NULL)
		return false;
	handler = context->exit_handler;
	argument = context->exit_handler_argument;
	handler(argument, code);
	postgamma_runtime_contract_violation();
	return true;
}
