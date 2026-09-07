/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_extension_runtime.c
 *    Instance and logical-session lifecycle for bundled PG19 extensions.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "postgamma/extension_runtime.h"
#include "postgamma/instance_runtime.h"
#include "postgamma/path_runtime.h"
#include "postgamma/postmaster_control_runtime.h"
#include "postgamma/static_module_provider.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/elog.h"


typedef struct PostgammaExtensionBinding
{
	const pgmex_descriptor *descriptor;
	ShmemCallbacks callbacks;
	void	   *shared_memory;
	void	   *session_state;
	struct PostgammaExtensionBinding *next;
	bool		instance_requested;
	bool		instance_started;
	bool		session_initialized;
	bool		instance_owner;
	uint64_t	instance_requests;
	uint64_t	instance_startups;
	uint64_t	instance_shutdowns;
} PostgammaExtensionBinding;

typedef struct PostgammaExtensionExecutionState
{
	PostgammaExtensionBinding *bindings;
} PostgammaExtensionExecutionState;


static bool descriptor_is_valid(const pgmex_descriptor *descriptor);
static bool descriptor_resources_are_valid(
	const pgmex_descriptor *descriptor);
static PostgammaExtensionExecutionState *execution_state(
	PostgammaExecutionContext *execution, bool create);
static PostgammaExtensionBinding *find_binding(
	PostgammaExtensionExecutionState *state, const char *extension_id);
static pgmex_context lifecycle_context(
	const PostgammaExtensionBinding *binding, pgmex_phase phase);
static void extension_request(void *argument);
static void extension_initialize(void *argument);
static void extension_attach(void *argument);
static void extension_shutdown_callback(int code, Datum argument);
static void extension_shutdown(PostgammaExtensionBinding *binding);
static void raise_lifecycle_error(
	const pgmex_descriptor *descriptor, const char *phase, int status);
static bool resource_is_declared(
	const pgmex_descriptor *descriptor, const char *logical_path);


int
pgmex_register(const pgmex_descriptor *descriptor)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	PostgammaExtensionExecutionState *state;
	PostgammaExtensionBinding *binding;
	int			status;

	if (execution == NULL || !descriptor_is_valid(descriptor))
		return EINVAL;
	state = execution_state(execution, true);
	if (state == NULL)
		return ENOMEM;
	binding = find_binding(state, descriptor->id);
	if (binding != NULL)
		return binding->descriptor == descriptor ? 0 : EEXIST;
	binding = calloc(1, sizeof(*binding));
	if (binding == NULL)
		return ENOMEM;
	if (execution->connection_id != 0 && descriptor->session_state_size != 0)
	{
		binding->session_state = calloc(1, descriptor->session_state_size);
		if (binding->session_state == NULL)
		{
			free(binding);
			return ENOMEM;
		}
	}
	status = postgamma_static_module_register_extension(descriptor);
	if (status != 0)
	{
		free(binding->session_state);
		free(binding);
		return status;
	}
	binding->descriptor = descriptor;
	binding->instance_owner =
		postgamma_postmaster_control_runtime_is_bound();
	binding->callbacks.flags = 0;
	binding->callbacks.request_fn = extension_request;
	binding->callbacks.init_fn = extension_initialize;
	binding->callbacks.attach_fn = extension_attach;
	binding->callbacks.opaque_arg = binding;
	binding->next = state->bindings;
	state->bindings = binding;
	RegisterShmemCallbacks(&binding->callbacks);
	return 0;
}


uint64_t
pgmex_current_instance_generation(void)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	PostgammaInstanceRuntime *runtime;

	if (execution == NULL || execution->instance == NULL)
		return 0;
	runtime = postgamma_instance_context_runtime(execution->instance);
	return runtime != NULL ? postgamma_instance_runtime_generation(runtime) : 0;
}


uint64_t
pgmex_current_connection_id(void)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();

	return execution != NULL ? execution->connection_id : 0;
}


