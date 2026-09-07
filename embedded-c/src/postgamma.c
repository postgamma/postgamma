/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include "postgamma/embedded_kernel.h"
#include "postgamma/private/data_directory_lock.h"
#include "postgamma/private/kernel_supervisor_adapter.h"
#include "postgamma/private/initdb_host.h"
#include "postgamma/private/libpq_memory_adapter.h"
#include "postgamma/private/memory_transport.h"
#include "postgamma/private/postgres_static_modules.h"
#include "postgamma/private/public_runtime.h"
#include "postgamma/private/server_transport_adapter.h"
#include "postgamma/private/supervisor.h"
#include "postgamma/thread_runtime.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>


#ifndef POSTGAMMA_PRODUCT_VERSION
#error "POSTGAMMA_PRODUCT_VERSION must be supplied by the product build"
#endif
#ifndef POSTGAMMA_POSTGRESQL_MAJOR
#error "POSTGAMMA_POSTGRESQL_MAJOR must be supplied by the product build"
#endif

#define PGM_OPEN_TIMEOUT_NS UINT64_C(60000000000)
#define PGM_CONNECT_TIMEOUT_NS INT64_C(30000000000)
#define PGM_CANCEL_PROPAGATION_TIMEOUT_NS UINT64_C(1000000000)
#define PGM_CANCEL_REDISPATCH_INTERVAL_NS UINT64_C(10000000)
#define PGM_DEFAULT_CONTROL_QUEUE_CAPACITY 64U
#define PGM_DEFAULT_TRANSPORT_QUEUE_CAPACITY (64U * 1024U)
#define PGM_DEFAULT_RESULT_BUFFER_LIMIT (16U * 1024U * 1024U)
#define PGM_DEFAULT_MAXIMUM_VALUE_SIZE (8U * 1024U * 1024U)
#define PGM_DEFAULT_EVENT_QUEUE_CAPACITY 64U


_Static_assert(
	PGM_SHUTDOWN_SMART == POSTGAMMA_SUPERVISOR_SHUTDOWN_SMART,
	"public and supervisor smart shutdown modes must match");
_Static_assert(
	PGM_SHUTDOWN_FAST == POSTGAMMA_SUPERVISOR_SHUTDOWN_FAST,
	"public and supervisor fast shutdown modes must match");
_Static_assert(
	PGM_SHUTDOWN_IMMEDIATE == POSTGAMMA_SUPERVISOR_SHUTDOWN_IMMEDIATE,
	"public and supervisor immediate shutdown modes must match");
_Static_assert(
	PGM_EXTENSION_CAP_THREAD_SAFE == PGMEX_CAP_THREAD_SAFE &&
	PGM_EXTENSION_CAP_MULTI_INSTANCE_SAFE == PGMEX_CAP_MULTI_INSTANCE_SAFE &&
	PGM_EXTENSION_CAP_SESSION_MOBILITY_SAFE ==
		PGMEX_CAP_SESSION_MOBILITY_SAFE &&
	PGM_EXTENSION_CAP_PARALLEL_WORKER_SAFE ==
		PGMEX_CAP_PARALLEL_WORKER_SAFE &&
	PGM_EXTENSION_CAP_INSTANCE_SHMEM == PGMEX_CAP_INSTANCE_SHMEM &&
	PGM_EXTENSION_CAP_BACKGROUND_WORKER == PGMEX_CAP_BACKGROUND_WORKER &&
	PGM_EXTENSION_CAP_FILESYSTEM_READ == PGMEX_CAP_FILESYSTEM_READ &&
	PGM_EXTENSION_CAP_FILESYSTEM_WRITE == PGMEX_CAP_FILESYSTEM_WRITE &&
	PGM_EXTENSION_CAP_HOST_LIBRARY_DEPENDENCY ==
		PGMEX_CAP_HOST_LIBRARY_DEPENDENCY &&
	PGM_EXTENSION_CAP_PROCESS_GLOBAL_STATE ==
		PGMEX_CAP_PROCESS_GLOBAL_STATE,
	"public and extension SDK capability values must match");


static _Atomic uint64_t PostgammaPublicGeneration = UINT64_C(1);


static char *duplicate_string(const char *source);
static char *absolute_requested_path(const char *source);
static void copy_string(char *target, size_t capacity, const char *source);
static pgm_status status_from_errno(int status);
static int deadline_after(uint64_t interval_ns, uint64_t *deadline_ns);
static int run_kernel(PostgammaSupervisor *supervisor, void *argument);
static int wait_and_destroy_ticket(
	PostgammaSupervisorTicket **ticket, uint64_t deadline_ns,
	int *operation_status);
static int cancel_private_request(
	void *argument, uint64_t connection_generation,
	uint64_t request_generation, int backend_pid);
static void forward_notice(
	void *argument, const char *sqlstate, const char *severity,
	const char *message, const char *detail, const char *hint);
static void forward_notification(
	void *argument, int backend_pid,
	const char *channel, const char *payload);
static void notify_frontend_waitable(void *argument, uint32_t events);
static int wait_for_backend_release(
	pgm_connection *connection, int64_t timeout_ms);
static int copy_instance_settings(
	const pgm_setting *settings, size_t setting_count,
	PostgammaKernelSetting **copied_settings);
static void free_instance_settings(
	PostgammaKernelSetting *settings, size_t setting_count);
static char *build_startup_options(
	const pgm_setting *settings, size_t setting_count);
static bool settings_are_valid(
	const pgm_setting *settings, size_t setting_count);
static int copy_request_parameters(
	const pgm_parameter *parameters, size_t parameter_count,
	PostgammaPrivateParameter **copied_parameters);
static void free_request_parameters(
	PostgammaPrivateParameter *parameters, size_t parameter_count);
void result_lease_release(PostgammaPublicResultLease *lease);
static bool request_is_active(pgm_request *request);
static bool progress_unclaimed_copy(
	pgm_request *request, bool *handled, bool *made_progress);
static bool retire_active_request(pgm_request *request);
static void abort_active_request(pgm_request *request);
static pgm_result *wrap_result(PostgammaPrivateOwnedResult *private_result);
static pgm_result_status public_result_status(
	PostgammaPrivateResultStatus status);
static bool result_instance_reentrant(const pgm_result *result);


uint32_t
pgm_abi_version(void)
{
	return PGM_ABI_VERSION;
}


uint64_t
pgm_capabilities(void)
{
	/* Advertise only implementation-backed, conformance-gated capabilities. */
	return PGM_CAP_PREPARED_STATEMENTS | PGM_CAP_CHUNKED_RESULTS |
		PGM_CAP_COPY_IN | PGM_CAP_COPY_OUT | PGM_CAP_ARROW_C_DATA |
		PGM_CAP_NOTIFICATIONS |
		PGM_CAP_REQUEST_NOTICES | PGM_CAP_STATUS_TELEMETRY |
		PGM_CAP_INSTANCE_EVENTS | PGM_CAP_MANAGEMENT_OPERATIONS |
		PGM_CAP_LOGICAL_BACKUP | PGM_CAP_LOGICAL_RESTORE |
		PGM_CAP_MAINTENANCE | PGM_CAP_MULTIPLE_INSTANCES |
		PGM_CAP_BUNDLED_EXTENSIONS;
}


const char *
pgm_postgresql_version(void)
{
	return "19";
}


const char *
pgm_build_id(void)
{
	return "postgamma-" POSTGAMMA_PRODUCT_VERSION "-pg"
		POSTGAMMA_POSTGRESQL_MAJOR;
}


size_t
pgm_bundled_extension_count(void)
{
	size_t		count = 0;
	size_t		index;

	for (index = 0; index < postgamma_product_static_module_count(); index++)
	{
		PostgammaProductStaticModuleInfo private_info;

		if (postgamma_product_static_module_info(index, &private_info) &&
			private_info.sdk_contract)
			count++;
	}
	return count;
}


pgm_status
pgm_bundled_extension_get(
	size_t index, pgm_bundled_extension_info *info)
{
	size_t		module_index;
	size_t		visible_index = 0;

	if (info == NULL || info->struct_size < sizeof(*info) ||
		info->reserved != 0)
		return PGM_STATUS_INVALID_ARGUMENT;
	for (module_index = 0;
		 module_index < postgamma_product_static_module_count(); module_index++)
	{
		PostgammaProductStaticModuleInfo private_info;

		if (!postgamma_product_static_module_info(
				module_index, &private_info) || !private_info.sdk_contract)
			continue;
		if (visible_index++ != index)
			continue;
		info->postgresql_major = private_info.postgresql_major;
		info->sdk_abi_version = private_info.sdk_abi_version;
		info->capabilities = private_info.capabilities;
		info->id = private_info.id;
		info->sql_name = private_info.logical_name;
		info->version = private_info.version;
		return PGM_STATUS_OK;
	}
	return PGM_STATUS_INVALID_ARGUMENT;
}


const char *
pgm_status_name(pgm_status status)
{
	switch (status)
	{
		case PGM_STATUS_OK:
			return "ok";
		case PGM_STATUS_INVALID_ARGUMENT:
			return "invalid-argument";
		case PGM_STATUS_BUSY:
			return "busy";
		case PGM_STATUS_TIMEOUT:
			return "timeout";
		case PGM_STATUS_CANCELED:
			return "canceled";
		case PGM_STATUS_IO_ERROR:
			return "io-error";
		case PGM_STATUS_POSTGRES_ERROR:
			return "postgres-error";
		case PGM_STATUS_CONNECTION_FAILED:
			return "connection-failed";
		case PGM_STATUS_INSTANCE_FAILED:
			return "instance-failed";
		case PGM_STATUS_FORKED_PROCESS:
			return "forked-process";
		case PGM_STATUS_VERSION_MISMATCH:
			return "version-mismatch";
		case PGM_STATUS_UNSUPPORTED:
			return "unsupported";
		case PGM_STATUS_OUT_OF_MEMORY:
			return "out-of-memory";
		case PGM_STATUS_INTERNAL_ERROR:
			return "internal-error";
		case PGM_STATUS_REENTRANT_CALL:
			return "reentrant-call";
	}
	return "invalid-status";
}


