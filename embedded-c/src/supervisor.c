/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/private/supervisor.h"

#include "postgamma/thread_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define POSTGAMMA_SUPERVISOR_MAGIC UINT64_C(0x50474d5355504552)
#define POSTGAMMA_SUPERVISOR_TICKET_MAGIC UINT64_C(0x50474d5449434b54)


struct PostgammaSupervisorTicket
{
	uint64_t	magic;
	PostgammaSupervisor *supervisor;
	uint64_t	generation;
	uint64_t	sequence;
	PostgammaSupervisorControlKind kind;
	PostgammaSupervisorShutdownMode shutdown_mode;
	void	   *payload;
	int			operation_status;
	bool		completed;
};


struct PostgammaSupervisor
{
	uint64_t	magic;
	uint64_t	generation;
	PostgammaMutex *mutex;
	PostgammaCondition *condition;
	PostgammaWakeTarget *control_wake;
	PostgammaThread *thread;
	PostgammaSupervisorCallbacks callbacks;
	void	   *callback_argument;
	PostgammaSupervisorTicket **queue;
	PostgammaSupervisorTicket *inflight_ticket;
	size_t		queue_capacity;
	size_t		queue_head;
	size_t		queue_depth;
	size_t		queue_depth_peak;
	PostgammaSupervisorState state;
	uint64_t	next_sequence;
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
	bool		main_exited;
	bool		joining;
};


static bool supervisor_is_valid(const PostgammaSupervisor *supervisor);
static bool ticket_is_valid(const PostgammaSupervisorTicket *ticket);
static int lock_supervisor(PostgammaSupervisor *supervisor);
static int unlock_supervisor(PostgammaSupervisor *supervisor, int status);
static int wait_condition(
	PostgammaSupervisor *supervisor, uint64_t deadline_ns);
static int allocate_ticket(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorControlKind kind,
	void *payload,
	PostgammaSupervisorTicket **ticket);
static int enqueue_ticket(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorTicket *ticket);
static PostgammaSupervisorTicket *dequeue_ticket(
	PostgammaSupervisor *supervisor);
static void complete_ticket(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorTicket *ticket,
	int operation_status);
static void reject_queued_tickets(
	PostgammaSupervisor *supervisor,
	int operation_status);
static void set_failed(
	PostgammaSupervisor *supervisor,
	int failure_status,
	PostgammaSupervisorFailureOrigin origin,
	bool process_restart_required,
	const PostgammaSupervisorTicket *ticket);
static void *supervisor_main(void *argument);


int
postgamma_supervisor_create(
	const PostgammaSupervisorOptions *options,
	PostgammaSupervisor **supervisor)
{
	PostgammaSupervisor *created;
	int			status;

	if (options == NULL || supervisor == NULL ||
		options->struct_size != sizeof(*options) ||
		options->generation == 0 || options->queue_capacity < 2 ||
		(options->callbacks.run != NULL &&
		 (options->callbacks.dispatch != NULL ||
		  options->callbacks.shutdown != NULL)) ||
		options->queue_capacity > SIZE_MAX / sizeof(*created->queue))
		return EINVAL;
	*supervisor = NULL;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->queue = calloc(
		options->queue_capacity, sizeof(*created->queue));
	if (created->queue == NULL)
	{
		free(created);
		return ENOMEM;
	}
	status = postgamma_mutex_create(&created->mutex);
	if (status != 0)
		goto fail;
	status = postgamma_condition_create(&created->condition);
	if (status != 0)
		goto fail;
	status = postgamma_wake_target_create(&created->control_wake);
	if (status != 0)
		goto fail;
	created->magic = POSTGAMMA_SUPERVISOR_MAGIC;
	created->generation = options->generation;
	created->callbacks = options->callbacks;
	created->callback_argument = options->callback_argument;
	created->queue_capacity = options->queue_capacity;
	created->state = POSTGAMMA_SUPERVISOR_STATE_NEW;
	created->next_sequence = UINT64_C(1);
	created->last_failure = (PostgammaSupervisorFailureRecord)
		POSTGAMMA_SUPERVISOR_FAILURE_RECORD_INIT;
	*supervisor = created;
	return 0;

fail:
	if (created->control_wake != NULL)
		(void) postgamma_wake_target_destroy(created->control_wake);
	if (created->condition != NULL)
		(void) postgamma_condition_destroy(created->condition);
	if (created->mutex != NULL)
		(void) postgamma_mutex_destroy(created->mutex);
	free(created->queue);
	free(created);
	return status;
}


