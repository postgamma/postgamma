/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/postgamma.h"
#include "postgamma/private/data_directory_lock.h"
#include "postgamma/shmem_platform.h"
#include "postgamma/dsm_platform.h"
#include "postgamma/io_platform.h"
#include <emscripten.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static pthread_t driver;
static _Atomic int completed;
static _Atomic unsigned rounds_completed;
static _Atomic bool started;
static int joined_result;

static void log_record(void *unused, const pgm_log_record *record)
{
    (void) unused;
    fprintf(stderr, "kernel: %s: %s\n", record->severity, record->message);
}

static int check(pgm_status status, pgm_error **error, const char *operation)
{
    if (status == PGM_STATUS_OK) return 0;
    fprintf(stderr, "%s: %s: %s\n", operation, pgm_status_name(status),
            *error ? pgm_error_message(*error) : "no diagnostic");
    pgm_error_free(*error);
    *error = NULL;
    return status;
}

static int scalar(pgm_connection *connection, const char *sql, const char *expected)
{
    pgm_result *result = NULL;
    pgm_error *error = NULL;
    pgm_value_view value = PGM_VALUE_VIEW_INIT;
    int status = check(pgm_execute(connection, sql, NULL, 0, PGM_FORMAT_TEXT,
                                  10000, &result, &error), &error, sql);
    if (!status && (pgm_result_row_count(result) != 1 || pgm_result_column_count(result) != 1))
        status = PGM_STATUS_INTERNAL_ERROR;
    if (!status)
        status = check(pgm_result_value(result, 0, 0, &value, &error), &error, "scalar value");
    if (!status && (value.is_null || value.size != strlen(expected) ||
                   memcmp(value.data, expected, value.size))) {
        fprintf(stderr, "unexpected scalar result for %s\n", sql);
        status = PGM_STATUS_INTERNAL_ERROR;
    }
    pgm_result_free(result);
    return status;
}

static int expect_error(pgm_connection *connection, const char *sql, const char *sqlstate)
{
    pgm_result *result = NULL;
    pgm_error *error = NULL;
    pgm_status status = pgm_execute(connection, sql, NULL, 0, PGM_FORMAT_TEXT,
                                   10000, &result, &error);
    int failed = status != PGM_STATUS_POSTGRES_ERROR || error == NULL ||
        pgm_error_sqlstate(error) == NULL || strcmp(pgm_error_sqlstate(error), sqlstate);
    if (failed)
        fprintf(stderr, "expected SQLSTATE %s, received %s (%s)\n", sqlstate,
            pgm_error_sqlstate(error), pgm_status_name(status));
    pgm_result_free(result);
    pgm_error_free(error);
    return failed ? PGM_STATUS_INTERNAL_ERROR : 0;
}

