/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "postgamma/postgamma_arrow.h"

#include "postgamma/private/public_runtime.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* PostgreSQL built-in type OIDs used by the versioned Arrow mapping. */
#define PGM_PG_BOOL_OID UINT32_C(16)
#define PGM_PG_BYTEA_OID UINT32_C(17)
#define PGM_PG_INT8_OID UINT32_C(20)
#define PGM_PG_INT2_OID UINT32_C(21)
#define PGM_PG_INT4_OID UINT32_C(23)
#define PGM_PG_TEXT_OID UINT32_C(25)
#define PGM_PG_OID_OID UINT32_C(26)
#define PGM_PG_FLOAT4_OID UINT32_C(700)
#define PGM_PG_FLOAT8_OID UINT32_C(701)
#define PGM_PG_BPCHAR_OID UINT32_C(1042)
#define PGM_PG_VARCHAR_OID UINT32_C(1043)
#define PGM_PG_DATE_OID UINT32_C(1082)
#define PGM_PG_TIMESTAMP_OID UINT32_C(1114)
#define PGM_PG_TIMESTAMPTZ_OID UINT32_C(1184)
#define PGM_PG_UUID_OID UINT32_C(2950)

#define PGM_PG_EPOCH_DAYS INT32_C(10957)
#define PGM_PG_EPOCH_MICROSECONDS INT64_C(946684800000000)

#define PGM_ARROW_FLAG_NULLABLE INT64_C(2)


/*
 * The optional public header deliberately forward-declares these records.
 * Their definitions below are the stable Arrow C Data Interface ABI, not an
 * Arrow runtime dependency.
 */
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


typedef enum PostgammaArrowKind
{
	POSTGAMMA_ARROW_BOOL,
	POSTGAMMA_ARROW_INT16,
	POSTGAMMA_ARROW_INT32,
	POSTGAMMA_ARROW_UINT32,
	POSTGAMMA_ARROW_INT64,
	POSTGAMMA_ARROW_FLOAT32,
	POSTGAMMA_ARROW_FLOAT64,
	POSTGAMMA_ARROW_DATE32,
	POSTGAMMA_ARROW_TIMESTAMP,
	POSTGAMMA_ARROW_UUID,
	POSTGAMMA_ARROW_UTF8,
	POSTGAMMA_ARROW_BINARY
} PostgammaArrowKind;

typedef struct PostgammaArrowType
{
	PostgammaArrowKind kind;
	const char *format;
	size_t		width;
	bool		variable;
} PostgammaArrowType;


static void release_schema(struct ArrowSchema *schema);
static void release_array(struct ArrowArray *array);
static char *duplicate_string(const char *source);
static void write_i32_le(unsigned char *target, int32_t value);
static char *build_metadata(const PostgammaPrivateResultField *field);
static PostgammaArrowType map_type(
	const PostgammaPrivateResultField *field);
static bool checked_multiply(size_t left, size_t right, size_t *product);
static bool value_bytes(
	const PostgammaPrivateResultField *field,
	const PostgammaPrivateResultValue *value,
	PostgammaArrowKind kind,
	const unsigned char **bytes, size_t *length,
	unsigned char uuid_storage[16], unsigned char **allocated);
static bool convert_fixed_value(
	const PostgammaPrivateResultField *field,
	const PostgammaPrivateResultValue *value,
	PostgammaArrowKind kind, unsigned char *target);
static bool parse_bool(const unsigned char *data, size_t length, bool *value);
static bool parse_signed(
	const unsigned char *data, size_t length,
	int64_t minimum, int64_t maximum, int64_t *value);
static bool parse_unsigned(
	const unsigned char *data, size_t length,
	uint64_t maximum, uint64_t *value);
static bool parse_double(
	const unsigned char *data, size_t length, double *value);
static uint16_t read_u16_be(const unsigned char *data);
static uint32_t read_u32_be(const unsigned char *data);
static uint64_t read_u64_be(const unsigned char *data);
static bool parse_date_text(
	const unsigned char *data, size_t length, int32_t *days);
static bool parse_timestamp_text(
	const unsigned char *data, size_t length, int64_t *microseconds);
static int64_t civil_days(int year, unsigned int month, unsigned int day);
static bool parse_uuid_text(
	const unsigned char *data, size_t length, unsigned char output[16]);
