/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgres.h"

#include <errno.h>
#include <string.h>

#include "fmgr.h"
#include "postgamma/postgamma_extension.h"
#include "postgamma_pgvector_adapter.h"
#include "utils/elog.h"


#define POSTGAMMA_PGVECTOR_EXTENSION_ID "pgvector"
#define POSTGAMMA_PGVECTOR_SHARED_MAGIC UINT64_C(0x5047564543544f52)


typedef struct PostgammaPgvectorSharedState
{
	uint64_t	magic;
	uint64_t	generation;
} PostgammaPgvectorSharedState;


static int pgvector_library_initialize(void);
static int pgvector_instance_request(const pgmex_context *context);
static int pgvector_instance_startup(const pgmex_context *context);
static void pgvector_instance_shutdown(const pgmex_context *context);

extern void postgamma_pgvector_upstream_init(void);


static const char *const PostgammaPgvectorResources[] =
{
	"share/extension/vector.control",
	"share/extension/vector--0.8.6.sql",
};

static const pgmex_descriptor PostgammaPgvectorDescriptor =
{
	.struct_size = sizeof(pgmex_descriptor),
	.abi_version = PGMEX_ABI_VERSION,
	.postgresql_major = 19,
	.reserved = 0,
	.capabilities =
		PGMEX_CAP_THREAD_SAFE |
		PGMEX_CAP_MULTI_INSTANCE_SAFE |
		PGMEX_CAP_SESSION_MOBILITY_SAFE |
		PGMEX_CAP_PARALLEL_WORKER_SAFE |
		PGMEX_CAP_INSTANCE_SHMEM |
		PGMEX_CAP_FILESYSTEM_READ,
	.id = POSTGAMMA_PGVECTOR_EXTENSION_ID,
	.sql_name = "vector",
	.version = "0.8.6",
	.shared_memory_name = "PostGamma pgvector adapter",
	.shared_memory_size = sizeof(PostgammaPgvectorSharedState),
	.shared_memory_alignment = _Alignof(PostgammaPgvectorSharedState),
	.session_state_size = 0,
	.resources = PostgammaPgvectorResources,
	.resource_count = lengthof(PostgammaPgvectorResources),
	.library_initialize = pgvector_library_initialize,
	.instance_request = pgvector_instance_request,
	.instance_startup = pgvector_instance_startup,
	.instance_shutdown = pgvector_instance_shutdown,
	.session_initialize = NULL,
	.session_reset = NULL,
	.session_destroy = NULL,
};


void
_PG_init(void)
{
	int			status = pgmex_register(&PostgammaPgvectorDescriptor);

	if (status != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not register the bundled pgvector extension"),
				 errdetail("Extension SDK status: %s", strerror(status))));
	postgamma_pgvector_upstream_init();
}


void *
postgamma_pgvector_role_state_address(
	const char *identifier, const volatile void *template_address,
	size_t size, size_t alignment)
{
	void	   *value = postgamma_extension_role_state_address(
		identifier, template_address, size, alignment);

	if (value == NULL)
	{
		int			status = errno != 0 ? errno : EINVAL;

		ereport(ERROR,
				(errcode(status == ENOMEM ? ERRCODE_OUT_OF_MEMORY :
						ERRCODE_INTERNAL_ERROR),
				 errmsg("could not bind pgvector role state"),
				 errdetail("Role-state status: %s", strerror(status))));
	}
	return value;
}


static int
pgvector_library_initialize(void)
{
	return 0;
}


static int
pgvector_instance_request(const pgmex_context *context)
{
	return context != NULL &&
		context->struct_size == sizeof(*context) &&
		context->phase == PGMEX_PHASE_INSTANCE_REQUEST &&
		context->instance_generation != 0 &&
		context->connection_id == 0 &&
		context->instance_shared_memory == NULL ? 0 : EPROTO;
}


static int
pgvector_instance_startup(const pgmex_context *context)
{
	PostgammaPgvectorSharedState *shared;

	if (context == NULL ||
		context->struct_size != sizeof(*context) ||
		context->phase != PGMEX_PHASE_INSTANCE_STARTUP ||
		context->instance_generation == 0 || context->connection_id != 0 ||
		context->instance_shared_memory == NULL ||
		context->instance_shared_memory_size < sizeof(*shared))
		return EPROTO;
	shared = context->instance_shared_memory;
	shared->magic = POSTGAMMA_PGVECTOR_SHARED_MAGIC;
	shared->generation = context->instance_generation;
	return 0;
}


static void
pgvector_instance_shutdown(const pgmex_context *context)
{
	PostgammaPgvectorSharedState *shared;

	if (context == NULL ||
		context->phase != PGMEX_PHASE_INSTANCE_SHUTDOWN ||
		context->instance_shared_memory == NULL ||
		context->instance_shared_memory_size < sizeof(*shared))
		return;
	shared = context->instance_shared_memory;
	if (shared->magic == POSTGAMMA_PGVECTOR_SHARED_MAGIC &&
		shared->generation == context->instance_generation)
		memset(shared, 0, sizeof(*shared));
}
