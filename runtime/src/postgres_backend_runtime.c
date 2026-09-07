/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_backend_runtime.c
 *    PostgreSQL integration for threaded backend executions.
 *
 * Generated include files provide only reviewed, version-dependent facts.
 * Launch, signal, completion, and lifecycle algorithms remain handwritten.
 *
 *-------------------------------------------------------------------------
 */

#define POSTGAMMA_POSTGRES_RUNTIME_IMPLEMENTATION
#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdlib.h>
#ifdef HAVE_GETRLIMIT
#include <sys/resource.h>
#endif
#include <sys/wait.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "fmgr.h"
#include "libpq/hba.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "postgamma/extension_runtime.h"
#include "postgamma/instance_runtime.h"
#include "postgamma/postmaster_control_runtime.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/pg_shmem.h"
#include "storage/shmem_internal.h"
#include "storage/waiteventset.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"


extern HTAB *GetLockMethodLocalHash(void);


#define PG_STATE(name) POSTGAMMA_BACKEND_STATE_GLOBAL(name)
#define POSTGAMMA_REGISTRY_EXTRA_SLOTS 128
#define POSTGAMMA_STATIC_MODULES_PER_ROLE 32


typedef enum PostgammaLocaleCategory
{
	POSTGAMMA_LOCALE_COLLATE = 0,
	POSTGAMMA_LOCALE_CTYPE,
	POSTGAMMA_LOCALE_MESSAGES,
	POSTGAMMA_LOCALE_MONETARY,
	POSTGAMMA_LOCALE_NUMERIC,
	POSTGAMMA_LOCALE_TIME,
	POSTGAMMA_LOCALE_CATEGORY_COUNT
} PostgammaLocaleCategory;


typedef enum PostgammaBackendDisposition
{
	POSTGAMMA_BACKEND_DISPOSITION_THREAD = 0,
	POSTGAMMA_BACKEND_DISPOSITION_LOGICAL_ALIAS,
	POSTGAMMA_BACKEND_DISPOSITION_FORBIDDEN
} PostgammaBackendDisposition;

typedef enum PostgammaBackendPolicyClass
{
	POSTGAMMA_BACKEND_POLICY_CLASS_NONE = 0,
	POSTGAMMA_BACKEND_POLICY_CLASS_CLIENT,
	POSTGAMMA_BACKEND_POLICY_CLASS_DEDICATED,
	POSTGAMMA_BACKEND_POLICY_CLASS_DYNAMIC
} PostgammaBackendPolicyClass;

typedef struct PostgammaExternalPipe
{
	FILE	   *stream;
	pid_t		pid;
	int			wait_status;
	bool		stream_closed;
	bool		child_reaped;
	struct PostgammaExternalPipe *next;
} PostgammaExternalPipe;

/*
 * A carrier frame is valid only while one native worker is executing a
 * logical backend.  No address into this frame may survive a yield boundary.
 */
typedef struct PostgammaCarrierFrame
{
	PostgammaExecutionContext *previous_execution;
	pg_stack_base_t previous_stack_base;
	sigjmp_buf	exit_jump;
	int			saved_errno;
#if !defined(WIN32) && defined(HAVE_USELOCALE)
	locale_t	previous_locale;
#endif
	bool		exit_jump_ready;
	bool		execution_bound;
} PostgammaCarrierFrame;

typedef struct PostgammaVirtualTimer
{
	TimestampTz timeout_due_at;
	int64		timeout_interval_us;
	bool		timeout_armed;
	bool		timeout_wait_shortened;
} PostgammaVirtualTimer;

typedef struct PostgammaStandaloneSignalState
{
	PostgammaVirtualTimer timer;
	sigset_t	logical_signal_mask;
	pqsigfunc	signal_handlers[POSTGAMMA_BACKEND_SIGNAL_COUNT + 1];
	uint64		pending_signals;
	bool		dispatching_signals;
} PostgammaStandaloneSignalState;

/* Persistent state owned by one logical PostgreSQL backend session. */
typedef struct PostgammaClientSession
{
	PostgammaInstanceContext private_instance;
	PostgammaInstanceContext *shared_instance;
	struct PostgammaPathRuntime *path_runtime;
	PostgammaInstanceRuntime *runtime;
	PostgammaBackendRegistry *registry;
	PostgammaWakeTarget *wake_target;
	PostgammaKernelServerTransportOps transport_ops;
	void	   *transport;
	uint64_t	transport_generation;
	PostgammaKernelResultPolicy result_policy;
	uint64_t	connection_id;
	size_t		result_budget_bytes;
	uint32_t	result_budget_rows;
	PostgammaRoleContext role;
	PostgammaExecutionContext execution;
	PostgammaCarrierFrame carrier;
	PostgammaPostgresBackendMain main_function;
	PostgammaBackendHandle handle;
	PostgammaBackendExitStatus exit_status;
	sigset_t	logical_signal_mask;
	pqsigfunc	signal_handlers[POSTGAMMA_BACKEND_SIGNAL_COUNT + 1];
	char	   *guc_state;
	size_t		guc_state_length;
	ClientSocket client_socket;
	int			backend_type;
	int			child_slot;
	int			client_socket_fd;
	pid_t		system_child_pid;
	PostgammaExternalPipe *external_pipes;
	uint64_t	initialized_module_generation;
	const void *initialized_module_handles[POSTGAMMA_STATIC_MODULES_PER_ROLE];
	size_t		initialized_module_count;
	PostgammaVirtualTimer timer;
#if !defined(WIN32) && defined(HAVE_USELOCALE)
	locale_t	session_locale;
	char		locale_names[POSTGAMMA_LOCALE_CATEGORY_COUNT][LOCALE_NAME_BUFLEN];
#endif
	bool		shared_memory_access;
	bool		has_client_socket;
	bool		transport_notify_bound;
	bool		memory_contexts_initialized;
	bool		dsm_control_handle_set;
	bool		wait_support_initialized;
	bool		dispatching_signals;
	bool		backend_initialized;
	bool		postgres_main_initialized;
	bool		quantum_yielded;
	bool		result_budget_active;
	bool		saved_send_ready_for_query;
	bool		saved_idle_in_transaction_timeout_enabled;
	bool		saved_idle_session_timeout_enabled;
	uint64_t	saved_idle_session_deadline_ns;
#if !defined(WIN32) && defined(HAVE_USELOCALE)
	bool		locale_names_initialized;
#endif
} PostgammaClientSession;


static POSTGAMMA_THREAD_LOCAL PostgammaClientSession *
	PostgammaCurrentBackendSession;
static POSTGAMMA_THREAD_LOCAL const char *PostgammaCurrentBootstrapInput;
static POSTGAMMA_THREAD_LOCAL size_t PostgammaCurrentBootstrapInputLength;
static POSTGAMMA_THREAD_LOCAL const char *PostgammaCurrentStandaloneInput;
static POSTGAMMA_THREAD_LOCAL size_t PostgammaCurrentStandaloneInputLength;
static POSTGAMMA_THREAD_LOCAL size_t PostgammaCurrentStandaloneInputOffset;
static POSTGAMMA_THREAD_LOCAL PostgammaStandaloneSignalState
	PostgammaStandaloneSignals;
static POSTGAMMA_THREAD_LOCAL PostgammaKernelConnectRequest *
	PostgammaPendingConnectRequest;
static POSTGAMMA_THREAD_LOCAL uint64_t
	PostgammaCarrierInitializedModuleGeneration;
static POSTGAMMA_THREAD_LOCAL const void *
	PostgammaCarrierInitializedModuleHandles[POSTGAMMA_STATIC_MODULES_PER_ROLE];
static POSTGAMMA_THREAD_LOCAL size_t PostgammaCarrierInitializedModuleCount;
static pthread_mutex_t PostgammaStaticModuleProviderMutex =
	PTHREAD_MUTEX_INITIALIZER;
static const PostgammaStaticModuleProvider *PostgammaInstalledModuleProvider;

extern char **environ;

static const char *const PostgammaInheritedBackendState[] =
{
#define POSTGAMMA_BACKEND_INHERITED_STATE(identifier, strategy) identifier,
#include "postgamma/backend_inheritance.inc"
#undef POSTGAMMA_BACKEND_INHERITED_STATE
};


static PostgammaInstanceRuntime *postgamma_current_instance_runtime(void);
static bool postgamma_embedded_profile_active(void);
static const PostgammaStaticModuleProvider *
postgamma_current_static_module_provider(void);
static const void *postgamma_open_static_module(
	const char *filename,
	const PostgammaStaticModuleProvider **provider_result);
static void postgamma_validate_static_module(
	const PostgammaStaticModuleProvider *provider,
	const void *handle, const char *filename);
static void postgamma_initialize_static_module(
	const PostgammaStaticModuleProvider *provider,
	const void *handle, const char *filename);
static PostgammaBackendRegistry *postgamma_current_backend_registry(void);
static int postgamma_ensure_registry(
	PostgammaInstanceRuntime *runtime,
	PostgammaBackendRegistry **registry);
static int postgamma_prepare_process_fd_limit(int child_capacity);
static bool transport_ops_are_valid(
	const PostgammaKernelServerTransportOps *transport_ops);
static void postgamma_backend_transport_notify(
	void *argument, uint32_t events);
static int postgamma_release_backend_transport(
	PostgammaClientSession *state);
static bool postgamma_execution_policy(
	int backend_type, PostgammaBackendDisposition *disposition,
	PostgammaBackendPolicyClass *policy_class);
static PostgammaBackendClass postgamma_execution_class(
	PostgammaBackendPolicyClass policy_class,
	const void *startup_data, size_t startup_data_length);
static PostgammaBackendExitStatus postgamma_postgres_backend_main(
	const PostgammaBackendStartInfo *start_info,
	const void *startup_data, size_t startup_data_length,
	void *main_argument);
static int postgamma_postgres_backend_cleanup(
	const PostgammaBackendStartInfo *start_info, void *cleanup_argument);
static void postgamma_postgres_backend_completed(void *argument);
static void postgamma_initialize_backend_thread(
	PostgammaClientSession *state,
	const PostgammaBackendStartInfo *start_info,
	const void *startup_data, size_t startup_data_length);
static void postgamma_bind_backend_session(PostgammaClientSession *state);
static void postgamma_unbind_backend_session(PostgammaClientSession *state);
static void postgamma_validate_backend_stack_depth(
	const PostgammaBackendStartInfo *start_info);
static void postgamma_log_backend_stack_telemetry(
	uint64_t generation, const char *class_name,
	const PostgammaBackendStackTelemetry *stack);
static void postgamma_discard_memory_context_callbacks(MemoryContext context);
static bool postgamma_backend_exit_is_crash(
	PostgammaBackendExitStatus exit_status);
static void postgamma_set_exit_status(
	PostgammaBackendExitKind kind, int code);
static int postgamma_wait_status(PostgammaBackendExitStatus status);
static bool postgamma_decode_compat_pid(pid_t pid,
									   PostgammaCompatPid *compat_pid);
static PostgammaVirtualTimer *postgamma_current_virtual_timer(void);
static bool postgamma_virtual_timer_due(PostgammaVirtualTimer *timer);
static bool postgamma_session_holds_advisory_lock(void);
static bool postgamma_quantum_is_pooled_client(
	const PostgammaClientSession *state);
static bool postgamma_quantum_update_pin_state(
	PostgammaClientSession *state);
static bool postgamma_quantum_prepare_yield(
	PostgammaClientSession *state, bool yield_when_runnable);
static int postgamma_initialize_shell_spawn_attributes(
	posix_spawnattr_t *attributes, bool reset_sigpipe);
static int postgamma_set_close_on_exec(int descriptor);
static void postgamma_system_child_finished(
	PostgammaClientSession *state, pid_t pid);
static void postgamma_external_pipe_finished(
	PostgammaClientSession *state,
	PostgammaExternalPipe *pipe, int wait_status);
static int postgamma_wait_external_child(pid_t pid, int *wait_status);
static void postgamma_cleanup_external_children(
	PostgammaClientSession *state);
static void postgamma_release_external_pipes(
	PostgammaClientSession *state);
#if !defined(WIN32) && defined(HAVE_USELOCALE)
static int postgamma_locale_category_index(int category);
static int postgamma_locale_category_mask(int category);
static const char *postgamma_effective_locale_name(
	int category, const char *locale);
static void postgamma_initialize_locale_names(
	PostgammaClientSession *state);
#endif

Datum pg_postgamma_crash_backend(PG_FUNCTION_ARGS);
Datum pg_postgamma_pause_backend(PG_FUNCTION_ARGS);


