/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_LOGICAL_TOOL_HOST_H
#define POSTGAMMA_PRIVATE_LOGICAL_TOOL_HOST_H

#include "postgamma/postgamma.h"
#include "postgamma/private/memory_transport.h"

#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


typedef enum PostgammaLogicalToolKind
{
	POSTGAMMA_LOGICAL_TOOL_DUMP = 1,
	POSTGAMMA_LOGICAL_TOOL_RESTORE = 2
} PostgammaLogicalToolKind;

typedef struct PostgammaLogicalToolWorker PostgammaLogicalToolWorker;

#define POSTGAMMA_LOGICAL_TOOL_STREAM_PATH \
	"/@postgamma/in-process-logical-archive"

typedef struct PostgammaLogicalToolOptions
{
	uint32_t	struct_size;
	pgm_instance *instance;
	const char *database;
	const char *user;
	const char *archive_path;
	uint32_t	flags;
	PostgammaMemoryEndpoint *archive_endpoint;
	uint64_t	archive_generation;
} PostgammaLogicalToolOptions;

#define POSTGAMMA_LOGICAL_TOOL_OPTIONS_INIT \
	{sizeof(PostgammaLogicalToolOptions), NULL, NULL, NULL, NULL, UINT32_C(0), \
	 NULL, UINT64_C(0)}

typedef struct PostgammaLogicalToolResult
{
	uint32_t	struct_size;
	int			status;
	int			postgres_exit_code;
	uint64_t	request_generation;
	char		message[256];
} PostgammaLogicalToolResult;

#define POSTGAMMA_LOGICAL_TOOL_RESULT_INIT \
	{sizeof(PostgammaLogicalToolResult), 0, 0, UINT64_C(0), ""}

typedef void (*PostgammaLogicalToolCompletionFunction) (
	void *argument, PostgammaLogicalToolWorker *worker,
	const PostgammaLogicalToolResult *result);

int postgamma_logical_tool_dump_file(
	const PostgammaLogicalToolOptions *options,
	PostgammaLogicalToolResult *result);
int postgamma_logical_tool_restore_file(
	const PostgammaLogicalToolOptions *options,
	PostgammaLogicalToolResult *result);
int postgamma_logical_tool_start(
	PostgammaLogicalToolKind kind,
	const PostgammaLogicalToolOptions *options,
	PostgammaLogicalToolCompletionFunction completion,
	void *completion_argument,
	PostgammaLogicalToolWorker **worker);
int postgamma_logical_tool_cancel(PostgammaLogicalToolWorker *worker);


#ifdef __cplusplus
}
#endif


#endif /* POSTGAMMA_PRIVATE_LOGICAL_TOOL_HOST_H */
