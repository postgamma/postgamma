#include "postgres.h"

#include <errno.h>
#include <string.h>

#include "fmgr.h"
#include "port/atomics.h"
#include "postgamma/postgamma_extension.h"
#include "utils/builtins.h"
#include "utils/guc.h"


PG_MODULE_MAGIC_EXT(
	.name = "postgamma_sdk_probe",
	.version = "1.0");


#define POSTGAMMA_SDK_PROBE_EXTENSION_ID "postgamma_sdk_probe"
#define POSTGAMMA_SDK_PROBE_SHARED_MAGIC UINT64_C(0x50474d363553484d)
#define POSTGAMMA_SDK_PROBE_RESOURCE \
	"share/postgamma/extensions/postgamma_sdk_probe/seed.txt"


typedef struct PostgammaSdkProbeSharedState
{
	uint64_t	magic;
	uint64_t	generation;
	pg_atomic_uint64 instance_startups;
	pg_atomic_uint64 instance_shutdowns;
	pg_atomic_uint64 active_sessions;
	pg_atomic_uint64 session_initializations;
	pg_atomic_uint64 session_resets;
	pg_atomic_uint64 session_destroys;
	pg_atomic_uint64 instance_counter;
	pg_atomic_uint64 parallel_worker_calls;
} PostgammaSdkProbeSharedState;

typedef struct PostgammaSdkProbeSessionState
{
	uint64_t	generation;
	uint64_t	connection_id;
	uint64_t	value;
} PostgammaSdkProbeSessionState;


static int probe_library_initialize(void);
static int probe_instance_request(const pgmex_context *context);
static int probe_instance_startup(const pgmex_context *context);
static void probe_instance_shutdown(const pgmex_context *context);
static int probe_session_initialize(const pgmex_context *context);
static int probe_session_reset(const pgmex_context *context);
static void probe_session_destroy(const pgmex_context *context);
static PostgammaSdkProbeSharedState *probe_shared_state(void);
static PostgammaSdkProbeSessionState *probe_session_state(void);


static const char *const PostgammaSdkProbeResources[] =
{
	"share/extension/postgamma_sdk_probe--1.0.sql",
	"share/extension/postgamma_sdk_probe.control",
	POSTGAMMA_SDK_PROBE_RESOURCE,
};

static const pgmex_descriptor PostgammaSdkProbeDescriptor =
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
	.id = POSTGAMMA_SDK_PROBE_EXTENSION_ID,
	.sql_name = "postgamma_sdk_probe",
	.version = "1.0",
	.shared_memory_name = "postgamma SDK probe",
	.shared_memory_size = sizeof(PostgammaSdkProbeSharedState),
	.shared_memory_alignment = PG_CACHE_LINE_SIZE,
	.session_state_size = sizeof(PostgammaSdkProbeSessionState),
	.resources = PostgammaSdkProbeResources,
	.resource_count = lengthof(PostgammaSdkProbeResources),
	.library_initialize = probe_library_initialize,
	.instance_request = probe_instance_request,
	.instance_startup = probe_instance_startup,
	.instance_shutdown = probe_instance_shutdown,
	.session_initialize = probe_session_initialize,
	.session_reset = probe_session_reset,
	.session_destroy = probe_session_destroy,
};


void
_PG_init(void)
{
	int			status = pgmex_register(&PostgammaSdkProbeDescriptor);

	if (status != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not register postgamma SDK probe extension"),
				 errdetail("Extension SDK status: %s", strerror(status))));
}


static int
probe_library_initialize(void)
{
	return 0;
}


static int
probe_instance_request(const pgmex_context *context)
{
	return context != NULL &&
		context->struct_size == sizeof(*context) &&
		context->phase == PGMEX_PHASE_INSTANCE_REQUEST &&
		context->instance_generation != 0 &&
		context->connection_id == 0 &&
		context->instance_shared_memory == NULL ? 0 : EPROTO;
}


