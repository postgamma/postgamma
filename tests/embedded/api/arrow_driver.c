#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma.h"
#include "postgamma/postgamma_arrow.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define TEST_TIMEOUT_MS INT64_C(30000)
#define COLUMN_COUNT 15


/* Independent consumer-side Arrow C Data definitions. */
struct ArrowSchema
{
	const char *format;
	const char *name;
	const char *metadata;
	int64_t		flags;
	int64_t		n_children;
	struct ArrowSchema **children;
	struct ArrowSchema *dictionary;
	void		(*release) (struct ArrowSchema *);
	void	   *private_data;
};

struct ArrowArray
{
	int64_t		length;
	int64_t		null_count;
	int64_t		offset;
	int64_t		n_buffers;
	int64_t		n_children;
	const void **buffers;
	struct ArrowArray **children;
	struct ArrowArray *dictionary;
	void		(*release) (struct ArrowArray *);
	void	   *private_data;
};


static int report_failure(
	const char *operation, pgm_status status, const pgm_error *error);
static bool validate_export(pgm_result **result, bool binary);
static bool is_valid(const struct ArrowArray *array, size_t row);
static int32_t read_i32_le(const unsigned char *data);
static bool metadata_value(
	const char *metadata, const char *key, char *value, size_t capacity);
static int64_t fixed_i64(const struct ArrowArray *array, size_t row);
static int32_t fixed_i32(const struct ArrowArray *array, size_t row);
static int16_t fixed_i16(const struct ArrowArray *array, size_t row);
static float fixed_float(const struct ArrowArray *array, size_t row);
static double fixed_double(const struct ArrowArray *array, size_t row);
static bool variable_equals(
	const struct ArrowArray *array, size_t row,
	const void *expected, size_t expected_size);


int
main(int argument_count, char **arguments)
{
	static const char query[] =
		"SELECT * FROM (VALUES "
		"(true::bool, (-1234)::int2, 123456::int4, 42::oid, "
		"(-9223372036854775807)::int8, 1.25::float4, (-2.5)::float8, "
		"DATE '1970-01-02', TIMESTAMP '1970-01-01 00:00:00.123456', "
		"TIMESTAMPTZ '1970-01-01 00:00:00+00', "
		"'550e8400-e29b-41d4-a716-446655440000'::uuid, "
		"decode('0001ff', 'hex')::bytea, 'hello'::text, "
		"12.340::numeric, NULL::int4), "
		"(NULL::bool, NULL::int2, NULL::int4, NULL::oid, NULL::int8, "
		"NULL::float4, NULL::float8, NULL::date, NULL::timestamp, "
		"NULL::timestamptz, NULL::uuid, NULL::bytea, NULL::text, "
		"NULL::numeric, 7::int4)) AS v("
		"b,i2,i4,o,i8,f4,f8,d,ts,tstz,u,bytes,text_value,numeric_value,n)";
	pgm_instance_options instance_options = PGM_INSTANCE_OPTIONS_INIT;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_instance *instance = NULL;
	pgm_connection *connection = NULL;
	pgm_result *result = NULL;
	pgm_result *command_result = NULL;
	pgm_error  *error = NULL;
	pgm_status	status = PGM_STATUS_INTERNAL_ERROR;
	bool		passed = false;

	if (argument_count != 5 || strcmp(arguments[4], "create") != 0)
	{
		fprintf(stderr,
			"usage: %s DATA_DIRECTORY EXECUTABLE_PATH RESOURCE_ROOT create\n",
			arguments[0]);
		return 2;
	}
	if ((pgm_capabilities() & PGM_CAP_ARROW_C_DATA) == 0)
	{
		fprintf(stderr, "Arrow C Data capability is not advertised\n");
		goto fail;
	}
	instance_options.path = arguments[1];
	instance_options.create = UINT32_C(1);
	instance_options.executable_path = arguments[2];
	instance_options.resource_root = arguments[3];
	status = pgm_instance_open(&instance_options, &instance, &error);
	if (status != PGM_STATUS_OK)
		goto fail;
	connection_options.user = "postgamma";
	connection_options.database = "postgres";
	connection_options.application_name = "postgamma-c-api-arrow";
	status = pgm_connection_open(
		instance, &connection_options, &connection, &error);
	if (status != PGM_STATUS_OK)
		goto fail;

	for (uint16_t format = PGM_FORMAT_TEXT;
		 format <= PGM_FORMAT_BINARY; format++)
	{
		status = pgm_execute(
			connection, query, NULL, 0, format, TEST_TIMEOUT_MS,
			&result, &error);
		if (status != PGM_STATUS_OK || result == NULL ||
			!validate_export(&result, format == PGM_FORMAT_BINARY))
			goto fail;
		result = NULL;
	}

	status = pgm_execute(
		connection, "CREATE TEMP TABLE arrow_command(i int)",
		NULL, 0, PGM_FORMAT_TEXT, TEST_TIMEOUT_MS, &command_result, &error);
	if (status != PGM_STATUS_OK || command_result == NULL)
		goto fail;
	{
		struct ArrowSchema schema;
		struct ArrowArray array;

		memset(&schema, 0x5a, sizeof(schema));
		memset(&array, 0x5a, sizeof(array));
		status = pgm_result_export_arrow(
			command_result, &schema, &array, &error);
		if (status != PGM_STATUS_INVALID_ARGUMENT || schema.release != NULL ||
			array.release != NULL ||
			pgm_result_kind(command_result) != PGM_RESULT_COMMAND_OK)
		{
			fprintf(stderr, "Arrow conversion failure consumed its source result\n");
			goto fail;
		}
		pgm_error_free(error);
		error = NULL;
	}
	pgm_result_free(command_result);
	command_result = NULL;
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
	printf(
		"POSTGAMMA_KERNEL_3_ARROW formats=text,binary rows=2 columns=%d "
		"primitive_mapping=true nulls=true metadata=true fallback=true "
		"result_independent=true release=true no_arrow_runtime=true phase=closed\n",
		COLUMN_COUNT);

fail:
	if (!passed)
		(void) report_failure("Arrow management Arrow driver", status, error);
	pgm_error_free(error);
	pgm_result_free(command_result);
	pgm_result_free(result);
	if (connection != NULL)
		(void) pgm_connection_close(connection, TEST_TIMEOUT_MS, NULL);
	if (instance != NULL)
		(void) pgm_instance_close(
			instance, PGM_SHUTDOWN_IMMEDIATE, TEST_TIMEOUT_MS, NULL);
	return passed ? 0 : 1;
}


