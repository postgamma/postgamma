/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_SUPERVISOR_H
#define POSTGAMMA_PRIVATE_SUPERVISOR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POSTGAMMA_SUPERVISOR_NO_DEADLINE UINT64_MAX

typedef enum PostgammaSupervisorState
{
	POSTGAMMA_SUPERVISOR_STATE_NEW = 0,
	POSTGAMMA_SUPERVISOR_STATE_STARTING,
	POSTGAMMA_SUPERVISOR_STATE_RECOVERING,
	POSTGAMMA_SUPERVISOR_STATE_READY,
	POSTGAMMA_SUPERVISOR_STATE_QUIESCING,
	POSTGAMMA_SUPERVISOR_STATE_STOPPING,
	POSTGAMMA_SUPERVISOR_STATE_CLOSED,
	POSTGAMMA_SUPERVISOR_STATE_FAILED
} PostgammaSupervisorState;

typedef enum PostgammaSupervisorControlKind
{
	POSTGAMMA_SUPERVISOR_CONTROL_NONE = -1,
	POSTGAMMA_SUPERVISOR_CONTROL_CONNECT = 0,
	POSTGAMMA_SUPERVISOR_CONTROL_CANCEL,
	POSTGAMMA_SUPERVISOR_CONTROL_DISCONNECT,
	POSTGAMMA_SUPERVISOR_CONTROL_RELOAD,
	POSTGAMMA_SUPERVISOR_CONTROL_ROLE_COMPLETION,
	POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN,
	POSTGAMMA_SUPERVISOR_CONTROL_CHECKPOINT
} PostgammaSupervisorControlKind;

typedef enum PostgammaSupervisorShutdownMode
{
	POSTGAMMA_SUPERVISOR_SHUTDOWN_SMART = 0,
	POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST,
	POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE
} PostgammaSupervisorShutdownMode;

typedef enum PostgammaSupervisorFailureOrigin
{
	POSTGAMMA_SUPERVISOR_FAILURE_NONE = 0,
	POSTGAMMA_SUPERVISOR_FAILURE_THREAD_START,
	POSTGAMMA_SUPERVISOR_FAILURE_WAIT,
	POSTGAMMA_SUPERVISOR_FAILURE_BOOT,
	POSTGAMMA_SUPERVISOR_FAILURE_RUN,
	POSTGAMMA_SUPERVISOR_FAILURE_SHUTDOWN,
	POSTGAMMA_SUPERVISOR_FAILURE_HOST_FAIL_STOP,
	POSTGAMMA_SUPERVISOR_FAILURE_CONTRACT
} PostgammaSupervisorFailureOrigin;

typedef struct PostgammaSupervisor PostgammaSupervisor;
typedef struct PostgammaSupervisorTicket PostgammaSupervisorTicket;

typedef int (*PostgammaSupervisorBootFunction) (
	PostgammaSupervisor *supervisor,
	void *argument);
typedef int (*PostgammaSupervisorRunFunction) (
	PostgammaSupervisor *supervisor,
	void *argument);
typedef int (*PostgammaSupervisorDispatchFunction) (
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorControlKind kind,
	void *payload,
	void *argument);
typedef int (*PostgammaSupervisorShutdownFunction) (
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorShutdownMode mode,
	void *argument);

typedef struct PostgammaSupervisorCallbacks
{
	PostgammaSupervisorBootFunction boot;
	PostgammaSupervisorRunFunction run;
	PostgammaSupervisorDispatchFunction dispatch;
	PostgammaSupervisorShutdownFunction shutdown;
} PostgammaSupervisorCallbacks;

typedef struct PostgammaSupervisorOptions
{
	uint32_t	struct_size;
	uint64_t	generation;
	size_t		queue_capacity;
	PostgammaSupervisorCallbacks callbacks;
	void	   *callback_argument;
} PostgammaSupervisorOptions;

#define POSTGAMMA_SUPERVISOR_OPTIONS_INIT \
	{sizeof(PostgammaSupervisorOptions), UINT64_C(0), 16, \
	 {NULL, NULL, NULL, NULL}, NULL}

typedef struct PostgammaSupervisorControl
{
	uint64_t	generation;
	uint64_t	sequence;
	PostgammaSupervisorControlKind kind;
	PostgammaSupervisorShutdownMode shutdown_mode;
	void	   *payload;
	void	   *private_token;
} PostgammaSupervisorControl;