static int hex_value(unsigned char value);
static bool initialize_column(
	const PostgammaPrivateOwnedResult *result, size_t column_index,
	struct ArrowSchema *schema, struct ArrowArray *array);
static bool initialize_variable_column(
	const PostgammaPrivateOwnedResult *result, size_t column_index,
	PostgammaArrowKind kind, struct ArrowArray *array);
static bool initialize_fixed_column(
	const PostgammaPrivateOwnedResult *result, size_t column_index,
	const PostgammaArrowType *type, struct ArrowArray *array);
static void set_valid(unsigned char *bitmap, size_t index);


pgm_status
pgm_result_export_arrow(
	const pgm_result *result,
	struct ArrowSchema *schema,
	struct ArrowArray *array,
	pgm_error **error)
{
	const PostgammaPrivateOwnedResult *source;
	size_t		column_count;
	size_t		row_count;
	size_t		index;

	if (error != NULL)
		*error = NULL;
	if (!result_is_valid(result) || schema == NULL || array == NULL)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"invalid Arrow export arguments");
	if (result->lease != NULL && result->lease->owner != NULL &&
		public_instance_reentrant(
			result->lease->owner->connection->instance))
		return return_simple_error(
			error, PGM_STATUS_REENTRANT_CALL,
			"callback cannot reenter the same instance");
	memset(schema, 0, sizeof(*schema));
	memset(array, 0, sizeof(*array));
	source = result->private_result;
	if (source->status != POSTGAMMA_PRIVATE_RESULT_TUPLES_OK &&
		source->status != POSTGAMMA_PRIVATE_RESULT_TUPLES_CHUNK)
		return return_simple_error(
			error, PGM_STATUS_INVALID_ARGUMENT,
			"Arrow export requires a tuple result or tuple chunk");
	column_count = source->columns;
	row_count = source->rows;
	if (column_count > INT64_MAX || row_count > INT64_MAX ||
		column_count > SIZE_MAX / sizeof(*schema->children) ||
		column_count > SIZE_MAX / sizeof(*array->children))
		return return_simple_error(
			error, PGM_STATUS_OUT_OF_MEMORY,
			"Arrow result dimensions exceed host limits");

	schema->format = duplicate_string("+s");
	schema->name = duplicate_string("");
	schema->flags = 0;
	schema->n_children = (int64_t) column_count;
	schema->release = release_schema;
	array->length = (int64_t) row_count;
	array->null_count = 0;
	array->offset = 0;
	array->n_buffers = 1;
	array->n_children = (int64_t) column_count;
	array->release = release_array;
	array->buffers = calloc(1, sizeof(*array->buffers));
	if (column_count != 0)
	{
		schema->children = calloc(column_count, sizeof(*schema->children));
		array->children = calloc(column_count, sizeof(*array->children));
	}
	if (schema->format == NULL || schema->name == NULL ||
		array->buffers == NULL ||
		(column_count != 0 &&
		 (schema->children == NULL || array->children == NULL)))
		goto no_memory;

	for (index = 0; index < column_count; index++)
	{
		schema->children[index] = calloc(1, sizeof(*schema->children[index]));
		array->children[index] = calloc(1, sizeof(*array->children[index]));
		if (schema->children[index] == NULL ||
			array->children[index] == NULL ||
			!initialize_column(
				source, index, schema->children[index],
				array->children[index]))
			goto conversion_failed;
	}
	return PGM_STATUS_OK;

no_memory:
	release_schema(schema);
	release_array(array);
	return return_simple_error(
		error, PGM_STATUS_OUT_OF_MEMORY,
		"could not allocate Arrow result storage");

conversion_failed:
	release_schema(schema);
	release_array(array);
	return return_simple_error(
		error, errno == ENOMEM ? PGM_STATUS_OUT_OF_MEMORY :
			PGM_STATUS_POSTGRES_ERROR,
		"could not convert PostgreSQL value to its Arrow representation");
}


static void
release_schema(struct ArrowSchema *schema)
{
	int64_t		index;

	if (schema == NULL || schema->release == NULL)
		return;
	for (index = 0; index < schema->n_children; index++)
	{
		if (schema->children != NULL && schema->children[index] != NULL)
		{
			if (schema->children[index]->release != NULL)
				schema->children[index]->release(schema->children[index]);
			free(schema->children[index]);
		}
	}
	if (schema->dictionary != NULL)
	{
		if (schema->dictionary->release != NULL)
			schema->dictionary->release(schema->dictionary);
		free(schema->dictionary);
	}
	free(schema->children);
	free((void *) schema->format);
	free((void *) schema->name);
	free((void *) schema->metadata);
	memset(schema, 0, sizeof(*schema));
}


