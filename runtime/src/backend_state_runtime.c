/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * backend_state_runtime.c
 *    Runtime implementation for PostgreSQL backend-state virtualization.
 *
 * PostgreSQL-specific state facts are supplied by generated layout files.
 * No PostgreSQL-version-specific algorithm belongs in this implementation.
 *
 *-------------------------------------------------------------------------
 */

#include "postgamma/guc_runtime.h"
#include "postgamma/thread_runtime.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define POSTGAMMA_BACKEND_STATE_MAGIC UINT64_C(0x504742454E445354)


typedef struct PostgammaBackendStateDescriptor
{
	const char *identifier;
	PostgammaBackendStateOwner owner;
	size_t		local_index;
	size_t		offset;
	size_t		size;
	size_t		alignment;
	bool		copy_template;
	bool		has_relocations;
} PostgammaBackendStateDescriptor;

typedef struct PostgammaBackendStateStore
{
	uint64_t	magic;
	PostgammaBackendStateOwner owner;
	size_t		data_size;
	size_t		slot_count;
	_Atomic unsigned char *initialized;
	PostgammaMutex *initialization_mutex;
	unsigned char *data_allocation;
	unsigned char *data;
} PostgammaBackendStateStore;

typedef struct PostgammaExtensionRoleStateEntry
{
	struct PostgammaExtensionRoleStateEntry *next;
	char	   *identifier;
	const volatile void *template_address;
	void	   *allocation;
	void	   *value;
	size_t		size;
	size_t		alignment;
} PostgammaExtensionRoleStateEntry;


#define POSTGAMMA_EXTENSION_STATE_IDENTIFIER_MAX 255


static const PostgammaBackendStateDescriptor PostgammaBackendStateDescriptors[] =
{
#include "postgamma/backend_state_descriptors.inc"
};


static bool
postgamma_alignment_is_valid(size_t alignment)
{
	return alignment != 0 &&
		(alignment & (alignment - 1)) == 0;
}


static void *
postgamma_aligned_allocation(
	size_t size, size_t alignment, void **allocation)
{
	void	   *raw;
	uintptr_t	address;
	uintptr_t	aligned;

	if (allocation == NULL || size == 0 ||
		!postgamma_alignment_is_valid(alignment) ||
		size > SIZE_MAX - (alignment - 1))
	{
		errno = EINVAL;
		return NULL;
	}
	raw = malloc(size + alignment - 1);
	if (raw == NULL)
	{
		errno = ENOMEM;
		return NULL;
	}
	address = (uintptr_t) raw;
	aligned = (address + alignment - 1) & ~(uintptr_t) (alignment - 1);
	*allocation = raw;
	return (void *) aligned;
}


static const PostgammaBackendStateDescriptor *
postgamma_backend_state_descriptor_by_id(const char *identifier)
{
	size_t		low = 0;
	size_t		high = sizeof(PostgammaBackendStateDescriptors) /
		sizeof(PostgammaBackendStateDescriptors[0]);

	while (low < high)
	{
		size_t		middle = low + (high - low) / 2;
		int			comparison = strcmp(identifier,
								PostgammaBackendStateDescriptors[middle].identifier);

		if (comparison < 0)
			high = middle;
		else if (comparison > 0)
			low = middle + 1;
		else
			return &PostgammaBackendStateDescriptors[middle];
	}
	return NULL;
}


static size_t
postgamma_backend_state_bytes(PostgammaBackendStateOwner owner)
{
	switch (owner)
	{
		case POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE:
			return POSTGAMMA_BACKEND_INSTANCE_STATE_BYTES;
		case POSTGAMMA_BACKEND_STATE_OWNER_ROLE:
			return POSTGAMMA_BACKEND_ROLE_STATE_BYTES;
		case POSTGAMMA_BACKEND_STATE_OWNER_SESSION:
			return POSTGAMMA_BACKEND_SESSION_STATE_BYTES;
	}
	postgamma_runtime_contract_violation();
	return 0;
}


static size_t
postgamma_backend_state_slots(PostgammaBackendStateOwner owner)
{
	switch (owner)
	{
		case POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE:
			return POSTGAMMA_BACKEND_INSTANCE_STATE_SLOTS;
		case POSTGAMMA_BACKEND_STATE_OWNER_ROLE:
			return POSTGAMMA_BACKEND_ROLE_STATE_SLOTS;
		case POSTGAMMA_BACKEND_STATE_OWNER_SESSION:
			return POSTGAMMA_BACKEND_SESSION_STATE_SLOTS;
	}
	postgamma_runtime_contract_violation();
	return 0;
}


