/* Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 * Diagnostic link only: -Wl,--wrap=pthread_getattr_np.
 */
#define _GNU_SOURCE
#include "postgamma/thread_runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

int __real_pthread_getattr_np(pthread_t thread, pthread_attr_t *attributes);
int __wrap_pthread_getattr_np(pthread_t thread, pthread_attr_t *attributes);

int
__wrap_pthread_getattr_np(pthread_t thread, pthread_attr_t *attributes)
{
	const char *marker = getenv("POSTGAMMA_TEST_STACK_FAILURE");
	int status = __real_pthread_getattr_np(thread, attributes);
	size_t actual;
	size_t configured;
	size_t guard;

	if (status == 0 && marker != NULL && access(marker, F_OK) == 0 &&
		postgamma_thread_role_stack_policy(POSTGAMMA_THREAD_ROLE_CLIENT,
			&configured, &guard) == 0 &&
		pthread_attr_getstacksize(attributes, &actual) == 0 && actual == configured)
	{
		(void) pthread_attr_destroy(attributes);
		return EIO;
	}
	return status;
}
