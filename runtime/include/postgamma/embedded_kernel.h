/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * embedded_kernel.h
 *    Versioned internal ABI between the embedded host and PostgreSQL.
 *
 * This boundary deliberately contains only fixed-width integers, sizes,
 * strings, callbacks, and opaque pointers.  PostgreSQL implementation types
 * must not cross it.  It is an internal ABI, not the public libpostgamma API.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_EMBEDDED_KERNEL_H
#define POSTGAMMA_EMBEDDED_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION UINT32_C(8)

#define POSTGAMMA_KERNEL_HOST_CAP_CONTROL_WAKE_FD (UINT64_C(1) << 0)
#define POSTGAMMA_KERNEL_HOST_CAP_ASYNC_NOTIFICATION (UINT64_C(1) << 1)
#define POSTGAMMA_KERNEL_HOST_CAP_FAIL_STOP (UINT64_C(1) << 2)
#define POSTGAMMA_KERNEL_HOST_CAP_LOG_EVENTS (UINT64_C(1) << 3)

#define POSTGAMMA_KERNEL_CAP_SERVER_LIFECYCLE (UINT64_C(1) << 0)
#define POSTGAMMA_KERNEL_CAP_MEMORY_PROTOCOL (UINT64_C(1) << 1)
#define POSTGAMMA_KERNEL_CAP_CLUSTER_CREATE (UINT64_C(1) << 2)
#define POSTGAMMA_KERNEL_CAP_LOGICAL_TOOLS (UINT64_C(1) << 3)
#define POSTGAMMA_KERNEL_CAP_STATIC_EXTENSIONS (UINT64_C(1) << 4)
#define POSTGAMMA_KERNEL_CAP_MULTI_INSTANCE (UINT64_C(1) << 5)

typedef int32_t PostgammaKernelControlKind;

#define POSTGAMMA_KERNEL_CONTROL_CONNECT INT32_C(0)
#define POSTGAMMA_KERNEL_CONTROL_CANCEL INT32_C(1)
#define POSTGAMMA_KERNEL_CONTROL_DISCONNECT INT32_C(2)
#define POSTGAMMA_KERNEL_CONTROL_RELOAD INT32_C(3)
#define POSTGAMMA_KERNEL_CONTROL_ROLE_COMPLETION INT32_C(4)
#define POSTGAMMA_KERNEL_CONTROL_SHUTDOWN INT32_C(5)
#define POSTGAMMA_KERNEL_CONTROL_CHECKPOINT INT32_C(6)

typedef int32_t PostgammaKernelShutdownMode;

#define POSTGAMMA_KERNEL_SHUTDOWN_SMART INT32_C(0)
#define POSTGAMMA_KERNEL_SHUTDOWN_FAST INT32_C(1)
#define POSTGAMMA_KERNEL_SHUTDOWN_IMMEDIATE INT32_C(2)

typedef struct PostgammaKernelControl
{
	uint64_t	generation;
	uint64_t	sequence;
	PostgammaKernelControlKind kind;
	PostgammaKernelShutdownMode shutdown_mode;
	void	   *payload;
	void	   *private_token;
} PostgammaKernelControl;

typedef void (*PostgammaKernelTransportNotifyFunction) (
	void *argument, uint32_t events);
#define POSTGAMMA_KERNEL_TRANSPORT_READABLE UINT32_C(0x01)
#define POSTGAMMA_KERNEL_TRANSPORT_WRITABLE UINT32_C(0x02)
#define POSTGAMMA_KERNEL_TRANSPORT_PEER_CLOSED UINT32_C(0x04)
#define POSTGAMMA_KERNEL_TRANSPORT_ALL UINT32_C(0x07)

#define POSTGAMMA_KERNEL_PIN_TRANSACTION UINT32_C(0x01)
#define POSTGAMMA_KERNEL_PIN_ADVISORY_LOCK UINT32_C(0x02)
#define POSTGAMMA_KERNEL_PIN_PORTAL UINT32_C(0x04)
#define POSTGAMMA_KERNEL_PIN_COPY UINT32_C(0x08)
#define POSTGAMMA_KERNEL_PIN_OTHER UINT32_C(0x10)
#define POSTGAMMA_KERNEL_PIN_ALL UINT32_C(0x1f)

#define POSTGAMMA_KERNEL_EXECUTOR_POOLED UINT32_C(0)
#define POSTGAMMA_KERNEL_EXECUTOR_DEDICATED UINT32_C(1)

