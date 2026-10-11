/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_LOGICAL_TOOL_RUNTIME_H
#define POSTGAMMA_PRIVATE_LOGICAL_TOOL_RUNTIME_H

/*
 * Forced include for transformed PostgreSQL logical-tool translation units.
 * Process assumptions are intercepted by the localized host object; this
 * header supplies only generated invocation-state access at source level.
 */
#include "postgres_fe.h"

#include "postgamma/tool_state_runtime.h"

#include <stdio.h>
#include "libpq-fe.h"

/*
 * pg_dump normally resolves these public encoding helpers from libpq.  The
 * embedded closure links the matching PostgreSQL common archive directly, so
 * bind every tool reference to that archive's version-private spellings.
 */
#define pg_char_to_encoding pg_char_to_encoding_private
#define pg_encoding_to_char pg_encoding_to_char_private
#define pg_valid_server_encoding pg_valid_server_encoding_private
#define pg_valid_server_encoding_id pg_valid_server_encoding_id_private
#define pg_utf_mblen pg_utf_mblen_private

extern int pg_char_to_encoding_private(const char *name);
extern const char *pg_encoding_to_char_private(int encoding);
extern int pg_valid_server_encoding_private(const char *name);
extern int pg_valid_server_encoding_id_private(int encoding);
extern int pg_utf_mblen_private(const unsigned char *value);

#define fopen postgamma_logical_tool_fopen
#define fdopen postgamma_logical_tool_fdopen
#define fclose postgamma_logical_tool_fclose
extern FILE *postgamma_logical_tool_fopen(
	const char *path, const char *mode);
extern FILE *postgamma_logical_tool_fdopen(int descriptor, const char *mode);
extern int postgamma_logical_tool_fclose(FILE *stream);

/* Frontend diagnostics belong to the operation, never to the host process. */
#undef stderr
#define stderr postgamma_logical_tool_stderr()
extern FILE *postgamma_logical_tool_stderr(void);

/* A fatal archive write can bypass pg_dump's PQfreemem(copybuf). */
#define PQgetCopyData postgamma_logical_tool_get_copy_data
#define PQfreemem postgamma_logical_tool_freemem
extern int postgamma_logical_tool_get_copy_data(PGconn *connection, char **buffer, int async);
extern void postgamma_logical_tool_freemem(void *pointer);


#endif /* POSTGAMMA_PRIVATE_LOGICAL_TOOL_RUNTIME_H */