void *
pgmex_instance_shared_memory(const char *extension_id, size_t minimum_size)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	PostgammaExtensionExecutionState *state;
	PostgammaExtensionBinding *binding;

	if (execution == NULL || extension_id == NULL)
		return NULL;
	state = execution_state(execution, false);
	binding = find_binding(state, extension_id);
	if (binding == NULL || binding->shared_memory == NULL ||
		binding->descriptor->shared_memory_size < minimum_size)
		return NULL;
	return binding->shared_memory;
}


void *
pgmex_session_state(const char *extension_id, size_t minimum_size)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	PostgammaExtensionExecutionState *state;
	PostgammaExtensionBinding *binding;

	if (execution == NULL || extension_id == NULL)
		return NULL;
	state = execution_state(execution, false);
	binding = find_binding(state, extension_id);
	if (binding == NULL || !binding->session_initialized ||
		binding->session_state == NULL ||
		binding->descriptor->session_state_size < minimum_size)
		return NULL;
	return binding->session_state;
}


int
pgmex_resource_read(
	const char *extension_id, const char *logical_path, size_t offset,
	void *buffer, size_t capacity, size_t *transferred)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	PostgammaExtensionExecutionState *state;
	PostgammaExtensionBinding *binding;
	PostgammaPathRuntime *path_runtime;
	ssize_t		read_size;
	int			descriptor;
	int			status = 0;

	if (transferred == NULL || (capacity != 0 && buffer == NULL))
		return EINVAL;
	*transferred = 0;
	if (execution == NULL || extension_id == NULL || logical_path == NULL)
		return EINVAL;
	state = execution_state(execution, false);
	binding = find_binding(state, extension_id);
	if (binding == NULL ||
		(binding->descriptor->capabilities & PGMEX_CAP_FILESYSTEM_READ) == 0 ||
		!resource_is_declared(binding->descriptor, logical_path))
		return EACCES;
	path_runtime = postgamma_instance_context_path_runtime(execution->instance);
	if (path_runtime == NULL)
		return ENODEV;
	descriptor = postgamma_path_resource_open(
		path_runtime, logical_path, O_RDONLY);
	if (descriptor < 0)
		return errno != 0 ? errno : EIO;
	read_size = pread(descriptor, buffer, capacity, (off_t) offset);
	if (read_size < 0)
		status = errno != 0 ? errno : EIO;
	else
		*transferred = (size_t) read_size;
	if (close(descriptor) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	return status;
}


int
postgamma_extension_session_reset(PostgammaExecutionContext *execution)
{
	PostgammaExtensionExecutionState *state;
	PostgammaExtensionBinding *binding;
	int			first_error = 0;

	if (execution == NULL ||
		execution->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC)
		return EINVAL;
	state = execution_state(execution, false);
	if (state == NULL)
		return 0;
	for (binding = state->bindings; binding != NULL; binding = binding->next)
	{
		pgmex_context context;
		int			status;

		if (!binding->session_initialized ||
			binding->descriptor->session_reset == NULL)
			continue;
		context = lifecycle_context(binding, PGMEX_PHASE_SESSION_RESET);
		status = binding->descriptor->session_reset(&context);
		if (status != 0 && first_error == 0)
			first_error = status > 0 ? status : EPROTO;
	}
	return first_error;
}


void
postgamma_extension_reset_current_session(void)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	int			status;

	/* DISCARD ALL is also reachable in the non-embedded threaded profile. */
	if (execution == NULL)
		return;
	status = postgamma_extension_session_reset(execution);

	if (status != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bundled extension session reset failed"),
				 errdetail("Extension lifecycle status: %s",
					 strerror(status))));
}


int
postgamma_extension_instance_shutdown(PostgammaExecutionContext *execution)
{
	PostgammaExtensionExecutionState *state;
	PostgammaExtensionBinding *binding;

	if (execution == NULL ||
		execution->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC ||
		execution != postgamma_execution_context_current() ||
		execution->connection_id != 0)
		return EINVAL;
	state = execution_state(execution, false);
	if (state == NULL)
		return 0;
	for (binding = state->bindings; binding != NULL; binding = binding->next)
		extension_shutdown(binding);
	return 0;
}


