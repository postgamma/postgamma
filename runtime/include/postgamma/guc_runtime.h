/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * guc_runtime.h
 *    Runtime API for PostgreSQL GUC state virtualization.
 *
 * PostgreSQL-dependent fields and slots are generated in guc_state_layout.h.
 * This file contains the stable execution-context contract.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_GUC_RUNTIME_H
#define POSTGAMMA_GUC_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "postgamma/backend_state_runtime.h"
#include "postgamma/contract_runtime.h"


#ifndef POSTGAMMA_THREAD_LOCAL
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define POSTGAMMA_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define POSTGAMMA_THREAD_LOCAL __thread
#else
#error "postgamma requires compiler-supported thread-local storage"
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#define POSTGAMMA_UNLIKELY(condition) __builtin_expect(!!(condition), 0)
#else
#define POSTGAMMA_UNLIKELY(condition) (condition)
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct config_generic;
struct PostgammaInstanceRuntime;
struct PostgammaPathRuntime;

typedef void (*PostgammaExecutionExitHandler) (void *argument, int code);

#define POSTGAMMA_INSTANCE_CONTEXT_MAGIC UINT64_C(0x5047494E5354414E)
#define POSTGAMMA_ROLE_CONTEXT_MAGIC UINT64_C(0x5047524F4C454354)
#define POSTGAMMA_EXECUTION_CONTEXT_MAGIC UINT64_C(0x5047455845434354)

typedef enum PostgammaGucOwner
{
	POSTGAMMA_GUC_OWNER_IMMUTABLE = 0,
	POSTGAMMA_GUC_OWNER_INSTANCE,
	POSTGAMMA_GUC_OWNER_ROLE,
	POSTGAMMA_GUC_OWNER_SESSION
} PostgammaGucOwner;

/* Generated GUC state fields, slot enums, and typed access macros. */
#include "postgamma/guc_state_layout.h"

typedef struct PostgammaInstanceContext
{
	uint64_t	magic;
	struct PostgammaInstanceRuntime *runtime;
	struct PostgammaPathRuntime *path_runtime;
	void	   *postgres_backend_state;
	bool		owns_postgres_backend_state;
	PostgammaImmutableGucState immutable_guc;
	PostgammaInstanceGucState instance_guc;
} PostgammaInstanceContext;

typedef struct PostgammaRoleContext
{
	uint64_t	magic;
	PostgammaInstanceContext *instance;
	PostgammaRoleGucState guc;
	void	   *postgres_backend_state;
	void	   *extension_role_state;
} PostgammaRoleContext;

typedef struct PostgammaExecutionContext
{
	uint64_t	magic;
	PostgammaInstanceContext *instance;
	PostgammaRoleContext *role;
	PostgammaSessionGucState session_guc;
	void	   *postgres_guc_control;
	void	   *postgres_backend_state;
	void	   *postgres_extension_state;
	uint64_t	connection_id;
	PostgammaExecutionExitHandler exit_handler;
	void	   *exit_handler_argument;
} PostgammaExecutionContext;

extern POSTGAMMA_THREAD_LOCAL PostgammaExecutionContext *PostgammaCurrentExecutionContext;

int postgamma_instance_context_init(PostgammaInstanceContext *context);
void postgamma_instance_context_init_shared(
	PostgammaInstanceContext *context,
	PostgammaInstanceContext *owner);
void postgamma_instance_context_destroy(PostgammaInstanceContext *context);
void postgamma_instance_context_attach_runtime(
	PostgammaInstanceContext *context,
	struct PostgammaInstanceRuntime *runtime);
void postgamma_instance_context_detach_runtime(
	PostgammaInstanceContext *context,
	struct PostgammaInstanceRuntime *runtime);
struct PostgammaInstanceRuntime *postgamma_instance_context_runtime(
	const PostgammaInstanceContext *context);
void postgamma_instance_context_attach_path_runtime(
	PostgammaInstanceContext *context,
	struct PostgammaPathRuntime *path_runtime);
void postgamma_instance_context_detach_path_runtime(
	PostgammaInstanceContext *context,
	struct PostgammaPathRuntime *path_runtime);
struct PostgammaPathRuntime *postgamma_instance_context_path_runtime(
	const PostgammaInstanceContext *context);
int postgamma_role_context_init(PostgammaRoleContext *context,
								PostgammaInstanceContext *instance);
void postgamma_role_context_destroy(PostgammaRoleContext *context);
int postgamma_execution_context_init(PostgammaExecutionContext *context,
								 PostgammaInstanceContext *instance,
								 PostgammaRoleContext *role);
void postgamma_execution_context_destroy(PostgammaExecutionContext *context);
PostgammaExecutionContext *postgamma_execution_context_bind(
	PostgammaExecutionContext *context);
void postgamma_execution_context_restore(
	PostgammaExecutionContext *expected_current,
	PostgammaExecutionContext *previous);
PostgammaExecutionContext *postgamma_execution_context_current(void);
void postgamma_execution_context_set_exit_handler(
	PostgammaExecutionContext *context,
	PostgammaExecutionExitHandler handler,
	void *argument);
bool postgamma_execution_context_dispatch_exit(int code);
void postgamma_execution_context_set_connection_id(
	PostgammaExecutionContext *context, uint64_t connection_id);
uint64_t postgamma_execution_context_connection_id(
	const PostgammaExecutionContext *context);
void *postgamma_guc_slot_address(PostgammaGucSlot slot);
void *postgamma_guc_control_slot_address(PostgammaGucControlSlot slot);

/* PostgreSQL integration anchors. */
void postgamma_server_execution_context_bootstrap(void);
void postgamma_initialize_builtin_guc_values(void);
void postgamma_bind_builtin_guc_variables(void);
void postgamma_seed_inherited_guc_shadow(
	PostgammaInstanceContext *shared_instance);
bool postgamma_guc_state_requires_transfer(
	const struct config_generic *guc);
void postgamma_destroy_builtin_guc_variables(PostgammaExecutionContext *execution);

static inline PostgammaExecutionContext *
postgamma_execution_context_require(void)
{
	PostgammaExecutionContext *context = PostgammaCurrentExecutionContext;

	if (POSTGAMMA_UNLIKELY(context == NULL ||
						 context->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC))
		postgamma_runtime_contract_violation();
	return context;
}

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_GUC_RUNTIME_H */