int
postgamma_supervisor_start(
	PostgammaSupervisor *supervisor,
	uint64_t deadline_ns)
{
	PostgammaThreadAttributes attributes = {
		.name = "pgm-supervisor",
		.role = POSTGAMMA_THREAD_ROLE_SUPERVISOR,
		.stack_size = 0,
		.guard_size = 0,
	};
	PostgammaThread *thread;
	int			status;

	if (!supervisor_is_valid(supervisor))
		return EINVAL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_NEW ||
		supervisor->thread != NULL)
		return unlock_supervisor(supervisor, EALREADY);
	supervisor->state = POSTGAMMA_SUPERVISOR_STATE_STARTING;
	status = unlock_supervisor(supervisor, 0);
	if (status != 0)
		return status;

	status = postgamma_thread_create(
		&thread, &attributes, supervisor_main, supervisor);
	if (status != 0)
	{
		int			lock_status = lock_supervisor(supervisor);

		if (lock_status != 0)
			return lock_status;
		set_failed(supervisor, status,
			POSTGAMMA_SUPERVISOR_FAILURE_THREAD_START, false, NULL);
		supervisor->last_failure.supervisor_thread_exited = true;
		supervisor->last_failure.supervisor_thread_joined = true;
		(void) postgamma_condition_broadcast(supervisor->condition);
		return unlock_supervisor(supervisor, status);
	}

	status = lock_supervisor(supervisor);
	if (status != 0)
	{
		(void) postgamma_thread_join(thread, NULL);
		(void) postgamma_thread_destroy(thread);
		return status;
	}
	supervisor->thread = thread;
	supervisor->threads_started++;
	(void) postgamma_condition_broadcast(supervisor->condition);
	while (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_STARTING ||
		   supervisor->state == POSTGAMMA_SUPERVISOR_STATE_RECOVERING)
	{
		status = wait_condition(supervisor, deadline_ns);
		if (status != 0)
			return unlock_supervisor(supervisor, status);
	}
	if (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_READY)
		return unlock_supervisor(supervisor, 0);
	status = supervisor->failure_status != 0 ?
		supervisor->failure_status : EPROTO;
	return unlock_supervisor(supervisor, status);
}


int
postgamma_supervisor_mark_recovering(PostgammaSupervisor *supervisor)
{
	int			status;

	if (!supervisor_is_valid(supervisor))
		return EINVAL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (supervisor->thread == NULL ||
		!postgamma_thread_is_current(supervisor->thread))
		return unlock_supervisor(supervisor, EPERM);
	if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_STARTING)
		return unlock_supervisor(supervisor, EPROTO);
	supervisor->state = POSTGAMMA_SUPERVISOR_STATE_RECOVERING;
	(void) postgamma_condition_broadcast(supervisor->condition);
	return unlock_supervisor(supervisor, 0);
}


int
postgamma_supervisor_mark_ready(PostgammaSupervisor *supervisor)
{
	int			status;

	if (!supervisor_is_valid(supervisor))
		return EINVAL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (supervisor->callbacks.run == NULL || supervisor->thread == NULL ||
		!postgamma_thread_is_current(supervisor->thread))
		return unlock_supervisor(supervisor, EPERM);
	if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_STARTING &&
		supervisor->state != POSTGAMMA_SUPERVISOR_STATE_RECOVERING)
		return unlock_supervisor(supervisor, EPROTO);
	supervisor->state = POSTGAMMA_SUPERVISOR_STATE_READY;
	(void) postgamma_condition_broadcast(supervisor->condition);
	return unlock_supervisor(supervisor, 0);
}


int
postgamma_supervisor_submit(
	PostgammaSupervisor *supervisor,
	uint64_t generation,
	PostgammaSupervisorControlKind kind,
	void *payload,
	PostgammaSupervisorTicket **ticket)
{
	PostgammaSupervisorTicket *created = NULL;
	int			status;

	if (!supervisor_is_valid(supervisor) || ticket == NULL ||
		kind < POSTGAMMA_SUPERVISOR_CONTROL_CONNECT ||
		kind > POSTGAMMA_SUPERVISOR_CONTROL_CHECKPOINT ||
		kind == POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN)
		return EINVAL;
	*ticket = NULL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (generation != supervisor->generation)
	{
		supervisor->stale_controls_rejected++;
		return unlock_supervisor(supervisor, ESTALE);
	}
	if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_READY)
	{
		supervisor->controls_rejected_after_quiesce++;
		return unlock_supervisor(supervisor, ESHUTDOWN);
	}
	if (supervisor->queue_depth >= supervisor->queue_capacity - 1)
		return unlock_supervisor(supervisor, EAGAIN);
	status = postgamma_wake_target_wake(supervisor->control_wake);
	if (status != 0)
		return unlock_supervisor(supervisor, status);
	status = allocate_ticket(supervisor, kind, payload, &created);
	if (status == 0)
		status = enqueue_ticket(supervisor, created);
	if (status != 0)
	{
		free(created);
		return unlock_supervisor(supervisor, status);
	}
	*ticket = created;
	(void) postgamma_condition_signal(supervisor->condition);
	return unlock_supervisor(supervisor, 0);
}


