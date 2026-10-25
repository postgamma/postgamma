/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_DEADLINE_HEAP_H
#define POSTGAMMA_DEADLINE_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Intrusive timer queue. Its owner supplies storage and synchronization. */
typedef struct PostgammaDeadline
{
	uint64_t	at;
	size_t		position;
} PostgammaDeadline;

typedef struct PostgammaDeadlineHeap
{
	PostgammaDeadline **entries;
	size_t		count;
	size_t		capacity;
} PostgammaDeadlineHeap;

static inline void
postgamma_deadline_swap(PostgammaDeadlineHeap *heap, size_t a, size_t b)
{
	PostgammaDeadline *entry = heap->entries[a];

	heap->entries[a] = heap->entries[b];
	heap->entries[b] = entry;
	heap->entries[a]->position = a;
	heap->entries[b]->position = b;
}

static inline void
postgamma_deadline_repair(PostgammaDeadlineHeap *heap, size_t position)
{
	if (position > 0 && heap->entries[position]->at <
		heap->entries[(position - 1) / 2]->at)
	{
		while (position > 0)
		{
			size_t parent = (position - 1) / 2;

			if (heap->entries[parent]->at <= heap->entries[position]->at)
				break;
			postgamma_deadline_swap(heap, parent, position);
			position = parent;
		}
	}
	else
	{
		while (position < heap->count / 2)
		{
			size_t child = position * 2 + 1;

			if (child + 1 < heap->count &&
				heap->entries[child + 1]->at < heap->entries[child]->at)
				child++;
			if (heap->entries[position]->at <= heap->entries[child]->at)
				break;
			postgamma_deadline_swap(heap, position, child);
			position = child;
		}
	}
}

/* Zero cancels. Rearm and removal do not allocate or retain stale entries. */
static inline void
postgamma_deadline_set(PostgammaDeadlineHeap *heap,
	PostgammaDeadline *entry, uint64_t deadline)
{
	if (entry->at != 0)
	{
		size_t position = entry->position;

		if (position >= heap->count || heap->entries[position] != entry)
			abort();
		heap->count--;
		if (position < heap->count)
		{
			heap->entries[position] = heap->entries[heap->count];
			heap->entries[position]->position = position;
			postgamma_deadline_repair(heap, position);
		}
	}
	entry->at = deadline;
	entry->position = SIZE_MAX;
	if (deadline != 0)
	{
		if (heap->count >= heap->capacity)
			abort();
		entry->position = heap->count;
		heap->entries[heap->count++] = entry;
		postgamma_deadline_repair(heap, entry->position);
	}
}

#endif
