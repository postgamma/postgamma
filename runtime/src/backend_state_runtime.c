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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "postgamma/guc_runtime.h"
#include "postgamma/thread_runtime.h"
#include "postgamma/memory_block_runtime.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(POSTGAMMA_ROLE_CACHE_BYTES) && defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#include <unistd.h>
#endif


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
	_Atomic(const PostgammaBackendStatePreparation *) prepared;
	bool		was_bound;
#ifdef POSTGAMMA_ROLE_CACHE_BYTES
	struct PostgammaBackendStateStore *pool_owner;
	struct PostgammaBackendStateStore *pool_next;
	struct PostgammaBackendStateStore *pool_head;
	size_t		allocation_size;
	size_t		mapping_size;
	size_t		pool_bytes;
	size_t		pool_leases;
#endif
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

#ifdef POSTGAMMA_PREPARED_ROLE_STATE
#include "postgamma/backend_state_preparation.inc"
#endif


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

		if (descriptor->owner != owner)
			continue;
		if (!postgamma_alignment_is_valid(descriptor->alignment))
			postgamma_runtime_contract_violation();
		if (descriptor->alignment > alignment)
			alignment = descriptor->alignment;
	}
	return alignment;
}


int
postgamma_backend_state_context_init(PostgammaBackendStateOwner owner,
									void **storage)
#ifdef POSTGAMMA_PRIVATE_MEMORY_BLOCKS
{
	return postgamma_backend_state_context_init_in_pool(owner, storage, NULL);
}

int
postgamma_backend_state_context_init_in_pool(PostgammaBackendStateOwner owner,
	void **storage, struct PostgammaMemoryPool *pool)
#endif
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
#if defined(POSTGAMMA_ROLE_CACHE_BYTES) && defined(__linux__) && !defined(__EMSCRIPTEN__)
	if (owner == POSTGAMMA_BACKEND_STATE_OWNER_ROLE && allocation_size >= 4096
#ifdef POSTGAMMA_PRIVATE_MEMORY_BLOCKS
		&& pool == NULL
#endif
		)
	{
		long page_size = sysconf(_SC_PAGESIZE);
		size_t mapped_size;
		void *mapping;
		if (page_size <= 0 || allocation_size > SIZE_MAX - (size_t) page_size + 1)
			postgamma_runtime_contract_violation();
		mapped_size = (allocation_size + (size_t) page_size - 1) / (size_t) page_size * (size_t) page_size;
		mapping = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping != MAP_FAILED)
		{
			store->data_allocation = mapping;
			store->mapping_size = mapped_size;
		}
	}
	else
#endif
	store->data_allocation =
#ifdef POSTGAMMA_PRIVATE_MEMORY_BLOCKS
		postgamma_memory_block_allocate_in(pool, allocation_size, 1);
#else
		postgamma_memory_block_calloc(allocation_size, 1);
#endif
#ifdef POSTGAMMA_ROLE_CACHE_BYTES
	store->allocation_size = allocation_size;
