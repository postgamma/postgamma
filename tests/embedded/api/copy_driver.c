#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#define TEST_TIMEOUT_MS INT64_C(30000)
#define COPY_QUEUE_CAPACITY 64U
#define LARGE_PAYLOAD_SIZE (256U * 1024U)
#define REJECTED_PAYLOAD_SIZE (1024U * 1024U)
#define RSS_ALLOWANCE_BYTES UINT64_C(67108864)


typedef struct CopyWriter
{
	pgm_copy   *copy;
	const unsigned char *data;
	size_t		size;
	size_t		consumed;
	pgm_status	status;
	pgm_error  *error;
} CopyWriter;


static int report_failure(
	const char *operation, pgm_status status, const pgm_error *error);
static bool execute_command(pgm_connection *connection, const char *sql);
static bool execute_reuse_probe(
	pgm_connection *connection, const char *expected);
static bool begin_copy(
	pgm_connection *connection, const char *sql,
	pgm_result_status expected_kind, pgm_request **request,
	pgm_copy **copy, pgm_error **error);
static bool next_result(
	pgm_request *request, pgm_result **result, pgm_error **error);
static bool finish_command_request(
	pgm_request *request, const char *expected_command, pgm_error **error);
static bool finish_request(
	pgm_request *request, const char *name, pgm_error **error);
static bool finish_error_request(
	pgm_request *request, const char *expected_sqlstate,
	pgm_status expected_status, pgm_error **error);
static bool make_copy_data(
	unsigned char **data, size_t *size, size_t large_payload_size);
static bool make_rejected_copy_data(
	unsigned char **data, size_t *size, size_t target_size);
static bool progress_briefly(pgm_request *request, pgm_error **error);
static bool pause_milliseconds(long milliseconds);
static uint64_t resident_bytes(void);
static size_t thread_count(void);
static uint64_t fnv1a64(const void *data, size_t size);
static void receive_notice(void *argument, const pgm_notice *notice);
static void *copy_writer_main(void *argument);
static bool test_invalid_take(pgm_connection *connection);
static bool test_unclaimed_copy_result(pgm_connection *connection);
static bool test_early_copy_error(
	pgm_connection *connection, size_t rejected_payload_size,
	size_t *accepted, size_t *offered);
static bool test_abort_before_first_byte(pgm_connection *connection);
static bool test_cancel_before_first_byte(pgm_connection *connection);
static bool test_stalled_cancel_and_timeout(
	pgm_connection *connection, uint64_t generated_rows,
	uint64_t *rss_delta, size_t *thread_growth);
static bool test_deferred_request_free(pgm_connection *connection);
static bool test_concurrent_copy_owner(pgm_connection *connection);


static _Atomic unsigned int NoticeCount;
static _Atomic bool BlockNotice;
static _Atomic bool NoticeBlocked;
static _Atomic bool ReleaseNotice;


