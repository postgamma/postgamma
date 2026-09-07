/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_H
#define POSTGAMMA_H

/*
 * Stable host-facing C ABI for PostGamma embedded.
 *
 * The implementation is currently supported on POSIX hosts.  This header does
 * not expose PostgreSQL implementation types; all handles are opaque and all
 * transferred data has an explicit owner described below.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define PGM_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define PGM_API __attribute__((visibility("default")))
#else
#define PGM_API
#endif

#define PGM_ABI_VERSION_ENCODE(major, minor) \
	((((uint32_t) (major)) << 16) | \
	 ((uint32_t) (minor) & UINT32_C(0xffff)))
#define PGM_ABI_VERSION_MAJOR UINT32_C(1)
#define PGM_ABI_VERSION_MINOR UINT32_C(3)
#define PGM_ABI_VERSION \
	PGM_ABI_VERSION_ENCODE(PGM_ABI_VERSION_MAJOR, PGM_ABI_VERSION_MINOR)
#define PGM_NO_TIMEOUT INT64_C(-1)

/*
 * Handle ownership follows the hierarchy instance -> connection -> request.
 * A parent cannot be closed while a child handle remains open.  The caller
 * owns every handle returned through an output parameter and releases it with
 * the matching close or free function.
 */
typedef struct pgm_instance pgm_instance;
typedef struct pgm_connection pgm_connection;
typedef struct pgm_statement pgm_statement;
typedef struct pgm_request pgm_request;
typedef struct pgm_result pgm_result;
typedef struct pgm_copy pgm_copy;
typedef struct pgm_event pgm_event;
typedef struct pgm_operation pgm_operation;
typedef struct pgm_error pgm_error;

typedef uint64_t pgm_connection_id;
typedef uint64_t pgm_request_id;
typedef int32_t pgm_status;

/* Optional surfaces are used only when their capability bit is set. */
#define PGM_CAP_PREPARED_STATEMENTS       (UINT64_C(1) << 0)
#define PGM_CAP_CHUNKED_RESULTS           (UINT64_C(1) << 1)
#define PGM_CAP_COPY_IN                   (UINT64_C(1) << 2)
#define PGM_CAP_COPY_OUT                  (UINT64_C(1) << 3)
#define PGM_CAP_ARROW_C_DATA              (UINT64_C(1) << 4)
#define PGM_CAP_NOTIFICATIONS             (UINT64_C(1) << 5)
#define PGM_CAP_REQUEST_NOTICES           (UINT64_C(1) << 6)
#define PGM_CAP_STATUS_TELEMETRY          (UINT64_C(1) << 7)
#define PGM_CAP_INSTANCE_EVENTS           (UINT64_C(1) << 8)
#define PGM_CAP_MANAGEMENT_OPERATIONS     (UINT64_C(1) << 9)
#define PGM_CAP_MULTIPLE_INSTANCES        (UINT64_C(1) << 10)
#define PGM_CAP_NATIVE_EXTENSION_LOADING  (UINT64_C(1) << 11)
#define PGM_CAP_LOGICAL_BACKUP            (UINT64_C(1) << 12)
#define PGM_CAP_LOGICAL_RESTORE           (UINT64_C(1) << 13)
/* Reserved for a future implementation; ABI v1.3 does not advertise it. */
#define PGM_CAP_PHYSICAL_BACKUP           (UINT64_C(1) << 14)
#define PGM_CAP_MAINTENANCE               (UINT64_C(1) << 15)
#define PGM_CAP_BUNDLED_EXTENSIONS        (UINT64_C(1) << 16)

/*
 * Capabilities reported for one reviewed bundled extension.  Their exact
 * safety and runtime-permission semantics are defined by
 * postgamma_extension.h; this duplicate numeric vocabulary keeps host
 * introspection independent of the extension SDK header.
 */
#define PGM_EXTENSION_CAP_THREAD_SAFE             (UINT64_C(1) << 0)
#define PGM_EXTENSION_CAP_MULTI_INSTANCE_SAFE     (UINT64_C(1) << 1)
#define PGM_EXTENSION_CAP_SESSION_MOBILITY_SAFE   (UINT64_C(1) << 2)
#define PGM_EXTENSION_CAP_PARALLEL_WORKER_SAFE    (UINT64_C(1) << 3)
#define PGM_EXTENSION_CAP_INSTANCE_SHMEM          (UINT64_C(1) << 4)
#define PGM_EXTENSION_CAP_BACKGROUND_WORKER       (UINT64_C(1) << 5)
#define PGM_EXTENSION_CAP_FILESYSTEM_READ         (UINT64_C(1) << 6)
#define PGM_EXTENSION_CAP_FILESYSTEM_WRITE        (UINT64_C(1) << 7)
#define PGM_EXTENSION_CAP_HOST_LIBRARY_DEPENDENCY (UINT64_C(1) << 8)
#define PGM_EXTENSION_CAP_PROCESS_GLOBAL_STATE    (UINT64_C(1) << 9)

