/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_embedded_kernel.c
 *    PG19-local top-level entry for an embedded postmaster instance.
 *
 *-------------------------------------------------------------------------
 */

#define POSTGAMMA_POSTGRES_RUNTIME_IMPLEMENTATION
#include "postgres.h"

#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "postgamma/embedded_kernel.h"
#include "postgamma/extension_runtime.h"
#include "postgamma/instance_runtime.h"
#include "postgamma/path_runtime.h"
#include "postgamma/postmaster_control_runtime.h"
#include "postgamma/postgres_backend_runtime.h"
#include "utils/memutils.h"
#include "utils/elog.h"


typedef struct PostgammaEmbeddedKernelContext
{
	PostgammaInstanceRuntime *instance_runtime;
	PostgammaPathRuntime *path_runtime;
	PostgammaPostmasterControlRuntime *control_runtime;
	PostgammaInstanceContext instance;
	PostgammaRoleContext role;
	PostgammaExecutionContext execution;
	PostgammaExecutionContext *previous_execution;
	sigjmp_buf	exit_jump;
	int			exit_code;
	bool		instance_initialized;
	bool		instance_runtime_attached;
	bool		path_runtime_attached;
	bool		role_initialized;
	bool		execution_initialized;
	bool		execution_bound;
	bool		exit_handler_set;
	bool		control_bound;
	bool		memory_contexts_initialized;
} PostgammaEmbeddedKernelContext;


const char *progname = "postgamma";

static _Atomic uint64_t PostgammaKernelInstancesEntered;
static _Atomic uint64_t PostgammaKernelInstancesClosed;
static _Atomic uint64_t PostgammaKernelInstancesFailed;
static _Atomic uint64_t PostgammaKernelCleanupFailures;
static _Atomic uint64_t PostgammaKernelActiveInstances;
static _Atomic uint64_t PostgammaKernelActiveMemoryContexts;


static bool boot_options_are_valid(const PostgammaKernelBootOptions *options);
static bool host_provider_is_valid(const PostgammaKernelHostProvider *host);
static bool fault_provider_is_valid(
	const PostgammaKernelFaultProvider *faults);
static int check_fault(
	const PostgammaKernelBootOptions *options,
	PostgammaKernelFaultPoint point,
	PostgammaKernelFaultPoint *injected_point);
static int build_postmaster_arguments(
	const PostgammaKernelBootOptions *options, int *argument_count,
	char ***arguments);
static void free_postmaster_arguments(int argument_count, char **arguments);
static int append_setting_argument(
	char **arguments, size_t *argument_index,
	const PostgammaKernelSetting *setting);
static void embedded_postmaster_exit(void *argument, int code);
static int cleanup_kernel_context(PostgammaEmbeddedKernelContext *kernel);
static void record_kernel_finish(int status, int cleanup_status);
static void set_result(
	PostgammaKernelResult *result, uint64_t generation, int status,
	int postgres_exit_code, int cleanup_status,
	PostgammaKernelFaultPoint fault_point,
	const char *phase, const char *diagnostic);
static int embedded_instance_main(
	const PostgammaKernelBootOptions *options,
	PostgammaKernelResult *result);
static int embedded_instance_telemetry(
	void *runtime_handle, PostgammaKernelInstanceTelemetry *telemetry);


DispatchOption
parse_dispatch_option(const char *name)
{
	(void) name;
	return DISPATCH_POSTMASTER;
}


static const PostgammaKernelEntrypoints PostgammaEmbeddedEntrypoints =
{
	.struct_size = sizeof(PostgammaKernelEntrypoints),
	.abi_version = POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION,
	.capabilities = POSTGAMMA_KERNEL_CAP_SERVER_LIFECYCLE |
		POSTGAMMA_KERNEL_CAP_MEMORY_PROTOCOL,
	.instance_main = embedded_instance_main,
	.instance_telemetry = embedded_instance_telemetry,
};


const PostgammaKernelEntrypoints *
postgamma_embedded_kernel_entrypoints(void)
{
	return &PostgammaEmbeddedEntrypoints;
}