int
postgamma_supervisor_request_shutdown(
	PostgammaSupervisor *supervisor,
	uint64_t generation,
	PostgammaSupervisorShutdownMode mode,
	PostgammaSupervisorTicket **ticket)
{
	PostgammaSupervisorTicket *created = NULL;
	int			status;

	if (!supervisor_is_valid(supervisor) || ticket == NULL ||
		mode < POSTGAMMA_SUPERVISOR_SHUTDOWN_SMART ||
		mode > POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE)
		return EINVAL;
	*ticket = NULL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (generation != supervisor->generation)
	{
		supervisor->stale_controls_rejected++;
		return unlock_supervisor(supervisor, ESTALE);
	}
	if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_READY)
	{
		supervisor->controls_rejected_after_quiesce++;
		return unlock_supervisor(supervisor, ESHUTDOWN);
	}
	if (supervisor->queue_depth >= supervisor->queue_capacity)
		return unlock_supervisor(supervisor, EAGAIN);
	status = postgamma_wake_target_wake(supervisor->control_wake);
	if (status != 0)
		return unlock_supervisor(supervisor, status);
	status = allocate_ticket(
		supervisor, POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN,
		NULL, &created);
	if (status == 0)
	{
		created->shutdown_mode = mode;
		status = enqueue_ticket(supervisor, created);
	}
	if (status != 0)
	{
		free(created);
		return unlock_supervisor(supervisor, status);
	}
	supervisor->state = POSTGAMMA_SUPERVISOR_STATE_QUIESCING;
	supervisor->shutdown_requests++;
	*ticket = created;
	(void) postgamma_condition_broadcast(supervisor->condition);
	return unlock_supervisor(supervisor, 0);
}


int
postgamma_supervisor_ticket_wait(
	PostgammaSupervisorTicket *ticket,
	uint64_t deadline_ns,
	int *operation_status)
{
	PostgammaSupervisor *supervisor;
	int			status;

	if (!ticket_is_valid(ticket) || operation_status == NULL)
		return EINVAL;
	supervisor = ticket->supervisor;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	while (!ticket->completed)
	{
		status = wait_condition(supervisor, deadline_ns);
		if (status != 0)
			return unlock_supervisor(supervisor, status);
	}
	*operation_status = ticket->operation_status;
	return unlock_supervisor(supervisor, 0);
}


int
postgamma_supervisor_ticket_destroy(PostgammaSupervisorTicket *ticket)
{
	PostgammaSupervisor *supervisor;
	int			status;

	if (!ticket_is_valid(ticket))
		return EINVAL;
	supervisor = ticket->supervisor;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (!ticket->completed)
		return unlock_supervisor(supervisor, EBUSY);
	if (supervisor->active_tickets == 0)
		return unlock_supervisor(supervisor, EPROTO);
	supervisor->active_tickets--;
	ticket->magic = 0;
	ticket->supervisor = NULL;
	status = unlock_supervisor(supervisor, 0);
	if (status == 0)
		free(ticket);
	return status;
}


int
postgamma_supervisor_control_wake_fd(
	const PostgammaSupervisor *supervisor)
{
	return supervisor_is_valid(supervisor) ?
		postgamma_wake_target_fd(supervisor->control_wake) : -1;
}


int
postgamma_supervisor_control_wake_drain(
	PostgammaSupervisor *supervisor,
	uint64_t *wake_count)
{
	int			status;

	if (!supervisor_is_valid(supervisor) || wake_count == NULL)
		return EINVAL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (supervisor->callbacks.run == NULL || supervisor->thread == NULL ||
		!postgamma_thread_is_current(supervisor->thread))
		return unlock_supervisor(supervisor, EPERM);
	status = unlock_supervisor(supervisor, 0);
	return status == 0 ?
		postgamma_wake_target_drain(supervisor->control_wake, wake_count) :
		status;
}


int
postgamma_supervisor_control_notify(
	PostgammaSupervisor *supervisor,
	uint64_t generation)
{
	int			status;

	if (!supervisor_is_valid(supervisor))
		return EINVAL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (generation != supervisor->generation)
	{
		supervisor->stale_controls_rejected++;
		return unlock_supervisor(supervisor, ESTALE);
	}
	if (supervisor->callbacks.run == NULL ||
		supervisor->state == POSTGAMMA_SUPERVISOR_STATE_NEW ||
		supervisor->state == POSTGAMMA_SUPERVISOR_STATE_CLOSED ||
		supervisor->state == POSTGAMMA_SUPERVISOR_STATE_FAILED)
		return unlock_supervisor(supervisor, ESHUTDOWN);
	if (supervisor->control_notifications == UINT64_MAX)
		return unlock_supervisor(supervisor, EOVERFLOW);
	status = postgamma_wake_target_wake(supervisor->control_wake);
	if (status == 0)
		supervisor->control_notifications++;
	return unlock_supervisor(supervisor, status);
}