/*
 * Every function returning pgm_status uses one of these values.
 * PGM_STATUS_OUT_OF_MEMORY represents a recoverable host-boundary allocation
 * failure; internal contract violation is never used as its representation.
 */
#define PGM_STATUS_OK INT32_C(0)
#define PGM_STATUS_INVALID_ARGUMENT INT32_C(1)
#define PGM_STATUS_BUSY INT32_C(2)
#define PGM_STATUS_TIMEOUT INT32_C(3)
#define PGM_STATUS_CANCELED INT32_C(4)
#define PGM_STATUS_IO_ERROR INT32_C(5)
#define PGM_STATUS_POSTGRES_ERROR INT32_C(6)
#define PGM_STATUS_CONNECTION_FAILED INT32_C(7)
#define PGM_STATUS_INSTANCE_FAILED INT32_C(8)
#define PGM_STATUS_FORKED_PROCESS INT32_C(9)
#define PGM_STATUS_VERSION_MISMATCH INT32_C(10)
#define PGM_STATUS_UNSUPPORTED INT32_C(11)
#define PGM_STATUS_OUT_OF_MEMORY INT32_C(12)
#define PGM_STATUS_INTERNAL_ERROR INT32_C(13)
#define PGM_STATUS_REENTRANT_CALL INT32_C(14)

#define PGM_FORMAT_TEXT UINT16_C(0)
#define PGM_FORMAT_BINARY UINT16_C(1)

typedef int32_t pgm_shutdown_mode;

#define PGM_SHUTDOWN_SMART INT32_C(0)
#define PGM_SHUTDOWN_FAST INT32_C(1)
#define PGM_SHUTDOWN_IMMEDIATE INT32_C(2)

typedef int32_t pgm_request_state;

#define PGM_REQUEST_PENDING INT32_C(0)
#define PGM_REQUEST_RUNNING INT32_C(1)
#define PGM_REQUEST_COMPLETED INT32_C(2)
#define PGM_REQUEST_CANCELED INT32_C(3)
#define PGM_REQUEST_FAILED INT32_C(4)

typedef int32_t pgm_availability;

#define PGM_AVAILABILITY_READY INT32_C(0)
#define PGM_AVAILABILITY_AGAIN INT32_C(1)
#define PGM_AVAILABILITY_END INT32_C(2)

typedef int32_t pgm_io_state;

#define PGM_IO_PROGRESS INT32_C(0)
#define PGM_IO_AGAIN INT32_C(1)
#define PGM_IO_END INT32_C(2)

/*
 * Stream callbacks run synchronously on the thread calling
 * pgm_operation_progress and must not reenter the same instance.
 *
 * A read callback returns PGM_IO_PROGRESS with 0 < *produced <= capacity,
 * PGM_IO_AGAIN with *produced == 0, or PGM_IO_END with *produced == 0.
 * Final bytes are returned as PROGRESS; a later call reports END.
 *
 * A write callback returns PGM_IO_PROGRESS with 0 < *consumed <= size or
 * PGM_IO_AGAIN with *consumed == 0.  Partial progress is valid.  END is not
 * valid because PostGamma, rather than the sink, owns archive completion.
 * Any other state/count combination fails the operation.
 */
typedef pgm_io_state (*pgm_stream_read_callback) (
	void *user_data, void *buffer, size_t capacity, size_t *produced);
typedef pgm_io_state (*pgm_stream_write_callback) (
	void *user_data, const void *data, size_t size, size_t *consumed);

typedef int32_t pgm_result_status;

#define PGM_RESULT_COMMAND_OK INT32_C(0)
#define PGM_RESULT_TUPLES_OK INT32_C(1)
#define PGM_RESULT_COPY_IN INT32_C(2)
#define PGM_RESULT_COPY_OUT INT32_C(3)
#define PGM_RESULT_EMPTY_QUERY INT32_C(4)
#define PGM_RESULT_TUPLES_CHUNK INT32_C(5)
#define PGM_RESULT_ERROR INT32_C(6)

typedef int32_t pgm_transaction_status;

#define PGM_TRANSACTION_IDLE INT32_C(0)
#define PGM_TRANSACTION_ACTIVE INT32_C(1)
#define PGM_TRANSACTION_INTRANS INT32_C(2)
#define PGM_TRANSACTION_INERROR INT32_C(3)
#define PGM_TRANSACTION_UNKNOWN INT32_C(4)

#define PGM_PIN_TRANSACTION UINT32_C(1)
#define PGM_PIN_ADVISORY_LOCK UINT32_C(2)
#define PGM_PIN_PORTAL UINT32_C(4)
#define PGM_PIN_COPY UINT32_C(8)
#define PGM_PIN_OTHER UINT32_C(16)

typedef int32_t pgm_delivery_mode;

#define PGM_DELIVERY_MATERIALIZED INT32_C(0)
#define PGM_DELIVERY_CHUNKED INT32_C(1)