pid_t
postgamma_postmaster_child_launch(
	int backend_type, int child_slot,
	void *startup_data, size_t startup_data_length,
	const void *client_socket,
	PostgammaPostgresBackendMain main_function,
	bool shared_memory_access, const char *thread_name)
{
	PostgammaClientSession *state;
	PostgammaExecutionContext *parent_execution;
	PostgammaInstanceRuntime *runtime;
	PostgammaBackendRegistry *registry;
	PostgammaBackendDisposition disposition;
	PostgammaBackendPolicyClass policy_class;
	PostgammaBackendLaunchRequest request;
	PostgammaBackendHandle handle;
	Size		guc_state_length;
	int			status;
	bool		instance_initialized = false;
	bool		role_initialized = false;
	bool		execution_initialized = false;
	bool		syslogger_startup_prepared = false;
	bool		syslogger_pipe_prepared = false;
	PostgammaKernelConnectRequest *connect_request = NULL;
	char		generated_name[16];

	(void) thread_name;
	if (main_function == NULL || backend_type <= B_INVALID ||
		backend_type >= BACKEND_NUM_TYPES || child_slot < 0 ||
		(startup_data_length != 0 && startup_data == NULL))
	{
		errno = EINVAL;
		return -1;
	}
	if (IsExternalConnectionBackend((BackendType) backend_type))
	{
		BackendStartupData *backend_startup;

		if (startup_data == NULL ||
			startup_data_length != sizeof(BackendStartupData))
		{
			errno = EINVAL;
			return -1;
		}
		backend_startup = startup_data;
		backend_startup->fork_started = GetCurrentTimestamp();
	}
	parent_execution = postgamma_execution_context_current();
	if (parent_execution == NULL)
	{
		errno = EPROTO;
		return -1;
	}
	runtime = postgamma_instance_context_runtime(parent_execution->instance);
	if (runtime == NULL)
	{
		errno = EPROTO;
		return -1;
	}
	status = postgamma_ensure_registry(runtime, &registry);
	if (status != 0)
	{
		errno = status;
		return -1;
	}
	if (!postgamma_execution_policy(backend_type, &disposition, &policy_class) ||
		disposition != POSTGAMMA_BACKEND_DISPOSITION_THREAD)
	{
		postgamma_backend_registry_record_unsupported_request(
			registry);
		errno = ENOTSUP;
		return -1;
	}

	state = calloc(1, sizeof(*state));
	if (state == NULL)
	{
		errno = ENOMEM;
		return -1;
	}
	state->shared_instance = parent_execution->instance;
	state->runtime = runtime;
	state->registry = registry;
	state->main_function = main_function;
	state->backend_type = backend_type;
	state->child_slot = child_slot;
	state->shared_memory_access = shared_memory_access;
	state->client_socket_fd = PGINVALID_SOCKET;
	postgamma_instance_context_init_shared(
		&state->private_instance, state->shared_instance);
	instance_initialized = true;
	postgamma_instance_context_attach_runtime(&state->private_instance, runtime);
	state->path_runtime = postgamma_instance_context_path_runtime(
		parent_execution->instance);
	if (state->path_runtime != NULL)
		postgamma_instance_context_attach_path_runtime(
			&state->private_instance, state->path_runtime);
	status = postgamma_role_context_init(&state->role, &state->private_instance);
	if (status != 0)
		goto fail;
	role_initialized = true;
	status = postgamma_execution_context_init(
		&state->execution, &state->private_instance, &state->role);
	if (status != 0)
		goto fail;
	execution_initialized = true;
	if (IsExternalConnectionBackend((BackendType) backend_type) &&
		PostgammaPendingConnectRequest != NULL)
	{
		connect_request = PostgammaPendingConnectRequest;
		if (!transport_ops_are_valid(connect_request->transport_ops) ||
			connect_request->transport == NULL ||
			connect_request->generation !=
				postgamma_instance_runtime_generation(runtime))
		{
			status = EPROTO;
			goto fail;
		}
		state->transport_ops = *connect_request->transport_ops;
		state->transport_generation = connect_request->generation;
		state->connection_id = connect_request->connection_id;
		postgamma_execution_context_set_connection_id(
			&state->execution, state->connection_id);
		status = state->transport_ops.retain(
			connect_request->transport, state->transport_generation,
			&state->transport);
		if (status != 0 || state->transport == NULL)
		{
			if (status == 0)
				status = EPROTO;
			goto fail;
		}
	}
	postgamma_backend_state_context_copy_ids(
		state->role.postgres_backend_state,
		parent_execution->role->postgres_backend_state,
		PostgammaInheritedBackendState,
		lengthof(PostgammaInheritedBackendState));

	guc_state_length = EstimateGUCStateSpace();
	if (guc_state_length > 0)
	{
		state->guc_state = malloc(guc_state_length);
		if (state->guc_state == NULL)
		{
			status = ENOMEM;
			goto fail;
		}
		SerializeGUCState(guc_state_length, state->guc_state);
		state->guc_state_length = guc_state_length;
	}
	if (client_socket != NULL)
	{
		memcpy(&state->client_socket, client_socket, sizeof(ClientSocket));
		state->client_socket_fd = state->client_socket.sock;
		state->has_client_socket = true;
	}
	if (backend_type == B_LOGGER)
	{
		PostgammaExecutionContext *previous_execution;

		previous_execution = postgamma_execution_context_bind(&state->execution);
		status = postgamma_prepare_syslogger_pipe_state();
		postgamma_execution_context_restore(
			&state->execution, previous_execution);
		if (status != 0)
			goto fail;
		syslogger_pipe_prepared = true;
		status = postgamma_prepare_syslogger_startup_data(
			startup_data, startup_data_length);
		if (status != 0)
			goto fail;
		syslogger_startup_prepared = true;
	}

	memset(&request, 0, sizeof(request));
	snprintf(generated_name, sizeof(generated_name), "pg-b%d", backend_type);
	request.backend_type = backend_type;
	request.execution_class = postgamma_execution_class(
		policy_class, startup_data, startup_data_length);
	request.thread_name = generated_name;
	request.startup_data = startup_data;
	request.startup_data_length = startup_data_length;
	request.main_function = postgamma_postgres_backend_main;
	request.main_argument = state;
	request.cleanup_function = postgamma_postgres_backend_cleanup;
	request.cleanup_argument = state;
	request.native_wake_signal =
		postgamma_instance_runtime_profile(runtime) ==
		POSTGAMMA_RUNTIME_PROFILE_EMBEDDED ? 0 : SIGURG;
	request.completion_notification = postgamma_postgres_backend_completed;
	request.completion_notification_argument = runtime;
	status = postgamma_backend_launch(registry, &request, &handle);
	if (status != 0)
		goto fail;
	if (connect_request != NULL)
		connect_request->backend_pid = handle.compat_pid;
	syslogger_startup_prepared = false;
	if (client_socket != NULL)
		((ClientSocket *) client_socket)->sock = PGINVALID_SOCKET;
	return (pid_t) handle.compat_pid;

fail:
	if (syslogger_startup_prepared)
		postgamma_discard_syslogger_startup_data(
			startup_data, startup_data_length);
	if (syslogger_pipe_prepared)
	{
		PostgammaExecutionContext *previous_execution;

		previous_execution = postgamma_execution_context_bind(&state->execution);
		postgamma_discard_syslogger_pipe_state();
		postgamma_execution_context_restore(
			&state->execution, previous_execution);
	}
	free(state->guc_state);
	(void) postgamma_release_backend_transport(state);
	if (execution_initialized)
		postgamma_execution_context_destroy(&state->execution);
	if (role_initialized)
		postgamma_role_context_destroy(&state->role);
	if (instance_initialized && state->path_runtime != NULL)
		postgamma_instance_context_detach_path_runtime(
			&state->private_instance, state->path_runtime);
	if (instance_initialized)
	{
		postgamma_instance_context_detach_runtime(
			&state->private_instance, runtime);
		postgamma_instance_context_destroy(&state->private_instance);
	}
	free(state);
	errno = status;
	return -1;
}


int
postgamma_postmaster_connect_begin(PostgammaKernelConnectRequest *request)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	PostgammaInstanceRuntime *runtime;

	if (request == NULL || request->struct_size != sizeof(*request) ||
		request->abi_version != POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION ||
		request->generation == 0 || request->transport == NULL ||
		!transport_ops_are_valid(request->transport_ops) ||
		request->connection_id == 0 || request->backend_pid != 0 ||
		execution == NULL ||
		PostgammaPendingConnectRequest != NULL)
		return EINVAL;
	runtime = postgamma_instance_context_runtime(execution->instance);
	if (runtime == NULL || request->generation !=
		postgamma_instance_runtime_generation(runtime))
		return ESTALE;
	PostgammaPendingConnectRequest = request;
	return 0;
}


int
postgamma_postmaster_connect_end(
	PostgammaKernelConnectRequest *request, int launch_status)
{
	if (request == NULL || PostgammaPendingConnectRequest != request)
		return EINVAL;
	PostgammaPendingConnectRequest = NULL;
	if (launch_status != 0)
		return launch_status;
	return request->backend_pid > 0 ? 0 : EPROTO;
}


bool
postgamma_in_backend_thread(void)
{
	return PostgammaCurrentBackendSession != NULL;
}


bool
postgamma_backend_transport_is_bound(void)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;

	return state != NULL && state->transport != NULL &&
		transport_ops_are_valid(&state->transport_ops);
}


bool
postgamma_backend_transport_read(
	void *buffer, size_t length, ssize_t *result)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	size_t		transferred = 0;
	int			status;

	if (!postgamma_backend_transport_is_bound())
		return false;
	if (buffer == NULL || length == 0 || result == NULL || length > SSIZE_MAX)
	{
		errno = EINVAL;
		if (result != NULL)
			*result = -1;
		return true;
	}
	status = state->transport_ops.read(
		state->transport, state->transport_generation,
		buffer, length, &transferred);
	if (status == 0 && transferred <= length && transferred <= SSIZE_MAX)
	{
		*result = (ssize_t) transferred;
		errno = 0;
	}
	else
	{
		*result = -1;
		errno = status != 0 ? status : EPROTO;
	}
	return true;
}


bool
postgamma_backend_transport_write(
	const void *buffer, size_t length, ssize_t *result)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	size_t		transferred = 0;
	int			status;

	if (!postgamma_backend_transport_is_bound())
		return false;
	if (buffer == NULL || length == 0 || result == NULL || length > SSIZE_MAX)
	{
		errno = EINVAL;
		if (result != NULL)
			*result = -1;
		return true;
	}
	status = state->transport_ops.write(
		state->transport, state->transport_generation,
		buffer, length, &transferred);
	if (status == 0 && transferred != 0 && transferred <= length &&
		transferred <= SSIZE_MAX)
	{
		*result = (ssize_t) transferred;
		errno = 0;
	}
	else
	{
		*result = -1;
		errno = status != 0 ? status : EPROTO;
	}
	return true;
}


void
postgamma_postgres_result_budget_begin(void)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	PostgammaKernelResultPolicy policy = POSTGAMMA_KERNEL_RESULT_POLICY_INIT;
	int			status;

	if (state == NULL)
		return;
	state->result_budget_active = false;
	state->result_budget_bytes = 0;
	state->result_budget_rows = 0;
	state->result_policy = policy;
	if (!postgamma_backend_transport_is_bound())
		return;
	status = state->transport_ops.get_result_policy(
		state->transport, state->transport_generation, &policy);
	if (status != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not read the embedded result policy: %s",
						strerror(status))));
	if (policy.request_generation == 0)
		return;
	if ((policy.delivery_mode != POSTGAMMA_KERNEL_DELIVERY_MATERIALIZED &&
		 policy.delivery_mode != POSTGAMMA_KERNEL_DELIVERY_CHUNKED) ||
		policy.result_buffer_limit == 0 || policy.maximum_value_size == 0 ||
		policy.maximum_value_size > policy.result_buffer_limit ||
		(policy.delivery_mode == POSTGAMMA_KERNEL_DELIVERY_MATERIALIZED &&
		 policy.target_chunk_rows != 0) ||
		(policy.delivery_mode == POSTGAMMA_KERNEL_DELIVERY_CHUNKED &&
		 policy.target_chunk_rows == 0))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("invalid embedded result policy")));
	state->result_policy = policy;
	state->result_budget_active = true;
}


void
postgamma_postgres_result_budget_check(const void *data, size_t length)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	const unsigned char *bytes = data;
	size_t		offset = 0;
	size_t		row_charge;
	uint16_t	column_count;

	if (state == NULL || !state->result_budget_active)
		return;
	if (bytes == NULL || length < sizeof(uint16_t))
		goto malformed;
	column_count = ((uint16_t) bytes[0] << 8) | (uint16_t) bytes[1];
	offset = sizeof(uint16_t);
	for (uint16_t column = 0; column < column_count; column++)
	{
		uint32_t raw_length;
		int32_t	field_length;

		if (length - offset < sizeof(uint32_t))
			goto malformed;
		raw_length = ((uint32_t) bytes[offset] << 24) |
			((uint32_t) bytes[offset + 1] << 16) |
			((uint32_t) bytes[offset + 2] << 8) |
			(uint32_t) bytes[offset + 3];
		offset += sizeof(uint32_t);
		field_length = (int32_t) raw_length;
		if (field_length == -1)
			continue;
		if (field_length < 0 || (size_t) field_length > length - offset)
			goto malformed;
		if ((size_t) field_length > state->result_policy.maximum_value_size)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("embedded result value exceeds maximum_value_size"),
					 errdetail("Value size is %zu bytes; the configured limit is %zu bytes.",
							   (size_t) field_length,
							   state->result_policy.maximum_value_size)));
		offset += (size_t) field_length;
	}
	if (offset != length)
		goto malformed;
	if (length > SIZE_MAX - 5)
		goto malformed;
	row_charge = length + 5;
	if (state->result_policy.delivery_mode ==
		POSTGAMMA_KERNEL_DELIVERY_CHUNKED &&
		state->result_budget_rows == state->result_policy.target_chunk_rows)
	{
		state->result_budget_bytes = 0;
		state->result_budget_rows = 0;
	}
	if (row_charge > state->result_policy.result_buffer_limit ||
		state->result_budget_bytes >
		state->result_policy.result_buffer_limit - row_charge)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("embedded result exceeds result_buffer_limit"),
				 errdetail("The next encoded row requires %zu bytes; %zu of %zu bytes are already budgeted.",
						   row_charge, state->result_budget_bytes,
						   state->result_policy.result_buffer_limit)));
	state->result_budget_bytes += row_charge;
	state->result_budget_rows++;
	return;

malformed:
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("malformed embedded DataRow result")));
}


int
postgamma_backend_transport_getsockname(
	int socket_descriptor, struct sockaddr *address,
	socklen_t *address_length)
{
	struct sockaddr_un synthetic_address;
	socklen_t	required = (socklen_t) offsetof(
		struct sockaddr_un, sun_path) + 1;

	if (!postgamma_backend_transport_is_bound())
		return getsockname(socket_descriptor, address, address_length);
	if (address == NULL || address_length == NULL ||
		*address_length < required)
	{
		errno = EINVAL;
		return -1;
	}
	memset(&synthetic_address, 0, sizeof(synthetic_address));
	synthetic_address.sun_family = AF_UNIX;
	memcpy(address, &synthetic_address, required);
	*address_length = required;
	return 0;
}


bool
postgamma_locale_environment_is_private(void)
{
	return PostgammaCurrentBackendSession != NULL ||
		postgamma_postmaster_control_runtime_is_bound();
}


int
postgamma_backend_current_wake_fd(void)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;

	return state == NULL || state->wake_target == NULL ? -1 :
		postgamma_wake_target_fd(state->wake_target);
}


int
postgamma_backend_current_wake_drain(uint64_t *wake_count)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;

	if (wake_count == NULL)
		return EINVAL;
	*wake_count = 0;
	if (state == NULL || state->wake_target == NULL)
		return ENOENT;
	return postgamma_wake_target_drain(state->wake_target, wake_count);
}


ssize_t
postgamma_backend_stack_depth_limit(void)
{
	size_t		stack_limit;
	int			status = postgamma_thread_current_stack_limit(&stack_limit);

	if (status == ENOENT)
		return -1;
	if (status != 0)
		return STACK_DEPTH_SLOP;
	if (stack_limit > (size_t) SSIZE_MAX)
		return SSIZE_MAX;
	return (ssize_t) stack_limit;
}


