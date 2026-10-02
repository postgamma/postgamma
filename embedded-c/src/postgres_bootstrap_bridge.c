/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <setjmp.h>
#include <stdlib.h>
#include <unistd.h>

#include "bootstrap/bootstrap.h"
#include "miscadmin.h"
#include "postgamma/instance_runtime.h"
#include "postgamma/path_runtime.h"
#include "postgamma/private/postgres_bootstrap_bridge.h"
#include "postgamma/private/bootstrap_probe.h"
#include "postgamma/private/tool_context.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"

/* This host bridge must open the instance root before path virtualization. */
#ifdef open
#undef open
#endif
#ifdef realpath
#undef realpath
#endif


typedef struct PostgammaBootstrapKernelContext
{
	PostgammaToolContext *tool;
	PostgammaInstanceRuntime *instance_runtime;
	PostgammaPathRuntime *path_runtime;
	PostgammaInstanceContext instance;
	PostgammaRoleContext role;
	PostgammaExecutionContext execution;
	PostgammaExecutionContext *previous_execution;
	char	   *canonical_data_directory;
	char	   *bootstrap_input_data;
	size_t		bootstrap_input_length;
	int			data_directory_fd;
	sigjmp_buf	exit_jump;
	int			exit_code;
	int			exit_handler_status;
	bool		instance_initialized;
	bool		instance_runtime_attached;
	bool		path_runtime_attached;
	bool		role_initialized;
	bool		execution_initialized;
	bool		execution_bound;
	bool		bootstrap_input_bound;
	bool		standalone_input_bound;
	bool		exit_handler_set;
	bool		memory_contexts_initialized;
} PostgammaBootstrapKernelContext;


static _Thread_local const char *PostgammaBootstrapMemoryInput;
static _Thread_local size_t PostgammaBootstrapMemoryInputLength;
static _Thread_local uint64_t PostgammaBootstrapWalSegmentSize =
	UINT64_C(16777216);
static _Thread_local bool PostgammaBootstrapDataChecksums = true;
static _Thread_local mode_t PostgammaBootstrapLogicalUmask = 0077;


