/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_PUBLIC_RUNTIME_H
#define POSTGAMMA_PRIVATE_PUBLIC_RUNTIME_H

#include "postgamma/postgamma.h"

#include "postgamma/embedded_kernel.h"
#include "postgamma/instance_runtime.h"
#include "postgamma/private/data_directory_lock.h"
#include "postgamma/private/instance_open_bridge.h"
#include "postgamma/private/libpq_memory_adapter.h"
#include "postgamma/private/logical_tool_host.h"
#include "postgamma/private/memory_transport.h"
#include "postgamma/private/supervisor.h"
#include "postgamma/thread_runtime.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <sys/types.h>

#define PGM_INSTANCE_MAGIC UINT64_C(0x50474D494E535431)
#define PGM_CONNECTION_MAGIC UINT64_C(0x50474D434F4E4E31)
#define PGM_STATEMENT_MAGIC UINT64_C(0x50474D53544D5431)
#define PGM_REQUEST_MAGIC UINT64_C(0x50474D5245515531)
#define PGM_RESULT_MAGIC UINT64_C(0x50474D5245535531)
#define PGM_COPY_MAGIC UINT64_C(0x50474D434F505931)
#define PGM_EVENT_MAGIC UINT64_C(0x50474D45564E5431)
#define PGM_OPERATION_MAGIC UINT64_C(0x50474D4F50455231)
#define PGM_ERROR_MAGIC UINT64_C(0x50474D4552524F31)
#define PGM_DIAGNOSTIC_FIELD_COUNT 17U

typedef struct PostgammaPublicEventRouter PostgammaPublicEventRouter;

typedef struct PostgammaPublicCallbackFrame
{
	pgm_instance *instance;
	struct PostgammaPublicCallbackFrame *previous;
} PostgammaPublicCallbackFrame;

typedef enum PostgammaPublicRequestMode
{
	POSTGAMMA_PUBLIC_REQUEST_EXTENDED = 0,
	POSTGAMMA_PUBLIC_REQUEST_SCRIPT,
	POSTGAMMA_PUBLIC_REQUEST_PREPARED
} PostgammaPublicRequestMode;

typedef struct PostgammaPublicResultLease
{
	_Atomic unsigned int references;
	_Atomic bool outstanding;
	struct pgm_request *owner;
} PostgammaPublicResultLease;

typedef enum PostgammaPublicCopyState
{
	POSTGAMMA_PUBLIC_COPY_ACTIVE = 0,
	POSTGAMMA_PUBLIC_COPY_FINISHING,
	POSTGAMMA_PUBLIC_COPY_ABORTED,
	POSTGAMMA_PUBLIC_COPY_ENDED
} PostgammaPublicCopyState;

struct pgm_error
{
	uint64_t	magic;
	pgm_status	status;
	char	   *fields[PGM_DIAGNOSTIC_FIELD_COUNT];
};

struct pgm_result
{
	uint64_t	magic;
	PostgammaPrivateOwnedResult *private_result;
	PostgammaPublicResultLease *lease;
};

struct pgm_copy
{
	uint64_t	magic;
	pid_t		owner_pid;
	PostgammaMutex *mutex;
	_Atomic bool io_owner_active;
	struct pgm_request *request;
	PostgammaPublicResultLease *lease;
	PostgammaPrivateCopyDirection direction;
	PostgammaPublicCopyState state;
	char	   *finish_message;
	size_t		finish_message_size;
	bool		finish_message_set;
	bool		io_ended;
};

struct pgm_operation
{
	uint64_t	magic;
	pid_t		owner_pid;
	pgm_instance *instance;
	PostgammaMutex *mutex;
	PostgammaCheckpointTracker *tracker;
	PostgammaSupervisorTicket *ticket;
	PostgammaKernelCheckpointRequest checkpoint_request;
	PostgammaLogicalToolWorker *logical_worker;
	PostgammaMemoryEndpoint *archive_host_endpoint;
	PostgammaMemoryEndpoint *archive_worker_endpoint;
	pgm_connection *management_connection;
	pgm_request *management_request;
	pgm_operation_kind kind;
	pgm_operation_state state;
	pgm_operation_phase phase;
	pgm_status	operation_status;
	uint32_t	flags;
	uint64_t	archive_generation;
	uint64_t	bytes_received;
	uint64_t	bytes_produced;
	uint64_t	total_bytes;
	uint64_t	objects_completed;
	uint64_t	objects_total;
	size_t		channel_capacity;
	size_t		progress_quantum;
	unsigned char *stream_buffer;
	size_t		stream_buffer_offset;
	size_t		stream_buffer_size;
	char	   *database;
	char	   *user;
	char	   *maintenance_sql;
	pgm_stream_read_callback stream_read;
	pgm_stream_write_callback stream_write;
	void	   *stream_user_data;
	char		diagnostic[256];
	_Atomic uint32_t lifecycle_state;
	bool		worker_done;
	bool		stream_end;
	bool		cancel_requested;
	bool		logical_reserved;
	bool		free_pending;
	bool		registered_with_instance;
};