static int
probe_instance_startup(const pgmex_context *context)
{
	PostgammaSdkProbeSharedState *shared;
	const char *cluster_name;

	if (context == NULL ||
		context->phase != PGMEX_PHASE_INSTANCE_STARTUP ||
		context->instance_generation == 0 || context->connection_id != 0 ||
		context->instance_shared_memory == NULL ||
		context->instance_shared_memory_size < sizeof(*shared))
		return EPROTO;
	cluster_name = GetConfigOptionByName("cluster_name", NULL, false);
	if (cluster_name != NULL &&
		strcmp(cluster_name, "postgamma-sdk-probe-fail") == 0)
		return ECANCELED;
	shared = context->instance_shared_memory;
	shared->magic = POSTGAMMA_SDK_PROBE_SHARED_MAGIC;
	shared->generation = context->instance_generation;
	pg_atomic_init_u64(&shared->instance_startups, UINT64_C(1));
	pg_atomic_init_u64(&shared->instance_shutdowns, UINT64_C(0));
	pg_atomic_init_u64(&shared->active_sessions, UINT64_C(0));
	pg_atomic_init_u64(&shared->session_initializations, UINT64_C(0));
	pg_atomic_init_u64(&shared->session_resets, UINT64_C(0));
	pg_atomic_init_u64(&shared->session_destroys, UINT64_C(0));
	pg_atomic_init_u64(&shared->instance_counter, UINT64_C(0));
	pg_atomic_init_u64(&shared->parallel_worker_calls, UINT64_C(0));
	return 0;
}


static void
probe_instance_shutdown(const pgmex_context *context)
{
	PostgammaSdkProbeSharedState *shared;

	if (context == NULL ||
		context->phase != PGMEX_PHASE_INSTANCE_SHUTDOWN ||
		context->instance_shared_memory == NULL)
		return;
	shared = context->instance_shared_memory;
	if (shared->magic == POSTGAMMA_SDK_PROBE_SHARED_MAGIC)
		(void) pg_atomic_fetch_add_u64(
			&shared->instance_shutdowns, UINT64_C(1));
}


static int
probe_session_initialize(const pgmex_context *context)
{
	PostgammaSdkProbeSharedState *shared;
	PostgammaSdkProbeSessionState *session;

	if (context == NULL ||
		context->phase != PGMEX_PHASE_SESSION_INITIALIZE ||
		context->instance_generation == 0 || context->connection_id == 0 ||
		context->instance_shared_memory == NULL ||
		context->instance_shared_memory_size < sizeof(*shared) ||
		context->session_state == NULL ||
		context->session_state_size < sizeof(*session))
		return EPROTO;
	shared = context->instance_shared_memory;
	if (shared->magic != POSTGAMMA_SDK_PROBE_SHARED_MAGIC ||
		shared->generation != context->instance_generation)
		return ESTALE;
	session = context->session_state;
	session->generation = context->instance_generation;
	session->connection_id = context->connection_id;
	session->value = context->connection_id * UINT64_C(1000);
	(void) pg_atomic_fetch_add_u64(&shared->active_sessions, UINT64_C(1));
	(void) pg_atomic_fetch_add_u64(
		&shared->session_initializations, UINT64_C(1));
	return 0;
}


static int
probe_session_reset(const pgmex_context *context)
{
	PostgammaSdkProbeSharedState *shared;
	PostgammaSdkProbeSessionState *session;

	if (context == NULL || context->phase != PGMEX_PHASE_SESSION_RESET ||
		context->instance_shared_memory == NULL ||
		context->instance_shared_memory_size < sizeof(*shared) ||
		context->session_state == NULL ||
		context->session_state_size < sizeof(*session))
		return EPROTO;
	shared = context->instance_shared_memory;
	session = context->session_state;
	if (shared->magic != POSTGAMMA_SDK_PROBE_SHARED_MAGIC ||
		shared->generation != context->instance_generation ||
		session->generation != context->instance_generation ||
		session->connection_id != context->connection_id)
		return ESTALE;
	session->value = session->connection_id * UINT64_C(1000);
	(void) pg_atomic_fetch_add_u64(&shared->session_resets, UINT64_C(1));
	return 0;
}