int
postgamma_extension_execution_destroy(PostgammaExecutionContext *execution)
{
	PostgammaExtensionExecutionState *state;
	PostgammaExtensionBinding *binding;

	if (execution == NULL ||
		execution->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC ||
		execution != postgamma_execution_context_current())
		return EINVAL;
	state = execution_state(execution, false);
	if (state == NULL)
		return 0;
	binding = state->bindings;
	while (binding != NULL)
	{
		PostgammaExtensionBinding *next = binding->next;

		if (binding->session_initialized)
		{
			pgmex_context context = lifecycle_context(
				binding, PGMEX_PHASE_SESSION_DESTROY);

			binding->descriptor->session_destroy(&context);
			binding->session_initialized = false;
			(void) fprintf(
				stderr,
				"POSTGAMMA_EXTENSION_SESSION generation=%llu id=%s "
				"connection=%llu phase=destroyed\n",
				(unsigned long long) context.instance_generation,
				binding->descriptor->id,
				(unsigned long long) context.connection_id);
		}
		free(binding->session_state);
		memset(binding, 0, sizeof(*binding));
		free(binding);
		binding = next;
	}
	memset(state, 0, sizeof(*state));
	free(state);
	execution->postgres_extension_state = NULL;
	return 0;
}


static bool
descriptor_is_valid(const pgmex_descriptor *descriptor)
{
	const uint64_t required =
		PGMEX_CAP_THREAD_SAFE | PGMEX_CAP_MULTI_INSTANCE_SAFE |
		PGMEX_CAP_SESSION_MOBILITY_SAFE | PGMEX_CAP_PARALLEL_WORKER_SAFE;
	const uint64_t unsupported =
		PGMEX_CAP_BACKGROUND_WORKER | PGMEX_CAP_FILESYSTEM_WRITE |
		PGMEX_CAP_HOST_LIBRARY_DEPENDENCY |
		PGMEX_CAP_PROCESS_GLOBAL_STATE;
	bool		uses_instance_shmem;
	bool		has_session_state;

	if (descriptor == NULL ||
		descriptor->struct_size != sizeof(*descriptor) ||
		descriptor->abi_version != PGMEX_ABI_VERSION ||
		descriptor->postgresql_major != PG_VERSION_NUM / 10000 ||
		descriptor->reserved != 0 || descriptor->id == NULL ||
		descriptor->id[0] == '\0' || descriptor->sql_name == NULL ||
		descriptor->sql_name[0] == '\0' || descriptor->version == NULL ||
		descriptor->version[0] == '\0' ||
		(descriptor->capabilities & ~PGMEX_CAP_ALL) != 0 ||
		(descriptor->capabilities & required) != required ||
		(descriptor->capabilities & unsupported) != 0 ||
		descriptor->library_initialize == NULL ||
		descriptor->instance_request == NULL ||
		descriptor->instance_startup == NULL ||
		descriptor->instance_shutdown == NULL ||
		!descriptor_resources_are_valid(descriptor))
		return false;
	if (descriptor->resource_count != 0 &&
		(descriptor->capabilities & PGMEX_CAP_FILESYSTEM_READ) == 0)
		return false;
	uses_instance_shmem =
		(descriptor->capabilities & PGMEX_CAP_INSTANCE_SHMEM) != 0;
	if (uses_instance_shmem)
	{
		if (descriptor->shared_memory_name == NULL ||
			descriptor->shared_memory_name[0] == '\0' ||
			descriptor->shared_memory_size == 0 ||
			(descriptor->shared_memory_alignment != 0 &&
			 (descriptor->shared_memory_alignment &
			  (descriptor->shared_memory_alignment - 1)) != 0))
			return false;
	}
	else if (descriptor->shared_memory_name != NULL ||
			 descriptor->shared_memory_size != 0 ||
			 descriptor->shared_memory_alignment != 0)
		return false;
	has_session_state = descriptor->session_state_size != 0;
	if (has_session_state)
	{
		if (descriptor->session_initialize == NULL ||
			descriptor->session_destroy == NULL)
			return false;
	}
	else if (descriptor->session_initialize != NULL ||
			 descriptor->session_reset != NULL ||
			 descriptor->session_destroy != NULL)
		return false;
	return true;
}


