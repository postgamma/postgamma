/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_KERNEL_SUPERVISOR_ADAPTER_H
#define POSTGAMMA_PRIVATE_KERNEL_SUPERVISOR_ADAPTER_H

#include "postgamma/embedded_kernel.h"
#include "postgamma/private/supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

int postgamma_kernel_supervisor_provider_init(
	PostgammaSupervisor *supervisor,
	PostgammaKernelHostProvider *provider);

#ifdef __cplusplus
}
#endif

#endif
