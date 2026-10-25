/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/* Diagnostic link fixture; see README.md for the five --wrap options. */
#include "postgamma/backend_state_runtime.h"
#include "postgamma/guc_runtime.h"
#include "postgamma/instance_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
fail_bootstrap(const char *stage)
{
	const char *requested = getenv("POSTGAMMA_TEST_BOOTSTRAP_FAILURE");

	if (requested == NULL || strcmp(requested, stage) != 0)
		return 0;
	if (postgamma_execution_context_current() != NULL)
		abort();
	fprintf(stderr, "POSTGAMMA_TEST_BOOTSTRAP_FAILURE %s\n", stage);
	return 1;
}

int __real_postgamma_instance_runtime_create(PostgammaInstanceRuntime **runtime,
	const PostgammaInstanceRuntimeOptions *options);
int __wrap_postgamma_instance_runtime_create(PostgammaInstanceRuntime **runtime,
	const PostgammaInstanceRuntimeOptions *options);
int __real_postgamma_instance_context_init(PostgammaInstanceContext *context);
int __wrap_postgamma_instance_context_init(PostgammaInstanceContext *context);
int __real_postgamma_role_context_init(PostgammaRoleContext *context,
	PostgammaInstanceContext *instance);
int __wrap_postgamma_role_context_init(PostgammaRoleContext *context,
	PostgammaInstanceContext *instance);
int __real_postgamma_execution_context_init(PostgammaExecutionContext *context,
	PostgammaInstanceContext *instance, PostgammaRoleContext *role);
int __wrap_postgamma_execution_context_init(PostgammaExecutionContext *context,
	PostgammaInstanceContext *instance, PostgammaRoleContext *role);
int __real_postgamma_backend_state_context_prepare(void *storage);
int __wrap_postgamma_backend_state_context_prepare(void *storage);

int
__wrap_postgamma_instance_runtime_create(PostgammaInstanceRuntime **runtime,
	const PostgammaInstanceRuntimeOptions *options)
{
	return fail_bootstrap("instance-runtime") ? ENOMEM :
		__real_postgamma_instance_runtime_create(runtime, options);
}

int
__wrap_postgamma_instance_context_init(PostgammaInstanceContext *context)
{
	return fail_bootstrap("instance-context") ? ENOMEM :
		__real_postgamma_instance_context_init(context);
}

int
__wrap_postgamma_role_context_init(PostgammaRoleContext *context,
	PostgammaInstanceContext *instance)
{
	return fail_bootstrap("role-context") ? ENOMEM :
		__real_postgamma_role_context_init(context, instance);
}

int
__wrap_postgamma_execution_context_init(PostgammaExecutionContext *context,
	PostgammaInstanceContext *instance, PostgammaRoleContext *role)
{
	return fail_bootstrap("execution-context") ? ENOMEM :
		__real_postgamma_execution_context_init(context, instance, role);
}

int
__wrap_postgamma_backend_state_context_prepare(void *storage)
{
	return fail_bootstrap("role-preparation") ? ENOMEM :
		__real_postgamma_backend_state_context_prepare(storage);
}