static size_t
postgamma_backend_state_alignment(PostgammaBackendStateOwner owner)
{
	size_t		alignment = 1;
	size_t		index;

	for (index = 0;
		 index < sizeof(PostgammaBackendStateDescriptors) /
		 sizeof(PostgammaBackendStateDescriptors[0]);
		 index++)
	{
		const PostgammaBackendStateDescriptor *descriptor =
			&PostgammaBackendStateDescriptors[index];

		if (descriptor->owner == owner && descriptor->alignment > alignment)
			alignment = descriptor->alignment;
	}
	return alignment;
}


int
postgamma_backend_state_context_init(PostgammaBackendStateOwner owner,
									void **storage)
{
	PostgammaBackendStateStore *store;
	size_t		alignment;
	size_t		allocation_size;
	size_t		data_size;
	size_t		index;
	size_t		initialization_count;
	size_t		slot_count;
	size_t		padding;
	uintptr_t	allocation_address;
	int			status;

	if (storage == NULL || *storage != NULL)
		postgamma_runtime_contract_violation();
	data_size = postgamma_backend_state_bytes(owner);
	slot_count = postgamma_backend_state_slots(owner);
	initialization_count = slot_count == 0 ? 1 : slot_count;
	alignment = postgamma_backend_state_alignment(owner);
	if (alignment == 0 || data_size > SIZE_MAX - (alignment - 1))
		postgamma_runtime_contract_violation();
	allocation_size = data_size == 0 ? 1 : data_size + alignment - 1;
	store = calloc(1, sizeof(*store));
	if (store == NULL)
		return ENOMEM;
	store->initialized = calloc(
		initialization_count, sizeof(*store->initialized));
	store->data_allocation = calloc(allocation_size, 1);
	if (store->initialized == NULL || store->data_allocation == NULL)
	{
		free(store->initialized);
		free(store->data_allocation);
		free(store);
		return ENOMEM;
	}
	/* Zeroed storage does not replace C11 atomic object initialization. */
	for (index = 0; index < initialization_count; index++)
		atomic_init(&store->initialized[index], 0);
	status = owner == POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE ?
		postgamma_mutex_create(&store->initialization_mutex) : 0;
	if (status != 0)
	{
		free(store->initialized);
		free(store->data_allocation);
		free(store);
		return status;
	}
	allocation_address = (uintptr_t) store->data_allocation;
	padding = (alignment - allocation_address % alignment) % alignment;
	store->data = store->data_allocation + padding;
	store->magic = POSTGAMMA_BACKEND_STATE_MAGIC;
	store->owner = owner;
	store->data_size = data_size;
	store->slot_count = slot_count;
	*storage = store;
	return 0;
}


void
postgamma_backend_state_context_destroy(void **storage)
{
	PostgammaBackendStateStore *store;

	if (storage == NULL || *storage == NULL)
		return;
	store = *storage;
	if (store->magic != POSTGAMMA_BACKEND_STATE_MAGIC)
		postgamma_runtime_contract_violation();
	if (store->initialization_mutex != NULL &&
		postgamma_mutex_destroy(store->initialization_mutex) != 0)
		postgamma_runtime_contract_violation();
	store->magic = 0;
	free(store->initialized);
	free(store->data_allocation);
	free(store);
	*storage = NULL;
}


void
postgamma_backend_state_context_copy_ids(
	void *destination, const void *source,
	const char *const *identifiers, size_t identifier_count)
{
	PostgammaBackendStateStore *destination_store = destination;
	const PostgammaBackendStateStore *source_store = source;
	size_t		index;

	if (destination_store == NULL || source_store == NULL ||
		destination_store == source_store ||
		(identifier_count != 0 && identifiers == NULL) ||
		destination_store->magic != POSTGAMMA_BACKEND_STATE_MAGIC ||
		source_store->magic != POSTGAMMA_BACKEND_STATE_MAGIC ||
		destination_store->owner != source_store->owner ||
		destination_store->data_size != source_store->data_size ||
		destination_store->slot_count != source_store->slot_count)
		postgamma_runtime_contract_violation();

	for (index = 0; index < identifier_count; index++)
	{
		const PostgammaBackendStateDescriptor *descriptor;

		if (identifiers[index] == NULL)
			postgamma_runtime_contract_violation();
		descriptor = postgamma_backend_state_descriptor_by_id(identifiers[index]);
		if (descriptor == NULL || descriptor->owner != source_store->owner ||
			descriptor->has_relocations ||
			descriptor->local_index >= source_store->slot_count ||
			descriptor->offset + descriptor->size > source_store->data_size ||
			atomic_load_explicit(
				&source_store->initialized[descriptor->local_index],
				memory_order_acquire) == 2 ||
			atomic_load_explicit(
				&destination_store->initialized[descriptor->local_index],
				memory_order_acquire) != 0)
			postgamma_runtime_contract_violation();
		if (atomic_load_explicit(
				&source_store->initialized[descriptor->local_index],
				memory_order_acquire) == 1)
		{
			memcpy(destination_store->data + descriptor->offset,
				   source_store->data + descriptor->offset, descriptor->size);
			atomic_store_explicit(
				&destination_store->initialized[descriptor->local_index], 1,
				memory_order_release);
		}
	}
}