typedef int32_t pgm_event_kind;

#define PGM_EVENT_LOG INT32_C(0)
#define PGM_EVENT_NOTICE INT32_C(1)
#define PGM_EVENT_NOTIFICATION INT32_C(2)
#define PGM_EVENT_OVERFLOW INT32_C(3)

typedef int32_t pgm_diagnostic_field;

#define PGM_DIAG_SQLSTATE INT32_C(1)
#define PGM_DIAG_SEVERITY INT32_C(2)
#define PGM_DIAG_MESSAGE INT32_C(3)
#define PGM_DIAG_DETAIL INT32_C(4)
#define PGM_DIAG_HINT INT32_C(5)
#define PGM_DIAG_POSITION INT32_C(6)
#define PGM_DIAG_INTERNAL_POSITION INT32_C(7)
#define PGM_DIAG_INTERNAL_QUERY INT32_C(8)
#define PGM_DIAG_CONTEXT INT32_C(9)
#define PGM_DIAG_SCHEMA INT32_C(10)
#define PGM_DIAG_TABLE INT32_C(11)
#define PGM_DIAG_COLUMN INT32_C(12)
#define PGM_DIAG_DATATYPE INT32_C(13)
#define PGM_DIAG_CONSTRAINT INT32_C(14)
#define PGM_DIAG_SOURCE_FILE INT32_C(15)
#define PGM_DIAG_SOURCE_LINE INT32_C(16)
#define PGM_DIAG_SOURCE_FUNCTION INT32_C(17)

typedef int32_t pgm_operation_state;

#define PGM_OPERATION_PENDING INT32_C(0)
#define PGM_OPERATION_RUNNING INT32_C(1)
#define PGM_OPERATION_COMPLETED INT32_C(2)
#define PGM_OPERATION_CANCELED INT32_C(3)
#define PGM_OPERATION_FAILED INT32_C(4)

typedef int32_t pgm_operation_kind;

#define PGM_OPERATION_CHECKPOINT INT32_C(0)
#define PGM_OPERATION_LOGICAL_DUMP INT32_C(1)
#define PGM_OPERATION_LOGICAL_RESTORE INT32_C(2)
#define PGM_OPERATION_MAINTENANCE INT32_C(3)

typedef int32_t pgm_operation_phase;

#define PGM_OPERATION_PHASE_PENDING INT32_C(0)
#define PGM_OPERATION_PHASE_STARTING INT32_C(1)
#define PGM_OPERATION_PHASE_DATABASE INT32_C(2)
/* Reserved in ABI v1.2; no current operation reports this phase. */
#define PGM_OPERATION_PHASE_ARCHIVE_IO INT32_C(3)
#define PGM_OPERATION_PHASE_HOST_IO INT32_C(4)
/* Reserved in ABI v1.2; no current operation reports this phase. */
#define PGM_OPERATION_PHASE_FINALIZING INT32_C(5)
#define PGM_OPERATION_PHASE_TERMINAL INT32_C(6)

typedef int32_t pgm_maintenance_kind;

#define PGM_MAINTENANCE_VACUUM INT32_C(0)
#define PGM_MAINTENANCE_ANALYZE INT32_C(1)
#define PGM_MAINTENANCE_VACUUM_ANALYZE INT32_C(2)
#define PGM_MAINTENANCE_REINDEX_DATABASE INT32_C(3)

#define PGM_LOGICAL_SCHEMA_ONLY UINT32_C(0x0001)
#define PGM_LOGICAL_DATA_ONLY UINT32_C(0x0002)
#define PGM_LOGICAL_CLEAN UINT32_C(0x0004)
#define PGM_LOGICAL_CREATE UINT32_C(0x0008)
#define PGM_LOGICAL_NO_OWNER UINT32_C(0x0010)
#define PGM_LOGICAL_NO_PRIVILEGES UINT32_C(0x0020)
#define PGM_LOGICAL_FLAGS_ALL UINT32_C(0x003f)

typedef struct pgm_setting
{
	const char *name;
	const char *value;
} pgm_setting;

/* Borrowed immutable metadata for one SDK-conforming bundled extension. */
typedef struct pgm_bundled_extension_info
{
	uint32_t	struct_size;
	uint32_t	postgresql_major;
	uint32_t	sdk_abi_version;
	uint32_t	reserved;
	uint64_t	capabilities;
	const char *id;
	const char *sql_name;
	const char *version;
} pgm_bundled_extension_info;

#define PGM_BUNDLED_EXTENSION_INFO_INIT \
	{sizeof(pgm_bundled_extension_info), UINT32_C(0), UINT32_C(0), \
	 UINT32_C(0), UINT64_C(0), NULL, NULL, NULL}

typedef struct pgm_parameter
{
	uint32_t	struct_size;
	uint32_t	type_oid;
	uint16_t	format;
	uint16_t	is_null;
	const void *data;
	size_t		size;
} pgm_parameter;

