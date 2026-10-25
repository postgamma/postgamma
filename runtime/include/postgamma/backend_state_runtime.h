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

#include "postgamma/contract_runtime.h"
#include "postgamma/thread_runtime.h"


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

/* Immutable facts with static lifetime. Providers must not resolve live state. */
typedef const volatile void *(*PostgammaBackendStateTemplateProvider)(void);
typedef struct PostgammaBackendStatePreparationEntry
{
	PostgammaBackendStateSlot slot;
	PostgammaBackendStateTemplateProvider provider;
	const PostgammaBackendStateSlot *targets;
	size_t target_count;
	bool zero_initialize;
} PostgammaBackendStatePreparationEntry;

typedef struct PostgammaBackendStatePreparation
{
	const char *identity;
	const char *layout_identity;
	const PostgammaBackendStatePreparationEntry *entries;
	size_t count;
} PostgammaBackendStatePreparation;

/*
 * Binding always installs a readable table.  Unbound carriers use an immutable
 * all-NULL table, so a miss still reaches the checked initialization boundary.
 * Only the resolver publishes addresses into the logical execution's table.
 */
/*
 * An executable-only build may avoid the GOT lookup without hiding this
 * symbol from dynamically loaded extensions. Never enable this for objects
 * destined for an SDK archive, a shared library, or a Wasm module.
 */
#if defined(POSTGAMMA_SERVER_LOCAL_EXEC_TLS)
#if !defined(__ELF__) || !(defined(__GNUC__) || defined(__clang__))
#error "server local-exec TLS requires an ELF compiler with tls_model support"
#endif
#define POSTGAMMA_STATE_TLS_MODEL __attribute__((tls_model("local-exec")))
#else
#define POSTGAMMA_STATE_TLS_MODEL
#endif
extern POSTGAMMA_THREAD_LOCAL void *const *PostgammaCurrentBackendStateAddresses
	POSTGAMMA_STATE_TLS_MODEL;

#ifdef POSTGAMMA_DIRECT_ROLE_STATE
#ifndef POSTGAMMA_PREPARED_ROLE_STATE
#error "direct role access requires prepared role state"
#endif
#if !(defined(__GNUC__) || defined(__clang__))
#error "direct role access requires constant-expression and inline support"
#endif
#include "postgamma/backend_state_direct.h"
extern POSTGAMMA_THREAD_LOCAL unsigned char *PostgammaCurrentPreparedRoleState
	POSTGAMMA_STATE_TLS_MODEL;

/* Only the binding boundary publishes this view; taking an address checks it. */
static inline __attribute__((always_inline)) void *
postgamma_backend_state_direct_address_in(
	unsigned char *base, PostgammaBackendStateSlot slot,
	const volatile void *template_address)
{
	size_t offset = postgamma_backend_state_prepared_offset(slot);

	if (POSTGAMMA_UNLIKELY(base == NULL || template_address == NULL || offset == SIZE_MAX))
		postgamma_runtime_contract_violation();
	return base + offset;
}

static inline __attribute__((always_inline)) void *
postgamma_backend_state_direct_address(
	PostgammaBackendStateSlot slot, const volatile void *template_address)
{
	return postgamma_backend_state_direct_address_in(
		PostgammaCurrentPreparedRoleState, slot, template_address);
}

/*
 * Experimental generated implementations may borrow a view for one bound
 * call. It must not escape that call or span a yield. Callbacks must restore
 * the caller's binding before it resumes, including nonlocal error paths.
 * This interface alone does not qualify a call graph for view propagation.
 */
#define POSTGAMMA_BACKEND_STATE_VALUE_IN(view, slot, original) \
	(*(__typeof__(&(original))) \
	 (__builtin_constant_p(slot) && postgamma_backend_state_prepared_offset(slot) != SIZE_MAX ? \
	  postgamma_backend_state_direct_address_in((view), (slot), &(original)) : \
	  postgamma_backend_state_address_cached((slot), &(original))))
#define POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE_IN(view, slot, original, bridge) \
	(*(__typeof__(&(original))) \
	 (__builtin_constant_p(slot) && postgamma_backend_state_prepared_offset(slot) != SIZE_MAX ? \
	  postgamma_backend_state_direct_address_in((view), (slot), &(original)) : \
	  postgamma_backend_state_checked_bridge((slot), &(original), (bridge))))

/*
 * A function may borrow the bound role without changing its C ABI or cloning
 * its body. A NULL entry view must remain lazy: an access-free return is valid
 * and a callback may establish the first binding before the first access.
 * A non-NULL borrow requires every returning callee to restore its binding.
 */
#define POSTGAMMA_BACKEND_STATE_SCOPE_VALUE(view, slot, original) \
	POSTGAMMA_BACKEND_STATE_VALUE_IN( \
		(view) != NULL ? (view) : PostgammaCurrentPreparedRoleState, slot, original)
#define POSTGAMMA_BACKEND_STATE_SCOPE_RELOCATED_VALUE(view, slot, original, bridge) \
	POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE_IN( \
		(view) != NULL ? (view) : PostgammaCurrentPreparedRoleState, slot, original, bridge)
#endif
#undef POSTGAMMA_STATE_TLS_MODEL

