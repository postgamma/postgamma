#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#define TEST_TIMEOUT_MS INT64_C(30000)
#define TEST_CHANNEL_CAPACITY 4096U
#define TEST_PROGRESS_QUANTUM 1024U
#define TEST_PROGRESS_LIMIT 200000U
#define TEST_CALLBACK_WAIT_LIMIT 30000U
#define TEST_RELEASE_RETRY_LIMIT 30000U


typedef struct MemoryArchive
{
	pgm_instance *instance;
	unsigned char *data;
	size_t		size;
	size_t		capacity;
	size_t		read_offset;
	uint64_t	write_calls;
	uint64_t	read_calls;
	bool		reentry_proved;
	pthread_t	progress_thread;
	bool		progress_thread_set;
	bool		callback_thread_match;
} MemoryArchive;

typedef struct ConcurrentProgressFixture
{
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	pgm_operation *operation;
	pgm_status	progress_status;
	pgm_operation_state progress_state;
	bool		callback_entered;
	bool		release_callback;
	bool		progress_finished;
} ConcurrentProgressFixture;

typedef struct RunningFreeFixture
{
	uint64_t	callback_calls;
} RunningFreeFixture;


static bool execute_command(pgm_connection *connection, const char *sql);
static bool drive_operation(
	pgm_operation *operation, pgm_operation_state expected_terminal,
	pgm_operation_progress_snapshot *snapshot);
static bool drive_operation_expected(
	pgm_operation *operation, pgm_operation_state expected_terminal,
	pgm_status expected_status, const char *expected_message,
	pgm_operation_progress_snapshot *snapshot);
static pgm_io_state archive_write(
	void *argument, const void *data, size_t size, size_t *consumed);
static pgm_io_state archive_read(
	void *argument, void *buffer, size_t capacity, size_t *produced);
static pgm_io_state stalled_write(
	void *argument, const void *data, size_t size, size_t *consumed);
static pgm_io_state invalid_write(
	void *argument, const void *data, size_t size, size_t *consumed);
static pgm_io_state concurrent_write(
	void *argument, const void *data, size_t size, size_t *consumed);
static pgm_io_state running_free_write(
	void *argument, const void *data, size_t size, size_t *consumed);
static void *concurrent_progress_main(void *argument);
static bool prove_concurrent_progress_and_cancel(
	pgm_instance *instance, pgm_logical_dump_options *options);
static bool prove_running_free_cleanup(
	pgm_instance *instance, const pgm_logical_dump_options *options);
static bool value_equals(
	const pgm_result *result, size_t row, size_t column,
	const char *expected);
static int report_error(
	const char *operation, pgm_status status, const pgm_error *error);


