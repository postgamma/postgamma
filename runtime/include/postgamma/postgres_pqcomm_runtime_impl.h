/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_pqcomm_runtime_impl.h
 *    PostgreSQL-local address adapter for memory-backed client sessions.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_PQCOMM_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_PQCOMM_RUNTIME_IMPL_H


#define getsockname(socket_descriptor, address, address_length) \
	postgamma_backend_transport_getsockname( \
		(socket_descriptor), (address), (address_length))


#endif /* POSTGAMMA_POSTGRES_PQCOMM_RUNTIME_IMPL_H */
