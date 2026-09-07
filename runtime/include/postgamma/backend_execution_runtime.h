/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * backend_execution_runtime.h
 *    Thread registry for PostgreSQL backend executions.
 *
 * A backend execution is a logical server participant such as a client
 * backend, checkpointer, WAL writer, or parallel worker. It is not a SQL
 * role and its identity is independent of the native thread handle.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_BACKEND_EXECUTION_RUNTIME_H
#define POSTGAMMA_BACKEND_EXECUTION_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "postgamma/thread_runtime.h"


#define POSTGAMMA_BACKEND_SIGNAL_COUNT 64
#define POSTGAMMA_COMPAT_PID_FIRST INT32_C(1000000000)

typedef uint64_t PostgammaBackendId;
typedef int32_t PostgammaCompatPid;

typedef enum PostgammaBackendClass
{
	POSTGAMMA_BACKEND_CLASS_DEDICATED = 0,
	POSTGAMMA_BACKEND_CLASS_CLIENT,
	POSTGAMMA_BACKEND_CLASS_PARALLEL,
	POSTGAMMA_BACKEND_CLASS_COUNT
} PostgammaBackendClass;

typedef enum PostgammaBackendProviderKind
{
	POSTGAMMA_BACKEND_PROVIDER_DEDICATED = 0,
	POSTGAMMA_BACKEND_PROVIDER_POOLED,
	POSTGAMMA_BACKEND_PROVIDER_COUNT
} PostgammaBackendProviderKind;

typedef struct PostgammaBackendProviderConfig
{
	PostgammaBackendProviderKind kind;
	uint32_t	worker_count;
	uint32_t	queue_capacity;
	uint32_t	execution_token_count;
} PostgammaBackendProviderConfig;

typedef enum PostgammaBackendExitKind
{
	POSTGAMMA_BACKEND_EXIT_NORMAL = 0,
	POSTGAMMA_BACKEND_EXIT_YIELD,
	POSTGAMMA_BACKEND_EXIT_FATAL,
	POSTGAMMA_BACKEND_EXIT_STARTUP_TARGET,
	POSTGAMMA_BACKEND_EXIT_QUICKDIE,
	POSTGAMMA_BACKEND_EXIT_PANIC,
	POSTGAMMA_BACKEND_EXIT_UNEXPECTED_RETURN
} PostgammaBackendExitKind;

typedef enum PostgammaBackendSlotState
{
	POSTGAMMA_BACKEND_SLOT_FREE = 0,
	POSTGAMMA_BACKEND_SLOT_STARTING,
	POSTGAMMA_BACKEND_SLOT_RUNNING,
	POSTGAMMA_BACKEND_SLOT_COMPLETED,
	POSTGAMMA_BACKEND_SLOT_DEQUEUED,
	POSTGAMMA_BACKEND_SLOT_JOINING
} PostgammaBackendSlotState;

typedef struct PostgammaBackendHandle
{
	PostgammaBackendId id;
	PostgammaCompatPid compat_pid;
	int			backend_type;
	PostgammaBackendClass execution_class;
} PostgammaBackendHandle;

typedef struct PostgammaBackendExitStatus
{
	PostgammaBackendExitKind kind;
	int			code;
} PostgammaBackendExitStatus;

typedef struct PostgammaBackendCompletion
{
	PostgammaBackendHandle handle;
	PostgammaBackendExitStatus exit_status;
} PostgammaBackendCompletion;

typedef struct PostgammaBackendStartInfo
{
	PostgammaBackendHandle handle;
	PostgammaWakeTarget *wake_target;
	uint32_t	carrier_index;
	uint64_t	quantum_sequence;
	int			stack_status;
	PostgammaThreadStackInfo stack_info;
} PostgammaBackendStartInfo;

typedef PostgammaBackendExitStatus (*PostgammaBackendMain) (
	const PostgammaBackendStartInfo *start_info,
	const void *startup_data,
	size_t startup_data_length,
	void *main_argument);
typedef int (*PostgammaBackendCleanup) (
	const PostgammaBackendStartInfo *start_info,
	void *cleanup_argument);
typedef void (*PostgammaBackendCompletionNotify) (void *notification_argument);

typedef struct PostgammaBackendLaunchRequest
{
	int			backend_type;
	PostgammaBackendClass execution_class;
	const char *thread_name;
	size_t		stack_size;
	size_t		guard_size;
	const void *startup_data;
	size_t		startup_data_length;
	PostgammaBackendMain main_function;
	void	   *main_argument;
	PostgammaBackendCleanup cleanup_function;
	void	   *cleanup_argument;
	int			native_wake_signal;
	PostgammaBackendCompletionNotify completion_notification;
	void	   *completion_notification_argument;
} PostgammaBackendLaunchRequest;

typedef struct PostgammaBackendStackTelemetry
{
	uint64_t	observations;
	uint64_t	observation_failures;
	uint64_t	depth_validations;
	uint64_t	depth_validation_failures;
	uint64_t	accounting_failures;
	uint64_t	active_reservation_bytes;
	uint64_t	peak_reservation_bytes;
	size_t		minimum_configured_stack_size;
	size_t		maximum_configured_stack_size;
	size_t		minimum_configured_guard_size;
	size_t		minimum_native_stack_size;
	size_t		minimum_native_guard_size;
	size_t		minimum_usable_stack_size;
	size_t		minimum_depth_limit;
	size_t		maximum_configured_depth;
} PostgammaBackendStackTelemetry;