/*
 * Initialize every public input/output structure with its PGM_*_INIT macro.
 * The library accepts a struct_size at least as large as the ABI v1 structure
 * and ignores trailing bytes.  A smaller structure is rejected.
 */
#define PGM_PARAMETER_INIT \
	{sizeof(pgm_parameter), UINT32_C(0), UINT16_C(0), UINT16_C(0), NULL, 0}

typedef struct pgm_log_record
{
	uint32_t	struct_size;
	int32_t		virtual_backend_pid;
	pgm_connection_id connection_id;
	pgm_request_id request_id;
	const char *severity;
	const char *sqlstate;
	const char *message;
	const char *detail;
} pgm_log_record;

#define PGM_LOG_RECORD_INIT \
	{sizeof(pgm_log_record), INT32_C(0), UINT64_C(0), UINT64_C(0), NULL, \
	 NULL, NULL, NULL}

typedef struct pgm_notice
{
	uint32_t	struct_size;
	const char *sqlstate;
	const char *severity;
	const char *message;
	const char *detail;
	const char *hint;
	pgm_connection_id connection_id;
	pgm_request_id request_id;
} pgm_notice;

#define PGM_NOTICE_INIT \
	{sizeof(pgm_notice), NULL, NULL, NULL, NULL, NULL, UINT64_C(0), \
	 UINT64_C(0)}

typedef struct pgm_notification
{
	uint32_t	struct_size;
	int32_t		virtual_backend_pid;
	const char *channel;
	const char *payload;
	pgm_connection_id connection_id;
} pgm_notification;

#define PGM_NOTIFICATION_INIT \
	{sizeof(pgm_notification), INT32_C(0), NULL, NULL, UINT64_C(0)}

typedef void (*pgm_log_callback) (
	void *user_data, const pgm_log_record *record);
typedef void (*pgm_notice_callback) (
	void *user_data, const pgm_notice *notice);
typedef void (*pgm_notification_callback) (
	void *user_data, const pgm_notification *notification);

/*
 * Callback arguments and strings are borrowed for the callback duration.
 * Callbacks run only on a host thread executing progress or dispatch and never
 * on a PostgreSQL role thread or while an internal lock is held.  Reentry into
 * the same instance is rejected with PGM_STATUS_REENTRANT_CALL.
 */

/*
 * executor_worker_count bounds concurrent carrier execution.  A connection
 * retains one worker while it is inside an explicit transaction or holds a
 * session-level advisory lock.  Consequently, the number of concurrently
 * executing pinned transactions cannot exceed executor_worker_count.
 */
typedef struct pgm_instance_options
{
	uint32_t	struct_size;
	uint32_t	create;
	const char *path;
	const char *executable_path;
	const char *resource_root;
	const pgm_setting *settings;
	size_t		setting_count;
	size_t		control_queue_capacity;
	size_t		transport_queue_capacity;
	uint32_t	executor_worker_count;
	uint32_t	execution_queue_capacity;
	uint32_t	logical_umask;
	uint32_t	reserved;
	void	   *user_data;
	size_t		result_buffer_limit;
	size_t		maximum_value_size;
	size_t		event_queue_capacity;
	pgm_log_callback log_callback;
	void	   *log_user_data;
} pgm_instance_options;

#define PGM_INSTANCE_OPTIONS_INIT \
	{sizeof(pgm_instance_options), UINT32_C(0), NULL, NULL, NULL, NULL, 0, \
	 64, 64U * 1024U, UINT32_C(4), UINT32_C(0), UINT32_C(0077), \
	 UINT32_C(0), NULL, 0, 0, 64, NULL, NULL}

typedef struct pgm_instance_telemetry
{
	uint32_t	struct_size;
	uint32_t	executor_worker_count;
	uint64_t	connection_count;
	uint64_t	active_request_count;
	uint64_t	running_session_count;
	uint64_t	pinned_session_count;
	uint64_t	runnable_session_count;
	uint64_t	queued_request_count;
	uint64_t	parallel_tokens_in_use;
	uint64_t	event_queue_depth;
	uint64_t	event_queue_capacity;
	uint64_t	request_count;
	uint64_t	completed_request_count;
	uint64_t	canceled_request_count;
	uint64_t	failed_request_count;
	uint64_t	execution_token_rejections;
	uint64_t	queue_wait_ns_max;
	uint64_t	dropped_log_count;
	uint64_t	dropped_notice_count;
	uint64_t	dropped_notification_count;
} pgm_instance_telemetry;

#define PGM_INSTANCE_TELEMETRY_INIT \
	{sizeof(pgm_instance_telemetry), UINT32_C(0), UINT64_C(0), UINT64_C(0), \
	 UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), \
	 UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), \
	 UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), \
	 UINT64_C(0)}

typedef struct pgm_event_overflow_record
{
	uint32_t	struct_size;
	pgm_event_kind dropped_kind;
	uint64_t	dropped_count;
} pgm_event_overflow_record;