void
postgamma_backend_log_telemetry(void)
{
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();
	PostgammaInstanceRuntimeTelemetry instance_telemetry = {0};
	PostgammaBackendTelemetry *telemetry = &instance_telemetry.backend;
	int			status = 0;

	status = runtime == NULL ? EPROTO :
		postgamma_instance_runtime_telemetry(runtime, &instance_telemetry);
	if (status != 0)
		elog(PANIC, "could not read PostGamma backend telemetry: %s",
			 strerror(status));
	elog(LOG,
		 "POSTGAMMA_RUNTIME backend_model=thread host_pid=%d "
		 "provider=%s "
		 "threads_started=%s role_process_launches=" UINT64_FORMAT " "
		 "forbidden_process_launch_attempts=" UINT64_FORMAT " "
		 "unsupported_role_requests=" UINT64_FORMAT " "
		 "client_threads_started=" UINT64_FORMAT " "
		 "client_threads_peak=" UINT64_FORMAT " "
		 "parallel_threads_started=" UINT64_FORMAT " "
		 "dedicated_threads_started=" UINT64_FORMAT " "
		 "role_completions=" UINT64_FORMAT " "
		 "role_threads_active=" UINT64_FORMAT " "
		 "pooled_worker_threads=" UINT64_FORMAT " "
		 "client_quantums=" UINT64_FORMAT " "
		 "quantum_yields=" UINT64_FORMAT " "
		 "carrier_migrations=" UINT64_FORMAT " "
		 "work_steals=" UINT64_FORMAT " "
		 "runnable_sessions=" UINT64_FORMAT " "
		 "runnable_sessions_peak=" UINT64_FORMAT " "
		 "running_quantums=" UINT64_FORMAT " "
		 "running_quantums_peak=" UINT64_FORMAT " "
		 "pinned_sessions=" UINT64_FORMAT " "
		 "pinned_sessions_peak=" UINT64_FORMAT " "
		 "blocked_sessions=" UINT64_FORMAT " "
		 "blocked_sessions_peak=" UINT64_FORMAT " "
		 "execution_tokens_active=" UINT64_FORMAT " "
		 "execution_tokens_peak=" UINT64_FORMAT " "
		 "execution_token_budget=" UINT64_FORMAT " "
		 "execution_token_rejections=" UINT64_FORMAT " "
		 "queue_wait_ns_total=" UINT64_FORMAT " "
		 "queue_wait_ns_max=" UINT64_FORMAT,
		 (int) getpid(),
		 telemetry->provider_kind == POSTGAMMA_BACKEND_PROVIDER_POOLED ?
		 "pooled" : "dedicated",
		 telemetry->threads_started ? "true" : "false",
		 telemetry->backend_process_launches,
		 telemetry->forbidden_process_launch_attempts,
		 telemetry->unsupported_backend_requests,
		 telemetry->client_threads_started,
		 telemetry->client_threads_peak,
		 telemetry->parallel_threads_started,
		 telemetry->dedicated_threads_started,
		 telemetry->backend_completions,
		 telemetry->backend_threads_active,
		 telemetry->pooled_worker_threads,
		 telemetry->client_quantums,
		 telemetry->quantum_yields,
		 telemetry->carrier_migrations,
		 telemetry->work_steals,
		 telemetry->runnable_sessions,
		 telemetry->runnable_sessions_peak,
		 telemetry->running_quantums,
		 telemetry->running_quantums_peak,
		 telemetry->pinned_sessions,
		 telemetry->pinned_sessions_peak,
		 telemetry->blocked_sessions,
		 telemetry->blocked_sessions_peak,
		 telemetry->execution_tokens_active,
		 telemetry->execution_tokens_peak,
		 telemetry->execution_token_budget,
		 telemetry->execution_token_rejections,
		 telemetry->queue_wait_ns_total,
		 telemetry->queue_wait_ns_max);
	postgamma_log_backend_stack_telemetry(
		instance_telemetry.generation, "dedicated",
		&telemetry->stack[POSTGAMMA_BACKEND_CLASS_DEDICATED]);
	postgamma_log_backend_stack_telemetry(
		instance_telemetry.generation, "client",
		&telemetry->stack[POSTGAMMA_BACKEND_CLASS_CLIENT]);
	postgamma_log_backend_stack_telemetry(
		instance_telemetry.generation, "parallel",
		&telemetry->stack[POSTGAMMA_BACKEND_CLASS_PARALLEL]);
}


void
postgamma_embedded_emit_log(ErrorData *error_data)
{
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();
	PostgammaClientSession *session = PostgammaCurrentBackendSession;
	PostgammaKernelLogRecord record = POSTGAMMA_KERNEL_LOG_RECORD_INIT;
	PostgammaKernelResultPolicy policy = POSTGAMMA_KERNEL_RESULT_POLICY_INIT;

	if (runtime == NULL || error_data == NULL ||
		postgamma_instance_runtime_profile(runtime) !=
			POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
		return;
	record.virtual_backend_pid = PG_STATE(MyProcPid);
	record.connection_id = session != NULL ?
		session->connection_id : UINT64_C(0);
	if (session != NULL && postgamma_backend_transport_is_bound() &&
		session->transport_ops.get_result_policy(
			session->transport, session->transport_generation, &policy) == 0)
		record.request_id = policy.request_generation;
	record.severity = error_severity(error_data->elevel);
	record.sqlstate = unpack_sql_state(error_data->sqlerrcode);
	record.message = error_data->message;
	record.detail = error_data->detail_log != NULL ?
		error_data->detail_log : error_data->detail;
	(void) postgamma_instance_runtime_emit_log(runtime, &record);
}


static void
postgamma_log_backend_stack_telemetry(
	uint64_t generation, const char *class_name,
	const PostgammaBackendStackTelemetry *stack)
{
	elog(LOG,
		 "POSTGAMMA_STACK generation=" UINT64_FORMAT " class=%s "
		 "observations=" UINT64_FORMAT " "
		 "observation_failures=" UINT64_FORMAT " "
		 "depth_validations=" UINT64_FORMAT " "
		 "depth_validation_failures=" UINT64_FORMAT " "
		 "accounting_failures=" UINT64_FORMAT " "
		 "active_reservation_bytes=" UINT64_FORMAT " "
		 "peak_reservation_bytes=" UINT64_FORMAT " "
		 "configured_stack_min=%zu configured_stack_max=%zu "
		 "configured_guard_min=%zu "
		 "native_stack_min=%zu "
		 "native_guard_min=%zu usable_stack_min=%zu "
		 "depth_limit_min=%zu configured_depth_max=%zu",
		 generation, class_name, stack->observations,
		 stack->observation_failures,
		 stack->depth_validations, stack->depth_validation_failures,
		 stack->accounting_failures,
		 stack->active_reservation_bytes, stack->peak_reservation_bytes,
		 stack->minimum_configured_stack_size,
		 stack->maximum_configured_stack_size,
		 stack->minimum_configured_guard_size,
		 stack->minimum_native_stack_size,
		 stack->minimum_native_guard_size,
		 stack->minimum_usable_stack_size,
		 stack->minimum_depth_limit,
		 stack->maximum_configured_depth);
}


bool
postgamma_backend_pqsignal(int signal_number, pqsigfunc handler)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();

	if (state == NULL)
	{
		if (PostgammaCurrentStandaloneInput != NULL)
		{
			if (signal_number <= 0 ||
				signal_number > POSTGAMMA_BACKEND_SIGNAL_COUNT)
				postgamma_runtime_contract_violation();
			PostgammaStandaloneSignals.signal_handlers[signal_number] = handler;
			return true;
		}
		return postgamma_postmaster_control_runtime_is_bound() ||
			(runtime != NULL &&
			 postgamma_instance_runtime_profile(runtime) ==
				 POSTGAMMA_RUNTIME_PROFILE_EMBEDDED);
	}
	if (signal_number <= 0 ||
		signal_number > POSTGAMMA_BACKEND_SIGNAL_COUNT)
		postgamma_runtime_contract_violation();
	state->signal_handlers[signal_number] = handler;
	return true;
}


int
postgamma_bootstrap_input_bind(const char *input, size_t length)
{
	if (input == NULL || length == 0 || length > INT_MAX ||
		PostgammaCurrentBootstrapInput != NULL)
		return EINVAL;
	PostgammaCurrentBootstrapInput = input;
	PostgammaCurrentBootstrapInputLength = length;
	return 0;
}


int
postgamma_bootstrap_input_unbind(const char *expected_input)
{
	if (expected_input == NULL ||
		PostgammaCurrentBootstrapInput != expected_input)
		return EINVAL;
	PostgammaCurrentBootstrapInput = NULL;
	PostgammaCurrentBootstrapInputLength = 0;
	return 0;
}


int
postgamma_bootstrap_scanner_bind(void *scanner)
{
	extern void *boot_yy_scan_bytes(const char *bytes, int length,
									 void *scanner);

	if (PostgammaCurrentBootstrapInput == NULL)
		return 0;
	if (scanner == NULL || PostgammaCurrentBootstrapInputLength > INT_MAX)
		return EINVAL;
	return boot_yy_scan_bytes(
		PostgammaCurrentBootstrapInput,
		(int) PostgammaCurrentBootstrapInputLength,
		scanner) == NULL ? ENOMEM : 0;
}


int
postgamma_standalone_input_bind(const char *input, size_t length)
{
	if (input == NULL || length == 0 ||
		PostgammaCurrentStandaloneInput != NULL)
		return EINVAL;
	PostgammaCurrentStandaloneInput = input;
	PostgammaCurrentStandaloneInputLength = length;
	PostgammaCurrentStandaloneInputOffset = 0;
	memset(&PostgammaStandaloneSignals, 0,
		   sizeof(PostgammaStandaloneSignals));
	sigemptyset(&PostgammaStandaloneSignals.logical_signal_mask);
	return 0;
}


int
postgamma_standalone_input_unbind(const char *expected_input)
{
	if (expected_input == NULL ||
		PostgammaCurrentStandaloneInput != expected_input)
		return EINVAL;
	PostgammaCurrentStandaloneInput = NULL;
	PostgammaCurrentStandaloneInputLength = 0;
	PostgammaCurrentStandaloneInputOffset = 0;
	memset(&PostgammaStandaloneSignals, 0,
		   sizeof(PostgammaStandaloneSignals));
	return 0;
}


bool
postgamma_standalone_input_getc(int *result)
{
	if (PostgammaCurrentStandaloneInput == NULL)
		return false;
	if (result == NULL)
		postgamma_runtime_contract_violation();
	if (PostgammaCurrentStandaloneInputOffset >=
		PostgammaCurrentStandaloneInputLength)
		*result = EOF;
	else
		*result = (unsigned char)
			PostgammaCurrentStandaloneInput[
				PostgammaCurrentStandaloneInputOffset++];
	return true;
}


static bool
postgamma_embedded_profile_active(void)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	PostgammaInstanceRuntime *runtime;

	if (execution == NULL || execution->instance == NULL)
		return false;
	runtime = postgamma_instance_context_runtime(execution->instance);
	return runtime != NULL &&
		postgamma_instance_runtime_profile(runtime) ==
		POSTGAMMA_RUNTIME_PROFILE_EMBEDDED;
}


static const PostgammaStaticModuleProvider *
postgamma_current_static_module_provider(void)
{
	const PostgammaStaticModuleProvider *provider;
	int			status;

	status = pthread_mutex_lock(&PostgammaStaticModuleProviderMutex);
	if (status != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not lock the static module provider: %s",
						strerror(status))));
	provider = PostgammaInstalledModuleProvider;
	status = pthread_mutex_unlock(&PostgammaStaticModuleProviderMutex);
	if (status != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not unlock the static module provider: %s",
						strerror(status))));
	return provider;
}


static void
postgamma_validate_static_module(
	const PostgammaStaticModuleProvider *provider,
	const void *handle, const char *filename)
{
	static const Pg_abi_values expected_abi = PG_MODULE_ABI_DATA;
	PGModuleMagicFunction magic_function;
	const Pg_magic_struct *magic;

	magic_function = (PGModuleMagicFunction) provider->symbol(
		provider->context, handle, PG_MAGIC_FUNCTION_NAME_STRING);
	if (magic_function == NULL)
		ereport(ERROR,
				(errmsg("incompatible bundled module \"%s\": missing magic block",
						filename)));
	magic = magic_function();
	if (magic == NULL || magic->len != sizeof(*magic) ||
		memcmp(&magic->abi_fields, &expected_abi, sizeof(expected_abi)) != 0)
		ereport(ERROR,
				(errmsg("incompatible bundled module \"%s\": ABI mismatch",
						filename)));
}


static void
postgamma_initialize_static_module(
	const PostgammaStaticModuleProvider *provider,
	const void *handle, const char *filename)
{
	typedef void (*PostgammaModuleInitFunction) (void);
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();
	uint64_t	generation =
		postgamma_instance_runtime_generation(runtime);
	uint64_t   *initialized_generation = state != NULL ?
		&state->initialized_module_generation :
		&PostgammaCarrierInitializedModuleGeneration;
	const void **initialized_handles = state != NULL ?
		state->initialized_module_handles :
		PostgammaCarrierInitializedModuleHandles;
	size_t	   *initialized_count = state != NULL ?
		&state->initialized_module_count :
		&PostgammaCarrierInitializedModuleCount;
	PostgammaModuleInitFunction initialize;
	size_t		index;

	if (generation == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bundled module \"%s\" has no instance generation",
						filename)));
	if (*initialized_generation != generation)
	{
		*initialized_generation = generation;
		*initialized_count = 0;
		memset(initialized_handles, 0,
			POSTGAMMA_STATIC_MODULES_PER_ROLE * sizeof(*initialized_handles));
	}
	for (index = 0; index < *initialized_count; index++)
	{
		if (initialized_handles[index] == handle)
			return;
	}
	if (*initialized_count >=
		POSTGAMMA_STATIC_MODULES_PER_ROLE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many bundled modules loaded in one embedded role")));
	initialize = (PostgammaModuleInitFunction) provider->symbol(
		provider->context, handle, "_PG_init");
	if (initialize != NULL)
		initialize();
	initialized_handles[(*initialized_count)++] = handle;
}


static const void *
postgamma_open_static_module(
	const char *filename,
	const PostgammaStaticModuleProvider **provider_result)
{
	const PostgammaStaticModuleProvider *provider;
	const void *handle = NULL;

	provider = postgamma_current_static_module_provider();
	if (provider == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("native modules are unavailable in this embedded build")));
	if (filename == NULL ||
		!provider->open(provider->context, filename, &handle) ||
		handle == NULL || !provider->owns(provider->context, handle))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("native module \"%s\" is not bundled for embedded mode",
						filename != NULL ? filename : "")));
	postgamma_validate_static_module(provider, handle, filename);
	postgamma_initialize_static_module(provider, handle, filename);
	if (provider_result != NULL)
		*provider_result = provider;
	return handle;
}


bool
postgamma_embedded_skip_system_collation_import(void)
{
	return postgamma_embedded_profile_active();
}


int
postgamma_static_module_provider_install(
	const PostgammaStaticModuleProvider *provider)
{
	int			status;
	int			unlock_status;

	if (provider == NULL || provider->struct_size != sizeof(*provider) ||
		provider->abi_tag != POSTGAMMA_STATIC_MODULE_PROVIDER_ABI_TAG ||
		provider->open == NULL || provider->owns == NULL ||
		provider->symbol == NULL || provider->register_extension == NULL ||
		provider->bundled_extension_count == NULL ||
		provider->bundled_extension_name == NULL)
		return EINVAL;
	status = pthread_mutex_lock(&PostgammaStaticModuleProviderMutex);
	if (status != 0)
		return status;
	if (PostgammaInstalledModuleProvider == NULL)
		PostgammaInstalledModuleProvider = provider;
	else if (PostgammaInstalledModuleProvider != provider)
		status = EBUSY;
	unlock_status = pthread_mutex_unlock(&PostgammaStaticModuleProviderMutex);
	return status != 0 ? status : unlock_status;
}


