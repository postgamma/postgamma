#include "postgamma/postgamma.h"
#include "postgamma/postgamma_arrow.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__cplusplus)
#define PGM_TEST_ALIGNOF(type) alignof(type)
#else
#define PGM_TEST_ALIGNOF(type) _Alignof(type)
#endif

#define PGM_TEST_NUMERIC(name) \
	(void) printf("numeric %s %" PRIu64 "\n", #name, (uint64_t) (name))
#define PGM_TEST_LAYOUT(type) \
	(void) printf("layout %s %zu %zu\n", #type, sizeof(type), \
		(size_t) PGM_TEST_ALIGNOF(type))
#define PGM_TEST_FIELD(type, field) \
	(void) printf("field %s %s %zu\n", #type, #field, offsetof(type, field))

static int
check_initializers(void)
{
	pgm_parameter parameter = PGM_PARAMETER_INIT;
	pgm_bundled_extension_info extension =
		PGM_BUNDLED_EXTENSION_INFO_INIT;
	pgm_log_record log_record = PGM_LOG_RECORD_INIT;
	pgm_notice notice = PGM_NOTICE_INIT;
	pgm_notification notification = PGM_NOTIFICATION_INIT;
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_instance_telemetry telemetry = PGM_INSTANCE_TELEMETRY_INIT;
	pgm_event_overflow_record overflow = PGM_EVENT_OVERFLOW_RECORD_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_connection_status_snapshot connection_status =
		PGM_CONNECTION_STATUS_SNAPSHOT_INIT;
	pgm_execute_options execute_options = PGM_EXECUTE_OPTIONS_INIT;
	pgm_script_options script_options = PGM_SCRIPT_OPTIONS_INIT;
	pgm_prepare_options prepare_options = PGM_PREPARE_OPTIONS_INIT;
	pgm_statement_description statement_description =
		PGM_STATEMENT_DESCRIPTION_INIT;
	pgm_column column = PGM_COLUMN_INIT;
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	pgm_checkpoint_options checkpoint = PGM_CHECKPOINT_OPTIONS_INIT;
	pgm_logical_dump_options logical_dump = PGM_LOGICAL_DUMP_OPTIONS_INIT;
	pgm_logical_restore_options logical_restore =
		PGM_LOGICAL_RESTORE_OPTIONS_INIT;
	pgm_maintenance_options maintenance = PGM_MAINTENANCE_OPTIONS_INIT;
	pgm_operation_progress_snapshot operation_progress =
		PGM_OPERATION_PROGRESS_SNAPSHOT_INIT;

	return parameter.struct_size == sizeof(parameter) &&
		extension.struct_size == sizeof(extension) &&
		log_record.struct_size == sizeof(log_record) &&
		notice.struct_size == sizeof(notice) &&
		notification.struct_size == sizeof(notification) &&
		instance_options.struct_size == sizeof(instance_options) &&
		telemetry.struct_size == sizeof(telemetry) &&
		overflow.struct_size == sizeof(overflow) &&
		connection_options.struct_size == sizeof(connection_options) &&
		connection_status.struct_size == sizeof(connection_status) &&
		execute_options.struct_size == sizeof(execute_options) &&
		script_options.struct_size == sizeof(script_options) &&
		prepare_options.struct_size == sizeof(prepare_options) &&
		statement_description.struct_size == sizeof(statement_description) &&
		column.struct_size == sizeof(column) &&
		value.struct_size == sizeof(value) &&
		checkpoint.struct_size == sizeof(checkpoint) &&
		logical_dump.struct_size == sizeof(logical_dump) &&
		logical_restore.struct_size == sizeof(logical_restore) &&
		maintenance.struct_size == sizeof(maintenance) &&
		operation_progress.struct_size == sizeof(operation_progress);
}

static void
print_numerics(void)
{
	PGM_TEST_NUMERIC(PGM_ABI_VERSION);
	PGM_TEST_NUMERIC(PGM_ABI_VERSION_MAJOR);
	PGM_TEST_NUMERIC(PGM_ABI_VERSION_MINOR);
	PGM_TEST_NUMERIC(PGM_STATUS_OK);
	PGM_TEST_NUMERIC(PGM_STATUS_INVALID_ARGUMENT);
	PGM_TEST_NUMERIC(PGM_STATUS_BUSY);
	PGM_TEST_NUMERIC(PGM_STATUS_TIMEOUT);
	PGM_TEST_NUMERIC(PGM_STATUS_CANCELED);
	PGM_TEST_NUMERIC(PGM_STATUS_IO_ERROR);
	PGM_TEST_NUMERIC(PGM_STATUS_POSTGRES_ERROR);
	PGM_TEST_NUMERIC(PGM_STATUS_CONNECTION_FAILED);
	PGM_TEST_NUMERIC(PGM_STATUS_INSTANCE_FAILED);
	PGM_TEST_NUMERIC(PGM_STATUS_FORKED_PROCESS);
	PGM_TEST_NUMERIC(PGM_STATUS_VERSION_MISMATCH);
	PGM_TEST_NUMERIC(PGM_STATUS_UNSUPPORTED);
	PGM_TEST_NUMERIC(PGM_STATUS_OUT_OF_MEMORY);
	PGM_TEST_NUMERIC(PGM_STATUS_INTERNAL_ERROR);
	PGM_TEST_NUMERIC(PGM_STATUS_REENTRANT_CALL);
	PGM_TEST_NUMERIC(PGM_FORMAT_TEXT);
	PGM_TEST_NUMERIC(PGM_FORMAT_BINARY);
	PGM_TEST_NUMERIC(PGM_SHUTDOWN_SMART);
	PGM_TEST_NUMERIC(PGM_SHUTDOWN_FAST);
	PGM_TEST_NUMERIC(PGM_SHUTDOWN_IMMEDIATE);
	PGM_TEST_NUMERIC(PGM_REQUEST_PENDING);
	PGM_TEST_NUMERIC(PGM_REQUEST_RUNNING);
	PGM_TEST_NUMERIC(PGM_REQUEST_COMPLETED);
	PGM_TEST_NUMERIC(PGM_REQUEST_CANCELED);
	PGM_TEST_NUMERIC(PGM_REQUEST_FAILED);
	PGM_TEST_NUMERIC(PGM_AVAILABILITY_READY);
	PGM_TEST_NUMERIC(PGM_AVAILABILITY_AGAIN);
	PGM_TEST_NUMERIC(PGM_AVAILABILITY_END);
	PGM_TEST_NUMERIC(PGM_IO_PROGRESS);
	PGM_TEST_NUMERIC(PGM_IO_AGAIN);
	PGM_TEST_NUMERIC(PGM_IO_END);
	PGM_TEST_NUMERIC(PGM_RESULT_COMMAND_OK);
	PGM_TEST_NUMERIC(PGM_RESULT_TUPLES_OK);
	PGM_TEST_NUMERIC(PGM_RESULT_COPY_IN);
	PGM_TEST_NUMERIC(PGM_RESULT_COPY_OUT);
	PGM_TEST_NUMERIC(PGM_RESULT_EMPTY_QUERY);
	PGM_TEST_NUMERIC(PGM_RESULT_TUPLES_CHUNK);
	PGM_TEST_NUMERIC(PGM_RESULT_ERROR);
	PGM_TEST_NUMERIC(PGM_TRANSACTION_IDLE);
	PGM_TEST_NUMERIC(PGM_TRANSACTION_ACTIVE);
	PGM_TEST_NUMERIC(PGM_TRANSACTION_INTRANS);
	PGM_TEST_NUMERIC(PGM_TRANSACTION_INERROR);
	PGM_TEST_NUMERIC(PGM_TRANSACTION_UNKNOWN);
	PGM_TEST_NUMERIC(PGM_PIN_TRANSACTION);
	PGM_TEST_NUMERIC(PGM_PIN_ADVISORY_LOCK);
	PGM_TEST_NUMERIC(PGM_PIN_PORTAL);
	PGM_TEST_NUMERIC(PGM_PIN_COPY);
	PGM_TEST_NUMERIC(PGM_PIN_OTHER);
	PGM_TEST_NUMERIC(PGM_DELIVERY_MATERIALIZED);
	PGM_TEST_NUMERIC(PGM_DELIVERY_CHUNKED);
	PGM_TEST_NUMERIC(PGM_EVENT_LOG);
	PGM_TEST_NUMERIC(PGM_EVENT_NOTICE);
	PGM_TEST_NUMERIC(PGM_EVENT_NOTIFICATION);
	PGM_TEST_NUMERIC(PGM_EVENT_OVERFLOW);
	PGM_TEST_NUMERIC(PGM_DIAG_SQLSTATE);
	PGM_TEST_NUMERIC(PGM_DIAG_SEVERITY);
	PGM_TEST_NUMERIC(PGM_DIAG_MESSAGE);
	PGM_TEST_NUMERIC(PGM_DIAG_DETAIL);
	PGM_TEST_NUMERIC(PGM_DIAG_HINT);
	PGM_TEST_NUMERIC(PGM_DIAG_POSITION);
	PGM_TEST_NUMERIC(PGM_DIAG_INTERNAL_POSITION);
	PGM_TEST_NUMERIC(PGM_DIAG_INTERNAL_QUERY);
	PGM_TEST_NUMERIC(PGM_DIAG_CONTEXT);
	PGM_TEST_NUMERIC(PGM_DIAG_SCHEMA);
	PGM_TEST_NUMERIC(PGM_DIAG_TABLE);
	PGM_TEST_NUMERIC(PGM_DIAG_COLUMN);
	PGM_TEST_NUMERIC(PGM_DIAG_DATATYPE);
	PGM_TEST_NUMERIC(PGM_DIAG_CONSTRAINT);
	PGM_TEST_NUMERIC(PGM_DIAG_SOURCE_FILE);
	PGM_TEST_NUMERIC(PGM_DIAG_SOURCE_LINE);
	PGM_TEST_NUMERIC(PGM_DIAG_SOURCE_FUNCTION);
	PGM_TEST_NUMERIC(PGM_OPERATION_PENDING);
	PGM_TEST_NUMERIC(PGM_OPERATION_RUNNING);
	PGM_TEST_NUMERIC(PGM_OPERATION_COMPLETED);
	PGM_TEST_NUMERIC(PGM_OPERATION_CANCELED);
	PGM_TEST_NUMERIC(PGM_OPERATION_FAILED);
	PGM_TEST_NUMERIC(PGM_OPERATION_CHECKPOINT);
	PGM_TEST_NUMERIC(PGM_OPERATION_LOGICAL_DUMP);
	PGM_TEST_NUMERIC(PGM_OPERATION_LOGICAL_RESTORE);
	PGM_TEST_NUMERIC(PGM_OPERATION_MAINTENANCE);
	PGM_TEST_NUMERIC(PGM_OPERATION_PHASE_PENDING);
	PGM_TEST_NUMERIC(PGM_OPERATION_PHASE_STARTING);
	PGM_TEST_NUMERIC(PGM_OPERATION_PHASE_DATABASE);
	PGM_TEST_NUMERIC(PGM_OPERATION_PHASE_ARCHIVE_IO);
	PGM_TEST_NUMERIC(PGM_OPERATION_PHASE_HOST_IO);
	PGM_TEST_NUMERIC(PGM_OPERATION_PHASE_FINALIZING);
	PGM_TEST_NUMERIC(PGM_OPERATION_PHASE_TERMINAL);
	PGM_TEST_NUMERIC(PGM_MAINTENANCE_VACUUM);
	PGM_TEST_NUMERIC(PGM_MAINTENANCE_ANALYZE);
	PGM_TEST_NUMERIC(PGM_MAINTENANCE_VACUUM_ANALYZE);
	PGM_TEST_NUMERIC(PGM_MAINTENANCE_REINDEX_DATABASE);
	PGM_TEST_NUMERIC(PGM_LOGICAL_SCHEMA_ONLY);
	PGM_TEST_NUMERIC(PGM_LOGICAL_DATA_ONLY);
	PGM_TEST_NUMERIC(PGM_LOGICAL_CLEAN);
	PGM_TEST_NUMERIC(PGM_LOGICAL_CREATE);
	PGM_TEST_NUMERIC(PGM_LOGICAL_NO_OWNER);
	PGM_TEST_NUMERIC(PGM_LOGICAL_NO_PRIVILEGES);
	PGM_TEST_NUMERIC(PGM_LOGICAL_FLAGS_ALL);
	PGM_TEST_NUMERIC(PGM_CAP_PREPARED_STATEMENTS);
	PGM_TEST_NUMERIC(PGM_CAP_CHUNKED_RESULTS);
	PGM_TEST_NUMERIC(PGM_CAP_COPY_IN);
	PGM_TEST_NUMERIC(PGM_CAP_COPY_OUT);
	PGM_TEST_NUMERIC(PGM_CAP_ARROW_C_DATA);
	PGM_TEST_NUMERIC(PGM_CAP_NOTIFICATIONS);
	PGM_TEST_NUMERIC(PGM_CAP_REQUEST_NOTICES);
	PGM_TEST_NUMERIC(PGM_CAP_STATUS_TELEMETRY);
	PGM_TEST_NUMERIC(PGM_CAP_INSTANCE_EVENTS);
	PGM_TEST_NUMERIC(PGM_CAP_MANAGEMENT_OPERATIONS);
	PGM_TEST_NUMERIC(PGM_CAP_MULTIPLE_INSTANCES);
	PGM_TEST_NUMERIC(PGM_CAP_NATIVE_EXTENSION_LOADING);
	PGM_TEST_NUMERIC(PGM_CAP_LOGICAL_BACKUP);
	PGM_TEST_NUMERIC(PGM_CAP_LOGICAL_RESTORE);
	PGM_TEST_NUMERIC(PGM_CAP_PHYSICAL_BACKUP);
	PGM_TEST_NUMERIC(PGM_CAP_MAINTENANCE);
	PGM_TEST_NUMERIC(PGM_CAP_BUNDLED_EXTENSIONS);
	PGM_TEST_NUMERIC(PGM_EXTENSION_CAP_THREAD_SAFE);
	PGM_TEST_NUMERIC(PGM_EXTENSION_CAP_MULTI_INSTANCE_SAFE);
	PGM_TEST_NUMERIC(PGM_EXTENSION_CAP_SESSION_MOBILITY_SAFE);
	PGM_TEST_NUMERIC(PGM_EXTENSION_CAP_PARALLEL_WORKER_SAFE);
	PGM_TEST_NUMERIC(PGM_EXTENSION_CAP_INSTANCE_SHMEM);
	PGM_TEST_NUMERIC(PGM_EXTENSION_CAP_BACKGROUND_WORKER);
	PGM_TEST_NUMERIC(PGM_EXTENSION_CAP_FILESYSTEM_READ);
	PGM_TEST_NUMERIC(PGM_EXTENSION_CAP_FILESYSTEM_WRITE);
	PGM_TEST_NUMERIC(PGM_EXTENSION_CAP_HOST_LIBRARY_DEPENDENCY);
	PGM_TEST_NUMERIC(PGM_EXTENSION_CAP_PROCESS_GLOBAL_STATE);
}

static void
print_layouts(void)
{
#define PGM_LAYOUT_TYPE(type) PGM_TEST_LAYOUT(type)
#define PGM_LAYOUT_FIELD(type, field) PGM_TEST_FIELD(type, field)
	PGM_LAYOUT_TYPE(pgm_setting);
	PGM_LAYOUT_FIELD(pgm_setting, name);
	PGM_LAYOUT_FIELD(pgm_setting, value);
	PGM_LAYOUT_TYPE(pgm_bundled_extension_info);
	PGM_LAYOUT_FIELD(pgm_bundled_extension_info, struct_size);
	PGM_LAYOUT_FIELD(pgm_bundled_extension_info, postgresql_major);
	PGM_LAYOUT_FIELD(pgm_bundled_extension_info, sdk_abi_version);
	PGM_LAYOUT_FIELD(pgm_bundled_extension_info, reserved);
	PGM_LAYOUT_FIELD(pgm_bundled_extension_info, capabilities);
	PGM_LAYOUT_FIELD(pgm_bundled_extension_info, id);
	PGM_LAYOUT_FIELD(pgm_bundled_extension_info, sql_name);
	PGM_LAYOUT_FIELD(pgm_bundled_extension_info, version);
	PGM_LAYOUT_TYPE(pgm_parameter);
	PGM_LAYOUT_FIELD(pgm_parameter, struct_size);
	PGM_LAYOUT_FIELD(pgm_parameter, type_oid);
	PGM_LAYOUT_FIELD(pgm_parameter, format);
	PGM_LAYOUT_FIELD(pgm_parameter, is_null);
	PGM_LAYOUT_FIELD(pgm_parameter, data);
	PGM_LAYOUT_FIELD(pgm_parameter, size);
	PGM_LAYOUT_TYPE(pgm_log_record);
	PGM_LAYOUT_FIELD(pgm_log_record, struct_size);
	PGM_LAYOUT_FIELD(pgm_log_record, virtual_backend_pid);
	PGM_LAYOUT_FIELD(pgm_log_record, connection_id);
	PGM_LAYOUT_FIELD(pgm_log_record, request_id);
	PGM_LAYOUT_FIELD(pgm_log_record, severity);
	PGM_LAYOUT_FIELD(pgm_log_record, sqlstate);
	PGM_LAYOUT_FIELD(pgm_log_record, message);
	PGM_LAYOUT_FIELD(pgm_log_record, detail);
	PGM_LAYOUT_TYPE(pgm_notice);
	PGM_LAYOUT_FIELD(pgm_notice, struct_size);
	PGM_LAYOUT_FIELD(pgm_notice, sqlstate);
	PGM_LAYOUT_FIELD(pgm_notice, severity);
	PGM_LAYOUT_FIELD(pgm_notice, message);
	PGM_LAYOUT_FIELD(pgm_notice, detail);
	PGM_LAYOUT_FIELD(pgm_notice, hint);
	PGM_LAYOUT_FIELD(pgm_notice, connection_id);
	PGM_LAYOUT_FIELD(pgm_notice, request_id);
	PGM_LAYOUT_TYPE(pgm_notification);
	PGM_LAYOUT_FIELD(pgm_notification, struct_size);
	PGM_LAYOUT_FIELD(pgm_notification, virtual_backend_pid);
	PGM_LAYOUT_FIELD(pgm_notification, channel);
	PGM_LAYOUT_FIELD(pgm_notification, payload);
	PGM_LAYOUT_FIELD(pgm_notification, connection_id);
	PGM_LAYOUT_TYPE(pgm_instance_options);
	PGM_LAYOUT_FIELD(pgm_instance_options, struct_size);
	PGM_LAYOUT_FIELD(pgm_instance_options, create);
	PGM_LAYOUT_FIELD(pgm_instance_options, path);
	PGM_LAYOUT_FIELD(pgm_instance_options, executable_path);
	PGM_LAYOUT_FIELD(pgm_instance_options, resource_root);
	PGM_LAYOUT_FIELD(pgm_instance_options, settings);
	PGM_LAYOUT_FIELD(pgm_instance_options, setting_count);
	PGM_LAYOUT_FIELD(pgm_instance_options, control_queue_capacity);
	PGM_LAYOUT_FIELD(pgm_instance_options, transport_queue_capacity);
	PGM_LAYOUT_FIELD(pgm_instance_options, executor_worker_count);
	PGM_LAYOUT_FIELD(pgm_instance_options, execution_queue_capacity);
	PGM_LAYOUT_FIELD(pgm_instance_options, logical_umask);
	PGM_LAYOUT_FIELD(pgm_instance_options, reserved);
	PGM_LAYOUT_FIELD(pgm_instance_options, user_data);
	PGM_LAYOUT_FIELD(pgm_instance_options, result_buffer_limit);
	PGM_LAYOUT_FIELD(pgm_instance_options, maximum_value_size);
	PGM_LAYOUT_FIELD(pgm_instance_options, event_queue_capacity);
	PGM_LAYOUT_FIELD(pgm_instance_options, log_callback);
	PGM_LAYOUT_FIELD(pgm_instance_options, log_user_data);
	PGM_LAYOUT_TYPE(pgm_instance_telemetry);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, struct_size);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, executor_worker_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, connection_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, active_request_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, running_session_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, pinned_session_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, runnable_session_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, queued_request_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, parallel_tokens_in_use);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, event_queue_depth);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, event_queue_capacity);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, request_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, completed_request_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, canceled_request_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, failed_request_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, execution_token_rejections);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, queue_wait_ns_max);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, dropped_log_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, dropped_notice_count);
	PGM_LAYOUT_FIELD(pgm_instance_telemetry, dropped_notification_count);
	PGM_LAYOUT_TYPE(pgm_event_overflow_record);
	PGM_LAYOUT_FIELD(pgm_event_overflow_record, struct_size);
	PGM_LAYOUT_FIELD(pgm_event_overflow_record, dropped_kind);
	PGM_LAYOUT_FIELD(pgm_event_overflow_record, dropped_count);
	PGM_LAYOUT_TYPE(pgm_connection_options);
	PGM_LAYOUT_FIELD(pgm_connection_options, struct_size);
	PGM_LAYOUT_FIELD(pgm_connection_options, reserved);
	PGM_LAYOUT_FIELD(pgm_connection_options, user);
	PGM_LAYOUT_FIELD(pgm_connection_options, database);
	PGM_LAYOUT_FIELD(pgm_connection_options, application_name);
	PGM_LAYOUT_FIELD(pgm_connection_options, settings);
	PGM_LAYOUT_FIELD(pgm_connection_options, setting_count);
	PGM_LAYOUT_FIELD(pgm_connection_options, notice_callback);
	PGM_LAYOUT_FIELD(pgm_connection_options, notification_callback);
	PGM_LAYOUT_FIELD(pgm_connection_options, user_data);
	PGM_LAYOUT_TYPE(pgm_connection_status_snapshot);
	PGM_LAYOUT_FIELD(pgm_connection_status_snapshot, struct_size);
	PGM_LAYOUT_FIELD(pgm_connection_status_snapshot, transaction_status);
	PGM_LAYOUT_FIELD(pgm_connection_status_snapshot, pin_reasons);
	PGM_LAYOUT_FIELD(pgm_connection_status_snapshot, carrier_retained);
	PGM_LAYOUT_FIELD(pgm_connection_status_snapshot, virtual_backend_pid);
	PGM_LAYOUT_FIELD(pgm_connection_status_snapshot, connection_id);
	PGM_LAYOUT_FIELD(pgm_connection_status_snapshot, active_request_id);
	PGM_LAYOUT_TYPE(pgm_execute_options);
	PGM_LAYOUT_FIELD(pgm_execute_options, struct_size);
	PGM_LAYOUT_FIELD(pgm_execute_options, flags);
	PGM_LAYOUT_FIELD(pgm_execute_options, parameters);
	PGM_LAYOUT_FIELD(pgm_execute_options, parameter_count);
	PGM_LAYOUT_FIELD(pgm_execute_options, result_format);
	PGM_LAYOUT_FIELD(pgm_execute_options, reserved16);
	PGM_LAYOUT_FIELD(pgm_execute_options, delivery_mode);
	PGM_LAYOUT_FIELD(pgm_execute_options, target_chunk_rows);
	PGM_LAYOUT_FIELD(pgm_execute_options, result_buffer_limit);
	PGM_LAYOUT_FIELD(pgm_execute_options, notice_callback);
	PGM_LAYOUT_FIELD(pgm_execute_options, notice_user_data);
	PGM_LAYOUT_TYPE(pgm_script_options);
	PGM_LAYOUT_FIELD(pgm_script_options, struct_size);
	PGM_LAYOUT_FIELD(pgm_script_options, flags);
	PGM_LAYOUT_FIELD(pgm_script_options, delivery_mode);
	PGM_LAYOUT_FIELD(pgm_script_options, target_chunk_rows);
	PGM_LAYOUT_FIELD(pgm_script_options, result_buffer_limit);
	PGM_LAYOUT_FIELD(pgm_script_options, notice_callback);
	PGM_LAYOUT_FIELD(pgm_script_options, notice_user_data);
	PGM_LAYOUT_TYPE(pgm_prepare_options);
	PGM_LAYOUT_FIELD(pgm_prepare_options, struct_size);
	PGM_LAYOUT_FIELD(pgm_prepare_options, flags);
	PGM_LAYOUT_FIELD(pgm_prepare_options, parameter_type_oids);
	PGM_LAYOUT_FIELD(pgm_prepare_options, parameter_count);
	PGM_LAYOUT_TYPE(pgm_statement_description);
	PGM_LAYOUT_FIELD(pgm_statement_description, struct_size);
	PGM_LAYOUT_FIELD(pgm_statement_description, reserved);
	PGM_LAYOUT_FIELD(pgm_statement_description, parameter_count);
	PGM_LAYOUT_FIELD(pgm_statement_description, column_count);
	PGM_LAYOUT_TYPE(pgm_column);
	PGM_LAYOUT_FIELD(pgm_column, struct_size);
	PGM_LAYOUT_FIELD(pgm_column, name);
	PGM_LAYOUT_FIELD(pgm_column, table_oid);
	PGM_LAYOUT_FIELD(pgm_column, table_column);
	PGM_LAYOUT_FIELD(pgm_column, type_oid);
	PGM_LAYOUT_FIELD(pgm_column, type_size);
	PGM_LAYOUT_FIELD(pgm_column, type_modifier);
	PGM_LAYOUT_FIELD(pgm_column, format);
	PGM_LAYOUT_FIELD(pgm_column, reserved);
	PGM_LAYOUT_TYPE(pgm_value_view);
	PGM_LAYOUT_FIELD(pgm_value_view, struct_size);
	PGM_LAYOUT_FIELD(pgm_value_view, type_oid);
	PGM_LAYOUT_FIELD(pgm_value_view, format);
	PGM_LAYOUT_FIELD(pgm_value_view, is_null);
	PGM_LAYOUT_FIELD(pgm_value_view, data);
	PGM_LAYOUT_FIELD(pgm_value_view, size);
	PGM_LAYOUT_TYPE(pgm_checkpoint_options);
	PGM_LAYOUT_FIELD(pgm_checkpoint_options, struct_size);
	PGM_LAYOUT_FIELD(pgm_checkpoint_options, flags);
	PGM_LAYOUT_TYPE(pgm_logical_dump_options);
	PGM_LAYOUT_FIELD(pgm_logical_dump_options, struct_size);
	PGM_LAYOUT_FIELD(pgm_logical_dump_options, flags);
	PGM_LAYOUT_FIELD(pgm_logical_dump_options, database);
	PGM_LAYOUT_FIELD(pgm_logical_dump_options, user);
	PGM_LAYOUT_FIELD(pgm_logical_dump_options, channel_capacity);
	PGM_LAYOUT_FIELD(pgm_logical_dump_options, progress_quantum);
	PGM_LAYOUT_FIELD(pgm_logical_dump_options, write);
	PGM_LAYOUT_FIELD(pgm_logical_dump_options, user_data);
	PGM_LAYOUT_TYPE(pgm_logical_restore_options);
	PGM_LAYOUT_FIELD(pgm_logical_restore_options, struct_size);
	PGM_LAYOUT_FIELD(pgm_logical_restore_options, flags);
	PGM_LAYOUT_FIELD(pgm_logical_restore_options, database);
	PGM_LAYOUT_FIELD(pgm_logical_restore_options, user);
	PGM_LAYOUT_FIELD(pgm_logical_restore_options, channel_capacity);
	PGM_LAYOUT_FIELD(pgm_logical_restore_options, progress_quantum);
	PGM_LAYOUT_FIELD(pgm_logical_restore_options, read);
	PGM_LAYOUT_FIELD(pgm_logical_restore_options, user_data);
	PGM_LAYOUT_TYPE(pgm_maintenance_options);
	PGM_LAYOUT_FIELD(pgm_maintenance_options, struct_size);
	PGM_LAYOUT_FIELD(pgm_maintenance_options, flags);
	PGM_LAYOUT_FIELD(pgm_maintenance_options, kind);
	PGM_LAYOUT_FIELD(pgm_maintenance_options, reserved);
	PGM_LAYOUT_FIELD(pgm_maintenance_options, database);
	PGM_LAYOUT_FIELD(pgm_maintenance_options, user);
	PGM_LAYOUT_TYPE(pgm_operation_progress_snapshot);
	PGM_LAYOUT_FIELD(pgm_operation_progress_snapshot, struct_size);
	PGM_LAYOUT_FIELD(pgm_operation_progress_snapshot, kind);
	PGM_LAYOUT_FIELD(pgm_operation_progress_snapshot, state);
	PGM_LAYOUT_FIELD(pgm_operation_progress_snapshot, phase);
	PGM_LAYOUT_FIELD(pgm_operation_progress_snapshot, bytes_received);
	PGM_LAYOUT_FIELD(pgm_operation_progress_snapshot, bytes_produced);
	PGM_LAYOUT_FIELD(pgm_operation_progress_snapshot, total_bytes);
	PGM_LAYOUT_FIELD(pgm_operation_progress_snapshot, objects_completed);
	PGM_LAYOUT_FIELD(pgm_operation_progress_snapshot, objects_total);
#undef PGM_LAYOUT_FIELD
#undef PGM_LAYOUT_TYPE
}

int
main(void)
{
	if (!check_initializers())
		return 2;
	print_numerics();
	print_layouts();
	return 0;
}