static bool
descriptor_resources_are_valid(const pgmex_descriptor *descriptor)
{
	size_t		index;

	if (descriptor->resource_count != 0 && descriptor->resources == NULL)
		return false;
	for (index = 0; index < descriptor->resource_count; index++)
	{
		const char *path = descriptor->resources[index];
		size_t		other;

		if (!postgamma_path_resource_name_is_valid(path))
			return false;
		for (other = 0; other < index; other++)
		{
			if (strcmp(path, descriptor->resources[other]) == 0)
				return false;
		}
	}
	return true;
}


static PostgammaExtensionExecutionState *
execution_state(PostgammaExecutionContext *execution, bool create)
{
	PostgammaExtensionExecutionState *state;

	if (execution == NULL)
		return NULL;
	state = execution->postgres_extension_state;
	if (state == NULL && create)
	{
		state = calloc(1, sizeof(*state));
		if (state != NULL)
			execution->postgres_extension_state = state;
	}
	return state;
}


static PostgammaExtensionBinding *
find_binding(PostgammaExtensionExecutionState *state, const char *extension_id)
{
	PostgammaExtensionBinding *binding;

	if (state == NULL || extension_id == NULL)
		return NULL;
	for (binding = state->bindings; binding != NULL; binding = binding->next)
	{
		if (strcmp(binding->descriptor->id, extension_id) == 0)
			return binding;
	}
	return NULL;
}


static pgmex_context
lifecycle_context(
	const PostgammaExtensionBinding *binding, pgmex_phase phase)
{
	pgmex_context context = PGMEX_CONTEXT_INIT;

	context.phase = phase;
	context.instance_generation = pgmex_current_instance_generation();
	context.connection_id = pgmex_current_connection_id();
	context.instance_shared_memory = binding->shared_memory;
	context.instance_shared_memory_size =
		binding->descriptor->shared_memory_size;
	context.session_state = binding->session_state;
	context.session_state_size = binding->descriptor->session_state_size;
	return context;
}


static void
extension_request(void *argument)
{
	PostgammaExtensionBinding *binding = argument;
	pgmex_context context;
	int			status;

	if (binding == NULL || binding->instance_requested)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("invalid bundled extension request lifecycle")));
	if (binding->instance_owner)
	{
		context = lifecycle_context(binding, PGMEX_PHASE_INSTANCE_REQUEST);
		status = binding->descriptor->instance_request(&context);
		if (status != 0)
			raise_lifecycle_error(
				binding->descriptor, "instance request", status);
	}
	if ((binding->descriptor->capabilities & PGMEX_CAP_INSTANCE_SHMEM) != 0)
		ShmemRequestStruct(
			.name = binding->descriptor->shared_memory_name,
			.size = (ssize_t) binding->descriptor->shared_memory_size,
			.alignment = binding->descriptor->shared_memory_alignment,
			.ptr = &binding->shared_memory);
	binding->instance_requested = true;
	binding->instance_requests++;
}