void
postgamma_load_bundled_extensions(void)
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();
	PostgammaInstanceRuntime *runtime;
	const PostgammaStaticModuleProvider *provider;
	size_t		count;
	size_t		index;
	int			status;

	if (!postgamma_embedded_profile_active() || execution == NULL ||
		execution->instance == NULL)
		return;
	runtime = postgamma_instance_context_runtime(execution->instance);
	if (runtime == NULL)
		return;
	provider = postgamma_current_static_module_provider();
	if (provider == NULL)
		return;
	count = provider->bundled_extension_count(provider->context);
	if (count == 0)
		return;
	if (postgamma_postmaster_control_runtime_is_bound())
	{
		status = postgamma_instance_runtime_activate_bundled_extensions(runtime);
		if (status != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not activate bundled extensions: %s",
						 strerror(status))));
	}
	if (!postgamma_instance_runtime_bundled_extensions_active(runtime))
	{
		/*
		 * Cluster creation runs PostgreSQL bootstrap/standalone phases before
		 * the server supervisor exists.  Those roles intentionally do not load
		 * bundled extensions; every server role must observe activation.
		 */
		if (PostgammaCurrentBootstrapInput != NULL ||
			PostgammaCurrentStandaloneInput != NULL)
			return;
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bundled extensions were not activated before role startup")));
	}
	for (index = 0; index < count; index++)
	{
		const char *name = provider->bundled_extension_name(
			provider->context, index);

		if (name == NULL || name[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("bundled extension inventory is incomplete")));
		(void) postgamma_open_static_module(name, NULL);
	}
}


int
postgamma_static_module_register_extension(
	const pgmex_descriptor *descriptor)
{
	const PostgammaStaticModuleProvider *provider =
		postgamma_current_static_module_provider();

	if (provider == NULL || provider->register_extension == NULL)
		return ENOTSUP;
	return provider->register_extension(provider->context, descriptor);
}


bool
postgamma_embedded_load_external_function(
	const char *filename, const char *function_name, bool signal_not_found,
	void **file_handle, void **result)
{
	const PostgammaStaticModuleProvider *provider;
	const void *handle;
	void	   *function;

	if (!postgamma_embedded_profile_active())
		return false;
	if (filename == NULL || function_name == NULL || result == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("invalid static module lookup request")));
	handle = postgamma_open_static_module(filename, &provider);
	function = provider->symbol(
		provider->context, handle, function_name);
	if (function == NULL && signal_not_found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not find function \"%s\" in bundled module \"%s\"",
						function_name, filename)));
	if (file_handle != NULL)
		*file_handle = (void *) handle;
	*result = function;
	return true;
}


bool
postgamma_embedded_lookup_external_function(
	void *file_handle, const char *function_name, void **result)
{
	const PostgammaStaticModuleProvider *provider;

	if (!postgamma_embedded_profile_active())
		return false;
	if (file_handle == NULL || function_name == NULL || result == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("invalid bundled module handle")));
	provider = postgamma_current_static_module_provider();
	if (provider == NULL ||
		!provider->owns(provider->context, file_handle))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dynamic module handles are disabled in embedded mode")));
	*result = provider->symbol(
		provider->context, file_handle, function_name);
	return true;
}


bool
postgamma_embedded_load_file(const char *filename, bool restricted)
{
	const PostgammaStaticModuleProvider *provider;

	/*
	 * Both restricted and unrestricted requests use the same fail-closed
	 * product registry.  The embedded profile never opens a filesystem module.
	 */
	(void) restricted;
	if (!postgamma_embedded_profile_active())
		return false;
	(void) postgamma_open_static_module(filename, &provider);
	return true;
}


int
postgamma_backend_printf(const char *format, ...)
{
	va_list		arguments;
	int			result;

	if (format == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	if (PostgammaCurrentBootstrapInput != NULL ||
		PostgammaCurrentStandaloneInput != NULL)
		return 0;
	va_start(arguments, format);
	result = vprintf(format, arguments);
	va_end(arguments);
	return result;
}


void
postgamma_dispatch_pending_signals(void)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	uint64_t	pending;
	uint64_t	blocked = 0;
	pg_signal_info signal_info;
	int			status;

	if (state == NULL)
	{
		PostgammaStandaloneSignalState *standalone =
			&PostgammaStandaloneSignals;

		if (PostgammaCurrentStandaloneInput == NULL ||
			standalone->dispatching_signals)
			return;
		pending = standalone->pending_signals;
		standalone->pending_signals = 0;
		if (postgamma_virtual_timer_due(&standalone->timer))
			pending |= UINT64_C(1) << (SIGALRM - 1);
		if (pending == 0)
			return;
		standalone->dispatching_signals = true;
		signal_info.pid = getpid();
		signal_info.uid = geteuid();
		for (int signal_number = 1;
			 signal_number <= POSTGAMMA_BACKEND_SIGNAL_COUNT;
			 signal_number++)
		{
			uint64_t bit = UINT64_C(1) << (signal_number - 1);
			pqsigfunc handler;

			if ((pending & bit) == 0)
				continue;
			if (sigismember(
					&standalone->logical_signal_mask, signal_number) == 1)
			{
				standalone->pending_signals |= bit;
				continue;
			}
			handler = standalone->signal_handlers[signal_number];
			if (handler == NULL || handler == PG_SIG_IGN ||
				handler == PG_SIG_DFL)
				continue;
			handler(signal_number, &signal_info);
		}
		standalone->dispatching_signals = false;
		return;
	}
	if (state->dispatching_signals)
		return;
	status = postgamma_backend_take_pending_signals(
		state->registry, state->handle.id, &pending);
	if (status == ESTALE)
		return;
	if (status != 0)
		postgamma_runtime_contract_violation();
	if (postgamma_virtual_timer_due(&state->timer))
		pending |= UINT64_C(1) << (SIGALRM - 1);
	status = postgamma_backend_wait_while_paused(
		state->registry, state->handle.id);
	if (status == ESTALE)
		return;
	if (status != 0)
		postgamma_runtime_contract_violation();
	state->dispatching_signals = true;
	signal_info.pid = PG_STATE(PostmasterPid);
	signal_info.uid = geteuid();
	for (int signal_number = 1;
		 signal_number <= POSTGAMMA_BACKEND_SIGNAL_COUNT;
		 signal_number++)
	{
		uint64_t	bit = UINT64_C(1) << (signal_number - 1);
		pqsigfunc	handler;

		if ((pending & bit) == 0)
			continue;
		if (sigismember(&state->logical_signal_mask, signal_number) == 1)
		{
			blocked |= bit;
			continue;
		}
		if (signal_number == SIGQUIT || signal_number == SIGKILL)
		{
			state->dispatching_signals = false;
			postgamma_set_exit_status(
				POSTGAMMA_BACKEND_EXIT_QUICKDIE, signal_number);
		}
		handler = state->signal_handlers[signal_number];
		if (handler == PG_SIG_IGN)
			continue;
		if (handler == NULL || handler == PG_SIG_DFL)
		{
			state->dispatching_signals = false;
			postgamma_set_exit_status(POSTGAMMA_BACKEND_EXIT_FATAL, 1);
		}
		handler(signal_number, &signal_info);
	}
	state->dispatching_signals = false;
	if (blocked != 0 &&
		postgamma_backend_requeue_pending_signals(
			state->registry, state->handle.id, blocked) != 0)
		postgamma_runtime_contract_violation();
}


#ifndef WIN32
char *
postgamma_backend_setlocale(int category, const char *locale)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;

	if (state == NULL)
	{
		if (postgamma_postmaster_control_runtime_is_bound())
			return postgamma_postmaster_control_current_setlocale(
				category, locale);
		return setlocale(category, locale);
	}
#ifdef HAVE_USELOCALE
	{
		locale_t	base;
		locale_t	updated;
		locale_t	old_locale;
		const char *resolved;
		int			category_index;
		int			category_mask;
		int			saved_errno;

		category_index = postgamma_locale_category_index(category);
		category_mask = postgamma_locale_category_mask(category);
		if (category_index < 0 || category_mask == 0)
		{
			errno = EINVAL;
			return NULL;
		}
		postgamma_initialize_locale_names(state);
		if (locale == NULL)
			return state->locale_names[category_index];

		old_locale = state->session_locale;
		base = duplocale(old_locale != (locale_t) 0 ?
						 old_locale : LC_GLOBAL_LOCALE);
		if (base == (locale_t) 0)
			return NULL;
		updated = newlocale(category_mask, locale, base);
		if (updated == (locale_t) 0)
		{
			saved_errno = errno;
			freelocale(base);
			errno = saved_errno;
			return NULL;
		}
		if (uselocale(updated) == (locale_t) 0)
		{
			saved_errno = errno;
			freelocale(updated);
			errno = saved_errno;
			return NULL;
		}
		state->session_locale = updated;
		if (old_locale != (locale_t) 0)
			freelocale(old_locale);

		resolved = postgamma_effective_locale_name(category, locale);
		strlcpy(state->locale_names[category_index], resolved,
				sizeof(state->locale_names[category_index]));
		return state->locale_names[category_index];
	}
#else
	errno = ENOTSUP;
	return NULL;
#endif
}


int
postgamma_backend_setitimer(int which, const struct itimerval *new_value,
							struct itimerval *old_value)
{
	PostgammaVirtualTimer *timer = postgamma_current_virtual_timer();
	TimestampTz now;
	int64		value_us;
	int64		interval_us;

	if (timer == NULL)
	{
		if (postgamma_embedded_profile_active() ||
			postgamma_postmaster_control_runtime_is_bound())
		{
			errno = ENOTSUP;
			return -1;
		}
		return setitimer(which, new_value, old_value);
	}
	if (which != ITIMER_REAL || new_value == NULL ||
		new_value->it_value.tv_sec < 0 ||
		new_value->it_value.tv_usec < 0 ||
		new_value->it_value.tv_usec >= USECS_PER_SEC ||
		new_value->it_interval.tv_sec < 0 ||
		new_value->it_interval.tv_usec < 0 ||
		new_value->it_interval.tv_usec >= USECS_PER_SEC ||
		new_value->it_value.tv_sec >
		(PG_INT64_MAX - new_value->it_value.tv_usec) / USECS_PER_SEC ||
		new_value->it_interval.tv_sec >
		(PG_INT64_MAX - new_value->it_interval.tv_usec) / USECS_PER_SEC)
	{
		errno = EINVAL;
		return -1;
	}
	now = GetCurrentTimestamp();
	if (old_value != NULL)
	{
		int64		remaining_us = 0;

		MemSet(old_value, 0, sizeof(*old_value));
		if (timer->timeout_armed && timer->timeout_due_at > now)
			remaining_us = timer->timeout_due_at - now;
		old_value->it_value.tv_sec = remaining_us / USECS_PER_SEC;
		old_value->it_value.tv_usec = remaining_us % USECS_PER_SEC;
		old_value->it_interval.tv_sec =
			timer->timeout_interval_us / USECS_PER_SEC;
		old_value->it_interval.tv_usec =
			timer->timeout_interval_us % USECS_PER_SEC;
	}
	value_us = (int64) new_value->it_value.tv_sec * USECS_PER_SEC +
		new_value->it_value.tv_usec;
	interval_us = (int64) new_value->it_interval.tv_sec * USECS_PER_SEC +
		new_value->it_interval.tv_usec;
	if (value_us == 0)
	{
		timer->timeout_wait_shortened = false;
		timer->timeout_interval_us = interval_us;
		timer->timeout_armed = false;
		timer->timeout_due_at = 0;
		return 0;
	}
	if (now > PG_INT64_MAX - value_us)
	{
		errno = EOVERFLOW;
		return -1;
	}
	timer->timeout_wait_shortened = false;
	timer->timeout_interval_us = interval_us;
	timer->timeout_due_at = now + value_us;
	timer->timeout_armed = true;
	return 0;
}
#endif


long
postgamma_backend_adjust_wait_timeout(long requested_timeout)
{
	PostgammaVirtualTimer *timer = postgamma_current_virtual_timer();
	TimestampTz now;
	int64		remaining_us;
	int64		remaining_ms;

	if (timer == NULL)
		return requested_timeout;
	if (!timer->timeout_armed)
	{
		timer->timeout_wait_shortened = false;
		return requested_timeout;
	}
	now = GetCurrentTimestamp();
	remaining_us = timer->timeout_due_at - now;
	if (remaining_us <= 0)
		remaining_ms = 0;
	else
		remaining_ms = (remaining_us + 999) / 1000;
	if (remaining_ms > LONG_MAX)
		remaining_ms = LONG_MAX;
	if (requested_timeout < 0 || remaining_ms < requested_timeout)
	{
		timer->timeout_wait_shortened = true;
		return (long) remaining_ms;
	}
	timer->timeout_wait_shortened = false;
	return requested_timeout;
}


bool
postgamma_backend_timeout_wait_expired(int wait_result)
{
	PostgammaVirtualTimer *timer = postgamma_current_virtual_timer();
	bool		shortened;

	if (timer == NULL)
		return false;
	shortened = timer->timeout_wait_shortened;
	timer->timeout_wait_shortened = false;
	return wait_result == -1 && shortened;
}


bool
postgamma_postgres_quantum_begin(
	volatile bool *send_ready_for_query,
	volatile bool *idle_in_transaction_timeout_enabled,
	volatile bool *idle_session_timeout_enabled)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;

	if (send_ready_for_query == NULL ||
		idle_in_transaction_timeout_enabled == NULL ||
		idle_session_timeout_enabled == NULL)
		postgamma_runtime_contract_violation();
	if (state == NULL || !state->postgres_main_initialized ||
		state->handle.execution_class != POSTGAMMA_BACKEND_CLASS_CLIENT ||
		postgamma_backend_registry_provider_kind(state->registry) !=
			POSTGAMMA_BACKEND_PROVIDER_POOLED)
		return false;
	*send_ready_for_query = state->saved_send_ready_for_query;
	*idle_in_transaction_timeout_enabled =
		state->saved_idle_in_transaction_timeout_enabled;
	*idle_session_timeout_enabled =
		state->saved_idle_session_timeout_enabled;
	return true;
}


void
postgamma_postgres_quantum_initialized(void)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;

	if (state != NULL &&
		state->handle.execution_class == POSTGAMMA_BACKEND_CLASS_CLIENT)
		state->postgres_main_initialized = true;
}


static bool
postgamma_quantum_is_pooled_client(const PostgammaClientSession *state)
{
	return state != NULL &&
		state->handle.execution_class == POSTGAMMA_BACKEND_CLASS_CLIENT &&
		postgamma_backend_registry_provider_kind(state->registry) ==
			POSTGAMMA_BACKEND_PROVIDER_POOLED;
}


static bool
postgamma_quantum_update_pin_state(PostgammaClientSession *state)
{
	uint32_t	pin_reasons = 0;
	bool		pinned;
	int			status;

	if (IsTransactionOrTransactionBlock())
		pin_reasons |= POSTGAMMA_KERNEL_PIN_TRANSACTION;
	if (postgamma_session_holds_advisory_lock())
		pin_reasons |= POSTGAMMA_KERNEL_PIN_ADVISORY_LOCK;
	pinned = pin_reasons != 0;
	status = postgamma_backend_set_pinned(
		state->registry, state->handle.id, pinned);
	if (status != 0)
		postgamma_runtime_contract_violation();
	status = state->transport_ops.set_session_status(
		state->transport, state->transport_generation, pin_reasons,
		pinned ? UINT32_C(1) : UINT32_C(0));
	if (status != 0)
		postgamma_runtime_contract_violation();
	if (pinned && postgamma_backend_set_deadline(
			state->registry, state->handle.id, 0) != 0)
		postgamma_runtime_contract_violation();
	return pinned;
}