static void
release_array(struct ArrowArray *array)
{
	int64_t		index;

	if (array == NULL || array->release == NULL)
		return;
	for (index = 0; index < array->n_children; index++)
	{
		if (array->children != NULL && array->children[index] != NULL)
		{
			if (array->children[index]->release != NULL)
				array->children[index]->release(array->children[index]);
			free(array->children[index]);
		}
	}
	if (array->dictionary != NULL)
	{
		if (array->dictionary->release != NULL)
			array->dictionary->release(array->dictionary);
		free(array->dictionary);
	}
	for (index = 0; index < array->n_buffers; index++)
		free((void *) (array->buffers != NULL ? array->buffers[index] : NULL));
	free(array->buffers);
	free(array->children);
	memset(array, 0, sizeof(*array));
}


static char *
duplicate_string(const char *source)
{
	size_t		length;
	char	   *copy;

	if (source == NULL)
		return NULL;
	length = strlen(source);
	if (length == SIZE_MAX)
		return NULL;
	copy = malloc(length + 1);
	if (copy != NULL)
		memcpy(copy, source, length + 1);
	return copy;
}


static void
write_i32_le(unsigned char *target, int32_t value)
{
	uint32_t	bits = (uint32_t) value;

	target[0] = (unsigned char) bits;
	target[1] = (unsigned char) (bits >> 8);
	target[2] = (unsigned char) (bits >> 16);
	target[3] = (unsigned char) (bits >> 24);
}


static char *
build_metadata(const PostgammaPrivateResultField *field)
{
	static const char *const keys[] = {
		"postgamma.postgresql.type_oid",
		"postgamma.postgresql.format",
		"postgamma.postgresql.type_modifier",
	};
	char		oid[32];
	char		modifier[32];
	const char *values[3];
	size_t		total = 4;
	size_t		offset = 0;
	unsigned char *metadata;

	(void) snprintf(oid, sizeof(oid), "%u", field->type_oid);
	(void) snprintf(modifier, sizeof(modifier), "%d", field->type_modifier);
	values[0] = oid;
	values[1] = field->format == PGM_FORMAT_BINARY ? "binary" : "text";
	values[2] = modifier;
	for (size_t index = 0; index < 3; index++)
	{
		size_t key_length = strlen(keys[index]);
		size_t value_length = strlen(values[index]);

		if (key_length > INT32_MAX || value_length > INT32_MAX ||
			total > SIZE_MAX - 8 - key_length - value_length)
			return NULL;
		total += 8 + key_length + value_length;
	}
	metadata = calloc(total + 1, 1);
	if (metadata == NULL)
		return NULL;
	write_i32_le(metadata, INT32_C(3));
	offset = 4;
	for (size_t index = 0; index < 3; index++)
	{
		int32_t key_length = (int32_t) strlen(keys[index]);
		int32_t value_length = (int32_t) strlen(values[index]);

		write_i32_le(metadata + offset, key_length);
		offset += 4;
		memcpy(metadata + offset, keys[index], (size_t) key_length);
		offset += (size_t) key_length;
		write_i32_le(metadata + offset, value_length);
		offset += 4;
		memcpy(metadata + offset, values[index], (size_t) value_length);
		offset += (size_t) value_length;
	}
	return (char *) metadata;
}