static int
postgamma_read_bootstrap_input(
	const char *path, char **input, size_t *input_length)
{
	struct stat status_buffer;
	char	   *buffer;
	size_t		used = 0;
	int			descriptor;
	int			status = 0;

	if (path == NULL || input == NULL || input_length == NULL)
		return EINVAL;
	*input = NULL;
	*input_length = 0;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno != 0 ? errno : EIO;
	if (fstat(descriptor, &status_buffer) != 0)
	{
		status = errno != 0 ? errno : EIO;
		goto finish;
	}
	if (status_buffer.st_size <= 0 ||
		(uintmax_t) status_buffer.st_size > SIZE_MAX - 1)
	{
		status = EFBIG;
		goto finish;
	}
	buffer = malloc((size_t) status_buffer.st_size + 1);
	if (buffer == NULL)
	{
		status = ENOMEM;
		goto finish;
	}
	while (used < (size_t) status_buffer.st_size)
	{
		ssize_t count = read(
			descriptor, buffer + used, (size_t) status_buffer.st_size - used);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
		{
			status = count == 0 ? EIO : (errno != 0 ? errno : EIO);
			free(buffer);
			goto finish;
		}
		used += (size_t) count;
	}
	buffer[used] = '\0';
	*input = buffer;
	*input_length = used;

finish:
	if (close(descriptor) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	return status;
}


static void
postgamma_bootstrap_exit(void *argument, int code)
{
	PostgammaBootstrapKernelContext *kernel = argument;

	kernel->exit_code = code;
	kernel->exit_handler_status =
		postgamma_tool_context_record_exit(kernel->tool, code);
	siglongjmp(kernel->exit_jump, 1);
}


static int
postgamma_bootstrap_kernel_cleanup(void *argument)
{
	PostgammaBootstrapKernelContext *kernel = argument;
	int			status = 0;
	int			operation_status;

	if (kernel->exit_handler_set)
	{
		postgamma_execution_context_set_exit_handler(
			&kernel->execution, NULL, NULL);
		kernel->exit_handler_set = false;
	}
	if (kernel->execution_bound)
	{
		operation_status = postgamma_extension_instance_shutdown(
			&kernel->execution);
		if (operation_status != 0 && status == 0)
			status = operation_status;
		operation_status = postgamma_extension_execution_destroy(
			&kernel->execution);
		if (operation_status != 0 && status == 0)
			status = operation_status;
	}
	if (kernel->memory_contexts_initialized)
	{
		postgamma_shutdown_latch_wait_set();
		postgamma_shutdown_wait_event_support();
		postgamma_shutdown_xlog_file_access();
		postgamma_shutdown_file_access(false);
		postgamma_destroy_postgres_memory_contexts(true);
		kernel->memory_contexts_initialized = false;
	}
	if (kernel->bootstrap_input_bound)
	{
		int input_status = postgamma_bootstrap_input_unbind(
			kernel->bootstrap_input_data);

		if (input_status != 0)
			status = input_status;
		kernel->bootstrap_input_bound = false;
	}
	if (kernel->standalone_input_bound)
	{
		int input_status = postgamma_standalone_input_unbind(
			kernel->bootstrap_input_data);

		if (input_status != 0 && status == 0)
			status = input_status;
		kernel->standalone_input_bound = false;
	}
	free(kernel->bootstrap_input_data);
	kernel->bootstrap_input_data = NULL;
	kernel->bootstrap_input_length = 0;
	if (kernel->execution_initialized &&
		kernel->execution.postgres_guc_control != NULL)
		postgamma_destroy_builtin_guc_variables(&kernel->execution);
	if (kernel->execution_bound)
		postgamma_shutdown_owned_data_directory();
	if (kernel->execution_bound)
	{
		postgamma_execution_context_restore(
			&kernel->execution, kernel->previous_execution);
		kernel->execution_bound = false;
	}
	if (kernel->execution_initialized)
	{
		postgamma_execution_context_destroy(&kernel->execution);
		kernel->execution_initialized = false;
	}
	if (kernel->role_initialized)
	{
		postgamma_role_context_destroy(&kernel->role);
		kernel->role_initialized = false;
	}
	if (kernel->path_runtime_attached)
	{
		postgamma_instance_context_detach_path_runtime(
			&kernel->instance, kernel->path_runtime);
		kernel->path_runtime_attached = false;
	}
	if (kernel->instance_runtime_attached)
	{
		postgamma_instance_context_detach_runtime(
			&kernel->instance, kernel->instance_runtime);
		kernel->instance_runtime_attached = false;
	}
	if (kernel->instance_initialized)
	{
		postgamma_instance_context_destroy(&kernel->instance);
		kernel->instance_initialized = false;
	}
	if (kernel->path_runtime != NULL)
	{
		operation_status = postgamma_path_runtime_destroy(kernel->path_runtime);
		if (operation_status != 0 && status == 0)
			status = operation_status;
		kernel->path_runtime = NULL;
	}
	if (kernel->instance_runtime != NULL)
	{
		operation_status =
			postgamma_instance_runtime_destroy(kernel->instance_runtime);
		if (operation_status != 0 && status == 0)
			status = operation_status;
		kernel->instance_runtime = NULL;
	}
	if (kernel->data_directory_fd >= 0)
	{
		if (close(kernel->data_directory_fd) != 0 && status == 0)
			status = errno != 0 ? errno : EIO;
		kernel->data_directory_fd = -1;
	}
	free(kernel->canonical_data_directory);
	kernel->canonical_data_directory = NULL;
	{
		int clear_status = postgamma_tool_context_clear_kernel_context(
			kernel->tool, kernel);

		if (status == 0)
			status = clear_status;
	}
	return status;
}


static pgm_bootstrap_result
postgamma_bootstrap_failure(uint64_t generation, unsigned int checks,
							int exit_code, const char *phase,
							const char *detail, unsigned int resources)
{
	return (pgm_bootstrap_result) {
		.status = PGM_BOOTSTRAP_PROBE_FAIL,
		.generation = generation,
		.checks = checks,
		.resources_remaining = resources,
		.exit_code = exit_code,
		.last_phase = phase,
		.detail = detail,
	};
}


pgm_bootstrap_result
pgm_bootstrap_run(uint64_t generation, const char *data_directory,
					   const char *resource_root,
					   const char *bootstrap_input)
{
	PostgammaInstanceRuntimeOptions runtime_options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaPathRuntimeOptions path_options =
		POSTGAMMA_PATH_RUNTIME_OPTIONS_INIT;
	PostgammaToolOptions options = {
		.generation = generation,
		.kind = POSTGAMMA_TOOL_BOOTSTRAP,
		.data_directory = data_directory,
		.resource_root = resource_root,
		.bootstrap_input = bootstrap_input,
	};
	PostgammaToolContext *tool = NULL;
	PostgammaToolContext *previous_tool = NULL;
	PostgammaBootstrapKernelContext kernel;
	PostgammaToolTelemetry telemetry = {
		.phase = POSTGAMMA_TOOL_PHASE_FAILED,
	};
	unsigned int checks = 0;
	unsigned int resources = 0;
	const char *phase = "create";
	const char *detail = "bootstrap setup failed";
	bool		cleanup_registered = false;
	int			argument_count = 0;
	int			status;
	char	   *arguments[13];
	char		executable_path[MAXPGPATH];
	char		wal_segment_size[32];

	memset(&kernel, 0, sizeof(kernel));
	kernel.data_directory_fd = -1;
	if (PostgammaBootstrapWalSegmentSize == 0 ||
		PostgammaBootstrapWalSegmentSize > INT_MAX ||
		(PostgammaBootstrapLogicalUmask & ~0777) != 0 ||
		snprintf(wal_segment_size, sizeof(wal_segment_size), "%" PRIu64,
			PostgammaBootstrapWalSegmentSize) < 0)
		return postgamma_bootstrap_failure(
			generation, checks, EINVAL, phase, detail, resources);
	status = postgamma_tool_context_create(&options, &tool);
	checks++;
	if (status != 0)
		return postgamma_bootstrap_failure(
			generation, checks, status, phase, detail, resources);
	kernel.tool = tool;
	status = postgamma_tool_context_set_kernel_context(tool, &kernel);
	if (status == 0)
		status = postgamma_tool_context_bind(tool, &previous_tool);
	if (status == 0)
	{
		status = postgamma_tool_context_register_cleanup(
			tool, postgamma_bootstrap_kernel_cleanup, &kernel);
		cleanup_registered = status == 0;
	}
	if (status == 0)
	{
		kernel.canonical_data_directory = realpath(data_directory, NULL);
		if (kernel.canonical_data_directory == NULL)
			status = errno != 0 ? errno : ENOENT;
	}
	if (status == 0)
	{
		kernel.data_directory_fd = open(
			kernel.canonical_data_directory,
			O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (kernel.data_directory_fd < 0)
			status = errno != 0 ? errno : EIO;
	}
	if (status == 0 && PostgammaBootstrapMemoryInput != NULL)
	{
		kernel.bootstrap_input_data =
			malloc(PostgammaBootstrapMemoryInputLength + 1);
		if (kernel.bootstrap_input_data == NULL)
			status = ENOMEM;
		else
		{
			memcpy(
				kernel.bootstrap_input_data, PostgammaBootstrapMemoryInput,
				PostgammaBootstrapMemoryInputLength);
			kernel.bootstrap_input_data[PostgammaBootstrapMemoryInputLength] = '\0';
			kernel.bootstrap_input_length = PostgammaBootstrapMemoryInputLength;
		}
	}
	else if (status == 0)
		status = postgamma_read_bootstrap_input(
			bootstrap_input, &kernel.bootstrap_input_data,
			&kernel.bootstrap_input_length);
	if (status == 0)
	{
		runtime_options.generation = generation;
		runtime_options.profile = POSTGAMMA_RUNTIME_PROFILE_EMBEDDED;
		status = postgamma_instance_runtime_create(
			&kernel.instance_runtime, &runtime_options);
	}
	if (status == 0)
	{
		path_options.generation = generation;
		path_options.data_directory_fd = kernel.data_directory_fd;
		path_options.canonical_data_directory =
			kernel.canonical_data_directory;
		path_options.logical_umask = PostgammaBootstrapLogicalUmask;
		path_options.canonical_resource_root = resource_root;
		status = postgamma_path_runtime_create(
			&kernel.path_runtime, &path_options);
	}
	if (status == 0)
		status = postgamma_instance_context_init(&kernel.instance);
	if (status == 0)
	{
		kernel.instance_initialized = true;
		postgamma_instance_context_attach_runtime(
			&kernel.instance, kernel.instance_runtime);
		kernel.instance_runtime_attached = true;
		postgamma_instance_context_attach_path_runtime(
			&kernel.instance, kernel.path_runtime);
		kernel.path_runtime_attached = true;
		status = postgamma_role_context_init(&kernel.role, &kernel.instance);
	}
	if (status == 0)
	{
		kernel.role_initialized = true;
		status = postgamma_execution_context_init(
			&kernel.execution, &kernel.instance, &kernel.role);
	}
	if (status == 0)
	{
		kernel.execution_initialized = true;
		kernel.previous_execution =
			postgamma_execution_context_bind(&kernel.execution);
		kernel.execution_bound = true;
		postgamma_execution_context_set_exit_handler(
			&kernel.execution, postgamma_bootstrap_exit, &kernel);
		kernel.exit_handler_set = true;
		status = postgamma_bootstrap_input_bind(
			kernel.bootstrap_input_data, kernel.bootstrap_input_length);
		if (status == 0)
			kernel.bootstrap_input_bound = true;
	}
	if (status == 0)
	{
		int			path_length;

		status = postgamma_tool_context_begin(tool);
		path_length = snprintf(executable_path, sizeof(executable_path),
						   "%s/bin/postgres", resource_root);
		if (status == 0 &&
			(path_length < 0 || (size_t) path_length >= sizeof(executable_path)))
			status = ENAMETOOLONG;
		if (status == 0)
		{
			arguments[argument_count++] = executable_path;
			arguments[argument_count++] = (char *) "--boot";
			arguments[argument_count++] = (char *) "-F";
			arguments[argument_count++] = (char *) "-c";
			arguments[argument_count++] = (char *) "log_checkpoints=false";
			arguments[argument_count++] = (char *) "-D";
			arguments[argument_count++] = (char *) data_directory;
			arguments[argument_count++] = (char *) "-X";
			arguments[argument_count++] = wal_segment_size;
			if (PostgammaBootstrapDataChecksums)
				arguments[argument_count++] = (char *) "-k";
			arguments[argument_count] = NULL;
		}
	}
	checks++;
	if (status != 0)
	{
		phase = "bind";
		detail = "tool execution context binding failed";
		if (postgamma_tool_context_current() != tool)
		{
			(void) postgamma_bootstrap_kernel_cleanup(&kernel);
			(void) postgamma_tool_context_destroy(tool);
			return postgamma_bootstrap_failure(
				generation, checks, status, phase, detail, resources);
		}
		if (!cleanup_registered)
		{
			(void) postgamma_bootstrap_kernel_cleanup(&kernel);
			(void) postgamma_tool_context_restore(tool, previous_tool);
			(void) postgamma_tool_context_destroy(tool);
			return postgamma_bootstrap_failure(
				generation, checks, status, phase, detail, resources);
		}
		goto finish;
	}

	phase = "postgres-startup";
	if (sigsetjmp(kernel.exit_jump, 1) == 0)
	{
		POSTGAMMA_BACKEND_STATE_GLOBAL(MyProcPid) = getpid();
		strlcpy(POSTGAMMA_BACKEND_STATE_GLOBAL(my_exec_path),
				executable_path,
				MAXPGPATH);
		MemoryContextInit();
		kernel.memory_contexts_initialized = true;
		(void) set_stack_base();
		postgamma_initialize_builtin_guc_values();
		checks++;
		phase = "bootstrap";
		BootstrapModeMain(argument_count, arguments, false);
		kernel.exit_code = EPROTO;
		status = postgamma_tool_context_record_exit(tool, EPROTO);
		if (status != 0)
			kernel.exit_handler_status = status;
	}
	checks++;
	if (kernel.exit_handler_status != 0)
	{
		status = kernel.exit_handler_status;
		detail = "PostgreSQL exit did not match the active tool generation";
	}
	else if (kernel.exit_code != 0)
	{
		status = kernel.exit_code;
		detail = "PostgreSQL bootstrap exited with failure";
	}
	else
	{
		status = 0;
		detail = "PostgreSQL bootstrap completed through the tool exit boundary";
	}

finish:
	phase = "cleanup";
	if (postgamma_tool_context_current() == tool)
	{
		int			cleanup_status = postgamma_tool_context_unwind(tool);

		if (cleanup_status != 0 && status == 0)
		{
			status = cleanup_status;
			detail = "PostgreSQL bootstrap cleanup failed";
		}
	}
	if (postgamma_tool_context_current() == tool)
	{
		int			restore_status =
			postgamma_tool_context_restore(tool, previous_tool);

		if (restore_status != 0 && status == 0)
		{
			status = restore_status;
			detail = "tool TLS restoration failed";
		}
	}
	if (postgamma_tool_context_telemetry(tool, &telemetry) == 0)
	{
		phase = postgamma_tool_phase_name(telemetry.phase);
		resources = (unsigned int) telemetry.cleanup_remaining +
			telemetry.active_bindings + telemetry.active_kernel_contexts;
	}
	else
	{
		status = status == 0 ? EPROTO : status;
		detail = "tool telemetry was unavailable";
	}
	if (postgamma_execution_context_current() != NULL)
		resources++;
	if (postgamma_tool_context_current() != previous_tool)
		resources++;
	checks++;
	if (postgamma_tool_context_destroy(tool) != 0)
	{
		resources++;
		status = status == 0 ? EBUSY : status;
		detail = "tool context remained live after cleanup";
	}
	if (status != 0 || resources != 0 ||
		telemetry.phase != POSTGAMMA_TOOL_PHASE_COMPLETE)
		return postgamma_bootstrap_failure(
			generation, checks, status, phase, detail, resources);
	return (pgm_bootstrap_result) {
		.status = PGM_BOOTSTRAP_PROBE_PASS,
		.generation = generation,
		.checks = checks,
		.resources_remaining = 0,
		.exit_code = 0,
		.last_phase = phase,
		.detail = detail,
	};
}


int
postgamma_postgres_bootstrap_memory(
	uint64_t generation, const char *data_directory,
	const char *resource_root, uint64_t wal_segment_size_bytes,
	bool data_checksums, mode_t logical_umask,
	const char *input, size_t input_length,
	int *postgres_exit_code)
{
	pgm_bootstrap_result result;
	uint64_t	previous_wal_segment_size;
	bool		previous_data_checksums;
	mode_t		previous_logical_umask;

	if (postgres_exit_code != NULL)
		*postgres_exit_code = 0;
	if (generation == 0 || data_directory == NULL ||
		resource_root == NULL || input == NULL || input_length == 0 ||
		input_length > INT_MAX || wal_segment_size_bytes == 0 ||
		wal_segment_size_bytes > INT_MAX || (logical_umask & ~0777) != 0 ||
		PostgammaBootstrapMemoryInput != NULL)
		return EINVAL;
	previous_wal_segment_size = PostgammaBootstrapWalSegmentSize;
	previous_data_checksums = PostgammaBootstrapDataChecksums;
	previous_logical_umask = PostgammaBootstrapLogicalUmask;
	PostgammaBootstrapMemoryInput = input;
	PostgammaBootstrapMemoryInputLength = input_length;
	PostgammaBootstrapWalSegmentSize = wal_segment_size_bytes;
	PostgammaBootstrapDataChecksums = data_checksums;
	PostgammaBootstrapLogicalUmask = logical_umask;
	result = pgm_bootstrap_run(
		generation, data_directory, resource_root, "<memory-bootstrap-input>");
	PostgammaBootstrapMemoryInput = NULL;
	PostgammaBootstrapMemoryInputLength = 0;
	PostgammaBootstrapWalSegmentSize = previous_wal_segment_size;
	PostgammaBootstrapDataChecksums = previous_data_checksums;
	PostgammaBootstrapLogicalUmask = previous_logical_umask;
	if (postgres_exit_code != NULL)
		*postgres_exit_code = result.exit_code;
	if (result.status == PGM_BOOTSTRAP_PROBE_PASS && result.exit_code == 0 &&
		result.resources_remaining == 0)
		return 0;
	return result.exit_code != 0 ? EPROTO : EIO;
}


int
postgamma_postgres_single_user_memory(
	uint64_t generation, const char *data_directory,
	const char *executable_path, const char *resource_root,
	const char *username, mode_t logical_umask,
	const char *input, size_t input_length,
	int *postgres_exit_code)
{
	PostgammaInstanceRuntimeOptions runtime_options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaPathRuntimeOptions path_options =
		POSTGAMMA_PATH_RUNTIME_OPTIONS_INIT;
	PostgammaToolOptions tool_options = {
		.generation = generation,
		.kind = POSTGAMMA_TOOL_SINGLE_USER,
		.data_directory = data_directory,
		.resource_root = resource_root,
		.bootstrap_input = "<memory-single-user-input>",
	};
	PostgammaBootstrapKernelContext kernel;
	PostgammaToolContext *tool = NULL;
	PostgammaToolContext *previous_tool = NULL;
	PostgammaToolTelemetry telemetry;
	bool		cleanup_registered = false;
	int			status;
	char	   *arguments[] = {
		(char *) executable_path,
		(char *) "--single",
		(char *) "-F",
		(char *) "-O",
		(char *) "-j",
		(char *) "-c", (char *) "search_path=pg_catalog",
		(char *) "-c", (char *) "exit_on_error=true",
		(char *) "-c", (char *) "log_checkpoints=false",
		(char *) "-D", (char *) data_directory,
		(char *) "template1",
		NULL,
	};

	if (postgres_exit_code != NULL)
		*postgres_exit_code = 0;
	if (generation == 0 || data_directory == NULL ||
		executable_path == NULL || resource_root == NULL || username == NULL ||
		input == NULL || input_length == 0 || input_length > INT_MAX ||
		(logical_umask & ~0777) != 0)
		return EINVAL;
	memset(&kernel, 0, sizeof(kernel));
	kernel.data_directory_fd = -1;
	status = postgamma_tool_context_create(&tool_options, &tool);
	if (status == 0)
	{
		kernel.tool = tool;
		status = postgamma_tool_context_set_kernel_context(tool, &kernel);
	}
	if (status == 0)
		status = postgamma_tool_context_bind(tool, &previous_tool);
	if (status == 0)
	{
		status = postgamma_tool_context_register_cleanup(
			tool, postgamma_bootstrap_kernel_cleanup, &kernel);
		cleanup_registered = status == 0;
	}
	if (status == 0)
	{
		kernel.canonical_data_directory = realpath(data_directory, NULL);
		if (kernel.canonical_data_directory == NULL)
			status = errno != 0 ? errno : ENOENT;
	}
	if (status == 0)
	{
		kernel.data_directory_fd = open(
			kernel.canonical_data_directory,
			O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (kernel.data_directory_fd < 0)
			status = errno != 0 ? errno : EIO;
	}
	if (status == 0)
	{
		kernel.bootstrap_input_data = malloc(input_length + 1);
		if (kernel.bootstrap_input_data == NULL)
			status = ENOMEM;
		else
		{
			memcpy(kernel.bootstrap_input_data, input, input_length);
			kernel.bootstrap_input_data[input_length] = '\0';
			kernel.bootstrap_input_length = input_length;
		}
	}
	if (status == 0)
	{
		runtime_options.generation = generation;
		runtime_options.profile = POSTGAMMA_RUNTIME_PROFILE_EMBEDDED;
		status = postgamma_instance_runtime_create(
			&kernel.instance_runtime, &runtime_options);
	}
	if (status == 0)
	{
		path_options.generation = generation;
		path_options.data_directory_fd = kernel.data_directory_fd;
		path_options.canonical_data_directory =
			kernel.canonical_data_directory;
		path_options.logical_umask = logical_umask;
		path_options.canonical_resource_root = resource_root;
		status = postgamma_path_runtime_create(
			&kernel.path_runtime, &path_options);
	}
	if (status == 0)
		status = postgamma_instance_context_init(&kernel.instance);
	if (status == 0)
	{
		kernel.instance_initialized = true;
		postgamma_instance_context_attach_runtime(
			&kernel.instance, kernel.instance_runtime);
		kernel.instance_runtime_attached = true;
		postgamma_instance_context_attach_path_runtime(
			&kernel.instance, kernel.path_runtime);
		kernel.path_runtime_attached = true;
		status = postgamma_role_context_init(&kernel.role, &kernel.instance);
	}
	if (status == 0)
	{
		kernel.role_initialized = true;
		status = postgamma_execution_context_init(
			&kernel.execution, &kernel.instance, &kernel.role);
	}
	if (status == 0)
	{
		kernel.execution_initialized = true;
		kernel.previous_execution =
			postgamma_execution_context_bind(&kernel.execution);
		kernel.execution_bound = true;
		postgamma_execution_context_set_exit_handler(
			&kernel.execution, postgamma_bootstrap_exit, &kernel);
		kernel.exit_handler_set = true;
		status = postgamma_standalone_input_bind(
			kernel.bootstrap_input_data, kernel.bootstrap_input_length);
		if (status == 0)
			kernel.standalone_input_bound = true;
	}
	if (status == 0)
		status = postgamma_tool_context_begin(tool);
	if (status == 0 && sigsetjmp(kernel.exit_jump, 1) == 0)
	{
		POSTGAMMA_BACKEND_STATE_GLOBAL(MyProcPid) = getpid();
		strlcpy(
			POSTGAMMA_BACKEND_STATE_GLOBAL(my_exec_path),
			executable_path, MAXPGPATH);
		MemoryContextInit();
		kernel.memory_contexts_initialized = true;
		(void) set_stack_base();
		postgamma_initialize_builtin_guc_values();
		POSTGAMMA_BACKEND_STATE_GLOBAL(whereToSendOutput) = DestNone;
		PostgresSingleUserMain(
			(int) (lengthof(arguments) - 1), arguments, username);
		kernel.exit_code = EPROTO;
		status = postgamma_tool_context_record_exit(tool, EPROTO);
		if (status != 0)
			kernel.exit_handler_status = status;
	}
	if (status == 0 && kernel.exit_handler_status != 0)
		status = kernel.exit_handler_status;
	if (status == 0 && kernel.exit_code != 0)
		status = EPROTO;
	if (postgres_exit_code != NULL)
		*postgres_exit_code = kernel.exit_code;
	if (tool != NULL && postgamma_tool_context_current() == tool)
	{
		int cleanup_status = postgamma_tool_context_unwind(tool);

		if (status == 0 && cleanup_status != 0)
			status = cleanup_status;
	}
	else if (cleanup_registered)
		status = status != 0 ? status : EPROTO;
	else if (tool != NULL)
	{
		int cleanup_status = postgamma_bootstrap_kernel_cleanup(&kernel);

		if (status == 0 && cleanup_status != 0)
			status = cleanup_status;
	}
	if (tool != NULL && postgamma_tool_context_current() == tool)
	{
		int restore_status =
			postgamma_tool_context_restore(tool, previous_tool);

		if (status == 0 && restore_status != 0)
			status = restore_status;
	}
	if (tool != NULL &&
		postgamma_tool_context_telemetry(tool, &telemetry) == 0 &&
		(telemetry.cleanup_remaining != 0 ||
		 telemetry.active_bindings != 0 ||
		 telemetry.active_kernel_contexts != 0))
		status = status != 0 ? status : EBUSY;
	if (tool != NULL)
	{
		int destroy_status = postgamma_tool_context_destroy(tool);

		if (status == 0 && destroy_status != 0)
			status = destroy_status;
	}
	return status;
}