#define POSTGAMMA_KERNEL_DELIVERY_MATERIALIZED UINT32_C(0)
#define POSTGAMMA_KERNEL_DELIVERY_CHUNKED UINT32_C(1)

typedef struct PostgammaKernelResultPolicy
{
	uint64_t	request_generation;
	uint32_t	delivery_mode;
	uint32_t	target_chunk_rows;
	size_t		result_buffer_limit;
	size_t		maximum_value_size;
} PostgammaKernelResultPolicy;

#define POSTGAMMA_KERNEL_RESULT_POLICY_INIT \
	{UINT64_C(0), POSTGAMMA_KERNEL_DELIVERY_MATERIALIZED, UINT32_C(0), 0, 0}

typedef int (*PostgammaKernelTransportRetainFunction) (
	void *transport, uint64_t generation, void **retained_transport);
typedef int (*PostgammaKernelTransportReleaseFunction) (
	void **transport, uint64_t generation);
typedef int (*PostgammaKernelTransportSetNotifyFunction) (
	void *transport, uint64_t generation,
	PostgammaKernelTransportNotifyFunction notify, void *notify_argument);
typedef int (*PostgammaKernelTransportReadFunction) (
	void *transport, uint64_t generation, void *buffer, size_t length,
	size_t *transferred);
typedef int (*PostgammaKernelTransportWriteFunction) (
	void *transport, uint64_t generation, const void *buffer, size_t length,
	size_t *transferred);
typedef int (*PostgammaKernelTransportReadyFunction) (
	void *transport, uint64_t generation, uint32_t *events);
typedef int (*PostgammaKernelTransportHalfCloseFunction) (
	void *transport, uint64_t generation);
typedef int (*PostgammaKernelTransportSetSessionStatusFunction) (
	void *transport, uint64_t generation, uint32_t pin_reasons,
	uint32_t carrier_retained);
typedef int (*PostgammaKernelTransportGetResultPolicyFunction) (
	void *transport, uint64_t generation,
	PostgammaKernelResultPolicy *policy);

typedef struct PostgammaKernelServerTransportOps
{
	uint32_t	struct_size;
	uint32_t	abi_version;
	PostgammaKernelTransportRetainFunction retain;
	PostgammaKernelTransportReleaseFunction release;
	PostgammaKernelTransportSetNotifyFunction set_notify;
	PostgammaKernelTransportReadFunction read;
	PostgammaKernelTransportWriteFunction write;
	PostgammaKernelTransportReadyFunction ready;
	PostgammaKernelTransportHalfCloseFunction half_close_write;
	PostgammaKernelTransportSetSessionStatusFunction set_session_status;
	PostgammaKernelTransportGetResultPolicyFunction get_result_policy;
} PostgammaKernelServerTransportOps;

typedef struct PostgammaKernelConnectRequest
{
	uint32_t	struct_size;
	uint32_t	abi_version;
	uint64_t	generation;
	const PostgammaKernelServerTransportOps *transport_ops;
	void	   *transport;
	uint64_t	connection_id;
	int32_t		backend_pid;
} PostgammaKernelConnectRequest;

#define POSTGAMMA_KERNEL_CONNECT_REQUEST_INIT \
	{sizeof(PostgammaKernelConnectRequest), \
	 POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION, UINT64_C(0), NULL, NULL, \
	 UINT64_C(0), 0}

typedef struct PostgammaKernelCancelRequest
{
	uint32_t	struct_size;
	uint32_t	abi_version;
	uint64_t	generation;
	uint64_t	request_generation;
	int32_t		backend_pid;
	uint32_t	dispatched;
} PostgammaKernelCancelRequest;

#define POSTGAMMA_KERNEL_CANCEL_REQUEST_INIT \
	{sizeof(PostgammaKernelCancelRequest), \
	 POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION, UINT64_C(0), UINT64_C(0), 0, 0}

#define POSTGAMMA_KERNEL_CHECKPOINT_FAST UINT32_C(0x01)
#define POSTGAMMA_KERNEL_CHECKPOINT_FORCE UINT32_C(0x02)
#define POSTGAMMA_KERNEL_CHECKPOINT_ALL UINT32_C(0x03)

typedef struct PostgammaKernelCheckpointRequest
{
	uint32_t	struct_size;
	uint32_t	abi_version;
	uint64_t	generation;
	uint32_t	flags;
	uint32_t	dispatched;
	void	   *tracker;
} PostgammaKernelCheckpointRequest;

