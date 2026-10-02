/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/private/kernel_supervisor_adapter.h"

#include <errno.h>
#include <string.h>


_Static_assert(
	POSTGAMMA_KERNEL_CONTROL_CONNECT ==
		(int) POSTGAMMA_SUPERVISOR_CONTROL_CONNECT,
	"kernel and supervisor control kinds must match");
_Static_assert(
	POSTGAMMA_KERNEL_CONTROL_CANCEL ==
		(int) POSTGAMMA_SUPERVISOR_CONTROL_CANCEL,
	"kernel and supervisor control kinds must match");
_Static_assert(
	POSTGAMMA_KERNEL_CONTROL_DISCONNECT ==
		(int) POSTGAMMA_SUPERVISOR_CONTROL_DISCONNECT,
	"kernel and supervisor control kinds must match");
_Static_assert(
	POSTGAMMA_KERNEL_CONTROL_RELOAD ==
		(int) POSTGAMMA_SUPERVISOR_CONTROL_RELOAD,
	"kernel and supervisor control kinds must match");
_Static_assert(
	POSTGAMMA_KERNEL_CONTROL_ROLE_COMPLETION ==
		(int) POSTGAMMA_SUPERVISOR_CONTROL_ROLE_COMPLETION,
	"kernel and supervisor control kinds must match");
_Static_assert(
	POSTGAMMA_KERNEL_CONTROL_SHUTDOWN ==
		(int) POSTGAMMA_SUPERVISOR_CONTROL_SHUTDOWN,
	"kernel and supervisor control kinds must match");
_Static_assert(
	POSTGAMMA_KERNEL_CONTROL_CHECKPOINT ==
		(int) POSTGAMMA_SUPERVISOR_CONTROL_CHECKPOINT,
	"kernel and supervisor control kinds must match");
_Static_assert(
	POSTGAMMA_KERNEL_SHUTDOWN_SMART ==
		(int) POSTGAMMA_SUPERVISOR_SHUTDOWN_SMART,
	"kernel and supervisor shutdown modes must match");
_Static_assert(
	POSTGAMMA_KERNEL_SHUTDOWN_FAST ==
		(int) POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST,
	"kernel and supervisor shutdown modes must match");
_Static_assert(
	POSTGAMMA_KERNEL_SHUTDOWN_IMMEDIATE ==
		(int) POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE,
	"kernel and supervisor shutdown modes must match");


static int
validate_generation(PostgammaSupervisor *supervisor, uint64_t generation)
{
	return postgamma_supervisor_generation(supervisor) == generation ?
		0 : ESTALE;
}


static int
mark_recovering(void *context, uint64_t generation)
{
	PostgammaSupervisor *supervisor = context;
	int			status = validate_generation(supervisor, generation);

	return status == 0 ?
		postgamma_supervisor_mark_recovering(supervisor) : status;
}


static int
mark_ready(void *context, uint64_t generation)
{
	PostgammaSupervisor *supervisor = context;
	int			status = validate_generation(supervisor, generation);

	return status == 0 ?
		postgamma_supervisor_mark_ready(supervisor) : status;
}


static int
control_wake_fd(void *context, uint64_t generation, int *wake_fd)
{
	PostgammaSupervisor *supervisor = context;
	int			status;

	if (wake_fd == NULL)
		return EINVAL;
	*wake_fd = -1;
	status = validate_generation(supervisor, generation);
	if (status != 0)
		return status;
	*wake_fd = postgamma_supervisor_control_wake_fd(supervisor);
	return *wake_fd >= 0 ? 0 : EINVAL;
}


static int
control_wake_drain(
	void *context, uint64_t generation, uint64_t *wake_count)
{
	PostgammaSupervisor *supervisor = context;
	int			status = validate_generation(supervisor, generation);

	return status == 0 ?
		postgamma_supervisor_control_wake_drain(supervisor, wake_count) :
		status;
}


static int
control_notify(void *context, uint64_t generation)
{
	return postgamma_supervisor_control_notify(context, generation);
}


static int
control_take(
	void *context, uint64_t generation, PostgammaKernelControl *control)
{
	PostgammaSupervisor *supervisor = context;
	PostgammaSupervisorControl supervisor_control;
	int			status;

	if (control == NULL)
		return EINVAL;
	memset(control, 0, sizeof(*control));
	status = validate_generation(supervisor, generation);
	if (status != 0)
		return status;
	status = postgamma_supervisor_control_take(
		supervisor, &supervisor_control);
	if (status != 0)
		return status;
	control->generation = supervisor_control.generation;
	control->sequence = supervisor_control.sequence;
	control->kind = (PostgammaKernelControlKind) supervisor_control.kind;
	control->shutdown_mode =
		(PostgammaKernelShutdownMode) supervisor_control.shutdown_mode;
	control->payload = supervisor_control.payload;
	control->private_token = supervisor_control.private_token;
	return 0;
}


static int
control_complete(
	void *context, PostgammaKernelControl *control, int operation_status)
{
	PostgammaSupervisor *supervisor = context;
	PostgammaSupervisorControl supervisor_control;
	int			status;

	if (control == NULL)
		return EINVAL;
	supervisor_control.generation = control->generation;
	supervisor_control.sequence = control->sequence;
	supervisor_control.kind =
		(PostgammaSupervisorControlKind) control->kind;
	supervisor_control.shutdown_mode =
		(PostgammaSupervisorShutdownMode) control->shutdown_mode;
	supervisor_control.payload = control->payload;
	supervisor_control.private_token = control->private_token;
	status = postgamma_supervisor_control_complete(
		supervisor, &supervisor_control, operation_status);
	if (status == 0)
		memset(control, 0, sizeof(*control));
	return status;
}


static int
fail_supervisor(void *context, uint64_t generation, int failure_status)
{
	PostgammaSupervisor *supervisor = context;
	int			status = validate_generation(supervisor, generation);

	return status == 0 ?
		postgamma_supervisor_fail(supervisor, failure_status) : status;
}


int
postgamma_kernel_supervisor_provider_init(
	PostgammaSupervisor *supervisor,
	PostgammaKernelHostProvider *provider)
{
	PostgammaKernelHostProvider initialized =
		POSTGAMMA_KERNEL_HOST_PROVIDER_INIT;

	if (supervisor == NULL || provider == NULL ||
		postgamma_supervisor_generation(supervisor) == 0)
		return EINVAL;
	initialized.capabilities =
		POSTGAMMA_KERNEL_HOST_CAP_CONTROL_WAKE_FD |
		POSTGAMMA_KERNEL_HOST_CAP_ASYNC_NOTIFICATION |
		POSTGAMMA_KERNEL_HOST_CAP_FAIL_STOP;
	initialized.context = supervisor;
	initialized.mark_recovering = mark_recovering;
	initialized.mark_ready = mark_ready;
	initialized.control_wake_fd = control_wake_fd;
	initialized.control_wake_drain = control_wake_drain;
	initialized.control_notify = control_notify;
	initialized.control_take = control_take;
	initialized.control_complete = control_complete;
	initialized.fail = fail_supervisor;
	*provider = initialized;
	return 0;
}
