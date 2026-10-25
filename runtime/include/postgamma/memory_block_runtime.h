/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_MEMORY_BLOCK_RUNTIME_H
#define POSTGAMMA_MEMORY_BLOCK_RUNTIME_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Backing storage for private MemoryContext blocks and role images only.
 * PostgreSQL still owns chunk allocation, context reset and context lifetime.
 * Do not use these functions for pointers exchanged with libc or an extension.
 * The experimental switch leaves the host's malloc policy untouched.
 */
#ifndef POSTGAMMA_PRIVATE_MEMORY_BLOCKS
#define postgamma_memory_block_malloc malloc
#define postgamma_memory_block_calloc calloc
#define postgamma_memory_block_realloc realloc
#define postgamma_memory_block_free free
#define postgamma_context_block_malloc malloc
#define postgamma_context_block_realloc realloc
#else

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#include <unistd.h>
#define POSTGAMMA_MAPPED_MEMORY_BLOCKS 1
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define POSTGAMMA_BLOCK_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(POSTGAMMA_BLOCK_ASAN)
#define POSTGAMMA_BLOCK_ASAN 1
#endif
#ifdef POSTGAMMA_BLOCK_ASAN
#include <sanitizer/asan_interface.h>
#define POSTGAMMA_BLOCK_POISON(p, n) ASAN_POISON_MEMORY_REGION(p, n)
#define POSTGAMMA_BLOCK_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION(p, n)
#else
#define POSTGAMMA_BLOCK_POISON(p, n) ((void) 0)
#define POSTGAMMA_BLOCK_UNPOISON(p, n) ((void) 0)
#endif

#include "postgamma/memory_pool_runtime.h"

struct PostgammaMemoryArena;
union PostgammaMemorySegment;
typedef union PostgammaMemoryBlock
{
	max_align_t alignment;
	struct
	{
		uint64_t magic;
		size_t size;
		size_t mapped_size;
		PostgammaMemoryPool *pool;
		struct PostgammaMemoryArena *arena;
		union PostgammaMemorySegment *segment;
		union PostgammaMemoryBlock *next;
		unsigned bin;
	} fields;
} PostgammaMemoryBlock;

/* One active carrier owns an arena. It follows the logical session on yield. */
#define POSTGAMMA_MEMORY_ARENA_BYTES (4 * 1024 * 1024)
#define POSTGAMMA_MEMORY_ARENA_BINS 11
typedef union PostgammaMemorySegment
{
	max_align_t alignment;
	struct
	{
		union PostgammaMemorySegment *next;
		size_t used;
		size_t live_blocks;
	} fields;
} PostgammaMemorySegment;

typedef struct PostgammaMemoryArena
{
	PostgammaMemoryPool *pool;
	PostgammaMemorySegment *segments;
	PostgammaMemorySegment *current;
	PostgammaMemorySegment *empty;
	size_t segment_count;
	size_t live_blocks;
	PostgammaMemoryBlock *available[POSTGAMMA_MEMORY_ARENA_BINS];
} PostgammaMemoryArena;

extern _Thread_local PostgammaMemoryArena *PostgammaCurrentMemoryArena;

#define POSTGAMMA_MEMORY_BLOCK_MAGIC UINT64_C(0x50474d424c4f434b)