#define POSTGAMMA_KERNEL_CHECKPOINT_REQUEST_INIT \
	{sizeof(PostgammaKernelCheckpointRequest), \
	 POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION, UINT64_C(0), \
	 POSTGAMMA_KERNEL_CHECKPOINT_FAST | POSTGAMMA_KERNEL_CHECKPOINT_FORCE, \
	 UINT32_C(0), NULL}

typedef int (*PostgammaKernelMarkStateFunction) (
	void *context, uint64_t generation);
typedef int (*PostgammaKernelControlWakeFdFunction) (
	void *context, uint64_t generation, int *wake_fd);
typedef int (*PostgammaKernelControlWakeDrainFunction) (
	void *context, uint64_t generation, uint64_t *wake_count);
typedef int (*PostgammaKernelControlNotifyFunction) (
	void *context, uint64_t generation);
typedef int (*PostgammaKernelControlTakeFunction) (
	void *context, uint64_t generation, PostgammaKernelControl *control);
typedef int (*PostgammaKernelControlCompleteFunction) (
	void *context, PostgammaKernelControl *control, int operation_status);
typedef int (*PostgammaKernelFailFunction) (
	void *context, uint64_t generation, int failure_status);

typedef struct PostgammaKernelLogRecord
{
	uint32_t	struct_size;
	int32_t		virtual_backend_pid;
	uint64_t	connection_id;
	uint64_t	request_id;
	const char *severity;
	const char *sqlstate;
	const char *message;
	const char *detail;
} PostgammaKernelLogRecord;

#define POSTGAMMA_KERNEL_LOG_RECORD_INIT \
	{sizeof(PostgammaKernelLogRecord), INT32_C(0), UINT64_C(0), \
	 UINT64_C(0), NULL, NULL, NULL, NULL}

typedef int (*PostgammaKernelEmitLogFunction) (
	void *context, uint64_t generation,
	const PostgammaKernelLogRecord *record);

typedef struct PostgammaKernelHostProvider
{
	uint32_t	struct_size;
	uint32_t	abi_version;
	uint64_t	capabilities;
	void	   *context;
	PostgammaKernelMarkStateFunction mark_recovering;
	PostgammaKernelMarkStateFunction mark_ready;
	PostgammaKernelControlWakeFdFunction control_wake_fd;
	PostgammaKernelControlWakeDrainFunction control_wake_drain;
	PostgammaKernelControlNotifyFunction control_notify;
	PostgammaKernelControlTakeFunction control_take;
	PostgammaKernelControlCompleteFunction control_complete;
	PostgammaKernelFailFunction fail;
	void	   *log_context;
	PostgammaKernelEmitLogFunction emit_log;
} PostgammaKernelHostProvider;

#define POSTGAMMA_KERNEL_HOST_PROVIDER_INIT \
	{sizeof(PostgammaKernelHostProvider), \
	 POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION, UINT64_C(0), NULL, \
	 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}

typedef struct PostgammaKernelSetting
{
	const char *name;
	const char *value;
} PostgammaKernelSetting;

typedef enum PostgammaKernelFaultPoint
{
	POSTGAMMA_KERNEL_FAULT_NONE = 0,
	POSTGAMMA_KERNEL_FAULT_AFTER_ARGUMENTS,
	POSTGAMMA_KERNEL_FAULT_AFTER_INSTANCE_RUNTIME,
	POSTGAMMA_KERNEL_FAULT_AFTER_PATH_RUNTIME,
	POSTGAMMA_KERNEL_FAULT_AFTER_INSTANCE_CONTEXT,
	POSTGAMMA_KERNEL_FAULT_AFTER_ROLE_CONTEXT,
	POSTGAMMA_KERNEL_FAULT_AFTER_EXECUTION_CONTEXT,
	POSTGAMMA_KERNEL_FAULT_AFTER_CONTROL_RUNTIME,
	POSTGAMMA_KERNEL_FAULT_AFTER_MEMORY_CONTEXT,
	POSTGAMMA_KERNEL_FAULT_AFTER_GUC_INITIALIZATION,
	POSTGAMMA_KERNEL_FAULT_BEFORE_POSTMASTER,
	POSTGAMMA_KERNEL_FAULT_POINT_COUNT
} PostgammaKernelFaultPoint;

typedef int (*PostgammaKernelFaultCheckFunction) (
	void *context,
	uint64_t generation,
	PostgammaKernelFaultPoint point);