int
postgamma_embedded_kernel_global_telemetry(
	PostgammaKernelGlobalTelemetry *telemetry)
{
	if (telemetry == NULL || telemetry->struct_size != sizeof(*telemetry))
		return EINVAL;
	telemetry->instances_entered = atomic_load_explicit(
		&PostgammaKernelInstancesEntered, memory_order_relaxed);
	telemetry->instances_closed = atomic_load_explicit(
		&PostgammaKernelInstancesClosed, memory_order_relaxed);
	telemetry->instances_failed = atomic_load_explicit(
		&PostgammaKernelInstancesFailed, memory_order_relaxed);
	telemetry->cleanup_failures = atomic_load_explicit(
		&PostgammaKernelCleanupFailures, memory_order_relaxed);
	telemetry->active_instances = atomic_load_explicit(
		&PostgammaKernelActiveInstances, memory_order_relaxed);
	telemetry->active_memory_contexts = atomic_load_explicit(
		&PostgammaKernelActiveMemoryContexts, memory_order_relaxed);
	return 0;
}


const char *
postgamma_kernel_fault_point_name(PostgammaKernelFaultPoint point)
{
	switch (point)
	{
		case POSTGAMMA_KERNEL_FAULT_NONE:
			return "none";
		case POSTGAMMA_KERNEL_FAULT_AFTER_ARGUMENTS:
			return "after-arguments";
		case POSTGAMMA_KERNEL_FAULT_AFTER_INSTANCE_RUNTIME:
			return "after-instance-runtime";
		case POSTGAMMA_KERNEL_FAULT_AFTER_PATH_RUNTIME:
			return "after-path-runtime";
		case POSTGAMMA_KERNEL_FAULT_AFTER_INSTANCE_CONTEXT:
			return "after-instance-context";
		case POSTGAMMA_KERNEL_FAULT_AFTER_ROLE_CONTEXT:
			return "after-role-context";
		case POSTGAMMA_KERNEL_FAULT_AFTER_EXECUTION_CONTEXT:
			return "after-execution-context";
		case POSTGAMMA_KERNEL_FAULT_AFTER_CONTROL_RUNTIME:
			return "after-control-runtime";
		case POSTGAMMA_KERNEL_FAULT_AFTER_MEMORY_CONTEXT:
			return "after-memory-context";
		case POSTGAMMA_KERNEL_FAULT_AFTER_GUC_INITIALIZATION:
			return "after-guc-initialization";
		case POSTGAMMA_KERNEL_FAULT_BEFORE_POSTMASTER:
			return "before-postmaster";
		case POSTGAMMA_KERNEL_FAULT_POINT_COUNT:
			break;
	}
	return "invalid";
}