static inline void *
postgamma_memory_block_allocate_in(PostgammaMemoryPool *pool, size_t size, int zero)
{
	PostgammaMemoryBlock *block = NULL;
	size_t total;
	size_t mapped = 0;

	if (size > SIZE_MAX - sizeof(*block))
	{
		errno = ENOMEM;
		return NULL;
	}
	total = sizeof(*block) + size;
#ifdef POSTGAMMA_MAPPED_MEMORY_BLOCKS
	{
		long page = sysconf(_SC_PAGESIZE);

		if (page <= 0)
			abort();
		if (total >= (size_t) page)
		{
			if (total > SIZE_MAX - ((size_t) page - 1))
			{
				errno = ENOMEM;
				return NULL;
			}
			mapped = (total + (size_t) page - 1) / (size_t) page * (size_t) page;
			block = pool == NULL ? NULL : postgamma_memory_pool_take(pool, mapped);
			if (block != NULL && zero)
				memset(block + 1, 0, size);
			if (block == NULL)
			{
				block = mmap(NULL, mapped, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
				if (block == MAP_FAILED)
				{
					if (pool != NULL)
						postgamma_memory_pool_put(pool, NULL, 0);
					return NULL;
				}
			}
		}
	}
#endif
	if (mapped == 0)
		block = zero ? calloc(1, total) : malloc(total);
	if (block == NULL)
		return NULL;
	block->fields.magic = POSTGAMMA_MEMORY_BLOCK_MAGIC;
	block->fields.size = size;
	block->fields.mapped_size = mapped;
	block->fields.pool = mapped == 0 ? NULL : pool;
	block->fields.arena = NULL;
	/* Preserve allocator boundary checks in ASan builds, including padding. */
	if (mapped != 0)
		POSTGAMMA_BLOCK_POISON((char *) block + total, mapped - total);
	POSTGAMMA_BLOCK_POISON(block, sizeof(*block));
	return block + 1;
}

static inline void *
postgamma_memory_block_allocate(size_t size, int zero)
{
	return postgamma_memory_block_allocate_in(NULL, size, zero);
}

static inline void *
postgamma_memory_block_malloc(size_t size)
{
	return postgamma_memory_block_allocate(size, 0);
}

static inline void *
postgamma_memory_block_calloc(size_t count, size_t size)
{
	if (size != 0 && count > SIZE_MAX / size)
	{
		errno = ENOMEM;
		return NULL;
	}
	return postgamma_memory_block_allocate(count * size, 1);
}

static inline size_t
postgamma_memory_block_size(void *pointer)
{
	PostgammaMemoryBlock *block = (PostgammaMemoryBlock *) pointer - 1;
	size_t size;

	POSTGAMMA_BLOCK_UNPOISON(block, sizeof(*block));
	if (block->fields.magic != POSTGAMMA_MEMORY_BLOCK_MAGIC)
		abort();
	size = block->fields.size;
	POSTGAMMA_BLOCK_POISON(block, sizeof(*block));
	return size;
}

static inline void
postgamma_memory_arena_unlink_free(PostgammaMemoryArena *arena,
	PostgammaMemorySegment *segment)
{
	unsigned bin;

	for (bin = 0; bin < POSTGAMMA_MEMORY_ARENA_BINS; bin++)
	{
		PostgammaMemoryBlock *block = arena->available[bin];
		PostgammaMemoryBlock *previous = NULL;

		while (block != NULL)
		{
			PostgammaMemoryBlock *next;

			POSTGAMMA_BLOCK_UNPOISON(block, sizeof(*block));
			next = block->fields.next;
			if (block->fields.segment == segment)
			{
				if (previous == NULL)
					arena->available[bin] = next;
				else
				{
					POSTGAMMA_BLOCK_UNPOISON(previous, sizeof(*previous));
					previous->fields.next = next;
					POSTGAMMA_BLOCK_POISON(previous, sizeof(*previous));
				}
			}
			else
				previous = block;
			POSTGAMMA_BLOCK_POISON(block, sizeof(*block));
			block = next;
		}
	}
}

/* Unlink an entirely idle segment; no live block can refer to it. */
static inline void
postgamma_memory_arena_detach(PostgammaMemoryArena *arena,
	PostgammaMemorySegment *segment)
{
	PostgammaMemorySegment **link = &arena->segments;

	postgamma_memory_arena_unlink_free(arena, segment);
	while (*link != segment)
	{
		if (*link == NULL)
			abort();
		link = &(*link)->fields.next;
	}
	*link = segment->fields.next;
	if (arena->current == segment)
		arena->current = arena->empty;
	arena->segment_count--;
}

static inline void
postgamma_memory_block_free(void *pointer)
{
	PostgammaMemoryBlock *block;
	size_t mapped;

	if (pointer == NULL)
		return;
	block = (PostgammaMemoryBlock *) pointer - 1;
	POSTGAMMA_BLOCK_UNPOISON(block, sizeof(*block));
	if (block->fields.magic != POSTGAMMA_MEMORY_BLOCK_MAGIC)
		abort();
	if (block->fields.arena != NULL)
	{
		PostgammaMemoryArena *arena = block->fields.arena;
		PostgammaMemorySegment *segment = block->fields.segment;
		unsigned bin = block->fields.bin;

		if (bin >= POSTGAMMA_MEMORY_ARENA_BINS || arena->live_blocks == 0 ||
			segment->fields.live_blocks == 0)
			abort();
		block->fields.magic = 0;
		block->fields.next = arena->available[bin];
		arena->available[bin] = block;
		arena->live_blocks--;
		segment->fields.live_blocks--;
		POSTGAMMA_BLOCK_POISON(block, ((size_t) 256 << bin) + sizeof(*block));
		if (segment->fields.live_blocks == 0)
		{
			if (arena->empty == NULL)
				arena->empty = segment;
			else
			{
				/* Retain at most one wholly idle segment within this session. */
				postgamma_memory_arena_detach(arena, segment);
				postgamma_memory_block_free(segment);
			}
		}
		return;
	}
	mapped = block->fields.mapped_size;
	block->fields.magic = 0;
#ifdef POSTGAMMA_MAPPED_MEMORY_BLOCKS
	if (mapped != 0)
	{
		if (block->fields.pool != NULL)
		{
			postgamma_memory_pool_put(block->fields.pool, block, mapped);
			return;
		}
		POSTGAMMA_BLOCK_UNPOISON(block, mapped);
		if (munmap(block, mapped) != 0)
			abort();
		return;
	}
#else
	(void) mapped;
#endif
	free(block);
}

static inline void *
postgamma_memory_arena_allocate(PostgammaMemoryArena *arena, size_t size)
{
	unsigned bin = 0;
	size_t capacity = 256;
	size_t stride;
	PostgammaMemoryBlock *block;
	PostgammaMemorySegment *segment;

	if (arena == NULL || size > ((size_t) 256 << (POSTGAMMA_MEMORY_ARENA_BINS - 1)))
		return postgamma_memory_block_allocate_in(arena == NULL ? NULL : arena->pool, size, 0);
	while (capacity < size)
	{
		capacity *= 2;
		bin++;
	}
	/* Keep metadata outside the size class; it must not double an 8 KiB block. */
	stride = capacity + sizeof(*block);
	block = arena->available[bin];
	if (block != NULL)
	{
		POSTGAMMA_BLOCK_UNPOISON(block, stride);
		arena->available[bin] = block->fields.next;
		segment = block->fields.segment;
	}
	else
	{
		segment = arena->current;
		if (segment == NULL || stride > POSTGAMMA_MEMORY_ARENA_BYTES - segment->fields.used)
		{
			if (arena->empty != NULL)
			{
				/* A new allocation mix must not strand the idle segment's bins. */
				segment = arena->empty;
				postgamma_memory_arena_unlink_free(arena, segment);
				segment->fields.used = 0;
				arena->current = segment;
			}
			else
			{
				segment = postgamma_memory_block_allocate_in(arena->pool,
					sizeof(*segment) + POSTGAMMA_MEMORY_ARENA_BYTES, 0);
				if (segment == NULL)
					return NULL;
				segment->fields.next = arena->segments;
				segment->fields.used = 0;
				segment->fields.live_blocks = 0;
				arena->segments = arena->current = segment;
				arena->segment_count++;
				POSTGAMMA_BLOCK_POISON(segment + 1, POSTGAMMA_MEMORY_ARENA_BYTES);
			}
		}
		block = (PostgammaMemoryBlock *) ((char *) (segment + 1) + segment->fields.used);
		segment->fields.used += stride;
		POSTGAMMA_BLOCK_UNPOISON(block, stride);
	}
	block->fields.magic = POSTGAMMA_MEMORY_BLOCK_MAGIC;
	block->fields.size = size;
	block->fields.mapped_size = 0;
	block->fields.pool = arena->pool;
	block->fields.arena = arena;
	block->fields.segment = segment;
	block->fields.bin = bin;
	arena->live_blocks++;
	segment->fields.live_blocks++;
	if (arena->empty == segment)
		arena->empty = NULL;
	POSTGAMMA_BLOCK_POISON((char *) (block + 1) + size, capacity - size);
	POSTGAMMA_BLOCK_POISON(block, sizeof(*block));
	return block + 1;
}

static inline void
postgamma_memory_arena_destroy(PostgammaMemoryArena *arena)
{
	PostgammaMemorySegment *segment = arena->segments;

	/* The session has ended; reclaim pool blocks even if context linking failed. */
	while (segment != NULL)
	{
		PostgammaMemorySegment *next = segment->fields.next;

		postgamma_memory_block_free(segment);
		segment = next;
	}
	memset(arena, 0, sizeof(*arena));
}

static inline void *
postgamma_context_block_malloc(size_t size)
{
	return postgamma_memory_arena_allocate(PostgammaCurrentMemoryArena, size);
}

static inline void *
postgamma_memory_block_realloc(void *pointer, size_t size)
{
	PostgammaMemoryBlock *block;
	PostgammaMemoryArena *arena;
	PostgammaMemoryPool *pool;
	void *replacement;
	size_t old_size;

	if (pointer == NULL)
		return postgamma_memory_block_malloc(size);
	if (size == 0)
	{
		postgamma_memory_block_free(pointer);
		return NULL;
	}
	old_size = postgamma_memory_block_size(pointer);
	block = (PostgammaMemoryBlock *) pointer - 1;
	POSTGAMMA_BLOCK_UNPOISON(block, sizeof(*block));
	arena = block->fields.arena;
	pool = block->fields.pool;
	POSTGAMMA_BLOCK_POISON(block, sizeof(*block));
	replacement = arena != NULL ? postgamma_memory_arena_allocate(arena, size) :
		postgamma_memory_block_allocate_in(pool, size, 0);
	if (replacement == NULL)
		return NULL;
	memcpy(replacement, pointer, old_size < size ? old_size : size);
	postgamma_memory_block_free(pointer);
	return replacement;
}

static inline void *
postgamma_context_block_realloc(void *pointer, size_t size)
{
	return pointer == NULL ? postgamma_context_block_malloc(size) :
		postgamma_memory_block_realloc(pointer, size);
}
#endif
#endif
