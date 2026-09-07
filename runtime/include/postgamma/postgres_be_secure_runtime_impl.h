/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_be_secure_runtime_impl.h
 *    PostgreSQL-local wait adapter for memory-backed client sessions.
 *
 * A role wake descriptor is readable for both inbound bytes and newly
 * available outbound capacity.  Keep PostgreSQL's socket wait position but
 * map its writable interest to readable readiness for this provider.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_BE_SECURE_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_BE_SECURE_RUNTIME_IMPL_H


static void
postgamma_backend_modify_transport_wait_event(
	WaitEventSet *set, int position, uint32 events, void *user_data)
{
	if (postgamma_backend_transport_is_bound() &&
		position == FeBeWaitSetSocketPos &&
		(events & WL_SOCKET_WRITEABLE) != 0)
	{
		events &= ~WL_SOCKET_WRITEABLE;
		events |= WL_SOCKET_READABLE;
	}
	(ModifyWaitEvent)(set, position, events, user_data);
}


#define ModifyWaitEvent(set, position, events, user_data) \
	postgamma_backend_modify_transport_wait_event( \
		(set), (position), (events), (user_data))


#endif /* POSTGAMMA_POSTGRES_BE_SECURE_RUNTIME_IMPL_H */