int
main(int argument_count, char **arguments)
{
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_instance *instance = NULL;
	pgm_connection *connection = NULL;
	pgm_request *request = NULL;
	pgm_result *result = NULL;
	pgm_copy   *copy = NULL;
	pgm_error  *error = NULL;
	unsigned char *input = NULL;
	unsigned char *output = NULL;
	size_t		input_size = 0;
	size_t		output_size = 0;
	size_t		output_capacity = 0;
	size_t		partial_writes = 0;
	size_t		write_again = 0;
	size_t		read_calls = 0;
	size_t		early_accepted = 0;
	size_t		early_offered = 0;
	size_t		thread_growth = 0;
	uint64_t	rss_delta = 0;
	uint64_t	input_hash;
	uint64_t	generated_rows = UINT64_C(100000);
	size_t		large_payload_size = LARGE_PAYLOAD_SIZE;
	size_t		rejected_payload_size = REJECTED_PAYLOAD_SIZE;
	unsigned int reference_notice_count;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	bool		passed = false;

	if ((argument_count != 5 && argument_count != 6) ||
		strcmp(arguments[4], "create") != 0 ||
		(argument_count == 6 && strcmp(arguments[5], "trace") != 0))
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT "
			"create [trace]\n",
			arguments[0]);
		return 2;
	}
	if (argument_count == 6)
	{
		large_payload_size = 4096U;
		rejected_payload_size = 32768U;
		generated_rows = UINT64_C(1000);
	}
	if ((pgm_capabilities() & (PGM_CAP_COPY_IN | PGM_CAP_COPY_OUT)) !=
		(PGM_CAP_COPY_IN | PGM_CAP_COPY_OUT))
	{
		fprintf(stderr, "COPY capabilities are not advertised\n");
		goto fail;
	}
	if (!make_copy_data(&input, &input_size, large_payload_size))
		goto fail;
	input_hash = fnv1a64(input, input_size);
	instance_options.path = arguments[1];
	instance_options.create = UINT32_C(1);
	instance_options.executable_path = arguments[2];
	instance_options.resource_root = arguments[3];
	instance_options.transport_queue_capacity = COPY_QUEUE_CAPACITY;
	instance_options.result_buffer_limit = 1024U * 1024U;
	instance_options.maximum_value_size = 512U * 1024U;
	status = pgm_instance_open(&instance_options, &instance, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-c-api-copy";
	connection_options.notice_callback = receive_notice;
	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	if (!execute_command(
			connection,
			"CREATE TABLE copy_fixture (id integer PRIMARY KEY, payload text)") ||
		!execute_command(
			connection,
			"CREATE FUNCTION copy_notice() RETURNS trigger LANGUAGE plpgsql "
			"AS $$ BEGIN RAISE NOTICE 'copied %', NEW.id; RETURN NEW; END $$") ||
		!execute_command(
			connection,
			"CREATE TRIGGER copy_notice_trigger BEFORE INSERT ON copy_fixture "
			"FOR EACH ROW EXECUTE FUNCTION copy_notice()") ||
		!execute_command(
			connection,
			"CREATE TABLE copy_reject "
			"(id integer CONSTRAINT copy_reject_check CHECK (id < 2))") ||
		!execute_command(connection, "CREATE TABLE copy_abort (id integer)") ||
		!execute_command(connection, "CREATE TABLE copy_orphan (id integer)") ||
		!execute_command(
			connection,
			"CREATE TABLE copy_busy_fixture (id integer, payload text)") ||
		!execute_command(
			connection,
			"CREATE TRIGGER copy_busy_trigger BEFORE INSERT ON copy_busy_fixture "
			"FOR EACH ROW EXECUTE FUNCTION copy_notice()"))
		goto fail;
	if (!test_invalid_take(connection))
		goto fail;

	{
		static const char sql[] =
			"COPY copy_fixture FROM STDIN WITH (FORMAT csv)";
		size_t offset = 0;
		pgm_io_state io_state = PGM_IO_AGAIN;
		pgm_result *blocked_result = NULL;
		pgm_availability availability = PGM_AVAILABILITY_AGAIN;

		if (!begin_copy(
				connection, sql, PGM_RESULT_COPY_IN, &request, &copy, &error))
			goto fail;
		status = pgm_request_next_result(
			request, 0, &blocked_result, &availability, &error);
		if (status != PGM_STATUS_BUSY || blocked_result != NULL || error == NULL ||
			pgm_error_status(error) != PGM_STATUS_BUSY)
		{
			fprintf(stderr, "COPY ownership did not block the next result\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
		status = PGM_STATUS_OK;
		printf(
			"POSTGAMMA_COPY_CASE name=copy_in sequence=0 kind=copy_in "
			"bytes=%zu hash=%016" PRIx64 "\n",
			input_size, input_hash);
		while (offset < input_size)
		{
			size_t consumed = 0;

			status = pgm_copy_write(
				copy, input + offset, input_size - offset, &consumed,
				&io_state, &error);
			if (status != PGM_STATUS_OK || consumed > input_size - offset)
				goto fail;
			if (consumed < input_size - offset)
				partial_writes++;
			offset += consumed;
			if (io_state == PGM_IO_AGAIN)
			{
				write_again++;
				if (!progress_briefly(request, &error))
					goto fail;
			}
		}
		do
		{
			status = pgm_copy_finish(copy, NULL, 0, &io_state, &error);
			if (status != PGM_STATUS_OK)
				goto fail;
			if (io_state == PGM_IO_AGAIN &&
				!progress_briefly(request, &error))
				goto fail;
		} while (io_state == PGM_IO_AGAIN);
		if (io_state != PGM_IO_END)
			goto fail;
		status = pgm_copy_close(copy, TEST_TIMEOUT_MS, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		copy = NULL;
		if (!finish_request(request, "copy_in", &error))
			goto fail;
		pgm_request_free(request);
		request = NULL;
	}

	{
		static const char sql[] =
			"COPY (SELECT id, payload FROM copy_fixture ORDER BY id) "
			"TO STDOUT WITH (FORMAT csv)";
		pgm_io_state io_state = PGM_IO_AGAIN;

		if (!begin_copy(
				connection, sql, PGM_RESULT_COPY_OUT, &request, &copy, &error))
			goto fail;
		for (;;)
		{
			unsigned char fragment[17];
			size_t produced = 0;

			status = pgm_copy_read(
				copy, fragment, sizeof(fragment), &produced, &io_state, &error);
			if (status != PGM_STATUS_OK)
				goto fail;
			read_calls++;
			if (produced != 0)
			{
				if (output_size > SIZE_MAX - produced)
					goto fail;
				if (output_size + produced > output_capacity)
				{
					size_t new_capacity = output_capacity == 0 ? 1024 :
						output_capacity * 2;
					unsigned char *resized;

					while (new_capacity < output_size + produced)
						new_capacity *= 2;
					resized = realloc(output, new_capacity);
					if (resized == NULL)
						goto fail;
					output = resized;
					output_capacity = new_capacity;
				}
				memcpy(output + output_size, fragment, produced);
				output_size += produced;
			}
			if (io_state == PGM_IO_END)
				break;
			if (io_state == PGM_IO_AGAIN &&
				!progress_briefly(request, &error))
				goto fail;
		}
		if (output_size != input_size ||
			memcmp(output, input, input_size) != 0 ||
			fnv1a64(output, output_size) != input_hash)
		{
			fprintf(stderr,
				"COPY OUT mismatch: expected=%zu observed=%zu\n",
				input_size, output_size);
			goto fail;
		}
		printf(
			"POSTGAMMA_COPY_CASE name=copy_out sequence=0 kind=copy_out "
			"bytes=%zu hash=%016" PRIx64 "\n",
			output_size, fnv1a64(output, output_size));
		status = pgm_copy_close(copy, TEST_TIMEOUT_MS, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		copy = NULL;
		if (!finish_request(request, "copy_out", &error))
			goto fail;
		pgm_request_free(request);
		request = NULL;
	}

	reference_notice_count =
		atomic_load_explicit(&NoticeCount, memory_order_relaxed);
	if (reference_notice_count != 3 ||
		!test_unclaimed_copy_result(connection) ||
		!test_early_copy_error(
			connection, rejected_payload_size, &early_accepted, &early_offered) ||
		!test_abort_before_first_byte(connection) ||
		!test_cancel_before_first_byte(connection) ||
		!test_stalled_cancel_and_timeout(
			connection, generated_rows, &rss_delta, &thread_growth) ||
		!test_deferred_request_free(connection) ||
		!test_concurrent_copy_owner(connection) ||
		!execute_reuse_probe(connection, "42"))
		goto fail;

	status = pgm_connection_close(connection, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection = NULL;
	status = pgm_instance_close(
		instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	instance = NULL;
	passed = true;

fail:
	if (!passed)
		(void) report_failure("COPY streaming gate", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	if (copy != NULL)
		(void) pgm_copy_close(copy, TEST_TIMEOUT_MS, NULL);
	if (request != NULL)
		pgm_request_free(request);
	if (connection != NULL)
		(void) pgm_connection_close(connection, TEST_TIMEOUT_MS, NULL);
	if (instance != NULL)
		(void) pgm_instance_close(
			instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, NULL);
	free(output);
	free(input);
	if (!passed)
		return 1;
	printf(
		"POSTGAMMA_COPY_COPY postgres=19 copy_in=true copy_out=true "
		"queue_capacity=%u payload_bytes=%zu payload_hash=%016" PRIx64 " "
		"read_buffer=17 partial_writes=%zu write_again=%zu read_calls=%zu "
		"reference_notices=%u partial_io=true byte_exact=true "
		"take_failure_ownership=true outstanding_result_busy=true "
		"unclaimed_result_retired=true early_error=true early_accepted=%zu "
		"early_offered=%zu abort_before_byte=true cancel_before_byte=true "
		"cancel_mid_copy_out=true close_timeout_retry=true "
		"deferred_request_free=true concurrent_owner_busy=true "
		"stalled_consumer=true thread_growth=%zu rss_delta_bytes=%" PRIu64 " "
		"rss_allowance_bytes=%" PRIu64 " connection_reuse=true "
		"copy_capabilities=true phase=closed\n",
		COPY_QUEUE_CAPACITY, input_size, input_hash, partial_writes,
		write_again, read_calls, reference_notice_count, early_accepted,
		early_offered, thread_growth, rss_delta, RSS_ALLOWANCE_BYTES);
	return 0;
}


static int
report_failure(
	const char *operation, pgm_status status, const pgm_error *error)
{
	fprintf(stderr, "%s failed: status=%s sqlstate=%s message=%s detail=%s\n",
		operation, pgm_status_name(status), pgm_error_sqlstate(error),
		pgm_error_message(error), pgm_error_detail(error));
	return 1;
}


static bool
execute_command(pgm_connection *connection, const char *sql)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	bool		ok = status == PGM_STATUS_OK && result != NULL &&
		pgm_result_kind(result) == PGM_RESULT_COMMAND_OK;

	if (!ok)
		(void) report_failure("execute command", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}


static bool
execute_reuse_probe(pgm_connection *connection, const char *expected)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	pgm_status	status = pgm_execute(
		connection, "SELECT 42", NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	bool		ok = status == PGM_STATUS_OK && result != NULL &&
		pgm_result_kind(result) == PGM_RESULT_TUPLES_OK &&
		pgm_result_row_count(result) == 1 &&
		pgm_result_column_count(result) == 1 &&
		pgm_result_value(result, 0, 0, &value, &error) == PGM_STATUS_OK &&
		!value.is_null && value.size == strlen(expected) &&
		memcmp(value.data, expected, value.size) == 0;

	if (!ok)
		(void) report_failure("connection reuse probe", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}


static bool
begin_copy(
	pgm_connection *connection, const char *sql,
	pgm_result_status expected_kind, pgm_request **request,
	pgm_copy **copy, pgm_error **error)
{
	pgm_execute_options options = PGM_EXECUTE_OPTIONS_INIT;
	pgm_result *result = NULL;
	pgm_status	status;

	*request = NULL;
	*copy = NULL;
	status = pgm_execute_async_ex(
		connection, sql, strlen(sql), &options, request, error);
	if (status != PGM_STATUS_OK ||
		!next_result(*request, &result, error) ||
		pgm_result_kind(result) != expected_kind)
	{
		pgm_result_free(result);
		return false;
	}
	status = pgm_result_take_copy(&result, copy, error);
	if (status != PGM_STATUS_OK || result != NULL || *copy == NULL)
	{
		pgm_result_free(result);
		return false;
	}
	return true;
}


static bool
next_result(
	pgm_request *request, pgm_result **result, pgm_error **error)
{
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_status	status = pgm_request_next_result(
		request, TEST_TIMEOUT_MS, result, &availability, error);

	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
		*result == NULL)
	{
		(void) report_failure("next result", status, *error);
		return false;
	}
	return true;
}


static bool
finish_command_request(
	pgm_request *request, const char *expected_command, pgm_error **error)
{
	pgm_result *result = NULL;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_status	status;

	status = pgm_request_next_result(
		request, TEST_TIMEOUT_MS, &result, &availability, error);
	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
		result == NULL || pgm_result_kind(result) != PGM_RESULT_COMMAND_OK ||
		strcmp(pgm_result_command_status(result), expected_command) != 0)
	{
		(void) report_failure("COPY terminal command", status, *error);
		pgm_result_free(result);
		return false;
	}
	pgm_result_free(result);
	result = NULL;
	status = pgm_request_next_result(
		request, TEST_TIMEOUT_MS, &result, &availability, error);
	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_END ||
		result != NULL)
	{
		(void) report_failure("COPY result sequence end", status, *error);
		pgm_result_free(result);
		return false;
	}
	return true;
}


static bool
finish_request(
	pgm_request *request, const char *name, pgm_error **error)
{
	pgm_result *result = NULL;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_status	status;

	status = pgm_request_next_result(
		request, TEST_TIMEOUT_MS, &result, &availability, error);
	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
		result == NULL || pgm_result_kind(result) != PGM_RESULT_COMMAND_OK ||
		strcmp(pgm_result_command_status(result), "COPY 3") != 0)
	{
		(void) report_failure("COPY terminal result", status, *error);
		pgm_result_free(result);
		return false;
	}
	printf(
		"POSTGAMMA_COPY_CASE name=%s sequence=1 kind=command "
		"bytes=0 hash=0000000000000000\n",
		name);
	pgm_result_free(result);
	result = NULL;
	status = pgm_request_next_result(
		request, TEST_TIMEOUT_MS, &result, &availability, error);
	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_END ||
		result != NULL)
	{
		(void) report_failure("COPY result sequence end", status, *error);
		pgm_result_free(result);
		return false;
	}
	return true;
}


static bool
finish_error_request(
	pgm_request *request, const char *expected_sqlstate,
	pgm_status expected_status, pgm_error **error)
{
	pgm_result *result = NULL;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_status	status = pgm_request_next_result(
		request, TEST_TIMEOUT_MS, &result, &availability, error);

	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
		result == NULL || pgm_result_kind(result) != PGM_RESULT_ERROR ||
		*error == NULL || pgm_error_status(*error) != expected_status ||
		strcmp(pgm_error_sqlstate(*error), expected_sqlstate) != 0)
	{
		(void) report_failure("COPY terminal error", status, *error);
		pgm_result_free(result);
		return false;
	}
	pgm_result_free(result);
	result = NULL;
	pgm_error_free(*error);
	*error = NULL;
	status = pgm_request_next_result(
		request, TEST_TIMEOUT_MS, &result, &availability, error);
	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_END ||
		result != NULL)
	{
		(void) report_failure("COPY error sequence end", status, *error);
		pgm_result_free(result);
		return false;
	}
	return true;
}


static bool
make_copy_data(
	unsigned char **data, size_t *size, size_t large_payload_size)
{
	static const char prefix[] = "1,alpha\n2,";
	static const char suffix[] = "\n3,omega\n";
	size_t total =
		sizeof(prefix) - 1 + large_payload_size + sizeof(suffix) - 1;
	unsigned char *created = malloc(total);

	if (created == NULL)
		return false;
	memcpy(created, prefix, sizeof(prefix) - 1);
	memset(created + sizeof(prefix) - 1, 'x', large_payload_size);
	memcpy(created + sizeof(prefix) - 1 + large_payload_size,
		   suffix, sizeof(suffix) - 1);
	*data = created;
	*size = total;
	return true;
}


static bool
make_rejected_copy_data(
	unsigned char **data, size_t *size, size_t target_size)
{
	unsigned char *created;

	if (target_size < 6 || (target_size & 1U) != 0)
		return false;
	created = malloc(target_size);
	if (created == NULL)
		return false;
	memcpy(created, "1\n2\n", 4);
	for (size_t offset = 4; offset < target_size; offset += 2)
		memcpy(created + offset, "1\n", 2);
	*data = created;
	*size = target_size;
	return true;
}


static bool
progress_briefly(pgm_request *request, pgm_error **error)
{
	pgm_request_state state;
	pgm_status	status = pgm_request_progress(request, &state, error);

	if (status != PGM_STATUS_OK)
		return false;
	return pause_milliseconds(1);
}


static bool
pause_milliseconds(long milliseconds)
{
	struct timespec delay;

	delay.tv_sec = milliseconds / 1000;
	delay.tv_nsec = (milliseconds % 1000) * 1000000L;
	while (nanosleep(&delay, &delay) != 0)
	{
		if (errno != EINTR)
			return false;
	}
	return true;
}


static uint64_t
resident_bytes(void)
{
	FILE   *stream;
	unsigned long total_pages;
	unsigned long resident_pages;
	long	page_size;

	stream = fopen("/proc/self/statm", "r");
	if (stream == NULL)
		return 0;
	if (fscanf(stream, "%lu %lu", &total_pages, &resident_pages) != 2)
	{
		(void) fclose(stream);
		return 0;
	}
	(void) fclose(stream);
	(void) total_pages;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 ||
		(uint64_t) resident_pages > UINT64_MAX / (uint64_t) page_size)
		return 0;
	return (uint64_t) resident_pages * (uint64_t) page_size;
}