static PostgammaBackendStateStore *
postgamma_backend_state_store(PostgammaBackendStateOwner owner)
{
	PostgammaExecutionContext *execution = postgamma_execution_context_require();
	PostgammaBackendStateStore *store = NULL;

	switch (owner)
	{
		case POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE:
			store = execution->instance->postgres_backend_state;
			break;
		case POSTGAMMA_BACKEND_STATE_OWNER_ROLE:
			store = execution->role->postgres_backend_state;
			break;
		case POSTGAMMA_BACKEND_STATE_OWNER_SESSION:
			store = execution->postgres_backend_state;
			break;
	}
	if (store == NULL || store->magic != POSTGAMMA_BACKEND_STATE_MAGIC ||
		store->owner != owner)
		postgamma_runtime_contract_violation();
	return store;
}


static void *
postgamma_backend_state_address_internal(
	PostgammaBackendStateSlot slot, const volatile void *template_address,
	const PostgammaBackendStateTemplate *targets, size_t target_count,
	bool store_locked)
{
	const PostgammaBackendStateDescriptor *descriptor;
	PostgammaBackendStateStore *store;
	unsigned char *address;
	size_t		target_index;
	unsigned char initialization_state;
	bool		locked_here = false;

	if (slot < 0 || slot >= POSTGAMMA_BACKEND_STATE_SLOT_COUNT ||
		template_address == NULL)
		postgamma_runtime_contract_violation();
	descriptor = &PostgammaBackendStateDescriptors[slot];
	store = postgamma_backend_state_store(descriptor->owner);
	if (descriptor->local_index >= store->slot_count ||
		descriptor->offset + descriptor->size > store->data_size ||
		((uintptr_t) (store->data + descriptor->offset) % descriptor->alignment) != 0)
		postgamma_runtime_contract_violation();
	address = store->data + descriptor->offset;
	initialization_state = atomic_load_explicit(
		&store->initialized[descriptor->local_index], memory_order_acquire);
	if (store->initialization_mutex != NULL && initialization_state != 1 &&
		!store_locked)
	{
		if (postgamma_mutex_lock(store->initialization_mutex) != 0)
			postgamma_runtime_contract_violation();
		locked_here = true;
		initialization_state = atomic_load_explicit(
			&store->initialized[descriptor->local_index], memory_order_acquire);
	}
	if (initialization_state == 2)
		postgamma_runtime_contract_violation();
	if (initialization_state == 0)
	{
		atomic_store_explicit(
			&store->initialized[descriptor->local_index], 2,
			memory_order_relaxed);
		if (descriptor->copy_template)
			memcpy(address, (const void *) template_address, descriptor->size);
		for (target_index = 0; target_index < target_count; target_index++)
		{
			const PostgammaBackendStateTemplate *target = &targets[target_index];
			const PostgammaBackendStateDescriptor *target_descriptor;
			void	   *target_address;
			uintptr_t	old_pointer = (uintptr_t) target->address;
			uintptr_t	new_pointer;
			bool		relocated = false;
			size_t		offset;

			if (target->slot < 0 ||
				target->slot >= POSTGAMMA_BACKEND_STATE_SLOT_COUNT)
				postgamma_runtime_contract_violation();
			target_descriptor = &PostgammaBackendStateDescriptors[target->slot];
			if (target_descriptor->owner != descriptor->owner)
				postgamma_runtime_contract_violation();
			if (target_descriptor->local_index >= store->slot_count ||
				target_descriptor->offset + target_descriptor->size > store->data_size ||
				((uintptr_t) (store->data + target_descriptor->offset) %
				 target_descriptor->alignment) != 0)
				postgamma_runtime_contract_violation();
			/*
			 * A static aggregate can point to itself, and relocation graphs can
			 * contain cycles.  An initializing slot already owns its final
			 * address, so use that address directly instead of recursively
			 * initializing it a second time.
			 */
			if (atomic_load_explicit(
					&store->initialized[target_descriptor->local_index],
					memory_order_acquire) == 2)
				target_address = store->data + target_descriptor->offset;
			else
				target_address = postgamma_backend_state_address_internal(
					target->slot, target->address, NULL, 0,
					store_locked || locked_here);
			new_pointer = (uintptr_t) target_address;
			for (offset = 0;
				 offset + sizeof(uintptr_t) <= descriptor->size;
				 offset++)
			{
				uintptr_t	value;

				memcpy(&value, address + offset, sizeof(value));
				if (value == old_pointer)
				{
					memcpy(address + offset, &new_pointer, sizeof(new_pointer));
					relocated = true;
				}
			}
			if (!relocated)
				postgamma_runtime_contract_violation();
		}
		atomic_store_explicit(
			&store->initialized[descriptor->local_index], 1,
			memory_order_release);
	}
	if (locked_here && postgamma_mutex_unlock(store->initialization_mutex) != 0)
		postgamma_runtime_contract_violation();
	return address;
}


