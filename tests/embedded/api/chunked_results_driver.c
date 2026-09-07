#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#define TEST_TIMEOUT_MS INT64_C(30000)
#define INSTANCE_RESULT_LIMIT (64U * 1024U)
#define INSTANCE_VALUE_LIMIT (8U * 1024U)
#define SLOW_ROW_COUNT UINT64_C(10000000)
#define SLOW_CHUNK_ROWS UINT32_C(256)
#define RSS_ALLOWANCE_BYTES UINT64_C(67108864)


typedef struct ExpectedResult
{
	pgm_result_status kind;
	size_t		rows;
} ExpectedResult;


static int report_failure(
	const char *operation, pgm_status status, const pgm_error *error);
static bool emit_case(
	const char *name, size_t sequence, const pgm_result *result,
	const pgm_error *error);
static bool consume_expected(
	pgm_request *request, const char *name, size_t first_sequence,
	const ExpectedResult *expected, size_t expected_count,
	bool expect_sql_error);
static bool expect_request_end(pgm_request *request);
static bool expect_text_value(
	const pgm_result *result, size_t row, size_t column,
	const char *expected);
static bool execute_reuse_probe(pgm_connection *connection, const char *value);
static uint64_t resident_bytes(void);
static size_t thread_count(void);
static bool pause_milliseconds(long milliseconds);
static void emit_hex(const void *data, size_t size);
static const char *result_kind_name(pgm_result_status kind);