#define PGM_EVENT_OVERFLOW_RECORD_INIT \
	{sizeof(pgm_event_overflow_record), PGM_EVENT_LOG, UINT64_C(0)}

typedef struct pgm_connection_options
{
	uint32_t	struct_size;
	uint32_t	reserved;
	const char *user;
	const char *database;
	const char *application_name;
	const pgm_setting *settings;
	size_t		setting_count;
	pgm_notice_callback notice_callback;
	pgm_notification_callback notification_callback;
	void	   *user_data;
} pgm_connection_options;

#define PGM_CONNECTION_OPTIONS_INIT \
	{sizeof(pgm_connection_options), UINT32_C(0), NULL, NULL, NULL, NULL, 0, \
	 NULL, NULL, NULL}

typedef struct pgm_connection_status_snapshot
{
	uint32_t	struct_size;
	pgm_transaction_status transaction_status;
	uint32_t	pin_reasons;
	uint32_t	carrier_retained;
	int32_t		virtual_backend_pid;
	pgm_connection_id connection_id;
	pgm_request_id active_request_id;
} pgm_connection_status_snapshot;

#define PGM_CONNECTION_STATUS_SNAPSHOT_INIT \
	{sizeof(pgm_connection_status_snapshot), PGM_TRANSACTION_UNKNOWN, \
	 UINT32_C(0), UINT32_C(0), INT32_C(0), UINT64_C(0), UINT64_C(0)}

typedef struct pgm_execute_options
{
	uint32_t	struct_size;
	uint32_t	flags;
	const pgm_parameter *parameters;
	size_t		parameter_count;
	uint16_t	result_format;
	uint16_t	reserved16;
	pgm_delivery_mode delivery_mode;
	uint32_t	target_chunk_rows;
	size_t		result_buffer_limit;
	pgm_notice_callback notice_callback;
	void	   *notice_user_data;
} pgm_execute_options;

#define PGM_EXECUTE_OPTIONS_INIT \
	{sizeof(pgm_execute_options), UINT32_C(0), NULL, 0, PGM_FORMAT_TEXT, \
	 UINT16_C(0), PGM_DELIVERY_MATERIALIZED, UINT32_C(0), 0, NULL, NULL}

typedef struct pgm_script_options
{
	uint32_t	struct_size;
	uint32_t	flags;
	pgm_delivery_mode delivery_mode;
	uint32_t	target_chunk_rows;
	size_t		result_buffer_limit;
	pgm_notice_callback notice_callback;
	void	   *notice_user_data;
} pgm_script_options;

#define PGM_SCRIPT_OPTIONS_INIT \
	{sizeof(pgm_script_options), UINT32_C(0), PGM_DELIVERY_MATERIALIZED, \
	 UINT32_C(0), 0, NULL, NULL}

typedef struct pgm_prepare_options
{
	uint32_t	struct_size;
	uint32_t	flags;
	const uint32_t *parameter_type_oids;
	size_t		parameter_count;
} pgm_prepare_options;

#define PGM_PREPARE_OPTIONS_INIT \
	{sizeof(pgm_prepare_options), UINT32_C(0), NULL, 0}

typedef struct pgm_statement_description
{
	uint32_t	struct_size;
	uint32_t	reserved;
	size_t		parameter_count;
	size_t		column_count;
} pgm_statement_description;

#define PGM_STATEMENT_DESCRIPTION_INIT \
	{sizeof(pgm_statement_description), UINT32_C(0), 0, 0}

typedef struct pgm_column
{
	uint32_t	struct_size;
	const char *name;
	uint32_t	table_oid;
	int32_t		table_column;
	uint32_t	type_oid;
	int32_t		type_size;
	int32_t		type_modifier;
	uint16_t	format;
	uint16_t	reserved;
} pgm_column;

#define PGM_COLUMN_INIT \
	{sizeof(pgm_column), NULL, UINT32_C(0), INT32_C(0), UINT32_C(0), \
	 INT32_C(0), INT32_C(0), PGM_FORMAT_TEXT, UINT16_C(0)}

typedef struct pgm_value_view
{
	uint32_t	struct_size;
	uint32_t	type_oid;
	uint16_t	format;
	uint16_t	is_null;
	const void *data;
	size_t		size;
} pgm_value_view;

#define PGM_VALUE_VIEW_INIT \
	{sizeof(pgm_value_view), UINT32_C(0), PGM_FORMAT_TEXT, UINT16_C(0), \
	 NULL, 0}

typedef struct pgm_checkpoint_options
{
	uint32_t	struct_size;
	uint32_t	flags;
} pgm_checkpoint_options;

#define PGM_CHECKPOINT_OPTIONS_INIT \
	{sizeof(pgm_checkpoint_options), UINT32_C(0)}

/* ABI v1.1 reserves checkpoint flags; callers must currently pass zero. */