pgm_status
pgm_instance_open(
	const pgm_instance_options *options,
	pgm_instance **instance,
	pgm_error **error)
{
	PostgammaDataDirectoryLockOptions lock_options =
		POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT;
	PostgammaDataDirectoryLockTelemetry lock_telemetry;
	PostgammaSupervisorOptions supervisor_options =
		POSTGAMMA_SUPERVISOR_OPTIONS_INIT;
	PostgammaInitdbResult initdb_result = POSTGAMMA_INITDB_RESULT_INIT;
	pgm_instance *created = NULL;
	char		failure_detail[256] = "";
	uint64_t	deadline_ns;
	int			status;
	bool		require_missing = options != NULL &&
		options->reserved == POSTGAMMA_PRIVATE_INSTANCE_OPEN_REQUIRE_MISSING;

	if (error != NULL)
		*error = NULL;
	if (instance == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"instance output parameter is required");
	*instance = NULL;
	if (options == NULL || options->struct_size < sizeof(*options) ||
		options->path == NULL || options->path[0] == '\0' ||
		options->executable_path == NULL || options->executable_path[0] == '\0' ||
		options->resource_root == NULL || options->resource_root[0] == '\0' ||
		!settings_are_valid(options->settings, options->setting_count) ||
		options->create > UINT32_C(1) ||
		options->executor_worker_count < UINT32_C(2) ||
		options->logical_umask > UINT32_C(0777) ||
		(options->reserved != 0 &&
		 options->reserved !=
		 POSTGAMMA_PRIVATE_INSTANCE_OPEN_REQUIRE_MISSING) ||
		(require_missing && options->create == 0) ||
		(options->log_callback == NULL && options->log_user_data != NULL))
	{
		return_error(error, make_error(
			PGM_STATUS_INVALID_ARGUMENT, NULL, NULL, NULL,
			"invalid instance options"));
		return PGM_STATUS_INVALID_ARGUMENT;
	}
	if (options->create != 0 && options->logical_umask != UINT32_C(0077))
		return return_simple_error(
			error, PGM_STATUS_UNSUPPORTED,
			"cluster creation currently requires logical_umask 0077");
	status = deadline_after(PGM_OPEN_TIMEOUT_NS, &deadline_ns);
	if (status != 0)
		return return_simple_error(
			error, status_from_errno(status),
			"could not establish the instance open deadline");
	status = postgamma_static_module_provider_install(
		postgamma_product_static_module_provider());
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not install the embedded static module provider");
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		goto no_memory;
	created->magic = PGM_INSTANCE_MAGIC;
	created->owner_pid = getpid();
	created->generation = atomic_fetch_add_explicit(
		&PostgammaPublicGeneration, UINT64_C(1), memory_order_relaxed);
	if (created->generation == 0 || created->generation == UINT64_MAX)
	{
		status = EOVERFLOW;
		goto fail;
	}
	atomic_init(&created->next_request_generation, UINT64_C(1));
	atomic_init(&created->next_connection_id, UINT64_C(1));
	atomic_init(&created->next_statement_id, UINT64_C(1));
	atomic_init(&created->active_request_count, UINT64_C(0));
	atomic_init(&created->request_count, UINT64_C(0));
	atomic_init(&created->completed_request_count, UINT64_C(0));
	atomic_init(&created->canceled_request_count, UINT64_C(0));
	atomic_init(&created->failed_request_count, UINT64_C(0));
	created->executor_worker_count = options->executor_worker_count;
	created->transport_queue_capacity =
		options->transport_queue_capacity != 0 ?
		options->transport_queue_capacity : PGM_DEFAULT_TRANSPORT_QUEUE_CAPACITY;
	created->result_buffer_limit = options->result_buffer_limit != 0 ?
		options->result_buffer_limit : PGM_DEFAULT_RESULT_BUFFER_LIMIT;
	created->maximum_value_size = options->maximum_value_size != 0 ?
		options->maximum_value_size :
		(created->result_buffer_limit < PGM_DEFAULT_MAXIMUM_VALUE_SIZE ?
		 created->result_buffer_limit : PGM_DEFAULT_MAXIMUM_VALUE_SIZE);
	if (created->maximum_value_size > created->result_buffer_limit)
	{
		status = EINVAL;
		copy_string(
			failure_detail, sizeof(failure_detail),
			"maximum_value_size exceeds result_buffer_limit");
		goto fail;
	}
	created->data_directory = absolute_requested_path(options->path);
	created->executable_path = realpath(options->executable_path, NULL);
	created->resource_root = realpath(options->resource_root, NULL);
	if (created->data_directory == NULL || created->executable_path == NULL ||
		created->resource_root == NULL)
	{
		status = errno != 0 ? errno : ENOENT;
		goto fail;
	}
	status = copy_instance_settings(
		options->settings, options->setting_count, &created->settings);
	if (status != 0)
		goto fail;
	created->setting_count = options->setting_count;
	if (options->create != 0)
	{
		PostgammaInitdbOptions initdb_options =
			POSTGAMMA_INITDB_OPTIONS_INIT;
		PostgammaInitdbSetting *initdb_settings = NULL;
		size_t		index;

		if (created->setting_count != 0)
		{
			if (created->setting_count >
				SIZE_MAX / sizeof(*initdb_settings))
			{
				status = EOVERFLOW;
				goto fail;
			}
			initdb_settings = calloc(
				created->setting_count, sizeof(*initdb_settings));
			if (initdb_settings == NULL)
				goto no_memory;
			for (index = 0; index < created->setting_count; index++)
			{
				initdb_settings[index].name = created->settings[index].name;
				initdb_settings[index].value = created->settings[index].value;
			}
		}
		initdb_options.generation = created->generation;
		initdb_options.deadline_ns = deadline_ns;
		initdb_options.data_directory = created->data_directory;
		initdb_options.executable_path = created->executable_path;
		initdb_options.resource_root = created->resource_root;
		initdb_options.username = "postgamma";
		initdb_options.settings = initdb_settings;
		initdb_options.setting_count = created->setting_count;
		initdb_options.logical_umask = (mode_t) options->logical_umask;
		initdb_options.require_missing = require_missing;
		status = postgamma_initdb_create_locked(
			&initdb_options, &initdb_result, &created->data_lock);
		free(initdb_settings);
		if (status != 0)
		{
			copy_string(
				failure_detail, sizeof(failure_detail), initdb_result.message);
			goto fail;
		}
	}
	status = postgamma_mutex_create(&created->mutex);
	if (status != 0)
		goto fail;
	status = postgamma_public_event_router_create(
		created,
		options->event_queue_capacity != 0 ?
		options->event_queue_capacity : PGM_DEFAULT_EVENT_QUEUE_CAPACITY,
		options->log_callback, options->log_user_data);
	if (status != 0)
		goto fail;
	lock_options.generation = created->generation;
	lock_options.path = created->data_directory;
	status = created->data_lock != NULL ? 0 :
		postgamma_data_directory_lock_acquire(
			&lock_options, &created->data_lock);
	if (status != 0)
		goto fail;
	memset(&lock_telemetry, 0, sizeof(lock_telemetry));
	status = postgamma_data_directory_lock_telemetry(
		created->data_lock, &lock_telemetry);
	if (status != 0)
		goto fail;
	free(created->data_directory);
	created->data_directory = duplicate_string(lock_telemetry.canonical_path);
	if (created->data_directory == NULL)
		goto no_memory;
	created->entrypoints = postgamma_embedded_kernel_entrypoints();
	if (created->entrypoints == NULL ||
		created->entrypoints->struct_size != sizeof(*created->entrypoints) ||
		created->entrypoints->abi_version !=
			POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION ||
		created->entrypoints->instance_main == NULL ||
		created->entrypoints->instance_telemetry == NULL ||
		(created->entrypoints->capabilities &
		 POSTGAMMA_KERNEL_CAP_SERVER_LIFECYCLE) == 0 ||
		(created->entrypoints->capabilities &
		 POSTGAMMA_KERNEL_CAP_MEMORY_PROTOCOL) == 0)
	{
		status = EPROTO;
		goto fail;
	}
	supervisor_options.generation = created->generation;
	supervisor_options.queue_capacity =
		options->control_queue_capacity != 0 ?
		options->control_queue_capacity : PGM_DEFAULT_CONTROL_QUEUE_CAPACITY;
	supervisor_options.callbacks.run = run_kernel;
	supervisor_options.callback_argument = created;
	status = postgamma_supervisor_create(
		&supervisor_options, &created->supervisor);
	if (status != 0)
		goto fail;
	created->host = (PostgammaKernelHostProvider)
		POSTGAMMA_KERNEL_HOST_PROVIDER_INIT;
	status = postgamma_kernel_supervisor_provider_init(
		created->supervisor, &created->host);
	if (status != 0)
		goto fail;
	created->host.capabilities |= POSTGAMMA_KERNEL_HOST_CAP_LOG_EVENTS;
	created->host.log_context = created;
	created->host.emit_log = postgamma_public_event_enqueue_kernel_log;
	created->boot_options = (PostgammaKernelBootOptions)
		POSTGAMMA_KERNEL_BOOT_OPTIONS_INIT;
	created->kernel_result = (PostgammaKernelResult)
		POSTGAMMA_KERNEL_RESULT_INIT;
	created->boot_options.generation = created->generation;
	created->boot_options.data_directory_fd = lock_telemetry.directory_descriptor;
	created->boot_options.logical_umask = options->logical_umask;
	created->boot_options.data_directory = created->data_directory;
	created->boot_options.executable_path = created->executable_path;
	created->boot_options.resource_root = created->resource_root;
	created->boot_options.settings = created->settings;
	created->boot_options.setting_count = created->setting_count;
	created->boot_options.executor_worker_count =
		options->executor_worker_count;
	created->boot_options.execution_queue_capacity =
		options->execution_queue_capacity;
	created->boot_options.host = &created->host;
	status = postgamma_supervisor_start(created->supervisor, deadline_ns);
	if (status != 0)
		goto fail;
	*instance = created;
	return PGM_STATUS_OK;

no_memory:
	status = ENOMEM;

fail:
	{
		pgm_status public_status = status_from_errno(status);

		if (failure_detail[0] == '\0' && initdb_result.created)
			copy_string(
				failure_detail, sizeof(failure_detail),
				"cluster creation completed, but embedded instance startup failed; "
				"the published cluster remains available for retry");
		if (public_status == PGM_STATUS_IO_ERROR ||
			public_status == PGM_STATUS_INTERNAL_ERROR)
			public_status = PGM_STATUS_INSTANCE_FAILED;
		return_error(error, make_error(
			public_status, NULL, NULL,
			failure_detail[0] != '\0' ? failure_detail :
			created != NULL && created->kernel_result.diagnostic[0] != '\0' ?
			created->kernel_result.diagnostic : NULL,
			"embedded instance open failed: %s", strerror(status)));
		if (created != NULL)
		{
			if (created->supervisor != NULL)
			{
				PostgammaSupervisorState state =
					postgamma_supervisor_state(created->supervisor);

				if (state != POSTGAMMA_SUPERVISOR_STATE_NEW &&
					state != POSTGAMMA_SUPERVISOR_STATE_CLOSED &&
					state != POSTGAMMA_SUPERVISOR_STATE_FAILED)
					(void) postgamma_supervisor_fail(created->supervisor, status);
				(void) postgamma_supervisor_join(
					created->supervisor, POSTGAMMA_SUPERVISOR_NO_DEADLINE);
				(void) postgamma_supervisor_destroy(created->supervisor);
			}
			if (created->data_lock != NULL)
				(void) postgamma_data_directory_lock_release(created->data_lock);
			if (created->event_router != NULL)
				(void) postgamma_public_event_router_destroy(created);
			if (created->mutex != NULL)
				(void) postgamma_mutex_destroy(created->mutex);
			free_instance_settings(created->settings, created->setting_count);
			free(created->resource_root);
			free(created->executable_path);
			free(created->data_directory);
			created->magic = 0;
			free(created);
		}
		return public_status;
	}
}


