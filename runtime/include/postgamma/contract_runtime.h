/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_CONTRACT_RUNTIME_H
#define POSTGAMMA_CONTRACT_RUNTIME_H

/*
 * Fail immediately when an internal runtime invariant is violated.
 *
 * This declaration intentionally lives in a dependency-free header so that
 * integration code can use the fail-fast boundary without importing the
 * generated state layouts owned by guc_runtime.h.
 */
void postgamma_runtime_contract_violation(void);

#endif /* POSTGAMMA_CONTRACT_RUNTIME_H */
