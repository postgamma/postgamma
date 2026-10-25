/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <libpq-fe.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void
require(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "%s\n", message);
		exit(1);
	}
}

static void
result(PGconn *connection, PGresult *value, const char *sqlstate,
	const char *scalar)
{
	const char *error;
	ExecStatusType status;

	require(value != NULL, PQerrorMessage(connection));
	status = PQresultStatus(value);
	error = PQresultErrorField(value, PG_DIAG_SQLSTATE);
	if (sqlstate != NULL)
		require(status == PGRES_FATAL_ERROR && error != NULL &&
			strcmp(error, sqlstate) == 0, PQresultErrorMessage(value));
	else
		require(status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK,
			PQresultErrorMessage(value));
	if (scalar != NULL)
		require(PQntuples(value) == 1 && PQnfields(value) == 1 &&
			!PQgetisnull(value, 0, 0) && strcmp(PQgetvalue(value, 0, 0), scalar) == 0,
			"unexpected scalar result");
	PQclear(value);
}

static void
query(PGconn *connection, const char *sql, const char *state, const char *scalar)
{
	result(connection, PQexec(connection, sql), state, scalar);
}

static void
receive_notification(PGconn *connection, const char *expected)
{
	struct timespec start;
	PGnotify *notification;

	require(clock_gettime(CLOCK_MONOTONIC, &start) == 0, "clock_gettime failed");
	while ((notification = PQnotifies(connection)) == NULL)
	{
		struct timespec now;
		struct pollfd socket = {PQsocket(connection), POLLIN, 0};
		long elapsed;
		int ready;

		require(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "clock_gettime failed");
		elapsed = (now.tv_sec - start.tv_sec) * 1000 +
			(now.tv_nsec - start.tv_nsec) / 1000000;
		require(elapsed < 5000, "notification delivery timed out");
		ready = poll(&socket, 1, (int) (5000 - elapsed));
		if (ready < 0 && errno == EINTR)
			continue;
		require(ready > 0 && PQconsumeInput(connection), PQerrorMessage(connection));
	}
	require(strcmp(notification->relname, "chain_events") == 0 &&
		strcmp(notification->extra, expected) == 0,
		"unexpected or stale notification");
	PQfreemem(notification);
}

static void
cancel_sleep(PGconn *connection, PGconn *observer)
{
	char sql[256];
	char error[256];
	PGcancel *cancel;
	PGresult *value;
	struct timespec pause = {0, 1000000};
	int sleeping = 0;

	require(PQsendQuery(connection, "SELECT pg_sleep(60)") == 1,
		PQerrorMessage(connection));
	snprintf(sql, sizeof(sql), "SELECT wait_event = 'PgSleep' FROM pg_stat_activity WHERE pid=%d",
		PQbackendPID(connection));
	for (int i = 0; i < 5000 && !sleeping; i++)
	{
		value = PQexec(observer, sql);
		require(PQresultStatus(value) == PGRES_TUPLES_OK, PQerrorMessage(observer));
		sleeping = PQntuples(value) == 1 && strcmp(PQgetvalue(value, 0, 0), "t") == 0;
		PQclear(value);
		if (!sleeping)
			nanosleep(&pause, NULL);
	}
	require(sleeping, "query did not enter its wait");
	cancel = PQgetCancel(connection);
	require(cancel != NULL, "could not allocate cancel request");
	require(PQcancel(cancel, error, sizeof(error)) == 1, error);
	PQfreeCancel(cancel);
	result(connection, PQgetResult(connection), "57014", NULL);
	require(PQgetResult(connection) == NULL, "cancel left an extra result");
	query(connection, "SELECT 42", NULL, "42");
}

int
main(int argc, char **argv)
{
	PGconn *first;
	PGconn *second;
	int first_pid;
	int second_pid;
	int iterations;

	require(argc == 3, "usage: transaction-chain CONNECTION_STRING ITERATIONS");
	iterations = atoi(argv[2]);
	require(iterations > 0 && iterations <= 1000, "invalid iteration count");
	first = PQconnectdb(argv[1]);
	second = PQconnectdb(argv[1]);
	require(PQstatus(first) == CONNECTION_OK, PQerrorMessage(first));
	require(PQstatus(second) == CONNECTION_OK, PQerrorMessage(second));
	first_pid = PQbackendPID(first);
	second_pid = PQbackendPID(second);
	require(first_pid != second_pid, "sessions share a backend identity");
	query(first, "CREATE TABLE chain_probe(id int PRIMARY KEY, value int NOT NULL)", NULL, NULL);
	query(first, "INSERT INTO chain_probe VALUES(1,0)", NULL, NULL);
	query(first, "SET application_name='chain-first'; SET statement_timeout='5s'", NULL, NULL);
	query(second, "SET application_name='chain-second'; SET statement_timeout='5s'; LISTEN chain_events", NULL, NULL);
	for (int i = 0; i < iterations; i++)
	{
		char expected[32];
		char notify[128];
		const char *parameters[] = {"1"};

		query(first, "BEGIN; UPDATE chain_probe SET value=value+1 WHERE id=1", NULL, NULL);
		query(first, "SAVEPOINT keep_update", NULL, NULL);
		query(first, "INSERT INTO chain_probe VALUES(1,99)", "23505", NULL);
		require(PQtransactionStatus(first) == PQTRANS_INERROR, "error did not abort the transaction");
		query(first, "ROLLBACK TO keep_update; COMMIT", NULL, NULL);
		require(PQtransactionStatus(first) == PQTRANS_IDLE, "commit did not finish the transaction");
		snprintf(expected, sizeof(expected), "%d", i + 1);
		result(first, PQexecParams(first, "SELECT value FROM chain_probe WHERE id=$1",
			1, NULL, parameters, NULL, NULL, 0), NULL, expected);
		query(second, "SELECT value FROM chain_probe", NULL, expected);
		query(first, "SHOW application_name", NULL, "chain-first");
		query(second, "SHOW application_name", NULL, "chain-second");

		query(first, "BEGIN; SELECT * FROM chain_probe FOR UPDATE", NULL, NULL);
		query(second, "SET lock_timeout='30ms'", NULL, NULL);
		query(second, "UPDATE chain_probe SET value=-1", "55P03", NULL);
		query(first, "ROLLBACK", NULL, NULL);
		query(second, "RESET lock_timeout; BEGIN; SELECT * FROM chain_probe FOR UPDATE; COMMIT", NULL, NULL);
		query(second, "SELECT value FROM chain_probe", NULL, expected);

		query(first, "SET statement_timeout='30ms'", NULL, NULL);
		query(first, "SELECT pg_sleep(60)", "57014", NULL);
		query(first, "RESET statement_timeout; SELECT 1", NULL, "1");
		cancel_sleep(first, second);
		query(first, "SET statement_timeout='5s'; BEGIN; NOTIFY chain_events, 'aborted'; ROLLBACK", NULL, NULL);
		snprintf(notify, sizeof(notify), "NOTIFY chain_events, '%s'", expected);
		query(first, notify, NULL, NULL);
		receive_notification(second, expected);
		require(PQbackendPID(first) == first_pid && PQbackendPID(second) == second_pid,
			"an error replaced a session instead of restoring it");
	}
	PQfinish(second);
	PQfinish(first);
	printf("{\"passed\":true,\"iterations\":%d,\"sessions\":2}\n", iterations);
	return 0;
}
