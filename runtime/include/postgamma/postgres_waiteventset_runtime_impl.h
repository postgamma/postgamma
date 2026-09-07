/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_waiteventset_runtime_impl.h
 *    Thread-exit bridge for PostgreSQL's file-local wait support.
 *
 * This implementation header is included exactly once, by waiteventset.c in
 * the generated tree.  Platform-specific descriptors must be released when
 * a backend execution ends because the containing process remains alive.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_WAITEVENTSET_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_WAITEVENTSET_RUNTIME_IMPL_H

#include <errno.h>
#include <limits.h>


static char PostgammaBackendWakeEventTag;


static int
postgamma_backend_wait_event_capacity(int requested_capacity)
{
	if (postgamma_backend_current_wake_fd() < 0)
		return requested_capacity;
	if (requested_capacity == INT_MAX)
		elog(FATAL, "postgamma backend wait-event capacity overflow");
	return requested_capacity + 1;
}


static void
postgamma_backend_attach_wait_event(WaitEventSet *set)
{
	int			wake_fd = postgamma_backend_current_wake_fd();

	if (wake_fd < 0)
		return;
	for (int index = 0; index < set->nevents; index++)
	{
		if (set->events[index].user_data == &PostgammaBackendWakeEventTag ||
			set->events[index].fd == wake_fd)
			return;
	}
	AddWaitEventToSet(
		set, WL_SOCKET_READABLE, wake_fd, NULL,
		&PostgammaBackendWakeEventTag);
}


static int
postgamma_backend_filter_wake_events(WaitEvent *events, int event_count)
{
	int			kept = 0;
	int			wake_fd = postgamma_backend_current_wake_fd();

	if (event_count <= 0)
		return event_count;
	for (int index = 0; index < event_count; index++)
	{
		if (events[index].user_data == &PostgammaBackendWakeEventTag)
		{
			uint64_t	wake_count = 0;
			int			status = postgamma_backend_current_wake_drain(
				&wake_count);

			if (status != 0 && status != ENOENT)
				elog(FATAL, "postgamma backend wake drain failed: %s",
					 strerror(status));
			(void) wake_count;
			continue;
		}
		if (postgamma_backend_transport_is_bound() &&
			events[index].fd == wake_fd)
		{
			uint64_t	wake_count = 0;
			int			status = postgamma_backend_current_wake_drain(
				&wake_count);

			if (status != 0 && status != ENOENT)
				elog(FATAL, "postgamma transport wake drain failed: %s",
					 strerror(status));
			if (kept != index)
				events[kept] = events[index];
			kept++;
			continue;
		}
		if (kept != index)
			events[kept] = events[index];
		kept++;
	}
	return kept;
}


#define POSTGAMMA_WAIT_EVENT_STATE(name) \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE( \
		"internal:src/backend/storage/ipc/waiteventset.c:" \
		"src/backend/storage/ipc/waiteventset.c::" #name, name)

void
postgamma_shutdown_wait_event_support(void)
{
#ifndef WIN32
	POSTGAMMA_WAIT_EVENT_STATE(waiting) = false;
#endif

#ifdef WAIT_USE_SELF_PIPE
	if (POSTGAMMA_WAIT_EVENT_STATE(selfpipe_readfd) != -1)
	{
		(void) close(POSTGAMMA_WAIT_EVENT_STATE(selfpipe_readfd));
		POSTGAMMA_WAIT_EVENT_STATE(selfpipe_readfd) = -1;
		ReleaseExternalFD();
	}
	if (POSTGAMMA_WAIT_EVENT_STATE(selfpipe_writefd) != -1)
	{
		(void) close(POSTGAMMA_WAIT_EVENT_STATE(selfpipe_writefd));
		POSTGAMMA_WAIT_EVENT_STATE(selfpipe_writefd) = -1;
		ReleaseExternalFD();
	}
	POSTGAMMA_WAIT_EVENT_STATE(selfpipe_owner_pid) = 0;
#endif

#ifdef WAIT_USE_SIGNALFD
	if (POSTGAMMA_WAIT_EVENT_STATE(signal_fd) != -1)
	{
		(void) close(POSTGAMMA_WAIT_EVENT_STATE(signal_fd));
		POSTGAMMA_WAIT_EVENT_STATE(signal_fd) = -1;
		ReleaseExternalFD();
	}
#endif
}

#undef POSTGAMMA_WAIT_EVENT_STATE

#endif /* POSTGAMMA_POSTGRES_WAITEVENTSET_RUNTIME_IMPL_H */
