/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_MEMORY_POOL_RUNTIME_H
#define POSTGAMMA_MEMORY_POOL_RUNTIME_H

/* Included by memory_block_runtime.h after the mapping and ASan definitions. */
#include <pthread.h>

#define POSTGAMMA_MEMORY_POOL_BUCKETS 32
#define POSTGAMMA_MEMORY_POOL_ENTRIES 256

typedef struct PostgammaMemoryPoolEntry
{
	struct PostgammaMemoryPoolEntry *next;
	size_t size;
} PostgammaMemoryPoolEntry;

/*
 * An instance owns the pool; each client holds a lease through all cleanup.
 * Only backing mappings are recycled; calloc clears the requested payload.
 * No MemoryContext, role, transaction, or session initialization is reused.
 * The last lease releases all idle mappings, including their physical pages.
 * Neither clearing nor mmap/munmap takes place while the mutex is held.
 */
typedef struct PostgammaMemoryPool
{
	pthread_mutex_t mutex;
	PostgammaMemoryPoolEntry *buckets[POSTGAMMA_MEMORY_POOL_BUCKETS];
	size_t limit;
	size_t bytes;
	size_t entries;
	size_t leases;
	size_t outstanding;
} PostgammaMemoryPool;

static inline void
postgamma_memory_pool_lock(PostgammaMemoryPool *pool)
{
	if (pthread_mutex_lock(&pool->mutex) != 0)
		abort();
}

static inline void
postgamma_memory_pool_unlock(PostgammaMemoryPool *pool)
{
	if (pthread_mutex_unlock(&pool->mutex) != 0)
		abort();
}

static inline int
postgamma_memory_pool_init(PostgammaMemoryPool *pool, size_t limit)
{
	memset(pool, 0, sizeof(*pool));
	pool->limit = limit;
	return pthread_mutex_init(&pool->mutex, NULL);
}

static inline void
postgamma_memory_pool_acquire(PostgammaMemoryPool *pool)
{
	postgamma_memory_pool_lock(pool);
	if (pool->leases == SIZE_MAX)
		abort();
	pool->leases++;
	postgamma_memory_pool_unlock(pool);
}

/* Caller holds the mutex. Detaching makes concurrent new leases independent. */
static inline PostgammaMemoryPoolEntry *
postgamma_memory_pool_detach(PostgammaMemoryPool *pool)
{
	PostgammaMemoryPoolEntry *head = NULL;

	for (size_t i = 0; i < POSTGAMMA_MEMORY_POOL_BUCKETS; i++)
	{
		PostgammaMemoryPoolEntry *entry = pool->buckets[i];

		while (entry != NULL)
		{
			PostgammaMemoryPoolEntry *next = entry->next;

			entry->next = head;
			head = entry;
			entry = next;
		}
		pool->buckets[i] = NULL;
	}
	pool->bytes = pool->entries = 0;
	return head;
}

static inline void
postgamma_memory_pool_discard(PostgammaMemoryPoolEntry *head)
{
	while (head != NULL)
	{
		PostgammaMemoryPoolEntry *next = head->next;
		size_t size = head->size;

		POSTGAMMA_BLOCK_UNPOISON(head, size);
#ifdef POSTGAMMA_MAPPED_MEMORY_BLOCKS
		if (munmap(head, size) != 0)
			abort();
#else
		free(head);
#endif
		head = next;
	}
}

static inline void
postgamma_memory_pool_release(PostgammaMemoryPool *pool)
{
	PostgammaMemoryPoolEntry *idle = NULL;

	postgamma_memory_pool_lock(pool);
	if (pool->leases == 0)
		abort();
	if (--pool->leases == 0)
	{
		if (pool->outstanding != 0)
			abort();
		idle = postgamma_memory_pool_detach(pool);
	}
	postgamma_memory_pool_unlock(pool);
	postgamma_memory_pool_discard(idle);
}

/* The owner must exclude new leases while destroying the instance. */
static inline int
postgamma_memory_pool_destroy(PostgammaMemoryPool *pool)
{
	int busy;

	postgamma_memory_pool_lock(pool);
	busy = pool->leases != 0 || pool->outstanding != 0 || pool->entries != 0;
	postgamma_memory_pool_unlock(pool);
	return busy ? EBUSY : pthread_mutex_destroy(&pool->mutex);
}

static inline void *
postgamma_memory_pool_take(PostgammaMemoryPool *pool, size_t size)
{
	PostgammaMemoryPoolEntry **link;
	PostgammaMemoryPoolEntry *entry;

	postgamma_memory_pool_lock(pool);
	if (pool->leases == 0 || pool->outstanding == SIZE_MAX)
		abort();
	pool->outstanding++;
	link = &pool->buckets[(size / 4096) % POSTGAMMA_MEMORY_POOL_BUCKETS];
	while (*link != NULL && (*link)->size != size)
		link = &(*link)->next;
	entry = *link;
	if (entry != NULL)
	{
		*link = entry->next;
		pool->bytes -= size;
		pool->entries--;
	}
	postgamma_memory_pool_unlock(pool);
	if (entry != NULL)
		POSTGAMMA_BLOCK_UNPOISON(entry, size);
	return entry;
}

/* A NULL mapping rolls back an unsuccessful mmap following a cache miss. */
static inline void
postgamma_memory_pool_put(PostgammaMemoryPool *pool, void *mapping, size_t size)
{
	int retained = 0;

	postgamma_memory_pool_lock(pool);
	if (pool->leases == 0 || pool->outstanding == 0)
		abort();
	pool->outstanding--;
	if (mapping != NULL && size <= pool->limit &&
		pool->bytes <= pool->limit - size &&
		pool->entries < POSTGAMMA_MEMORY_POOL_ENTRIES)
	{
		PostgammaMemoryPoolEntry *entry = mapping;
		size_t bucket = (size / 4096) % POSTGAMMA_MEMORY_POOL_BUCKETS;

		POSTGAMMA_BLOCK_POISON(mapping, size);
		POSTGAMMA_BLOCK_UNPOISON(entry, sizeof(*entry));
		entry->size = size;
		entry->next = pool->buckets[bucket];
		pool->buckets[bucket] = entry;
		pool->bytes += size;
		pool->entries++;
		retained = 1;
	}
	postgamma_memory_pool_unlock(pool);
	if (mapping != NULL && !retained)
	{
		PostgammaMemoryPoolEntry *entry = mapping;

		POSTGAMMA_BLOCK_UNPOISON(entry, sizeof(*entry));
		entry->size = size;
		entry->next = NULL;
		postgamma_memory_pool_discard(entry);
	}
}

#endif