typedef struct PostgammaBackendTelemetry
{
	PostgammaBackendProviderKind provider_kind;
	bool		threads_started;
	uint64_t	backend_process_launches;
	uint64_t	forbidden_process_launch_attempts;
	uint64_t	unsupported_backend_requests;
	uint64_t	client_threads_started;
	uint64_t	client_threads_peak;
	uint64_t	parallel_threads_started;
	uint64_t	dedicated_threads_started;
	uint64_t	backend_completions;
	uint64_t	backend_threads_active;
	uint64_t	pooled_worker_threads;
	uint64_t	client_quantums;
	uint64_t	quantum_yields;
	uint64_t	carrier_migrations;
	uint64_t	runnable_sessions;
	uint64_t	runnable_sessions_peak;
	uint64_t	running_quantums;
	uint64_t	running_quantums_peak;
	uint64_t	pinned_sessions;
	uint64_t	pinned_sessions_peak;
	uint64_t	blocked_sessions;
	uint64_t	blocked_sessions_peak;
	uint64_t	queue_wait_ns_total;
	uint64_t	queue_wait_ns_max;
	uint64_t	work_steals;
	uint64_t	execution_tokens_active;
	uint64_t	execution_tokens_peak;
	uint64_t	execution_token_budget;
	uint64_t	execution_token_rejections;
	PostgammaBackendStackTelemetry stack[POSTGAMMA_BACKEND_CLASS_COUNT];
} PostgammaBackendTelemetry;

typedef struct PostgammaBackendRegistry PostgammaBackendRegistry;

int postgamma_backend_registry_create(PostgammaBackendRegistry **registry,
									  uint32_t capacity);
int postgamma_backend_registry_create_with_provider(
	PostgammaBackendRegistry **registry, uint32_t capacity,
	const PostgammaBackendProviderConfig *provider_config);
int postgamma_backend_registry_destroy(PostgammaBackendRegistry *registry);
PostgammaBackendProviderKind postgamma_backend_registry_provider_kind(
	const PostgammaBackendRegistry *registry);
PostgammaThreadRole postgamma_backend_thread_role(
	PostgammaBackendClass backend_class);
int postgamma_backend_launch(PostgammaBackendRegistry *registry,
						 const PostgammaBackendLaunchRequest *request,
						 PostgammaBackendHandle *handle);
int postgamma_backend_completion_pop(PostgammaBackendRegistry *registry,
									 PostgammaBackendCompletion *completion);
int postgamma_backend_completion_join(PostgammaBackendRegistry *registry,
									  const PostgammaBackendCompletion *completion);
int postgamma_backend_registry_wake_fd(const PostgammaBackendRegistry *registry);
uint32_t postgamma_backend_registry_active_count(PostgammaBackendRegistry *registry);
int postgamma_backend_registry_lookup(PostgammaBackendRegistry *registry,
								  PostgammaBackendId id,
								  PostgammaBackendHandle *handle,
								  PostgammaBackendSlotState *state);
int postgamma_backend_signal(PostgammaBackendRegistry *registry,
						 PostgammaCompatPid compat_pid,
						 int signal_number);
int postgamma_backend_pause(PostgammaBackendRegistry *registry,
						PostgammaCompatPid compat_pid);
int postgamma_backend_resume(PostgammaBackendRegistry *registry,
						 PostgammaCompatPid compat_pid);
int postgamma_backend_wait_while_paused(PostgammaBackendRegistry *registry,
									   PostgammaBackendId id);
int postgamma_backend_wake(PostgammaBackendRegistry *registry,
					   PostgammaCompatPid compat_pid);
int postgamma_backend_schedule(PostgammaBackendRegistry *registry,
						   PostgammaBackendId id);
int postgamma_backend_notify(PostgammaBackendRegistry *registry,
						 PostgammaBackendId id, bool runnable);
int postgamma_backend_schedule_epoch(PostgammaBackendRegistry *registry,
								 PostgammaBackendId id, uint64_t *epoch);
int postgamma_backend_prepare_yield(PostgammaBackendRegistry *registry,
								PostgammaBackendId id,
								uint64_t observed_schedule_epoch,
								bool runnable);
int postgamma_backend_set_pinned(PostgammaBackendRegistry *registry,
							 PostgammaBackendId id, bool pinned);
int postgamma_backend_set_blocked(PostgammaBackendRegistry *registry,
							  PostgammaBackendId id, bool blocked);
int postgamma_backend_set_deadline(PostgammaBackendRegistry *registry,
							   PostgammaBackendId id, uint64_t deadline_ns);
int postgamma_backend_take_pending_signals(PostgammaBackendRegistry *registry,
										 PostgammaBackendId id,
										 uint64_t *pending);
int postgamma_backend_requeue_pending_signals(PostgammaBackendRegistry *registry,
										PostgammaBackendId id,
										uint64_t pending);
void postgamma_backend_registry_record_process_launch(
	PostgammaBackendRegistry *registry);
void postgamma_backend_registry_record_forbidden_process_launch(
	PostgammaBackendRegistry *registry);
void postgamma_backend_registry_record_unsupported_request(
	PostgammaBackendRegistry *registry);
int postgamma_backend_registry_record_stack_validation(
	PostgammaBackendRegistry *registry, PostgammaBackendId id,
	size_t configured_depth, size_t available_depth, bool valid);
int postgamma_backend_registry_telemetry(PostgammaBackendRegistry *registry,
										PostgammaBackendTelemetry *telemetry);

#endif
