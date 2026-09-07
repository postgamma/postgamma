/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_INITDB_RUNTIME_H
#define POSTGAMMA_PRIVATE_INITDB_RUNTIME_H

#include "postgres_fe.h"

#include "postgamma/private/initdb_host.h"

#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>

#include "port.h"


#ifdef __cplusplus
extern "C" {
#endif


pg_noreturn void postgamma_initdb_exit(int code);
int postgamma_initdb_atexit(void (*function) (void));
int postgamma_initdb_fflush(FILE *stream);
int postgamma_initdb_phase_select(
	PostgammaInitdbPhase phase,
	uint64_t wal_segment_size_bytes,
	bool data_checksums);
FILE *postgamma_initdb_popen(const char *command, const char *mode);
int postgamma_initdb_pclose_check(FILE *stream);
int postgamma_initdb_system(const char *command);
pqsigfunc postgamma_initdb_pqsignal(int signal_number, pqsigfunc handler);
mode_t postgamma_initdb_umask(mode_t mask);
char *postgamma_initdb_setlocale(int category, const char *locale);
int postgamma_initdb_setenv(
	const char *name, const char *value, int overwrite);
int postgamma_initdb_unsetenv(const char *name);
uid_t postgamma_initdb_geteuid(void);
int postgamma_initdb_find_other_exec(
	const char *argv0, const char *target, const char *versionstr,
	char *retpath);
void postgamma_initdb_set_pglocale_pgservice(
	const char *argv0, const char *app);
FILE *postgamma_initdb_stdout(void);
FILE *postgamma_initdb_stderr(void);
int postgamma_initdb_printf(const char *format, ...)
	pg_attribute_printf(1, 2);
int postgamma_initdb_puts(const char *value);


#ifdef __cplusplus
}
#endif


#ifdef POSTGAMMA_INITDB_UPSTREAM
#define exit(code) postgamma_initdb_exit((code))
#define atexit(function) postgamma_initdb_atexit((function))
#define fflush(stream) postgamma_initdb_fflush((stream))
#define POSTGAMMA_INITDB_PHASE_SELECT(phase, wal_size, checksums) \
	do { \
		if (postgamma_initdb_phase_select( \
				(phase), (wal_size), (checksums)) != 0) \
			exit(1); \
	} while (0)
#define popen(command, mode) postgamma_initdb_popen((command), (mode))
#define pclose_check(stream) postgamma_initdb_pclose_check((stream))
#define system(command) postgamma_initdb_system((command))
#define pqsignal_fe(signal_number, handler) \
	postgamma_initdb_pqsignal((signal_number), (handler))
#define umask(mask) postgamma_initdb_umask((mask))
#define setlocale(category, locale) \
	postgamma_initdb_setlocale((category), (locale))
#define setenv(name, value, overwrite) \
	postgamma_initdb_setenv((name), (value), (overwrite))
#define unsetenv(name) postgamma_initdb_unsetenv((name))
#define geteuid() postgamma_initdb_geteuid()
#define find_other_exec(argv0, target, versionstr, retpath) \
	postgamma_initdb_find_other_exec( \
		(argv0), (target), (versionstr), (retpath))
#define set_pglocale_pgservice(argv0, app) \
	postgamma_initdb_set_pglocale_pgservice((argv0), (app))
#ifdef printf
#undef printf
#endif
#define printf(...) postgamma_initdb_printf(__VA_ARGS__)
#ifdef puts
#undef puts
#endif
#define puts(value) postgamma_initdb_puts((value))
#ifdef stdout
#undef stdout
#endif
#define stdout postgamma_initdb_stdout()
#ifdef stderr
#undef stderr
#endif
#define stderr postgamma_initdb_stderr()
#endif /* POSTGAMMA_INITDB_UPSTREAM */


#endif /* POSTGAMMA_PRIVATE_INITDB_RUNTIME_H */