static int
embedded_instance_main(
	const PostgammaKernelBootOptions *options,
	PostgammaKernelResult *result)
{
	PostgammaInstanceRuntimeOptions runtime_options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaPathRuntimeOptions path_options =
		POSTGAMMA_PATH_RUNTIME_OPTIONS_INIT;
	PostgammaEmbeddedKernelContext kernel;
	char	  **arguments = NULL;
	int			argument_count = 0;
	int			status;
	int			cleanup_status;
	PostgammaKernelFaultPoint injected_point = POSTGAMMA_KERNEL_FAULT_NONE;
	char		diagnostic[256];

	if (result == NULL || result->struct_size != sizeof(*result))
		return EINVAL;
	set_result(result, options == NULL ? 0 : options->generation,
		EINVAL, 0, 0, POSTGAMMA_KERNEL_FAULT_NONE,
		"validate", "invalid embedded kernel boot options");
	if (!boot_options_are_valid(options))
		return EINVAL;
	memset(&kernel, 0, sizeof(kernel));
	atomic_fetch_add_explicit(
		&PostgammaKernelInstancesEntered, UINT64_C(1), memory_order_relaxed);
	atomic_fetch_add_explicit(
		&PostgammaKernelActiveInstances, UINT64_C(1), memory_order_relaxed);

	status = build_postmaster_arguments(
		options, &argument_count, &arguments);
	if (status != 0)
		goto fail;
	status = check_fault(
		options, POSTGAMMA_KERNEL_FAULT_AFTER_ARGUMENTS, &injected_point);
	if (status != 0)
		goto fail;
	runtime_options.generation = options->generation;
	runtime_options.profile = POSTGAMMA_RUNTIME_PROFILE_EMBEDDED;
	if (options->executor_kind == POSTGAMMA_KERNEL_EXECUTOR_POOLED)
	{
		runtime_options.backend_provider.kind =
			POSTGAMMA_BACKEND_PROVIDER_POOLED;
		runtime_options.backend_provider.worker_count =
			options->executor_worker_count;
		runtime_options.backend_provider.queue_capacity =
			options->execution_queue_capacity;
		runtime_options.backend_provider.execution_token_count =
			options->executor_worker_count;
	}
	else
	{
		runtime_options.backend_provider.kind =
			POSTGAMMA_BACKEND_PROVIDER_DEDICATED;
	}
	runtime_options.wake_notification = options->host->control_notify;
	runtime_options.wake_notification_argument = options->host->context;
	runtime_options.emit_log = options->host->emit_log;
	runtime_options.log_argument = options->host->log_context;
	status = postgamma_instance_runtime_create(
		&kernel.instance_runtime, &runtime_options);
	if (status != 0)
		goto fail;
	result->runtime_handle = kernel.instance_runtime;
	status = check_fault(
		options, POSTGAMMA_KERNEL_FAULT_AFTER_INSTANCE_RUNTIME,
		&injected_point);
	if (status != 0)
		goto fail;

	path_options.generation = options->generation;
	path_options.data_directory_fd = options->data_directory_fd;
	path_options.canonical_data_directory = options->data_directory;
	path_options.logical_umask = (mode_t) options->logical_umask;
	path_options.canonical_resource_root = options->resource_root;
	status = postgamma_path_runtime_create(
		&kernel.path_runtime, &path_options);
	if (status != 0)
		goto fail;
	status = check_fault(
		options, POSTGAMMA_KERNEL_FAULT_AFTER_PATH_RUNTIME, &injected_point);
	if (status != 0)
		goto fail;

	status = postgamma_instance_context_init(&kernel.instance);
	if (status != 0)
		goto fail;
	kernel.instance_initialized = true;
	postgamma_instance_context_attach_runtime(
		&kernel.instance, kernel.instance_runtime);
	kernel.instance_runtime_attached = true;
	postgamma_instance_context_attach_path_runtime(
		&kernel.instance, kernel.path_runtime);
	kernel.path_runtime_attached = true;
	status = check_fault(
		options, POSTGAMMA_KERNEL_FAULT_AFTER_INSTANCE_CONTEXT,
		&injected_point);
	if (status != 0)
		goto fail;
	status = postgamma_role_context_init(&kernel.role, &kernel.instance);
	if (status != 0)
		goto fail;
	kernel.role_initialized = true;
	status = check_fault(
		options, POSTGAMMA_KERNEL_FAULT_AFTER_ROLE_CONTEXT, &injected_point);
	if (status != 0)
		goto fail;
	status = postgamma_execution_context_init(
		&kernel.execution, &kernel.instance, &kernel.role);
	if (status != 0)
		goto fail;
	kernel.execution_initialized = true;
	kernel.previous_execution =
		postgamma_execution_context_bind(&kernel.execution);
	kernel.execution_bound = true;
	postgamma_execution_context_set_exit_handler(
		&kernel.execution, embedded_postmaster_exit, &kernel);
	kernel.exit_handler_set = true;
	status = check_fault(
		options, POSTGAMMA_KERNEL_FAULT_AFTER_EXECUTION_CONTEXT,
		&injected_point);
	if (status != 0)
		goto fail;

	status = postgamma_postmaster_control_runtime_create(
		&kernel.control_runtime, kernel.instance_runtime, options->host);
	if (status == 0)
		status = postgamma_postmaster_control_runtime_bind(
			kernel.control_runtime);
	if (status != 0)
		goto fail;
	kernel.control_bound = true;
	status = check_fault(
		options, POSTGAMMA_KERNEL_FAULT_AFTER_CONTROL_RUNTIME,
		&injected_point);
	if (status != 0)
		goto fail;

	if (sigsetjmp(kernel.exit_jump, 1) == 0)
	{
		POSTGAMMA_BACKEND_STATE_GLOBAL(MyProcPid) = getpid();
		POSTGAMMA_BACKEND_STATE_GLOBAL(MyBackendType) = B_INVALID;
		POSTGAMMA_BACKEND_STATE_GLOBAL(IsUnderPostmaster) = false;
		POSTGAMMA_BACKEND_STATE_GLOBAL(IsPostmasterEnvironment) = true;
		POSTGAMMA_BACKEND_STATE_GLOBAL(progname) = "postgamma";
		MemoryContextInit();
		kernel.memory_contexts_initialized = true;
		POSTGAMMA_BACKEND_STATE_GLOBAL(emit_log_hook) =
			postgamma_embedded_emit_log;
		atomic_fetch_add_explicit(
			&PostgammaKernelActiveMemoryContexts, UINT64_C(1),
			memory_order_relaxed);
		status = check_fault(
			options, POSTGAMMA_KERNEL_FAULT_AFTER_MEMORY_CONTEXT,
			&injected_point);
		if (status != 0)
			goto fail;
		(void) set_stack_base();
		postgamma_initialize_builtin_guc_values();
		status = check_fault(
			options, POSTGAMMA_KERNEL_FAULT_AFTER_GUC_INITIALIZATION,
			&injected_point);
		if (status != 0)
			goto fail;
		status = check_fault(
			options, POSTGAMMA_KERNEL_FAULT_BEFORE_POSTMASTER,
			&injected_point);
		if (status != 0)
			goto fail;
		PostmasterMain(argument_count, arguments);
		kernel.exit_code = EPROTO;
	}

	status = kernel.exit_code == 0 ? 0 : EIO;
	cleanup_status = cleanup_kernel_context(&kernel);
	if (status == 0 && cleanup_status != 0)
		status = cleanup_status;
	free_postmaster_arguments(argument_count, arguments);
	if (status == 0)
	{
		set_result(result, options->generation, 0, kernel.exit_code,
			cleanup_status, POSTGAMMA_KERNEL_FAULT_NONE,
			"closed", "embedded PostgreSQL instance stopped cleanly");
		record_kernel_finish(0, cleanup_status);
		return 0;
	}
	set_result(result, options->generation, status, kernel.exit_code,
		cleanup_status, POSTGAMMA_KERNEL_FAULT_NONE,
		"cleanup", "embedded PostgreSQL instance failed or leaked runtime state");
	record_kernel_finish(status, cleanup_status);
	return status;

fail:
	(void) options->host->fail(
		options->host->context, options->generation,
		status != 0 ? status : EIO);
	cleanup_status = cleanup_kernel_context(&kernel);
	if (status == 0)
		status = cleanup_status != 0 ? cleanup_status : EIO;
	free_postmaster_arguments(argument_count, arguments);
	if (injected_point != POSTGAMMA_KERNEL_FAULT_NONE)
	{
		(void) snprintf(
			diagnostic, sizeof(diagnostic),
			"fault injected at %s",
			postgamma_kernel_fault_point_name(injected_point));
		set_result(result, options->generation, status, kernel.exit_code,
			cleanup_status, injected_point, "fault-injection", diagnostic);
	}
	else
		set_result(result, options->generation, status, kernel.exit_code,
			cleanup_status, POSTGAMMA_KERNEL_FAULT_NONE,
			"startup", "embedded PostgreSQL instance setup failed");
	record_kernel_finish(status, cleanup_status);
	return status;
}


