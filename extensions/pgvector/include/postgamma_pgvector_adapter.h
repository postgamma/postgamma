/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PGVECTOR_ADAPTER_H
#define POSTGAMMA_PGVECTOR_ADAPTER_H

#include "postgres.h"

#include "storage/lwlock.h"


/*
 * PostgreSQL's generated LWLock names are source macros whose bodies refer to
 * MainLWLockArray.  A consumer such as pgvector only spells the lock-name
 * macro, so a source-level AST rewrite cannot replace the hidden global
 * reference.  Bind that kernel macro dependency to the generated role-state
 * accessor after PostgreSQL has declared the original symbol.
 */
#define MainLWLockArray POSTGAMMA_BACKEND_STATE_GLOBAL_MainLWLockArray


/* Error-reporting bridge used by generated pgvector state replacements. */
void *postgamma_pgvector_role_state_address(
	const char *identifier, const volatile void *template_address,
	size_t size, size_t alignment);

#endif /* POSTGAMMA_PGVECTOR_ADAPTER_H */