static PostgammaArrowType
map_type(const PostgammaPrivateResultField *field)
{
	switch (field->type_oid)
	{
		case PGM_PG_BOOL_OID:
			return (PostgammaArrowType) {POSTGAMMA_ARROW_BOOL, "b", 1, false};
		case PGM_PG_INT2_OID:
			return (PostgammaArrowType) {POSTGAMMA_ARROW_INT16, "s", 2, false};
		case PGM_PG_INT4_OID:
			return (PostgammaArrowType) {POSTGAMMA_ARROW_INT32, "i", 4, false};
		case PGM_PG_OID_OID:
			return (PostgammaArrowType) {POSTGAMMA_ARROW_UINT32, "I", 4, false};
		case PGM_PG_INT8_OID:
			return (PostgammaArrowType) {POSTGAMMA_ARROW_INT64, "l", 8, false};
		case PGM_PG_FLOAT4_OID:
			return (PostgammaArrowType) {POSTGAMMA_ARROW_FLOAT32, "f", 4, false};
		case PGM_PG_FLOAT8_OID:
			return (PostgammaArrowType) {POSTGAMMA_ARROW_FLOAT64, "g", 8, false};
		case PGM_PG_DATE_OID:
			return (PostgammaArrowType) {POSTGAMMA_ARROW_DATE32, "tdD", 4, false};
		case PGM_PG_TIMESTAMP_OID:
			return (PostgammaArrowType) {
				POSTGAMMA_ARROW_TIMESTAMP, "tsu:", 8, false};
		case PGM_PG_TIMESTAMPTZ_OID:
			return (PostgammaArrowType) {
				POSTGAMMA_ARROW_TIMESTAMP, "tsu:UTC", 8, false};
		case PGM_PG_UUID_OID:
			return (PostgammaArrowType) {
				POSTGAMMA_ARROW_UUID, "w:16", 16, false};
		case PGM_PG_BYTEA_OID:
			return (PostgammaArrowType) {POSTGAMMA_ARROW_BINARY, "z", 0, true};
		case PGM_PG_TEXT_OID:
		case PGM_PG_BPCHAR_OID:
		case PGM_PG_VARCHAR_OID:
			return (PostgammaArrowType) {POSTGAMMA_ARROW_UTF8, "u", 0, true};
		default:
			return field->format == PGM_FORMAT_BINARY ?
				(PostgammaArrowType) {POSTGAMMA_ARROW_BINARY, "z", 0, true} :
				(PostgammaArrowType) {POSTGAMMA_ARROW_UTF8, "u", 0, true};
	}
}


static bool
checked_multiply(size_t left, size_t right, size_t *product)
{
	if (product == NULL || (left != 0 && right > SIZE_MAX / left))
		return false;
	*product = left * right;
	return true;
}


static bool
value_bytes(
	const PostgammaPrivateResultField *field,
	const PostgammaPrivateResultValue *value,
	PostgammaArrowKind kind,
	const unsigned char **bytes, size_t *length,
	unsigned char uuid_storage[16], unsigned char **allocated)
{
	*bytes = value->data;
	*length = value->length;
	*allocated = NULL;
	if (kind == POSTGAMMA_ARROW_UUID)
	{
		if (field->format == PGM_FORMAT_BINARY)
		{
			if (value->length != 16)
				return false;
			return true;
		}
		if (!parse_uuid_text(value->data, value->length, uuid_storage))
			return false;
		*bytes = uuid_storage;
		*length = 16;
		return true;
	}
	if (field->type_oid == PGM_PG_BYTEA_OID &&
		field->format == PGM_FORMAT_TEXT)
	{
		size_t output_length;
		unsigned char *output;

		if (value->length < 2 || value->data[0] != '\\' ||
			value->data[1] != 'x' || (value->length - 2) % 2 != 0)
			return false;
		output_length = (value->length - 2) / 2;
		output = malloc(output_length == 0 ? 1 : output_length);
		if (output == NULL)
		{
			errno = ENOMEM;
			return false;
		}
		for (size_t index = 0; index < output_length; index++)
		{
			int high = hex_value(value->data[2 + index * 2]);
			int low = hex_value(value->data[3 + index * 2]);

			if (high < 0 || low < 0)
			{
				free(output);
				return false;
			}
			output[index] = (unsigned char) ((high << 4) | low);
		}
		*bytes = output;
		*length = output_length;
		*allocated = output;
	}
	return true;
}