int
main(int argument_count, char **arguments)
{
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_execute_options options = PGM_EXECUTE_OPTIONS_INIT;
	pgm_script_options script_options = PGM_SCRIPT_OPTIONS_INIT;
	pgm_instance *instance = NULL;
	pgm_connection *connection = NULL;
	pgm_request *request = NULL;
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	uint64_t	baseline_rss;
	uint64_t	peak_rss;
	uint64_t	rss_delta;
	uint64_t	slow_rows = 0;
	size_t		slow_chunks = 0;
	size_t		baseline_threads;
	size_t		maximum_threads;
	size_t		terminal_results = 0;
	uint64_t	slow_row_target = SLOW_ROW_COUNT;
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
		slow_row_target = UINT64_C(10000);
	if ((pgm_capabilities() & PGM_CAP_CHUNKED_RESULTS) == 0)
	{
		fprintf(stderr, "chunked-result capability is not advertised\n");
		goto fail;
	}
	instance_options.path = arguments[1];
	instance_options.create = UINT32_C(1);
	instance_options.executable_path = arguments[2];
	instance_options.resource_root = arguments[3];
	instance_options.result_buffer_limit = INSTANCE_RESULT_LIMIT;
	instance_options.maximum_value_size = INSTANCE_VALUE_LIMIT;
	status = pgm_instance_open(&instance_options, &instance, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-c-api-chunked";
	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;

	options.delivery_mode = PGM_DELIVERY_CHUNKED;
	options.target_chunk_rows = UINT32_C(0);
	status = pgm_execute_async_ex(
		connection, "SELECT 1", strlen("SELECT 1"), &options, &request, &error);
	if (status != PGM_STATUS_INVALID_ARGUMENT || request != NULL || error == NULL)
	{
		fprintf(stderr, "chunked delivery accepted a zero row target\n");
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	options.delivery_mode = PGM_DELIVERY_MATERIALIZED;
	options.target_chunk_rows = UINT32_C(1);
	status = pgm_execute_async_ex(
		connection, "SELECT 1", strlen("SELECT 1"), &options, &request, &error);
	if (status != PGM_STATUS_INVALID_ARGUMENT || request != NULL || error == NULL)
	{
		fprintf(stderr, "materialized delivery accepted a chunk row target\n");
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	options.target_chunk_rows = UINT32_C(0);
	options.result_buffer_limit = INSTANCE_RESULT_LIMIT + 1U;
	status = pgm_execute_async_ex(
		connection, "SELECT 1", strlen("SELECT 1"), &options, &request, &error);
	if (status != PGM_STATUS_INVALID_ARGUMENT || request != NULL || error == NULL)
	{
		fprintf(stderr, "a request raised the instance result limit\n");
		goto fail;
	}
	pgm_error_free(error);
	error = NULL;
	options.result_buffer_limit = 0;

	{
		static const char sql[] =
			"WITH delay AS MATERIALIZED (SELECT pg_sleep(0.1)) "
			"SELECT g FROM delay, generate_series(1, 10) AS g";
		static const ExpectedResult remaining[] =
		{
			{PGM_RESULT_TUPLES_CHUNK, 3},
			{PGM_RESULT_TUPLES_CHUNK, 3},
			{PGM_RESULT_TUPLES_CHUNK, 1},
			{PGM_RESULT_TUPLES_OK, 0},
		};

		options.delivery_mode = PGM_DELIVERY_CHUNKED;
		options.target_chunk_rows = UINT32_C(3);
		status = pgm_execute_async_ex(
			connection, sql, strlen(sql), &options, &request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = pgm_request_wait(request, TEST_TIMEOUT_MS, &result, &error);
		if (status != PGM_STATUS_UNSUPPORTED || result != NULL || error == NULL)
		{
			fprintf(stderr, "single-result wait consumed a chunked request\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
		status = pgm_request_next_result(
			request, 0, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_AGAIN ||
			result != NULL || error != NULL)
		{
			fprintf(stderr, "zero-timeout next-result did not report AGAIN\n");
			goto fail;
		}
		status = pgm_request_next_result(
			request, TEST_TIMEOUT_MS, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
			pgm_result_kind(result) != PGM_RESULT_TUPLES_CHUNK ||
			pgm_result_row_count(result) != 3 ||
			!emit_case("basic", 0, result, NULL))
			goto fail;
		{
			pgm_result *outstanding = result;

			status = pgm_request_next_result(
				request, 0, &result, &availability, &error);
			if (status != PGM_STATUS_BUSY || error == NULL)
			{
				fprintf(stderr, "outstanding chunk ownership was not enforced\n");
				pgm_result_free(outstanding);
				goto fail;
			}
			pgm_error_free(error);
			error = NULL;
			pgm_result_free(outstanding);
		}
		result = NULL;
		if (!consume_expected(
				request, "basic", 1, remaining,
				sizeof(remaining) / sizeof(remaining[0]), false))
			goto fail;
		terminal_results++;
		pgm_request_free(request);
		request = NULL;
	}

	{
		static const ExpectedResult expected[] =
		{
			{PGM_RESULT_TUPLES_CHUNK, 2},
			{PGM_RESULT_TUPLES_CHUNK, 2},
			{PGM_RESULT_TUPLES_CHUNK, 1},
			{PGM_RESULT_TUPLES_OK, 0},
		};
		static const char sql[] = "SELECT g::int4 FROM generate_series(1, 5) AS g";

		options.result_format = PGM_FORMAT_BINARY;
		options.target_chunk_rows = UINT32_C(2);
		status = pgm_execute_async_ex(
			connection, sql, strlen(sql), &options, &request, &error);
		if (status != PGM_STATUS_OK ||
			!consume_expected(
				request, "binary", 0, expected,
				sizeof(expected) / sizeof(expected[0]), false))
			goto fail;
		terminal_results++;
		pgm_request_free(request);
		request = NULL;
		options.result_format = PGM_FORMAT_TEXT;
	}

	{
		static const ExpectedResult expected[] =
		{
			{PGM_RESULT_TUPLES_CHUNK, 2},
			{PGM_RESULT_TUPLES_CHUNK, 2},
			{PGM_RESULT_TUPLES_OK, 0},
			{PGM_RESULT_COMMAND_OK, 0},
			{PGM_RESULT_TUPLES_CHUNK, 2},
			{PGM_RESULT_TUPLES_CHUNK, 1},
			{PGM_RESULT_TUPLES_OK, 0},
		};
		static const char sql[] =
			"SELECT g FROM generate_series(1, 4) AS g; "
			"SET application_name = 'postgamma-chunked_results-script'; "
			"SELECT g FROM generate_series(7, 9) AS g";

		script_options.delivery_mode = PGM_DELIVERY_CHUNKED;
		script_options.target_chunk_rows = UINT32_C(2);
		status = pgm_execute_script_async(
			connection, sql, strlen(sql), &script_options, &request, &error);
		if (status != PGM_STATUS_OK ||
			!consume_expected(
				request, "script", 0, expected,
				sizeof(expected) / sizeof(expected[0]), false))
			goto fail;
		terminal_results += 2;
		pgm_request_free(request);
		request = NULL;
	}

	{
		static const ExpectedResult expected[] =
		{
			{PGM_RESULT_TUPLES_CHUNK, 3},
			{PGM_RESULT_TUPLES_CHUNK, 3},
			{PGM_RESULT_ERROR, 0},
		};
		static const char sql[] =
			"SELECT 10 / (g - 8) FROM generate_series(1, 10) AS g";

		options.target_chunk_rows = UINT32_C(3);
		status = pgm_execute_async_ex(
			connection, sql, strlen(sql), &options, &request, &error);
		if (status != PGM_STATUS_OK ||
			!consume_expected(
				request, "late_error", 0, expected,
				sizeof(expected) / sizeof(expected[0]), true))
			goto fail;
		terminal_results++;
		pgm_request_free(request);
		request = NULL;
		if (!execute_reuse_probe(connection, "42"))
			goto fail;
		terminal_results++;
	}

	options.delivery_mode = PGM_DELIVERY_MATERIALIZED;
	options.target_chunk_rows = UINT32_C(0);
	{
		static const char sql[] = "SELECT repeat('x', 8192)";

		status = pgm_execute_async_ex(
			connection, sql, strlen(sql), &options, &request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = pgm_request_next_result(
			request, TEST_TIMEOUT_MS, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
			pgm_result_kind(result) != PGM_RESULT_TUPLES_OK ||
			pgm_result_row_count(result) != 1 ||
			pgm_result_column_count(result) != 1)
			goto fail;
		{
			pgm_value_view value = PGM_VALUE_VIEW_INIT;

			if (pgm_result_value(result, 0, 0, &value, &error) != PGM_STATUS_OK ||
				value.is_null != 0 || value.size != INSTANCE_VALUE_LIMIT)
				goto fail;
		}
		pgm_result_free(result);
		result = NULL;
		if (!expect_request_end(request))
			goto fail;
		pgm_request_free(request);
		request = NULL;
	}

	{
		static const char sql[] = "SELECT repeat('x', 8193)";

		status = pgm_execute_async_ex(
			connection, sql, strlen(sql), &options, &request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = pgm_request_next_result(
			request, TEST_TIMEOUT_MS, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
			pgm_result_kind(result) != PGM_RESULT_ERROR || error == NULL ||
			strcmp(pgm_error_sqlstate(error), "54000") != 0)
			goto fail;
		pgm_error_free(error);
		error = NULL;
		pgm_result_free(result);
		result = NULL;
		if (!expect_request_end(request))
			goto fail;
		pgm_request_free(request);
		request = NULL;
		if (!execute_reuse_probe(connection, "43"))
			goto fail;
	}

	{
		static const char sql[] =
			"SELECT repeat('y', 20) FROM generate_series(1, 100)";

		options.result_buffer_limit = 1024;
		status = pgm_execute_async_ex(
			connection, sql, strlen(sql), &options, &request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = pgm_request_next_result(
			request, TEST_TIMEOUT_MS, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
			pgm_result_kind(result) != PGM_RESULT_ERROR || error == NULL ||
			strcmp(pgm_error_sqlstate(error), "54000") != 0)
			goto fail;
		pgm_error_free(error);
		error = NULL;
		pgm_result_free(result);
		result = NULL;
		if (!expect_request_end(request))
			goto fail;
		pgm_request_free(request);
		request = NULL;
		options.result_buffer_limit = 0;
		if (!execute_reuse_probe(connection, "44"))
			goto fail;
	}

	{
		static const char sql[] =
			"SELECT generate_series(1, 10000000)";

		options.delivery_mode = PGM_DELIVERY_CHUNKED;
		options.target_chunk_rows = SLOW_CHUNK_ROWS;
		status = pgm_execute_async_ex(
			connection, sql, strlen(sql), &options, &request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = pgm_request_next_result(
			request, TEST_TIMEOUT_MS, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
			pgm_result_kind(result) != PGM_RESULT_TUPLES_CHUNK)
			goto fail;
		pgm_result_free(result);
		result = NULL;
		pgm_request_free(request);
		request = NULL;
		if (!execute_reuse_probe(connection, "45"))
			goto fail;
	}

	baseline_rss = resident_bytes();
	baseline_threads = thread_count();
	if (baseline_rss == 0 || baseline_threads == 0)
	{
		fprintf(stderr, "could not read Linux process resource counters\n");
		goto fail;
	}
	peak_rss = baseline_rss;
	maximum_threads = baseline_threads;
	{
		char		sql[96];
		bool		terminal_seen = false;
		int		written;

		written = snprintf(
			sql, sizeof(sql), "SELECT generate_series(1, %" PRIu64 ")",
			slow_row_target);
		if (written < 0 || (size_t) written >= sizeof(sql))
			goto fail;

		options.delivery_mode = PGM_DELIVERY_CHUNKED;
		options.target_chunk_rows = SLOW_CHUNK_ROWS;
		status = pgm_execute_async_ex(
			connection, sql, strlen(sql), &options, &request, &error);
		if (status != PGM_STATUS_OK)
			goto fail;
		status = pgm_request_next_result(
			request, TEST_TIMEOUT_MS, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
			pgm_result_kind(result) != PGM_RESULT_TUPLES_CHUNK ||
			pgm_result_row_count(result) == 0 ||
			pgm_result_row_count(result) > SLOW_CHUNK_ROWS)
			goto fail;
		slow_rows += pgm_result_row_count(result);
		slow_chunks++;
		if (!pause_milliseconds(150))
			goto fail;
		{
			uint64_t current_rss = resident_bytes();
			size_t	current_threads = thread_count();

			if (current_rss == 0 || current_threads == 0)
				goto fail;
			if (current_rss > peak_rss)
				peak_rss = current_rss;
			if (current_threads > maximum_threads)
				maximum_threads = current_threads;
		}
		pgm_result_free(result);
		result = NULL;
		for (;;)
		{
			status = pgm_request_next_result(
				request, TEST_TIMEOUT_MS, &result, &availability, &error);
			if (status != PGM_STATUS_OK)
				goto fail;
			if (availability == PGM_AVAILABILITY_END)
				break;
			if (availability != PGM_AVAILABILITY_READY || result == NULL)
				goto fail;
			if (pgm_result_kind(result) == PGM_RESULT_TUPLES_CHUNK)
			{
				size_t rows = pgm_result_row_count(result);

				if (rows == 0 || rows > SLOW_CHUNK_ROWS)
					goto fail;
				slow_rows += rows;
				slow_chunks++;
			}
			else if (pgm_result_kind(result) == PGM_RESULT_TUPLES_OK &&
					 pgm_result_row_count(result) == 0)
				terminal_seen = true;
			else
				goto fail;
			pgm_result_free(result);
			result = NULL;
			if ((slow_chunks & 255U) == 0)
			{
				uint64_t current_rss = resident_bytes();
				size_t	current_threads = thread_count();

				if (current_rss == 0 || current_threads == 0)
					goto fail;
				if (current_rss > peak_rss)
					peak_rss = current_rss;
				if (current_threads > maximum_threads)
					maximum_threads = current_threads;
			}
		}
		if (!terminal_seen || slow_rows != slow_row_target ||
			slow_chunks !=
				(size_t) ((slow_row_target + SLOW_CHUNK_ROWS - 1U) /
					SLOW_CHUNK_ROWS))
		{
			fprintf(stderr,
				"large stream boundary mismatch: rows=%" PRIu64
				" chunks=%zu terminal=%s\n",
				slow_rows, slow_chunks, terminal_seen ? "true" : "false");
			goto fail;
		}
		terminal_results++;
		pgm_request_free(request);
		request = NULL;
	}
	if (maximum_threads != baseline_threads)
	{
		fprintf(stderr,
			"slow consumer grew the thread set: before=%zu maximum=%zu\n",
			baseline_threads, maximum_threads);
		goto fail;
	}
	rss_delta = peak_rss > baseline_rss ? peak_rss - baseline_rss : 0;
	if (rss_delta > RSS_ALLOWANCE_BYTES)
	{
		fprintf(stderr,
			"large stream exceeded RSS allowance: delta=%" PRIu64
			" allowance=%" PRIu64 "\n",
			rss_delta, RSS_ALLOWANCE_BYTES);
		goto fail;
	}
	if (!execute_reuse_probe(connection, "46"))
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
		(void) report_failure("chunked results chunked-results gate", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	if (request != NULL)
		pgm_request_free(request);
	if (connection != NULL)
		(void) pgm_connection_close(connection, TEST_TIMEOUT_MS, NULL);
	if (instance != NULL)
		(void) pgm_instance_close(
			instance, PGM_SHUTDOWN_FAST, TEST_TIMEOUT_MS, NULL);
	if (!passed)
		return 1;
	printf(
		"POSTGAMMA_STREAMING_CHUNKED postgres=19 chunked_sequences=5 "
		"common_streamed_rows=28 common_terminal_results=6 "
		"observed_terminal_results=%zu text_binary=true "
		"script=true late_error=true wait_nonconsuming=true ownership=true "
		"limit_sqlstate=54000 maximum_value_limit=true result_buffer_limit=true "
		"slow_rows=%" PRIu64 " slow_chunks=%zu slow_terminal=true "
		"stalled_consumer=true thread_growth=%zu rss_delta_bytes=%" PRIu64
		" rss_allowance_bytes=%" PRIu64 " partial_retirement=true "
		"connection_reuse=true chunked_capability=true phase=closed\n",
		terminal_results, slow_rows, slow_chunks,
		maximum_threads - baseline_threads, rss_delta, RSS_ALLOWANCE_BYTES);
	return 0;
}


static int
report_failure(
	const char *operation, pgm_status status, const pgm_error *error)
{
	fprintf(stderr,
		"%s failed: status=%s sqlstate=%s message=%s detail=%s\n",
		operation, pgm_status_name(status), pgm_error_sqlstate(error),
		pgm_error_message(error), pgm_error_detail(error));
	return 1;
}


static bool
consume_expected(
	pgm_request *request, const char *name, size_t first_sequence,
	const ExpectedResult *expected, size_t expected_count,
	bool expect_sql_error)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_status	status;
	bool		ok = false;

	for (size_t index = 0; index < expected_count; index++)
	{
		status = pgm_request_next_result(
			request, TEST_TIMEOUT_MS, &result, &availability, &error);
		if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_READY ||
			result == NULL || pgm_result_kind(result) != expected[index].kind ||
			pgm_result_row_count(result) != expected[index].rows)
			goto done;
		if (expected[index].kind == PGM_RESULT_ERROR)
		{
			if (!expect_sql_error || error == NULL ||
				strcmp(pgm_error_sqlstate(error), "22012") != 0)
				goto done;
		}
		else if (error != NULL)
			goto done;
		if (!emit_case(name, first_sequence + index, result, error))
			goto done;
		pgm_error_free(error);
		error = NULL;
		pgm_result_free(result);
		result = NULL;
	}
	status = pgm_request_next_result(
		request, TEST_TIMEOUT_MS, &result, &availability, &error);
	if (status != PGM_STATUS_OK || availability != PGM_AVAILABILITY_END ||
		result != NULL || error != NULL)
		goto done;
	ok = true;

done:
	if (!ok)
		(void) report_failure("consume expected result sequence", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}


static bool
expect_request_end(pgm_request *request)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_availability availability = PGM_AVAILABILITY_AGAIN;
	pgm_status	status;

	status = pgm_request_next_result(
		request, TEST_TIMEOUT_MS, &result, &availability, &error);
	if (status == PGM_STATUS_OK && availability == PGM_AVAILABILITY_END &&
		result == NULL && error == NULL)
		return true;
	(void) report_failure("observe request end", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return false;
}


static bool
expect_text_value(
	const pgm_result *result, size_t row, size_t column,
	const char *expected)
{
	pgm_value_view value = PGM_VALUE_VIEW_INIT;
	size_t		expected_size = strlen(expected);

	return pgm_result_value(result, row, column, &value, NULL) == PGM_STATUS_OK &&
		value.format == PGM_FORMAT_TEXT && value.is_null == 0 &&
		value.size == expected_size &&
		memcmp(value.data, expected, expected_size) == 0;
}


static bool
execute_reuse_probe(pgm_connection *connection, const char *value)
{
	pgm_result *result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status;
	char		sql[64];
	bool		ok;

	if (snprintf(sql, sizeof(sql), "SELECT %s", value) < 0)
		return false;
	status = pgm_execute(
		connection, sql, NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS,
		&result, &error);
	ok = status == PGM_STATUS_OK && result != NULL &&
		pgm_result_kind(result) == PGM_RESULT_TUPLES_OK &&
		expect_text_value(result, 0, 0, value);
	if (strcmp(value, "42") == 0 && ok)
		ok = emit_case("reuse_after_late_error", 0, result, NULL);
	if (!ok)
		(void) report_failure("connection reuse probe", status, error);
	pgm_error_free(error);
	pgm_result_free(result);
	return ok;
}


static bool
emit_case(
	const char *name, size_t sequence, const pgm_result *result,
	const pgm_error *error)
{
	pgm_column column = PGM_COLUMN_INIT;
	pgm_value_view first = PGM_VALUE_VIEW_INIT;
	pgm_value_view last = PGM_VALUE_VIEW_INIT;
	const char *command;
	const char *sqlstate;
	size_t		rows;
	size_t		columns;
	uint16_t	format = PGM_FORMAT_TEXT;

	if (name == NULL || result == NULL)
		return false;
	rows = pgm_result_row_count(result);
	columns = pgm_result_column_count(result);
	if (columns > 0)
	{
		if (pgm_result_column(result, 0, &column, NULL) != PGM_STATUS_OK)
			return false;
		format = column.format;
	}
	if (rows > 0 && columns > 0)
	{
		if (pgm_result_value(result, 0, 0, &first, NULL) != PGM_STATUS_OK ||
			pgm_result_value(result, rows - 1, 0, &last, NULL) != PGM_STATUS_OK ||
			first.is_null != 0 || last.is_null != 0)
			return false;
	}
	command = pgm_result_command_status(result);
	sqlstate = pgm_error_sqlstate(error);
	printf(
		"POSTGAMMA_STREAMING_CASE name=%s sequence=%zu kind=%s rows=%zu "
		"columns=%zu format=%u first=",
		name, sequence, result_kind_name(pgm_result_kind(result)), rows, columns,
		(unsigned int) format);
	if (first.data != NULL)
		emit_hex(first.data, first.size);
	printf(" last=");
	if (last.data != NULL)
		emit_hex(last.data, last.size);
	printf(" command=");
	emit_hex(command, strlen(command));
	printf(" sqlstate=");
	emit_hex(sqlstate, strlen(sqlstate));
	(void) putchar('\n');
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


static void
emit_hex(const void *data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	const unsigned char *bytes = data;

	for (size_t index = 0; index < size; index++)
	{
		(void) putchar(digits[bytes[index] >> 4]);
		(void) putchar(digits[bytes[index] & 15]);
	}
}


static const char *
result_kind_name(pgm_result_status kind)
{
	switch (kind)
	{
		case PGM_RESULT_COMMAND_OK:
			return "command";
		case PGM_RESULT_TUPLES_OK:
			return "tuples";
		case PGM_RESULT_TUPLES_CHUNK:
			return "tuples_chunk";
		case PGM_RESULT_ERROR:
			return "error";
		default:
			return "other";
	}
}