int
postgamma_supervisor_control_take(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorControl *control)
{
	PostgammaSupervisorTicket *ticket;
	int			status;

	if (!supervisor_is_valid(supervisor) || control == NULL)
		return EINVAL;
	memset(control, 0, sizeof(*control));
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (supervisor->callbacks.run == NULL || supervisor->thread == NULL ||
		!postgamma_thread_is_current(supervisor->thread))
		return unlock_supervisor(supervisor, EPERM);
	if (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_FAILED)
		return unlock_supervisor(supervisor, ESHUTDOWN);
	if (supervisor->inflight_ticket != NULL)
		return unlock_supervisor(supervisor, EBUSY);
	if (supervisor->queue_depth == 0)
		return unlock_supervisor(supervisor, EAGAIN);
	ticket = dequeue_ticket(supervisor);
	if (ticket == NULL)
	{
		set_failed(supervisor, EPROTO,
			POSTGAMMA_SUPERVISOR_FAILURE_CONTRACT, false, NULL);
		return unlock_supervisor(supervisor, EPROTO);
	}
	if (ticket->kind == POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN)
	{
		if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_QUIESCING)
		{
			set_failed(supervisor, EPROTO,
				POSTGAMMA_SUPERVISOR_FAILURE_CONTRACT, false, ticket);
			complete_ticket(supervisor, ticket, EPROTO);
			(void) postgamma_condition_broadcast(supervisor->condition);
			return unlock_supervisor(supervisor, EPROTO);
		}
		supervisor->state = POSTGAMMA_SUPERVISOR_STATE_STOPPING;
	}
	else if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_READY &&
			 supervisor->state != POSTGAMMA_SUPERVISOR_STATE_QUIESCING)
	{
		set_failed(supervisor, EPROTO,
			POSTGAMMA_SUPERVISOR_FAILURE_CONTRACT, false, ticket);
		complete_ticket(supervisor, ticket, EPROTO);
		(void) postgamma_condition_broadcast(supervisor->condition);
		return unlock_supervisor(supervisor, EPROTO);
	}
	control->generation = ticket->generation;
	control->sequence = ticket->sequence;
	control->kind = ticket->kind;
	control->shutdown_mode = ticket->shutdown_mode;
	control->payload = ticket->payload;
	control->private_token = ticket;
	supervisor->inflight_ticket = ticket;
	(void) postgamma_condition_broadcast(supervisor->condition);
	return unlock_supervisor(supervisor, 0);
}


int
postgamma_supervisor_control_complete(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorControl *control,
	int operation_status)
{
	PostgammaSupervisorTicket *ticket;
	int			status;

	if (!supervisor_is_valid(supervisor) || control == NULL ||
		control->private_token == NULL)
		return EINVAL;
	ticket = control->private_token;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (supervisor->callbacks.run == NULL || supervisor->thread == NULL ||
		!postgamma_thread_is_current(supervisor->thread))
		return unlock_supervisor(supervisor, EPERM);
	if (supervisor->inflight_ticket != ticket)
		return unlock_supervisor(supervisor, EPROTO);
	if (!ticket_is_valid(ticket) || ticket->supervisor != supervisor ||
		ticket->generation != control->generation ||
		ticket->sequence != control->sequence ||
		ticket->kind != control->kind ||
		ticket->shutdown_mode != control->shutdown_mode ||
		ticket->payload != control->payload)
		return unlock_supervisor(supervisor, EINVAL);
	if (ticket->completed)
		return unlock_supervisor(supervisor, EALREADY);
	if (control->kind == POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN &&
		supervisor->state != POSTGAMMA_SUPERVISOR_STATE_STOPPING &&
		supervisor->state != POSTGAMMA_SUPERVISOR_STATE_FAILED)
		return unlock_supervisor(supervisor, EPROTO);
	if (operation_status != 0 &&
		control->kind == POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN)
		set_failed(supervisor, operation_status,
			POSTGAMMA_SUPERVISOR_FAILURE_SHUTDOWN, false, ticket);
	if (operation_status == 0 &&
		supervisor->state == POSTGAMMA_SUPERVISOR_STATE_FAILED)
		operation_status = supervisor->failure_status != 0 ?
			supervisor->failure_status : EPROTO;
	complete_ticket(supervisor, ticket, operation_status);
	supervisor->inflight_ticket = NULL;
	memset(control, 0, sizeof(*control));
	(void) postgamma_condition_broadcast(supervisor->condition);
	return unlock_supervisor(supervisor, 0);
}