static bool
convert_fixed_value(
	const PostgammaPrivateResultField *field,
	const PostgammaPrivateResultValue *value,
	PostgammaArrowKind kind, unsigned char *target)
{
	int64_t		signed_value = 0;
	uint64_t	unsigned_value = 0;
	double		double_value = 0;

	if (field->format == PGM_FORMAT_BINARY)
	{
		switch (kind)
		{
			case POSTGAMMA_ARROW_BOOL:
				if (value->length != 1 || value->data[0] > 1)
					return false;
				target[0] = value->data[0];
				return true;
			case POSTGAMMA_ARROW_INT16:
				if (value->length != 2)
					return false;
				{
					int16_t converted = (int16_t) read_u16_be(value->data);
					memcpy(target, &converted, sizeof(converted));
				}
				return true;
			case POSTGAMMA_ARROW_INT32:
			case POSTGAMMA_ARROW_UINT32:
			case POSTGAMMA_ARROW_DATE32:
				if (value->length != 4)
					return false;
				{
					uint32_t converted = read_u32_be(value->data);

					if (kind == POSTGAMMA_ARROW_DATE32)
					{
						int32_t date = (int32_t) converted;

						if (date > INT32_MAX - PGM_PG_EPOCH_DAYS)
							return false;
						date += PGM_PG_EPOCH_DAYS;
						memcpy(target, &date, sizeof(date));
					}
					else
						memcpy(target, &converted, sizeof(converted));
				}
				return true;
			case POSTGAMMA_ARROW_INT64:
			case POSTGAMMA_ARROW_TIMESTAMP:
				if (value->length != 8)
					return false;
				{
					int64_t converted = (int64_t) read_u64_be(value->data);

					if (kind == POSTGAMMA_ARROW_TIMESTAMP)
					{
						if (converted > INT64_MAX - PGM_PG_EPOCH_MICROSECONDS)
							return false;
						converted += PGM_PG_EPOCH_MICROSECONDS;
					}
					memcpy(target, &converted, sizeof(converted));
				}
				return true;
			case POSTGAMMA_ARROW_FLOAT32:
				if (value->length != 4)
					return false;
				{
					uint32_t bits = read_u32_be(value->data);
					memcpy(target, &bits, sizeof(bits));
				}
				return true;
			case POSTGAMMA_ARROW_FLOAT64:
				if (value->length != 8)
					return false;
				{
					uint64_t bits = read_u64_be(value->data);
					memcpy(target, &bits, sizeof(bits));
				}
				return true;
			case POSTGAMMA_ARROW_UUID:
				if (value->length != 16)
					return false;
				memcpy(target, value->data, 16);
				return true;
			case POSTGAMMA_ARROW_UTF8:
			case POSTGAMMA_ARROW_BINARY:
				return false;
		}
	}

	switch (kind)
	{
		case POSTGAMMA_ARROW_BOOL:
		{
			bool converted;

			if (!parse_bool(value->data, value->length, &converted))
				return false;
			target[0] = converted ? 1 : 0;
			return true;
		}
		case POSTGAMMA_ARROW_INT16:
			if (!parse_signed(
					value->data, value->length, INT16_MIN, INT16_MAX,
					&signed_value))
				return false;
			{
				int16_t converted = (int16_t) signed_value;
				memcpy(target, &converted, sizeof(converted));
			}
			return true;
		case POSTGAMMA_ARROW_INT32:
			if (!parse_signed(
					value->data, value->length, INT32_MIN, INT32_MAX,
					&signed_value))
				return false;
			{
				int32_t converted = (int32_t) signed_value;
				memcpy(target, &converted, sizeof(converted));
			}
			return true;
		case POSTGAMMA_ARROW_UINT32:
			if (!parse_unsigned(
					value->data, value->length, UINT32_MAX, &unsigned_value))
				return false;
			{
				uint32_t converted = (uint32_t) unsigned_value;
				memcpy(target, &converted, sizeof(converted));
			}
			return true;
		case POSTGAMMA_ARROW_INT64:
			if (!parse_signed(
					value->data, value->length, INT64_MIN, INT64_MAX,
					&signed_value))
				return false;
			memcpy(target, &signed_value, sizeof(signed_value));
			return true;
		case POSTGAMMA_ARROW_FLOAT32:
		case POSTGAMMA_ARROW_FLOAT64:
			if (!parse_double(value->data, value->length, &double_value))
				return false;
			if (kind == POSTGAMMA_ARROW_FLOAT32)
			{
				float converted = (float) double_value;

				memcpy(target, &converted, sizeof(converted));
			}
			else
				memcpy(target, &double_value, sizeof(double_value));
			return true;
		case POSTGAMMA_ARROW_DATE32:
		{
			int32_t days;

			if (!parse_date_text(value->data, value->length, &days))
				return false;
			memcpy(target, &days, sizeof(days));
			return true;
		}
		case POSTGAMMA_ARROW_TIMESTAMP:
		{
			int64_t microseconds;

			if (!parse_timestamp_text(
					value->data, value->length, &microseconds))
				return false;
			memcpy(target, &microseconds, sizeof(microseconds));
			return true;
		}
		case POSTGAMMA_ARROW_UUID:
			return parse_uuid_text(
				value->data, value->length, target);
		case POSTGAMMA_ARROW_UTF8:
		case POSTGAMMA_ARROW_BINARY:
			return false;
	}
	return false;
}


