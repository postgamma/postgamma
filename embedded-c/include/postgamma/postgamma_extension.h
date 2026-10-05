/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_EXTENSION_H
#define POSTGAMMA_EXTENSION_H

/*
 * Stable C contract for native extensions bundled with PostGamma embedded.
 *
 * This header deliberately exposes no PostgreSQL implementation type.  A
 * bundled extension may include PostgreSQL headers in its own translation
 * unit, but all lifecycle and ownership exchange with PostGamma uses the
 * fixed-width types below.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PGMEX_ABI_VERSION UINT32_C(1)

/*
 * The first four bits are mandatory reviewed safety claims, not requests for
 * hidden runtime emulation:
 *
 * - THREAD_SAFE forbids unsynchronized process-global mutable state.
 * - MULTI_INSTANCE_SAFE requires all instance state to be obtained through
 *   the instance lifecycle and shared-memory APIs.
 * - SESSION_MOBILITY_SAFE requires logical-session state to remain valid when
 *   the same session moves between execution carriers.  Carrier unbind does
 *   not reset a session; session_reset is reserved for explicit PostgreSQL
 *   reset operations such as DISCARD ALL.
 * - PARALLEL_WORKER_SAFE permits reviewed SQL entry points to run in a
 *   PostgreSQL parallel worker.  Such a worker has connection id zero and no
 *   logical-session state.  Entry points that call pgmex_session_state must be
 *   declared PARALLEL RESTRICTED or PARALLEL UNSAFE in extension SQL.
 *
 * INSTANCE_SHMEM and FILESYSTEM_READ are runtime-enforced permissions.  SDK
 * ABI v1 rejects BACKGROUND_WORKER, FILESYSTEM_WRITE,
 * HOST_LIBRARY_DEPENDENCY, and PROCESS_GLOBAL_STATE.
 */
#define PGMEX_CAP_THREAD_SAFE             (UINT64_C(1) << 0)
#define PGMEX_CAP_MULTI_INSTANCE_SAFE     (UINT64_C(1) << 1)
#define PGMEX_CAP_SESSION_MOBILITY_SAFE   (UINT64_C(1) << 2)
#define PGMEX_CAP_PARALLEL_WORKER_SAFE    (UINT64_C(1) << 3)
#define PGMEX_CAP_INSTANCE_SHMEM          (UINT64_C(1) << 4)
#define PGMEX_CAP_BACKGROUND_WORKER       (UINT64_C(1) << 5)
#define PGMEX_CAP_FILESYSTEM_READ         (UINT64_C(1) << 6)
#define PGMEX_CAP_FILESYSTEM_WRITE        (UINT64_C(1) << 7)
#define PGMEX_CAP_HOST_LIBRARY_DEPENDENCY (UINT64_C(1) << 8)
#define PGMEX_CAP_PROCESS_GLOBAL_STATE    (UINT64_C(1) << 9)
#define PGMEX_CAP_ALL                     UINT64_C(0x03ff)

typedef int32_t pgmex_phase;

#define PGMEX_PHASE_LIBRARY_INITIALIZE INT32_C(0)
#define PGMEX_PHASE_INSTANCE_REQUEST   INT32_C(1)
#define PGMEX_PHASE_INSTANCE_STARTUP   INT32_C(2)
#define PGMEX_PHASE_INSTANCE_SHUTDOWN  INT32_C(3)
#define PGMEX_PHASE_SESSION_INITIALIZE INT32_C(4)
#define PGMEX_PHASE_SESSION_RESET      INT32_C(5)
#define PGMEX_PHASE_SESSION_DESTROY    INT32_C(6)

typedef struct pgmex_context
{
	uint32_t	struct_size;
	pgmex_phase phase;
	uint64_t	instance_generation;
	uint64_t	connection_id;
	void	   *instance_shared_memory;
	size_t		instance_shared_memory_size;
	void	   *session_state;
	size_t		session_state_size;
} pgmex_context;

#define PGMEX_CONTEXT_INIT \
	{sizeof(pgmex_context), PGMEX_PHASE_LIBRARY_INITIALIZE, UINT64_C(0), \
	 UINT64_C(0), NULL, 0, NULL, 0}

typedef int (*pgmex_library_initialize_callback) (void);
typedef int (*pgmex_lifecycle_callback) (const pgmex_context *context);
typedef void (*pgmex_lifecycle_destroy_callback) (
	const pgmex_context *context);

typedef struct pgmex_descriptor
{
	uint32_t	struct_size;
	uint32_t	abi_version;
	uint32_t	postgresql_major;
	uint32_t	reserved;
	uint64_t	capabilities;
	const char *id;
	const char *sql_name;
	const char *version;
	const char *shared_memory_name;
	size_t		shared_memory_size;
	size_t		shared_memory_alignment;
	size_t		session_state_size;
	const char *const *resources;
	size_t		resource_count;
	pgmex_library_initialize_callback library_initialize;
	pgmex_lifecycle_callback instance_request;
	pgmex_lifecycle_callback instance_startup;
	pgmex_lifecycle_destroy_callback instance_shutdown;
	pgmex_lifecycle_callback session_initialize;
	pgmex_lifecycle_callback session_reset;
	pgmex_lifecycle_destroy_callback session_destroy;
} pgmex_descriptor;

/*
 * Register exactly one immutable descriptor from _PG_init().  Registration
 * is repeated in each PostgreSQL role, while library initialization and
 * instance startup retain their documented once-only cardinality.
 * library_initialize must be bounded, nonblocking, and free of external I/O;
 * concurrent first registration waits for that process-wide callback.
 */
int pgmex_register(const pgmex_descriptor *descriptor);

uint64_t pgmex_current_instance_generation(void);
uint64_t pgmex_current_connection_id(void);
void *pgmex_instance_shared_memory(
	const char *extension_id, size_t minimum_size);
void *pgmex_session_state(
	const char *extension_id, size_t minimum_size);

/*
 * Read a manifest-declared immutable resource.  logical_path is relative to
 * the resource-pack root, may not contain dot components, and never resolves
 * through the host working directory.  Returns zero or an errno value.
 */
int pgmex_resource_read(
	const char *extension_id, const char *logical_path, size_t offset,
	void *buffer, size_t capacity, size_t *transferred);

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_EXTENSION_H */