int
postgamma_supervisor_join(
	PostgammaSupervisor *supervisor,
	uint64_t deadline_ns)
{
	PostgammaThread *thread;
	int			status;

	if (!supervisor_is_valid(supervisor))
		return EINVAL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (supervisor->thread == NULL)
		return unlock_supervisor(
			supervisor, supervisor->threads_joined != 0 ? 0 : EPROTO);
	if (supervisor->joining)
		return unlock_supervisor(supervisor, EBUSY);
	if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_QUIESCING &&
		supervisor->state != POSTGAMMA_SUPERVISOR_STATE_STOPPING &&
		supervisor->state != POSTGAMMA_SUPERVISOR_STATE_CLOSED &&
		supervisor->state != POSTGAMMA_SUPERVISOR_STATE_FAILED)
		return unlock_supervisor(supervisor, EBUSY);
	supervisor->joining = true;
	while (!supervisor->main_exited)
	{
		status = wait_condition(supervisor, deadline_ns);
		if (status != 0)
		{
			supervisor->joining = false;
			return unlock_supervisor(supervisor, status);
		}
	}
	thread = supervisor->thread;
	status = unlock_supervisor(supervisor, 0);
	if (status != 0)
		return status;
	status = postgamma_thread_join(thread, NULL);
	if (status == 0)
		status = postgamma_thread_destroy(thread);
	{
		int			lock_status = lock_supervisor(supervisor);

		if (lock_status != 0)
			return lock_status;
		supervisor->joining = false;
		if (status == 0)
		{
			supervisor->thread = NULL;
			supervisor->threads_joined++;
			if (supervisor->last_failure.present)
				supervisor->last_failure.supervisor_thread_joined = true;
		}
		return unlock_supervisor(supervisor, status);
	}
}


int
postgamma_supervisor_fail(
	PostgammaSupervisor *supervisor,
	int failure_status)
{
	return postgamma_supervisor_fail_detailed(
		supervisor, failure_status, false);
}


int
postgamma_supervisor_fail_detailed(
	PostgammaSupervisor *supervisor,
	int failure_status,
	bool process_restart_required)
{
	int			status;

	if (!supervisor_is_valid(supervisor) || failure_status == 0)
		return EINVAL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_CLOSED)
		return unlock_supervisor(supervisor, ESHUTDOWN);
	set_failed(supervisor, failure_status,
		POSTGAMMA_SUPERVISOR_FAILURE_HOST_FAIL_STOP,
		process_restart_required, supervisor->inflight_ticket);
	(void) postgamma_wake_target_wake(supervisor->control_wake);
	(void) postgamma_condition_broadcast(supervisor->condition);
	return unlock_supervisor(supervisor, 0);
}


uint64_t
postgamma_supervisor_generation(const PostgammaSupervisor *supervisor)
{
	return supervisor_is_valid(supervisor) ? supervisor->generation : 0;
}


PostgammaSupervisorState
postgamma_supervisor_state(PostgammaSupervisor *supervisor)
{
	PostgammaSupervisorState state;

	if (!supervisor_is_valid(supervisor) ||
		lock_supervisor(supervisor) != 0)
		return POSTGAMMA_SUPERVISOR_STATE_FAILED;
	state = supervisor->state;
	if (unlock_supervisor(supervisor, 0) != 0)
		return POSTGAMMA_SUPERVISOR_STATE_FAILED;
	return state;
}


int
postgamma_supervisor_telemetry(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorTelemetry *telemetry)
{
	int			status;

	if (!supervisor_is_valid(supervisor) || telemetry == NULL)
		return EINVAL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	memset(telemetry, 0, sizeof(*telemetry));
	telemetry->generation = supervisor->generation;
	telemetry->state = supervisor->state;
	telemetry->queue_capacity = supervisor->queue_capacity;
	telemetry->queue_depth = supervisor->queue_depth;
	telemetry->queue_depth_peak = supervisor->queue_depth_peak;
	telemetry->controls_submitted = supervisor->controls_submitted;
	telemetry->controls_completed = supervisor->controls_completed;
	telemetry->control_notifications =
		supervisor->control_notifications;
	telemetry->stale_controls_rejected =
		supervisor->stale_controls_rejected;
	telemetry->controls_rejected_after_quiesce =
		supervisor->controls_rejected_after_quiesce;
	telemetry->shutdown_requests = supervisor->shutdown_requests;
	telemetry->threads_started = supervisor->threads_started;
	telemetry->threads_joined = supervisor->threads_joined;
	telemetry->active_tickets = supervisor->active_tickets;
	telemetry->failure_attempts = supervisor->failure_attempts;
	telemetry->failure_status = supervisor->failure_status;
	telemetry->last_failure = supervisor->last_failure;
	return unlock_supervisor(supervisor, 0);
}


int
postgamma_supervisor_failure_record(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorFailureRecord *record)
{
	int			status;

	if (!supervisor_is_valid(supervisor) || record == NULL ||
		record->struct_size != sizeof(*record))
		return EINVAL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	*record = supervisor->last_failure;
	return unlock_supervisor(supervisor, 0);
}