int postgamma_backend_state_context_init(PostgammaBackendStateOwner owner,
									void **storage);
#ifdef POSTGAMMA_PRIVATE_MEMORY_BLOCKS
struct PostgammaMemoryPool;
int postgamma_backend_state_context_init_in_pool(PostgammaBackendStateOwner owner,
	void **storage, struct PostgammaMemoryPool *pool);
#endif
#ifdef POSTGAMMA_ROLE_CACHE_BYTES
int postgamma_backend_state_role_acquire(void *instance_storage, void **storage);
void postgamma_backend_state_role_cache_usage(void *instance_storage,
	size_t *bytes, size_t *leases);
#endif
void postgamma_backend_state_context_destroy(void **storage);
void postgamma_backend_state_context_copy_ids(
	void *destination, const void *source,
	const char *const *identifiers, size_t identifier_count);
/* Call after inheritance, before first bind. Failure leaves storage unchanged. */
int postgamma_backend_state_context_prepare(void *storage);
int postgamma_backend_state_context_prepare_plan(
	void *storage, const PostgammaBackendStatePreparation *plan);
void postgamma_backend_state_context_before_bind(void *storage);
/* The configured preparation set, or NULL while it has not been published. */
void *postgamma_backend_state_context_ready_data(void *storage);
/* A NULL result denotes an unpublished or different preparation set. */
void *postgamma_backend_state_prepared_data(
	void *storage, const PostgammaBackendStatePreparation *plan);
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

/* NULL means first access; initialization publishes only completed addresses. */
static inline void *
postgamma_backend_state_cached_address(
	PostgammaBackendStateSlot slot, const volatile void *template_address)
{
	void *const *addresses = PostgammaCurrentBackendStateAddresses;

	if (POSTGAMMA_UNLIKELY(slot < 0 ||
		slot >= POSTGAMMA_BACKEND_STATE_SLOT_COUNT || template_address == NULL))
		postgamma_runtime_contract_violation();
	return addresses[slot];
}

/* Resolve and initialize a slot only on the bound execution's first access. */
static inline void *
postgamma_backend_state_address_cached(
	PostgammaBackendStateSlot slot, const volatile void *template_address)
{
	void *address = postgamma_backend_state_cached_address(slot, template_address);

	if (POSTGAMMA_UNLIKELY(address == NULL))
		address = postgamma_backend_state_address(slot, template_address);
	return address;
}

static inline void *
postgamma_backend_state_address_relocated_cached(
	PostgammaBackendStateSlot slot, const volatile void *template_address,
	const PostgammaBackendStateTemplate *targets, size_t target_count)
{
	void *address;

	if (POSTGAMMA_UNLIKELY(targets == NULL || target_count == 0))
		postgamma_runtime_contract_violation();
	address = postgamma_backend_state_cached_address(slot, template_address);
	if (POSTGAMMA_UNLIKELY(address == NULL))
		address = postgamma_backend_state_address_relocated(
			slot, template_address, targets, target_count);
	return address;
}

#ifdef POSTGAMMA_DIRECT_ROLE_STATE
/* A dynamic/side-effecting slot is evaluated once, through the existing API. */
#define POSTGAMMA_BACKEND_STATE_VALUE(slot, original) \
	(*(__typeof__(&(original))) \
	 (__builtin_constant_p(slot) && postgamma_backend_state_prepared_offset(slot) != SIZE_MAX ? \
	  postgamma_backend_state_direct_address((slot), &(original)) : \
	  postgamma_backend_state_address_cached((slot), &(original))))
#else
#define POSTGAMMA_BACKEND_STATE_VALUE(slot, original) \
	(*(__typeof__(&(original))) postgamma_backend_state_address_cached((slot), &(original)))
#endif

#define POSTGAMMA_BACKEND_STATE_NAMED_VALUE(identifier, original) \
	(*(__typeof__(&(original))) postgamma_backend_state_address_by_id( \
		(identifier), &(original)))

#define POSTGAMMA_BACKEND_STATE_RELOCATED_VALUE(original, bridge) \
	(*(__typeof__(&(original))) (bridge)())

/* Generated accesses carry the slot identity; legacy bridge expressions do not. */
static inline void *
postgamma_backend_state_checked_bridge(
	PostgammaBackendStateSlot slot, const volatile void *template_address,
	void *(*bridge)(void))
{
	if (POSTGAMMA_UNLIKELY(slot < 0 || slot >= POSTGAMMA_BACKEND_STATE_SLOT_COUNT ||
						  template_address == NULL || bridge == NULL))
		postgamma_runtime_contract_violation();
	return bridge();
}

#ifdef POSTGAMMA_DIRECT_ROLE_STATE
#define POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE(slot, original, bridge) \
	(*(__typeof__(&(original))) \
	 (__builtin_constant_p(slot) && postgamma_backend_state_prepared_offset(slot) != SIZE_MAX ? \
	  postgamma_backend_state_direct_address((slot), &(original)) : \
	  postgamma_backend_state_checked_bridge((slot), &(original), (bridge))))
#else
#define POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE(slot, original, bridge) \
	(*(__typeof__(&(original))) \
	 postgamma_backend_state_checked_bridge((slot), &(original), (bridge)))
#endif

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