static void
probe_session_destroy(const pgmex_context *context)
{
	PostgammaSdkProbeSharedState *shared;
	PostgammaSdkProbeSessionState *session;

	if (context == NULL || context->phase != PGMEX_PHASE_SESSION_DESTROY ||
		context->instance_shared_memory == NULL ||
		context->session_state == NULL)
		return;
	shared = context->instance_shared_memory;
	session = context->session_state;
	if (shared->magic != POSTGAMMA_SDK_PROBE_SHARED_MAGIC ||
		session->generation != shared->generation)
		return;
	(void) pg_atomic_fetch_sub_u64(&shared->active_sessions, UINT64_C(1));
	(void) pg_atomic_fetch_add_u64(
		&shared->session_destroys, UINT64_C(1));
}


static PostgammaSdkProbeSharedState *
probe_shared_state(void)
{
	PostgammaSdkProbeSharedState *shared = pgmex_instance_shared_memory(
		POSTGAMMA_SDK_PROBE_EXTENSION_ID, sizeof(*shared));

	if (shared == NULL || shared->magic != POSTGAMMA_SDK_PROBE_SHARED_MAGIC ||
		shared->generation != pgmex_current_instance_generation())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("postgamma SDK instance state is unavailable")));
	return shared;
}


static PostgammaSdkProbeSessionState *
probe_session_state(void)
{
	PostgammaSdkProbeSessionState *session = pgmex_session_state(
		POSTGAMMA_SDK_PROBE_EXTENSION_ID, sizeof(*session));

	if (session == NULL ||
		session->generation != pgmex_current_instance_generation() ||
		session->connection_id != pgmex_current_connection_id())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("postgamma SDK session state is unavailable")));
	return session;
}


PG_FUNCTION_INFO_V1(postgamma_sdk_probe_snapshot);

Datum
postgamma_sdk_probe_snapshot(PG_FUNCTION_ARGS)
{
	PostgammaSdkProbeSharedState *shared = probe_shared_state();
	PostgammaSdkProbeSessionState *session = probe_session_state();
	char	   *snapshot;

	snapshot = psprintf(
		"generation=%llu connection=%llu startups=%llu shutdowns=%llu "
		"active_sessions=%llu session_initializations=%llu "
		"session_resets=%llu session_destroys=%llu session_value=%llu "
		"instance_counter=%llu parallel_worker_calls=%llu",
		(unsigned long long) shared->generation,
		(unsigned long long) session->connection_id,
		(unsigned long long) pg_atomic_read_u64(&shared->instance_startups),
		(unsigned long long) pg_atomic_read_u64(&shared->instance_shutdowns),
		(unsigned long long) pg_atomic_read_u64(&shared->active_sessions),
		(unsigned long long) pg_atomic_read_u64(
			&shared->session_initializations),
		(unsigned long long) pg_atomic_read_u64(&shared->session_resets),
		(unsigned long long) pg_atomic_read_u64(&shared->session_destroys),
		(unsigned long long) session->value,
		(unsigned long long) pg_atomic_read_u64(&shared->instance_counter),
		(unsigned long long) pg_atomic_read_u64(
			&shared->parallel_worker_calls));
	PG_RETURN_TEXT_P(cstring_to_text(snapshot));
}


PG_FUNCTION_INFO_V1(postgamma_sdk_probe_session_add);

Datum
postgamma_sdk_probe_session_add(PG_FUNCTION_ARGS)
{
	PostgammaSdkProbeSessionState *session = probe_session_state();
	int64		delta = PG_GETARG_INT64(0);

	session->value += (uint64_t) delta;
	PG_RETURN_INT64((int64) session->value);
}


PG_FUNCTION_INFO_V1(postgamma_sdk_probe_instance_add);

Datum
postgamma_sdk_probe_instance_add(PG_FUNCTION_ARGS)
{
	PostgammaSdkProbeSharedState *shared = probe_shared_state();
	uint64_t	delta = (uint64_t) PG_GETARG_INT64(0);
	uint64_t	previous = pg_atomic_fetch_add_u64(
		&shared->instance_counter, delta);

	PG_RETURN_INT64((int64) (previous + delta));
}


PG_FUNCTION_INFO_V1(postgamma_sdk_probe_resource);

