/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#include "../check.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct SysloggerStartupData
{
	int syslogFile;
	int csvlogFile;
	int jsonlogFile;
} SysloggerStartupData;

static _Thread_local int syslogPipe[2] = {-1, -1};
static _Thread_local FILE *syslogFile;
static _Thread_local FILE *csvlogFile;
static _Thread_local FILE *jsonlogFile;

#define lengthof(array) (sizeof(array) / sizeof((array)[0]))
#define POSTGAMMA_BACKEND_STATE_NAMED_VALUE(identifier, original) (original)
static void postgamma_runtime_contract_violation(void) { abort(); }
#include "postgamma/postgres_syslogger_runtime_impl.h"

static void *
logger(void *argument)
{
	int descriptor = *(int *) argument;
	char byte;

	CHECK(syslogPipe[0] == -1 && syslogPipe[1] == -1);
	postgamma_install_syslogger_pipe_state(descriptor);
	CHECK(syslogPipe[0] == descriptor && syslogPipe[1] == -1);
	CHECK(read(syslogPipe[0], &byte, 1) == 1 && byte == 'x');
	postgamma_shutdown_syslogger_file_access();
	CHECK(syslogPipe[0] == -1 && syslogPipe[1] == -1);
	CHECK(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
	return NULL;
}

int
main(void)
{
	int descriptors[2];
	int transferred = -1;
	int rollback = -1;
	pthread_t thread;

	CHECK(postgamma_prepare_syslogger_pipe_state(&transferred) == EINVAL);
	CHECK(pipe(descriptors) == 0);
	syslogPipe[0] = descriptors[0];
	syslogPipe[1] = descriptors[1];
	CHECK(postgamma_prepare_syslogger_pipe_state(&transferred) == 0);
	CHECK(transferred != descriptors[0]);
	CHECK(fcntl(transferred, F_GETFD) & FD_CLOEXEC);
	CHECK(postgamma_prepare_syslogger_pipe_state(&rollback) == 0);
	postgamma_discard_syslogger_pipe(rollback);
	CHECK(fcntl(rollback, F_GETFD) == -1 && errno == EBADF);
	CHECK(fcntl(descriptors[0], F_GETFD) >= 0);
	CHECK(syslogPipe[0] == descriptors[0] && syslogPipe[1] == descriptors[1]);
	CHECK(close(descriptors[0]) == 0);
	CHECK(write(descriptors[1], "x", 1) == 1);
	CHECK(pthread_create(&thread, NULL, logger, &transferred) == 0);
	CHECK(pthread_join(thread, NULL) == 0);
	CHECK(syslogPipe[0] == descriptors[0] && syslogPipe[1] == descriptors[1]);
	CHECK(close(descriptors[1]) == 0);
	return 0;
}
