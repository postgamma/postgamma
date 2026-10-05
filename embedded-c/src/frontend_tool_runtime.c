/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "postgamma/private/frontend_tool_runtime.h"

#include <pthread.h>
#include <unistd.h>


static pthread_mutex_t PostgammaFrontendToolExecutionMutex =
	PTHREAD_MUTEX_INITIALIZER;


int
postgamma_frontend_tool_execution_lock(void)
{
	return pthread_mutex_lock(&PostgammaFrontendToolExecutionMutex);
}


int
postgamma_frontend_tool_execution_trylock(void)
{
	return pthread_mutex_trylock(&PostgammaFrontendToolExecutionMutex);
}


int
postgamma_frontend_tool_execution_unlock(void)
{
	return pthread_mutex_unlock(&PostgammaFrontendToolExecutionMutex);
}


void
postgamma_frontend_tool_getopt_reset(void)
{
	/*
	 * GNU-compatible implementations reserve zero for a complete reset of
	 * hidden parser state.  Assigning one resets only the public cursor and can
	 * retain permutation pointers into a previous invocation's argv array.
	 */
#if defined(__GLIBC__) || defined(__linux__) || defined(__CYGWIN__)
	optind = 0;
#else
	optind = 1;
#endif

	/* BSD-derived implementations expose a separate full-reset flag. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
	defined(__OpenBSD__) || defined(__DragonFly__)
	extern int optreset;

	optreset = 1;
#endif
	opterr = 0;
}
