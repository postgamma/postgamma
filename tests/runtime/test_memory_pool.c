/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "postgamma/memory_block_runtime.h"
#include "../check.h"

_Thread_local PostgammaMemoryArena *PostgammaCurrentMemoryArena;

static void *
churn(void *argument)
{
	PostgammaMemoryPool *pool = argument;

	postgamma_memory_pool_acquire(pool);
	for (size_t i = 0; i < 500; i++)
	{
		PostgammaMemoryArena arena = {0};
		unsigned char *block;

		arena.pool = pool;
		block = postgamma_memory_arena_allocate(&arena, 8192);
		CHECK(block != NULL);
		memset(block, 91, 8192);
		/* An exit also reclaims blocks orphaned before context linking. */
		postgamma_memory_arena_destroy(&arena);
	}
	postgamma_memory_pool_release(pool);
	return NULL;
}

static void *
reopen(void *argument)
{
	PostgammaMemoryPool *pool = argument;

	for (size_t i = 0; i < 100; i++)
	{
		unsigned char *block;

		/* A new lease may race with the last client's detached unmaps. */
		postgamma_memory_pool_acquire(pool);
		block = postgamma_memory_block_allocate_in(pool, 65536, 1);
		CHECK(block != NULL);
		for (size_t j = 0; j < 65536; j++)
			CHECK(block[j] == 0);
		memset(block, 73, 65536);
		postgamma_memory_block_free(block);
		postgamma_memory_pool_release(pool);
	}
	return NULL;
}

int
main(void)
{
	PostgammaMemoryPool pool;
	PostgammaMemoryPool other;
	pthread_t threads[4];
	unsigned char *block;
	unsigned char *second;
	uintptr_t address;
	size_t cached;

	CHECK(postgamma_memory_pool_init(&pool, 16 * 1024 * 1024) == 0);
	CHECK(postgamma_memory_pool_init(&other, 0) == 0);
	postgamma_memory_pool_acquire(&pool);
	postgamma_memory_pool_acquire(&other);
	CHECK(postgamma_memory_pool_destroy(&pool) == EBUSY);
	block = postgamma_memory_block_allocate_in(&pool, 65536, 1);
	CHECK(block != NULL);
	memset(block, 73, 65536);
	address = (uintptr_t) block;
	postgamma_memory_block_free(block);
#ifdef POSTGAMMA_MAPPED_MEMORY_BLOCKS
	cached = pool.bytes;
	CHECK(cached > 65536 && pool.entries == 1 && pool.outstanding == 0);
#else
	cached = 0;
#endif
	second = postgamma_memory_block_allocate_in(&other, 65536, 1);
	CHECK(second != NULL);
#ifdef POSTGAMMA_MAPPED_MEMORY_BLOCKS
	CHECK((uintptr_t) second != address);
#endif
	postgamma_memory_block_free(second);
	CHECK(other.bytes == 0 && other.outstanding == 0);
	block = postgamma_memory_block_allocate_in(&pool, 65536, 1);
	CHECK(block != NULL);
#ifdef POSTGAMMA_MAPPED_MEMORY_BLOCKS
	CHECK((uintptr_t) block == address && pool.bytes == 0);
	for (size_t i = 0; i < 65536; i++)
		CHECK(block[i] == 0);
#endif
	block[0] = 57;
	second = postgamma_memory_block_realloc(block, SIZE_MAX / 2 + 1);
	CHECK(second == NULL && errno == ENOMEM && block[0] == 57);
	second = postgamma_memory_block_realloc(block, 66000);
	CHECK(second != NULL && second[0] == 57);
	postgamma_memory_block_free(second);
	CHECK(pool.bytes <= pool.limit && pool.outstanding == 0);
	/* Retention is bounded even while the instance still has live clients. */
	for (size_t i = 0; i < 4; i++)
		CHECK(pthread_create(&threads[i], NULL, churn, &pool) == 0);
	for (size_t i = 0; i < 4; i++)
		CHECK(pthread_join(threads[i], NULL) == 0);
	CHECK(pool.leases == 1 && pool.bytes <= pool.limit && pool.outstanding == 0);
	CHECK(pool.entries <= POSTGAMMA_MEMORY_POOL_ENTRIES);
	{
		PostgammaMemoryPool entry_pool;
		void *small[POSTGAMMA_MEMORY_POOL_ENTRIES + 1];

		/* Exhaust the entry budget while remaining below the byte budget. */
		CHECK(postgamma_memory_pool_init(&entry_pool, SIZE_MAX) == 0);
		postgamma_memory_pool_acquire(&entry_pool);
		for (size_t i = 0; i < POSTGAMMA_MEMORY_POOL_ENTRIES + 1; i++)
		{
			small[i] = postgamma_memory_block_allocate_in(&entry_pool, 65536, 0);
			CHECK(small[i] != NULL);
		}
		for (size_t i = 0; i < POSTGAMMA_MEMORY_POOL_ENTRIES + 1; i++)
			postgamma_memory_block_free(small[i]);
#ifdef POSTGAMMA_MAPPED_MEMORY_BLOCKS
		CHECK(entry_pool.entries == POSTGAMMA_MEMORY_POOL_ENTRIES);
#endif
		CHECK(entry_pool.outstanding == 0 && entry_pool.bytes <= entry_pool.limit);
		postgamma_memory_pool_release(&entry_pool);
		CHECK(postgamma_memory_pool_destroy(&entry_pool) == 0);
	}
	/* The final client closes: mappings and resident storage must disappear. */
	postgamma_memory_pool_release(&pool);
	CHECK(pool.leases == 0 && pool.bytes == 0 && pool.entries == 0);
#ifdef POSTGAMMA_MAPPED_MEMORY_BLOCKS
	{
		long page = sysconf(_SC_PAGESIZE);
		unsigned char residency;

		CHECK(page > 0 && cached > 0);
		CHECK(mincore((void *) (address / (size_t) page * (size_t) page),
			(size_t) page, &residency) == -1 && errno == ENOMEM);
	}
#else
	(void) cached;
	(void) address;
#endif
	for (size_t i = 0; i < 4; i++)
		CHECK(pthread_create(&threads[i], NULL, reopen, &pool) == 0);
	for (size_t i = 0; i < 4; i++)
		CHECK(pthread_join(threads[i], NULL) == 0);
	CHECK(pool.leases == 0 && pool.outstanding == 0 && pool.entries == 0 && pool.bytes == 0);
	postgamma_memory_pool_release(&other);
	CHECK(postgamma_memory_pool_destroy(&other) == 0);
	CHECK(postgamma_memory_pool_destroy(&pool) == 0);
	return 0;
}
