/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_VIRTUAL_SIGNAL_H
#define POSTGAMMA_VIRTUAL_SIGNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <sys/time.h>

#include "postgamma/backend_execution_runtime.h"

/* Match the upstream handler signature without depending on PostgreSQL state. */
struct pg_signal_info;
typedef void (*PostgammaVirtualSignalHandler)(int, const struct pg_signal_info *);
#define POSTGAMMA_SIGNAL_DEFAULT ((PostgammaVirtualSignalHandler) (void (*)(void)) SIG_DFL)
#define POSTGAMMA_SIGNAL_IGNORE  ((PostgammaVirtualSignalHandler) (void (*)(void)) SIG_IGN)

typedef struct PostgammaVirtualTimer
{
	int64_t		timeout_due_at;
	int64_t		timeout_interval_us;
	bool		timeout_armed;
	bool		timeout_wait_shortened;
} PostgammaVirtualTimer;

/*
 * One logical session, or one bound standalone input, owns this state.
 * Backend pending bits belong to the registry; pending_signals is only used
 * by standalone input. No signal handler is installed in the host process.
 */
typedef struct PostgammaVirtualSignals
{
	sigset_t	logical_signal_mask;
	PostgammaVirtualSignalHandler signal_handlers[POSTGAMMA_BACKEND_SIGNAL_COUNT + 1];
	uint64_t	pending_signals;
	bool		dispatching_signals;
} PostgammaVirtualSignals;

typedef enum PostgammaSignalExit
{
	POSTGAMMA_SIGNAL_NO_EXIT,
	POSTGAMMA_SIGNAL_DEFAULT_EXIT,
	POSTGAMMA_SIGNAL_QUICK_EXIT
} PostgammaSignalExit;

typedef struct PostgammaSignalDispatchResult
{
	uint64_t	blocked;
	PostgammaSignalExit exit_kind;
	int			signal_number;
} PostgammaSignalDispatchResult;

/*
 * The caller owns reentrancy checks and registry/standalone pending dequeue.
 * An exit result requires immediate session termination: remaining pending
 * bits are intentionally discarded, as they were before this module split.
 * A handler may leave through the caller's existing nonlocal error/exit path.
 */
PostgammaSignalDispatchResult postgamma_virtual_signals_dispatch(
	PostgammaVirtualSignals *signals, uint64_t pending, bool backend,
	const struct pg_signal_info *info);
int postgamma_virtual_signal_mask(sigset_t *mask, int how,
								 const sigset_t *set, sigset_t *old_set);

/* now is supplied by PostgreSQL, in microseconds in its TimestampTz epoch. */
int postgamma_virtual_timer_set(PostgammaVirtualTimer *timer, int which,
	const struct itimerval *new_value, struct itimerval *old_value, int64_t now);
bool postgamma_virtual_timer_due(PostgammaVirtualTimer *timer, int64_t now);
long postgamma_virtual_timer_adjust_wait(PostgammaVirtualTimer *timer,
										long requested_timeout, int64_t now);
bool postgamma_virtual_timer_wait_expired(PostgammaVirtualTimer *timer,
										 int wait_result);

#endif