int
postgamma_supervisor_destroy(PostgammaSupervisor *supervisor)
{
	int			status;

	if (!supervisor_is_valid(supervisor))
		return EINVAL;
	status = lock_supervisor(supervisor);
	if (status != 0)
		return status;
	if (supervisor->thread != NULL || supervisor->joining ||
		supervisor->queue_depth != 0 || supervisor->inflight_ticket != NULL ||
		supervisor->active_tickets != 0 ||
		(supervisor->state != POSTGAMMA_SUPERVISOR_STATE_NEW &&
		 supervisor->state != POSTGAMMA_SUPERVISOR_STATE_CLOSED &&
		 supervisor->state != POSTGAMMA_SUPERVISOR_STATE_FAILED))
		return unlock_supervisor(supervisor, EBUSY);
	status = unlock_supervisor(supervisor, 0);
	if (status != 0)
		return status;
	status = postgamma_wake_target_destroy(supervisor->control_wake);
	if (status != 0)
		return status;
	status = postgamma_condition_destroy(supervisor->condition);
	if (status != 0)
		return status;
	status = postgamma_mutex_destroy(supervisor->mutex);
	if (status != 0)
		return status;
	supervisor->magic = 0;
	free(supervisor->queue);
	memset(supervisor, 0, sizeof(*supervisor));
	free(supervisor);
	return 0;
}


const char *
postgamma_supervisor_state_name(PostgammaSupervisorState state)
{
	switch (state)
	{
		case POSTGAMMA_SUPERVISOR_STATE_NEW:
			return "new";
		case POSTGAMMA_SUPERVISOR_STATE_STARTING:
			return "starting";
		case POSTGAMMA_SUPERVISOR_STATE_RECOVERING:
			return "recovering";
		case POSTGAMMA_SUPERVISOR_STATE_READY:
			return "ready";
		case POSTGAMMA_SUPERVISOR_STATE_QUIESCING:
			return "quiescing";
		case POSTGAMMA_SUPERVISOR_STATE_STOPPING:
			return "stopping";
		case POSTGAMMA_SUPERVISOR_STATE_CLOSED:
			return "closed";
		case POSTGAMMA_SUPERVISOR_STATE_FAILED:
			return "failed";
	}
	return "invalid";
}


const char *
postgamma_supervisor_failure_origin_name(
	PostgammaSupervisorFailureOrigin origin)
{
	switch (origin)
	{
		case POSTGAMMA_SUPERVISOR_FAILURE_NONE:
			return "none";
		case POSTGAMMA_SUPERVISOR_FAILURE_THREAD_START:
			return "thread-start";
		case POSTGAMMA_SUPERVISOR_FAILURE_WAIT:
			return "wait";
		case POSTGAMMA_SUPERVISOR_FAILURE_BOOT:
			return "boot";
		case POSTGAMMA_SUPERVISOR_FAILURE_RUN:
			return "run";
		case POSTGAMMA_SUPERVISOR_FAILURE_SHUTDOWN:
			return "shutdown";
		case POSTGAMMA_SUPERVISOR_FAILURE_HOST_FAIL_STOP:
			return "host-fail-stop";
		case POSTGAMMA_SUPERVISOR_FAILURE_CONTRACT:
			return "contract";
	}
	return "invalid";
}


static bool
supervisor_is_valid(const PostgammaSupervisor *supervisor)
{
	return supervisor != NULL &&
		supervisor->magic == POSTGAMMA_SUPERVISOR_MAGIC;
}


static bool
ticket_is_valid(const PostgammaSupervisorTicket *ticket)
{
	return ticket != NULL &&
		ticket->magic == POSTGAMMA_SUPERVISOR_TICKET_MAGIC &&
		supervisor_is_valid(ticket->supervisor) &&
		ticket->generation == ticket->supervisor->generation;
}


static int
lock_supervisor(PostgammaSupervisor *supervisor)
{
	return postgamma_mutex_lock(supervisor->mutex);
}


static int
unlock_supervisor(PostgammaSupervisor *supervisor, int status)
{
	int			unlock_status = postgamma_mutex_unlock(supervisor->mutex);

	return unlock_status != 0 ? unlock_status : status;
}


static int
wait_condition(PostgammaSupervisor *supervisor, uint64_t deadline_ns)
{
	if (deadline_ns == POSTGAMMA_SUPERVISOR_NO_DEADLINE)
		return postgamma_condition_wait(
			supervisor->condition, supervisor->mutex);
	return postgamma_condition_timed_wait(
		supervisor->condition, supervisor->mutex, deadline_ns);
}


static int
allocate_ticket(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorControlKind kind,
	void *payload,
	PostgammaSupervisorTicket **ticket)
{
	PostgammaSupervisorTicket *created;

	if (supervisor->next_sequence == 0 ||
		supervisor->next_sequence == UINT64_MAX ||
		supervisor->controls_submitted == UINT64_MAX ||
		supervisor->active_tickets == UINT64_MAX)
		return EOVERFLOW;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->magic = POSTGAMMA_SUPERVISOR_TICKET_MAGIC;
	created->supervisor = supervisor;
	created->generation = supervisor->generation;
	created->sequence = supervisor->next_sequence++;
	created->kind = kind;
	created->payload = payload;
	*ticket = created;
	return 0;
}