Datum
postgamma_sdk_probe_resource(PG_FUNCTION_ARGS)
{
	char		buffer[128];
	size_t		transferred = 0;
	int			status;

	status = pgmex_resource_read(
		POSTGAMMA_SDK_PROBE_EXTENSION_ID, POSTGAMMA_SDK_PROBE_RESOURCE, 0,
		buffer, sizeof(buffer) - 1, &transferred);
	if (status != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read bundled extension resource"),
				 errdetail("Extension resource status: %s", strerror(status))));
	buffer[transferred] = '\0';
	while (transferred != 0 &&
		(buffer[transferred - 1] == '\n' || buffer[transferred - 1] == '\r'))
		buffer[--transferred] = '\0';
	PG_RETURN_TEXT_P(cstring_to_text(buffer));
}


PG_FUNCTION_INFO_V1(postgamma_sdk_probe_parallel_value);

Datum
postgamma_sdk_probe_parallel_value(PG_FUNCTION_ARGS)
{
	PostgammaSdkProbeSharedState *shared = probe_shared_state();
	int64		value = PG_GETARG_INT64(0);

	if (pgmex_current_connection_id() == 0)
		(void) pg_atomic_fetch_add_u64(
			&shared->parallel_worker_calls, UINT64_C(1));
	PG_RETURN_INT64(value);
}


PG_FUNCTION_INFO_V1(postgamma_sdk_probe_parallel_calls);

Datum
postgamma_sdk_probe_parallel_calls(PG_FUNCTION_ARGS)
{
	PostgammaSdkProbeSharedState *shared = probe_shared_state();

	PG_RETURN_INT64((int64) pg_atomic_read_u64(
		&shared->parallel_worker_calls));
}


PG_FUNCTION_INFO_V1(postgamma_sdk_probe_descriptor_rejected);

Datum
postgamma_sdk_probe_descriptor_rejected(PG_FUNCTION_ARGS)
{
	const char *single_resource[1];
	const char *duplicate_resources[2] = {"safe", "safe"};
	pgmex_descriptor candidate = PostgammaSdkProbeDescriptor;
	int32		scenario = PG_GETARG_INT32(0);
	int			status;

	switch (scenario)
	{
		case 1:
			candidate.struct_size--;
			break;
		case 2:
			candidate.abi_version++;
			break;
		case 3:
			candidate.postgresql_major++;
			break;
		case 4:
			candidate.reserved = UINT32_C(1);
			break;
		case 5:
			candidate.capabilities &= ~PGMEX_CAP_THREAD_SAFE;
			break;
		case 6:
			candidate.capabilities |= PGMEX_CAP_PROCESS_GLOBAL_STATE;
			break;
		case 7:
			candidate.shared_memory_alignment = 3;
			break;
		case 8:
			candidate.instance_startup = NULL;
			break;
		case 9:
			single_resource[0] = "../unsafe";
			candidate.resources = single_resource;
			candidate.resource_count = 1;
			break;
		case 10:
			single_resource[0] = "/absolute";
			candidate.resources = single_resource;
			candidate.resource_count = 1;
			break;
		case 11:
			candidate.resources = duplicate_resources;
			candidate.resource_count = lengthof(duplicate_resources);
			break;
		case 12:
			single_resource[0] = "trailing/";
			candidate.resources = single_resource;
			candidate.resource_count = 1;
			break;
		case 13:
			single_resource[0] = "backslash\\component";
			candidate.resources = single_resource;
			candidate.resource_count = 1;
			break;
		case 14:
			candidate.capabilities &= ~PGMEX_CAP_INSTANCE_SHMEM;
			break;
		case 15:
			candidate.capabilities |= PGMEX_CAP_FILESYSTEM_WRITE;
			break;
		case 16:
			candidate.session_state_size = 0;
			break;
		case 17:
			single_resource[0] = "repeated//separator";
			candidate.resources = single_resource;
			candidate.resource_count = 1;
			break;
		default:
			PG_RETURN_BOOL(false);
	}
	status = pgmex_register(&candidate);
	PG_RETURN_BOOL(status == EINVAL);
}