#endif
	if (store->initialized == NULL || store->data_allocation == NULL)
	{
		free(store->initialized);
#if defined(POSTGAMMA_ROLE_CACHE_BYTES) && defined(__linux__) && !defined(__EMSCRIPTEN__)
		if (store->mapping_size != 0)
			(void) munmap(store->data_allocation, store->mapping_size);
		else
#endif
			postgamma_memory_block_free(store->data_allocation);
		free(store);
		return ENOMEM;
	}
	/* Zeroed storage does not replace C11 atomic object initialization. */
	for (index = 0; index < initialization_count; index++)
		atomic_init(&store->initialized[index], 0);
	atomic_init(&store->prepared, NULL);
	status = owner == POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE ?
		postgamma_mutex_create(&store->initialization_mutex) : 0;
	if (status != 0)
	{
		free(store->initialized);
		postgamma_memory_block_free(store->data_allocation);
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


#ifdef POSTGAMMA_ROLE_CACHE_BYTES
static size_t
role_cache_charge(const PostgammaBackendStateStore *store)
{
	/* Bound requested cache storage, including metadata and mapping padding. */
	return (store->mapping_size != 0 ? store->mapping_size : store->allocation_size) +
		sizeof(*store) + (store->slot_count == 0 ? 1 : store->slot_count);
}

int
postgamma_backend_state_role_acquire(void *instance_storage, void **storage)
{
	PostgammaBackendStateStore *owner = instance_storage;
	PostgammaBackendStateStore *store;
	int status;

	if (owner == NULL || owner->magic != POSTGAMMA_BACKEND_STATE_MAGIC ||
		owner->owner != POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE ||
		storage == NULL || *storage != NULL)
		postgamma_runtime_contract_violation();
	if (postgamma_mutex_lock(owner->initialization_mutex) != 0)
		postgamma_runtime_contract_violation();
	store = owner->pool_head;
	if (store != NULL)
	{
		owner->pool_head = store->pool_next;
		owner->pool_bytes -= role_cache_charge(store);
	}
	/* Reserve owner lifetime even while allocation/clearing runs outside lock. */
	owner->pool_leases++;
	if (postgamma_mutex_unlock(owner->initialization_mutex) != 0)
		postgamma_runtime_contract_violation();
	if (store == NULL)
	{
		status = postgamma_backend_state_context_init(POSTGAMMA_BACKEND_STATE_OWNER_ROLE, storage);
		if (status != 0)
		{
			if (postgamma_mutex_lock(owner->initialization_mutex) != 0)
				postgamma_runtime_contract_violation();
			owner->pool_leases--;
			if (postgamma_mutex_unlock(owner->initialization_mutex) != 0)
				postgamma_runtime_contract_violation();
			return status;
		}
		store = *storage;
	}
	else
	{
		/* Recycle physical storage only; no logical initialization survives. */
		memset(store->data_allocation, 0, store->allocation_size);
		for (size_t i = 0; i < store->slot_count; i++)
			atomic_store_explicit(&store->initialized[i], 0, memory_order_relaxed);
		atomic_store_explicit(&store->prepared, NULL, memory_order_relaxed);
		store->was_bound = false;
		store->pool_next = NULL;
		store->magic = POSTGAMMA_BACKEND_STATE_MAGIC;
		*storage = store;
	}
	store->pool_owner = owner;
	return 0;
}

void
postgamma_backend_state_role_cache_usage(void *instance_storage, size_t *bytes, size_t *leases)
{
	PostgammaBackendStateStore *owner = instance_storage;
	if (owner == NULL || owner->magic != POSTGAMMA_BACKEND_STATE_MAGIC ||
		owner->owner != POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE || bytes == NULL || leases == NULL)
		postgamma_runtime_contract_violation();
	if (postgamma_mutex_lock(owner->initialization_mutex) != 0)
		postgamma_runtime_contract_violation();
	*bytes = owner->pool_bytes;
	*leases = owner->pool_leases;
	if (postgamma_mutex_unlock(owner->initialization_mutex) != 0)
		postgamma_runtime_contract_violation();
}
#endif

void
postgamma_backend_state_context_destroy(void **storage)
{
	PostgammaBackendStateStore *store;

	if (storage == NULL || *storage == NULL)
		return;
	store = *storage;
	if (store->magic != POSTGAMMA_BACKEND_STATE_MAGIC)
		postgamma_runtime_contract_violation();
#ifdef POSTGAMMA_ROLE_CACHE_BYTES
	if (store->owner == POSTGAMMA_BACKEND_STATE_OWNER_INSTANCE)
	{
		PostgammaBackendStateStore *cached;
		if (postgamma_mutex_lock(store->initialization_mutex) != 0)
			postgamma_runtime_contract_violation();
		if (store->pool_leases != 0)
			postgamma_runtime_contract_violation();
		cached = store->pool_head;
		store->pool_head = NULL;
		store->pool_bytes = 0;
		if (postgamma_mutex_unlock(store->initialization_mutex) != 0)
			postgamma_runtime_contract_violation();
		while (cached != NULL)
		{
			PostgammaBackendStateStore *next = cached->pool_next;
			void *entry = cached;
			cached->pool_owner = NULL;
			cached->magic = POSTGAMMA_BACKEND_STATE_MAGIC;
			postgamma_backend_state_context_destroy(&entry);
			cached = next;
		}
	}
	if (store->pool_owner != NULL)
	{
		PostgammaBackendStateStore *owner = store->pool_owner;
		size_t charge = role_cache_charge(store);
		bool retain = false;
		if (postgamma_mutex_lock(owner->initialization_mutex) != 0)
			postgamma_runtime_contract_violation();
		if (owner->pool_leases == 0)
			postgamma_runtime_contract_violation();
		owner->pool_leases--;
		if (charge <= POSTGAMMA_ROLE_CACHE_BYTES &&
			owner->pool_bytes <= POSTGAMMA_ROLE_CACHE_BYTES - charge)
		{
			store->magic = 0;
			store->pool_next = owner->pool_head;
			owner->pool_head = store;
			owner->pool_bytes += charge;
			retain = true;
		}
		if (postgamma_mutex_unlock(owner->initialization_mutex) != 0)
			postgamma_runtime_contract_violation();
		if (retain)
		{
			*storage = NULL;
			return;
		}
	}
#endif
	if (store->initialization_mutex != NULL &&
		postgamma_mutex_destroy(store->initialization_mutex) != 0)
		postgamma_runtime_contract_violation();
	store->magic = 0;
	free(store->initialized);
#if defined(POSTGAMMA_ROLE_CACHE_BYTES) && defined(__linux__) && !defined(__EMSCRIPTEN__)
	if (store->mapping_size != 0)
	{
		if (munmap(store->data_allocation, store->mapping_size) != 0)
			postgamma_runtime_contract_violation();
	}
	else
#endif
	postgamma_memory_block_free(store->data_allocation);
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
	if (atomic_load_explicit(&destination_store->prepared, memory_order_acquire) != NULL)
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


/* Keep the existing byte-wise relocation algorithm on both cold paths. */
static bool
postgamma_relocate_template_pointer(unsigned char *address, size_t size,
								   const volatile void *old_address, void *new_address)
{
	uintptr_t	old_pointer = (uintptr_t) old_address;
	uintptr_t	new_pointer = (uintptr_t) new_address;
	size_t		offset;
	bool		relocated = false;

	for (offset = 0; offset + sizeof(uintptr_t) <= size; offset++)
	{
		uintptr_t	value;

		memcpy(&value, address + offset, sizeof(value));
		if (value == old_pointer)
		{
			memcpy(address + offset, &new_pointer, sizeof(new_pointer));
			relocated = true;
		}
	}
	return relocated;
}


static size_t
postgamma_preparation_index(const PostgammaBackendStatePreparation *plan,
						   PostgammaBackendStateSlot slot)
{
	size_t		low = 0;
	size_t		high = plan->count;

	while (low < high)
	{
		size_t		middle = low + (high - low) / 2;

		if (plan->entries[middle].slot < slot)
			low = middle + 1;
		else if (plan->entries[middle].slot > slot)
			high = middle;
		else
			return middle;
	}
	return SIZE_MAX;
}


int
postgamma_backend_state_context_prepare_plan(
	void *storage, const PostgammaBackendStatePreparation *plan)
{
	typedef struct PreparationValue
	{
		const volatile void *template_address;
		size_t		staged_offset;
	} PreparationValue;

	PostgammaBackendStateStore *store = storage;
	const PostgammaBackendStatePreparation *prepared;
	PreparationValue *values = NULL;
	unsigned char *staged = NULL;
	size_t		staged_size = 0;
	size_t		staged_offset = 0;
	size_t		index;
	int			status = EINVAL;

	if (store == NULL || store->magic != POSTGAMMA_BACKEND_STATE_MAGIC ||
		store->owner != POSTGAMMA_BACKEND_STATE_OWNER_ROLE || plan == NULL ||
		plan->identity == NULL || plan->identity[0] == '\0' ||
		plan->layout_identity == NULL ||
		strcmp(plan->layout_identity, POSTGAMMA_BACKEND_STATE_LAYOUT_ID) != 0 ||
		plan->count > store->slot_count ||
		(plan->count != 0 && plan->entries == NULL))
		return EINVAL;
	prepared = atomic_load_explicit(&store->prepared, memory_order_acquire);
	if (prepared != NULL)
		return prepared == plan ? 0 : EINVAL;
	if (store->was_bound)
		return EINVAL;

	/* Validate the whole set before looking up targets or touching live bytes. */
	for (index = 0; index < plan->count; index++)
	{
		const PostgammaBackendStatePreparationEntry *entry = &plan->entries[index];
		const PostgammaBackendStateDescriptor *descriptor;

		if (entry->slot < 0 || entry->slot >= POSTGAMMA_BACKEND_STATE_SLOT_COUNT ||
			(index != 0 && plan->entries[index - 1].slot >= entry->slot))
			return EINVAL;
		descriptor = &PostgammaBackendStateDescriptors[entry->slot];
		if (descriptor->owner != POSTGAMMA_BACKEND_STATE_OWNER_ROLE ||
			descriptor->local_index >= store->slot_count ||
			descriptor->offset > store->data_size ||
			descriptor->size > store->data_size - descriptor->offset ||
			atomic_load_explicit(&store->initialized[descriptor->local_index],
							 memory_order_acquire) > 1 ||
			(!entry->zero_initialize && entry->provider == NULL) ||
			descriptor->has_relocations != (entry->target_count != 0) ||
			entry->target_count > plan->count ||
			(entry->target_count != 0 && (entry->targets == NULL || entry->zero_initialize)))
			return EINVAL;
		if (!entry->zero_initialize &&
			atomic_load_explicit(&store->initialized[descriptor->local_index],
							 memory_order_acquire) == 0)
		{
			if (descriptor->size > SIZE_MAX - staged_size)
				return EINVAL;
			staged_size += descriptor->size;
		}
	}
	if (plan->count != 0)
	{
		values = calloc(plan->count, sizeof(*values));
		staged = malloc(staged_size == 0 ? 1 : staged_size);
		if (values == NULL || staged == NULL)
		{
			status = ENOMEM;
			goto done;
		}
	}
	for (index = 0; index < plan->count; index++)
	{
		const PostgammaBackendStatePreparationEntry *entry = &plan->entries[index];
		const PostgammaBackendStateDescriptor *descriptor =
			&PostgammaBackendStateDescriptors[entry->slot];

		if (entry->provider != NULL)
		{
			values[index].template_address = entry->provider();
			if (values[index].template_address == NULL)
				goto done;
		}
		/* Inherited live values always win over the default template. */
		if (atomic_load_explicit(&store->initialized[descriptor->local_index],
							 memory_order_acquire) != 0)
			continue;
		/* New and recycled role storage starts with all bytes zero.
		 * Do not fault in unused zero pages merely to mark a slot initialized. */
		if (!entry->zero_initialize)
		{
			values[index].staged_offset = staged_offset;
			memcpy(staged + staged_offset,
				   (const void *) values[index].template_address, descriptor->size);
			staged_offset += descriptor->size;
		}
	}
	for (index = 0; index < plan->count; index++)
	{
		const PostgammaBackendStatePreparationEntry *entry = &plan->entries[index];
		const PostgammaBackendStateDescriptor *descriptor =
			&PostgammaBackendStateDescriptors[entry->slot];
		size_t		target_index;

		for (target_index = 0; target_index < entry->target_count; target_index++)
		{
			size_t		target = postgamma_preparation_index(plan, entry->targets[target_index]);
			const PostgammaBackendStateDescriptor *target_descriptor;

			if (target == SIZE_MAX || values[target].template_address == NULL)
				goto done;
			target_descriptor = &PostgammaBackendStateDescriptors[entry->targets[target_index]];
			if (atomic_load_explicit(&store->initialized[descriptor->local_index],
								 memory_order_acquire) == 0 &&
				!postgamma_relocate_template_pointer(staged + values[index].staged_offset,
												   descriptor->size, values[target].template_address,
												   store->data + target_descriptor->offset))
				goto done;
		}
	}
	/* Nothing below this point can fail. No cache/TLS view has been published. */
	for (index = 0; index < plan->count; index++)
	{
		const PostgammaBackendStateDescriptor *descriptor =
			&PostgammaBackendStateDescriptors[plan->entries[index].slot];

		if (!plan->entries[index].zero_initialize &&
			atomic_load_explicit(&store->initialized[descriptor->local_index],
							 memory_order_acquire) == 0)
			memcpy(store->data + descriptor->offset, staged + values[index].staged_offset,
				   descriptor->size);
		atomic_store_explicit(&store->initialized[descriptor->local_index], 1,
							 memory_order_release);
	}
	/* copy_ids and named/lazy resolvers must see initialized before readiness. */
	atomic_store_explicit(&store->prepared, plan, memory_order_release);
	status = 0;
done:
	free(staged);
	free(values);
	return status;
}


int
postgamma_backend_state_context_prepare(void *storage)
{
#ifdef POSTGAMMA_PREPARED_ROLE_STATE
	return postgamma_backend_state_context_prepare_plan(storage, &PostgammaPreparedRoleState);
#else
	(void) storage;
	return 0;
#endif
}


void *
postgamma_backend_state_prepared_data(
	void *storage, const PostgammaBackendStatePreparation *plan)
{
	PostgammaBackendStateStore *store = storage;

	if (store == NULL || store->magic != POSTGAMMA_BACKEND_STATE_MAGIC ||
		store->owner != POSTGAMMA_BACKEND_STATE_OWNER_ROLE || plan == NULL)
		postgamma_runtime_contract_violation();
	return atomic_load_explicit(&store->prepared, memory_order_acquire) == plan ?
		store->data : NULL;
}


void *
postgamma_backend_state_context_ready_data(void *storage)
{
#ifdef POSTGAMMA_PREPARED_ROLE_STATE
	return postgamma_backend_state_prepared_data(storage, &PostgammaPreparedRoleState);
#else
	(void) storage;
	return NULL;
#endif
}


void
postgamma_backend_state_context_before_bind(void *storage)
{
	PostgammaBackendStateStore *store = storage;

	if (store == NULL || store->magic != POSTGAMMA_BACKEND_STATE_MAGIC ||
		store->owner != POSTGAMMA_BACKEND_STATE_OWNER_ROLE)
		postgamma_runtime_contract_violation();
#ifdef POSTGAMMA_PREPARED_ROLE_STATE
	if (postgamma_backend_state_context_ready_data(storage) == NULL)
		postgamma_runtime_contract_violation();
#endif
	store->was_bound = true;
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
		((uintptr_t) (store->data + descriptor->offset) &
		 (descriptor->alignment - 1)) != 0)
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

			if (target->slot < 0 ||
				target->slot >= POSTGAMMA_BACKEND_STATE_SLOT_COUNT)
				postgamma_runtime_contract_violation();
			target_descriptor = &PostgammaBackendStateDescriptors[target->slot];
			if (target_descriptor->owner != descriptor->owner)
				postgamma_runtime_contract_violation();
			if (target_descriptor->local_index >= store->slot_count ||
				target_descriptor->offset + target_descriptor->size > store->data_size ||
				((uintptr_t) (store->data + target_descriptor->offset) &
				 (target_descriptor->alignment - 1)) != 0)
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
			if (!postgamma_relocate_template_pointer(address, descriptor->size,
												   target->address, target_address))
				postgamma_runtime_contract_violation();
		}
		atomic_store_explicit(
			&store->initialized[descriptor->local_index], 1,
			memory_order_release);
	}
	if (locked_here && postgamma_mutex_unlock(store->initialization_mutex) != 0)
		postgamma_runtime_contract_violation();
	/* Publish only fully initialized addresses; relocation may recurse. */
	PostgammaCurrentExecutionContext->backend_state_addresses[slot] = address;
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
	return postgamma_backend_state_address_cached(slot, template_address);
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