static bool
postgamma_quantum_prepare_yield(
	PostgammaClientSession *state, bool yield_when_runnable)
{
	bool		runnable;
	uint32_t	transport_events;
	uint64_t	schedule_epoch;
	int			status;

	for (;;)
	{
		status = postgamma_backend_schedule_epoch(
			state->registry, state->handle.id, &schedule_epoch);
		if (status != 0)
			postgamma_runtime_contract_violation();
		transport_events = 0;
		status = state->transport_ops.ready(
			state->transport, state->transport_generation, &transport_events);
		if (status != 0)
			postgamma_runtime_contract_violation();
		runnable = (transport_events &
			(POSTGAMMA_KERNEL_TRANSPORT_READABLE |
			 POSTGAMMA_KERNEL_TRANSPORT_PEER_CLOSED)) != 0;
		if (runnable && !yield_when_runnable)
			return false;
		status = postgamma_backend_prepare_yield(
			state->registry, state->handle.id, schedule_epoch, runnable);
		if (status == 0)
			break;
		if (status != EAGAIN)
			postgamma_runtime_contract_violation();
	}
	PG_STATE(PG_exception_stack) = NULL;
	state->quantum_yielded = true;
	return true;
}


bool
postgamma_postgres_quantum_yield_ready(
	bool send_ready_for_query,
	bool idle_in_transaction_timeout_enabled,
	bool idle_session_timeout_enabled)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	uint64_t	deadline_ns = 0;
	int			status;

	if (!postgamma_quantum_is_pooled_client(state))
		return false;
	if (send_ready_for_query || PG_STATE(error_context_stack) != NULL ||
		state->quantum_yielded)
		postgamma_runtime_contract_violation();
	status = postgamma_backend_set_blocked(
		state->registry, state->handle.id, false);
	if (status != 0)
		postgamma_runtime_contract_violation();
	if (postgamma_quantum_update_pin_state(state))
		return false;
	state->saved_send_ready_for_query = send_ready_for_query;
	state->saved_idle_in_transaction_timeout_enabled =
		idle_in_transaction_timeout_enabled;
	state->saved_idle_session_timeout_enabled =
		idle_session_timeout_enabled;
	/*
	 * A pooled client normally relinquishes its carrier after ReadyForQuery.
	 * A second clean seam immediately before ReadCommand can release a carrier
	 * that was resumed only to process idle role-local work.  Do not schedule a
	 * carrier merely for a non-fatal idle timer such as
	 * IDLE_STATS_UPDATE_TIMEOUT; it can wait until the next real resume.
	 *
	 * IdleSessionTimeout is different because its handler terminates the
	 * logical session.  It is armed immediately before this yield point, so a
	 * monotonic deadline derived here preserves that product-visible timeout
	 * without exposing the timeout.c implementation to the executor.
	 */
	if (idle_session_timeout_enabled)
	{
		uint64_t monotonic_now = postgamma_monotonic_now_ns();
		uint64_t timeout_ns;
		int idle_session_timeout =
			POSTGAMMA_GUC_VALUE(IdleSessionTimeout);

		if (idle_session_timeout <= 0)
			postgamma_runtime_contract_violation();
		timeout_ns = (uint64_t) idle_session_timeout >
			UINT64_MAX / UINT64_C(1000000) ?
			UINT64_MAX :
			(uint64_t) idle_session_timeout * UINT64_C(1000000);
		deadline_ns = timeout_ns > UINT64_MAX - monotonic_now ?
			UINT64_MAX : monotonic_now + timeout_ns;
	}
	state->saved_idle_session_deadline_ns = deadline_ns;
	status = postgamma_backend_set_deadline(
		state->registry, state->handle.id, deadline_ns);
	if (status != 0)
		postgamma_runtime_contract_violation();
	return postgamma_quantum_prepare_yield(state, true);
}


bool
postgamma_postgres_quantum_yield_command_read(
	bool send_ready_for_query,
	bool idle_in_transaction_timeout_enabled,
	bool idle_session_timeout_enabled)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	uint64_t	deadline_ns;
	int			status;

	if (!postgamma_quantum_is_pooled_client(state))
		return false;
	if (send_ready_for_query)
		postgamma_runtime_contract_violation();
	if (PG_STATE(error_context_stack) != NULL || state->quantum_yielded)
		postgamma_runtime_contract_violation();
	if (PG_STATE(InterruptHoldoffCount) != 0)
		postgamma_runtime_contract_violation();
	if (PG_STATE(QueryCancelHoldoffCount) != 0)
		postgamma_runtime_contract_violation();
	if (PG_STATE(CritSectionCount) != 0)
		postgamma_runtime_contract_violation();
	if (pq_is_reading_msg())
		postgamma_runtime_contract_violation();

	/*
	 * Dispatch role-local signals before deciding whether this clean protocol
	 * boundary is idle.  LISTEN notifications flush here while no frontend
	 * message has started, so returning from PostgresMain cannot discard
	 * partially consumed protocol state.
	 */
	ProcessClientReadInterrupt(false);
	if (PG_STATE(error_context_stack) != NULL || pq_is_reading_msg())
		postgamma_runtime_contract_violation();
	/*
	 * secure_read() may have filled PostgreSQL's receive buffer with more than
	 * one complete frontend message.  Those bytes are runnable work even when
	 * the memory transport itself is empty; yielding here would strand a Sync
	 * message and leave an extended-protocol request permanently busy.
	 */
	if (pq_buffer_remaining_data() > 0)
		return false;
	status = postgamma_backend_set_blocked(
		state->registry, state->handle.id, false);
	if (status != 0)
		postgamma_runtime_contract_violation();
	if (postgamma_quantum_update_pin_state(state))
		return false;
	state->saved_send_ready_for_query = send_ready_for_query;
	state->saved_idle_in_transaction_timeout_enabled =
		idle_in_transaction_timeout_enabled;
	state->saved_idle_session_timeout_enabled =
		idle_session_timeout_enabled;
	deadline_ns = idle_session_timeout_enabled ?
		state->saved_idle_session_deadline_ns : 0;
	if (idle_session_timeout_enabled && deadline_ns == 0)
		postgamma_runtime_contract_violation();
	if (!idle_session_timeout_enabled)
		state->saved_idle_session_deadline_ns = 0;
	status = postgamma_backend_set_deadline(
		state->registry, state->handle.id, deadline_ns);
	if (status != 0)
		postgamma_runtime_contract_violation();
	return postgamma_quantum_prepare_yield(state, false);
}


void
postgamma_postgres_blocking_begin(void)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;

	if (state != NULL &&
		state->handle.execution_class == POSTGAMMA_BACKEND_CLASS_CLIENT &&
		postgamma_backend_set_blocked(
			state->registry, state->handle.id, true) != 0)
		postgamma_runtime_contract_violation();
}


void
postgamma_postgres_blocking_end(void)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;

	if (state != NULL &&
		state->handle.execution_class == POSTGAMMA_BACKEND_CLASS_CLIENT &&
		postgamma_backend_set_blocked(
			state->registry, state->handle.id, false) != 0)
		postgamma_runtime_contract_violation();
}


static bool
postgamma_session_holds_advisory_lock(void)
{
	HTAB	   *local_locks = GetLockMethodLocalHash();
	HASH_SEQ_STATUS scan;
	LOCALLOCK  *lock;

	if (local_locks == NULL)
		return false;
	hash_seq_init(&scan, local_locks);
	while ((lock = hash_seq_search(&scan)) != NULL)
	{
		if (lock->nLocks <= 0 ||
			LOCALLOCK_LOCKMETHOD(*lock) != USER_LOCKMETHOD)
			continue;
		for (int index = 0; index < lock->numLockOwners; index++)
		{
			if (lock->lockOwners[index].owner == NULL &&
				lock->lockOwners[index].nLocks > 0)
			{
				hash_seq_term(&scan);
				return true;
			}
		}
	}
	return false;
}


int
postgamma_sigprocmask(int how, const sigset_t *set, sigset_t *old_set)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	sigset_t   *logical_mask = NULL;
	bool		dispatching = false;
	int			status;

	if (state == NULL)
	{
		if (PostgammaCurrentStandaloneInput != NULL)
		{
			logical_mask = &PostgammaStandaloneSignals.logical_signal_mask;
			dispatching = PostgammaStandaloneSignals.dispatching_signals;
		}
		else
		{
			status = postgamma_postmaster_control_current_sigprocmask(
				how, set, old_set);
			if (status == ENOENT)
				return sigprocmask(how, set, old_set);
			if (status == 0)
				return 0;
			errno = status;
			return -1;
		}
	}
	else
	{
		logical_mask = &state->logical_signal_mask;
		dispatching = state->dispatching_signals;
	}
	if (old_set != NULL)
		*old_set = *logical_mask;
	if (set == NULL)
		return 0;
	switch (how)
	{
		case SIG_BLOCK:
			for (int signal_number = 1;
				 signal_number <= POSTGAMMA_BACKEND_SIGNAL_COUNT;
				 signal_number++)
			{
				if (sigismember(set, signal_number) == 1)
					sigaddset(logical_mask, signal_number);
			}
			break;
		case SIG_UNBLOCK:
			for (int signal_number = 1;
				 signal_number <= POSTGAMMA_BACKEND_SIGNAL_COUNT;
				 signal_number++)
			{
				if (sigismember(set, signal_number) == 1)
					sigdelset(logical_mask, signal_number);
			}
			break;
		case SIG_SETMASK:
			*logical_mask = *set;
			break;
		default:
			errno = EINVAL;
			return -1;
	}
	if (!dispatching)
		postgamma_dispatch_pending_signals();
	return 0;
}


int
postgamma_threaded_server_completion_notify(
	void *argument, uint64_t generation)
{
	(void) argument;
	(void) generation;
	if (kill(getpid(), SIGCHLD) != 0)
		return errno != 0 ? errno : EIO;
	return 0;
}


pid_t
postgamma_getpid(void)
{
	if (PostgammaCurrentBackendSession != NULL)
		return (pid_t) PostgammaCurrentBackendSession->handle.compat_pid;
	return getpid();
}


