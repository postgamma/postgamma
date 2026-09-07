/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_SERVER_TRANSPORT_ADAPTER_H
#define POSTGAMMA_PRIVATE_SERVER_TRANSPORT_ADAPTER_H

#include "postgamma/embedded_kernel.h"
#include "postgamma/private/memory_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

int postgamma_memory_server_connect_init(
	uint64_t generation,
	PostgammaMemoryEndpoint *backend_endpoint,
	PostgammaKernelConnectRequest *request);

#ifdef __cplusplus
}
#endif

#endif
