/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#ifndef POSTGAMMA_WASM_PSQL_RUNTIME_H
#define POSTGAMMA_WASM_PSQL_RUNTIME_H
#include "postgres_fe.h"
#include <stdio.h>
#include <unistd.h>
extern FILE *postgamma_psql_stdin(void);
extern FILE *postgamma_psql_stdout(void);
extern char *postgamma_psql_fgets(char *, int, FILE *);
extern int postgamma_psql_fileno(FILE *);
#undef stdin
#undef stdout
#undef stderr
#define stdin postgamma_psql_stdin()
#define stdout postgamma_psql_stdout()
#define stderr postgamma_psql_stdout()
#define printf(...) fprintf(stdout, __VA_ARGS__)
#define puts(s) (fputs((s), stdout), fputc('\n', stdout))
#define putchar(c) fputc((c), stdout)
#define fgets postgamma_psql_fgets
#define fileno postgamma_psql_fileno
/* Keep frontend allocations separate from the database/libpq ownership. */
#include "logical_alloc.h"
#endif