typedef struct pgm_logical_dump_options
{
	uint32_t	struct_size;
	uint32_t	flags;
	const char *database;
	const char *user;
	size_t		channel_capacity;
	size_t		progress_quantum;
	pgm_stream_write_callback write;
	void	   *user_data;
} pgm_logical_dump_options;

#define PGM_LOGICAL_DUMP_OPTIONS_INIT \
	{sizeof(pgm_logical_dump_options), UINT32_C(0), NULL, NULL, \
	 256U * 1024U, 64U * 1024U, NULL, NULL}

typedef struct pgm_logical_restore_options
{
	uint32_t	struct_size;
	uint32_t	flags;
	const char *database;
	const char *user;
	size_t		channel_capacity;
	size_t		progress_quantum;
	pgm_stream_read_callback read;
	void	   *user_data;
} pgm_logical_restore_options;

#define PGM_LOGICAL_RESTORE_OPTIONS_INIT \
	{sizeof(pgm_logical_restore_options), UINT32_C(0), NULL, NULL, \
	 256U * 1024U, 64U * 1024U, NULL, NULL}

typedef struct pgm_maintenance_options
{
	uint32_t	struct_size;
	uint32_t	flags;
	pgm_maintenance_kind kind;
	uint32_t	reserved;
	const char *database;
	const char *user;
} pgm_maintenance_options;

#define PGM_MAINTENANCE_OPTIONS_INIT \
	{sizeof(pgm_maintenance_options), UINT32_C(0), \
	 PGM_MAINTENANCE_VACUUM, UINT32_C(0), NULL, NULL}

typedef struct pgm_operation_progress_snapshot
{
	uint32_t	struct_size;
	pgm_operation_kind kind;
	pgm_operation_state state;
	pgm_operation_phase phase;
	uint64_t	bytes_received;
	uint64_t	bytes_produced;
	uint64_t	total_bytes;
	uint64_t	objects_completed;
	uint64_t	objects_total;
} pgm_operation_progress_snapshot;

/*
 * total_bytes and objects_total are zero when no estimate is available.
 * objects_completed counts operation-defined completed units; maintenance
 * operations count consumed SQL command results.
 */

#define PGM_OPERATION_PROGRESS_SNAPSHOT_INIT \
	{sizeof(pgm_operation_progress_snapshot), PGM_OPERATION_CHECKPOINT, \
	 PGM_OPERATION_PENDING, PGM_OPERATION_PHASE_PENDING, UINT64_C(0), \
	 UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)}

PGM_API uint32_t pgm_abi_version(void);
PGM_API uint64_t pgm_capabilities(void);
PGM_API const char *pgm_postgresql_version(void);
PGM_API const char *pgm_build_id(void);
PGM_API const char *pgm_status_name(pgm_status status);
PGM_API size_t pgm_bundled_extension_count(void);
PGM_API pgm_status pgm_bundled_extension_get(
	size_t index, pgm_bundled_extension_info *info);

/*
 * Open a cluster directory.  When create is one, a missing cluster is created
 * in process and atomically published before startup.  When create is zero,
 * the directory must already contain a compatible cluster.  On success,
 * *instance is owned by the caller.  pgm_instance_close consumes it only when
 * the function returns PGM_STATUS_OK; a failed close may be retried.
 */
PGM_API pgm_status pgm_instance_open(
	const pgm_instance_options *options,
	pgm_instance **instance,
	pgm_error **error);
PGM_API pgm_status pgm_instance_close(
	pgm_instance *instance,
	pgm_shutdown_mode mode,
	int64_t timeout_ms,
	pgm_error **error);
PGM_API pgm_status pgm_instance_get_telemetry(
	pgm_instance *instance,
	pgm_instance_telemetry *telemetry,
	pgm_error **error);
PGM_API pgm_status pgm_instance_waitable(
	pgm_instance *instance,
	int *file_descriptor,
	pgm_error **error);
PGM_API pgm_status pgm_instance_next_event(
	pgm_instance *instance,
	int64_t timeout_ms,
	pgm_event **event,
	pgm_availability *availability,
	pgm_error **error);
PGM_API pgm_status pgm_instance_dispatch(
	pgm_instance *instance,
	size_t maximum_events,
	size_t *dispatched_events,
	pgm_error **error);

/* Events are owned values dequeued from the one instance event queue. */
PGM_API pgm_event_kind pgm_event_type(const pgm_event *event);
PGM_API pgm_connection_id pgm_event_connection(const pgm_event *event);
PGM_API pgm_request_id pgm_event_request(const pgm_event *event);
PGM_API pgm_status pgm_event_log(
	const pgm_event *event,
	pgm_log_record *record,
	pgm_error **error);
PGM_API pgm_status pgm_event_notice(
	const pgm_event *event,
	pgm_notice *notice,
	pgm_error **error);
PGM_API pgm_status pgm_event_notification(
	const pgm_event *event,
	pgm_notification *notification,
	pgm_error **error);
