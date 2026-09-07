/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * tool_state_runtime.c
 *    Invocation-owned storage for transformed PostgreSQL frontend tools.
 *
 *-------------------------------------------------------------------------
 */

#include "postgamma/tool_state_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define POSTGAMMA_TOOL_STATE_MAGIC UINT64_C(0x5047544f4f4c5354)


typedef struct PostgammaToolStateDescriptor
{
	const char *identifier;
	size_t		offset;
	size_t		size;
	size_t		alignment;
	bool		copy_template;
	bool		has_relocations;
} PostgammaToolStateDescriptor;

typedef struct PostgammaToolStateStore
{
	uint64_t	magic;
	unsigned char *initialized;
	unsigned char *allocation;
	unsigned char *data;
} PostgammaToolStateStore;


static const PostgammaToolStateDescriptor PostgammaToolStateDescriptors[] =
{
#include "postgamma/tool_state_descriptors.inc"
};

static _Thread_local PostgammaToolStateStore *PostgammaCurrentToolState;
static _Thread_local PostgammaToolStateFailureFunction PostgammaToolStateFailure;
static _Thread_local void *PostgammaToolStateFailureArgument;


static void *postgamma_tool_state_address_internal(
	PostgammaToolStateSlot slot, const volatile void *template_address,
	const PostgammaToolStateTemplate *targets, size_t target_count);


static bool
postgamma_tool_state_valid(const PostgammaToolStateStore *state)
{
	return state != NULL && state->magic == POSTGAMMA_TOOL_STATE_MAGIC &&
		state->initialized != NULL && state->allocation != NULL &&
		state->data != NULL;
}


static void *
postgamma_tool_state_fail(const volatile void *template_address, int status)
{
	PostgammaToolStateFailureFunction failure = PostgammaToolStateFailure;
	void	   *argument = PostgammaToolStateFailureArgument;

	if (failure != NULL)
		failure(argument, status != 0 ? status : EPROTO);
	/* A host callback must not return.  Preserve host safety if it does. */
	return (void *) template_address;
}


int
postgamma_tool_state_create(void **state)
{
	PostgammaToolStateStore *created;
	size_t		allocation_size;
	size_t		padding;
	uintptr_t	address;

	if (state == NULL || *state != NULL)
		return EINVAL;
	if (POSTGAMMA_TOOL_STATE_ALIGNMENT == 0 ||
		POSTGAMMA_TOOL_STATE_BYTES >
		SIZE_MAX - (POSTGAMMA_TOOL_STATE_ALIGNMENT - 1))
		return EOVERFLOW;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->initialized = calloc(
		POSTGAMMA_TOOL_STATE_SLOT_COUNT == 0 ? 1 :
		POSTGAMMA_TOOL_STATE_SLOT_COUNT, 1);
	allocation_size = POSTGAMMA_TOOL_STATE_BYTES == 0 ? 1 :
		POSTGAMMA_TOOL_STATE_BYTES + POSTGAMMA_TOOL_STATE_ALIGNMENT - 1;
	created->allocation = calloc(allocation_size, 1);
	if (created->initialized == NULL || created->allocation == NULL)
	{
		free(created->allocation);
		free(created->initialized);
		free(created);
		return ENOMEM;
	}
	address = (uintptr_t) created->allocation;
	padding = (POSTGAMMA_TOOL_STATE_ALIGNMENT -
		address % POSTGAMMA_TOOL_STATE_ALIGNMENT) %
		POSTGAMMA_TOOL_STATE_ALIGNMENT;
	created->data = created->allocation + padding;
	created->magic = POSTGAMMA_TOOL_STATE_MAGIC;
	*state = created;
	return 0;
}


void
postgamma_tool_state_destroy(void **state)
{
	PostgammaToolStateStore *store;

	if (state == NULL || *state == NULL)
		return;
	store = *state;
	if (!postgamma_tool_state_valid(store) || store == PostgammaCurrentToolState)
		return;
	store->magic = 0;
	free(store->allocation);
	free(store->initialized);
	free(store);
	*state = NULL;
}


int
postgamma_tool_state_bind(
	void *state, PostgammaToolStateFailureFunction failure, void *argument)
{
	PostgammaToolStateStore *store = state;

	if (!postgamma_tool_state_valid(store) || failure == NULL ||
		PostgammaCurrentToolState != NULL)
		return EINVAL;
	PostgammaCurrentToolState = store;
	PostgammaToolStateFailure = failure;
	PostgammaToolStateFailureArgument = argument;
	return 0;
}


