/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "postgamma/virtual_signal.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

#define USECS_PER_SEC INT64_C(1000000)

PostgammaSignalDispatchResult
postgamma_virtual_signals_dispatch(PostgammaVirtualSignals *signals,
	uint64_t pending, bool backend, const struct pg_signal_info *info)
{
	PostgammaSignalDispatchResult result = {0, POSTGAMMA_SIGNAL_NO_EXIT, 0};

	signals->dispatching_signals = true;
	for (int number = 1; number <= POSTGAMMA_BACKEND_SIGNAL_COUNT; number++)
	{
		uint64_t bit = UINT64_C(1) << (number - 1);
		PostgammaVirtualSignalHandler handler;

		if ((pending & bit) == 0)
			continue;
		if (sigismember(&signals->logical_signal_mask, number) == 1)
		{
			if (backend)
				result.blocked |= bit;
			else
				signals->pending_signals |= bit;
			continue;
		}
		if (backend && (number == SIGQUIT || number == SIGKILL))
		{
			result.exit_kind = POSTGAMMA_SIGNAL_QUICK_EXIT;
			result.signal_number = number;
			break;
		}
		handler = signals->signal_handlers[number];
		if (handler == POSTGAMMA_SIGNAL_IGNORE)
			continue;
		if (handler == NULL || handler == POSTGAMMA_SIGNAL_DEFAULT)
		{
			if (!backend)
				continue;
			result.exit_kind = POSTGAMMA_SIGNAL_DEFAULT_EXIT;
			result.signal_number = number;
			break;
		}
		handler(number, info);
	}
	signals->dispatching_signals = false;
	return result;
}

int
postgamma_virtual_signal_mask(sigset_t *logical_mask, int how,
							 const sigset_t *set, sigset_t *old_set)
{
	if (old_set != NULL)
		*old_set = *logical_mask;
	if (set == NULL)
		return 0;
	switch (how)
	{
		case SIG_BLOCK:
			for (int signal_number = 1;
				 signal_number <= POSTGAMMA_BACKEND_SIGNAL_COUNT;
				 signal_number++)
			{
				if (sigismember(set, signal_number) == 1)
					sigaddset(logical_mask, signal_number);
			}
			break;
		case SIG_UNBLOCK:
			for (int signal_number = 1;
				 signal_number <= POSTGAMMA_BACKEND_SIGNAL_COUNT;
				 signal_number++)
			{
				if (sigismember(set, signal_number) == 1)
					sigdelset(logical_mask, signal_number);
			}
			break;
		case SIG_SETMASK:
			*logical_mask = *set;
			break;
		default:
			errno = EINVAL;
			return -1;
	}
	return 0;
}


int
postgamma_virtual_timer_set(PostgammaVirtualTimer *timer, int which,
	const struct itimerval *new_value, struct itimerval *old_value, int64_t now)
{
	int64_t		value_us;
	int64_t		interval_us;

	if (which != ITIMER_REAL || new_value == NULL ||
		new_value->it_value.tv_sec < 0 ||
		new_value->it_value.tv_usec < 0 ||
		new_value->it_value.tv_usec >= USECS_PER_SEC ||
		new_value->it_interval.tv_sec < 0 ||
		new_value->it_interval.tv_usec < 0 ||
		new_value->it_interval.tv_usec >= USECS_PER_SEC ||
		new_value->it_value.tv_sec >
		(INT64_MAX - new_value->it_value.tv_usec) / USECS_PER_SEC ||
		new_value->it_interval.tv_sec >
		(INT64_MAX - new_value->it_interval.tv_usec) / USECS_PER_SEC)
	{
		errno = EINVAL;
		return -1;
	}
	if (old_value != NULL)
	{
		int64_t		remaining_us = 0;

		memset(old_value, 0, sizeof(*old_value));
		if (timer->timeout_armed && timer->timeout_due_at > now)
			remaining_us = timer->timeout_due_at - now;
		old_value->it_value.tv_sec = remaining_us / USECS_PER_SEC;
		old_value->it_value.tv_usec = remaining_us % USECS_PER_SEC;
		old_value->it_interval.tv_sec =
			timer->timeout_interval_us / USECS_PER_SEC;
		old_value->it_interval.tv_usec =
			timer->timeout_interval_us % USECS_PER_SEC;
	}
	value_us = (int64_t) new_value->it_value.tv_sec * USECS_PER_SEC +
		new_value->it_value.tv_usec;
	interval_us = (int64_t) new_value->it_interval.tv_sec * USECS_PER_SEC +
		new_value->it_interval.tv_usec;
	if (value_us == 0)
	{
		timer->timeout_wait_shortened = false;
		timer->timeout_interval_us = interval_us;
		timer->timeout_armed = false;
		timer->timeout_due_at = 0;
		return 0;
	}
	if (now > INT64_MAX - value_us)
	{
		errno = EOVERFLOW;
		return -1;
	}
	timer->timeout_wait_shortened = false;
	timer->timeout_interval_us = interval_us;
	timer->timeout_due_at = now + value_us;
	timer->timeout_armed = true;
	return 0;
}

long
postgamma_virtual_timer_adjust_wait(PostgammaVirtualTimer *timer,
								   long requested_timeout, int64_t now)
{
	int64_t		remaining_us;
	int64_t		remaining_ms;

	if (timer == NULL)
		return requested_timeout;
	if (!timer->timeout_armed)
	{
		timer->timeout_wait_shortened = false;
		return requested_timeout;
	}
	remaining_us = timer->timeout_due_at - now;
	if (remaining_us <= 0)
		remaining_ms = 0;
	else
		remaining_ms = (remaining_us + 999) / 1000;
	if (remaining_ms > LONG_MAX)
		remaining_ms = LONG_MAX;
	if (requested_timeout < 0 || remaining_ms < requested_timeout)
	{
		timer->timeout_wait_shortened = true;
		return (long) remaining_ms;
	}
	timer->timeout_wait_shortened = false;
	return requested_timeout;
}

bool
postgamma_virtual_timer_wait_expired(PostgammaVirtualTimer *timer, int wait_result)
{
	bool		shortened;

	if (timer == NULL)
		return false;
	shortened = timer->timeout_wait_shortened;
	timer->timeout_wait_shortened = false;
	return wait_result == -1 && shortened;
}

bool
postgamma_virtual_timer_due(PostgammaVirtualTimer *timer, int64_t now)
{
	int64_t		elapsed;
	int64_t		periods;

	if (timer == NULL || !timer->timeout_armed)
		return false;
	if (now < timer->timeout_due_at)
		return false;
	if (timer->timeout_interval_us <= 0)
	{
		timer->timeout_armed = false;
		timer->timeout_due_at = 0;
		return true;
	}

	elapsed = now - timer->timeout_due_at;
	periods = elapsed / timer->timeout_interval_us + 1;
	if (periods >
		(INT64_MAX - timer->timeout_due_at) /
		timer->timeout_interval_us)
	{
		timer->timeout_armed = false;
		timer->timeout_due_at = 0;
	}
	else
		timer->timeout_due_at += periods * timer->timeout_interval_us;
	return true;
}