struct pgm_instance
{
	uint64_t	magic;
	pid_t		owner_pid;
	uint64_t	generation;
	PostgammaMutex *mutex;
	PostgammaDataDirectoryLock *data_lock;
	PostgammaSupervisor *supervisor;
	PostgammaSupervisorTicket *shutdown_ticket;
	PostgammaKernelHostProvider host;
	PostgammaKernelBootOptions boot_options;
	PostgammaKernelResult kernel_result;
	const PostgammaKernelEntrypoints *entrypoints;
	char	   *data_directory;
	char	   *executable_path;
	char	   *resource_root;
	PostgammaKernelSetting *settings;
	size_t		setting_count;
	PostgammaPublicEventRouter *event_router;
	size_t		transport_queue_capacity;
	size_t		result_buffer_limit;
	size_t		maximum_value_size;
	size_t		connection_count;
	size_t		operation_count;
	uint32_t	executor_worker_count;
	_Atomic uint64_t next_connection_id;
	_Atomic uint64_t next_statement_id;
	_Atomic uint64_t next_request_generation;
	_Atomic uint64_t active_request_count;
	_Atomic uint64_t request_count;
	_Atomic uint64_t completed_request_count;
	_Atomic uint64_t canceled_request_count;
	_Atomic uint64_t failed_request_count;
	bool		shutdown_requested;
	bool		closing;
};

struct pgm_connection
{
	uint64_t	magic;
	pid_t		owner_pid;
	pgm_instance *instance;
	PostgammaMutex *mutex;
	PostgammaPrivateLibpqConnection *private_connection;
	PostgammaMemoryEndpoint *backend_monitor;
	struct pgm_request *active_request;
	pgm_connection_id identity;
	size_t		statement_count;
	int			backend_pid;
	pgm_notice_callback notice_callback;
	pgm_notification_callback notification_callback;
	void	   *callback_user_data;
	struct pgm_connection *event_registry_next;
	struct pgm_connection *event_ready_previous;
	struct pgm_connection *event_ready_next;
	_Atomic(struct pgm_connection *) event_pending_next;
	_Atomic bool event_ready_pending;
	unsigned int event_pump_references;
	bool		event_ready_queued;
	bool		event_registered;
	bool		registered_with_instance;
	bool		failed;
	bool		closing;
};

struct pgm_statement
{
	uint64_t	magic;
	pid_t		owner_pid;
	pgm_connection *connection;
	char	   *name;
	PostgammaPrivatePreparedDescription *description;
	struct pgm_request *active_request;
	bool		registered_with_connection;
};

struct pgm_request
{
	uint64_t	magic;
	pid_t		owner_pid;
	pgm_connection *connection;
	pgm_statement *statement;
	PostgammaMutex *mutex;
	PostgammaPrivateLibpqOperation *operation;
	PostgammaPublicResultLease *result_lease;
	struct pgm_copy *active_copy;
	PostgammaPublicRequestMode mode;
	pgm_request_state state;
	pgm_status	operation_status;
	pgm_result *result;
	pgm_error  *error;
	char	   *sql;
	PostgammaPrivateParameter *parameters;
	size_t		parameter_count;
	size_t		result_count;
	size_t		result_buffer_limit;
	size_t		maximum_value_size;
	uint16_t	result_format;
	pgm_delivery_mode delivery_mode;
	uint32_t	target_chunk_rows;
	uint64_t	generation;
	pgm_notice_callback notice_callback;
	void	   *notice_user_data;
	bool		cancel_requested;
	bool		saw_postgres_error;
	bool		saw_cancel_error;
	bool		terminal_accounted;
	bool		free_pending;
};