PGM_API pgm_status pgm_event_overflow(
	const pgm_event *event,
	pgm_event_overflow_record *overflow,
	pgm_error **error);
PGM_API void pgm_event_free(pgm_event *event);

/*
 * Different connections may execute concurrently.  Calls that mutate or
 * execute through the same connection must be externally serialized.  A
 * connection cannot close while its request handle exists, including after
 * that request has completed; free the request first.
 */
PGM_API pgm_status pgm_connection_open(
	pgm_instance *instance,
	const pgm_connection_options *options,
	pgm_connection **connection,
	pgm_error **error);
PGM_API pgm_status pgm_connection_close(
	pgm_connection *connection,
	int64_t timeout_ms,
	pgm_error **error);
PGM_API pgm_status pgm_connection_transaction_status(
	pgm_connection *connection,
	pgm_transaction_status *transaction_status,
	pgm_error **error);
PGM_API pgm_status pgm_connection_get_status(
	pgm_connection *connection,
	pgm_connection_status_snapshot *status,
	pgm_error **error);
PGM_API pgm_connection_id pgm_connection_identity(
	const pgm_connection *connection);
PGM_API pgm_status pgm_connection_parameter_status(
	pgm_connection *connection,
	const char *name,
	const char **value,
	pgm_error **error);

/*
 * A connection permits one in-flight request.  Polling or cancellation may be
 * performed by another host thread while no other operation uses that request
 * handle.  There must be only one waiter.  pgm_request_wait transfers an
 * available result and diagnostic to the caller; unclaimed objects remain
 * owned by the request and are released by pgm_request_free.  Freeing an
 * active request performs bounded cancellation.  If the request cannot be
 * retired safely, its transport is aborted and that connection rejects later
 * execution, while pgm_connection_close remains available for cleanup.
 */
PGM_API pgm_status pgm_execute_async(
	pgm_connection *connection,
	const char *sql,
	const pgm_parameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	pgm_request **request,
	pgm_error **error);
PGM_API pgm_status pgm_execute(
	pgm_connection *connection,
	const char *sql,
	const pgm_parameter *parameters,
	size_t parameter_count,
	uint16_t result_format,
	int64_t timeout_ms,
	pgm_result **result,
	pgm_error **error);
PGM_API pgm_status pgm_execute_async_ex(
	pgm_connection *connection,
	const char *sql,
	size_t sql_size,
	const pgm_execute_options *options,
	pgm_request **request,
	pgm_error **error);
PGM_API pgm_status pgm_execute_script_async(
	pgm_connection *connection,
	const char *sql,
	size_t sql_size,
	const pgm_script_options *options,
	pgm_request **request,
	pgm_error **error);

/* A prepared statement is owned by one logical connection. */
PGM_API pgm_status pgm_statement_prepare(
	pgm_connection *connection,
	const char *sql,
	size_t sql_size,
	const pgm_prepare_options *options,
	pgm_statement **statement,
	pgm_error **error);
PGM_API pgm_status pgm_statement_execute_async(
	pgm_statement *statement,
	const pgm_execute_options *options,
	pgm_request **request,
	pgm_error **error);
PGM_API pgm_status pgm_statement_describe(
	const pgm_statement *statement,
	pgm_statement_description *description,
	pgm_error **error);
PGM_API pgm_status pgm_statement_parameter_type(
	const pgm_statement *statement,
	size_t parameter_index,
	uint32_t *type_oid,
	pgm_error **error);
PGM_API pgm_status pgm_statement_column(
	const pgm_statement *statement,
	size_t column_index,
	pgm_column *column,
	pgm_error **error);
PGM_API pgm_status pgm_statement_close(
	pgm_statement *statement,
	pgm_error **error);

PGM_API pgm_status pgm_request_poll(
	pgm_request *request,
	pgm_request_state *state,
	pgm_error **error);
PGM_API pgm_status pgm_request_progress(
	pgm_request *request,
	pgm_request_state *state,
	pgm_error **error);
PGM_API pgm_status pgm_request_waitable(
	pgm_request *request,
	int *file_descriptor,
	pgm_error **error);
PGM_API pgm_status pgm_request_wait(
	pgm_request *request,
	int64_t timeout_ms,
	pgm_result **result,
	pgm_error **error);
PGM_API pgm_status pgm_request_cancel(
	pgm_request *request,
	pgm_error **error);
PGM_API pgm_status pgm_request_next_result(
	pgm_request *request,
	int64_t timeout_ms,
	pgm_result **result,
	pgm_availability *availability,
	pgm_error **error);
PGM_API pgm_request_id pgm_request_identity(const pgm_request *request);
PGM_API void pgm_request_free(pgm_request *request);

/* Column and value views borrow storage owned by result. */
PGM_API pgm_result_status pgm_result_kind(const pgm_result *result);
PGM_API size_t pgm_result_row_count(const pgm_result *result);
PGM_API size_t pgm_result_column_count(const pgm_result *result);
PGM_API const char *pgm_result_command_status(const pgm_result *result);
PGM_API pgm_status pgm_result_column(
	const pgm_result *result,
	size_t column_index,
	pgm_column *column,
	pgm_error **error);