static int
enqueue_ticket(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorTicket *ticket)
{
	size_t		tail;

	if (supervisor->queue_depth >= supervisor->queue_capacity)
		return EAGAIN;
	tail = (supervisor->queue_head + supervisor->queue_depth) %
		supervisor->queue_capacity;
	supervisor->queue[tail] = ticket;
	supervisor->queue_depth++;
	supervisor->controls_submitted++;
	supervisor->active_tickets++;
	if (supervisor->queue_depth > supervisor->queue_depth_peak)
		supervisor->queue_depth_peak = supervisor->queue_depth;
	return 0;
}


static PostgammaSupervisorTicket *
dequeue_ticket(PostgammaSupervisor *supervisor)
{
	PostgammaSupervisorTicket *ticket;

	if (supervisor->queue_depth == 0)
		return NULL;
	ticket = supervisor->queue[supervisor->queue_head];
	supervisor->queue[supervisor->queue_head] = NULL;
	supervisor->queue_head =
		(supervisor->queue_head + 1) % supervisor->queue_capacity;
	supervisor->queue_depth--;
	return ticket;
}


static void
complete_ticket(
	PostgammaSupervisor *supervisor,
	PostgammaSupervisorTicket *ticket,
	int operation_status)
{
	if (ticket == NULL || ticket->completed)
		return;
	ticket->operation_status = operation_status;
	ticket->completed = true;
	if (supervisor->controls_completed != UINT64_MAX)
		supervisor->controls_completed++;
}


static void
reject_queued_tickets(
	PostgammaSupervisor *supervisor,
	int operation_status)
{
	PostgammaSupervisorTicket *ticket;

	while ((ticket = dequeue_ticket(supervisor)) != NULL)
		complete_ticket(supervisor, ticket, operation_status);
}


static void
set_failed(
	PostgammaSupervisor *supervisor,
	int failure_status,
	PostgammaSupervisorFailureOrigin origin,
	bool process_restart_required,
	const PostgammaSupervisorTicket *ticket)
{
	int			recorded_status = failure_status != 0 ? failure_status : EPROTO;

	if (supervisor->failure_attempts != UINT64_MAX)
		supervisor->failure_attempts++;
	if (supervisor->last_failure.present)
	{
		if (process_restart_required)
			supervisor->last_failure.process_restart_required = true;
		return;
	}
	supervisor->last_failure.struct_size =
		sizeof(supervisor->last_failure);
	supervisor->last_failure.present = true;
	supervisor->last_failure.generation = supervisor->generation;
	supervisor->last_failure.ordinal = supervisor->failure_attempts;
	supervisor->last_failure.recorded_at_ns = postgamma_monotonic_now_ns();
	supervisor->last_failure.origin = origin;
	supervisor->last_failure.state_before_failure = supervisor->state;
	supervisor->last_failure.control_kind = ticket != NULL ?
		ticket->kind : POSTGAMMA_SUPERVISOR_CONTROL_NONE;
	supervisor->last_failure.control_sequence = ticket != NULL ?
		ticket->sequence : 0;
	supervisor->last_failure.status = recorded_status;
	supervisor->last_failure.process_restart_required =
		process_restart_required;
	supervisor->state = POSTGAMMA_SUPERVISOR_STATE_FAILED;
	supervisor->failure_status = recorded_status;
}


