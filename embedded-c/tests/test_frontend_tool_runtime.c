#define _POSIX_C_SOURCE 200809L

#include "postgamma/private/frontend_tool_runtime.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>


static void *
try_execution_lock(void *argument)
{
	int *status = argument;

	*status = postgamma_frontend_tool_execution_trylock();
	return NULL;
}


static void
test_process_wide_execution_lock(void)
{
	pthread_t thread;
	int status = 0;

	assert(postgamma_frontend_tool_execution_lock() == 0);
	assert(pthread_create(&thread, NULL, try_execution_lock, &status) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(status == EBUSY);
	assert(postgamma_frontend_tool_execution_unlock() == 0);
	assert(postgamma_frontend_tool_execution_trylock() == 0);
	assert(postgamma_frontend_tool_execution_unlock() == 0);
}


static void
test_getopt_full_reset(void)
{
	char *arguments[] = {"frontend-tool", "-a", NULL};

	optind = 47;
	opterr = 1;
	postgamma_frontend_tool_getopt_reset();
	assert(opterr == 0);
#if defined(__GLIBC__) || defined(__linux__) || defined(__CYGWIN__)
	assert(optind == 0);
#else
	assert(optind == 1);
#endif
	assert(getopt(2, arguments, "a") == 'a');
	assert(getopt(2, arguments, "a") == -1);
}


int
main(void)
{
	test_process_wide_execution_lock();
	test_getopt_full_reset();
	return 0;
}
