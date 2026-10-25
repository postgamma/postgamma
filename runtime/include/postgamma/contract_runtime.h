/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_CONTRACT_RUNTIME_H
#define POSTGAMMA_CONTRACT_RUNTIME_H

#if defined(__GNUC__) || defined(__clang__)
#define POSTGAMMA_UNLIKELY(condition) __builtin_expect(!!(condition), 0)
#else
#define POSTGAMMA_UNLIKELY(condition) (condition)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fail immediately when an internal runtime invariant is violated.
 *
 * This declaration intentionally lives in a dependency-free header so that
 * integration code can use the fail-fast boundary without importing the
 * generated state layouts owned by guc_runtime.h.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn, cold))
#elif defined(__cplusplus)
[[noreturn]]
#else
_Noreturn
#endif
void postgamma_runtime_contract_violation(void);

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_CONTRACT_RUNTIME_H */