static void
extension_initialize(void *argument)
{
	PostgammaExtensionBinding *binding = argument;
	pgmex_context context;
	bool		uses_instance_shmem;
	int			status;

	uses_instance_shmem = binding != NULL &&
		(binding->descriptor->capabilities & PGMEX_CAP_INSTANCE_SHMEM) != 0;
	if (binding == NULL || !binding->instance_owner ||
		!binding->instance_requested ||
		binding->instance_started ||
		uses_instance_shmem != (binding->shared_memory != NULL))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("invalid bundled extension startup lifecycle")));
	if (uses_instance_shmem)
		memset(binding->shared_memory, 0,
			binding->descriptor->shared_memory_size);
	context = lifecycle_context(binding, PGMEX_PHASE_INSTANCE_STARTUP);
	status = binding->descriptor->instance_startup(&context);
	if (status != 0)
		raise_lifecycle_error(binding->descriptor, "instance startup", status);
	binding->instance_started = true;
	binding->instance_startups++;
	before_shmem_exit(
		extension_shutdown_callback, PointerGetDatum(binding->descriptor));
}


static void
extension_attach(void *argument)
{
	PostgammaExtensionBinding *binding = argument;
	pgmex_context context;
	bool		uses_instance_shmem;
	int			status;

	uses_instance_shmem = binding != NULL &&
		(binding->descriptor->capabilities & PGMEX_CAP_INSTANCE_SHMEM) != 0;
	if (binding == NULL || !binding->instance_requested ||
		uses_instance_shmem != (binding->shared_memory != NULL))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("invalid bundled extension attach lifecycle")));
	if (pgmex_current_connection_id() == 0)
		return;
	if (binding->descriptor->session_state_size == 0)
		return;
	if (binding->session_initialized || binding->session_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("invalid bundled extension session lifecycle")));
	context = lifecycle_context(binding, PGMEX_PHASE_SESSION_INITIALIZE);
	status = binding->descriptor->session_initialize(&context);
	if (status != 0)
		raise_lifecycle_error(binding->descriptor, "session initialize", status);
	binding->session_initialized = true;
	(void) fprintf(
		stderr,
		"POSTGAMMA_EXTENSION_SESSION generation=%llu id=%s "
		"connection=%llu phase=initialized\n",
		(unsigned long long) context.instance_generation,
		binding->descriptor->id,
		(unsigned long long) context.connection_id);
}


static void
extension_shutdown_callback(int code, Datum argument)
{
	const pgmex_descriptor *descriptor = DatumGetPointer(argument);
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	PostgammaExtensionExecutionState *state =
		execution_state(execution, false);
	PostgammaExtensionBinding *binding = descriptor != NULL ?
		find_binding(state, descriptor->id) : NULL;

	(void) code;
	if (binding != NULL && binding->descriptor == descriptor)
		extension_shutdown(binding);
}


static void
extension_shutdown(PostgammaExtensionBinding *binding)
{
	pgmex_context context;

	if (binding == NULL || !binding->instance_started)
		return;
	context = lifecycle_context(binding, PGMEX_PHASE_INSTANCE_SHUTDOWN);
	binding->descriptor->instance_shutdown(&context);
	binding->instance_started = false;
	binding->instance_shutdowns++;
	(void) fprintf(
		stderr,
		"POSTGAMMA_EXTENSION generation=%llu id=%s "
		"instance_requests=%llu instance_startups=%llu "
		"instance_shutdowns=%llu "
		"phase=closed\n",
		(unsigned long long) context.instance_generation,
		binding->descriptor->id,
		(unsigned long long) binding->instance_requests,
		(unsigned long long) binding->instance_startups,
		(unsigned long long) binding->instance_shutdowns);
}


static void
raise_lifecycle_error(
	const pgmex_descriptor *descriptor, const char *phase, int status)
{
	int			error_number = status > 0 ? status : EPROTO;

	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bundled extension \"%s\" failed during %s",
				descriptor->id, phase),
			 errdetail("Extension lifecycle status: %s",
				strerror(error_number))));
}


static bool
resource_is_declared(
	const pgmex_descriptor *descriptor, const char *logical_path)
{
	size_t		index;

	if (logical_path[0] == '\0')
		return false;
	for (index = 0; index < descriptor->resource_count; index++)
	{
		if (descriptor->resources[index] != NULL &&
			strcmp(descriptor->resources[index], logical_path) == 0)
			return true;
	}
	return false;
}