static bool
boot_options_are_valid(const PostgammaKernelBootOptions *options)
{
	if (options == NULL || options->struct_size != sizeof(*options) ||
		options->abi_version != POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION ||
		options->generation == 0 || options->data_directory_fd < 0 ||
		options->logical_umask > 0777 || options->data_directory == NULL ||
		options->data_directory[0] != '/' || options->executable_path == NULL ||
		options->executable_path[0] != '/' || options->resource_root == NULL ||
		options->resource_root[0] != '/' ||
		options->executor_kind > POSTGAMMA_KERNEL_EXECUTOR_DEDICATED ||
		(options->executor_kind == POSTGAMMA_KERNEL_EXECUTOR_POOLED &&
		 options->executor_worker_count < 2) ||
		(options->executor_kind == POSTGAMMA_KERNEL_EXECUTOR_DEDICATED &&
		 (options->executor_worker_count != 0 ||
		  options->execution_queue_capacity != 0)) ||
		(options->setting_count != 0 && options->settings == NULL) ||
		!host_provider_is_valid(options->host) ||
		!fault_provider_is_valid(options->faults))
		return false;
	for (size_t index = 0; index < options->setting_count; index++)
	{
		const PostgammaKernelSetting *setting = &options->settings[index];

		if (setting->name == NULL || setting->name[0] == '\0' ||
			strchr(setting->name, '=') != NULL || setting->value == NULL)
			return false;
	}
	return true;
}


static bool
fault_provider_is_valid(const PostgammaKernelFaultProvider *faults)
{
	return faults == NULL ||
		(faults->struct_size == sizeof(*faults) &&
		 faults->abi_version == POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION &&
		 faults->context != NULL && faults->check != NULL);
}


static int
check_fault(
	const PostgammaKernelBootOptions *options,
	PostgammaKernelFaultPoint point,
	PostgammaKernelFaultPoint *injected_point)
{
	int			status;

	if (options->faults == NULL)
		return 0;
	status = options->faults->check(
		options->faults->context, options->generation, point);
	if (status < 0)
		status = EPROTO;
	if (status != 0)
		*injected_point = point;
	return status;
}