static bool
parse_bool(const unsigned char *data, size_t length, bool *value)
{
	if (length == 1 && (data[0] == 't' || data[0] == '1'))
	{
		*value = true;
		return true;
	}
	if (length == 1 && (data[0] == 'f' || data[0] == '0'))
	{
		*value = false;
		return true;
	}
	if (length == 4 && memcmp(data, "true", 4) == 0)
	{
		*value = true;
		return true;
	}
	if (length == 5 && memcmp(data, "false", 5) == 0)
	{
		*value = false;
		return true;
	}
	return false;
}


static bool
parse_signed(
	const unsigned char *data, size_t length,
	int64_t minimum, int64_t maximum, int64_t *value)
{
	char		buffer[96];
	char	   *end = NULL;
	long long	parsed;

	if (length == 0 || length >= sizeof(buffer))
		return false;
	memcpy(buffer, data, length);
	buffer[length] = '\0';
	errno = 0;
	parsed = strtoll(buffer, &end, 10);
	if (errno != 0 || end != buffer + length ||
		parsed < minimum || parsed > maximum)
		return false;
	*value = (int64_t) parsed;
	return true;
}


static bool
parse_unsigned(
	const unsigned char *data, size_t length,
	uint64_t maximum, uint64_t *value)
{
	char		buffer[96];
	char	   *end = NULL;
	unsigned long long parsed;

	if (length == 0 || length >= sizeof(buffer) || data[0] == '-')
		return false;
	memcpy(buffer, data, length);
	buffer[length] = '\0';
	errno = 0;
	parsed = strtoull(buffer, &end, 10);
	if (errno != 0 || end != buffer + length || parsed > maximum)
		return false;
	*value = (uint64_t) parsed;
	return true;
}


static bool
parse_double(const unsigned char *data, size_t length, double *value)
{
	char		buffer[128];
	char	   *end = NULL;
	double		parsed;

	if (length == 0 || length >= sizeof(buffer))
		return false;
	memcpy(buffer, data, length);
	buffer[length] = '\0';
	errno = 0;
	parsed = strtod(buffer, &end);
	if (errno != 0 || end != buffer + length)
		return false;
	*value = parsed;
	return true;
}


static uint16_t
read_u16_be(const unsigned char *data)
{
	return (uint16_t) (((uint16_t) data[0] << 8) | data[1]);
}


static uint32_t
read_u32_be(const unsigned char *data)
{
	return ((uint32_t) data[0] << 24) | ((uint32_t) data[1] << 16) |
		((uint32_t) data[2] << 8) | (uint32_t) data[3];
}


static uint64_t
read_u64_be(const unsigned char *data)
{
	return ((uint64_t) read_u32_be(data) << 32) |
		(uint64_t) read_u32_be(data + 4);
}


static bool
parse_date_text(const unsigned char *data, size_t length, int32_t *days)
{
	char buffer[11];
	int year;
	unsigned int month;
	unsigned int day;
	int64_t converted;

	if (length != 10 || data[4] != '-' || data[7] != '-')
		return false;
	memcpy(buffer, data, length);
	buffer[length] = '\0';
	if (sscanf(buffer, "%4d-%2u-%2u", &year, &month, &day) != 3 ||
		month < 1 || month > 12 || day < 1 || day > 31)
		return false;
	converted = civil_days(year, month, day);
	if (converted < INT32_MIN || converted > INT32_MAX)
		return false;
	*days = (int32_t) converted;
	return true;
}