int
postgamma_tool_state_unbind(void *expected_state)
{
	if (!postgamma_tool_state_valid(expected_state) ||
		PostgammaCurrentToolState != expected_state)
		return EINVAL;
	PostgammaCurrentToolState = NULL;
	PostgammaToolStateFailure = NULL;
	PostgammaToolStateFailureArgument = NULL;
	return 0;
}


static void *
postgamma_tool_state_address_internal(
	PostgammaToolStateSlot slot, const volatile void *template_address,
	const PostgammaToolStateTemplate *targets, size_t target_count)
{
	const PostgammaToolStateDescriptor *descriptor;
	PostgammaToolStateStore *store = PostgammaCurrentToolState;
	unsigned char *address;
	size_t		target_index;

	if (!postgamma_tool_state_valid(store) || slot < 0 ||
		slot >= POSTGAMMA_TOOL_STATE_SLOT_COUNT || template_address == NULL)
		return postgamma_tool_state_fail(template_address, EPROTO);
	descriptor = &PostgammaToolStateDescriptors[slot];
	if (descriptor->offset + descriptor->size > POSTGAMMA_TOOL_STATE_BYTES ||
		descriptor->alignment == 0 ||
		((uintptr_t) (store->data + descriptor->offset) %
		 descriptor->alignment) != 0)
		return postgamma_tool_state_fail(template_address, EPROTO);
	address = store->data + descriptor->offset;
	if (store->initialized[slot] == 2)
		return postgamma_tool_state_fail(template_address, ELOOP);
	if (store->initialized[slot] == 0)
	{
		const unsigned char *template_bytes =
			(const unsigned char *) template_address;

		store->initialized[slot] = 2;
		if (descriptor->copy_template)
			memcpy(address, (const void *) template_address, descriptor->size);
		else if (target_count != 0)
			return postgamma_tool_state_fail(template_address, EPROTO);
		for (target_index = 0; target_index < target_count; target_index++)
		{
			const PostgammaToolStateTemplate *target = &targets[target_index];
			const PostgammaToolStateDescriptor *target_descriptor;
			void	   *target_address;
			uintptr_t	old_base = (uintptr_t) target->address;
			uintptr_t	old_limit;
			uintptr_t	new_base;
			size_t		match_count = 0;
			size_t		offset;

			if (target->slot < 0 ||
				target->slot >= POSTGAMMA_TOOL_STATE_SLOT_COUNT ||
				target->expected_matches == 0)
				return postgamma_tool_state_fail(template_address, EPROTO);
			target_descriptor = &PostgammaToolStateDescriptors[target->slot];
			if (store->initialized[target->slot] == 2)
				target_address = store->data + target_descriptor->offset;
			else
				target_address = postgamma_tool_state_address_internal(
					target->slot, target->address, NULL, 0);
			if (target_descriptor->size > UINTPTR_MAX - old_base)
				return postgamma_tool_state_fail(template_address, EOVERFLOW);
			old_limit = old_base + target_descriptor->size;
			new_base = (uintptr_t) target_address;
			for (offset = 0;
				 offset + sizeof(uintptr_t) <= descriptor->size;
				 offset += sizeof(uintptr_t))
			{
				uintptr_t value;

				/*
				 * Read the immutable template, not the destination being
				 * rewritten.  This prevents one relocation from creating a
				 * shifted or cross-target match later in the scan.
				 */
				memcpy(&value, template_bytes + offset, sizeof(value));
				if (value >= old_base && value < old_limit)
				{
					uintptr_t delta = value - old_base;
					uintptr_t new_pointer;

					if (delta > UINTPTR_MAX - new_base)
						return postgamma_tool_state_fail(
							template_address, EOVERFLOW);
					new_pointer = new_base + delta;
					memcpy(address + offset, &new_pointer, sizeof(new_pointer));
					match_count++;
				}
			}
			/*
			 * The AST-derived count turns a pointer-like integer, packed
			 * pointer, or unsupported one-past pointer into a loud contract
			 * failure instead of silent state corruption.
			 */
			if (match_count != target->expected_matches)
				return postgamma_tool_state_fail(template_address, EPROTO);
		}
		store->initialized[slot] = 1;
	}
	return address;
}


void *
postgamma_tool_state_address(
	PostgammaToolStateSlot slot, const volatile void *template_address)
{
	return postgamma_tool_state_address_internal(
		slot, template_address, NULL, 0);
}


void *
postgamma_tool_state_address_relocated(
	PostgammaToolStateSlot slot, const volatile void *template_address,
	const PostgammaToolStateTemplate *targets, size_t target_count)
{
	if (targets == NULL || target_count == 0)
		return postgamma_tool_state_fail(template_address, EINVAL);
	return postgamma_tool_state_address_internal(
		slot, template_address, targets, target_count);
}