static int
report_failure(
	const char *operation, pgm_status status, const pgm_error *error)
{
	fprintf(stderr, "%s failed: status=%s message=%s detail=%s\n",
		operation, pgm_status_name(status),
		error != NULL ? pgm_error_message(error) : "",
		error != NULL ? pgm_error_detail(error) : "");
	return 1;
}


static bool
validate_export(pgm_result **result_pointer, bool binary)
{
	static const char *const formats[COLUMN_COUNT] = {
		"b", "s", "i", "I", "l", "f", "g", "tdD", "tsu:",
		"tsu:UTC", "w:16", "z", "u", NULL, "i",
	};
	static const unsigned char expected_uuid[16] = {
		0x55, 0x0e, 0x84, 0x00, 0xe2, 0x9b, 0x41, 0xd4,
		0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00,
	};
	static const unsigned char expected_bytes[3] = {0x00, 0x01, 0xff};
	struct ArrowSchema schema;
	struct ArrowArray array;
	char		metadata[64];
	pgm_error  *error = NULL;
	pgm_status	status;
	bool		ok = false;
	pgm_result *result = *result_pointer;

	memset(&schema, 0, sizeof(schema));
	memset(&array, 0, sizeof(array));
	status = pgm_result_export_arrow(result, &schema, &array, &error);
	if (status != PGM_STATUS_OK || schema.release == NULL ||
		array.release == NULL || strcmp(schema.format, "+s") != 0 ||
		schema.n_children != COLUMN_COUNT || array.n_children != COLUMN_COUNT ||
		array.length != 2)
		goto done;
	pgm_result_free(result);
	result = NULL;
	*result_pointer = NULL;
	for (size_t column = 0; column < COLUMN_COUNT; column++)
	{
		const char *expected_format = formats[column] != NULL ? formats[column] :
			(binary ? "z" : "u");

		if (schema.children[column] == NULL || array.children[column] == NULL ||
			strcmp(schema.children[column]->format, expected_format) != 0 ||
			array.children[column]->length != 2 ||
			array.children[column]->null_count != 1 ||
			!is_valid(array.children[column], column == 14 ? 1 : 0) ||
			is_valid(array.children[column], column == 14 ? 0 : 1) ||
			!metadata_value(
				schema.children[column]->metadata,
				"postgamma.postgresql.type_oid", metadata, sizeof(metadata)))
			goto done;
	}
	if (!is_valid(array.children[0], 0) ||
		(((const unsigned char *) array.children[0]->buffers[1])[0] & 1U) == 0 ||
		fixed_i16(array.children[1], 0) != -1234 ||
		fixed_i32(array.children[2], 0) != 123456 ||
		(uint32_t) fixed_i32(array.children[3], 0) != UINT32_C(42) ||
		fixed_i64(array.children[4], 0) != INT64_C(-9223372036854775807) ||
		fixed_float(array.children[5], 0) != 1.25F ||
		fixed_double(array.children[6], 0) != -2.5 ||
		fixed_i32(array.children[7], 0) != 1 ||
		fixed_i64(array.children[8], 0) != INT64_C(123456) ||
		fixed_i64(array.children[9], 0) != 0 ||
		memcmp(array.children[10]->buffers[1], expected_uuid, 16) != 0 ||
		!variable_equals(array.children[11], 0, expected_bytes, 3) ||
		!variable_equals(array.children[12], 0, "hello", 5) ||
		!variable_equals(
			array.children[13], 0,
			binary ? (const void *) array.children[13]->buffers[2] :
				(const void *) "12.340",
			binary ? (size_t) ((const int32_t *)
				array.children[13]->buffers[1])[1] : 6) ||
		fixed_i32(array.children[14], 1) != 7)
		goto done;
	ok = true;

done:
	pgm_error_free(error);
	pgm_result_free(result);
	*result_pointer = NULL;
	if (schema.release != NULL)
		schema.release(&schema);
	if (array.release != NULL)
		array.release(&array);
	if (ok && (schema.release != NULL || array.release != NULL))
		ok = false;
	return ok;
}