static bool
parse_timestamp_text(
	const unsigned char *data, size_t length, int64_t *microseconds)
{
	char		buffer[128];
	int		 year;
	unsigned int month;
	unsigned int day;
	unsigned int hour;
	unsigned int minute;
	unsigned int second;
	const char *cursor;
	int64_t		fraction = 0;
	unsigned int fraction_digits = 0;
	int		 timezone_sign = 0;
	unsigned int timezone_hour = 0;
	unsigned int timezone_minute = 0;
	int64_t		seconds;

	if (length < 19 || length >= sizeof(buffer))
		return false;
	memcpy(buffer, data, length);
	buffer[length] = '\0';
	if (sscanf(
			buffer, "%4d-%2u-%2u %2u:%2u:%2u",
			&year, &month, &day, &hour, &minute, &second) != 6 ||
		hour > 23 || minute > 59 || second > 60)
		return false;
	cursor = buffer + 19;
	if (*cursor == '.')
	{
		cursor++;
		while (*cursor >= '0' && *cursor <= '9')
		{
			if (fraction_digits < 6)
				fraction = fraction * 10 + (*cursor - '0');
			fraction_digits++;
			cursor++;
		}
		if (fraction_digits == 0)
			return false;
		while (fraction_digits < 6)
		{
			fraction *= 10;
			fraction_digits++;
		}
	}
	if (*cursor == '+' || *cursor == '-')
	{
		timezone_sign = *cursor == '+' ? 1 : -1;
		cursor++;
		if (cursor[0] < '0' || cursor[0] > '9' ||
			cursor[1] < '0' || cursor[1] > '9')
			return false;
		timezone_hour = (unsigned int) (cursor[0] - '0') * 10U +
			(unsigned int) (cursor[1] - '0');
		cursor += 2;
		if (*cursor == ':')
		{
			cursor++;
			if (cursor[0] < '0' || cursor[0] > '9' ||
				cursor[1] < '0' || cursor[1] > '9')
				return false;
			timezone_minute = (unsigned int) (cursor[0] - '0') * 10U +
				(unsigned int) (cursor[1] - '0');
			cursor += 2;
		}
		if (timezone_hour > 23 || timezone_minute > 59)
			return false;
	}
	if (*cursor != '\0')
		return false;
	seconds = civil_days(year, month, day) * INT64_C(86400) +
		(int64_t) hour * 3600 + (int64_t) minute * 60 + second;
	seconds -= (int64_t) timezone_sign *
		((int64_t) timezone_hour * 3600 + (int64_t) timezone_minute * 60);
	if (seconds > INT64_MAX / INT64_C(1000000) ||
		seconds < INT64_MIN / INT64_C(1000000))
		return false;
	*microseconds = seconds * INT64_C(1000000) + fraction;
	return true;
}


/* Howard Hinnant's civil-date conversion, with 1970-01-01 as day zero. */
static int64_t
civil_days(int year, unsigned int month, unsigned int day)
{
	int		adjusted_year = year - (month <= 2 ? 1 : 0);
	int		era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
	unsigned int year_of_era =
		(unsigned int) (adjusted_year - era * 400);
	unsigned int adjusted_month = month > 2 ? month - 3 : month + 9;
	unsigned int day_of_year =
		(153 * adjusted_month + 2) / 5 +
		day - 1;
	unsigned int day_of_era =
		year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

	return (int64_t) era * 146097 + day_of_era - 719468;
}


static bool
parse_uuid_text(
	const unsigned char *data, size_t length, unsigned char output[16])
{
	size_t		input = 0;
	size_t		written = 0;

	if (length != 36)
		return false;
	while (input < length)
	{
		int high;
		int low;

		if (input == 8 || input == 13 || input == 18 || input == 23)
		{
			if (data[input++] != '-')
				return false;
			continue;
		}
		if (input + 1 >= length || written >= 16)
			return false;
		high = hex_value(data[input]);
		low = hex_value(data[input + 1]);
		if (high < 0 || low < 0)
			return false;
		output[written++] = (unsigned char) ((high << 4) | low);
		input += 2;
	}
	return written == 16;
}


static int
hex_value(unsigned char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}


static bool
initialize_column(
	const PostgammaPrivateOwnedResult *result, size_t column_index,
	struct ArrowSchema *schema, struct ArrowArray *array)
{
	const PostgammaPrivateResultField *field = &result->fields[column_index];
	PostgammaArrowType type = map_type(field);

	schema->format = duplicate_string(type.format);
	schema->name = duplicate_string(field->name != NULL ? field->name : "");
	schema->metadata = build_metadata(field);
	schema->flags = PGM_ARROW_FLAG_NULLABLE;
	schema->n_children = 0;
	schema->release = release_schema;
	array->length = (int64_t) result->rows;
	array->offset = 0;
	array->n_children = 0;
	array->release = release_array;
	if (schema->format == NULL || schema->name == NULL ||
		schema->metadata == NULL)
	{
		errno = ENOMEM;
		return false;
	}
	return type.variable ?
		initialize_variable_column(result, column_index, type.kind, array) :
		initialize_fixed_column(result, column_index, &type, array);
}