pgm_status
pgm_instance_close(
	pgm_instance *instance,
	pgm_shutdown_mode mode,
	int64_t timeout_ms,
	pgm_error **error)
{
	PostgammaSupervisorShutdownMode private_mode;
	uint64_t	deadline_ns;
	int			operation_status = 0;
	int			status;
	bool		close_claimed = false;

	if (error != NULL)
		*error = NULL;
	if (!instance_is_valid(instance) || timeout_ms < PGM_NO_TIMEOUT ||
		mode < PGM_SHUTDOWN_SMART || mode > PGM_SHUTDOWN_IMMEDIATE)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid instance close arguments");
	if (!process_is_valid(instance->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"instance belongs to a different host process");
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = deadline_from_timeout(timeout_ms, &deadline_ns);
	if (status != 0)
		return return_simple_error(
			error, status_from_errno(status), "invalid instance close timeout");
	status = postgamma_mutex_lock(instance->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the embedded instance");
	if (instance->closing || instance->connection_count != 0 ||
		instance->operation_count != 0)
	{
		(void) postgamma_mutex_unlock(instance->mutex);
		return_error(error, make_error(
			PGM_STATUS_BUSY, NULL, NULL, NULL,
			"instance still has %zu open connection(s) and %zu operation(s)",
			instance->connection_count, instance->operation_count));
		return PGM_STATUS_BUSY;
	}
	instance->closing = true;
	close_claimed = true;
	(void) postgamma_mutex_unlock(instance->mutex);
	if (!instance->shutdown_requested)
	{
		private_mode = (PostgammaSupervisorShutdownMode) mode;
		status = postgamma_supervisor_request_shutdown(
			instance->supervisor, instance->generation, private_mode,
			&instance->shutdown_ticket);
		if (status != 0)
			goto fail;
		instance->shutdown_requested = true;
	}
	if (instance->shutdown_ticket != NULL)
	{
		status = wait_and_destroy_ticket(
			&instance->shutdown_ticket, deadline_ns, &operation_status);
		if (status != 0)
			goto fail;
		if (operation_status != 0)
		{
			status = operation_status;
			goto fail;
		}
	}
	if (instance->supervisor != NULL)
	{
		status = postgamma_supervisor_join(instance->supervisor, deadline_ns);
		if (status != 0)
			goto fail;
		status = postgamma_supervisor_destroy(instance->supervisor);
		if (status != 0)
			goto fail;
		instance->supervisor = NULL;
	}
	if (instance->data_lock != NULL)
	{
		status = postgamma_data_directory_lock_release(instance->data_lock);
		instance->data_lock = NULL;
		if (status != 0)
			goto fail;
	}
	status = postgamma_public_event_router_destroy(instance);
	if (status != 0)
		goto fail;
	status = postgamma_mutex_destroy(instance->mutex);
	if (status != 0)
		goto fail;
	instance->mutex = NULL;
	free_instance_settings(instance->settings, instance->setting_count);
	free(instance->resource_root);
	free(instance->executable_path);
	free(instance->data_directory);
	instance->magic = 0;
	free(instance);
	return PGM_STATUS_OK;

fail:
	if (close_claimed && instance->mutex != NULL &&
		postgamma_mutex_lock(instance->mutex) == 0)
	{
		instance->closing = false;
		(void) postgamma_mutex_unlock(instance->mutex);
	}
	return_error(error, make_error(
		status_from_errno(status), NULL, NULL, NULL,
		"embedded instance close failed: %s", strerror(status)));
	return status_from_errno(status);
}


pgm_status
pgm_instance_get_telemetry(
	pgm_instance *instance,
	pgm_instance_telemetry *telemetry,
	pgm_error **error)
{
	PostgammaKernelInstanceTelemetry kernel =
		POSTGAMMA_KERNEL_INSTANCE_TELEMETRY_INIT;
	uint64_t	connection_count;
	int			status;

	if (error != NULL)
		*error = NULL;
	if (!instance_is_valid(instance) || telemetry == NULL ||
		telemetry->struct_size < sizeof(*telemetry))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid instance telemetry arguments");
	if (!process_is_valid(instance->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"instance belongs to a different host process");
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	if (instance->kernel_result.runtime_handle == NULL ||
		instance->entrypoints == NULL ||
		instance->entrypoints->instance_telemetry == NULL)
		return return_simple_error(
			error, PGM_STATUS_INSTANCE_FAILED,
			"instance runtime telemetry is unavailable");
	status = instance->entrypoints->instance_telemetry(
		instance->kernel_result.runtime_handle, &kernel);
	if (status != 0)
		return return_simple_error(
			error, status_from_errno(status),
			"could not read scheduler telemetry");
	status = postgamma_mutex_lock(instance->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the embedded instance");
	connection_count = instance->connection_count;
	(void) postgamma_mutex_unlock(instance->mutex);
	*telemetry = (pgm_instance_telemetry) PGM_INSTANCE_TELEMETRY_INIT;
	telemetry->executor_worker_count = instance->executor_worker_count;
	telemetry->connection_count = connection_count;
	telemetry->active_request_count = atomic_load_explicit(
		&instance->active_request_count, memory_order_relaxed);
	telemetry->running_session_count = kernel.running_sessions;
	telemetry->pinned_session_count = kernel.pinned_sessions;
	telemetry->runnable_session_count = kernel.runnable_sessions;
	telemetry->queued_request_count = kernel.queued_requests;
	telemetry->parallel_tokens_in_use = kernel.parallel_tokens_in_use;
	telemetry->request_count = atomic_load_explicit(
		&instance->request_count, memory_order_relaxed);
	telemetry->completed_request_count = atomic_load_explicit(
		&instance->completed_request_count, memory_order_relaxed);
	telemetry->canceled_request_count = atomic_load_explicit(
		&instance->canceled_request_count, memory_order_relaxed);
	telemetry->failed_request_count = atomic_load_explicit(
		&instance->failed_request_count, memory_order_relaxed);
	telemetry->execution_token_rejections =
		kernel.execution_token_rejections;
	telemetry->queue_wait_ns_max = kernel.queue_wait_ns_max;
	status = postgamma_public_event_telemetry(
		instance, &telemetry->event_queue_depth,
		&telemetry->event_queue_capacity,
		&telemetry->dropped_log_count,
		&telemetry->dropped_notice_count,
		&telemetry->dropped_notification_count);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not read instance event telemetry");
	return PGM_STATUS_OK;
}


pgm_status
pgm_connection_open(
	pgm_instance *instance,
	const pgm_connection_options *options,
	pgm_connection **connection,
	pgm_error **error)
{
	PostgammaPrivateLibpqOptions private_options;
	PostgammaKernelConnectRequest connect_request =
		POSTGAMMA_KERNEL_CONNECT_REQUEST_INIT;
	PostgammaSupervisorTicket *ticket = NULL;
	PostgammaMemoryEndpoint *backend_endpoint = NULL;
	pgm_connection *created = NULL;
	char	   *startup_options = NULL;
	int64_t		private_deadline;
	uint64_t	supervisor_deadline;
	int			operation_status = 0;
	int			status = 0;
	PostgammaPrivateLibpqStatus private_status;

	if (error != NULL)
		*error = NULL;
	if (connection == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"connection output parameter is required");
	*connection = NULL;
	if (!instance_is_valid(instance) || options == NULL ||
		options->struct_size < sizeof(*options) || options->reserved != 0 ||
		options->user == NULL || options->user[0] == '\0' ||
		options->database == NULL || options->database[0] == '\0' ||
		!settings_are_valid(options->settings, options->setting_count))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid connection options");
	if (!process_is_valid(instance->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"instance belongs to a different host process");
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = postgamma_mutex_lock(instance->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the embedded instance");
	if (instance->closing ||
		postgamma_supervisor_state(instance->supervisor) !=
			POSTGAMMA_SUPERVISOR_STATE_READY)
	{
		(void) postgamma_mutex_unlock(instance->mutex);
		return return_simple_error(
			error, PGM_STATUS_INSTANCE_FAILED,
			"embedded instance is not ready for connections");
	}
	created = calloc(1, sizeof(*created));
	if (created == NULL)
	{
		status = ENOMEM;
		goto fail_locked;
	}
	created->magic = PGM_CONNECTION_MAGIC;
	created->owner_pid = instance->owner_pid;
	created->instance = instance;
	created->identity = atomic_fetch_add_explicit(
		&instance->next_connection_id, UINT64_C(1), memory_order_relaxed);
	if (created->identity == 0 || created->identity == UINT64_MAX)
	{
		status = EOVERFLOW;
		goto fail_locked;
	}
	created->notice_callback = options->notice_callback;
	created->notification_callback = options->notification_callback;
	created->callback_user_data = options->user_data;
	atomic_init(&created->event_pending_next, NULL);
	atomic_init(&created->event_ready_pending, false);
	status = postgamma_mutex_create(&created->mutex);
	if (status != 0)
		goto fail_locked;
	startup_options = build_startup_options(
		options->settings, options->setting_count);
	if (startup_options == NULL && options->setting_count != 0)
	{
		status = errno != 0 ? errno : ENOMEM;
		goto fail_locked;
	}
	memset(&private_options, 0, sizeof(private_options));
	private_options.generation = instance->generation;
	private_options.queue_capacity = instance->transport_queue_capacity;
	private_options.user = options->user;
	private_options.database = options->database;
	private_options.application_name = options->application_name;
	private_options.startup_options = startup_options;
	private_options.cancel_callback = cancel_private_request;
	private_options.cancel_argument = created;
	private_options.notice_callback = forward_notice;
	private_options.notice_argument = created;
	private_options.notification_callback = forward_notification;
	private_options.notification_argument = created;
	private_status = postgamma_private_libpq_create(
		&private_options, &created->private_connection, &backend_endpoint);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		status = EPROTO;
		goto fail_locked;
	}
	status = postgamma_memory_server_connect_init(
		instance->generation, backend_endpoint, &connect_request);
	connect_request.connection_id = created->identity;
	if (status == 0)
		status = postgamma_supervisor_submit(
			instance->supervisor, instance->generation,
			POSTGAMMA_SUPERVISOR_CONTROL_CONNECT, &connect_request, &ticket);
	if (status == 0)
		status = deadline_after(PGM_OPEN_TIMEOUT_NS, &supervisor_deadline);
	if (status == 0)
		status = wait_and_destroy_ticket(
			&ticket, supervisor_deadline, &operation_status);
	if (status == 0 && operation_status != 0)
		status = operation_status;
	if (status != 0 || connect_request.backend_pid <= 0)
	{
		if (status == 0)
			status = EPROTO;
		goto fail_locked;
	}
	created->backend_pid = connect_request.backend_pid;
	if (postgamma_memory_endpoint_retain(
			backend_endpoint, instance->generation,
			&created->backend_monitor) != POSTGAMMA_MEMORY_STATUS_OK)
	{
		status = EPROTO;
		goto fail_locked;
	}
	if (postgamma_memory_endpoint_release(
			&backend_endpoint, instance->generation) !=
		POSTGAMMA_MEMORY_STATUS_OK)
	{
		status = EPROTO;
		goto fail_locked;
	}
	connect_request.transport = NULL;
	status = private_deadline_after(PGM_CONNECT_TIMEOUT_NS, &private_deadline);
	if (status != 0)
		goto fail_locked;
	private_status = postgamma_private_libpq_connect(
		created->private_connection, private_deadline);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		status = EPROTO;
		goto fail_locked;
	}
	status = postgamma_public_event_register_connection(created);
	if (status != 0)
		goto fail_locked;
	private_status = postgamma_private_libpq_set_notify(
		created->private_connection, notify_frontend_waitable,
		created);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		status = EPROTO;
		goto fail_locked;
	}
	instance->connection_count++;
	created->registered_with_instance = true;
	(void) postgamma_mutex_unlock(instance->mutex);
	free(startup_options);
	*connection = created;
	return PGM_STATUS_OK;

fail_locked:
	(void) postgamma_mutex_unlock(instance->mutex);
	if (ticket != NULL)
		(void) postgamma_supervisor_ticket_destroy(ticket);
	if (backend_endpoint != NULL)
	{
		(void) postgamma_memory_endpoint_abort(
			backend_endpoint, instance->generation,
			POSTGAMMA_MEMORY_ABORT_SHUTDOWN);
		(void) postgamma_memory_endpoint_release(
			&backend_endpoint, instance->generation);
	}
	if (created != NULL)
	{
		if (created->private_connection != NULL)
			(void) postgamma_private_libpq_close(
				&created->private_connection);
		if (created->backend_monitor != NULL)
			(void) postgamma_memory_endpoint_release(
				&created->backend_monitor, instance->generation);
		if (created->event_registered)
		{
			(void) postgamma_public_event_begin_connection_close(created);
			postgamma_public_event_finish_connection_close(created);
		}
		if (created->mutex != NULL)
			(void) postgamma_mutex_destroy(created->mutex);
		created->magic = 0;
		free(created);
	}
	free(startup_options);
	return_error(error, make_error(
		status == ENOMEM ? PGM_STATUS_OUT_OF_MEMORY :
			PGM_STATUS_CONNECTION_FAILED,
		NULL, NULL, NULL, "embedded connection open failed: %s",
		strerror(status != 0 ? status : EIO)));
	return status == ENOMEM ? PGM_STATUS_OUT_OF_MEMORY :
		PGM_STATUS_CONNECTION_FAILED;
}


pgm_status
pgm_connection_close(
	pgm_connection *connection,
	int64_t timeout_ms,
	pgm_error **error)
{
	pgm_instance *instance;
	PostgammaPrivateLibpqStatus private_status;
	int			status;
	bool		notify_disabled = false;

	if (error != NULL)
		*error = NULL;
	if (!connection_is_valid(connection) || timeout_ms < PGM_NO_TIMEOUT)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid connection close arguments");
	if (!process_is_valid(connection->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"connection belongs to a different host process");
	instance = connection->instance;
	if (public_instance_reentrant(instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = postgamma_mutex_lock(connection->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the embedded connection");
	if (connection->closing || connection->active_request != NULL ||
		connection->statement_count != 0)
	{
		(void) postgamma_mutex_unlock(connection->mutex);
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			connection->active_request != NULL ?
			"free the connection request before closing its connection" :
			connection->statement_count != 0 ?
			"close prepared statements before closing their connection" :
			"connection close is already in progress");
	}
	connection->closing = true;
	(void) postgamma_mutex_unlock(connection->mutex);
	private_status = postgamma_private_libpq_set_notify(
		connection->private_connection, NULL, NULL);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		status = EPROTO;
		goto fail;
	}
	notify_disabled = true;
	status = postgamma_public_event_begin_connection_close(connection);
	if (status != 0)
		goto fail;
	if (connection->private_connection != NULL)
	{
		private_status = postgamma_private_libpq_close(
			&connection->private_connection);
		if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		{
			status = EPROTO;
			goto fail;
		}
	}
	status = wait_for_backend_release(connection, timeout_ms);
	if (status != 0)
		goto fail;
	if (connection->backend_monitor != NULL &&
		postgamma_memory_endpoint_release(
			&connection->backend_monitor, instance->generation) !=
		POSTGAMMA_MEMORY_STATUS_OK)
	{
		status = EIO;
		goto fail;
	}
	if (connection->registered_with_instance)
	{
		status = postgamma_mutex_lock(instance->mutex);
		if (status != 0)
			goto fail;
		if (instance->connection_count == 0)
		{
			(void) postgamma_mutex_unlock(instance->mutex);
			status = EPROTO;
			goto fail;
		}
		instance->connection_count--;
		connection->registered_with_instance = false;
		(void) postgamma_mutex_unlock(instance->mutex);
	}
	postgamma_public_event_finish_connection_close(connection);
	status = postgamma_mutex_destroy(connection->mutex);
	if (status != 0)
		goto fail;
	connection->mutex = NULL;
	connection->magic = 0;
	free(connection);
	return PGM_STATUS_OK;

fail:
	postgamma_public_event_cancel_connection_close(connection);
	if (notify_disabled && connection->private_connection != NULL)
		(void) postgamma_private_libpq_set_notify(
			connection->private_connection, notify_frontend_waitable,
			connection);
	if (postgamma_mutex_lock(connection->mutex) == 0)
	{
		connection->closing = false;
		(void) postgamma_mutex_unlock(connection->mutex);
	}
	return_error(error, make_error(
		status_from_errno(status), NULL, NULL, NULL,
		"embedded connection close failed: %s", strerror(status)));
	return status_from_errno(status);
}


pgm_status
request_start(
	pgm_connection *connection,
	PostgammaPublicRequestMode mode,
	pgm_statement *statement,
	const char *sql,
	const pgm_parameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	pgm_delivery_mode delivery_mode,
	uint32_t target_chunk_rows,
	size_t result_buffer_limit,
	pgm_notice_callback notice_callback,
	void *notice_user_data,
	pgm_request **request,
	pgm_error **error)
{
	pgm_request *created;
	PostgammaPrivateLibpqStatus private_status =
		POSTGAMMA_PRIVATE_LIBPQ_OK;
	PostgammaPrivateResultPolicy private_policy =
		POSTGAMMA_PRIVATE_RESULT_POLICY_INIT;
	size_t		resolved_result_buffer_limit;
	int			status = 0;

	if (error != NULL)
		*error = NULL;
	if (request == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"request output parameter is required");
	*request = NULL;
	if (!connection_is_valid(connection) || sql == NULL || sql[0] == '\0' ||
		mode < POSTGAMMA_PUBLIC_REQUEST_EXTENDED ||
		mode > POSTGAMMA_PUBLIC_REQUEST_PREPARED ||
		(mode == POSTGAMMA_PUBLIC_REQUEST_PREPARED &&
		 (!statement_is_valid(statement) || statement->connection != connection)) ||
		(mode != POSTGAMMA_PUBLIC_REQUEST_PREPARED && statement != NULL) ||
		(mode == POSTGAMMA_PUBLIC_REQUEST_SCRIPT && parameter_count != 0) ||
		result_format > 1 || parameter_count > INT32_MAX ||
		(parameter_count != 0 && parameters == NULL) ||
		(delivery_mode != PGM_DELIVERY_MATERIALIZED &&
		 delivery_mode != PGM_DELIVERY_CHUNKED) ||
		(delivery_mode == PGM_DELIVERY_MATERIALIZED &&
		 target_chunk_rows != 0) ||
		(delivery_mode == PGM_DELIVERY_CHUNKED && target_chunk_rows == 0))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid embedded request arguments");
	if (!process_is_valid(connection->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"connection belongs to a different host process");
	if (public_instance_reentrant(connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	resolved_result_buffer_limit = result_buffer_limit != 0 ?
		result_buffer_limit : connection->instance->result_buffer_limit;
	if (resolved_result_buffer_limit == 0 ||
		resolved_result_buffer_limit > connection->instance->result_buffer_limit)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid request result_buffer_limit");
	status = postgamma_mutex_lock(connection->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the embedded connection");
	if (connection->failed)
	{
		(void) postgamma_mutex_unlock(connection->mutex);
		return return_simple_error(
			error, PGM_STATUS_CONNECTION_FAILED,
			"connection was aborted while retiring an incomplete request");
	}
	if (connection->closing || connection->active_request != NULL ||
		(statement != NULL && statement->active_request != NULL))
	{
		(void) postgamma_mutex_unlock(connection->mutex);
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"connection already has an owned request");
	}
	created = calloc(1, sizeof(*created));
	if (created == NULL)
	{
		(void) postgamma_mutex_unlock(connection->mutex);
		return return_simple_error(
			error, PGM_STATUS_OUT_OF_MEMORY,
			"could not allocate embedded request handle");
	}
	created->magic = PGM_REQUEST_MAGIC;
	created->owner_pid = connection->owner_pid;
	created->connection = connection;
	created->statement = statement;
	created->mode = mode;
	created->state = PGM_REQUEST_PENDING;
	created->operation_status = PGM_STATUS_INTERNAL_ERROR;
	created->result_format = result_format;
	created->delivery_mode = delivery_mode;
	created->target_chunk_rows = target_chunk_rows;
	created->result_buffer_limit = resolved_result_buffer_limit;
	created->maximum_value_size =
		connection->instance->maximum_value_size < resolved_result_buffer_limit ?
		connection->instance->maximum_value_size : resolved_result_buffer_limit;
	created->notice_callback = notice_callback;
	created->notice_user_data = notice_user_data;
	created->sql = duplicate_string(sql);
	if (created->sql == NULL)
		status = ENOMEM;
	if (status == 0)
		status = copy_request_parameters(
			parameters, parameter_count, &created->parameters);
	created->parameter_count = parameter_count;
	if (status == 0)
		status = postgamma_mutex_create(&created->mutex);
	if (status == 0)
	{
		created->result_lease = calloc(1, sizeof(*created->result_lease));
		if (created->result_lease == NULL)
			status = ENOMEM;
		else
		{
			atomic_init(&created->result_lease->references, 1U);
			atomic_init(&created->result_lease->outstanding, false);
			created->result_lease->owner = created;
		}
	}
	if (status == 0)
	{
		created->generation = atomic_fetch_add_explicit(
			&connection->instance->next_request_generation, UINT64_C(1),
			memory_order_relaxed);
		if (created->generation == 0 || created->generation == UINT64_MAX)
			status = EOVERFLOW;
	}
	if (status == 0)
	{
		private_policy.delivery_mode = (uint32_t) created->delivery_mode;
		private_policy.target_chunk_rows = created->target_chunk_rows;
		private_policy.result_buffer_limit = created->result_buffer_limit;
		private_policy.maximum_value_size = created->maximum_value_size;
		if (mode == POSTGAMMA_PUBLIC_REQUEST_SCRIPT)
			private_status = postgamma_private_libpq_operation_start_script_ex(
				connection->private_connection, created->generation,
				created->sql, &private_policy, &created->operation);
		else if (mode == POSTGAMMA_PUBLIC_REQUEST_PREPARED)
			private_status = postgamma_private_libpq_operation_start_prepared_ex(
				connection->private_connection, created->generation,
				created->sql, created->parameters, created->parameter_count,
				created->result_format, &private_policy, &created->operation);
		else
			private_status = postgamma_private_libpq_operation_start_ex(
				connection->private_connection, created->generation,
				created->sql, created->parameters, created->parameter_count,
				created->result_format, &private_policy, &created->operation);
		if (private_status == POSTGAMMA_PRIVATE_LIBPQ_OK)
		{
			connection->active_request = created;
			if (statement != NULL)
				statement->active_request = created;
		}
		else
			status = EPROTO;
	}
	(void) postgamma_mutex_unlock(connection->mutex);
	if (status != 0)
	{
		if (created->mutex != NULL)
			(void) postgamma_mutex_destroy(created->mutex);
		free(created->result_lease);
		free_request_parameters(created->parameters, created->parameter_count);
		free(created->sql);
		created->magic = 0;
		free(created);
		return_error(error, make_error(
			private_status != POSTGAMMA_PRIVATE_LIBPQ_OK ?
			status_from_private(private_status) : status_from_errno(status),
			NULL, NULL, NULL, "could not start embedded request: %s",
			private_status != POSTGAMMA_PRIVATE_LIBPQ_OK ?
			postgamma_private_libpq_status_name(private_status) : strerror(status)));
		return private_status != POSTGAMMA_PRIVATE_LIBPQ_OK ?
			status_from_private(private_status) : status_from_errno(status);
	}
	(void) atomic_fetch_add_explicit(
		&connection->instance->request_count, UINT64_C(1),
		memory_order_relaxed);
	(void) atomic_fetch_add_explicit(
		&connection->instance->active_request_count, UINT64_C(1),
		memory_order_relaxed);
	*request = created;
	return PGM_STATUS_OK;
}


pgm_status
pgm_execute_async(
	pgm_connection *connection,
	const char *sql,
	const pgm_parameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	pgm_request **request,
	pgm_error **error)
{
	return request_start(
		connection, POSTGAMMA_PUBLIC_REQUEST_EXTENDED, NULL, sql,
		parameters, parameter_count, result_format,
		PGM_DELIVERY_MATERIALIZED, UINT32_C(0), 0,
		NULL, NULL, request, error);
}


pgm_status
pgm_execute(
	pgm_connection *connection,
	const char *sql,
	const pgm_parameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	int64_t timeout_ms,
	pgm_result **result,
	pgm_error **error)
{
	pgm_request *request = NULL;
	pgm_status	status;

	if (error != NULL)
		*error = NULL;
	if (result == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"result output parameter is required");
	*result = NULL;
	status = pgm_execute_async(
		connection, sql, parameters, parameter_count, result_format,
		&request, error);
	if (status != PGM_STATUS_OK)
		return status;
	status = pgm_request_wait(request, timeout_ms, result, error);
	if (status == PGM_STATUS_TIMEOUT)
		(void) pgm_request_cancel(request, NULL);
	pgm_request_free(request);
	return status;
}


pgm_status
pgm_request_poll(
	pgm_request *request,
	pgm_request_state *state,
	pgm_error **error)
{
	int			status;

	if (error != NULL)
		*error = NULL;
	if (!request_is_valid(request) || state == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid request poll arguments");
	if (!process_is_valid(request->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"request belongs to a different host process");
	if (public_instance_reentrant(request->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = postgamma_mutex_lock(request->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the embedded request");
	*state = request->state;
	(void) postgamma_mutex_unlock(request->mutex);
	return PGM_STATUS_OK;
}


pgm_status
pgm_request_progress(
	pgm_request *request,
	pgm_request_state *state,
	pgm_error **error)
{
	pgm_status	status;
	int			lock_status;

	if (error != NULL)
		*error = NULL;
	if (!request_is_valid(request) || state == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid request progress arguments");
	if (!process_is_valid(request->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"request belongs to a different host process");
	if (public_instance_reentrant(request->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	lock_status = postgamma_mutex_lock(request->mutex);
	if (lock_status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the embedded request");
	status = progress_request_locked(request);
	*state = request->state;
	(void) postgamma_mutex_unlock(request->mutex);
	postgamma_public_dispatch_progress_events(request);
	if (status != PGM_STATUS_OK)
		return return_simple_error(
			error, status, "could not progress the embedded request");
	return PGM_STATUS_OK;
}


pgm_status
pgm_request_waitable(
	pgm_request *request,
	int *file_descriptor,
	pgm_error **error)
{
	int			descriptor;

	if (error != NULL)
		*error = NULL;
	if (!request_is_valid(request) || file_descriptor == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid request waitable arguments");
	if (!process_is_valid(request->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"request belongs to a different host process");
	if (public_instance_reentrant(request->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	descriptor = postgamma_public_event_waitable_fd(
		request->connection->instance);
	if (descriptor < 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"request waitable is unavailable");
	*file_descriptor = descriptor;
	return PGM_STATUS_OK;
}


pgm_status
pgm_request_wait(
	pgm_request *request,
	int64_t timeout_ms,
	pgm_result **result,
	pgm_error **error)
{
	uint64_t	deadline_ns;
	pgm_result *collected = NULL;
	pgm_status	operation_status = PGM_STATUS_INTERNAL_ERROR;
	PostgammaPrivateLibpqStatus private_status;
	int			status;

	if (result != NULL)
		*result = NULL;
	if (error != NULL)
		*error = NULL;
	if (!request_is_valid(request) || timeout_ms < PGM_NO_TIMEOUT)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid request wait arguments");
	if (!process_is_valid(request->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"request belongs to a different host process");
	if (public_instance_reentrant(request->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	if (request->mode == POSTGAMMA_PUBLIC_REQUEST_SCRIPT ||
		request->delivery_mode != PGM_DELIVERY_MATERIALIZED)
		return return_simple_error(
			error, PGM_STATUS_UNSUPPORTED,
			"request wait supports one materialized extended result only");
	if (atomic_load_explicit(
			&request->result_lease->outstanding, memory_order_acquire))
		return return_simple_error(
			error, PGM_STATUS_BUSY,
			"free the outstanding request result before waiting again");
	status = deadline_from_timeout(timeout_ms, &deadline_ns);
	if (status != 0)
		return return_simple_error(
			error, status_from_errno(status), "invalid request wait timeout");
	for (;;)
	{
		pgm_request_state state;
		int64_t		private_deadline;
		pgm_status	progress_status;
		bool		collected_this_iteration = false;

		status = postgamma_mutex_lock(request->mutex);
		if (status != 0)
			goto internal_failure;
		progress_status = progress_request_locked(request);
		if (progress_status != PGM_STATUS_OK)
		{
			(void) postgamma_mutex_unlock(request->mutex);
			postgamma_public_dispatch_progress_events(request);
			status = EPROTO;
			goto internal_failure;
		}
		if (request->result != NULL)
		{
			pgm_result_status kind = pgm_result_kind(request->result);

			if (kind == PGM_RESULT_COPY_IN || kind == PGM_RESULT_COPY_OUT ||
				kind == PGM_RESULT_TUPLES_CHUNK)
			{
				(void) postgamma_mutex_unlock(request->mutex);
				postgamma_public_dispatch_progress_events(request);
				return return_simple_error(
					error, PGM_STATUS_UNSUPPORTED,
					"request wait cannot consume COPY or chunked results");
			}
			if (collected != NULL)
			{
				request->operation_status = PGM_STATUS_INTERNAL_ERROR;
				request->state = PGM_REQUEST_FAILED;
				request_account_terminal(request);
				(void) postgamma_mutex_unlock(request->mutex);
				postgamma_public_dispatch_progress_events(request);
				abort_active_request(request);
				status = EPROTO;
				goto internal_failure;
			}
			collected = request->result;
			request->result = NULL;
			collected_this_iteration = true;
		}
		state = request->state;
		(void) postgamma_mutex_unlock(request->mutex);
		postgamma_public_dispatch_progress_events(request);
		if (state != PGM_REQUEST_PENDING && state != PGM_REQUEST_RUNNING)
			break;
		if (collected_this_iteration)
			continue;
		if (deadline_ns != POSTGAMMA_SUPERVISOR_NO_DEADLINE &&
			postgamma_monotonic_now_ns() >= deadline_ns)
			goto timeout;
		private_deadline = deadline_ns == POSTGAMMA_SUPERVISOR_NO_DEADLINE ?
			POSTGAMMA_MEMORY_NO_DEADLINE : (int64_t) deadline_ns;
		private_status = postgamma_private_libpq_operation_wait(
			request->operation, private_deadline);
		if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		{
			if (private_status == POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT)
				goto timeout;
			status = EPROTO;
			goto internal_failure;
		}
	}
	status = postgamma_mutex_lock(request->mutex);
	if (status != 0)
		goto internal_failure;
	operation_status = request->operation_status;
	if (operation_status == PGM_STATUS_OK && collected != NULL && result != NULL)
	{
		pgm_status lease_status = result_attach_lease(
			collected, request->result_lease);

		if (lease_status != PGM_STATUS_OK)
		{
			request->result = collected;
			collected = NULL;
			(void) postgamma_mutex_unlock(request->mutex);
			postgamma_public_dispatch_progress_events(request);
			return return_simple_error(
				error, lease_status,
				"could not transfer the request result");
		}
		*result = collected;
		collected = NULL;
	}
	else if (operation_status == PGM_STATUS_OK && collected != NULL)
	{
		request->result = collected;
		collected = NULL;
	}
	if (error != NULL && request->error != NULL)
	{
		*error = request->error;
		request->error = NULL;
	}
	(void) postgamma_mutex_unlock(request->mutex);
	postgamma_public_dispatch_progress_events(request);
	if (operation_status != PGM_STATUS_OK)
		pgm_result_free(collected);
	if (operation_status != PGM_STATUS_OK && error != NULL && *error == NULL)
		(void) return_simple_error(
			error, operation_status, "embedded request failed");
	return operation_status;

timeout:
	if (collected != NULL && postgamma_mutex_lock(request->mutex) == 0)
	{
		if (request->result == NULL)
		{
			request->result = collected;
			collected = NULL;
		}
		(void) postgamma_mutex_unlock(request->mutex);
	}
	pgm_result_free(collected);
	postgamma_public_dispatch_progress_events(request);
	return return_simple_error(
		error, PGM_STATUS_TIMEOUT, "embedded request wait timed out");

internal_failure:
	if (collected != NULL && postgamma_mutex_lock(request->mutex) == 0)
	{
		if (request->result == NULL)
		{
			request->result = collected;
			collected = NULL;
		}
		(void) postgamma_mutex_unlock(request->mutex);
	}
	pgm_result_free(collected);
	postgamma_public_dispatch_progress_events(request);
	return return_simple_error(
		error, PGM_STATUS_INTERNAL_ERROR,
		"could not complete the embedded request");
}


pgm_status
pgm_request_cancel(pgm_request *request, pgm_error **error)
{
	uint64_t	deadline_ns;
	int			status;
	bool		copy_active;

	if (error != NULL)
		*error = NULL;
	if (!request_is_valid(request))
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid request cancel arguments");
	if (!process_is_valid(request->owner_pid))
		return return_simple_error(
			error, PGM_STATUS_FORKED_PROCESS,
			"request belongs to a different host process");
	if (public_instance_reentrant(request->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	status = deadline_after(PGM_CANCEL_PROPAGATION_TIMEOUT_NS, &deadline_ns);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not create a cancellation deadline");
	status = postgamma_mutex_lock(request->mutex);
	if (status != 0)
		return return_simple_error(
			error, PGM_STATUS_INTERNAL_ERROR,
			"could not lock the embedded request");
	if (request->state != PGM_REQUEST_PENDING &&
		request->state != PGM_REQUEST_RUNNING)
	{
		pgm_status completed_status = request->operation_status;

		(void) postgamma_mutex_unlock(request->mutex);
		if (completed_status == PGM_STATUS_OK)
			return PGM_STATUS_OK;
		return return_simple_error(
			error, completed_status,
			"embedded request completed before cancellation");
	}
	request->cancel_requested = true;
	copy_active = request->active_copy != NULL;
	(void) postgamma_mutex_unlock(request->mutex);
	if (copy_active)
	{
		PostgammaPrivateLibpqStatus private_status =
			postgamma_private_libpq_cancel(
				request->connection->private_connection, request->generation);

		if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
			return return_simple_error(
				error, status_from_private(private_status),
				"could not cancel an active COPY request");
		return PGM_STATUS_OK;
	}
	for (;;)
	{
		PostgammaPrivateLibpqStatus private_status;
		pgm_request_state state;
		pgm_status	progress_status;
		uint64_t	now_ns;
		uint64_t	redispatch_deadline_ns;
		bool		result_ready = false;

		progress_status = pgm_request_progress(request, &state, error);
		if (progress_status != PGM_STATUS_OK)
			return progress_status;
		if (state != PGM_REQUEST_PENDING && state != PGM_REQUEST_RUNNING)
			return PGM_STATUS_OK;
		if (postgamma_mutex_lock(request->mutex) == 0)
		{
			result_ready = request->result != NULL;
			(void) postgamma_mutex_unlock(request->mutex);
		}
		if (result_ready)
			return PGM_STATUS_OK;
		now_ns = postgamma_monotonic_now_ns();
		if (now_ns >= deadline_ns)
			return return_simple_error(
				error, PGM_STATUS_TIMEOUT,
				"cancellation did not complete before its deadline");
		private_status = postgamma_private_libpq_cancel(
			request->connection->private_connection, request->generation);
		if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		{
			return_error(error, make_error(
				status_from_private(private_status), NULL, NULL, NULL,
				"embedded request cancellation failed: %s",
				postgamma_private_libpq_status_name(private_status)));
			return status_from_private(private_status);
		}
		redispatch_deadline_ns =
			PGM_CANCEL_REDISPATCH_INTERVAL_NS > deadline_ns - now_ns ?
			deadline_ns : now_ns + PGM_CANCEL_REDISPATCH_INTERVAL_NS;
		private_status = postgamma_private_libpq_operation_wait(
			request->operation, (int64_t) redispatch_deadline_ns);
		if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK &&
			private_status != POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT)
		{
			return_error(error, make_error(
				status_from_private(private_status), NULL, NULL, NULL,
				"embedded cancellation wait failed: %s",
				postgamma_private_libpq_status_name(private_status)));
			return status_from_private(private_status);
		}
	}
}


void
pgm_request_free(pgm_request *request)
{
	bool		copy_active = false;

	if (!request_is_valid(request))
		return;
	if (public_instance_reentrant(request->connection->instance))
		return;
	if (postgamma_mutex_lock(request->mutex) == 0)
	{
		copy_active = request->active_copy != NULL;
		if (copy_active)
		{
			request->free_pending = true;
			request->cancel_requested = true;
		}
		(void) postgamma_mutex_unlock(request->mutex);
	}
	if (copy_active)
	{
		(void) postgamma_private_libpq_cancel(
			request->connection->private_connection, request->generation);
		return;
	}
	request_destroy(request);
}


void
request_destroy(pgm_request *request)
{
	pgm_connection *connection;

	if (!request_is_valid(request))
		return;
	if (request_is_active(request))
	{
		if (!request->cancel_requested)
			(void) pgm_request_cancel(request, NULL);
		(void) retire_active_request(request);
		if (request_is_active(request))
		{
			abort_active_request(request);
			if (postgamma_mutex_lock(request->mutex) == 0)
			{
				request->operation_status = PGM_STATUS_CONNECTION_FAILED;
				request->state = PGM_REQUEST_FAILED;
				request_account_terminal(request);
				(void) postgamma_mutex_unlock(request->mutex);
			}
		}
	}
	postgamma_public_event_forget_request(request);
	connection = request->connection;
	if (connection_is_valid(connection) && connection->mutex != NULL &&
		postgamma_mutex_lock(connection->mutex) == 0)
	{
		if (connection->active_request == request)
			connection->active_request = NULL;
		if (statement_is_valid(request->statement) &&
			request->statement->active_request == request)
			request->statement->active_request = NULL;
		(void) postgamma_mutex_unlock(connection->mutex);
	}
	pgm_result_free(request->result);
	pgm_error_free(request->error);
	postgamma_private_libpq_operation_free(&request->operation);
	if (connection_is_valid(connection))
		postgamma_public_event_notify_connection(
			connection, POSTGAMMA_MEMORY_WAIT_READABLE);
	free_request_parameters(request->parameters, request->parameter_count);
	free(request->sql);
	request->result_lease->owner = NULL;
	result_lease_release(request->result_lease);
	request->result_lease = NULL;
	if (request->mutex != NULL)
		(void) postgamma_mutex_destroy(request->mutex);
	request->magic = 0;
	free(request);
}


pgm_result_status
pgm_result_kind(const pgm_result *result)
{
	if (!result_is_valid(result) || result_instance_reentrant(result))
		return PGM_RESULT_ERROR;
	return public_result_status(result->private_result->status);
}


size_t
pgm_result_row_count(const pgm_result *result)
{
	return result_is_valid(result) && !result_instance_reentrant(result) ?
		result->private_result->rows : 0;
}


size_t
pgm_result_column_count(const pgm_result *result)
{
	return result_is_valid(result) && !result_instance_reentrant(result) ?
		result->private_result->columns : 0;
}


const char *
pgm_result_command_status(const pgm_result *result)
{
	return result_is_valid(result) && !result_instance_reentrant(result) ?
		result->private_result->command_status : "";
}


pgm_status
pgm_result_column(
	const pgm_result *result,
	size_t column_index,
	pgm_column *column,
	pgm_error **error)
{
	const PostgammaPrivateResultField *source;

	if (error != NULL)
		*error = NULL;
	if (!result_is_valid(result) || column == NULL ||
		column->struct_size < sizeof(*column) ||
		column_index >= result->private_result->columns)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid result column arguments");
	if (result_instance_reentrant(result))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	source = &result->private_result->fields[column_index];
	*column = (pgm_column) PGM_COLUMN_INIT;
	column->name = source->name;
	column->table_oid = source->table_oid;
	column->table_column = source->table_column;
	column->type_oid = source->type_oid;
	column->type_size = source->type_size;
	column->type_modifier = source->type_modifier;
	column->format = source->format;
	return PGM_STATUS_OK;
}


pgm_status
pgm_result_value(
	const pgm_result *result,
	size_t row_index,
	size_t column_index,
	pgm_value_view *value,
	pgm_error **error)
{
	const PostgammaPrivateResultValue *source;
	const PostgammaPrivateResultField *field;

	if (error != NULL)
		*error = NULL;
	if (!result_is_valid(result) || value == NULL ||
		value->struct_size < sizeof(*value) ||
		row_index >= result->private_result->rows ||
		column_index >= result->private_result->columns)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid result value arguments");
	if (result_instance_reentrant(result))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	source = &result->private_result->values[
		row_index * result->private_result->columns + column_index];
	field = &result->private_result->fields[column_index];
	*value = (pgm_value_view) PGM_VALUE_VIEW_INIT;
	value->type_oid = field->type_oid;
	value->format = field->format;
	value->is_null = source->is_null ? UINT16_C(1) : UINT16_C(0);
	value->data = source->data;
	value->size = source->length;
	return PGM_STATUS_OK;
}


void
pgm_result_free(pgm_result *result)
{
	if (!result_is_valid(result))
		return;
	if (result_instance_reentrant(result))
		return;
	postgamma_private_libpq_result_free(result->private_result);
	result->private_result = NULL;
	if (result->lease != NULL)
	{
		atomic_store_explicit(
			&result->lease->outstanding, false, memory_order_release);
		result_lease_release(result->lease);
		result->lease = NULL;
	}
	result->magic = 0;
	free(result);
}


pgm_status
pgm_error_status(const pgm_error *error)
{
	return error_is_valid(error) ? error->status : PGM_STATUS_INVALID_ARGUMENT;
}


const char *
pgm_error_sqlstate(const pgm_error *error)
{
	return error_is_valid(error) && error->fields[PGM_DIAG_SQLSTATE - 1] != NULL ?
		error->fields[PGM_DIAG_SQLSTATE - 1] : "";
}


const char *
pgm_error_severity(const pgm_error *error)
{
	return error_is_valid(error) && error->fields[PGM_DIAG_SEVERITY - 1] != NULL ?
		error->fields[PGM_DIAG_SEVERITY - 1] : "";
}


const char *
pgm_error_message(const pgm_error *error)
{
	return error_is_valid(error) && error->fields[PGM_DIAG_MESSAGE - 1] != NULL ?
		error->fields[PGM_DIAG_MESSAGE - 1] : "";
}


const char *
pgm_error_detail(const pgm_error *error)
{
	return error_is_valid(error) && error->fields[PGM_DIAG_DETAIL - 1] != NULL ?
		error->fields[PGM_DIAG_DETAIL - 1] : "";
}


const char *
pgm_error_field(const pgm_error *error, pgm_diagnostic_field field)
{
	if (!error_is_valid(error) || field < PGM_DIAG_SQLSTATE ||
		field > PGM_DIAG_SOURCE_FUNCTION || error->fields[field - 1] == NULL)
		return NULL;
	return error->fields[field - 1];
}


void
pgm_error_free(pgm_error *error)
{
	if (!error_is_valid(error))
		return;
	for (size_t field = 0; field < PGM_DIAGNOSTIC_FIELD_COUNT; field++)
		free(error->fields[field]);
	error->magic = 0;
	free(error);
}


bool
instance_is_valid(const pgm_instance *instance)
{
	return instance != NULL && instance->magic == PGM_INSTANCE_MAGIC;
}


bool
connection_is_valid(const pgm_connection *connection)
{
	return connection != NULL && connection->magic == PGM_CONNECTION_MAGIC;
}


bool
statement_is_valid(const pgm_statement *statement)
{
	return statement != NULL && statement->magic == PGM_STATEMENT_MAGIC &&
		connection_is_valid(statement->connection);
}


bool
request_is_valid(const pgm_request *request)
{
	return request != NULL && request->magic == PGM_REQUEST_MAGIC;
}


bool
result_is_valid(const pgm_result *result)
{
	return result != NULL && result->magic == PGM_RESULT_MAGIC &&
		result->private_result != NULL;
}


static bool
result_instance_reentrant(const pgm_result *result)
{
	pgm_request *request;

	if (!result_is_valid(result) || result->lease == NULL)
		return false;
	request = result->lease->owner;
	return request_is_valid(request) &&
		connection_is_valid(request->connection) &&
		public_instance_reentrant(request->connection->instance);
}


bool
copy_is_valid(const pgm_copy *copy)
{
	return copy != NULL && copy->magic == PGM_COPY_MAGIC &&
		request_is_valid(copy->request) && copy->mutex != NULL;
}


bool
error_is_valid(const pgm_error *error)
{
	return error != NULL && error->magic == PGM_ERROR_MAGIC;
}


bool
process_is_valid(pid_t owner_pid)
{
	return owner_pid > 0 && owner_pid == getpid();
}


static char *
duplicate_string(const char *source)
{
	size_t		length;
	char	   *copy;

	if (source == NULL)
		return NULL;
	length = strlen(source);
	copy = malloc(length + 1);
	if (copy != NULL)
		memcpy(copy, source, length + 1);
	return copy;
}


char *
duplicate_sql(const char *sql, size_t sql_size)
{
	char	   *copy;

	if (sql == NULL || sql_size == 0 || sql_size == SIZE_MAX ||
		memchr(sql, '\0', sql_size) != NULL)
	{
		errno = EINVAL;
		return NULL;
	}
	copy = malloc(sql_size + 1);
	if (copy == NULL)
		return NULL;
	memcpy(copy, sql, sql_size);
	copy[sql_size] = '\0';
	return copy;
}


static char *
absolute_requested_path(const char *source)
{
	char	   *working_directory;
	char	   *path;
	size_t		directory_length;
	size_t		source_length;

	if (source == NULL)
		return NULL;
	if (source[0] == '/')
		return duplicate_string(source);
	working_directory = getcwd(NULL, 0);
	if (working_directory == NULL)
		return NULL;
	directory_length = strlen(working_directory);
	source_length = strlen(source);
	if (directory_length > SIZE_MAX - source_length - 2)
	{
		free(working_directory);
		errno = EOVERFLOW;
		return NULL;
	}
	path = malloc(directory_length + source_length + 2);
	if (path != NULL)
		(void) snprintf(
			path, directory_length + source_length + 2,
			"%s/%s", working_directory, source);
	free(working_directory);
	return path;
}


static void
copy_string(char *target, size_t capacity, const char *source)
{
	if (capacity == 0)
		return;
	if (source == NULL)
		source = "";
	(void) snprintf(target, capacity, "%s", source);
}


pgm_error *
make_error(
	pgm_status status, const char *sqlstate, const char *severity,
	const char *detail, const char *format, ...)
{
	va_list		arguments;
	va_list		copy;
	const char *fields[PGM_DIAGNOSTIC_FIELD_COUNT] = {0};
	pgm_error  *error;
	char	   *message;
	int			length;

	va_start(arguments, format);
	va_copy(copy, arguments);
	length = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (length < 0)
	{
		va_end(arguments);
		return NULL;
	}
	message = malloc((size_t) length + 1);
	if (message == NULL)
	{
		va_end(arguments);
		return NULL;
	}
	(void) vsnprintf(message, (size_t) length + 1, format, arguments);
	va_end(arguments);
	fields[PGM_DIAG_SQLSTATE - 1] = sqlstate;
	fields[PGM_DIAG_SEVERITY - 1] = severity;
	fields[PGM_DIAG_MESSAGE - 1] = message;
	fields[PGM_DIAG_DETAIL - 1] = detail;
	error = make_error_from_diagnostics(
		status, fields, PGM_DIAGNOSTIC_FIELD_COUNT);
	free(message);
	return error;
}


pgm_error *
make_error_from_diagnostics(
	pgm_status status, const char *const *fields, size_t field_count)
{
	pgm_error  *error;

	if (fields == NULL && field_count != 0)
		return NULL;
	error = calloc(1, sizeof(*error));
	if (error == NULL)
		return NULL;
	error->magic = PGM_ERROR_MAGIC;
	error->status = status;
	if (field_count > PGM_DIAGNOSTIC_FIELD_COUNT)
		field_count = PGM_DIAGNOSTIC_FIELD_COUNT;
	for (size_t field = 0; field < field_count; field++)
	{
		if (fields[field] == NULL)
			continue;
		error->fields[field] = duplicate_string(fields[field]);
		if (error->fields[field] == NULL)
		{
			pgm_error_free(error);
			return NULL;
		}
	}
	return error;
}


void
return_error(pgm_error **target, pgm_error *error)
{
	if (target != NULL)
		*target = error;
	else
		pgm_error_free(error);
}


pgm_status
return_simple_error(
	pgm_error **target, pgm_status status, const char *message)
{
	if (target != NULL)
		*target = make_error(status, NULL, NULL, NULL, "%s", message);
	return status;
}


static pgm_status
status_from_errno(int status)
{
	switch (status)
	{
		case 0:
			return PGM_STATUS_OK;
		case EINVAL:
			return PGM_STATUS_INVALID_ARGUMENT;
		case EBUSY:
		case EAGAIN:
		case EALREADY:
			return PGM_STATUS_BUSY;
		case ETIMEDOUT:
			return PGM_STATUS_TIMEOUT;
		case ECANCELED:
		case EINTR:
			return PGM_STATUS_CANCELED;
		case ENOMEM:
			return PGM_STATUS_OUT_OF_MEMORY;
		case ENOTSUP:
			return PGM_STATUS_UNSUPPORTED;
		case ESTALE:
			return PGM_STATUS_VERSION_MISMATCH;
		case EIO:
		case ENOENT:
		case EACCES:
		case EPERM:
			return PGM_STATUS_IO_ERROR;
	}
	return PGM_STATUS_INTERNAL_ERROR;
}


pgm_status
status_from_private(PostgammaPrivateLibpqStatus status)
{
	switch (status)
	{
		case POSTGAMMA_PRIVATE_LIBPQ_OK:
			return PGM_STATUS_OK;
		case POSTGAMMA_PRIVATE_LIBPQ_INVALID_ARGUMENT:
			return PGM_STATUS_INVALID_ARGUMENT;
		case POSTGAMMA_PRIVATE_LIBPQ_NO_MEMORY:
			return PGM_STATUS_OUT_OF_MEMORY;
		case POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT:
			return PGM_STATUS_TIMEOUT;
		case POSTGAMMA_PRIVATE_LIBPQ_CANCELLED:
			return PGM_STATUS_CANCELED;
		case POSTGAMMA_PRIVATE_LIBPQ_TRANSPORT_ERROR:
			return PGM_STATUS_IO_ERROR;
		case POSTGAMMA_PRIVATE_LIBPQ_PROTOCOL_ERROR:
		case POSTGAMMA_PRIVATE_LIBPQ_UPSTREAM_ERROR:
			return PGM_STATUS_CONNECTION_FAILED;
		case POSTGAMMA_PRIVATE_LIBPQ_COPY_TERMINATED:
			return PGM_STATUS_POSTGRES_ERROR;
		case POSTGAMMA_PRIVATE_LIBPQ_CONTRACT_VIOLATION:
			return PGM_STATUS_INTERNAL_ERROR;
	}
	return PGM_STATUS_INTERNAL_ERROR;
}


static int
deadline_after(uint64_t interval_ns, uint64_t *deadline_ns)
{
	uint64_t	now;

	if (deadline_ns == NULL)
		return EINVAL;
	now = postgamma_monotonic_now_ns();
	if (now == 0 || now > UINT64_MAX - interval_ns)
		return EOVERFLOW;
	*deadline_ns = now + interval_ns;
	return 0;
}


int
deadline_from_timeout(int64_t timeout_ms, uint64_t *deadline_ns)
{
	if (deadline_ns == NULL || timeout_ms < PGM_NO_TIMEOUT)
		return EINVAL;
	if (timeout_ms == PGM_NO_TIMEOUT)
	{
		*deadline_ns = POSTGAMMA_SUPERVISOR_NO_DEADLINE;
		return 0;
	}
	if ((uint64_t) timeout_ms > UINT64_MAX / UINT64_C(1000000))
		return EOVERFLOW;
	return deadline_after((uint64_t) timeout_ms * UINT64_C(1000000), deadline_ns);
}


int
private_deadline_after(int64_t interval_ns, int64_t *deadline_ns)
{
	int64_t		now;

	if (deadline_ns == NULL || interval_ns < 0 ||
		postgamma_memory_clock_now(&now) != POSTGAMMA_MEMORY_STATUS_OK ||
		now > INT64_MAX - interval_ns)
		return EOVERFLOW;
	*deadline_ns = now + interval_ns;
	return 0;
}


static int
run_kernel(PostgammaSupervisor *supervisor, void *argument)
{
	pgm_instance *instance = argument;

	(void) supervisor;
	if (!instance_is_valid(instance) || instance->entrypoints == NULL)
		return EINVAL;
	return instance->entrypoints->instance_main(
		&instance->boot_options, &instance->kernel_result);
}


static int
wait_and_destroy_ticket(
	PostgammaSupervisorTicket **ticket, uint64_t deadline_ns,
	int *operation_status)
{
	int			status;

	if (ticket == NULL || *ticket == NULL || operation_status == NULL)
		return EINVAL;
	status = postgamma_supervisor_ticket_wait(
		*ticket, deadline_ns, operation_status);
	if (status != 0)
		return status;
	status = postgamma_supervisor_ticket_destroy(*ticket);
	if (status == 0)
		*ticket = NULL;
	return status;
}


static int
cancel_private_request(
	void *argument, uint64_t connection_generation,
	uint64_t request_generation, int backend_pid)
{
	pgm_connection *connection = argument;
	PostgammaKernelCancelRequest cancel_request =
		POSTGAMMA_KERNEL_CANCEL_REQUEST_INIT;
	PostgammaSupervisorTicket *ticket = NULL;
	uint64_t	deadline_ns;
	int			operation_status = 0;
	int			status;

	if (!connection_is_valid(connection) ||
		connection->instance->generation != connection_generation ||
		request_generation == 0 || backend_pid != connection->backend_pid)
		return EINVAL;
	cancel_request.generation = connection_generation;
	cancel_request.request_generation = request_generation;
	cancel_request.backend_pid = backend_pid;
	status = postgamma_supervisor_submit(
		connection->instance->supervisor, connection_generation,
		POSTGAMMA_SUPERVISOR_CONTROL_CANCEL, &cancel_request, &ticket);
	if (status == 0)
		status = deadline_after(PGM_OPEN_TIMEOUT_NS, &deadline_ns);
	if (status == 0)
		status = wait_and_destroy_ticket(
			&ticket, deadline_ns, &operation_status);
	if (status == 0 && operation_status != 0)
		status = operation_status;
	if (status == 0 && cancel_request.dispatched != 1)
		status = EPROTO;
	if (ticket != NULL)
		(void) postgamma_supervisor_ticket_destroy(ticket);
	return status;
}


static void
forward_notice(
	void *argument, const char *sqlstate, const char *severity,
	const char *message, const char *detail, const char *hint)
{
	pgm_connection *connection = argument;

	if (!connection_is_valid(connection))
		return;
	postgamma_public_event_enqueue_notice(
		connection, sqlstate, severity, message, detail, hint);
}


static void
forward_notification(
	void *argument, int backend_pid,
	const char *channel, const char *payload)
{
	pgm_connection *connection = argument;

	if (!connection_is_valid(connection))
		return;
	postgamma_public_event_enqueue_notification(
		connection, backend_pid, channel, payload);
}


static void
notify_frontend_waitable(void *argument, uint32_t events)
{
	pgm_connection *connection = argument;

	postgamma_public_event_notify_connection(connection, events);
}


static int
wait_for_backend_release(pgm_connection *connection, int64_t timeout_ms)
{
	uint64_t	deadline_ns;
	int			status;

	if (!connection_is_valid(connection))
		return EINVAL;
	if (connection->backend_monitor == NULL)
		return 0;
	status = deadline_from_timeout(timeout_ms, &deadline_ns);
	if (status != 0)
		return status;
	for (;;)
	{
		PostgammaMemoryTelemetry telemetry;
		PostgammaMemoryStatus memory_status =
			postgamma_memory_endpoint_snapshot(
				connection->backend_monitor,
				connection->instance->generation, &telemetry);

		if (memory_status != POSTGAMMA_MEMORY_STATUS_OK)
			return EIO;
		if (telemetry.backend_references == 1)
			return 0;
		if (telemetry.backend_references < 1)
			return EPROTO;
		if (deadline_ns != POSTGAMMA_SUPERVISOR_NO_DEADLINE &&
			postgamma_monotonic_now_ns() >= deadline_ns)
			return ETIMEDOUT;
		{
			struct timespec pause = {0, 1000000L};

			(void) nanosleep(&pause, NULL);
		}
	}
}


static int
copy_instance_settings(
	const pgm_setting *settings, size_t setting_count,
	PostgammaKernelSetting **copied_settings)
{
	PostgammaKernelSetting *created;

	if (copied_settings == NULL ||
		(setting_count != 0 && settings == NULL))
		return EINVAL;
	*copied_settings = NULL;
	if (setting_count == 0)
		return 0;
	if (setting_count > SIZE_MAX / sizeof(*created))
		return EOVERFLOW;
	created = calloc(setting_count, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	for (size_t index = 0; index < setting_count; index++)
	{
		if (settings[index].name == NULL || settings[index].name[0] == '\0' ||
			settings[index].value == NULL)
		{
			free_instance_settings(created, setting_count);
			return EINVAL;
		}
		created[index].name = duplicate_string(settings[index].name);
		created[index].value = duplicate_string(settings[index].value);
		if (created[index].name == NULL || created[index].value == NULL)
		{
			free_instance_settings(created, setting_count);
			return ENOMEM;
		}
	}
	*copied_settings = created;
	return 0;
}


static bool
settings_are_valid(const pgm_setting *settings, size_t setting_count)
{
	if (setting_count != 0 && settings == NULL)
		return false;
	for (size_t index = 0; index < setting_count; index++)
	{
		const char *name = settings[index].name;

		if (name == NULL || name[0] == '\0' || settings[index].value == NULL)
			return false;
		for (const unsigned char *character = (const unsigned char *) name;
			 *character != '\0'; character++)
		{
			if (!isalnum(*character) && *character != '_' && *character != '.')
				return false;
		}
		for (size_t previous = 0; previous < index; previous++)
		{
			if (strcasecmp(settings[previous].name, name) == 0)
				return false;
		}
	}
	return true;
}


static void
free_instance_settings(
	PostgammaKernelSetting *settings, size_t setting_count)
{
	if (settings == NULL)
		return;
	for (size_t index = 0; index < setting_count; index++)
	{
		free((void *) settings[index].name);
		free((void *) settings[index].value);
	}
	free(settings);
}


static char *
build_startup_options(const pgm_setting *settings, size_t setting_count)
{
	size_t		length = 0;
	char	   *options;
	char	   *cursor;

	if (setting_count == 0)
		return duplicate_string("");
	if (settings == NULL)
	{
		errno = EINVAL;
		return NULL;
	}
	for (size_t index = 0; index < setting_count; index++)
	{
		const char *name = settings[index].name;
		const char *value = settings[index].value;

		if (name == NULL || name[0] == '\0' || value == NULL)
		{
			errno = EINVAL;
			return NULL;
		}
		for (const unsigned char *character = (const unsigned char *) name;
			 *character != '\0'; character++)
		{
			if (!isalnum(*character) && *character != '_' && *character != '.')
			{
				errno = EINVAL;
				return NULL;
			}
		}
		if (length > SIZE_MAX - strlen(name) - strlen(value) - 5)
		{
			errno = EOVERFLOW;
			return NULL;
		}
		length += strlen(name) + strlen(value) + 4;
		for (const unsigned char *character = (const unsigned char *) value;
			 *character != '\0'; character++)
		{
			if (isspace(*character) || *character == '\\')
				length++;
		}
	}
	options = malloc(length + 1);
	if (options == NULL)
		return NULL;
	cursor = options;
	for (size_t index = 0; index < setting_count; index++)
	{
		const char *name = settings[index].name;
		const char *value = settings[index].value;

		if (index != 0)
			*cursor++ = ' ';
		memcpy(cursor, "-c", 2);
		cursor += 2;
		memcpy(cursor, name, strlen(name));
		cursor += strlen(name);
		*cursor++ = '=';
		for (const unsigned char *character = (const unsigned char *) value;
			 *character != '\0'; character++)
		{
			if (isspace(*character) || *character == '\\')
				*cursor++ = '\\';
			*cursor++ = (char) *character;
		}
	}
	*cursor = '\0';
	return options;
}


static int
copy_request_parameters(
	const pgm_parameter *parameters, size_t parameter_count,
	PostgammaPrivateParameter **copied_parameters)
{
	PostgammaPrivateParameter *created;

	if (copied_parameters == NULL ||
		(parameter_count != 0 && parameters == NULL))
		return EINVAL;
	*copied_parameters = NULL;
	if (parameter_count == 0)
		return 0;
	if (parameter_count > SIZE_MAX / sizeof(*created))
		return EOVERFLOW;
	created = calloc(parameter_count, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	for (size_t index = 0; index < parameter_count; index++)
	{
		const pgm_parameter *source = &parameters[index];
		void	   *data = NULL;

		if (source->struct_size < sizeof(*source) || source->format > 1 ||
			source->is_null > 1 ||
			(source->is_null != 0 &&
			 (source->data != NULL || source->size != 0)) ||
			(source->is_null == 0 && source->data == NULL) ||
			source->size > INT32_MAX)
		{
			free_request_parameters(created, parameter_count);
			return EINVAL;
		}
		if (source->is_null == 0)
		{
			data = malloc(source->size + 1);
			if (data == NULL)
			{
				free_request_parameters(created, parameter_count);
				return ENOMEM;
			}
			memcpy(data, source->data, source->size);
			((unsigned char *) data)[source->size] = '\0';
		}
		created[index].type_oid = source->type_oid;
		created[index].format = source->format;
		created[index].is_null = source->is_null != 0;
		created[index].data = data;
		created[index].length = source->size;
	}
	*copied_parameters = created;
	return 0;
}


static void
free_request_parameters(
	PostgammaPrivateParameter *parameters, size_t parameter_count)
{
	if (parameters == NULL)
		return;
	for (size_t index = 0; index < parameter_count; index++)
		free((void *) parameters[index].data);
	free(parameters);
}


void
result_lease_release(PostgammaPublicResultLease *lease)
{
	if (lease == NULL)
		return;
	if (atomic_fetch_sub_explicit(
			&lease->references, 1U, memory_order_acq_rel) == 1U)
		free(lease);
}


pgm_status
result_attach_lease(
	pgm_result *result, PostgammaPublicResultLease *lease)
{
	bool		expected = false;

	if (!result_is_valid(result) || lease == NULL || result->lease != NULL)
		return PGM_STATUS_INVALID_ARGUMENT;
	if (!atomic_compare_exchange_strong_explicit(
			&lease->outstanding, &expected, true,
			memory_order_acq_rel, memory_order_acquire))
		return PGM_STATUS_BUSY;
	(void) atomic_fetch_add_explicit(
		&lease->references, 1U, memory_order_relaxed);
	result->lease = lease;
	return PGM_STATUS_OK;
}


static bool
request_is_active(pgm_request *request)
{
	bool		active = true;

	if (!request_is_valid(request) || request->mutex == NULL)
		return false;
	if (postgamma_mutex_lock(request->mutex) == 0)
	{
		active = request->state == PGM_REQUEST_PENDING ||
			request->state == PGM_REQUEST_RUNNING;
		(void) postgamma_mutex_unlock(request->mutex);
	}
	return active;
}


static bool
progress_unclaimed_copy(
	pgm_request *request, bool *handled, bool *made_progress)
{
	static const char release_message[] =
		"request released before COPY ownership transfer";
	unsigned char discard[8192];
	PostgammaPrivateCopyDirection direction;
	PostgammaPrivateCopyProgress progress;
	PostgammaPrivateLibpqStatus private_status;
	size_t		produced = 0;

	*handled = false;
	*made_progress = false;
	private_status = postgamma_private_libpq_copy_direction(
		request->operation, &direction);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		return false;
	if (direction == POSTGAMMA_PRIVATE_COPY_NONE)
		return true;
	*handled = true;
	if (direction == POSTGAMMA_PRIVATE_COPY_IN)
		private_status = postgamma_private_libpq_copy_finish(
			request->operation, release_message, &progress);
	else if (direction == POSTGAMMA_PRIVATE_COPY_OUT)
		private_status = postgamma_private_libpq_copy_read(
			request->operation, discard, sizeof(discard), &produced, &progress);
	else
		return false;
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
		return false;
	*made_progress = progress != POSTGAMMA_PRIVATE_COPY_AGAIN || produced != 0;
	if (progress != POSTGAMMA_PRIVATE_COPY_END)
		return true;
	return postgamma_private_libpq_copy_release(request->operation) ==
		POSTGAMMA_PRIVATE_LIBPQ_OK;
}


static bool
retire_active_request(pgm_request *request)
{
	uint64_t	deadline_ns;
	uint64_t	next_cancel_dispatch_ns = 0;

	if (!request_is_valid(request) ||
		deadline_after(PGM_CANCEL_PROPAGATION_TIMEOUT_NS, &deadline_ns) != 0)
		return false;
	for (;;)
	{
		PostgammaPrivateLibpqStatus private_status;
		pgm_result *discarded = NULL;
		pgm_request_state state;
		pgm_status	progress_status;
		uint64_t	now_ns;
		uint64_t	wait_deadline_ns;
		bool		copy_handled;
		bool		made_progress;

		if (postgamma_mutex_lock(request->mutex) != 0)
			return false;
		if (request->result != NULL)
		{
			discarded = request->result;
			request->result = NULL;
		}
		if (!progress_unclaimed_copy(
				request, &copy_handled, &made_progress))
		{
			(void) postgamma_mutex_unlock(request->mutex);
			pgm_result_free(discarded);
			return false;
		}
		progress_status = copy_handled ? PGM_STATUS_OK :
			progress_request_locked(request);
		if (request->result != NULL && discarded == NULL)
		{
			discarded = request->result;
			request->result = NULL;
		}
		state = request->state;
		made_progress = made_progress || discarded != NULL;
		(void) postgamma_mutex_unlock(request->mutex);
		pgm_result_free(discarded);
		if (progress_status != PGM_STATUS_OK)
			return false;
		if (state != PGM_REQUEST_PENDING && state != PGM_REQUEST_RUNNING)
			return true;
		now_ns = postgamma_monotonic_now_ns();
		if (now_ns >= deadline_ns)
			return false;
		if (next_cancel_dispatch_ns == 0 || now_ns >= next_cancel_dispatch_ns)
		{
			private_status = postgamma_private_libpq_cancel(
				request->connection->private_connection, request->generation);
			if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK)
				return false;
			next_cancel_dispatch_ns =
				PGM_CANCEL_REDISPATCH_INTERVAL_NS > deadline_ns - now_ns ?
				deadline_ns : now_ns + PGM_CANCEL_REDISPATCH_INTERVAL_NS;
		}
		if (made_progress)
			continue;
		wait_deadline_ns = next_cancel_dispatch_ns < deadline_ns ?
			next_cancel_dispatch_ns : deadline_ns;
		private_status = postgamma_private_libpq_operation_wait(
			request->operation, (int64_t) wait_deadline_ns);
		if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK &&
			private_status != POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT)
			return false;
	}
}


static void
abort_active_request(pgm_request *request)
{
	pgm_connection *connection;

	if (!request_is_valid(request))
		return;
	connection = request->connection;
	if (!connection_is_valid(connection))
		return;
	if (connection->backend_monitor != NULL)
		(void) postgamma_memory_endpoint_abort(
			connection->backend_monitor, connection->instance->generation,
			POSTGAMMA_MEMORY_ABORT_DEADLINE);
	if (postgamma_mutex_lock(connection->mutex) == 0)
	{
		connection->failed = true;
		(void) postgamma_mutex_unlock(connection->mutex);
	}
}


pgm_status
progress_request_locked(pgm_request *request)
{
	PostgammaPrivateOwnedResult *private_result = NULL;
	PostgammaPrivateLibpqProgress progress;
	PostgammaPrivateLibpqStatus private_status;
	pgm_status	operation_status;

	if (!request_is_valid(request))
		return PGM_STATUS_INVALID_ARGUMENT;
	if (request->state != PGM_REQUEST_PENDING &&
		request->state != PGM_REQUEST_RUNNING)
		return PGM_STATUS_OK;
	if (request->result != NULL)
		return PGM_STATUS_OK;
	request->state = PGM_REQUEST_RUNNING;
	if (request->active_copy != NULL)
	{
		PostgammaPrivateCopyProgress copy_progress;

		private_status = postgamma_private_libpq_copy_pump(
			request->operation, &copy_progress);
		return status_from_private(private_status);
	}
	private_status = postgamma_private_libpq_operation_progress(
		request->operation, &progress, &private_result);
	if (private_status != POSTGAMMA_PRIVATE_LIBPQ_OK &&
		progress != POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_COMPLETE)
		return status_from_private(private_status);
	if (progress == POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_RESULT_READY)
	{
		request->result = wrap_result(private_result);
		if (request->result == NULL)
		{
			postgamma_private_libpq_result_free(private_result);
			request->operation_status = PGM_STATUS_OUT_OF_MEMORY;
			request->error = make_error(
				PGM_STATUS_OUT_OF_MEMORY, NULL, NULL, NULL,
				"could not allocate embedded result handle");
			request->state = PGM_REQUEST_FAILED;
			request_account_terminal(request);
			abort_active_request(request);
			return PGM_STATUS_OK;
		}
		request->result_count++;
		if (private_result->status == POSTGAMMA_PRIVATE_RESULT_FATAL_ERROR ||
			private_result->status == POSTGAMMA_PRIVATE_RESULT_BAD_RESPONSE ||
			private_result->status == POSTGAMMA_PRIVATE_RESULT_NONFATAL_ERROR)
		{
			request->saw_cancel_error =
				strcmp(private_result->sqlstate, "57014") == 0;
			request->saw_postgres_error = !request->saw_cancel_error;
			operation_status = request->saw_cancel_error ?
				PGM_STATUS_CANCELED : PGM_STATUS_POSTGRES_ERROR;
			if (request->error == NULL)
				request->error = error_from_result(
					operation_status, private_result);
		}
		return PGM_STATUS_OK;
	}
	if (progress != POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_COMPLETE)
		return PGM_STATUS_OK;
	operation_status = status_from_private(private_status);
	if (private_status == POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		if (request->saw_cancel_error || request->cancel_requested)
			operation_status = PGM_STATUS_CANCELED;
		else if (request->saw_postgres_error)
			operation_status = PGM_STATUS_POSTGRES_ERROR;
	}
	else if (request->error == NULL)
		request->error = make_error(
			operation_status, NULL, NULL, NULL,
			"private libpq request failed: %s",
			postgamma_private_libpq_status_name(private_status));
	request->operation_status = operation_status;
	if (operation_status == PGM_STATUS_OK)
		request->state = PGM_REQUEST_COMPLETED;
	else if (operation_status == PGM_STATUS_CANCELED ||
		request->cancel_requested)
		request->state = PGM_REQUEST_CANCELED;
	else
		request->state = PGM_REQUEST_FAILED;
	request_account_terminal(request);
	return PGM_STATUS_OK;
}


void
request_account_terminal(pgm_request *request)
{
	pgm_instance *instance;

	if (!request_is_valid(request) || request->terminal_accounted ||
		(request->state != PGM_REQUEST_COMPLETED &&
		 request->state != PGM_REQUEST_CANCELED &&
		 request->state != PGM_REQUEST_FAILED))
		return;
	instance = request->connection->instance;
	request->terminal_accounted = true;
	(void) atomic_fetch_sub_explicit(
		&instance->active_request_count, UINT64_C(1), memory_order_relaxed);
	if (request->state == PGM_REQUEST_COMPLETED)
		(void) atomic_fetch_add_explicit(
			&instance->completed_request_count, UINT64_C(1),
			memory_order_relaxed);
	else if (request->state == PGM_REQUEST_CANCELED)
		(void) atomic_fetch_add_explicit(
			&instance->canceled_request_count, UINT64_C(1),
			memory_order_relaxed);
	else
		(void) atomic_fetch_add_explicit(
			&instance->failed_request_count, UINT64_C(1),
			memory_order_relaxed);
}


static pgm_result *
wrap_result(PostgammaPrivateOwnedResult *private_result)
{
	pgm_result *result;

	if (private_result == NULL)
		return NULL;
	result = calloc(1, sizeof(*result));
	if (result == NULL)
		return NULL;
	result->magic = PGM_RESULT_MAGIC;
	result->private_result = private_result;
	return result;
}


pgm_error *
error_from_result(
	pgm_status status, const PostgammaPrivateOwnedResult *result)
{
	if (result == NULL)
		return make_error(
			status, NULL, NULL, NULL, "PostgreSQL request failed");
	{
		const char *fields[PGM_DIAGNOSTIC_FIELD_COUNT];
		pgm_error  *error;

		for (size_t field = 0; field < PGM_DIAGNOSTIC_FIELD_COUNT; field++)
			fields[field] = result->diagnostics[field];
		if (fields[PGM_DIAG_SQLSTATE - 1] == NULL)
			fields[PGM_DIAG_SQLSTATE - 1] = result->sqlstate;
		if (fields[PGM_DIAG_SEVERITY - 1] == NULL)
			fields[PGM_DIAG_SEVERITY - 1] = result->severity;
		if (fields[PGM_DIAG_MESSAGE - 1] == NULL)
			fields[PGM_DIAG_MESSAGE - 1] = result->message[0] != '\0' ?
				result->message : "PostgreSQL request failed";
		if (fields[PGM_DIAG_DETAIL - 1] == NULL)
			fields[PGM_DIAG_DETAIL - 1] = result->detail;
		error = make_error_from_diagnostics(
			status, fields, PGM_DIAGNOSTIC_FIELD_COUNT);
		return error;
	}
}


static pgm_result_status
public_result_status(PostgammaPrivateResultStatus status)
{
	switch (status)
	{
		case POSTGAMMA_PRIVATE_RESULT_COMMAND_OK:
			return PGM_RESULT_COMMAND_OK;
		case POSTGAMMA_PRIVATE_RESULT_TUPLES_OK:
			return PGM_RESULT_TUPLES_OK;
		case POSTGAMMA_PRIVATE_RESULT_COPY_IN:
			return PGM_RESULT_COPY_IN;
		case POSTGAMMA_PRIVATE_RESULT_COPY_OUT:
			return PGM_RESULT_COPY_OUT;
		case POSTGAMMA_PRIVATE_RESULT_EMPTY_QUERY:
			return PGM_RESULT_EMPTY_QUERY;
		case POSTGAMMA_PRIVATE_RESULT_TUPLES_CHUNK:
			return PGM_RESULT_TUPLES_CHUNK;
		case POSTGAMMA_PRIVATE_RESULT_NONE:
		case POSTGAMMA_PRIVATE_RESULT_BAD_RESPONSE:
		case POSTGAMMA_PRIVATE_RESULT_NONFATAL_ERROR:
		case POSTGAMMA_PRIVATE_RESULT_FATAL_ERROR:
		case POSTGAMMA_PRIVATE_RESULT_COPY_BOTH:
		case POSTGAMMA_PRIVATE_RESULT_SINGLE_TUPLE:
		case POSTGAMMA_PRIVATE_RESULT_PIPELINE_SYNC:
		case POSTGAMMA_PRIVATE_RESULT_PIPELINE_ABORTED:
			return PGM_RESULT_ERROR;
	}
	return PGM_RESULT_ERROR;
}