static bool
host_provider_is_valid(const PostgammaKernelHostProvider *host)
{
	uint64_t	required =
		POSTGAMMA_KERNEL_HOST_CAP_CONTROL_WAKE_FD |
		POSTGAMMA_KERNEL_HOST_CAP_ASYNC_NOTIFICATION |
		POSTGAMMA_KERNEL_HOST_CAP_FAIL_STOP;

	return host != NULL && host->struct_size == sizeof(*host) &&
		host->abi_version == POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION &&
		(host->capabilities & required) == required && host->context != NULL &&
		host->mark_recovering != NULL && host->mark_ready != NULL &&
		host->control_wake_fd != NULL && host->control_wake_drain != NULL &&
		host->control_notify != NULL && host->control_take != NULL &&
		host->control_complete != NULL && host->fail != NULL &&
		(((host->capabilities & POSTGAMMA_KERNEL_HOST_CAP_LOG_EVENTS) != 0 &&
		  host->log_context != NULL && host->emit_log != NULL) ||
		 ((host->capabilities & POSTGAMMA_KERNEL_HOST_CAP_LOG_EVENTS) == 0 &&
		  host->log_context == NULL && host->emit_log == NULL));
}


static int
embedded_instance_telemetry(
	void *runtime_handle, PostgammaKernelInstanceTelemetry *telemetry)
{
	PostgammaInstanceRuntimeTelemetry runtime_telemetry;
	int			status;

	if (runtime_handle == NULL || telemetry == NULL ||
		telemetry->struct_size != sizeof(*telemetry))
		return EINVAL;
	memset(&runtime_telemetry, 0, sizeof(runtime_telemetry));
	status = postgamma_instance_runtime_telemetry(
		runtime_handle, &runtime_telemetry);
	if (status != 0)
		return status;
	*telemetry = (PostgammaKernelInstanceTelemetry)
		POSTGAMMA_KERNEL_INSTANCE_TELEMETRY_INIT;
	telemetry->executor_worker_count =
		(uint32_t) runtime_telemetry.backend.pooled_worker_threads;
	telemetry->running_sessions =
		runtime_telemetry.backend.running_quantums;
	telemetry->pinned_sessions =
		runtime_telemetry.backend.pinned_sessions;
	telemetry->runnable_sessions =
		runtime_telemetry.backend.runnable_sessions;
	telemetry->queued_requests =
		runtime_telemetry.backend.runnable_sessions;
	telemetry->parallel_tokens_in_use =
		runtime_telemetry.backend.execution_tokens_active;
	telemetry->execution_token_rejections =
		runtime_telemetry.backend.execution_token_rejections;
	telemetry->queue_wait_ns_max =
		runtime_telemetry.backend.queue_wait_ns_max;
	return 0;
}


static int
build_postmaster_arguments(
	const PostgammaKernelBootOptions *options, int *argument_count,
	char ***arguments)
{
	size_t		count;
	size_t		index = 0;
	char	  **built;
	int			status = 0;

	if (options->setting_count > (SIZE_MAX - 4) / 2)
		return EOVERFLOW;
	count = 3 + options->setting_count * 2;
	if (count > INT_MAX)
		return EOVERFLOW;
	built = calloc(count + 1, sizeof(*built));
	if (built == NULL)
		return ENOMEM;
	built[index++] = strdup(options->executable_path);
	built[index++] = strdup("-D");
	built[index++] = strdup(options->data_directory);
	if (built[0] == NULL || built[1] == NULL || built[2] == NULL)
		status = ENOMEM;
	for (size_t setting_index = 0;
		 status == 0 && setting_index < options->setting_count;
		 setting_index++)
		status = append_setting_argument(
			built, &index, &options->settings[setting_index]);
	if (status != 0)
	{
		free_postmaster_arguments((int) (count + 1), built);
		return status;
	}
	built[index] = NULL;
	*argument_count = (int) index;
	*arguments = built;
	return 0;
}


static int
append_setting_argument(
	char **arguments, size_t *argument_index,
	const PostgammaKernelSetting *setting)
{
	size_t		name_length = strlen(setting->name);
	size_t		value_length = strlen(setting->value);
	size_t		length;
	char	   *assignment;

	if (name_length > SIZE_MAX - value_length - 2)
		return EOVERFLOW;
	length = name_length + value_length + 2;
	assignment = malloc(length);
	if (assignment == NULL)
		return ENOMEM;
	(void) snprintf(
		assignment, length, "%s=%s", setting->name, setting->value);
	arguments[(*argument_index)++] = strdup("-c");
	arguments[(*argument_index)++] = assignment;
	if (arguments[*argument_index - 2] == NULL)
		return ENOMEM;
	return 0;
}