int
main(int argument_count, char **arguments)
{
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_logical_dump_options dump_options = PGM_LOGICAL_DUMP_OPTIONS_INIT;
	pgm_logical_restore_options restore_options =
		PGM_LOGICAL_RESTORE_OPTIONS_INIT;
	pgm_maintenance_options maintenance_options = PGM_MAINTENANCE_OPTIONS_INIT;
	pgm_operation_progress_snapshot dump_progress =
		PGM_OPERATION_PROGRESS_SNAPSHOT_INIT;
	pgm_operation_progress_snapshot restore_progress =
		PGM_OPERATION_PROGRESS_SNAPSHOT_INIT;
	MemoryArchive archive = {0};
	MemoryArchive invalid_archive = {0};
	MemoryArchive retry_archive = {0};
	pgm_instance *instance = NULL;
	pgm_connection *connection = NULL;
	pgm_operation *operation = NULL;
	pgm_operation *unexpected = NULL;
	pgm_result *result = NULL;
	pgm_error *error = NULL;
	pgm_status status = PGM_STATUS_INTERNAL_ERROR;
	bool passed = false;

	if (argument_count != 4)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT\n",
			arguments[0]);
		return 2;
	}
	if (pgm_abi_version() != PGM_ABI_VERSION ||
		(pgm_capabilities() &
			(PGM_CAP_LOGICAL_BACKUP | PGM_CAP_LOGICAL_RESTORE |
			 PGM_CAP_MAINTENANCE)) !=
		(PGM_CAP_LOGICAL_BACKUP | PGM_CAP_LOGICAL_RESTORE |
		 PGM_CAP_MAINTENANCE))
	{
		fprintf(stderr, "logical management capabilities are not advertised\n");
		goto fail;
	}
	instance_options.path = arguments[1];
	instance_options.create = UINT32_C(1);
	instance_options.executable_path = arguments[2];
	instance_options.resource_root = arguments[3];
	status = pgm_instance_open(&instance_options, &instance, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	archive.instance = instance;
	invalid_archive.instance = instance;
	retry_archive.instance = instance;
	archive.progress_thread = pthread_self();
	archive.progress_thread_set = true;
	archive.callback_thread_match = true;
	invalid_archive.progress_thread = pthread_self();
	invalid_archive.progress_thread_set = true;
	invalid_archive.callback_thread_match = true;
	retry_archive.progress_thread = pthread_self();
	retry_archive.progress_thread_set = true;
	retry_archive.callback_thread_match = true;
	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-logical-management-stream";
	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (!execute_command(
			connection,
			"CREATE ROLE logical_reader NOLOGIN") ||
		!execute_command(connection, "CREATE SCHEMA logical_fixture") ||
		!execute_command(
			connection,
			"CREATE TABLE logical_fixture.parents("
			"id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, "
			"label text NOT NULL)") ||
		!execute_command(
			connection,
			"INSERT INTO logical_fixture.parents(label) VALUES ('one'), ('two')") ||
		!execute_command(
			connection,
			"CREATE TABLE logical_fixture.logical_stream_items("
			"id int PRIMARY KEY, parent_id bigint NOT NULL REFERENCES "
			"logical_fixture.parents(id), payload text NOT NULL)") ||
		!execute_command(
			connection,
			"COMMENT ON TABLE logical_fixture.logical_stream_items "
			"IS 'logical stream metadata'") ||
		!execute_command(
			connection,
			"INSERT INTO logical_fixture.logical_stream_items "
			"SELECT i, ((i - 1) / 2) + 1, repeat(chr(64 + i), 8192) "
			"FROM generate_series(1, 4) AS i") ||
		!execute_command(
			connection,
			"CREATE VIEW logical_fixture.item_summary AS "
			"SELECT parent_id, count(*) AS item_count "
			"FROM logical_fixture.logical_stream_items GROUP BY parent_id") ||
		!execute_command(
			connection,
			"GRANT USAGE ON SCHEMA logical_fixture TO logical_reader") ||
		!execute_command(
			connection,
			"GRANT SELECT ON logical_fixture.logical_stream_items TO logical_reader") ||
		!execute_command(
			connection,
			"SELECT lo_from_bytea(0, decode(repeat('5a', 32768), 'hex'))"))
		goto fail;
	status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection = NULL;

	dump_options.database = "postgres";
	dump_options.user = "postgamma";
	dump_options.channel_capacity = TEST_CHANNEL_CAPACITY;
	dump_options.progress_quantum = TEST_PROGRESS_QUANTUM;
	dump_options.write = archive_write;
	dump_options.user_data = &archive;
	dump_options.flags = PGM_LOGICAL_CLEAN | PGM_LOGICAL_DATA_ONLY;
	status = pgm_instance_logical_dump_async(
		instance, &dump_options, &operation, &error);
	if (status != PGM_STATUS_INVALID_ARGUMENT || operation != NULL)
	{
		fprintf(stderr, "invalid logical dump flags were accepted\n");
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	dump_options.flags = 0;
	status = pgm_instance_logical_dump_async(
		instance, &dump_options, &operation, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_instance_logical_dump_async(
		instance, &dump_options, &unexpected, &error);
	if (status != PGM_STATUS_BUSY || unexpected != NULL)
	{
		fprintf(stderr, "logical-tool serialization gate did not fail closed\n");
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	if (!drive_operation(operation, PGM_OPERATION_COMPLETED, &dump_progress))
		goto fail;
	pgm_operation_free(operation);
	operation = NULL;
	if (archive.size == 0 || dump_progress.kind != PGM_OPERATION_LOGICAL_DUMP ||
		dump_progress.bytes_produced != archive.size ||
		!archive.reentry_proved)
	{
		fprintf(stderr, "logical dump progress or callback contract is invalid\n");
		goto fail;
	}

	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (!execute_command(
			connection,
			"UPDATE logical_fixture.logical_stream_items SET payload = 'mutated'") ||
		!execute_command(
			connection,
			"COMMENT ON TABLE logical_fixture.logical_stream_items "
			"IS 'metadata changed after dump'") ||
		!execute_command(
			connection,
			"SELECT lo_put(oid, 0, decode(repeat('00', 32768), 'hex')) "
			"FROM pg_largeobject_metadata"))
		goto fail;
	status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection = NULL;

	restore_options.database = "postgres";
	restore_options.user = "postgamma";
	restore_options.flags = PGM_LOGICAL_CLEAN;
	restore_options.channel_capacity = TEST_CHANNEL_CAPACITY;
	restore_options.progress_quantum = TEST_PROGRESS_QUANTUM;
	restore_options.read = archive_read;
	restore_options.user_data = &archive;
	restore_options.flags = PGM_LOGICAL_CLEAN | PGM_LOGICAL_DATA_ONLY;
	status = pgm_instance_logical_restore_async(
		instance, &restore_options, &operation, &error);
	if (status != PGM_STATUS_INVALID_ARGUMENT || operation != NULL)
	{
		fprintf(stderr, "invalid logical restore flags were accepted\n");
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	restore_options.flags = PGM_LOGICAL_CLEAN;
	status = pgm_instance_logical_restore_async(
		instance, &restore_options, &operation, &error);
	if (status != PGM_STATUS_OK ||
		!drive_operation(operation, PGM_OPERATION_COMPLETED, &restore_progress))
		goto fail;
	pgm_operation_free(operation);
	operation = NULL;
	if (restore_progress.kind != PGM_OPERATION_LOGICAL_RESTORE ||
		restore_progress.bytes_received != archive.size)
	{
		fprintf(stderr, "logical restore progress is invalid\n");
		goto fail;
	}

	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_execute(
		connection,
		"SELECT count(*)::text, sum(id)::text, "
		"obj_description('logical_fixture.logical_stream_items'::regclass)::text, "
		"(SELECT count(*)::text FROM pg_largeobject_metadata), "
		"(SELECT encode(lo_get(oid, 0, 1), 'hex') "
		"FROM pg_largeobject_metadata LIMIT 1), "
		"(SELECT sum(item_count)::text FROM logical_fixture.item_summary), "
		"has_table_privilege('logical_reader', "
		"'logical_fixture.logical_stream_items', 'SELECT')::text, "
		"(pg_get_serial_sequence('logical_fixture.parents', 'id') "
		"IS NOT NULL)::text, "
		"(SELECT count(*)::text FROM pg_constraint "
		"WHERE conrelid = 'logical_fixture.logical_stream_items'::regclass "
		"AND contype = 'f') "
		"FROM logical_fixture.logical_stream_items",
		NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS, &result, &error);
	if (status != PGM_STATUS_OK || pgm_result_row_count(result) != 1 ||
		pgm_result_column_count(result) != 9 ||
		!value_equals(result, 0, 0, "4") ||
		!value_equals(result, 0, 1, "10") ||
		!value_equals(result, 0, 2, "logical stream metadata") ||
		!value_equals(result, 0, 3, "1") ||
		!value_equals(result, 0, 4, "5a") ||
		!value_equals(result, 0, 5, "4") ||
		!value_equals(result, 0, 6, "true") ||
		!value_equals(result, 0, 7, "true") ||
		!value_equals(result, 0, 8, "1"))
	{
		fprintf(stderr, "logical stream contents did not round trip\n");
		goto fail;
	}
	pgm_result_free(result);
	result = NULL;
	status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection = NULL;
	if (!prove_concurrent_progress_and_cancel(instance, &dump_options))
		goto fail;
	if (!prove_running_free_cleanup(instance, &dump_options))
		goto fail;

	maintenance_options.database = "postgres";
	maintenance_options.user = "postgamma";
	{
		const pgm_maintenance_kind maintenance_kinds[] =
		{
			PGM_MAINTENANCE_VACUUM,
			PGM_MAINTENANCE_ANALYZE,
			PGM_MAINTENANCE_VACUUM_ANALYZE,
			PGM_MAINTENANCE_REINDEX_DATABASE,
		};

		for (size_t index = 0;
			 index < sizeof(maintenance_kinds) / sizeof(maintenance_kinds[0]);
			 index++)
		{
			maintenance_options.kind = maintenance_kinds[index];
			status = pgm_instance_maintenance_async(
				instance, &maintenance_options, &operation, &error);
			if (status != PGM_STATUS_OK ||
				!drive_operation(operation, PGM_OPERATION_COMPLETED, NULL))
				goto fail;
			pgm_operation_free(operation);
			operation = NULL;
		}
	}

	invalid_archive.data = malloc(32);
	if (invalid_archive.data == NULL)
		goto fail;
	memcpy(invalid_archive.data, "not-a-postgresql-archive", 24);
	invalid_archive.size = 24;
	invalid_archive.capacity = 32;
	restore_options.user_data = &invalid_archive;
	status = pgm_instance_logical_restore_async(
		instance, &restore_options, &operation, &error);
	if (status != PGM_STATUS_OK ||
		!drive_operation(operation, PGM_OPERATION_FAILED, NULL))
		goto fail;
	pgm_error_free(error);
	error = NULL;
	pgm_operation_free(operation);
	operation = NULL;

	dump_options.write = invalid_write;
	dump_options.user_data = &archive;
	status = pgm_instance_logical_dump_async(
		instance, &dump_options, &operation, &error);
	if (status != PGM_STATUS_OK ||
		!drive_operation_expected(
			operation, PGM_OPERATION_FAILED, PGM_STATUS_INVALID_ARGUMENT,
			"logical dump write callback violated its contract", NULL))
		goto fail;
	pgm_operation_free(operation);
	operation = NULL;

	dump_options.write = stalled_write;
	dump_options.user_data = &archive;
	for (unsigned int retry = 0; retry < TEST_RELEASE_RETRY_LIMIT; retry++)
	{
		status = pgm_instance_logical_dump_async(
			instance, &dump_options, &operation, &error);
		if (status == PGM_STATUS_OK)
			break;
		if (status != PGM_STATUS_BUSY)
			goto fail;
		pgm_error_free(error);
		error = NULL;
		{
			const struct timespec retry_pause = {0, 1000000L};

			(void) nanosleep(&retry_pause, NULL);
		}
	}
	if (status != PGM_STATUS_OK)
		goto fail;
	{
		pgm_operation_state state = PGM_OPERATION_PENDING;

		status = pgm_operation_progress(operation, &state, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
	}
	status = pgm_operation_cancel(operation, &error);
	if (status != PGM_STATUS_OK ||
		!drive_operation(operation, PGM_OPERATION_CANCELED, NULL))
		goto fail;
	pgm_operation_free(operation);
	operation = NULL;

	dump_options.write = archive_write;
	dump_options.user_data = &retry_archive;
	status = pgm_instance_logical_dump_async(
		instance, &dump_options, &operation, &error);
	if (status != PGM_STATUS_OK ||
		!drive_operation(operation, PGM_OPERATION_COMPLETED, NULL) ||
		retry_archive.size == 0)
		goto fail;
	pgm_operation_free(operation);
	operation = NULL;
	if (!archive.callback_thread_match ||
		!invalid_archive.callback_thread_match ||
		!retry_archive.callback_thread_match)
	{
		fprintf(stderr, "logical callback ran outside its progress thread\n");
		goto fail;
	}

	status = pgm_instance_close(
		instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	instance = NULL;
	passed = true;
	printf(
		"POSTGAMMA_LOGICAL_STREAM pid=%ld archive_bytes=%zu "
		"dump_calls=%llu restore_calls=%llu channel_capacity=%u "
		"progress_quantum=%u large_objects=1 metadata=true maintenance=true "
		"maintenance_operations=4 "
		"dependencies=true acl=true identity=true "
		"clean_restore=true invalid_flags=true callback_failure=true "
		"failure_retry=true cancel_retry=true "
		"cancel=true reentrant=true "
		"stable_waitable=true callbacks_on_progress_thread=true "
		"concurrent_progress_busy=true concurrent_cancel=true "
		"running_free_cleanup=true "
		"subprocesses=0 staging_files=0 phase=closed\n",
		(long) getpid(), archive.size,
		(unsigned long long) archive.write_calls,
		(unsigned long long) archive.read_calls,
		TEST_CHANNEL_CAPACITY, TEST_PROGRESS_QUANTUM);

fail:
	if (!passed)
		(void) report_error("logical stream driver", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	pgm_operation_free(unexpected);
	pgm_operation_free(operation);
	if (connection != NULL)
		(void) pgm_connection_close(connection, TEST_TIMEOUT_MS, NULL);
	if (instance != NULL)
		(void) pgm_instance_close(
			instance, PGM_SHUTDOWN_IMMEDIATE, TEST_TIMEOUT_MS, NULL);
	free(archive.data);
	free(invalid_archive.data);
	free(retry_archive.data);
	return passed ? 0 : 1;
}


static bool
drive_operation(
	pgm_operation *operation, pgm_operation_state expected_terminal,
	pgm_operation_progress_snapshot *snapshot)
{
	pgm_status expected_status = PGM_STATUS_OK;
	const char *expected_message = NULL;

	if (expected_terminal == PGM_OPERATION_CANCELED)
		expected_status = PGM_STATUS_CANCELED;
	else if (expected_terminal == PGM_OPERATION_FAILED)
	{
		expected_status = PGM_STATUS_POSTGRES_ERROR;
		expected_message = "magic string";
	}
	return drive_operation_expected(
		operation, expected_terminal, expected_status, expected_message,
		snapshot);
}


static bool
drive_operation_expected(
	pgm_operation *operation, pgm_operation_state expected_terminal,
	pgm_status expected_status, const char *expected_message,
	pgm_operation_progress_snapshot *snapshot)
{
	pgm_operation_state state = PGM_OPERATION_PENDING;
	pgm_error *error = NULL;
	pgm_status status = PGM_STATUS_OK;
	int stable_descriptor = -1;
	unsigned int iteration;

	status = pgm_operation_waitable(operation, &stable_descriptor, &error);
	if (status != PGM_STATUS_OK || stable_descriptor < 0)
	{
		(void) report_error("get initial management waitable", status, error);
		pgm_error_free(error);
		return false;
	}

	for (iteration = 0; iteration < TEST_PROGRESS_LIMIT; iteration++)
	{
		int descriptor = -1;
		struct pollfd poll_descriptor;

		status = pgm_operation_progress(operation, &state, &error);
		if (status != PGM_STATUS_OK)
		{
			if (status == expected_status && state == expected_terminal &&
				(expected_message == NULL ||
				 (error != NULL && pgm_error_message(error) != NULL &&
				  strstr(pgm_error_message(error), expected_message) != NULL)))
			{
				pgm_error_free(error);
				error = NULL;
				break;
			}
			(void) report_error("progress management operation", status, error);
			pgm_error_free(error);
			return false;
		}
		pgm_error_free(error);
		error = NULL;
		if (state == PGM_OPERATION_COMPLETED ||
			state == PGM_OPERATION_CANCELED ||
			state == PGM_OPERATION_FAILED)
			break;
		status = pgm_operation_waitable(operation, &descriptor, &error);
		if (status != PGM_STATUS_OK || descriptor != stable_descriptor)
		{
			if (status == PGM_STATUS_OK)
				fprintf(stderr, "management waitable descriptor changed\n");
			else
				(void) report_error("get management waitable", status, error);
			pgm_error_free(error);
			return false;
		}
		poll_descriptor.fd = descriptor;
		poll_descriptor.events = POLLIN;
		poll_descriptor.revents = 0;
		(void) poll(&poll_descriptor, 1, 1);
		{
			const struct timespec scheduler_quantum = {0, 100000};

			(void) nanosleep(&scheduler_quantum, NULL);
		}
	}
	if (iteration == TEST_PROGRESS_LIMIT || state != expected_terminal)
	{
		fprintf(stderr,
			"management operation terminal state is invalid: state=%d\n",
			state);
		return false;
	}
	if (snapshot != NULL)
	{
		*snapshot = (pgm_operation_progress_snapshot)
			PGM_OPERATION_PROGRESS_SNAPSHOT_INIT;
		status = pgm_operation_get_progress(operation, snapshot, &error);
		if (status != PGM_STATUS_OK)
		{
			(void) report_error("get management progress", status, error);
			pgm_error_free(error);
			return false;
		}
	}
	return true;
}


static pgm_io_state
archive_write(
	void *argument, const void *data, size_t size, size_t *consumed)
{
	MemoryArchive *archive = argument;
	size_t amount = size < 257 ? size : 257;
	pgm_instance_telemetry telemetry = PGM_INSTANCE_TELEMETRY_INIT;
	pgm_error *error = NULL;
	pgm_status status;

	archive->write_calls++;
	*consumed = 0;
	if (!archive->progress_thread_set ||
		!pthread_equal(archive->progress_thread, pthread_self()))
	{
		archive->callback_thread_match = false;
		return PGM_IO_END;
	}
	if (!archive->reentry_proved)
	{
		status = pgm_instance_get_telemetry(
			archive->instance, &telemetry, &error);
		archive->reentry_proved = status == PGM_STATUS_REENTRANT_CALL;
		pgm_error_free(error);
		if (!archive->reentry_proved)
			return PGM_IO_END;
	}
	if (archive->write_calls % 5 == 0)
		return PGM_IO_AGAIN;
	if (amount > SIZE_MAX - archive->size)
		return PGM_IO_END;
	if (archive->size + amount > archive->capacity)
	{
		size_t capacity = archive->capacity != 0 ? archive->capacity : 4096;
		unsigned char *resized;

		while (capacity < archive->size + amount)
		{
			if (capacity > SIZE_MAX / 2)
				return PGM_IO_END;
			capacity *= 2;
		}
		resized = realloc(archive->data, capacity);
		if (resized == NULL)
			return PGM_IO_END;
		archive->data = resized;
		archive->capacity = capacity;
	}
	memcpy(archive->data + archive->size, data, amount);
	archive->size += amount;
	*consumed = amount;
	return PGM_IO_PROGRESS;
}


static pgm_io_state
archive_read(
	void *argument, void *buffer, size_t capacity, size_t *produced)
{
	MemoryArchive *archive = argument;
	size_t available;
	size_t amount;

	archive->read_calls++;
	*produced = 0;
	if (!archive->progress_thread_set ||
		!pthread_equal(archive->progress_thread, pthread_self()))
	{
		archive->callback_thread_match = false;
		return PGM_IO_END;
	}
	if (archive->read_calls % 4 == 0)
		return PGM_IO_AGAIN;
	if (archive->read_offset == archive->size)
		return PGM_IO_END;
	available = archive->size - archive->read_offset;
	amount = available < capacity ? available : capacity;
	if (amount > 193)
		amount = 193;
	memcpy(buffer, archive->data + archive->read_offset, amount);
	archive->read_offset += amount;
	*produced = amount;
	return PGM_IO_PROGRESS;
}


static pgm_io_state
stalled_write(
	void *argument, const void *data, size_t size, size_t *consumed)
{
	(void) argument;
	(void) data;
	(void) size;
	*consumed = 0;
	return PGM_IO_AGAIN;
}


static pgm_io_state
invalid_write(
	void *argument, const void *data, size_t size, size_t *consumed)
{
	(void) argument;
	(void) data;
	(void) size;
	*consumed = 0;
	return PGM_IO_END;
}


static pgm_io_state
concurrent_write(
	void *argument, const void *data, size_t size, size_t *consumed)
{
	ConcurrentProgressFixture *fixture = argument;

	(void) data;
	(void) size;
	*consumed = 0;
	if (pthread_mutex_lock(&fixture->mutex) != 0)
		return PGM_IO_END;
	fixture->callback_entered = true;
	(void) pthread_cond_broadcast(&fixture->condition);
	while (!fixture->release_callback)
	{
		if (pthread_cond_wait(&fixture->condition, &fixture->mutex) != 0)
		{
			(void) pthread_mutex_unlock(&fixture->mutex);
			return PGM_IO_END;
		}
	}
	(void) pthread_mutex_unlock(&fixture->mutex);
	return PGM_IO_AGAIN;
}


static pgm_io_state
running_free_write(
	void *argument, const void *data, size_t size, size_t *consumed)
{
	RunningFreeFixture *fixture = argument;

	(void) data;
	(void) size;
	fixture->callback_calls++;
	*consumed = 0;
	return PGM_IO_AGAIN;
}


static void *
concurrent_progress_main(void *argument)
{
	ConcurrentProgressFixture *fixture = argument;
	pgm_operation_state state = PGM_OPERATION_PENDING;
	pgm_error *error = NULL;
	pgm_status status = PGM_STATUS_OK;

	for (unsigned int iteration = 0;
		 iteration < TEST_PROGRESS_LIMIT;
		 iteration++)
	{
		const struct timespec scheduler_quantum = {0, 100000};

		status = pgm_operation_progress(fixture->operation, &state, &error);
		pgm_error_free(error);
		error = NULL;
		if (status != PGM_STATUS_OK || state == PGM_OPERATION_COMPLETED ||
			state == PGM_OPERATION_CANCELED || state == PGM_OPERATION_FAILED)
			break;
		(void) nanosleep(&scheduler_quantum, NULL);
	}
	if (pthread_mutex_lock(&fixture->mutex) == 0)
	{
		fixture->progress_status = status;
		fixture->progress_state = state;
		fixture->progress_finished = true;
		(void) pthread_cond_broadcast(&fixture->condition);
		(void) pthread_mutex_unlock(&fixture->mutex);
	}
	return NULL;
}


static bool
prove_concurrent_progress_and_cancel(
	pgm_instance *instance, pgm_logical_dump_options *options)
{
	ConcurrentProgressFixture fixture = {0};
	pgm_operation *operation = NULL;
	pgm_operation_state busy_state = (pgm_operation_state) -1;
	pgm_error *error = NULL;
	pgm_status status = PGM_STATUS_INTERNAL_ERROR;
	pthread_t progress_thread;
	struct timespec deadline;
	bool mutex_initialized = false;
	bool condition_initialized = false;
	bool thread_started = false;
	bool cancellation_requested = false;
	bool passed = false;
	const char *failure = "initialize concurrent progress fixture";

	if (pthread_mutex_init(&fixture.mutex, NULL) != 0)
		goto done;
	mutex_initialized = true;
	failure = "initialize concurrent progress condition";
	if (pthread_cond_init(&fixture.condition, NULL) != 0)
		goto done;
	condition_initialized = true;
	options->write = concurrent_write;
	options->user_data = &fixture;
	failure = "start concurrent logical dump";
	status = pgm_instance_logical_dump_async(
		instance, options, &operation, &error);
	if (status != PGM_STATUS_OK)
		goto done;
	fixture.operation = operation;
	failure = "start concurrent progress thread";
	if (pthread_create(
			&progress_thread, NULL, concurrent_progress_main, &fixture) != 0)
		goto done;
	thread_started = true;
	failure = "read concurrent progress deadline";
	if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
		goto release;
	deadline.tv_sec += 30;
	failure = "lock concurrent progress fixture";
	if (pthread_mutex_lock(&fixture.mutex) != 0)
		goto release;
	while (!fixture.callback_entered && !fixture.progress_finished)
	{
		int wait_status = pthread_cond_timedwait(
			&fixture.condition, &fixture.mutex, &deadline);

		if (wait_status != 0)
			break;
	}
	if (!fixture.callback_entered)
	{
		(void) pthread_mutex_unlock(&fixture.mutex);
		failure = "observe concurrent callback entry";
		goto release;
	}
	(void) pthread_mutex_unlock(&fixture.mutex);
	failure = "reject concurrent operation progress";
	status = pgm_operation_progress(operation, &busy_state, &error);
	if (status != PGM_STATUS_BUSY || busy_state != PGM_OPERATION_RUNNING)
		goto release;
	pgm_error_free(error);
	error = NULL;
	failure = "cancel concurrently progressed operation";
	status = pgm_operation_cancel(operation, &error);
	if (status != PGM_STATUS_OK)
		goto release;
	cancellation_requested = true;

release:
	if (mutex_initialized && pthread_mutex_lock(&fixture.mutex) == 0)
	{
		fixture.release_callback = true;
		(void) pthread_cond_broadcast(&fixture.condition);
		(void) pthread_mutex_unlock(&fixture.mutex);
	}
	if (thread_started)
		(void) pthread_join(progress_thread, NULL);
	if (cancellation_requested)
		failure = "drive concurrently canceled operation to completion";
	if (cancellation_requested && status == PGM_STATUS_OK &&
		(fixture.progress_status == PGM_STATUS_OK ||
		 fixture.progress_status == PGM_STATUS_CANCELED) &&
		drive_operation(operation, PGM_OPERATION_CANCELED, NULL))
		passed = true;

done:
	if (!passed)
	{
		fprintf(stderr,
			"concurrent progress proof failed: stage=%s status=%d "
			"progress_status=%d progress_state=%d busy_state=%d "
			"callback_entered=%d progress_finished=%d\n",
			failure, status, fixture.progress_status,
			fixture.progress_state, busy_state,
			fixture.callback_entered, fixture.progress_finished);
		if (error != NULL)
			(void) report_error(failure, status, error);
	}
	pgm_error_free(error);
	pgm_operation_free(operation);
	if (condition_initialized)
		(void) pthread_cond_destroy(&fixture.condition);
	if (mutex_initialized)
		(void) pthread_mutex_destroy(&fixture.mutex);
	return passed;
}


static bool
prove_running_free_cleanup(
	pgm_instance *instance, const pgm_logical_dump_options *options)
{
	pgm_logical_dump_options active_options = *options;
	pgm_logical_dump_options admission_options = *options;
	RunningFreeFixture fixture = {0};
	pgm_operation *operation = NULL;
	pgm_error *error = NULL;
	pgm_status status = PGM_STATUS_OK;
	int stable_descriptor = -1;
	bool passed = false;
	const char *failure = "start running logical dump";

	active_options.write = running_free_write;
	active_options.user_data = &fixture;
	status = pgm_instance_logical_dump_async(
		instance, &active_options, &operation, &error);
	if (status != PGM_STATUS_OK)
		goto done;
	failure = "get running logical dump waitable";
	status = pgm_operation_waitable(operation, &stable_descriptor, &error);
	if (status != PGM_STATUS_OK || stable_descriptor < 0)
		goto done;
	failure = "progress running logical dump to its callback";
	for (unsigned int iteration = 0;
		 iteration < TEST_CALLBACK_WAIT_LIMIT && fixture.callback_calls == 0;
		 iteration++)
	{
		pgm_operation_state state = PGM_OPERATION_PENDING;
		struct pollfd poll_descriptor;
		int descriptor = -1;

		status = pgm_operation_progress(operation, &state, &error);
		if (status != PGM_STATUS_OK || state != PGM_OPERATION_RUNNING)
			goto done;
		if (fixture.callback_calls != 0)
			break;
		status = pgm_operation_waitable(operation, &descriptor, &error);
		if (status != PGM_STATUS_OK || descriptor != stable_descriptor)
			goto done;
		poll_descriptor.fd = descriptor;
		poll_descriptor.events = POLLIN;
		poll_descriptor.revents = 0;
		(void) poll(&poll_descriptor, 1, 1);
		{
			const struct timespec scheduler_quantum = {0, 100000};

			(void) nanosleep(&scheduler_quantum, NULL);
		}
	}
	if (fixture.callback_calls == 0)
		goto done;
	failure = "free running logical dump";
	pgm_operation_free(operation);
	operation = NULL;

	/*
	 * Admission is released only by logical_worker_completed().  Reacquiring
	 * it proves that freeing a running operation canceled and joined its
	 * frontend worker without leaving the process-wide reservation stuck.
	 */
	admission_options.write = stalled_write;
	admission_options.user_data = NULL;
	failure = "reacquire logical-tool admission after running free";
	for (unsigned int retry = 0; retry < TEST_RELEASE_RETRY_LIMIT; retry++)
	{
		status = pgm_instance_logical_dump_async(
			instance, &admission_options, &operation, &error);
		if (status == PGM_STATUS_OK)
			break;
		if (status != PGM_STATUS_BUSY)
			goto done;
		pgm_error_free(error);
		error = NULL;
		{
			const struct timespec retry_pause = {0, 1000000L};

			(void) nanosleep(&retry_pause, NULL);
		}
	}
	if (status != PGM_STATUS_OK)
		goto done;
	failure = "cancel reacquired logical dump";
	status = pgm_operation_cancel(operation, &error);
	if (status != PGM_STATUS_OK)
		goto done;
	passed = true;

done:
	if (!passed)
	{
		fprintf(stderr,
			"running free proof failed: stage=%s status=%d "
			"callback_calls=%llu\n",
			failure, status, (unsigned long long) fixture.callback_calls);
		if (error != NULL)
			(void) report_error(failure, status, error);
	}
	pgm_error_free(error);
	pgm_operation_free(operation);
	return passed;
}


static bool
execute_command(pgm_connection *connection, const char *sql)
{
	pgm_result *result = NULL;
	pgm_error *error = NULL;
	pgm_status status;
	bool ok;

	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT,
		TEST_TIMEOUT_MS, &result, &error);
	ok = status == PGM_STATUS_OK && result != NULL &&
		(pgm_result_kind(result) == PGM_RESULT_COMMAND_OK ||
		 pgm_result_kind(result) == PGM_RESULT_TUPLES_OK);
	if (!ok)
		(void) report_error(sql, status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}


static bool
value_equals(
	const pgm_result *result, size_t row, size_t column,
	const char *expected)
{
	pgm_value_view value = PGM_VALUE_VIEW_INIT;

	return pgm_result_value(result, row, column, &value, NULL) ==
		PGM_STATUS_OK && value.is_null == 0 &&
		value.size == strlen(expected) &&
		memcmp(value.data, expected, value.size) == 0;
}


static int
report_error(
	const char *operation, pgm_status status, const pgm_error *error)
{
	fprintf(stderr, "%s failed: status=%s message=%s\n",
		operation != NULL ? operation : "operation",
		pgm_status_name(status),
		error != NULL && pgm_error_message(error) != NULL ?
		pgm_error_message(error) : "none");
	return 1;
}