static bool
initialize_variable_column(
	const PostgammaPrivateOwnedResult *result, size_t column_index,
	PostgammaArrowKind kind, struct ArrowArray *array)
{
	const PostgammaPrivateResultField *field = &result->fields[column_index];
	size_t		bitmap_size = (result->rows + 7) / 8;
	size_t		offset_count;
	size_t		offset_bytes;
	size_t		data_size = 0;
	unsigned char *bitmap;
	int32_t    *offsets;
	unsigned char *data;

	if (result->rows == SIZE_MAX ||
		!checked_multiply(result->rows + 1, sizeof(*offsets), &offset_bytes))
		return false;
	offset_count = result->rows + 1;
	(void) offset_count;
	for (size_t row = 0; row < result->rows; row++)
	{
		const PostgammaPrivateResultValue *value =
			&result->values[row * result->columns + column_index];
		const unsigned char *bytes;
		size_t length;
		unsigned char uuid_storage[16];
		unsigned char *allocated;

		if (value->is_null)
			continue;
		if (!value_bytes(
				field, value, kind, &bytes, &length, uuid_storage, &allocated))
			return false;
		free(allocated);
		if (length > INT32_MAX || data_size > (size_t) INT32_MAX - length)
			return false;
		data_size += length;
	}
	array->n_buffers = 3;
	array->buffers = calloc(3, sizeof(*array->buffers));
	bitmap = calloc(bitmap_size == 0 ? 1 : bitmap_size, 1);
	offsets = calloc(1, offset_bytes);
	data = malloc(data_size == 0 ? 1 : data_size);
	if (array->buffers == NULL || bitmap == NULL || offsets == NULL || data == NULL)
	{
		free(bitmap);
		free(offsets);
		free(data);
		errno = ENOMEM;
		return false;
	}
	array->buffers[0] = bitmap;
	array->buffers[1] = offsets;
	array->buffers[2] = data;
	data_size = 0;
	for (size_t row = 0; row < result->rows; row++)
	{
		const PostgammaPrivateResultValue *value =
			&result->values[row * result->columns + column_index];
		const unsigned char *bytes;
		size_t length;
		unsigned char uuid_storage[16];
		unsigned char *allocated;

		offsets[row] = (int32_t) data_size;
		if (value->is_null)
		{
			array->null_count++;
			continue;
		}
		if (!value_bytes(
				field, value, kind, &bytes, &length, uuid_storage, &allocated))
			return false;
		set_valid(bitmap, row);
		memcpy(data + data_size, bytes, length);
		data_size += length;
		free(allocated);
	}
	offsets[result->rows] = (int32_t) data_size;
	return true;
}


static bool
initialize_fixed_column(
	const PostgammaPrivateOwnedResult *result, size_t column_index,
	const PostgammaArrowType *type, struct ArrowArray *array)
{
	size_t		bitmap_size = (result->rows + 7) / 8;
	size_t		data_size;
	unsigned char *bitmap;
	unsigned char *data;

	if (type->kind == POSTGAMMA_ARROW_BOOL)
		data_size = bitmap_size;
	else if (!checked_multiply(result->rows, type->width, &data_size))
		return false;
	array->n_buffers = 2;
	array->buffers = calloc(2, sizeof(*array->buffers));
	bitmap = calloc(bitmap_size == 0 ? 1 : bitmap_size, 1);
	data = calloc(data_size == 0 ? 1 : data_size, 1);
	if (array->buffers == NULL || bitmap == NULL || data == NULL)
	{
		free(bitmap);
		free(data);
		errno = ENOMEM;
		return false;
	}
	array->buffers[0] = bitmap;
	array->buffers[1] = data;
	for (size_t row = 0; row < result->rows; row++)
	{
		const PostgammaPrivateResultValue *value =
			&result->values[row * result->columns + column_index];

		if (value->is_null)
		{
			array->null_count++;
			continue;
		}
		if (type->kind == POSTGAMMA_ARROW_BOOL)
		{
			unsigned char converted = 0;

			if (!convert_fixed_value(
					&result->fields[column_index], value, type->kind,
					&converted))
				return false;
			if (converted != 0)
				set_valid(data, row);
		}
		else if (!convert_fixed_value(
				&result->fields[column_index], value, type->kind,
				data + row * type->width))
			return false;
		set_valid(bitmap, row);
	}
	return true;
}


static void
set_valid(unsigned char *bitmap, size_t index)
{
	bitmap[index / 8] |= (unsigned char) (1U << (index % 8));
}
