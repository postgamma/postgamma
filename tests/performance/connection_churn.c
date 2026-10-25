/* Ordinary libpq connection/SELECT 1/close timing; never linked into the server. */
#define _POSIX_C_SOURCE 200809L
#include <libpq-fe.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Sample
{
	double connect_us;
	double complete_us;
} Sample;

typedef struct Worker
{
	const char *conninfo;
	int iterations;
	pthread_barrier_t *barrier;
	Sample *samples;
	double start_us;
	double end_us;
	char error[512];
} Worker;

static double
now_us(void)
{
	struct timespec value;
	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		abort();
	return value.tv_sec * 1000000.0 + value.tv_nsec / 1000.0;
}

static int
execute_checked(Worker *worker, PGconn *connection)
{
	PGresult *result = PQexec(connection, "SELECT 1");
	int valid = result != NULL && PQresultStatus(result) == PGRES_TUPLES_OK &&
		PQntuples(result) == 1 && PQnfields(result) == 1 &&
		PQftype(result, 0) == 23 && !PQgetisnull(result, 0, 0) &&
		strcmp(PQgetvalue(result, 0, 0), "1") == 0;
	if (!valid)
		snprintf(worker->error, sizeof(worker->error), "query: %s", PQerrorMessage(connection));
	PQclear(result);
	return valid;
}

static PGconn *
connect_checked(Worker *worker, int execute)
{
	PGconn *connection = PQconnectdb(worker->conninfo);
	if (connection == NULL || PQstatus(connection) != CONNECTION_OK)
	{
		snprintf(worker->error, sizeof(worker->error), "connect: %s",
				 connection == NULL ? "allocation failed" : PQerrorMessage(connection));
		PQfinish(connection);
		return NULL;
	}
	if (execute && !execute_checked(worker, connection))
	{
		PQfinish(connection);
		return NULL;
	}
	return connection;
}

static int
transaction(Worker *worker, Sample *sample)
{
	double start = now_us();
	PGconn *connection = connect_checked(worker, 0);
	double connected = now_us();
	int valid;
	if (connection == NULL)
		return 0;
	valid = execute_checked(worker, connection);
	PQfinish(connection);
	sample->connect_us = connected - start;
	sample->complete_us = now_us() - start;
	return valid;
}

static void *
run_worker(void *argument)
{
	Worker *worker = argument;
	Sample discarded;
	for (int i = 0; i < 50 && worker->error[0] == '\0'; ++i)
		(void) transaction(worker, &discarded);
	(void) pthread_barrier_wait(worker->barrier);
	worker->start_us = now_us();
	for (int i = 0; i < worker->iterations && worker->error[0] == '\0'; ++i)
		(void) transaction(worker, &worker->samples[i]);
	worker->end_us = now_us();
	return NULL;
}

static int
positive(const char *value, int limit)
{
	char *end;
	long number = strtol(value, &end, 10);
	if (*value == '\0' || *end != '\0' || number <= 0 || number > limit)
	{
		fprintf(stderr, "invalid count: %s\n", value);
		exit(2);
	}
	return (int) number;
}

int
main(int argc, char **argv)
{
	int clients;
	if (argc == 4 && strcmp(argv[1], "idle") == 0)
	{
		Worker worker = {.conninfo = argv[2]};
		PGconn **connections;
		int opened = 0;
		clients = positive(argv[3], 1024);
		connections = calloc((size_t) clients, sizeof(*connections));
		if (connections == NULL)
			return 1;
		for (; opened < clients; ++opened)
		{
			connections[opened] = connect_checked(&worker, 1);
			if (connections[opened] == NULL)
				break;
		}
		if (opened == clients)
		{
			puts("ready");
			fflush(stdout);
			(void) getchar();
		}
		for (int i = 0; i < opened; ++i)
			PQfinish(connections[i]);
		free(connections);
		if (worker.error[0])
			fprintf(stderr, "%s\n", worker.error);
		return opened == clients ? 0 : 1;
	}
	if (argc != 6 || strcmp(argv[1], "churn") != 0)
	{
		fprintf(stderr, "usage: %s churn CONNINFO CLIENTS ITERATIONS SAMPLES.csv\n"
				"       %s idle CONNINFO CLIENTS\n", argv[0], argv[0]);
		return 2;
	}
	clients = positive(argv[3], 1024);
	int iterations = positive(argv[4], 1000000);
	pthread_t *threads = calloc((size_t) clients, sizeof(*threads));
	Worker *workers = calloc((size_t) clients, sizeof(*workers));
	Sample *samples = calloc((size_t) clients * iterations, sizeof(*samples));
	pthread_barrier_t barrier;
	if (threads == NULL || workers == NULL || samples == NULL ||
		pthread_barrier_init(&barrier, NULL, (unsigned) clients + 1) != 0)
		return 1;
	for (int i = 0; i < clients; ++i)
	{
		workers[i] = (Worker) {.conninfo = argv[2], .iterations = iterations,
			.barrier = &barrier, .samples = samples + (size_t) i * iterations};
		if (pthread_create(&threads[i], NULL, run_worker, &workers[i]) != 0)
		{
			fputs("cannot start all clients\n", stderr);
			return 1;
		}
	}
	/* Use the earliest worker start and latest finish, excluding warmup/join. */
	(void) pthread_barrier_wait(&barrier);
	int failed = 0;
	for (int i = 0; i < clients; ++i)
	{
		if (pthread_join(threads[i], NULL) != 0)
			abort();
		if (workers[i].error[0])
		{
			fprintf(stderr, "client %d: %s\n", i, workers[i].error);
			failed = 1;
		}
	}
	double start = workers[0].start_us;
	double end = workers[0].end_us;
	for (int i = 1; i < clients; ++i)
	{
		if (workers[i].start_us < start)
			start = workers[i].start_us;
		if (workers[i].end_us > end)
			end = workers[i].end_us;
	}
	double elapsed = (end - start) / 1000000.0;
	FILE *output = fopen(argv[5], "w");
	if (output == NULL)
		return 1;
	if (fprintf(output, "client,iteration,connect_us,complete_us\n") < 0)
		failed = 1;
	for (int i = 0; i < clients; ++i)
		for (int j = 0; j < iterations; ++j)
		{
			Sample *sample = &samples[(size_t) i * iterations + j];
			if (fprintf(output, "%d,%d,%.3f,%.3f\n", i, j,
						sample->connect_us, sample->complete_us) < 0)
				failed = 1;
		}
	if (fclose(output) != 0)
		failed = 1;
	printf("{\"passed\":%s,\"transactions\":%d,\"seconds\":%.9f}\n",
		   failed ? "false" : "true", clients * iterations, elapsed);
	pthread_barrier_destroy(&barrier);
	free(samples);
	free(workers);
	free(threads);
	return failed ? 1 : 0;
}