static void
free_postmaster_arguments(int argument_count, char **arguments)
{
	if (arguments == NULL)
		return;
	for (int index = 0; index < argument_count; index++)
		free(arguments[index]);
	free(arguments);
}


static void
embedded_postmaster_exit(void *argument, int code)
{
	PostgammaEmbeddedKernelContext *kernel = argument;

	kernel->exit_code = code;
	siglongjmp(kernel->exit_jump, 1);
}


static int
cleanup_kernel_context(PostgammaEmbeddedKernelContext *kernel)
{
	int			first_error = 0;
	int			status;

	if (kernel->exit_handler_set)
	{
		postgamma_execution_context_set_exit_handler(
			&kernel->execution, NULL, NULL);
		kernel->exit_handler_set = false;
	}
	if (kernel->execution_bound)
	{
		status = postgamma_extension_instance_shutdown(&kernel->execution);
		if (status != 0 && first_error == 0)
			first_error = status;
		status = postgamma_extension_execution_destroy(&kernel->execution);
		if (status != 0 && first_error == 0)
			first_error = status;
	}
	if (kernel->memory_contexts_initialized)
	{
		postgamma_shutdown_postmaster_support();
		postgamma_shutdown_latch_wait_set();
		postgamma_shutdown_wait_event_support();
		postgamma_shutdown_xlog_file_access();
		postgamma_shutdown_file_access(false);
		postgamma_destroy_postgres_memory_contexts(true);
		kernel->memory_contexts_initialized = false;
		atomic_fetch_sub_explicit(
			&PostgammaKernelActiveMemoryContexts, UINT64_C(1),
			memory_order_relaxed);
	}
	if (kernel->execution_initialized &&
		kernel->execution.postgres_guc_control != NULL)
		postgamma_destroy_builtin_guc_variables(&kernel->execution);
	if (kernel->execution_bound)
		postgamma_shutdown_owned_data_directory();
	if (kernel->control_bound)
	{
		status = postgamma_postmaster_control_runtime_unbind(
			kernel->control_runtime);
		if (status != 0 && first_error == 0)
			first_error = status;
		kernel->control_bound = false;
	}
	if (kernel->control_runtime != NULL)
	{
		status = postgamma_postmaster_control_runtime_destroy(
			kernel->control_runtime);
		if (status != 0 && first_error == 0)
			first_error = status;
		kernel->control_runtime = NULL;
	}
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
		status = postgamma_path_runtime_destroy(kernel->path_runtime);
		if (status != 0 && first_error == 0)
			first_error = status;
		kernel->path_runtime = NULL;
	}
	if (kernel->instance_runtime != NULL)
	{
		status = postgamma_instance_runtime_destroy(
			kernel->instance_runtime);
		if (status != 0 && first_error == 0)
			first_error = status;
		kernel->instance_runtime = NULL;
	}
	return first_error;
}


static void
record_kernel_finish(int status, int cleanup_status)
{
	if (status == 0)
		atomic_fetch_add_explicit(
			&PostgammaKernelInstancesClosed, UINT64_C(1), memory_order_relaxed);
	else
		atomic_fetch_add_explicit(
			&PostgammaKernelInstancesFailed, UINT64_C(1), memory_order_relaxed);
	if (cleanup_status != 0)
		atomic_fetch_add_explicit(
			&PostgammaKernelCleanupFailures, UINT64_C(1), memory_order_relaxed);
	atomic_fetch_sub_explicit(
		&PostgammaKernelActiveInstances, UINT64_C(1), memory_order_relaxed);
}


static void
set_result(
	PostgammaKernelResult *result, uint64_t generation, int status,
	int postgres_exit_code, int cleanup_status,
	PostgammaKernelFaultPoint fault_point,
	const char *phase, const char *diagnostic)
{
	memset(result, 0, sizeof(*result));
	result->struct_size = sizeof(*result);
	result->generation = generation;
	result->status = status;
	result->postgres_exit_code = postgres_exit_code;
	result->cleanup_status = cleanup_status;
	result->runtime_handle = NULL;
	result->fault_point = fault_point;
	(void) snprintf(result->phase, sizeof(result->phase), "%s", phase);
	(void) snprintf(
		result->diagnostic, sizeof(result->diagnostic), "%s", diagnostic);
}