static void *run(void *unused)
{
    pgm_instance_options options = PGM_INSTANCE_OPTIONS_INIT;
    const pgm_setting settings[] = {
        {"shared_buffers", "24MB"}, {"max_connections", "16"},
        {"max_worker_processes", "4"}, {"max_parallel_workers", "0"},
        {"autovacuum", "off"}, {"max_wal_senders", "0"},
        {"max_logical_replication_workers", "0"}, {"wal_level", "minimal"}
    };
    int status = 0;
    (void) unused;
    options.path = "/database";
    options.executable_path = "/postgamma/bin/postgres";
    options.resource_root = "/postgamma";
    options.settings = settings;
    options.setting_count = sizeof(settings) / sizeof(settings[0]);
    options.log_callback = log_record;
    for (unsigned round = 0; round < 3; round++) {
        pgm_instance *instance = NULL;
        pgm_connection *connection = NULL;
        pgm_result *result = NULL;
        pgm_error *error = NULL;
        pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
        pgm_parameter parameter = PGM_PARAMETER_INIT;
        pgm_value_view value = PGM_VALUE_VIEW_INIT;
        options.create = round == 0;
        fprintf(stderr, "kernel smoke: round %u open\n", round);
        status = check(pgm_instance_open(&options, &instance, &error), &error, "open");
        if (status) goto finish_round;
        connection_options.user = "postgamma";
        connection_options.database = "postgres";
        connection_options.application_name = "postgamma-wasm-startup";
        status = check(pgm_connection_open(instance, &connection_options, &connection, &error), &error, "connect");
        if (status) goto finish_round;
        parameter.type_oid = 23;
        parameter.data = "1";
        parameter.size = 1;
        status = check(pgm_execute(connection, "SELECT $1::int4", &parameter, 1,
            PGM_FORMAT_TEXT, 10000, &result, &error), &error, "execute");
        if (status) goto finish_round;
        if (pgm_result_row_count(result) != 1 || pgm_result_column_count(result) != 1) {
            status = PGM_STATUS_INTERNAL_ERROR;
            goto finish_round;
        }
        status = check(pgm_result_value(result, 0, 0, &value, &error), &error, "value");
        if (!status && (value.is_null || value.size != 1 || memcmp(value.data, "1", 1)))
            status = PGM_STATUS_INTERNAL_ERROR;
        if (!status) status = scalar(connection,
            "SELECT count(DISTINCT backend_type)::text FROM pg_stat_activity "
            "WHERE backend_type IN ('checkpointer', 'background writer', 'walwriter')", "3");
        if (!status) status = scalar(connection,
            "SELECT to_tsvector('english', 'running')::text", "'run':1");
        if (!status) status = expect_error(connection, "SELECT 1 / 0", "22012");
        if (!status) status = expect_error(connection,
            "CREATE COLLATION unavailable_locale (provider = libc, locale = 'de_DE.UTF-8')", "22023");
        if (!status) status = scalar(connection, "SELECT 2::text", "2");
finish_round:
        pgm_result_free(result);
        if (connection) {
            int close_status = check(pgm_connection_close(connection, 10000, &error), &error, "disconnect");
            if (!status) status = close_status;
        }
        if (instance) {
            int close_status = check(pgm_instance_close(instance, PGM_SHUTDOWN_FAST, 10000, &error), &error, "close");
            if (!status) status = close_status;
        }
        if (postgamma_data_directory_lock_active_count() != 0 ||
            postgamma_platform_shmem_count() != 0 || postgamma_platform_dsm_count() != 0 ||
            postgamma_platform_pipe_count() != 0) {
            fprintf(stderr, "kernel resources remain: locks=%zu shmem=%zu dsm=%zu pipes=%zu\n",
                postgamma_data_directory_lock_active_count(), postgamma_platform_shmem_count(),
                postgamma_platform_dsm_count(), postgamma_platform_pipe_count());
            status = PGM_STATUS_INTERNAL_ERROR;
        }
        if (status) break;
        atomic_store(&rounds_completed, round + 1);
        fprintf(stderr, "kernel smoke: round %u closed\n", round);
    }
    atomic_store_explicit(&completed, status ? -status : 1, memory_order_release);
    return NULL;
}

EMSCRIPTEN_KEEPALIVE int postgamma_wasm_smoke_start(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&started, &expected, true)) return EALREADY;
    int status = pthread_create(&driver, NULL, run, NULL);
    if (status) atomic_store(&started, false);
    return status;
}

EMSCRIPTEN_KEEPALIVE int postgamma_wasm_smoke_poll(void)
{
    if (joined_result) return joined_result;
    if (!atomic_load(&started)) return -PGM_STATUS_INVALID_ARGUMENT;
    int status = atomic_load_explicit(&completed, memory_order_acquire);
    if (!status) return 0;
    int joined = pthread_tryjoin_np(driver, NULL);
    if (joined == EBUSY) return 0;
    if (joined) return -PGM_STATUS_INTERNAL_ERROR;
    joined_result = status < 0 ? status : (int) atomic_load(&rounds_completed);
    return joined_result;
}
