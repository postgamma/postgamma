/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "postgamma/memory_block_runtime.h"

#include <pthread.h>
#include <stdio.h>

_Thread_local PostgammaMemoryArena *PostgammaCurrentMemoryArena;

static void
require(int condition)
{
	if (!condition)
	{
		fputs("private memory block contract failed\n", stderr);
		abort();
	}
}

static void *
release_on_carrier(void *pointer)
{
	require(((unsigned char *) pointer)[32768] == 117);
	postgamma_memory_block_free(pointer);
	return NULL;
}

int
main(void)
{
	const size_t sizes[] = {0, 1, 31, 4095, 4096, 8192, 65537, 329072};
	pthread_t carrier;
	unsigned char *pointer;

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
	{
		size_t size = sizes[i];
		unsigned char *replacement;

		pointer = postgamma_memory_block_calloc(size, 1);
		require(pointer != NULL && (uintptr_t) pointer % _Alignof(max_align_t) == 0);
		for (size_t j = 0; j < size; j++)
		{
			require(pointer[j] == 0);
			pointer[j] = (unsigned char) j;
		}
		replacement = postgamma_memory_block_realloc(pointer, size + 16384);
		require(replacement != NULL);
		pointer = replacement;
		for (size_t j = 0; j < size; j++)
			require(pointer[j] == (unsigned char) j);
		replacement = postgamma_memory_block_realloc(pointer, 19);
		require(replacement != NULL);
		pointer = replacement;
		for (size_t j = 0; j < 19 && j < size; j++)
			require(pointer[j] == (unsigned char) j);
		pointer[0] = 127;
		replacement = postgamma_memory_block_realloc(pointer, SIZE_MAX);
		require(replacement == NULL && errno == ENOMEM && pointer[0] == 127);
		postgamma_memory_block_free(pointer);
	}
	pointer = postgamma_memory_block_calloc(SIZE_MAX, 2);
	require(pointer == NULL && errno == ENOMEM);
	pointer = postgamma_memory_block_malloc(SIZE_MAX);
	require(pointer == NULL && errno == ENOMEM);
	postgamma_memory_block_free(NULL);
	pointer = postgamma_memory_block_realloc(NULL, 65536);
	require(pointer != NULL);
	pointer[32768] = 117;
	require(pthread_create(&carrier, NULL, release_on_carrier, pointer) == 0);
	require(pthread_join(carrier, NULL) == 0);
	/* A free must remove the private mapping, independently of libc arenas. */
#ifdef POSTGAMMA_MAPPED_MEMORY_BLOCKS
	{
		long page = sysconf(_SC_PAGESIZE);
		uintptr_t address;
		unsigned char residency;
		int status;

		pointer = postgamma_memory_block_malloc(65536);
		require(pointer != NULL && page > 0);
		address = (uintptr_t) pointer / (size_t) page * (size_t) page;
		status = mincore((void *) address, (size_t) page, &residency);
		require(status == 0);
		pointer[0] = 91;
		/* Fits size_t arithmetic but cannot fit a userspace mapping. */
		require(postgamma_memory_block_realloc(pointer, SIZE_MAX / 2 + 1) == NULL);
		require(errno == ENOMEM && pointer[0] == 91);
		postgamma_memory_block_free(pointer);
		status = mincore((void *) address, (size_t) page, &residency);
		require(status == -1 && errno == ENOMEM);
	}
#endif
#ifdef POSTGAMMA_BLOCK_ASAN
	pointer = postgamma_memory_block_malloc(65537);
	require(pointer != NULL);
	require(__asan_address_is_poisoned(pointer - 1));
	require(__asan_address_is_poisoned(pointer + 65537));
	require(!__asan_address_is_poisoned(pointer + 65536));
	postgamma_memory_block_free(pointer);
#endif
	{
		PostgammaMemoryArena first = {0};
		PostgammaMemoryArena second = {0};
		unsigned char *saved;
		void *blocks[600];

		PostgammaCurrentMemoryArena = &first;
		pointer = postgamma_context_block_malloc(65536);
		require(pointer != NULL && first.live_blocks == 1);
		pointer[32768] = 117;
		saved = pointer;
		postgamma_memory_block_free(pointer);
		require(first.live_blocks == 0);
		pointer = postgamma_context_block_malloc(65536);
		require(pointer == saved && first.live_blocks == 1);
		pointer[32768] = 117;
		/* Binding another arena must not change this allocation's owner. */
		PostgammaCurrentMemoryArena = &second;
		pointer = postgamma_context_block_realloc(pointer, 66000);
		require(pointer != NULL && pointer[32768] == 117);
		require(first.live_blocks == 1 && second.live_blocks == 0);
		PostgammaCurrentMemoryArena = NULL;
		require(pthread_create(&carrier, NULL, release_on_carrier, pointer) == 0);
		require(pthread_join(carrier, NULL) == 0);
		require(first.live_blocks == 0);
		/* Live data can outgrow one segment; fully idle storage is bounded. */
		PostgammaCurrentMemoryArena = &first;
		{
			size_t maximum = (size_t) 256 << (POSTGAMMA_MEMORY_ARENA_BINS - 1);

			/* Metadata must not push an exact size-class payload into overflow. */
			pointer = postgamma_context_block_malloc(maximum);
			require(pointer != NULL && first.live_blocks == 1);
			pointer[maximum - 1] = 83;
			pointer = postgamma_context_block_realloc(pointer, maximum + 1);
			require(pointer != NULL && pointer[maximum - 1] == 83 && first.live_blocks == 0);
			postgamma_memory_block_free(pointer);
		}
		for (size_t i = 0; i < 600; i++)
		{
			blocks[i] = postgamma_context_block_malloc(8192);
			require(blocks[i] != NULL);
			memset(blocks[i], (unsigned char) i, 8192);
		}
		require(first.segment_count == 2 && first.live_blocks == 600);
		for (size_t i = 0; i < 600; i++)
		{
			pointer = blocks[i];
			require(pointer[0] == (unsigned char) i && pointer[8191] == (unsigned char) i);
			postgamma_memory_block_free(pointer);
		}
		require(first.live_blocks == 0 && first.segment_count == 1 && first.empty != NULL);
		/* Changing size classes after a burst must reuse the idle segment. */
		pointer = postgamma_context_block_malloc(16384);
		require(pointer != NULL && first.live_blocks == 1 && first.segment_count == 1);
		postgamma_memory_block_free(pointer);
		/* Retire segments with interleaved freelists, then reuse every class. */
		for (size_t round = 0; round < 3; round++)
		{
			for (size_t i = 0; i < 600; i++)
			{
				size_t size = (size_t) 256 << (i % POSTGAMMA_MEMORY_ARENA_BINS);

				blocks[i] = postgamma_context_block_malloc(size);
				require(blocks[i] != NULL);
				pointer = blocks[i];
				pointer[0] = (unsigned char) i;
				pointer[size - 1] = (unsigned char) round;
			}
			/* 173 and 600 are coprime, so this visits each allocation once. */
			for (size_t j = 0; j < 600; j++)
			{
				size_t i = j * 173 % 600;
				size_t size = (size_t) 256 << (i % POSTGAMMA_MEMORY_ARENA_BINS);

				pointer = blocks[i];
				require(pointer[0] == (unsigned char) i && pointer[size - 1] == (unsigned char) round);
				postgamma_memory_block_free(pointer);
			}
			require(first.live_blocks == 0 && first.segment_count == 1 && first.empty != NULL);
		}
		PostgammaCurrentMemoryArena = NULL;
		postgamma_memory_arena_destroy(&first);
		postgamma_memory_arena_destroy(&second);
		require(first.segments == NULL && first.segment_count == 0);
		/* A fresh logical session cannot inherit a prior session's freelists. */
		PostgammaCurrentMemoryArena = &first;
		pointer = postgamma_context_block_malloc(256);
		require(pointer != NULL && first.live_blocks == 1);
#ifdef POSTGAMMA_BLOCK_ASAN
		require(__asan_address_is_poisoned(pointer - 1));
		require(__asan_address_is_poisoned(pointer + 256));
		postgamma_memory_block_free(pointer);
		require(__asan_address_is_poisoned(pointer));
#endif
		/* Session exit also owns an allocation orphaned before context linking. */
		PostgammaCurrentMemoryArena = NULL;
		postgamma_memory_arena_destroy(&first);
	}
	puts("private memory blocks: allocation, resize, failure, migration and release passed");
	return 0;
}