typedef struct PostgammaSupervisorFailureRecord
{
	uint32_t	struct_size;
	bool		present;
	uint64_t	generation;
	uint64_t	ordinal;
	uint64_t	recorded_at_ns;
	PostgammaSupervisorFailureOrigin origin;
	PostgammaSupervisorState state_before_failure;
	PostgammaSupervisorControlKind control_kind;
	uint64_t	control_sequence;
	int			status;
	bool		process_restart_required;
	bool		supervisor_thread_exited;
	bool		supervisor_thread_joined;
} PostgammaSupervisorFailureRecord;

#define POSTGAMMA_SUPERVISOR_FAILURE_RECORD_INIT \
	{sizeof(PostgammaSupervisorFailureRecord), false, UINT64_C(0), \
	 UINT64_C(0), UINT64_C(0), POSTGAMMA_SUPERVISOR_FAILURE_NONE, \
	 POSTGAMMA_SUPERVISOR_STATE_NEW, POSTGAMMA_SUPERVISOR_CONTROL_NONE, \
	 UINT64_C(0), 0, false, false, false}

typedef struct PostgammaSupervisorTelemetry
{
	uint64_t	generation;
	PostgammaSupervisorState state;
	size_t		queue_capacity;
	size_t		queue_depth;
	size_t		queue_depth_peak;
	uint64_t	controls_submitted;
	uint64_t	controls_completed;
	uint64_t	control_notifications;
	uint64_t	stale_controls_rejected;
	uint64_t	controls_rejected_after_quiesce;
	uint64_t	shutdown_requests;
	uint64_t	threads_started;
	uint64_t	threads_joined;
	uint64_t	active_tickets;
	uint64_t	failure_attempts;
	int			failure_status;
	PostgammaSupervisorFailureRecord last_failure;
} PostgammaSupervisorTelemetry;

int postgamma_supervisor_create(
	const PostgammaSupervisorOptions *options,
	PostgammaSupervisor **supervisor);
int postgamma_supervisor_start(
	PostgammaSupervisor *supervisor,
	uint64_t deadline_ns);
int postgamma_supervisor_mark_recovering(
	PostgammaSupervisor *supervisor);
int postgamma_supervisor_mark_ready(
	PostgammaSupervisor *supervisor);
int postgamma_supervisor_submit(
	PostgammaSupervisor *supervisor,
	uint64_t generation,
	PostgammaSupervisorControlKind kind,
	void *payload,
	PostgammaSupervisorTicket **ticket);
int postgamma_supervisor_request_shutdown(
	PostgammaSupervisor *supervisor,
	uint64_t generation,
	PostgammaSupervisorShutdownMode mode,
	PostgammaSupervisorTicket **ticket);
int postgamma_supervisor_ticket_wait(
	PostgammaSupervisorTicket *ticket,
	uint64_t deadline_ns,
	int *operation_status);
int postgamma_supervisor_ticket_destroy(
	PostgammaSupervisorTicket *ticket);
int postgamma_supervisor_control_wake_fd(
	const PostgammaSupervisor *supervisor);
int postgamma_supervisor_control_wake_drain(
	PostgammaSupervisor *supervisor,
	uint64_t *wake_count);
int postgamma_supervisor_control_notify(
	PostgammaSupervisor *supervisor,
	uint64_t generation);
int postgamma_supervisor_control_take(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorControl *control);
int postgamma_supervisor_control_complete(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorControl *control,
	int operation_status);
int postgamma_supervisor_join(
	PostgammaSupervisor *supervisor,
	uint64_t deadline_ns);
int postgamma_supervisor_fail(
	PostgammaSupervisor *supervisor,
	int failure_status);
int postgamma_supervisor_fail_detailed(
	PostgammaSupervisor *supervisor,
	int failure_status,
	bool process_restart_required);
uint64_t postgamma_supervisor_generation(
	const PostgammaSupervisor *supervisor);
PostgammaSupervisorState postgamma_supervisor_state(
	PostgammaSupervisor *supervisor);
int postgamma_supervisor_telemetry(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorTelemetry *telemetry);
int postgamma_supervisor_failure_record(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorFailureRecord *record);
int postgamma_supervisor_destroy(
	PostgammaSupervisor *supervisor);
const char *postgamma_supervisor_state_name(
	PostgammaSupervisorState state);
const char *postgamma_supervisor_failure_origin_name(
	PostgammaSupervisorFailureOrigin origin);

#ifdef __cplusplus
}
#endif

#endif