typedef struct PostgammaKernelFaultProvider
{
	uint32_t	struct_size;
	uint32_t	abi_version;
	void	   *context;
	PostgammaKernelFaultCheckFunction check;
} PostgammaKernelFaultProvider;

#define POSTGAMMA_KERNEL_FAULT_PROVIDER_INIT \
	{sizeof(PostgammaKernelFaultProvider), \
	 POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION, NULL, NULL}

typedef struct PostgammaKernelBootOptions
{
	uint32_t	struct_size;
	uint32_t	abi_version;
	uint64_t	generation;
	int32_t		data_directory_fd;
	uint32_t	logical_umask;
	const char *data_directory;
	const char *executable_path;
	const char *resource_root;
	const PostgammaKernelSetting *settings;
	size_t		setting_count;
	uint32_t	executor_kind;
	uint32_t	executor_worker_count;
	uint32_t	execution_queue_capacity;
	const PostgammaKernelHostProvider *host;
	const PostgammaKernelFaultProvider *faults;
} PostgammaKernelBootOptions;

#define POSTGAMMA_KERNEL_BOOT_OPTIONS_INIT \
	{sizeof(PostgammaKernelBootOptions), \
	 POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION, UINT64_C(0), -1, 0077, \
	 NULL, NULL, NULL, NULL, 0, POSTGAMMA_KERNEL_EXECUTOR_POOLED, 4, 0, \
	 NULL, NULL}

typedef struct PostgammaKernelResult
{
	uint32_t	struct_size;
	int			status;
	int			postgres_exit_code;
	int			cleanup_status;
	uint64_t	generation;
	void	   *runtime_handle;
	PostgammaKernelFaultPoint fault_point;
	char		phase[32];
	char		diagnostic[256];
} PostgammaKernelResult;

#define POSTGAMMA_KERNEL_RESULT_INIT \
	{sizeof(PostgammaKernelResult), 0, 0, 0, UINT64_C(0), NULL, \
	 POSTGAMMA_KERNEL_FAULT_NONE, {0}, {0}}

typedef struct PostgammaKernelInstanceTelemetry
{
	uint32_t	struct_size;
	uint32_t	executor_worker_count;
	uint64_t	running_sessions;
	uint64_t	pinned_sessions;
	uint64_t	runnable_sessions;
	uint64_t	queued_requests;
	uint64_t	parallel_tokens_in_use;
	uint64_t	execution_token_rejections;
	uint64_t	queue_wait_ns_max;
} PostgammaKernelInstanceTelemetry;

#define POSTGAMMA_KERNEL_INSTANCE_TELEMETRY_INIT \
	{sizeof(PostgammaKernelInstanceTelemetry), UINT32_C(0), UINT64_C(0), \
	 UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), \
	 UINT64_C(0)}

typedef struct PostgammaKernelGlobalTelemetry
{
	uint32_t	struct_size;
	uint64_t	instances_entered;
	uint64_t	instances_closed;
	uint64_t	instances_failed;
	uint64_t	cleanup_failures;
	uint64_t	active_instances;
	uint64_t	active_memory_contexts;
} PostgammaKernelGlobalTelemetry;

#define POSTGAMMA_KERNEL_GLOBAL_TELEMETRY_INIT \
	{sizeof(PostgammaKernelGlobalTelemetry), UINT64_C(0), UINT64_C(0), \
	 UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)}

typedef int (*PostgammaKernelInstanceMainFunction) (
	const PostgammaKernelBootOptions *options,
	PostgammaKernelResult *result);
typedef int (*PostgammaKernelInstanceTelemetryFunction) (
	void *runtime_handle, PostgammaKernelInstanceTelemetry *telemetry);

typedef struct PostgammaKernelEntrypoints
{
	uint32_t	struct_size;
	uint32_t	abi_version;
	uint64_t	capabilities;
	PostgammaKernelInstanceMainFunction instance_main;
	PostgammaKernelInstanceTelemetryFunction instance_telemetry;
} PostgammaKernelEntrypoints;

const PostgammaKernelEntrypoints *postgamma_embedded_kernel_entrypoints(void);
const char *postgamma_kernel_fault_point_name(PostgammaKernelFaultPoint point);
int postgamma_embedded_kernel_global_telemetry(
	PostgammaKernelGlobalTelemetry *telemetry);

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_EMBEDDED_KERNEL_H */