static void *
supervisor_main(void *argument)
{
	PostgammaSupervisor *supervisor = argument;
	PostgammaSupervisorTicket *ticket;
	int			status = 0;

	(void) lock_supervisor(supervisor);
	while (supervisor->thread == NULL &&
		   supervisor->state != POSTGAMMA_SUPERVISOR_STATE_FAILED)
	{
		status = postgamma_condition_wait(
			supervisor->condition, supervisor->mutex);
		if (status != 0)
		{
			set_failed(supervisor, status,
				POSTGAMMA_SUPERVISOR_FAILURE_WAIT, false, NULL);
			break;
		}
	}
	if (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_FAILED)
		goto finish;
	(void) unlock_supervisor(supervisor, 0);

	if (supervisor->callbacks.boot != NULL)
		status = supervisor->callbacks.boot(
			supervisor, supervisor->callback_argument);
	(void) lock_supervisor(supervisor);
	if (status != 0)
		set_failed(supervisor, status,
			POSTGAMMA_SUPERVISOR_FAILURE_BOOT, false, NULL);
	else if (supervisor->callbacks.run == NULL &&
			 (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_STARTING ||
			  supervisor->state == POSTGAMMA_SUPERVISOR_STATE_RECOVERING))
		supervisor->state = POSTGAMMA_SUPERVISOR_STATE_READY;
	else if (supervisor->callbacks.run != NULL &&
			 (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_STARTING ||
			  supervisor->state == POSTGAMMA_SUPERVISOR_STATE_RECOVERING))
	{
		/* The kernel run callback publishes READY at its recovery boundary. */
	}
	else if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_FAILED)
		set_failed(supervisor, EPROTO,
			POSTGAMMA_SUPERVISOR_FAILURE_CONTRACT, false, NULL);
	(void) postgamma_condition_broadcast(supervisor->condition);
	if (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_FAILED)
		goto finish;

	if (supervisor->callbacks.run != NULL)
	{
		(void) unlock_supervisor(supervisor, 0);
		status = supervisor->callbacks.run(
			supervisor, supervisor->callback_argument);
		(void) lock_supervisor(supervisor);
		if (status != 0)
			set_failed(supervisor, status,
				POSTGAMMA_SUPERVISOR_FAILURE_RUN, false, NULL);
		else if (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_STOPPING &&
				 supervisor->inflight_ticket == NULL &&
				 supervisor->queue_depth == 0)
			supervisor->state = POSTGAMMA_SUPERVISOR_STATE_CLOSED;
		else if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_FAILED)
			set_failed(supervisor, EPROTO,
				POSTGAMMA_SUPERVISOR_FAILURE_CONTRACT, false, NULL);
		goto finish;
	}

	while (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_FAILED)
	{
		while (supervisor->queue_depth == 0 &&
			   supervisor->state != POSTGAMMA_SUPERVISOR_STATE_FAILED)
		{
			status = postgamma_condition_wait(
				supervisor->condition, supervisor->mutex);
			if (status != 0)
			{
				set_failed(supervisor, status,
					POSTGAMMA_SUPERVISOR_FAILURE_WAIT, false, NULL);
				break;
			}
		}
		if (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_FAILED)
			break;
		ticket = dequeue_ticket(supervisor);
		if (ticket == NULL)
		{
			set_failed(supervisor, EPROTO,
				POSTGAMMA_SUPERVISOR_FAILURE_CONTRACT, false, NULL);
			break;
		}
		supervisor->inflight_ticket = ticket;
		(void) unlock_supervisor(supervisor, 0);

		if (ticket->kind == POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN)
		{
			bool		shutdown_ready = false;

			(void) lock_supervisor(supervisor);
			if (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_QUIESCING)
			{
				supervisor->state = POSTGAMMA_SUPERVISOR_STATE_STOPPING;
				shutdown_ready = true;
			}
			else
				set_failed(supervisor, EPROTO,
					POSTGAMMA_SUPERVISOR_FAILURE_CONTRACT, false, ticket);
			(void) postgamma_condition_broadcast(supervisor->condition);
			(void) unlock_supervisor(supervisor, 0);
			status = shutdown_ready ? 0 : EPROTO;
			if (status == 0 && supervisor->callbacks.shutdown != NULL)
				status = supervisor->callbacks.shutdown(
					supervisor, ticket->shutdown_mode,
					supervisor->callback_argument);
			(void) lock_supervisor(supervisor);
			if (status == 0 &&
				supervisor->state == POSTGAMMA_SUPERVISOR_STATE_FAILED)
				status = supervisor->failure_status != 0 ?
					supervisor->failure_status : EPROTO;
			complete_ticket(supervisor, ticket, status);
			supervisor->inflight_ticket = NULL;
			if (status == 0 &&
				supervisor->state == POSTGAMMA_SUPERVISOR_STATE_STOPPING)
				supervisor->state = POSTGAMMA_SUPERVISOR_STATE_CLOSED;
			else if (supervisor->state != POSTGAMMA_SUPERVISOR_STATE_FAILED)
				set_failed(supervisor, status != 0 ? status : EPROTO,
					POSTGAMMA_SUPERVISOR_FAILURE_SHUTDOWN, false, ticket);
			break;
		}

		if (supervisor->callbacks.dispatch == NULL)
			status = ENOTSUP;
		else
			status = supervisor->callbacks.dispatch(
				supervisor, ticket->kind, ticket->payload,
				supervisor->callback_argument);
		(void) lock_supervisor(supervisor);
		if (status == 0 &&
			supervisor->state == POSTGAMMA_SUPERVISOR_STATE_FAILED)
			status = supervisor->failure_status != 0 ?
				supervisor->failure_status : EPROTO;
		complete_ticket(supervisor, ticket, status);
		supervisor->inflight_ticket = NULL;
		(void) postgamma_condition_broadcast(supervisor->condition);
	}

finish:
	if (supervisor->inflight_ticket != NULL)
	{
		PostgammaSupervisorTicket *inflight =
			supervisor->inflight_ticket;

		complete_ticket(
			supervisor, inflight, ESHUTDOWN);
		supervisor->inflight_ticket = NULL;
		set_failed(supervisor,
			supervisor->failure_status != 0 ?
			supervisor->failure_status : EPROTO,
			POSTGAMMA_SUPERVISOR_FAILURE_CONTRACT, false,
			inflight);
	}
	if (supervisor->state == POSTGAMMA_SUPERVISOR_STATE_FAILED)
		reject_queued_tickets(supervisor, ESHUTDOWN);
	supervisor->main_exited = true;
	if (supervisor->last_failure.present)
		supervisor->last_failure.supervisor_thread_exited = true;
	(void) postgamma_condition_broadcast(supervisor->condition);
	(void) unlock_supervisor(supervisor, 0);
	return NULL;
}
