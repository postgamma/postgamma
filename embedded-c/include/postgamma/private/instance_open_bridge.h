/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_INSTANCE_OPEN_BRIDGE_H
#define POSTGAMMA_PRIVATE_INSTANCE_OPEN_BRIDGE_H

#include <stdint.h>


/* Private use of the frozen instance-options reserve slot by bundled bindings. */
#define POSTGAMMA_PRIVATE_INSTANCE_OPEN_REQUIRE_MISSING \
	UINT32_C(0x50474D01)


#endif /* POSTGAMMA_PRIVATE_INSTANCE_OPEN_BRIDGE_H */
