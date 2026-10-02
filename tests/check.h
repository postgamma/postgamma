/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_TEST_CHECK_H
#define POSTGAMMA_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

/* Test actions and checks must run even when the compiler defines NDEBUG. */
#define CHECK(condition) \
	do { \
		if (!(condition)) \
		{ \
			fprintf(stderr, "%s:%d: check failed: %s\n", \
					__FILE__, __LINE__, #condition); \
			exit(EXIT_FAILURE); \
		} \
	} while (0)

#endif
