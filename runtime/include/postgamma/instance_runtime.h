/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * instance_runtime.h
 *    Instance-owned runtime state shared by server and embedded profiles.
 *
 * PostgreSQL-facing code reaches backend registries and process policy
 * through this owner.  No mutable database state in this interface is
 * process-global.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_INSTANCE_RUNTIME_H
#define POSTGAMMA_INSTANCE_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "postgamma/backend_execution_runtime.h"
#include "postgamma/embedded_kernel.h"


typedef enum PostgammaRuntimeProfile
{
	POSTGAMMA_RUNTIME_PROFILE_INVALID = 0,
	POSTGAMMA_RUNTIME_PROFILE_THREADED_SERVER,
	POSTGAMMA_RUNTIME_PROFILE_EMBEDDED
} PostgammaRuntimeProfile;

typedef struct PostgammaInstanceRuntime PostgammaInstanceRuntime;
typedef struct PostgammaCheckpointTracker PostgammaCheckpointTracker;

typedef int (*PostgammaInstanceWakeNotify) (
	void *argument, uint64_t generation);
typedef void (*PostgammaCheckpointCompletionFunction) (
	void *argument, uint64_t generation, int operation_status);

#define POSTGAMMA_INSTANCE_SIGNAL_MAX 64

typedef struct PostgammaInstanceRuntimeOptions
{
	uint32_t	struct_size;
	/* Zero allocates a runtime-local token; embedded kernels supply their own. */
	uint64_t	generation;
	PostgammaRuntimeProfile profile;
	PostgammaBackendProviderConfig backend_provider;
	PostgammaInstanceWakeNotify wake_notification;
	void	   *wake_notification_argument;
	PostgammaKernelEmitLogFunction emit_log;
	void	   *log_argument;
} PostgammaInstanceRuntimeOptions;

#define POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT \
	{sizeof(PostgammaInstanceRuntimeOptions), UINT64_C(0), \
	 POSTGAMMA_RUNTIME_PROFILE_EMBEDDED, \
	 {POSTGAMMA_BACKEND_PROVIDER_DEDICATED, 0, 0, 0}, NULL, NULL, NULL, NULL}

typedef struct PostgammaInstanceRuntimeTelemetry
{
	uint64_t	generation;
	PostgammaRuntimeProfile profile;
	bool		bundled_extensions_active;
	bool		backend_registry_created;
	uint32_t	backend_capacity;
	uint64_t	completion_notifications;
	uint64_t	supervisor_signal_notifications;
	uint64_t	pending_supervisor_signals;
	uint64_t	external_processes_started;
	uint64_t	external_processes_finished;
	uint64_t	external_process_launch_rejections;
	uint64_t	external_process_reservations;
	uint64_t	external_processes_active;
	PostgammaBackendTelemetry backend;
} PostgammaInstanceRuntimeTelemetry;

int postgamma_instance_runtime_create(
	PostgammaInstanceRuntime **runtime,
	const PostgammaInstanceRuntimeOptions *options);
int postgamma_instance_runtime_destroy(PostgammaInstanceRuntime *runtime);
uint64_t postgamma_instance_runtime_generation(
	const PostgammaInstanceRuntime *runtime);
PostgammaRuntimeProfile postgamma_instance_runtime_profile(
	const PostgammaInstanceRuntime *runtime);
int postgamma_instance_runtime_activate_bundled_extensions(
	PostgammaInstanceRuntime *runtime);
bool postgamma_instance_runtime_bundled_extensions_active(
	const PostgammaInstanceRuntime *runtime);
int postgamma_instance_runtime_ensure_backend_registry(
	PostgammaInstanceRuntime *runtime,
	uint32_t capacity,
	PostgammaBackendRegistry **registry);
PostgammaBackendRegistry *postgamma_instance_runtime_backend_registry(
	PostgammaInstanceRuntime *runtime);
int postgamma_instance_runtime_reserve_external_process(
	PostgammaInstanceRuntime *runtime);
int postgamma_instance_runtime_commit_external_process(
	PostgammaInstanceRuntime *runtime);
int postgamma_instance_runtime_cancel_external_process(
	PostgammaInstanceRuntime *runtime);
int postgamma_instance_runtime_record_external_process_finish(
	PostgammaInstanceRuntime *runtime);
int postgamma_instance_runtime_external_process_count(
	PostgammaInstanceRuntime *runtime,
	uint64_t *count);
int postgamma_instance_runtime_notify_completion(
	PostgammaInstanceRuntime *runtime);
/* Signals are coalesced by number and never delivered to the host process. */
int postgamma_instance_runtime_notify_supervisor_signal(
	PostgammaInstanceRuntime *runtime,
	uint64_t generation,
	int signal_number);
int postgamma_instance_runtime_take_supervisor_signals(
	PostgammaInstanceRuntime *runtime,
	uint64_t generation,
	uint64_t *pending_signals);
int postgamma_instance_runtime_telemetry(
	PostgammaInstanceRuntime *runtime,
	PostgammaInstanceRuntimeTelemetry *telemetry);
int postgamma_instance_runtime_emit_log(
	PostgammaInstanceRuntime *runtime,
	const PostgammaKernelLogRecord *record);
int postgamma_instance_checkpoint_tracker_create(
	PostgammaInstanceRuntime *runtime,
	uint64_t generation,
	PostgammaCheckpointCompletionFunction completion,
	void *completion_argument,
	PostgammaCheckpointTracker **tracker);
/* This call is allocation-free and safe while PostgreSQL holds ckpt_lck. */
int postgamma_instance_checkpoint_tracker_arm(
	PostgammaCheckpointTracker *tracker,
	uint64_t generation,
	int32_t baseline_started,
	int32_t baseline_failed);
int postgamma_instance_checkpoint_tracker_finish(
	PostgammaCheckpointTracker *tracker,
	uint64_t generation,
	int operation_status);
int postgamma_instance_checkpoint_observe(
	PostgammaInstanceRuntime *runtime,
	uint64_t generation,
	int32_t checkpoint_done,
	int32_t checkpoint_failed);
int postgamma_instance_checkpoint_tracker_waitable_fd(
	const PostgammaCheckpointTracker *tracker);
int postgamma_instance_checkpoint_tracker_wake(
	PostgammaCheckpointTracker *tracker);
int postgamma_instance_checkpoint_tracker_wake_drain(
	PostgammaCheckpointTracker *tracker,
	uint64_t *wake_count);
int postgamma_instance_checkpoint_tracker_destroy(
	PostgammaCheckpointTracker *tracker);

#endif /* POSTGAMMA_INSTANCE_RUNTIME_H */