static bool
is_valid(const struct ArrowArray *array, size_t row)
{
	const unsigned char *bitmap = array->buffers[0];

	return bitmap != NULL && (bitmap[row / 8] & (1U << (row % 8))) != 0;
}


static int32_t
read_i32_le(const unsigned char *data)
{
	return (int32_t) ((uint32_t) data[0] | ((uint32_t) data[1] << 8) |
		((uint32_t) data[2] << 16) | ((uint32_t) data[3] << 24));
}


static bool
metadata_value(
	const char *metadata, const char *key, char *value, size_t capacity)
{
	const unsigned char *cursor = (const unsigned char *) metadata;
	int32_t		pairs;

	if (metadata == NULL || value == NULL || capacity == 0)
		return false;
	pairs = read_i32_le(cursor);
	cursor += 4;
	for (int32_t index = 0; index < pairs; index++)
	{
		int32_t key_size = read_i32_le(cursor);
		int32_t value_size;

		cursor += 4;
		if (key_size < 0)
			return false;
		if (strlen(key) == (size_t) key_size &&
			memcmp(cursor, key, (size_t) key_size) == 0)
		{
			cursor += key_size;
			value_size = read_i32_le(cursor);
			cursor += 4;
			if (value_size < 0 || (size_t) value_size >= capacity)
				return false;
			memcpy(value, cursor, (size_t) value_size);
			value[value_size] = '\0';
			return true;
		}
		cursor += key_size;
		value_size = read_i32_le(cursor);
		cursor += 4;
		if (value_size < 0)
			return false;
		cursor += value_size;
	}
	return false;
}


static int64_t
fixed_i64(const struct ArrowArray *array, size_t row)
{
	int64_t value;

	memcpy(&value, (const unsigned char *) array->buffers[1] + row * 8, 8);
	return value;
}


static int32_t
fixed_i32(const struct ArrowArray *array, size_t row)
{
	int32_t value;

	memcpy(&value, (const unsigned char *) array->buffers[1] + row * 4, 4);
	return value;
}


static int16_t
fixed_i16(const struct ArrowArray *array, size_t row)
{
	int16_t value;

	memcpy(&value, (const unsigned char *) array->buffers[1] + row * 2, 2);
	return value;
}


static float
fixed_float(const struct ArrowArray *array, size_t row)
{
	float value;

	memcpy(&value, (const unsigned char *) array->buffers[1] + row * 4, 4);
	return value;
}


static double
fixed_double(const struct ArrowArray *array, size_t row)
{
	double value;

	memcpy(&value, (const unsigned char *) array->buffers[1] + row * 8, 8);
	return value;
}


static bool
variable_equals(
	const struct ArrowArray *array, size_t row,
	const void *expected, size_t expected_size)
{
	const int32_t *offsets = array->buffers[1];
	const unsigned char *data = array->buffers[2];
	int32_t begin = offsets[row];
	int32_t end = offsets[row + 1];

	return begin >= 0 && end >= begin && (size_t) (end - begin) == expected_size &&
		memcmp(data + begin, expected, expected_size) == 0;
}
