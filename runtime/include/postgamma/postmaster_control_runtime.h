/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postmaster_control_runtime.h
 *    Instance-scoped control boundary for an embedded postmaster thread.
 *
 * This interface deliberately contains no PostgreSQL implementation types.
 * PostgreSQL-specific mapping remains in postgres_postmaster_runtime_impl.h.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTMASTER_CONTROL_RUNTIME_H
#define POSTGAMMA_POSTMASTER_CONTROL_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include <locale.h>
#include <signal.h>

#include "postgamma/embedded_kernel.h"
#include "postgamma/instance_runtime.h"


#ifdef __cplusplus
extern "C" {
#endif


typedef struct PostgammaPostmasterControlRuntime
	PostgammaPostmasterControlRuntime;

int postgamma_postmaster_control_runtime_create(
	PostgammaPostmasterControlRuntime **control_runtime,
	PostgammaInstanceRuntime *instance_runtime,
	const PostgammaKernelHostProvider *host);
int postgamma_postmaster_control_runtime_destroy(
	PostgammaPostmasterControlRuntime *control_runtime);
int postgamma_postmaster_control_runtime_bind(
	PostgammaPostmasterControlRuntime *control_runtime);
int postgamma_postmaster_control_runtime_unbind(
	PostgammaPostmasterControlRuntime *control_runtime);
bool postgamma_postmaster_control_runtime_is_bound(void);

int postgamma_postmaster_control_current_wake_fd(int *wake_fd);
int postgamma_postmaster_control_current_wake_drain(uint64_t *wake_count);
int postgamma_postmaster_control_current_take_signals(
	uint64_t *pending_signals);
int postgamma_postmaster_control_current_take(
	PostgammaKernelControl *control);
int postgamma_postmaster_control_current_complete(
	PostgammaKernelControl *control,
	int operation_status);
int postgamma_postmaster_control_current_mark_recovering(void);
int postgamma_postmaster_control_current_mark_ready(void);
int postgamma_postmaster_control_current_fail(int failure_status);
int postgamma_postmaster_control_current_sigprocmask(
	int how,
	const sigset_t *set,
	sigset_t *old_set);
char *postgamma_postmaster_control_current_setlocale(
	int category,
	const char *locale_name);

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_POSTMASTER_CONTROL_RUNTIME_H */