void *
postgamma_backend_state_address(PostgammaBackendStateSlot slot,
								const volatile void *template_address)
{
	return postgamma_backend_state_address_internal(
		slot, template_address, NULL, 0, false);
}


void *
postgamma_backend_state_address_by_id(
	const char *identifier, const volatile void *template_address)
{
	const PostgammaBackendStateDescriptor *descriptor;
	PostgammaBackendStateSlot slot;

	if (identifier == NULL)
		postgamma_runtime_contract_violation();
	descriptor = postgamma_backend_state_descriptor_by_id(identifier);
	if (descriptor == NULL)
		postgamma_runtime_contract_violation();
	slot = (PostgammaBackendStateSlot)
		(descriptor - PostgammaBackendStateDescriptors);
	return postgamma_backend_state_address_internal(
		slot, template_address, NULL, 0, false);
}


void *
postgamma_backend_state_address_relocated(
	PostgammaBackendStateSlot slot, const volatile void *template_address,
	const PostgammaBackendStateTemplate *targets, size_t target_count)
{
	if (targets == NULL || target_count == 0)
		postgamma_runtime_contract_violation();
	return postgamma_backend_state_address_internal(
		slot, template_address, targets, target_count, false);
}


void *
postgamma_extension_role_state_address(
	const char *identifier, const volatile void *template_address,
	size_t size, size_t alignment)
{
	PostgammaExecutionContext *execution;
	PostgammaRoleContext *role;
	PostgammaExtensionRoleStateEntry *entry;
	size_t		identifier_length;
	void	   *allocation = NULL;
	void	   *value;

	if (identifier == NULL || template_address == NULL || size == 0 ||
		!postgamma_alignment_is_valid(alignment))
	{
		errno = EINVAL;
		return NULL;
	}
	identifier_length = strlen(identifier);
	if (identifier_length == 0 ||
		identifier_length > POSTGAMMA_EXTENSION_STATE_IDENTIFIER_MAX)
	{
		errno = EINVAL;
		return NULL;
	}
	execution = postgamma_execution_context_current();
	if (execution == NULL ||
		execution->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC ||
		execution->role == NULL ||
		execution->role->magic != POSTGAMMA_ROLE_CONTEXT_MAGIC)
	{
		errno = ENODEV;
		return NULL;
	}
	role = execution->role;
	for (entry = role->extension_role_state;
		 entry != NULL; entry = entry->next)
	{
		if (strcmp(entry->identifier, identifier) != 0)
			continue;
		if (entry->template_address != template_address ||
			entry->size != size || entry->alignment != alignment)
		{
			errno = EINVAL;
			return NULL;
		}
		return entry->value;
	}
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
	{
		errno = ENOMEM;
		return NULL;
	}
	entry->identifier = malloc(identifier_length + 1);
	if (entry->identifier == NULL)
	{
		free(entry);
		errno = ENOMEM;
		return NULL;
	}
	value = postgamma_aligned_allocation(size, alignment, &allocation);
	if (value == NULL)
	{
		free(entry->identifier);
		free(entry);
		return NULL;
	}
	memcpy(entry->identifier, identifier, identifier_length + 1);
	memcpy(value, (const void *) template_address, size);
	entry->template_address = template_address;
	entry->allocation = allocation;
	entry->value = value;
	entry->size = size;
	entry->alignment = alignment;
	entry->next = role->extension_role_state;
	role->extension_role_state = entry;
	return value;
}


void
postgamma_extension_role_state_destroy(void **state)
{
	PostgammaExtensionRoleStateEntry *entry;

	if (state == NULL)
		postgamma_runtime_contract_violation();
	entry = *state;
	while (entry != NULL)
	{
		PostgammaExtensionRoleStateEntry *next = entry->next;

		free(entry->allocation);
		free(entry->identifier);
		memset(entry, 0, sizeof(*entry));
		free(entry);
		entry = next;
	}
	*state = NULL;
}