static size_t
thread_count(void)
{
	DIR		   *directory;
	struct dirent *entry;
	size_t		count = 0;

	directory = opendir("/proc/self/task");
	if (directory == NULL)
		return 0;
	while ((entry = readdir(directory)) != NULL)
	{
		if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9')
			count++;
	}
	(void) closedir(directory);
	return count;
}


static uint64_t
fnv1a64(const void *data, size_t size)
{
	const unsigned char *bytes = data;
	uint64_t hash = UINT64_C(14695981039346656037);

	for (size_t index = 0; index < size; index++)
	{
		hash ^= bytes[index];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}


static void
receive_notice(void *argument, const pgm_notice *notice)
{
	(void) argument;
	if (notice != NULL && notice->sqlstate != NULL &&
		strcmp(notice->sqlstate, "00000") == 0)
		(void) atomic_fetch_add_explicit(
			&NoticeCount, 1U, memory_order_relaxed);
	if (!atomic_load_explicit(&BlockNotice, memory_order_acquire))
		return;
	atomic_store_explicit(&NoticeBlocked, true, memory_order_release);
	while (!atomic_load_explicit(&ReleaseNotice, memory_order_acquire))
		(void) pause_milliseconds(1);
}


static void *
copy_writer_main(void *argument)
{
	CopyWriter *writer = argument;

	writer->status = PGM_STATUS_OK;
	while (!atomic_load_explicit(&NoticeBlocked, memory_order_acquire))
	{
		pgm_io_state state = PGM_IO_AGAIN;
		size_t		consumed = 0;
		const void *data = NULL;
		size_t		size = 0;

		if (writer->consumed < writer->size)
		{
			data = writer->data + writer->consumed;
			size = writer->size - writer->consumed;
		}
		writer->status = pgm_copy_write(
			writer->copy, data, size, &consumed, &state, &writer->error);
		if (writer->status != PGM_STATUS_OK)
			break;
		writer->consumed += consumed;
		if (state == PGM_IO_AGAIN)
			(void) pause_milliseconds(1);
	}
	return NULL;
}


static bool
test_invalid_take(pgm_connection *connection)
{
	pgm_result *result = NULL;
	pgm_result *original;
	pgm_copy   *copy = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = pgm_execute(
		connection, "SELECT 1", NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	bool		ok;

	if (status != PGM_STATUS_OK || result == NULL)
		goto fail;
	original = result;
	status = pgm_result_take_copy(&result, &copy, &error);
	ok = status == PGM_STATUS_INVALID_ARGUMENT && result == original &&
		copy == NULL && error != NULL &&
		pgm_error_status(error) == PGM_STATUS_INVALID_ARGUMENT;
	if (!ok)
		goto fail;
	pgm_error_free(error);
	pgm_result_free(result);
	return execute_reuse_probe(connection, "42");

fail:
	(void) report_failure("invalid COPY ownership transfer", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	if (copy != NULL)
		(void) pgm_copy_close(copy, TEST_TIMEOUT_MS, NULL);
	return false;
}


static bool
test_unclaimed_copy_result(pgm_connection *connection)
{
	pgm_execute_options options = PGM_EXECUTE_OPTIONS_INIT;
	pgm_request *request = NULL;
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_status	status = pgm_execute_async_ex(
		connection, "COPY copy_orphan FROM STDIN",
		strlen("COPY copy_orphan FROM STDIN"), &options, &request, &error);
	bool		ok = false;

	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_request_next_result(
		request, TEST_TIMEOUT_MS, &result, &availability, &error);
	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
		result == NULL || pgm_result_kind(result) != PGM_RESULT_COPY_IN)
		goto fail;
	pgm_result_free(result);
	result = NULL;
	pgm_request_free(request);
	request = NULL;
	ok = execute_reuse_probe(connection, "42");

fail:
	if (!ok)
		(void) report_failure("unclaimed COPY result retirement", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	if (request != NULL)
		pgm_request_free(request);
	return ok;
}


static bool
test_early_copy_error(
	pgm_connection *connection, size_t rejected_payload_size,
	size_t *accepted, size_t *offered)
{
	pgm_request *request = NULL;
	pgm_copy   *copy = NULL;
	pgm_error  *error = NULL;
	unsigned char *data = NULL;
	size_t		size = 0;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	bool		error_seen = false;
	bool		ok = false;

	*accepted = 0;
	*offered = 0;
	if (!make_rejected_copy_data(&data, &size, rejected_payload_size) ||
		!begin_copy(
			connection, "COPY copy_reject FROM STDIN WITH (FORMAT csv)",
			PGM_RESULT_COPY_IN, &request, &copy, &error))
		goto fail;
	*offered = size;
	while (*accepted < size)
	{
		pgm_io_state state = PGM_IO_AGAIN;
		size_t		consumed = 0;

		status = pgm_copy_write(
			copy, data + *accepted, size - *accepted, &consumed, &state, &error);
		*accepted += consumed;
		if (status == PGM_STATUS_POSTGRES_ERROR)
		{
			error_seen = error != NULL &&
				pgm_error_status(error) == PGM_STATUS_POSTGRES_ERROR;
			pgm_error_free(error);
			error = NULL;
			break;
		}
		if (status != PGM_STATUS_OK || !progress_briefly(request, &error))
			goto fail;
	}
	if (!error_seen || *accepted >= size)
	{
		fprintf(stderr,
			"backend COPY error did not stop input early: accepted=%zu size=%zu\n",
			*accepted, size);
		goto fail;
	}
	status = pgm_copy_close(copy, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	copy = NULL;
	if (!finish_error_request(
			request, "23514", PGM_STATUS_POSTGRES_ERROR, &error))
		goto fail;
	pgm_request_free(request);
	request = NULL;
	ok = execute_reuse_probe(connection, "42");

fail:
	if (!ok)
		(void) report_failure("early backend COPY error", status, error);
	pgm_error_free(error);
	if (copy != NULL)
		(void) pgm_copy_close(copy, TEST_TIMEOUT_MS, NULL);
	if (request != NULL)
		pgm_request_free(request);
	free(data);
	return ok;
}


static bool
test_abort_before_first_byte(pgm_connection *connection)
{
	static const char message[] = "deterministic host abort";
	pgm_request *request = NULL;
	pgm_copy   *copy = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	bool		ok = false;

	if (!begin_copy(
			connection, "COPY copy_abort FROM STDIN", PGM_RESULT_COPY_IN,
			&request, &copy, &error))
		goto fail;
	status = pgm_copy_abort(
		copy, message, sizeof(message) - 1, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_copy_abort(copy, "ignored", strlen("ignored"), &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_copy_close(copy, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	copy = NULL;
	if (!finish_error_request(request, "57014", PGM_STATUS_CANCELED, &error))
		goto fail;
	pgm_request_free(request);
	request = NULL;
	ok = execute_reuse_probe(connection, "42");

fail:
	if (!ok)
		(void) report_failure("COPY abort before first byte", status, error);
	pgm_error_free(error);
	if (copy != NULL)
		(void) pgm_copy_close(copy, TEST_TIMEOUT_MS, NULL);
	if (request != NULL)
		pgm_request_free(request);
	return ok;
}


static bool
test_cancel_before_first_byte(pgm_connection *connection)
{
	pgm_request *request = NULL;
	pgm_copy   *copy = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	bool		ok = false;

	if (!begin_copy(
			connection, "COPY copy_abort FROM STDIN", PGM_RESULT_COPY_IN,
			&request, &copy, &error))
		goto fail;
	status = pgm_request_cancel(request, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_copy_close(copy, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	copy = NULL;
	if (!finish_error_request(request, "57014", PGM_STATUS_CANCELED, &error))
		goto fail;
	pgm_request_free(request);
	request = NULL;
	ok = execute_reuse_probe(connection, "42");

fail:
	if (!ok)
		(void) report_failure("COPY cancellation before first byte", status, error);
	pgm_error_free(error);
	if (copy != NULL)
		(void) pgm_copy_close(copy, TEST_TIMEOUT_MS, NULL);
	if (request != NULL)
		pgm_request_free(request);
	return ok;
}


static bool
test_stalled_cancel_and_timeout(
	pgm_connection *connection, uint64_t generated_rows,
	uint64_t *rss_delta, size_t *thread_growth)
{
	char		sql[192];
	pgm_request *request = NULL;
	pgm_copy   *copy = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	uint64_t	baseline_rss;
	uint64_t	stalled_rss;
	size_t		baseline_threads;
	size_t		stalled_threads;
	bool		read_data = false;
	bool		ok = false;
	int		written;

	written = snprintf(
		sql, sizeof(sql),
		"COPY (SELECT g, repeat('q', 4096) FROM generate_series(1, %" PRIu64
		") AS g) TO STDOUT WITH (FORMAT csv)",
		generated_rows);
	if (written < 0 || (size_t) written >= sizeof(sql))
		goto fail;
	baseline_rss = resident_bytes();
	baseline_threads = thread_count();
	if (baseline_rss == 0 || baseline_threads == 0 ||
		!begin_copy(
			connection, sql, PGM_RESULT_COPY_OUT, &request, &copy, &error) ||
		!pause_milliseconds(150))
		goto fail;
	stalled_rss = resident_bytes();
	stalled_threads = thread_count();
	if (stalled_rss == 0 || stalled_threads == 0 ||
		stalled_threads < baseline_threads)
		goto fail;
	*rss_delta = stalled_rss > baseline_rss ? stalled_rss - baseline_rss : 0;
	*thread_growth = stalled_threads - baseline_threads;
	if (*rss_delta > RSS_ALLOWANCE_BYTES || *thread_growth != 0)
		goto fail;
	status = pgm_copy_close(copy, 0, &error);
	if (status != PGM_STATUS_TIMEOUT || error == NULL ||
		pgm_error_status(error) != PGM_STATUS_TIMEOUT)
		goto fail;
	pgm_error_free(error);
	error = NULL;
	for (unsigned int attempt = 0; attempt < 10000 && !read_data; attempt++)
	{
		unsigned char fragment[17];
		pgm_io_state state = PGM_IO_AGAIN;
		size_t		produced = 0;

		status = pgm_copy_read(
			copy, fragment, sizeof(fragment), &produced, &state, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		read_data = produced != 0;
		if (!read_data && !progress_briefly(request, &error))
			goto fail;
	}
	if (!read_data)
		goto fail;
	status = pgm_request_cancel(request, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	status = pgm_copy_close(copy, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	copy = NULL;
	if (!finish_error_request(request, "57014", PGM_STATUS_CANCELED, &error))
		goto fail;
	pgm_request_free(request);
	request = NULL;
	ok = execute_reuse_probe(connection, "42");

fail:
	if (!ok)
		(void) report_failure("stalled COPY cancellation", status, error);
	pgm_error_free(error);
	if (copy != NULL)
		(void) pgm_copy_close(copy, TEST_TIMEOUT_MS, NULL);
	if (request != NULL)
		pgm_request_free(request);
	return ok;
}


static bool
test_deferred_request_free(pgm_connection *connection)
{
	pgm_request *request = NULL;
	pgm_copy   *copy = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	bool		ok = false;

	if (!begin_copy(
			connection, "COPY copy_orphan FROM STDIN", PGM_RESULT_COPY_IN,
			&request, &copy, &error))
		goto fail;
	pgm_request_free(request);
	request = NULL;
	status = pgm_copy_close(copy, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	copy = NULL;
	ok = execute_reuse_probe(connection, "42");

fail:
	if (!ok)
		(void) report_failure("deferred request release during COPY", status, error);
	pgm_error_free(error);
	if (copy != NULL)
		(void) pgm_copy_close(copy, TEST_TIMEOUT_MS, NULL);
	if (request != NULL)
		pgm_request_free(request);
	return ok;
}


static bool
test_concurrent_copy_owner(pgm_connection *connection)
{
	static const unsigned char row[] = "4,concurrent\n";
	pgm_request *request = NULL;
	pgm_copy   *copy = NULL;
	pgm_error  *error = NULL;
	CopyWriter writer;
	pthread_t	thread;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	bool		thread_started = false;
	bool		ok = false;

	memset(&writer, 0, sizeof(writer));
	if (!begin_copy(
			connection, "COPY copy_busy_fixture FROM STDIN WITH (FORMAT csv)",
			PGM_RESULT_COPY_IN, &request, &copy, &error))
		goto fail;
	writer.copy = copy;
	writer.data = row;
	writer.size = sizeof(row) - 1;
	atomic_store_explicit(&NoticeBlocked, false, memory_order_release);
	atomic_store_explicit(&ReleaseNotice, false, memory_order_release);
	atomic_store_explicit(&BlockNotice, true, memory_order_release);
	if (pthread_create(&thread, NULL, copy_writer_main, &writer) != 0)
		goto fail;
	thread_started = true;
	for (unsigned int attempt = 0; attempt < 30000; attempt++)
	{
		if (atomic_load_explicit(&NoticeBlocked, memory_order_acquire))
			break;
		if (!pause_milliseconds(1))
			goto fail;
	}
	if (!atomic_load_explicit(&NoticeBlocked, memory_order_acquire))
		goto fail;
	status = pgm_copy_close(copy, 0, &error);
	if (status != PGM_STATUS_BUSY || error == NULL ||
		pgm_error_status(error) != PGM_STATUS_BUSY)
		goto fail;
	pgm_error_free(error);
	error = NULL;
	atomic_store_explicit(&ReleaseNotice, true, memory_order_release);
	if (pthread_join(thread, NULL) != 0)
		goto fail;
	thread_started = false;
	atomic_store_explicit(&BlockNotice, false, memory_order_release);
	if (writer.status != PGM_STATUS_OK || writer.error != NULL ||
		writer.consumed != writer.size)
		goto fail;
	for (;;)
	{
		pgm_io_state state = PGM_IO_AGAIN;

		status = pgm_copy_finish(copy, NULL, 0, &state, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		if (state == PGM_IO_END)
			break;
		if (!progress_briefly(request, &error))
			goto fail;
	}
	status = pgm_copy_close(copy, TEST_TIMEOUT_MS, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	copy = NULL;
	if (!finish_command_request(request, "COPY 1", &error))
		goto fail;
	pgm_request_free(request);
	request = NULL;
	ok = execute_reuse_probe(connection, "42");

fail:
	atomic_store_explicit(&ReleaseNotice, true, memory_order_release);
	if (thread_started)
		(void) pthread_join(thread, NULL);
	atomic_store_explicit(&BlockNotice, false, memory_order_release);
	if (!ok)
		(void) report_failure("concurrent COPY ownership", status, error);
	pgm_error_free(writer.error);
	pgm_error_free(error);
	if (copy != NULL)
		(void) pgm_copy_close(copy, TEST_TIMEOUT_MS, NULL);
	if (request != NULL)
		pgm_request_free(request);
	return ok;
}
