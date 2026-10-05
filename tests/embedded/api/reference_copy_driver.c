#define _POSIX_C_SOURCE 200809L

#include "libpq-fe.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define LARGE_PAYLOAD_SIZE (256U * 1024U)


static bool execute_command(PGconn *connection, const char *sql);
static bool make_copy_data(unsigned char **data, size_t *size);
static uint64_t fnv1a64(const void *data, size_t size);
static void receive_notice(void *argument, const PGresult *result);


int
main(int argument_count, char **arguments)
{
	const char *keywords[] = {"host", "port", "user", "dbname", NULL};
	const char *values[5];
	PGconn	   *connection = NULL;
	PGresult   *result = NULL;
	unsigned char *input = NULL;
	size_t		input_size = 0;
	uint64_t	input_hash;
	uint64_t	output_hash = UINT64_C(14695981039346656037);
	size_t		output_size = 0;
	unsigned int notice_count = 0;
	bool		passed = false;

	if (argument_count != 3)
	{
		fprintf(stderr, "usage: %s SOCKET_DIRECTORY PORT\n", arguments[0]);
		return 2;
	}
	if (!make_copy_data(&input, &input_size))
		goto fail;
	input_hash = fnv1a64(input, input_size);
	values[0] = arguments[1];
	values[1] = arguments[2];
	values[2] = "postgamma";
	values[3] = "postgres";
	values[4] = NULL;
	connection = PQconnectdbParams(keywords, values, 0);
	if (connection == NULL || PQstatus(connection) != CONNECTION_OK)
		goto fail;
	(void) PQsetNoticeReceiver(connection, receive_notice, &notice_count);
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
			"FOR EACH ROW EXECUTE FUNCTION copy_notice()"))
		goto fail;

	result = PQexec(
		connection, "COPY copy_fixture FROM STDIN WITH (FORMAT csv)");
	if (result == NULL || PQresultStatus(result) != PGRES_COPY_IN)
		goto fail;
	PQclear(result);
	result = NULL;
	printf(
		"POSTGAMMA_COPY_CASE name=copy_in sequence=0 kind=copy_in "
		"bytes=%zu hash=%016" PRIx64 "\n",
		input_size, input_hash);
	if (PQputCopyData(connection, (const char *) input, (int) input_size) != 1 ||
		PQputCopyEnd(connection, NULL) != 1)
		goto fail;
	result = PQgetResult(connection);
	if (result == NULL || PQresultStatus(result) != PGRES_COMMAND_OK ||
		strcmp(PQcmdStatus(result), "COPY 3") != 0)
		goto fail;
	printf(
		"POSTGAMMA_COPY_CASE name=copy_in sequence=1 kind=command "
		"bytes=0 hash=0000000000000000\n");
	PQclear(result);
	result = NULL;
	if (PQgetResult(connection) != NULL || notice_count != 3)
		goto fail;

	result = PQexec(
		connection,
		"COPY (SELECT id, payload FROM copy_fixture ORDER BY id) "
		"TO STDOUT WITH (FORMAT csv)");
	if (result == NULL || PQresultStatus(result) != PGRES_COPY_OUT)
		goto fail;
	PQclear(result);
	result = NULL;
	for (;;)
	{
		char	   *data = NULL;
		int			length = PQgetCopyData(connection, &data, 0);

		if (length == -1)
			break;
		if (length <= 0 || data == NULL)
			goto fail;
		for (int index = 0; index < length; index++)
		{
			output_hash ^= (unsigned char) data[index];
			output_hash *= UINT64_C(1099511628211);
		}
		output_size += (size_t) length;
		PQfreemem(data);
	}
	printf(
		"POSTGAMMA_COPY_CASE name=copy_out sequence=0 kind=copy_out "
		"bytes=%zu hash=%016" PRIx64 "\n",
		output_size, output_hash);
	result = PQgetResult(connection);
	if (result == NULL || PQresultStatus(result) != PGRES_COMMAND_OK ||
		strcmp(PQcmdStatus(result), "COPY 3") != 0)
		goto fail;
	printf(
		"POSTGAMMA_COPY_CASE name=copy_out sequence=1 kind=command "
		"bytes=0 hash=0000000000000000\n");
	PQclear(result);
	result = NULL;
	if (PQgetResult(connection) != NULL || output_size != input_size ||
		output_hash != input_hash)
		goto fail;
	result = PQexec(connection, "SELECT 42");
	if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK ||
		PQntuples(result) != 1 || strcmp(PQgetvalue(result, 0, 0), "42") != 0)
		goto fail;
	PQclear(result);
	result = NULL;
	passed = true;

fail:
	if (!passed)
		fprintf(stderr, "reference COPY driver failed: %s",
			connection != NULL ? PQerrorMessage(connection) : "no connection\n");
	PQclear(result);
	if (connection != NULL)
		PQfinish(connection);
	free(input);
	if (!passed)
		return 1;
	printf(
		"POSTGAMMA_COPY_REFERENCE postgres=19 copy_in=true copy_out=true "
		"payload_bytes=%zu payload_hash=%016" PRIx64 " notices=3 "
		"byte_exact=true connection_reuse=true phase=closed\n",
		input_size, input_hash);
	return 0;
}


static bool
execute_command(PGconn *connection, const char *sql)
{
	PGresult   *result = PQexec(connection, sql);
	bool		ok = result != NULL && PQresultStatus(result) == PGRES_COMMAND_OK;

	PQclear(result);
	return ok;
}


static bool
make_copy_data(unsigned char **data, size_t *size)
{
	static const char prefix[] = "1,alpha\n2,";
	static const char suffix[] = "\n3,omega\n";
	size_t total = sizeof(prefix) - 1 + LARGE_PAYLOAD_SIZE + sizeof(suffix) - 1;
	unsigned char *created = malloc(total);

	if (created == NULL)
		return false;
	memcpy(created, prefix, sizeof(prefix) - 1);
	memset(created + sizeof(prefix) - 1, 'x', LARGE_PAYLOAD_SIZE);
	memcpy(created + sizeof(prefix) - 1 + LARGE_PAYLOAD_SIZE,
		   suffix, sizeof(suffix) - 1);
	*data = created;
	*size = total;
	return true;
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
receive_notice(void *argument, const PGresult *result)
{
	unsigned int *count = argument;

	(void) result;
	(*count)++;
}
