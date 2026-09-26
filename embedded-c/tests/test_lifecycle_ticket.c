/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "postgamma/private/public_runtime.h"

#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static _Atomic bool entered, released;
static int completion_status;
static uint64_t expected_request;

static int
dispatch(PostgammaSupervisor *supervisor, PostgammaSupervisorControlKind kind,
         void *payload, void *argument)
{
	PostgammaKernelCancelRequest *cancel = payload;
	(void) supervisor; (void) argument;
	assert(kind == POSTGAMMA_SUPERVISOR_CONTROL_CANCEL);
	atomic_store(&entered, true);
	while (!atomic_load(&released)) sched_yield();
	/* Read after the submitting host returned and polled repeatedly. */
	assert(cancel->generation == 41 && cancel->request_generation == expected_request);
	assert(cancel->backend_pid == 1234);
	cancel->dispatched = completion_status == 0;
	return completion_status;
}

int
main(void)
{
	pgm_instance instance = {0};
	pgm_connection connection = {0};
	PostgammaSupervisorOptions options = POSTGAMMA_SUPERVISOR_OPTIONS_INIT;
	PostgammaSupervisorTicket *shutdown = NULL;
	PostgammaSupervisorTelemetry telemetry;
	int operation_status;

	instance.magic = PGM_INSTANCE_MAGIC;
	instance.owner_pid = getpid(); instance.generation = 41;
	connection.magic = PGM_CONNECTION_MAGIC;
	connection.owner_pid = getpid(); connection.instance = &instance;
	connection.backend_pid = 1234;
	options.generation = 41; options.callbacks.dispatch = dispatch;
	assert(postgamma_supervisor_create(&options, &instance.supervisor) == 0);
	assert(postgamma_supervisor_start(instance.supervisor,
		postgamma_monotonic_now_ns() + UINT64_C(5000000000)) == 0);
	for (unsigned round = 0; round < 2; round++)
	{
		pgm_request *request = calloc(1, sizeof(*request));
		pgm_error *error = NULL;
		bool done = false;
		uint64_t started;
		pgm_status status;

		assert(request != NULL);
		request->magic = PGM_REQUEST_MAGIC; request->owner_pid = getpid();
		request->connection = &connection; request->generation = 7 + round;
		request->state = PGM_REQUEST_RUNNING;
		assert(postgamma_mutex_create(&request->mutex) == 0);
		expected_request = request->generation;
		completion_status = round ? EIO : 0;
		atomic_store(&entered, false); atomic_store(&released, false);
		assert(postgamma_public_cancel_dispatch_step(request, true, &done, &error) == PGM_STATUS_OK);
		assert(!done && request->cancel_ticket != NULL && error == NULL);
		while (!atomic_load(&entered)) sched_yield();
		started = postgamma_monotonic_now_ns();
		for (unsigned poll = 0; poll < 100; poll++)
		{
			assert(postgamma_public_cancel_dispatch_step(request, false, &done, &error) == PGM_STATUS_OK);
			assert(!done && request->cancel_ticket != NULL);
		}
		assert(postgamma_monotonic_now_ns() - started < UINT64_C(500000000));
		assert(postgamma_supervisor_telemetry(instance.supervisor, &telemetry) == 0);
		assert(telemetry.active_tickets == 1 && telemetry.controls_submitted == round + 1);
		atomic_store(&released, true);
		do {
			status = postgamma_public_cancel_dispatch_step(request, false, &done, &error);
			sched_yield();
		} while (status == PGM_STATUS_OK && !done);
		assert((status == PGM_STATUS_OK) == (round == 0));
		assert(request->cancel_ticket == NULL);
		pgm_error_free(error);
		assert(postgamma_supervisor_telemetry(instance.supervisor, &telemetry) == 0);
		assert(telemetry.active_tickets == 0);
		assert(postgamma_mutex_destroy(request->mutex) == 0); free(request);
	}
	assert(postgamma_supervisor_request_shutdown(instance.supervisor, 41,
		POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST, &shutdown) == 0);
	assert(postgamma_supervisor_ticket_wait(shutdown,
		postgamma_monotonic_now_ns() + UINT64_C(5000000000), &operation_status) == 0);
	assert(operation_status == 0 && postgamma_supervisor_ticket_destroy(shutdown) == 0);
	assert(postgamma_supervisor_join(instance.supervisor,
		postgamma_monotonic_now_ns() + UINT64_C(5000000000)) == 0);
	assert(postgamma_supervisor_destroy(instance.supervisor) == 0);
	puts("lifecycle ticket: pending payload lifetime, bounded polls, success and failure retirement passed");
	return 0;
}