PGM_API pgm_status pgm_result_value(
	const pgm_result *result,
	size_t row_index,
	size_t column_index,
	pgm_value_view *value,
	pgm_error **error);
PGM_API pgm_status pgm_result_take_copy(
	pgm_result **result,
	pgm_copy **copy,
	pgm_error **error);
PGM_API void pgm_result_free(pgm_result *result);

/*
 * COPY I/O is partial and reports ordinary backpressure through pgm_io_state.
 * One call owns the handle through host callback dispatch; another concurrent
 * COPY I/O or close call fails with PGM_STATUS_BUSY.
 */
PGM_API pgm_status pgm_copy_write(
	pgm_copy *copy,
	const void *data,
	size_t size,
	size_t *consumed,
	pgm_io_state *state,
	pgm_error **error);
PGM_API pgm_status pgm_copy_finish(
	pgm_copy *copy,
	const char *failure_message,
	size_t failure_message_size,
	pgm_io_state *state,
	pgm_error **error);
PGM_API pgm_status pgm_copy_read(
	pgm_copy *copy,
	void *buffer,
	size_t capacity,
	size_t *produced,
	pgm_io_state *state,
	pgm_error **error);
PGM_API pgm_status pgm_copy_close(
	pgm_copy *copy,
	int64_t timeout_ms,
	pgm_error **error);
PGM_API pgm_status pgm_copy_abort(
	pgm_copy *copy,
	const char *message,
	size_t message_size,
	pgm_error **error);

/*
 * For every function with a pgm_error ** parameter, *error is cleared before
 * work begins.  On a non-OK return the library attempts to provide an owned
 * diagnostic whose status matches the return value.  It can remain NULL only
 * when the caller passed NULL or diagnostic allocation itself failed.  Release
 * a returned diagnostic with pgm_error_free.
 */
PGM_API pgm_status pgm_error_status(const pgm_error *error);
PGM_API const char *pgm_error_sqlstate(const pgm_error *error);
PGM_API const char *pgm_error_severity(const pgm_error *error);
PGM_API const char *pgm_error_message(const pgm_error *error);
PGM_API const char *pgm_error_detail(const pgm_error *error);
/* Returns NULL when the diagnostic is absent or field is outside ABI v1. */
PGM_API const char *pgm_error_field(
	const pgm_error *error,
	pgm_diagnostic_field field);
PGM_API void pgm_error_free(pgm_error *error);

/*
 * Every operation handle belongs to one instance.  ABI v1.2 additionally
 * serializes logical dump and restore admission across the process: while one
 * logical operation handle owns that reservation, another logical submission
 * returns PGM_STATUS_BUSY, even for another instance.  This implementation
 * limit may be relaxed by a later additive release.
 *
 * Logical operations use an in-memory archive transport and maintenance owns
 * a private in-memory SQL connection; neither path opens a network endpoint or
 * launches a subprocess.  The waitable descriptor is stable for the lifetime
 * of an operation handle.  Call progress once to submit pending work, then
 * wait on the level-triggered descriptor and recheck state.  One thread owns
 * progress/wait at a time; a concurrent progress attempt returns
 * PGM_STATUS_BUSY and still writes a synchronized state snapshot.  One other
 * thread may request cancellation.  pgm_operation_free must not run
 * concurrently with another call that uses the same handle.  A dispatched
 * checkpoint is not cancelable.
 */
PGM_API pgm_status pgm_instance_checkpoint_async(
	pgm_instance *instance,
	const pgm_checkpoint_options *options,
	pgm_operation **operation,
	pgm_error **error);
PGM_API pgm_status pgm_instance_logical_dump_async(
	pgm_instance *instance,
	const pgm_logical_dump_options *options,
	pgm_operation **operation,
	pgm_error **error);
PGM_API pgm_status pgm_instance_logical_restore_async(
	pgm_instance *instance,
	const pgm_logical_restore_options *options,
	pgm_operation **operation,
	pgm_error **error);
PGM_API pgm_status pgm_instance_maintenance_async(
	pgm_instance *instance,
	const pgm_maintenance_options *options,
	pgm_operation **operation,
	pgm_error **error);
PGM_API pgm_status pgm_operation_progress(
	pgm_operation *operation,
	pgm_operation_state *state,
	pgm_error **error);
PGM_API pgm_status pgm_operation_get_progress(
	pgm_operation *operation,
	pgm_operation_progress_snapshot *progress,
	pgm_error **error);
PGM_API pgm_status pgm_operation_waitable(
	pgm_operation *operation,
	int *file_descriptor,
	pgm_error **error);
PGM_API pgm_status pgm_operation_cancel(
	pgm_operation *operation,
	pgm_error **error);
PGM_API void pgm_operation_free(pgm_operation *operation);

#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_H */