bool instance_is_valid(const pgm_instance *instance);
bool connection_is_valid(const pgm_connection *connection);
bool statement_is_valid(const pgm_statement *statement);
bool request_is_valid(const pgm_request *request);
bool result_is_valid(const pgm_result *result);
bool copy_is_valid(const pgm_copy *copy);
bool error_is_valid(const pgm_error *error);
bool operation_is_valid(const pgm_operation *operation);
bool process_is_valid(pid_t owner_pid);
bool public_instance_reentrant(const pgm_instance *instance);
void postgamma_public_callback_enter(
	PostgammaPublicCallbackFrame *frame, pgm_instance *instance);
void postgamma_public_callback_leave(PostgammaPublicCallbackFrame *frame);
char *duplicate_sql(const char *sql, size_t sql_size);
pgm_error *make_error(
	pgm_status status, const char *sqlstate, const char *severity,
	const char *detail, const char *format, ...);
pgm_error *make_error_from_diagnostics(
	pgm_status status, const char *const *fields, size_t field_count);
void return_error(pgm_error **target, pgm_error *error);
pgm_status return_simple_error(
	pgm_error **target, pgm_status status, const char *message);
pgm_status status_from_private(PostgammaPrivateLibpqStatus status);
int deadline_from_timeout(int64_t timeout_ms, uint64_t *deadline_ns);
int private_deadline_after(int64_t interval_ns, int64_t *deadline_ns);
pgm_status request_start(
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
	pgm_error **error);
pgm_status progress_request_locked(pgm_request *request);
pgm_error *error_from_result(
	pgm_status status, const PostgammaPrivateOwnedResult *result);
pgm_status result_attach_lease(
	pgm_result *result, PostgammaPublicResultLease *lease);
void result_lease_release(PostgammaPublicResultLease *lease);
void request_destroy(pgm_request *request);
void request_account_terminal(pgm_request *request);

bool postgamma_logical_operation_kind(pgm_operation_kind kind);
pgm_status postgamma_logical_operation_progress(
	pgm_operation *operation, pgm_operation_state *state, pgm_error **error);
pgm_status postgamma_logical_operation_waitable(
	pgm_operation *operation, int *file_descriptor, pgm_error **error);
pgm_status postgamma_logical_operation_cancel(
	pgm_operation *operation, pgm_error **error);
void postgamma_logical_operation_free(pgm_operation *operation);

int postgamma_public_event_router_create(
	pgm_instance *instance, size_t capacity,
	pgm_log_callback log_callback, void *log_user_data);
int postgamma_public_event_router_destroy(pgm_instance *instance);
int postgamma_public_event_register_connection(pgm_connection *connection);
int postgamma_public_event_begin_connection_close(pgm_connection *connection);
void postgamma_public_event_cancel_connection_close(
	pgm_connection *connection);
void postgamma_public_event_finish_connection_close(
	pgm_connection *connection);
void postgamma_public_event_forget_request(pgm_request *request);
void postgamma_public_event_notify_connection(
	pgm_connection *connection, uint32_t events);
int postgamma_public_event_waitable_fd(pgm_instance *instance);
void postgamma_public_event_refresh_connection(pgm_connection *connection);
void postgamma_public_event_enqueue_notice(
	pgm_connection *connection,
	const char *sqlstate, const char *severity, const char *message,
	const char *detail, const char *hint);
void postgamma_public_event_enqueue_notification(
	pgm_connection *connection, int backend_pid,
	const char *channel, const char *payload);
int postgamma_public_event_enqueue_kernel_log(
	void *argument, uint64_t generation,
	const PostgammaKernelLogRecord *record);
void postgamma_public_dispatch_progress_events(pgm_request *request);
void postgamma_public_dispatch_connection_events(
	pgm_connection *connection, pgm_request_id request_id);
int postgamma_public_event_telemetry(
	pgm_instance *instance,
	uint64_t *queue_depth, uint64_t *queue_capacity,
	uint64_t *dropped_logs, uint64_t *dropped_notices,
	uint64_t *dropped_notifications);

#endif /* POSTGAMMA_PRIVATE_PUBLIC_RUNTIME_H */
