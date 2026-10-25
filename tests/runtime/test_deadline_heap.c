/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#include "postgamma/deadline_heap.h"
#include "../check.h"

/* Differential oracle exercises cancel/rearm and every heap shape. */
int
main(void)
{
	PostgammaDeadline nodes[257] = {{0}};
	PostgammaDeadline *entries[257];
	PostgammaDeadlineHeap heap = {entries, 0, 257};
	uint64_t deadlines[257] = {0};
	uint32_t random = 42;

	for (unsigned i = 0; i < 100000; i++)
	{
		size_t index;
		size_t count = 0;
		uint64_t minimum = UINT64_MAX;
		uint64_t deadline;

		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		index = random % 257;
		deadline = i % 3 == 0 ? 0 : (uint64_t) random + 1;
		postgamma_deadline_set(&heap, &nodes[index], deadline);
		deadlines[index] = deadline;
		for (size_t j = 0; j < 257; j++)
		{
			CHECK(nodes[j].at == deadlines[j]);
			if (deadlines[j] != 0)
			{
				count++;
				CHECK(nodes[j].position < heap.count);
				CHECK(entries[nodes[j].position] == &nodes[j]);
				if (deadlines[j] < minimum)
					minimum = deadlines[j];
			}
		}
		CHECK(count == heap.count);
		if (count != 0)
			CHECK(heap.entries[0]->at == minimum);
	}
	for (size_t i = 0; i < 257; i++)
		postgamma_deadline_set(&heap, &nodes[i], UINT64_MAX - i);
	CHECK(heap.count == 257);
	for (size_t i = 0; i < 257; i++)
	{
		PostgammaDeadline *entry = heap.entries[0];

		CHECK(entry->at == UINT64_MAX - 256 + i);
		postgamma_deadline_set(&heap, entry, 0);
	}
	CHECK(heap.count == 0);
	return 0;
}
