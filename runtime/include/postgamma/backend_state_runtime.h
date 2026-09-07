/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * backend_state_runtime.h
 *    Runtime API for PostgreSQL backend-state virtualization.
 *
 * The PostgreSQL-dependent slot layout is generated separately.  This file
 * and its implementation contain only version-independent runtime logic.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_BACKEND_STATE_RUNTIME_H
#define POSTGAMMA_BACKEND_STATE_RUNTIME_H

#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef enum PostgammaBackendStateOwner
{
	POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE = 1,
	POSTGAMMA_BACKEND_STATE_OWNER_ROLE = 2,
	POSTGAMMA_BACKEND_STATE_OWNER_SESSION = 3
} PostgammaBackendStateOwner;

/* Generated enum values, storage sizes, and relocation declarations. */
#include "postgamma/backend_state_layout.h"

typedef struct PostgammaBackendStateTemplate
{
	PostgammaBackendStateSlot slot;
	const volatile void *address;
} PostgammaBackendStateTemplate;

int postgamma_backend_state_context_init(PostgammaBackendStateOwner owner,
									void **storage);
void postgamma_backend_state_context_destroy(void **storage);
void postgamma_backend_state_context_copy_ids(
	void *destination, const void *source,
	const char *const *identifiers, size_t identifier_count);
void *postgamma_backend_state_address(PostgammaBackendStateSlot slot,
									 const volatile void *template_address);
void *postgamma_backend_state_address_by_id(
	const char *identifier, const volatile void *template_address);
void *postgamma_backend_state_address_relocated(
	PostgammaBackendStateSlot slot, const volatile void *template_address,
	const PostgammaBackendStateTemplate *targets, size_t target_count);
void *postgamma_extension_role_state_address(
	const char *identifier, const volatile void *template_address,
	size_t size, size_t alignment);
void postgamma_extension_role_state_destroy(void **state);

#define POSTGAMMA_BACKEND_STATE_VALUE(slot, original) \
	(*(__typeof__(&(original))) postgamma_backend_state_address((slot), &(original)))

#define POSTGAMMA_BACKEND_STATE_NAMED_VALUE(identifier, original) \
	(*(__typeof__(&(original))) postgamma_backend_state_address_by_id( \
		(identifier), &(original)))

#define POSTGAMMA_BACKEND_STATE_RELOCATED_VALUE(original, bridge) \
	(*(__typeof__(&(original))) (bridge)())

/*
 * accessor must turn a recoverable allocation failure into the extension's
 * native error mechanism before returning.  The private runtime function
 * returns NULL and sets errno; it never terminates the host process.
 */
#define POSTGAMMA_EXTENSION_ROLE_STATE_VALUE( \
		accessor, identifier, original) \
	(*(__typeof__(&(original))) (accessor)( \
		(identifier), &(original), sizeof(original), \
		__alignof__(__typeof__(original))))

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_BACKEND_STATE_RUNTIME_H */