int
postgamma_kill(pid_t pid, int signal_number)
{
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();
	PostgammaBackendRegistry *registry = postgamma_current_backend_registry();
	PostgammaCompatPid compat_pid;
	int			status;

	if (!postgamma_decode_compat_pid(pid, &compat_pid))
	{
		if (runtime != NULL && postgamma_instance_runtime_profile(runtime) ==
			POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
		{
			if (pid <= 0 || pid != PG_STATE(PostmasterPid))
			{
				errno = ENOTSUP;
				return -1;
			}
			if (signal_number == 0)
				return 0;
			status = postgamma_instance_runtime_notify_supervisor_signal(
				runtime, postgamma_instance_runtime_generation(runtime),
				signal_number);
			if (status == 0)
				return 0;
			errno = status;
			return -1;
		}
		return kill(pid, signal_number);
	}
	if (registry == NULL)
	{
		errno = ESRCH;
		return -1;
	}
	if (signal_number == SIGURG)
		status = postgamma_backend_wake(registry, compat_pid);
	else
		status = postgamma_backend_signal(registry, compat_pid, signal_number);
	if (status == 0)
		return 0;
	errno = status;
	return -1;
}


pid_t
postgamma_waitpid(pid_t pid, int *exit_status, int options)
{
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();
	PostgammaBackendRegistry *registry = postgamma_current_backend_registry();
	PostgammaBackendCompletion completion;
	PostgammaCompatPid compat_pid;
	uint64_t	external_child_count = 0;
	int			status;

	if (postgamma_decode_compat_pid(pid, &compat_pid))
	{
		(void) compat_pid;
		errno = ECHILD;
		return -1;
	}
	if (registry != NULL && pid == -1 && (options & WNOHANG) != 0)
	{
		status = postgamma_backend_completion_pop(registry, &completion);
		if (status == 0)
		{
			if (exit_status != NULL)
				*exit_status = postgamma_wait_status(completion.exit_status);
			status = postgamma_backend_completion_join(registry, &completion);
			if (status != 0)
			{
				errno = status;
				return -1;
			}
			return (pid_t) completion.handle.compat_pid;
		}
		if (status != EAGAIN)
		{
			errno = status;
			return -1;
		}
	}
	if (pid == -1 && (options & WNOHANG) != 0)
	{
		if (runtime != NULL)
		{
			status = postgamma_instance_runtime_external_process_count(
				runtime, &external_child_count);
			if (status != 0)
			{
				errno = status;
				return -1;
			}
		}
		if (external_child_count != 0)
			return 0;
	}
	if (runtime != NULL && postgamma_instance_runtime_profile(runtime) ==
		POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
	{
		if (pid == -1 && (options & WNOHANG) != 0)
			return 0;
		errno = ECHILD;
		return -1;
	}
	return waitpid(pid, exit_status, options);
}


int
postgamma_system(const char *command)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();
	posix_spawnattr_t attributes;
	char	   *const arguments[] = {
		(char *) "sh", (char *) "-c", (char *) command, NULL
	};
	pid_t		pid;
	int			result;
	int			status;
	struct timespec delay = {0, 10 * 1000 * 1000};

	if (runtime != NULL && postgamma_instance_runtime_profile(runtime) ==
		POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
	{
		errno = ENOTSUP;
		return -1;
	}
	if (state == NULL)
		return system(command);
	if (command == NULL)
		return access("/bin/sh", X_OK) == 0;
	if (state->system_child_pid != 0)
	{
		errno = EDEADLK;
		return -1;
	}
	result = postgamma_initialize_shell_spawn_attributes(&attributes, false);
	if (result != 0)
	{
		errno = result;
		return -1;
	}
	result = postgamma_instance_runtime_reserve_external_process(state->runtime);
	if (result != 0)
	{
		(void) posix_spawnattr_destroy(&attributes);
		errno = result;
		return -1;
	}

	/*
	 * libc system() changes SIGINT and SIGQUIT dispositions for the entire
	 * process while it waits.  A role thread must never mutate those host
	 * dispositions, so spawn the shell with explicit child-only attributes.
	 * Reserve the child slot before spawning so postmaster waitpid(-1,
	 * WNOHANG) cannot reap this role-owned child between spawn and registration.
	 */
	result = posix_spawn(&pid, "/bin/sh", NULL, &attributes, arguments, environ);
	if (result == 0)
	{
		int			commit_status =
			postgamma_instance_runtime_commit_external_process(state->runtime);

		if (commit_status != 0)
			postgamma_runtime_contract_violation();
		state->system_child_pid = pid;
	}
	else if (postgamma_instance_runtime_cancel_external_process(
				 state->runtime) != 0)
		postgamma_runtime_contract_violation();
	(void) posix_spawnattr_destroy(&attributes);
	if (result != 0)
	{
		errno = result;
		return -1;
	}

	for (;;)
	{
		result = waitpid(pid, &status, WNOHANG);
		if (result == pid)
			break;
		if (result < 0 && errno != EINTR)
		{
			status = -1;
			break;
		}
		postgamma_dispatch_pending_signals();
		(void) nanosleep(&delay, NULL);
	}
	postgamma_system_child_finished(state, pid);
	return status;
}


FILE *
postgamma_popen(const char *command, const char *mode)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();
	PostgammaExternalPipe *external_pipe = NULL;
	posix_spawn_file_actions_t file_actions;
	posix_spawnattr_t attributes;
	char	   *const arguments[] = {
		(char *) "sh", (char *) "-c", (char *) command, NULL
	};
	int			pipe_fds[2] = {-1, -1};
	int			parent_fd;
	int			child_fd;
	int			child_target;
	int			result;
	bool		read_mode;
	bool		file_actions_initialized = false;
	bool		attributes_initialized = false;
	bool		external_process_reserved = false;

	if (runtime != NULL && postgamma_instance_runtime_profile(runtime) ==
		POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
	{
		errno = ENOTSUP;
		return NULL;
	}
	if (state == NULL)
		return popen(command, mode);
	if (command == NULL || mode == NULL ||
		(mode[0] != 'r' && mode[0] != 'w') ||
		(mode[1] != '\0' && !(mode[1] == 'e' && mode[2] == '\0')))
	{
		errno = EINVAL;
		return NULL;
	}
	read_mode = mode[0] == 'r';
	if (pipe(pipe_fds) != 0)
		return NULL;
	for (int index = 0; index < 2; index++)
	{
		if (pipe_fds[index] <= STDERR_FILENO)
		{
			int			moved_fd = fcntl(pipe_fds[index], F_DUPFD,
									 STDERR_FILENO + 1);

			if (moved_fd < 0)
				goto fail;
			(void) close(pipe_fds[index]);
			pipe_fds[index] = moved_fd;
		}
		result = postgamma_set_close_on_exec(pipe_fds[index]);
		if (result != 0)
		{
			errno = result;
			goto fail;
		}
	}
	parent_fd = pipe_fds[read_mode ? 0 : 1];
	child_fd = pipe_fds[read_mode ? 1 : 0];
	child_target = read_mode ? STDOUT_FILENO : STDIN_FILENO;

	external_pipe = calloc(1, sizeof(*external_pipe));
	if (external_pipe == NULL)
		goto fail;
	external_pipe->stream = fdopen(parent_fd, read_mode ? "r" : "w");
	if (external_pipe->stream == NULL)
		goto fail;

	result = posix_spawn_file_actions_init(&file_actions);
	if (result != 0)
	{
		errno = result;
		goto fail;
	}
	file_actions_initialized = true;
	result = posix_spawn_file_actions_adddup2(
		&file_actions, child_fd, child_target);
	if (result == 0)
		result = posix_spawn_file_actions_addclose(&file_actions, pipe_fds[0]);
	if (result == 0)
		result = posix_spawn_file_actions_addclose(&file_actions, pipe_fds[1]);
	if (result != 0)
	{
		errno = result;
		goto fail;
	}
	result = postgamma_initialize_shell_spawn_attributes(&attributes, true);
	if (result != 0)
	{
		errno = result;
		goto fail;
	}
	attributes_initialized = true;
	result = postgamma_instance_runtime_reserve_external_process(state->runtime);
	if (result != 0)
	{
		errno = result;
		goto fail;
	}
	external_process_reserved = true;

	/*
	 * libc popen() hides its child PID, so the postmaster's process-wide
	 * waitpid(-1, WNOHANG) can reap a role-owned command before pclose().
	 * Reserve instance ownership before the spawn, then commit the PID to role
	 * state only after the child exists.
	 */
	result = posix_spawn(&external_pipe->pid, "/bin/sh", &file_actions,
						 &attributes, arguments, environ);
	if (result == 0)
	{
		int			commit_status =
			postgamma_instance_runtime_commit_external_process(state->runtime);

		if (commit_status != 0)
			postgamma_runtime_contract_violation();
		external_process_reserved = false;
		external_pipe->next = state->external_pipes;
		state->external_pipes = external_pipe;
	}
	else
	{
		if (postgamma_instance_runtime_cancel_external_process(
				state->runtime) != 0)
			postgamma_runtime_contract_violation();
		external_process_reserved = false;
	}
	(void) posix_spawnattr_destroy(&attributes);
	(void) posix_spawn_file_actions_destroy(&file_actions);
	attributes_initialized = false;
	file_actions_initialized = false;
	(void) close(child_fd);
	pipe_fds[read_mode ? 1 : 0] = -1;
	pipe_fds[read_mode ? 0 : 1] = -1;
	if (result != 0)
	{
		errno = result;
		goto fail;
	}
	return external_pipe->stream;

fail:
	{
		int			save_errno = errno;

		if (attributes_initialized)
			(void) posix_spawnattr_destroy(&attributes);
		if (file_actions_initialized)
			(void) posix_spawn_file_actions_destroy(&file_actions);
		if (external_process_reserved &&
			postgamma_instance_runtime_cancel_external_process(
				state->runtime) != 0)
			postgamma_runtime_contract_violation();
		if (external_pipe != NULL && external_pipe->stream != NULL)
		{
			(void) fclose(external_pipe->stream);
			pipe_fds[read_mode ? 0 : 1] = -1;
		}
		for (int index = 0; index < 2; index++)
		{
			if (pipe_fds[index] >= 0)
				(void) close(pipe_fds[index]);
		}
		free(external_pipe);
		errno = save_errno;
		return NULL;
	}
}


int
postgamma_pclose(FILE *stream)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();
	PostgammaExternalPipe **link;
	PostgammaExternalPipe *external_pipe;
	int			close_errno = 0;
	int			wait_errno = 0;
	int			status;

	if (state == NULL)
	{
		if (runtime != NULL && postgamma_instance_runtime_profile(runtime) ==
			POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
		{
			errno = ENOTSUP;
			return -1;
		}
		return pclose(stream);
	}
	for (link = &state->external_pipes; *link != NULL; link = &(*link)->next)
	{
		if ((*link)->stream == stream)
			break;
	}
	if (*link == NULL)
	{
		if (postgamma_instance_runtime_profile(state->runtime) ==
			POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
		{
			errno = EINVAL;
			return -1;
		}
		return pclose(stream);
	}
	external_pipe = *link;
	if (!external_pipe->stream_closed)
	{
		if (fclose(stream) != 0)
			close_errno = errno;
		external_pipe->stream_closed = true;
	}
	if (!external_pipe->child_reaped)
	{
		wait_errno = postgamma_wait_external_child(
			external_pipe->pid, &status);
		postgamma_external_pipe_finished(
			state, external_pipe, wait_errno == 0 ? status : -1);
	}
	status = external_pipe->wait_status;
	*link = external_pipe->next;
	free(external_pipe);
	if (close_errno != 0 || wait_errno != 0)
	{
		errno = close_errno != 0 ? close_errno : wait_errno;
		return -1;
	}
	return status;
}


int
postgamma_atexit(void (*function) (void))
{
	PostgammaExecutionContext *execution =
		postgamma_execution_context_current();

	if (PostgammaCurrentBackendSession != NULL ||
		(execution != NULL && execution->exit_handler != NULL))
		return 0;
	return atexit(function);
}


void
postgamma_backend_exit(int code)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	PostgammaBackendExitKind kind;

	if (state == NULL)
	{
		(void) postgamma_execution_context_dispatch_exit(code);
		return;
	}
	kind = code == 0 ? POSTGAMMA_BACKEND_EXIT_NORMAL :
		POSTGAMMA_BACKEND_EXIT_FATAL;
	if (state->backend_type == B_STARTUP && code == 3)
		kind = POSTGAMMA_BACKEND_EXIT_STARTUP_TARGET;
	postgamma_set_exit_status(kind, code);
}


void
postgamma_backend_panic(void)
{
	if (PostgammaCurrentBackendSession != NULL)
		postgamma_set_exit_status(POSTGAMMA_BACKEND_EXIT_PANIC, SIGABRT);
}


pg_noreturn void
postgamma_immediate_exit(int code)
{
	if (PostgammaCurrentBackendSession != NULL)
	{
		postgamma_set_exit_status(
			code == 2 ? POSTGAMMA_BACKEND_EXIT_QUICKDIE :
			POSTGAMMA_BACKEND_EXIT_FATAL,
			code);
	}
	(void) postgamma_execution_context_dispatch_exit(code);
	_exit(code);
}


pg_noreturn void
postgamma_abort(void)
{
	if (PostgammaCurrentBackendSession != NULL)
		postgamma_set_exit_status(POSTGAMMA_BACKEND_EXIT_PANIC, SIGABRT);
	(void) postgamma_execution_context_dispatch_exit(SIGABRT);
	abort();
}


pid_t
postgamma_forbidden_fork_process(void)
{
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();
	PostgammaBackendRegistry *registry = NULL;

	if (runtime != NULL && postgamma_ensure_registry(runtime, &registry) == 0)
		postgamma_backend_registry_record_forbidden_process_launch(
			registry);
	errno = ENOTSUP;
	return -1;
}


static PostgammaInstanceRuntime *
postgamma_current_instance_runtime(void)
{
	PostgammaExecutionContext *execution = postgamma_execution_context_current();

	return execution == NULL ? NULL :
		postgamma_instance_context_runtime(execution->instance);
}


static PostgammaBackendRegistry *
postgamma_current_backend_registry(void)
{
	PostgammaInstanceRuntime *runtime = postgamma_current_instance_runtime();

	return runtime == NULL ? NULL :
		postgamma_instance_runtime_backend_registry(runtime);
}


static int
postgamma_ensure_registry(
	PostgammaInstanceRuntime *runtime,
	PostgammaBackendRegistry **registry)
{
	int			capacity;
	int			status;

	if (runtime == NULL || registry == NULL)
		return EINVAL;
	*registry = postgamma_instance_runtime_backend_registry(runtime);
	if (*registry != NULL)
		return 0;
	capacity = MaxLivePostmasterChildren();
	if (postgamma_instance_runtime_profile(runtime) ==
		POSTGAMMA_RUNTIME_PROFILE_THREADED_SERVER)
	{
		status = postgamma_prepare_process_fd_limit(capacity);
		if (status != 0)
			return status;
	}
	if (capacity > INT_MAX - POSTGAMMA_REGISTRY_EXTRA_SLOTS)
		return EOVERFLOW;
	capacity += POSTGAMMA_REGISTRY_EXTRA_SLOTS;
	return postgamma_instance_runtime_ensure_backend_registry(
		runtime, (uint32_t) capacity, registry);
}


/*
 * Preserve PostgreSQL's per-process descriptor budget for threaded roles.
 *
 * max_files_per_process bounds one backend process in upstream PostgreSQL.
 * All PostGamma roles share a single process limit, so reserve the same budget
 * for every concurrently live child plus the postmaster role before the first
 * backend thread starts.  The soft limit is process-wide; it is raised only
 * once and never above the host-provided hard limit.
 */
static int
postgamma_prepare_process_fd_limit(int child_capacity)
{
#ifdef HAVE_GETRLIMIT
	struct rlimit limit;
	uintmax_t	per_role;
	uintmax_t	role_count;
	uintmax_t	required;

	if (child_capacity <= 0 ||
		POSTGAMMA_GUC_VALUE(max_files_per_process) <= 0)
		return EINVAL;
	per_role = (uintmax_t) POSTGAMMA_GUC_VALUE(max_files_per_process);
	role_count = (uintmax_t) child_capacity + 1;
	if (role_count > UINTMAX_MAX / per_role)
		return EOVERFLOW;
	required = role_count * per_role;

	if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
		return errno != 0 ? errno : EIO;
	if (limit.rlim_cur == RLIM_INFINITY ||
		(uintmax_t) limit.rlim_cur >= required)
		return 0;
	if (limit.rlim_max != RLIM_INFINITY &&
		(uintmax_t) limit.rlim_max < required)
		return EMFILE;

	limit.rlim_cur = (rlim_t) required;
	if ((uintmax_t) limit.rlim_cur != required)
		return EOVERFLOW;
	if (setrlimit(RLIMIT_NOFILE, &limit) != 0)
		return errno != 0 ? errno : EIO;
	return 0;
#else
	(void) child_capacity;
	return ENOTSUP;
#endif
}


static bool
postgamma_execution_policy(
	int backend_type, PostgammaBackendDisposition *disposition,
	PostgammaBackendPolicyClass *policy_class)
{
	switch (backend_type)
	{
#define POSTGAMMA_BACKEND_EXECUTION(symbol, disposition_value, class_value) \
		case symbol: \
			*disposition = disposition_value; \
			*policy_class = class_value; \
			return true;
#include "postgamma/backend_execution.inc"
#undef POSTGAMMA_BACKEND_EXECUTION
	}
	return false;
}


static PostgammaBackendClass
postgamma_execution_class(
	PostgammaBackendPolicyClass policy_class,
	const void *startup_data, size_t startup_data_length)
{
	if (policy_class == POSTGAMMA_BACKEND_POLICY_CLASS_CLIENT)
		return POSTGAMMA_BACKEND_CLASS_CLIENT;
	if (policy_class == POSTGAMMA_BACKEND_POLICY_CLASS_DYNAMIC &&
		startup_data != NULL && startup_data_length == sizeof(BackgroundWorker) &&
		(((const BackgroundWorker *) startup_data)->bgw_flags &
		 BGWORKER_CLASS_PARALLEL) != 0)
		return POSTGAMMA_BACKEND_CLASS_PARALLEL;
	return POSTGAMMA_BACKEND_CLASS_DEDICATED;
}


static PostgammaBackendExitStatus
postgamma_postgres_backend_main(
	const PostgammaBackendStartInfo *start_info,
	const void *startup_data, size_t startup_data_length,
	void *main_argument)
{
	PostgammaClientSession *state = main_argument;
	PostgammaBackendExitStatus yielded = {
		POSTGAMMA_BACKEND_EXIT_YIELD,
		0,
	};

	if (state->handle.id == 0)
		state->handle = start_info->handle;
	else if (state->handle.id != start_info->handle.id ||
		state->handle.compat_pid != start_info->handle.compat_pid)
		postgamma_runtime_contract_violation();
	postgamma_bind_backend_session(state);
	if (state->wake_target == NULL)
		state->wake_target = start_info->wake_target;
	else if (state->wake_target != start_info->wake_target)
		postgamma_runtime_contract_violation();
	if (state->transport != NULL && !state->transport_notify_bound)
	{
		int status = state->transport_ops.set_notify(
			state->transport, state->transport_generation,
			postgamma_backend_transport_notify, state);

		if (status != 0)
		{
			state->exit_status.kind = POSTGAMMA_BACKEND_EXIT_PANIC;
			state->exit_status.code = status;
			return state->exit_status;
		}
		state->transport_notify_bound = true;
		state->client_socket.sock = postgamma_wake_target_fd(
			state->wake_target);
		if (state->client_socket.sock < 0)
		{
			state->exit_status.kind = POSTGAMMA_BACKEND_EXIT_PANIC;
			state->exit_status.code = EIO;
			return state->exit_status;
		}
	}
	state->exit_status.kind = POSTGAMMA_BACKEND_EXIT_UNEXPECTED_RETURN;
	state->exit_status.code = 0;
	if (start_info->stack_status != 0 ||
		start_info->stack_info.role !=
		postgamma_backend_thread_role(start_info->handle.execution_class))
	{
		state->exit_status.kind = POSTGAMMA_BACKEND_EXIT_PANIC;
		state->exit_status.code = start_info->stack_status != 0 ?
			start_info->stack_status : EPROTO;
		return state->exit_status;
	}
	if (!state->backend_initialized)
		sigfillset(&state->logical_signal_mask);
	state->carrier.exit_jump_ready = true;
	if (sigsetjmp(state->carrier.exit_jump, 1) == 0)
	{
		if (!state->backend_initialized)
		{
			postgamma_initialize_backend_thread(
				state, start_info, startup_data, startup_data_length);
			state->backend_initialized = true;
			state->main_function(startup_data, startup_data_length);
		}
		else if (state->postgres_main_initialized &&
			IsExternalConnectionBackend((BackendType) state->backend_type))
		{
			if (PG_STATE(MyProcPort) == NULL ||
				PG_STATE(MyProcPort)->database_name == NULL ||
				PG_STATE(MyProcPort)->user_name == NULL)
				postgamma_runtime_contract_violation();
			PostgresMain(
				PG_STATE(MyProcPort)->database_name,
				PG_STATE(MyProcPort)->user_name);
		}
		else
			state->main_function(startup_data, startup_data_length);
	}
	state->carrier.exit_jump_ready = false;
	if (state->quantum_yielded)
	{
		state->quantum_yielded = false;
		postgamma_unbind_backend_session(state);
		return yielded;
	}
	return state->exit_status;
}


static int
postgamma_postgres_backend_cleanup(
	const PostgammaBackendStartInfo *start_info, void *cleanup_argument)
{
	PostgammaClientSession *state = cleanup_argument;
	bool		crash_exit;

	if (state == NULL || state != PostgammaCurrentBackendSession ||
		state->handle.id != start_info->handle.id)
		return EPROTO;
	crash_exit = postgamma_backend_exit_is_crash(state->exit_status);
	if (postgamma_release_backend_transport(state) != 0)
		crash_exit = true;
	postgamma_cleanup_external_children(state);
	/*
	 * PostgreSQL releases transaction and shared-memory state through
	 * proc_exit callbacks before the normal exit jump.  The remaining work is
	 * strictly thread-owned memory and descriptor cleanup.
	 */
	if (state->wait_support_initialized)
	{
		if (PG_STATE(FeBeWaitSet) != NULL)
		{
			FreeWaitEventSet(PG_STATE(FeBeWaitSet));
			PG_STATE(FeBeWaitSet) = NULL;
		}
		postgamma_shutdown_latch_wait_set();
		postgamma_shutdown_wait_event_support();
		state->wait_support_initialized = false;
	}
	postgamma_shutdown_xlog_file_access();
	postgamma_shutdown_file_access(crash_exit);
	if (state->backend_type == B_LOGGER)
		postgamma_shutdown_syslogger_file_access();
	postgamma_release_external_pipes(state);
	if (state->client_socket_fd != PGINVALID_SOCKET)
	{
		(void) close(state->client_socket_fd);
		state->client_socket_fd = PGINVALID_SOCKET;
	}
	if (state->carrier.execution_bound &&
		postgamma_extension_execution_destroy(&state->execution) != 0)
		crash_exit = true;
	if (state->dsm_control_handle_set)
	{
		dsm_detach_all();
		state->dsm_control_handle_set = false;
	}
	if (state->memory_contexts_initialized)
		postgamma_destroy_postgres_memory_contexts(!crash_exit);
	if (state->execution.postgres_guc_control != NULL)
		postgamma_destroy_builtin_guc_variables(&state->execution);
	if (state->carrier.execution_bound)
		postgamma_unbind_backend_session(state);
#if !defined(WIN32) && defined(HAVE_USELOCALE)
	if (state->session_locale != (locale_t) 0)
	{
		freelocale(state->session_locale);
		state->session_locale = (locale_t) 0;
	}
#endif
	postgamma_execution_context_destroy(&state->execution);
	postgamma_role_context_destroy(&state->role);
	if (state->path_runtime != NULL)
	{
		postgamma_instance_context_detach_path_runtime(
			&state->private_instance, state->path_runtime);
		state->path_runtime = NULL;
	}
	postgamma_instance_context_detach_runtime(
		&state->private_instance, state->runtime);
	postgamma_instance_context_destroy(&state->private_instance);
	free(state->guc_state);
	free(state);
	return 0;
}


static bool
transport_ops_are_valid(
	const PostgammaKernelServerTransportOps *transport_ops)
{
	return transport_ops != NULL &&
		transport_ops->struct_size == sizeof(*transport_ops) &&
		transport_ops->abi_version == POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION &&
		transport_ops->retain != NULL && transport_ops->release != NULL &&
		transport_ops->set_notify != NULL && transport_ops->read != NULL &&
		transport_ops->write != NULL && transport_ops->ready != NULL &&
		transport_ops->half_close_write != NULL &&
		transport_ops->set_session_status != NULL &&
		transport_ops->get_result_policy != NULL;
}


static void
postgamma_backend_transport_notify(void *argument, uint32_t events)
{
	PostgammaClientSession *state = argument;
	bool		runnable = (events &
		(POSTGAMMA_KERNEL_TRANSPORT_READABLE |
		 POSTGAMMA_KERNEL_TRANSPORT_PEER_CLOSED)) != 0;

	if (state == NULL || state->wake_target == NULL ||
		(events & ~POSTGAMMA_KERNEL_TRANSPORT_ALL) != 0 ||
		postgamma_backend_notify(
			state->registry, state->handle.id, runnable) != 0)
		postgamma_runtime_contract_violation();
}


static int
postgamma_release_backend_transport(PostgammaClientSession *state)
{
	int			status = 0;
	int			operation_status;

	if (state == NULL || state->transport == NULL)
		return 0;
	if (state->transport_notify_bound)
	{
		operation_status = state->transport_ops.set_notify(
			state->transport, state->transport_generation, NULL, NULL);
		if (operation_status != 0)
			status = operation_status;
		state->transport_notify_bound = false;
	}
	operation_status = state->transport_ops.set_session_status(
		state->transport, state->transport_generation, UINT32_C(0), UINT32_C(0));
	if (operation_status != 0 && operation_status != EPIPE && status == 0)
		status = operation_status;
	operation_status = state->transport_ops.half_close_write(
		state->transport, state->transport_generation);
	if (operation_status != 0 && operation_status != EPIPE && status == 0)
		status = operation_status;
	operation_status = state->transport_ops.release(
		&state->transport, state->transport_generation);
	if (operation_status != 0 && status == 0)
		status = operation_status;
	state->transport_generation = 0;
	memset(&state->transport_ops, 0, sizeof(state->transport_ops));
	return status;
}


static int
postgamma_initialize_shell_spawn_attributes(
	posix_spawnattr_t *attributes, bool reset_sigpipe)
{
	short		flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
		POSIX_SPAWN_SETSIGMASK;
	sigset_t	default_signals;
	sigset_t	empty_mask;
	int			result;

	result = posix_spawnattr_init(attributes);
	if (result != 0)
		return result;
	sigemptyset(&default_signals);
	sigaddset(&default_signals, SIGINT);
	sigaddset(&default_signals, SIGQUIT);
	if (reset_sigpipe)
		sigaddset(&default_signals, SIGPIPE);
	sigemptyset(&empty_mask);
	result = posix_spawnattr_setsigdefault(attributes, &default_signals);
	if (result == 0)
		result = posix_spawnattr_setsigmask(attributes, &empty_mask);
	if (result == 0)
		result = posix_spawnattr_setpgroup(attributes, 0);
	if (result == 0)
		result = posix_spawnattr_setflags(attributes, flags);
	if (result != 0)
		(void) posix_spawnattr_destroy(attributes);
	return result;
}


static int
postgamma_set_close_on_exec(int descriptor)
{
	int			flags = fcntl(descriptor, F_GETFD);

	if (flags < 0)
		return errno != 0 ? errno : EIO;
	if (fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0)
		return errno != 0 ? errno : EIO;
	return 0;
}


static void
postgamma_system_child_finished(
	PostgammaClientSession *state, pid_t pid)
{
	if (state == NULL || state->system_child_pid != pid)
		postgamma_runtime_contract_violation();
	state->system_child_pid = 0;
	if (postgamma_instance_runtime_record_external_process_finish(
			state->runtime) != 0 ||
		postgamma_instance_runtime_notify_completion(state->runtime) != 0)
		postgamma_runtime_contract_violation();
}


static void
postgamma_external_pipe_finished(
	PostgammaClientSession *state,
	PostgammaExternalPipe *external_pipe, int wait_status)
{
	if (state == NULL || external_pipe == NULL ||
		external_pipe->child_reaped || external_pipe->pid == 0)
		postgamma_runtime_contract_violation();
	external_pipe->pid = 0;
	external_pipe->wait_status = wait_status;
	external_pipe->child_reaped = true;
	if (postgamma_instance_runtime_record_external_process_finish(
			state->runtime) != 0 ||
		postgamma_instance_runtime_notify_completion(state->runtime) != 0)
		postgamma_runtime_contract_violation();
}


static int
postgamma_wait_external_child(pid_t pid, int *wait_status)
{
	struct timespec delay = {0, 10 * 1000 * 1000};

	for (;;)
	{
		pid_t		result = waitpid(pid, wait_status, WNOHANG);

		if (result == pid)
			return 0;
		if (result < 0 && errno != EINTR)
			return errno != 0 ? errno : ECHILD;
		postgamma_dispatch_pending_signals();
		(void) nanosleep(&delay, NULL);
	}
}


static void
postgamma_cleanup_external_children(PostgammaClientSession *state)
{
	pid_t		pid = state->system_child_pid;
	int			status;

	if (pid != 0)
	{
		if (kill(-pid, SIGKILL) != 0 && errno != ESRCH)
			postgamma_runtime_contract_violation();
		while (waitpid(pid, &status, 0) < 0)
		{
			if (errno != EINTR && errno != ECHILD)
				postgamma_runtime_contract_violation();
			if (errno == ECHILD)
				break;
		}
		postgamma_system_child_finished(state, pid);
	}
	for (PostgammaExternalPipe *external_pipe = state->external_pipes;
		 external_pipe != NULL; external_pipe = external_pipe->next)
	{
		if (!external_pipe->stream_closed)
		{
			(void) fclose(external_pipe->stream);
			external_pipe->stream_closed = true;
		}
		if (external_pipe->child_reaped)
			continue;
		pid = external_pipe->pid;
		if (kill(-pid, SIGKILL) != 0 && errno != ESRCH)
			postgamma_runtime_contract_violation();
		while (waitpid(pid, &status, 0) < 0)
		{
			if (errno != EINTR && errno != ECHILD)
				postgamma_runtime_contract_violation();
			if (errno == ECHILD)
			{
				status = -1;
				break;
			}
		}
		postgamma_external_pipe_finished(state, external_pipe, status);
	}
}


static void
postgamma_release_external_pipes(PostgammaClientSession *state)
{
	while (state->external_pipes != NULL)
	{
		PostgammaExternalPipe *external_pipe = state->external_pipes;

		state->external_pipes = external_pipe->next;
		if (!external_pipe->stream_closed)
			(void) fclose(external_pipe->stream);
		if (!external_pipe->child_reaped)
			postgamma_runtime_contract_violation();
		free(external_pipe);
	}
}


static void
postgamma_postgres_backend_completed(void *argument)
{
	PostgammaInstanceRuntime *runtime = argument;

	if (postgamma_instance_runtime_notify_completion(runtime) != 0)
		postgamma_runtime_contract_violation();
}


static void
postgamma_bind_backend_session(PostgammaClientSession *state)
{
	PostgammaCarrierFrame *carrier;

	if (state == NULL || PostgammaCurrentBackendSession != NULL)
		postgamma_runtime_contract_violation();
	carrier = &state->carrier;
	if (carrier->execution_bound || carrier->exit_jump_ready)
		postgamma_runtime_contract_violation();
	carrier->saved_errno = errno;
	carrier->previous_execution =
		postgamma_execution_context_bind(&state->execution);
	carrier->execution_bound = true;
	PostgammaCurrentBackendSession = state;
	if (state->memory_contexts_initialized &&
		postgamma_instance_runtime_profile(state->runtime) ==
			POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
		PG_STATE(emit_log_hook) = postgamma_embedded_emit_log;
#if !defined(WIN32) && defined(HAVE_USELOCALE)
	carrier->previous_locale = uselocale(LC_GLOBAL_LOCALE);
	if (carrier->previous_locale == (locale_t) 0)
		postgamma_runtime_contract_violation();
	if (state->session_locale != (locale_t) 0 &&
		uselocale(state->session_locale) == (locale_t) 0)
		postgamma_runtime_contract_violation();
#endif
	/* stack_base_ptr belongs to the active carrier, not the session. */
	carrier->previous_stack_base = set_stack_base();
}


static void
postgamma_unbind_backend_session(PostgammaClientSession *state)
{
	PostgammaCarrierFrame *carrier;
	int			saved_errno;

	if (state == NULL || PostgammaCurrentBackendSession != state)
		postgamma_runtime_contract_violation();
	carrier = &state->carrier;
	if (!carrier->execution_bound || carrier->exit_jump_ready)
		postgamma_runtime_contract_violation();
	saved_errno = carrier->saved_errno;

	/* Do not retain pointers into a worker stack after releasing the carrier. */
	PG_STATE(PG_exception_stack) = NULL;
	PG_STATE(error_context_stack) = NULL;
	restore_stack_base(carrier->previous_stack_base);
	carrier->previous_stack_base = NULL;
#if !defined(WIN32) && defined(HAVE_USELOCALE)
	if (uselocale(carrier->previous_locale) == (locale_t) 0)
		postgamma_runtime_contract_violation();
	carrier->previous_locale = (locale_t) 0;
#endif
	PostgammaCurrentBackendSession = NULL;
	postgamma_execution_context_restore(
		&state->execution, carrier->previous_execution);
	carrier->previous_execution = NULL;
	carrier->execution_bound = false;
	errno = saved_errno;
}


static void
postgamma_initialize_backend_thread(
	PostgammaClientSession *state,
	const PostgammaBackendStartInfo *start_info,
	const void *startup_data, size_t startup_data_length)
{
	MemoryContext old_context;

	PG_STATE(MyProcPid) = start_info->handle.compat_pid;
	PG_STATE(MyBackendType) = (BackendType) state->backend_type;
	PG_STATE(MyPMChildSlot) = state->child_slot;
	PG_STATE(IsPostmasterEnvironment) = true;
	PG_STATE(IsUnderPostmaster) = true;
	PG_STATE(MyClientSocket) = NULL;

	MemoryContextInit();
	state->memory_contexts_initialized = true;
	/*
	 * EXEC_BACKEND children begin with each compiled GUC backing variable at
	 * its boot value.  A fresh role context is zero-filled instead, so copy
	 * those pristine backing values before PostgreSQL validates and initializes
	 * the role-owned GUC table.
	 */
	postgamma_initialize_builtin_guc_values();
	InitializeGUCOptions();
	postgamma_seed_inherited_guc_shadow(state->shared_instance);
	if (state->guc_state_length != 0)
		RestoreGUCState(state->guc_state);
	postgamma_validate_backend_stack_depth(start_info);
	/*
	 * Keep the role bound to its applied instance-configuration snapshot.
	 * PostgreSQL children inherit server-wide GUC values and then apply SIGHUP
	 * reloads independently.  Reading the postmaster's live backing storage
	 * would expose a new value before ProcessConfigFile() runs, defeating
	 * old/new comparisons and role-local assign-hook side effects.  The shared
	 * instance remains the canonical inheritance source for newly launched
	 * roles; both the cloned GUC table and transformed value references stay
	 * bound to this role's private snapshot for its lifetime.
	 */
	LocalProcessControlFile(false);

	/*
	 * Virtualized role state deliberately starts without the postmaster's
	 * backend-private shared-memory handles.  Reuse PostgreSQL's EXEC_BACKEND
	 * contract to enumerate every built-in area now; InitProcess() or
	 * InitAuxiliaryProcess() attaches those requests after it owns a PGPROC.
	 * EXEC_BACKEND also reloads shared-preload modules in every child before
	 * attaching their requested shared-memory callbacks.  A thread has its
	 * own virtualized loader state, so it must preserve the same ordering even
	 * though the module code is already mapped into the host process.
	 */
	RegisterBuiltinShmemCallbacks();
	process_shared_preload_libraries();
	if (state->shared_memory_access)
	{
		PGShmemHeader *header = PG_STATE(UsedShmemSegAddr);

		if (header == NULL)
			postgamma_runtime_contract_violation();
		dsm_set_control_handle(header->dsm_control);
		state->dsm_control_handle_set = true;
		InitShmemAllocator(header);
		ShmemCallRequestCallbacks();
	}
	else
	{
		PG_STATE(UsedShmemSegAddr) = NULL;
		PG_STATE(UsedShmemSegID) = 0;
	}

	InitProcessGlobals();
	on_exit_reset();
	pqinitmask();
	InitializeWaitEventSupport();
	InitProcessLocalLatch();
	InitializeLatchWaitSet();
	state->wait_support_initialized = true;
	pqsignal(SIGQUIT, SignalHandlerForCrashExit);
	sigdelset(&PG_STATE(BlockSig), SIGQUIT);
	postgamma_sigprocmask(SIG_SETMASK, &PG_STATE(BlockSig), NULL);

	PG_STATE(PostmasterContext) = AllocSetContextCreate(
		PG_STATE(TopMemoryContext), "Postmaster", ALLOCSET_DEFAULT_SIZES);
	if (state->backend_type == B_BACKEND ||
		state->backend_type == B_DEAD_END_BACKEND)
	{
		if (!load_hba())
			elog(FATAL, "could not load host-based authentication configuration");
		(void) load_ident();
	}
	if (state->has_client_socket)
	{
		PG_STATE(MyClientSocket) = MemoryContextAlloc(
			PG_STATE(TopMemoryContext), sizeof(ClientSocket));
		memcpy(PG_STATE(MyClientSocket), &state->client_socket,
			   sizeof(ClientSocket));
	}
	if (IsExternalConnectionBackend((BackendType) state->backend_type))
	{
		const BackendStartupData *backend_startup;

		if (startup_data == NULL ||
			startup_data_length != sizeof(BackendStartupData))
			postgamma_runtime_contract_violation();
		backend_startup = startup_data;
		PG_STATE(conn_timing).socket_create = backend_startup->socket_created;
		PG_STATE(conn_timing).fork_start = backend_startup->fork_started;
		PG_STATE(conn_timing).fork_end = GetCurrentTimestamp();
	}
	old_context = MemoryContextSwitchTo(PG_STATE(TopMemoryContext));
	(void) old_context;
	if (postgamma_instance_runtime_profile(state->runtime) ==
		POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
		PG_STATE(emit_log_hook) = postgamma_embedded_emit_log;
	postgamma_dispatch_pending_signals();
}


static void
postgamma_validate_backend_stack_depth(
	const PostgammaBackendStartInfo *start_info)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;
	ssize_t		stack_limit = postgamma_backend_stack_depth_limit();
	ssize_t		configured_limit = 0;
	size_t		available_depth = stack_limit > STACK_DEPTH_SLOP ?
		(size_t) (stack_limit - STACK_DEPTH_SLOP) : 0;
	int			configured_depth = POSTGAMMA_GUC_VALUE(max_stack_depth);
	bool		invalid_depth = configured_depth <= 0 ||
		(ssize_t) configured_depth > SSIZE_MAX / 1024;
	bool		valid;
	int			status;

	if (!invalid_depth)
		configured_limit = (ssize_t) configured_depth * 1024;
	valid = start_info != NULL && start_info->stack_status == 0 &&
		start_info->stack_info.role !=
			POSTGAMMA_THREAD_ROLE_COUNT &&
		start_info->stack_info.role ==
			postgamma_backend_thread_role(start_info->handle.execution_class) &&
		stack_limit > STACK_DEPTH_SLOP && !invalid_depth &&
		configured_limit <= stack_limit - STACK_DEPTH_SLOP;
	if (state == NULL || start_info == NULL)
		postgamma_runtime_contract_violation();
	status = postgamma_backend_registry_record_stack_validation(
		state->registry, start_info->handle.id,
		configured_limit > 0 ? (size_t) configured_limit : 0,
		available_depth, valid);
	if (status != 0)
		postgamma_runtime_contract_violation();
	if (!valid)
		elog(FATAL,
			 "PostGamma role stack cannot safely support max_stack_depth: "
			 "role=%d native=%zu guard=%zu usable=%zu max_stack_depth=%d",
			 start_info == NULL ? -1 : (int) start_info->stack_info.role,
			 start_info == NULL ? 0 : start_info->stack_info.native_stack_size,
			 start_info == NULL ? 0 : start_info->stack_info.native_guard_size,
			 start_info == NULL ? 0 : start_info->stack_info.usable_stack_size,
			 configured_depth);
}


void
postgamma_destroy_postgres_memory_contexts(bool invoke_callbacks)
{
	MemoryContext top = PG_STATE(TopMemoryContext);

	if (top == NULL)
		return;
	if (!invoke_callbacks)
		postgamma_discard_memory_context_callbacks(top);
	MemoryContextSwitchTo(top);
	MemoryContextDeleteChildren(top);
	PG_STATE(CurrentMemoryContext) = NULL;
	PG_STATE(ErrorContext) = NULL;
	PG_STATE(PostmasterContext) = NULL;
	PG_STATE(TopMemoryContext) = NULL;
	MemoryContextDelete(top);
	postgamma_shutdown_snapshot_storage();
	postgamma_shutdown_error_support();
	postgamma_shutdown_allocset_freelists();
}


void
postgamma_shutdown_owned_data_directory(void)
{
	free(PG_STATE(DataDir));
	PG_STATE(DataDir) = NULL;
}


static void
postgamma_discard_memory_context_callbacks(MemoryContext context)
{
	MemoryContext child;

	context->reset_cbs = NULL;
	for (child = context->firstchild; child != NULL; child = child->nextchild)
		postgamma_discard_memory_context_callbacks(child);
}


static bool
postgamma_backend_exit_is_crash(PostgammaBackendExitStatus exit_status)
{
	return exit_status.kind == POSTGAMMA_BACKEND_EXIT_QUICKDIE ||
		exit_status.kind == POSTGAMMA_BACKEND_EXIT_PANIC ||
		exit_status.kind == POSTGAMMA_BACKEND_EXIT_UNEXPECTED_RETURN;
}


static void
postgamma_set_exit_status(PostgammaBackendExitKind kind, int code)
{
	PostgammaClientSession *state = PostgammaCurrentBackendSession;

	if (state == NULL || !state->carrier.exit_jump_ready)
		abort();
	state->exit_status.kind = kind;
	state->exit_status.code = code;
	siglongjmp(state->carrier.exit_jump, 1);
}


static int
postgamma_wait_status(PostgammaBackendExitStatus status)
{
	switch (status.kind)
	{
		case POSTGAMMA_BACKEND_EXIT_NORMAL:
		case POSTGAMMA_BACKEND_EXIT_FATAL:
		case POSTGAMMA_BACKEND_EXIT_STARTUP_TARGET:
			return (status.code & 0xff) << 8;
		case POSTGAMMA_BACKEND_EXIT_YIELD:
			return SIGABRT;
		case POSTGAMMA_BACKEND_EXIT_QUICKDIE:
			if (status.code == SIGQUIT || status.code == SIGKILL)
				return status.code;
			return SIGQUIT;
		case POSTGAMMA_BACKEND_EXIT_PANIC:
		case POSTGAMMA_BACKEND_EXIT_UNEXPECTED_RETURN:
			return SIGABRT;
	}
	return SIGABRT;
}


bool
postgamma_backend_pid_is_compat(pid_t pid)
{
	PostgammaCompatPid compat_pid;

	return postgamma_decode_compat_pid(pid, &compat_pid);
}


/*
 * Crash a threaded backend role without exposing arbitrary host-process
 * signaling.  This narrow SQL bridge exists so PostgreSQL's crash-recovery
 * tests can retain their original semantics when pg_backend_pid() returns a
 * compatibility token rather than an operating-system PID.
 */
Datum
pg_postgamma_crash_backend(PG_FUNCTION_ARGS)
{
	pid_t		pid = (pid_t) PG_GETARG_INT32(0);
	int			signal_number = PG_GETARG_INT32(1);

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to crash threaded backend role")));
	if (signal_number != SIGQUIT && signal_number != SIGKILL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unsupported threaded backend crash signal: %d",
						signal_number),
				 errhint("Use SIGQUIT (%d) or SIGKILL (%d).",
						 SIGQUIT, SIGKILL)));
	if (!postgamma_backend_pid_is_compat(pid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("PID %d is not a threaded backend role", (int) pid)));
	if (postgamma_kill(pid, 0) != 0)
	{
		ereport(WARNING,
				(errmsg("could not find threaded backend role %d: %m",
						(int) pid)));
		PG_RETURN_BOOL(false);
	}
	if (postgamma_kill(pid, signal_number) != 0)
	{
		ereport(WARNING,
				(errmsg("could not send crash signal to threaded backend role %d: %m",
						(int) pid)));
		PG_RETURN_BOOL(false);
	}
	PG_RETURN_BOOL(true);
}


/* Cooperatively pause or resume one threaded backend role. */
Datum
pg_postgamma_pause_backend(PG_FUNCTION_ARGS)
{
	pid_t		pid = (pid_t) PG_GETARG_INT32(0);
	bool		pause = PG_GETARG_BOOL(1);
	PostgammaBackendRegistry *registry = postgamma_current_backend_registry();
	int			status;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to pause threaded backend role")));
	if (!postgamma_backend_pid_is_compat(pid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("PID %d is not a threaded backend role", (int) pid)));
	if (pid == postgamma_getpid())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a threaded backend role cannot pause itself")));
	status = registry == NULL ? ESRCH : pause ?
		postgamma_backend_pause(registry, pid) :
		postgamma_backend_resume(registry, pid);
	if (status != 0)
	{
		errno = status;
		ereport(WARNING,
				(errmsg("could not %s threaded backend role %d: %m",
						pause ? "pause" : "resume", (int) pid)));
		PG_RETURN_BOOL(false);
	}
	PG_RETURN_BOOL(true);
}


static bool
postgamma_decode_compat_pid(pid_t pid, PostgammaCompatPid *compat_pid)
{
	int64		candidate = pid;

	if (candidate < 0)
		candidate = -candidate;
	if (candidate < POSTGAMMA_COMPAT_PID_FIRST || candidate > INT32_MAX)
		return false;
	*compat_pid = (PostgammaCompatPid) candidate;
	return true;
}


static PostgammaVirtualTimer *
postgamma_current_virtual_timer(void)
{
	if (PostgammaCurrentBackendSession != NULL)
		return &PostgammaCurrentBackendSession->timer;
	if (PostgammaCurrentStandaloneInput != NULL)
		return &PostgammaStandaloneSignals.timer;
	return NULL;
}


static bool
postgamma_virtual_timer_due(PostgammaVirtualTimer *timer)
{
	TimestampTz now;
	int64		elapsed;
	int64		periods;

	if (timer == NULL || !timer->timeout_armed)
		return false;
	now = GetCurrentTimestamp();
	if (now < timer->timeout_due_at)
		return false;
	if (timer->timeout_interval_us <= 0)
	{
		timer->timeout_armed = false;
		timer->timeout_due_at = 0;
		return true;
	}

	elapsed = now - timer->timeout_due_at;
	periods = elapsed / timer->timeout_interval_us + 1;
	if (periods >
		(PG_INT64_MAX - timer->timeout_due_at) /
		timer->timeout_interval_us)
	{
		timer->timeout_armed = false;
		timer->timeout_due_at = 0;
	}
	else
		timer->timeout_due_at += periods * timer->timeout_interval_us;
	return true;
}


#if !defined(WIN32) && defined(HAVE_USELOCALE)
static int
postgamma_locale_category_index(int category)
{
	switch (category)
	{
		case LC_COLLATE:
			return POSTGAMMA_LOCALE_COLLATE;
		case LC_CTYPE:
			return POSTGAMMA_LOCALE_CTYPE;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			return POSTGAMMA_LOCALE_MESSAGES;
#endif
		case LC_MONETARY:
			return POSTGAMMA_LOCALE_MONETARY;
		case LC_NUMERIC:
			return POSTGAMMA_LOCALE_NUMERIC;
		case LC_TIME:
			return POSTGAMMA_LOCALE_TIME;
	}
	return -1;
}


static int
postgamma_locale_category_mask(int category)
{
	switch (category)
	{
		case LC_COLLATE:
			return LC_COLLATE_MASK;
		case LC_CTYPE:
			return LC_CTYPE_MASK;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			return LC_MESSAGES_MASK;
#endif
		case LC_MONETARY:
			return LC_MONETARY_MASK;
		case LC_NUMERIC:
			return LC_NUMERIC_MASK;
		case LC_TIME:
			return LC_TIME_MASK;
	}
	return 0;
}


static const char *
postgamma_effective_locale_name(int category, const char *locale)
{
	const char *category_environment = NULL;
	const char *result;

	if (locale != NULL && locale[0] != '\0')
		return locale;
	result = getenv("LC_ALL");
	if (result != NULL && result[0] != '\0')
		return result;
	switch (category)
	{
		case LC_COLLATE:
			category_environment = "LC_COLLATE";
			break;
		case LC_CTYPE:
			category_environment = "LC_CTYPE";
			break;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			category_environment = "LC_MESSAGES";
			break;
#endif
		case LC_MONETARY:
			category_environment = "LC_MONETARY";
			break;
		case LC_NUMERIC:
			category_environment = "LC_NUMERIC";
			break;
		case LC_TIME:
			category_environment = "LC_TIME";
			break;
	}
	if (category_environment != NULL)
	{
		result = getenv(category_environment);
		if (result != NULL && result[0] != '\0')
			return result;
	}
	result = getenv("LANG");
	if (result != NULL && result[0] != '\0')
		return result;
	return "C";
}


static void
postgamma_initialize_locale_names(PostgammaClientSession *state)
{
	static const int categories[POSTGAMMA_LOCALE_CATEGORY_COUNT] =
	{
		LC_COLLATE,
		LC_CTYPE,
#ifdef LC_MESSAGES
		LC_MESSAGES,
#else
		-1,
#endif
		LC_MONETARY,
		LC_NUMERIC,
		LC_TIME
	};

	if (state->locale_names_initialized)
		return;
	for (int index = 0; index < POSTGAMMA_LOCALE_CATEGORY_COUNT; index++)
	{
		const char *name = categories[index] < 0 ? "C" :
			postgamma_effective_locale_name(categories[index], "");

		strlcpy(state->locale_names[index], name,
				sizeof(state->locale_names[index]));
	}
	state->locale_names_initialized = true;
}
#endif
