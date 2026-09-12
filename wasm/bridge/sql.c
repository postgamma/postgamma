/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* This is a private, versioned byte protocol, never a native structure layout.
 * One host driver owns every pgm handle. Queries use nonblocking next-result
 * calls, so a kernel wait never occupies the control Worker or this driver. */
enum { OPEN = 1, CONNECT, EXECUTE, NEXT, RELEASE, DISCONNECT, CLOSE,
       CONNECTION_STATUS, CANCEL, TELEMETRY, STOP, COPY_READ, COPY_WRITE,
       COPY_FINISH, COPY_CLOSE, COPY_ABORT };
enum { INSTANCE = 1, CONNECTION, REQUEST, STREAM };
#define WIRE_VERSION 1
#define SLOT_COUNT 64
#define WIRE_LIMIT (2U * 1024U * 1024U)
typedef struct Handle { uint32_t kind, generation, parent; void *pointer; } Handle;
typedef struct Reader { const unsigned char *data; size_t size, offset; bool failed; } Reader;
typedef struct Writer { unsigned char *data; size_t size, capacity; bool failed; } Writer;
static Handle handles[SLOT_COUNT];
static pthread_t driver;
static _Atomic int state; /* 0 idle, 1 submitted, 2 reply, 3 stopped, -1 allocation failure */
static bool started;
static unsigned char *input;
static size_t input_size;
static Writer reply;

static uint32_t get(Reader *r)
{
    if (r->failed || r->size - r->offset < 4) { r->failed = true; return 0; }
    const unsigned char *p = r->data + r->offset;
    r->offset += 4;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static const void *blob(Reader *r, size_t *size)
{
    *size = get(r);
    if (r->failed || *size > r->size - r->offset) { r->failed = true; return NULL; }
    const void *p = r->data + r->offset;
    r->offset += *size;
    return p;
}
static char *string(Reader *r)
{
    size_t size;
    const void *p = blob(r, &size);
    if (!p || memchr(p, 0, size)) { r->failed = true; return NULL; }
    char *value = malloc(size + 1);
    if (!value) { r->failed = true; return NULL; }
    memcpy(value, p, size); value[size] = 0;
    return value;
}
static void bytes(Writer *w, const void *data, size_t size)
{
    if (w->failed || size > WIRE_LIMIT - w->size) { w->failed = true; return; }
    if (w->size + size > w->capacity) {
        size_t capacity = w->size + size + 4096;
        void *p = realloc(w->data, capacity);
        if (!p) { w->failed = true; return; }
        w->data = p; w->capacity = capacity;
    }
    if (size) memcpy(w->data + w->size, data, size);
    w->size += size;
}
static void put(Writer *w, uint32_t n)
{
    unsigned char p[4] = {n, n >> 8, n >> 16, n >> 24};
    bytes(w, p, 4);
}
static void value(Writer *w, const void *p, size_t size, bool is_null)
{
    put(w, is_null ? UINT32_MAX : (uint32_t)size);
    if (!is_null) bytes(w, p, size);
}
static void text(Writer *w, const char *s) { value(w, s, s ? strlen(s) : 0, s == NULL); }
static void number64(Writer *w, uint64_t n) { put(w, n); put(w, n >> 32); }
static Handle *lookup(Reader *r, uint32_t kind)
{
    uint32_t id = get(r), generation = get(r);
    if (id == 0 || id > SLOT_COUNT || handles[id - 1].kind != kind ||
        handles[id - 1].generation != generation) { r->failed = true; return NULL; }
    return &handles[id - 1];
}
static Handle *reserve(void)
{
    for (unsigned i = 0; i < SLOT_COUNT; i++)
        if (!handles[i].kind && handles[i].generation != UINT32_MAX) return &handles[i];
    return NULL;
}
static void publish(Writer *w, Handle *h, uint32_t kind, Handle *parent, void *pointer)
{
    h->kind = kind; h->generation++; h->pointer = pointer;
    h->parent = parent ? (uint32_t)(parent - handles + 1) : 0;
    put(w, kind); put(w, h - handles + 1); put(w, h->generation);
}
static bool children(Handle *h)
{
    for (unsigned i = 0; i < SLOT_COUNT; i++)
        if (handles[i].kind && handles[i].parent == (uint32_t)(h - handles + 1)) return true;
    return false;
}
static void retire(Handle *h) { h->kind = 0; h->parent = 0; h->pointer = NULL; }
static void log_record(void *unused, const pgm_log_record *record)
{
    (void)unused;
    fprintf(stderr, "kernel: %s: %s\n", record->severity, record->message);
}
static pgm_status marshal_result(Writer *w, pgm_result *result, pgm_error **error)
{
    put(w, pgm_result_kind(result)); text(w, pgm_result_command_status(result));
    size_t columns = pgm_result_column_count(result), rows = pgm_result_row_count(result);
    put(w, columns); put(w, rows);
    for (size_t c = 0; c < columns; c++) {
        pgm_column col = PGM_COLUMN_INIT;
        pgm_status status = pgm_result_column(result, c, &col, error);
        if (status) return status;
        text(w, col.name); put(w, col.type_oid); put(w, col.format);
        put(w, col.table_oid); put(w, col.table_column); put(w, col.type_size); put(w, col.type_modifier);
    }
    for (size_t r = 0; r < rows; r++) for (size_t c = 0; c < columns; c++) {
        pgm_value_view v = PGM_VALUE_VIEW_INIT;
        pgm_status status = pgm_result_value(result, r, c, &v, error);
        if (status) return status;
        value(w, v.data, v.size, v.is_null);
    }
    return PGM_STATUS_OK;
}
static bool command(void)
{
    Reader r = {input, input_size, 0, false};
    Writer payload = {0};
    pgm_error *error = NULL;
    pgm_status status = PGM_STATUS_OK;
    uint32_t version = get(&r), sequence = get(&r), op = get(&r);
    bool stop = false;
    Handle *h = NULL, *slot = NULL;
    if (version != WIRE_VERSION) r.failed = true;
#ifdef POSTGAMMA_WASM_TESTING
    /* Test-only actual Wasm traps/abort on the owning host pthread. */
    if (!r.failed && op == 65535) {
        uint32_t fault = get(&r);
        if (r.failed || r.offset != r.size) r.failed = true;
        else if (fault == 1) __builtin_trap();
        else if (fault == 2) abort();
    }
#endif
    if (op != OPEN && op != STOP)
        h = lookup(&r, op == CONNECT || op == CLOSE || op == TELEMETRY ? INSTANCE :
                   op == NEXT || op == RELEASE || op == CANCEL ? REQUEST :
                   op >= COPY_READ && op <= COPY_ABORT ? STREAM : CONNECTION);
    if (op == OPEN || op == CONNECT || op == EXECUTE) {
        slot = reserve();
        if (!slot) status = PGM_STATUS_BUSY;
    }
    if (r.failed || status) goto done;
    switch (op) {
    case OPEN: {
        uint32_t create = get(&r), workers = get(&r);
        char *path = string(&r);
        pgm_instance_options o = PGM_INSTANCE_OPTIONS_INIT;
        const pgm_setting settings[] = {
            {"shared_buffers", "24MB"}, {"max_connections", "16"},
            {"max_worker_processes", "4"}, {"max_parallel_workers", "0"},
            {"autovacuum", "off"}, {"max_wal_senders", "0"},
            {"max_logical_replication_workers", "0"}, {"wal_level", "minimal"}
        };
        for (unsigned i = 0; i < SLOT_COUNT; i++) if (handles[i].kind == INSTANCE) status = PGM_STATUS_BUSY;
        if (!r.failed && !status && create <= 1 && workers >= 2 && workers <= 4 && r.offset == r.size) {
            pgm_instance *instance = NULL;
            o.create = create; o.path = path; o.executor_worker_count = workers;
            o.executable_path = "/postgamma/bin/postgres"; o.resource_root = "/postgamma";
            o.settings = settings; o.setting_count = sizeof(settings) / sizeof(settings[0]);
            o.result_buffer_limit = 256 * 1024; o.maximum_value_size = 128 * 1024;
            o.log_callback = log_record;
            status = pgm_instance_open(&o, &instance, &error);
            if (!status) publish(&payload, slot, INSTANCE, NULL, instance);
        } else if (!status) r.failed = true;
        free(path); break;
    }
    case CONNECT: {
        char *name = string(&r);
        if (!r.failed && r.offset == r.size) {
            pgm_connection_options o = PGM_CONNECTION_OPTIONS_INIT;
            pgm_connection *connection = NULL;
            o.user = "postgamma"; o.database = "postgres"; o.application_name = name;
            status = pgm_connection_open(h->pointer, &o, &connection, &error);
            if (!status) publish(&payload, slot, CONNECTION, h, connection);
        } else r.failed = true;
        free(name); break;
    }
    case EXECUTE: {
        char *sql = string(&r);
        pgm_execute_options o = PGM_EXECUTE_OPTIONS_INIT;
        uint32_t format = get(&r), chunk_rows = get(&r), count = get(&r);
        pgm_parameter parameters[64];
        if (count > 64 || format > 1 || chunk_rows > 1024) r.failed = true;
        for (uint32_t i = 0; i < count && !r.failed; i++) {
            parameters[i] = (pgm_parameter)PGM_PARAMETER_INIT;
            parameters[i].type_oid = get(&r);
            uint32_t pf = get(&r), null = get(&r);
            if (pf > 1 || null > 1) r.failed = true;
            parameters[i].format = pf; parameters[i].is_null = null;
            parameters[i].data = blob(&r, &parameters[i].size);
            if (null) {
                if (parameters[i].size) r.failed = true;
                parameters[i].data = NULL;
            }
        }
        if (!r.failed && r.offset == r.size) {
            pgm_request *request = NULL;
            o.parameters = parameters; o.parameter_count = count; o.result_format = format;
            o.delivery_mode = chunk_rows ? PGM_DELIVERY_CHUNKED : PGM_DELIVERY_MATERIALIZED;
            o.target_chunk_rows = chunk_rows;
            o.result_buffer_limit = 256 * 1024;
            status = pgm_execute_async_ex(h->pointer, sql, strlen(sql), &o, &request, &error);
            if (!status) publish(&payload, slot, REQUEST, h, request);
        } else r.failed = true;
        free(sql); break;
    }
    case NEXT: {
        if (r.offset != r.size) { r.failed = true; break; }
        pgm_result *result = NULL;
        pgm_availability availability = PGM_AVAILABILITY_AGAIN;
        status = pgm_request_next_result(h->pointer, 0, &result, &availability, &error);
        if (!status && error) status = pgm_error_status(error);
        put(&payload, availability);
        if (!status && result) status = marshal_result(&payload, result, &error);
        if (!status && result && (pgm_result_kind(result) == PGM_RESULT_COPY_IN ||
                                  pgm_result_kind(result) == PGM_RESULT_COPY_OUT)) {
            pgm_copy *copy = NULL;
            slot = reserve();
            if (!slot) status = PGM_STATUS_BUSY;
            else {
                status = pgm_result_take_copy(&result, &copy, &error);
                if (!status) publish(&payload, slot, STREAM, h, copy);
            }
        }
        pgm_result_free(result); break;
    }
    case RELEASE:
    case DISCONNECT:
    case CLOSE:
        if (r.offset != r.size) { r.failed = true; break; }
        if (children(h)) { status = PGM_STATUS_BUSY; break; }
        if (op == RELEASE) pgm_request_free(h->pointer);
        else if (op == DISCONNECT) status = pgm_connection_close(h->pointer, 10000, &error);
        else status = pgm_instance_close(h->pointer, PGM_SHUTDOWN_FAST, 10000, &error);
        if (!status) {
            retire(h);
            if (op == CLOSE) {
                put(&payload, postgamma_data_directory_lock_active_count());
                put(&payload, postgamma_platform_shmem_count());
                put(&payload, postgamma_platform_dsm_count());
                put(&payload, postgamma_platform_pipe_count());
            }
        }
        break;
    case CONNECTION_STATUS: {
        if (r.offset != r.size) { r.failed = true; break; }
        pgm_connection_status_snapshot s = PGM_CONNECTION_STATUS_SNAPSHOT_INIT;
        status = pgm_connection_get_status(h->pointer, &s, &error);
        put(&payload, s.transaction_status); put(&payload, s.pin_reasons); put(&payload, s.carrier_retained);
        put(&payload, s.virtual_backend_pid); number64(&payload, s.connection_id); number64(&payload, s.active_request_id);
        break;
    }
    case CANCEL:
        if (r.offset != r.size) { r.failed = true; break; }
        status = pgm_request_cancel(h->pointer, &error); break;
    case COPY_READ: {
        uint32_t capacity = get(&r);
        unsigned char buffer[16384];
        size_t produced = 0;
        pgm_io_state progress = PGM_IO_AGAIN;
        if (r.offset != r.size || capacity == 0 || capacity > sizeof(buffer)) { r.failed = true; break; }
        status = pgm_copy_read(h->pointer, buffer, capacity, &produced, &progress, &error);
        put(&payload, progress); value(&payload, buffer, produced, false); break;
    }
    case COPY_WRITE: {
        size_t size, consumed = 0;
        const void *data = blob(&r, &size);
        pgm_io_state progress = PGM_IO_AGAIN;
        if (r.failed || r.offset != r.size || size == 0 || size > 16384) { r.failed = true; break; }
        status = pgm_copy_write(h->pointer, data, size, &consumed, &progress, &error);
        put(&payload, progress); put(&payload, consumed); break;
    }
    case COPY_FINISH: {
        pgm_io_state progress = PGM_IO_AGAIN;
        if (r.offset != r.size) { r.failed = true; break; }
        status = pgm_copy_finish(h->pointer, NULL, 0, &progress, &error);
        put(&payload, progress); break;
    }
    case COPY_CLOSE:
        if (r.offset != r.size) { r.failed = true; break; }
        status = pgm_copy_close(h->pointer, 10000, &error);
        if (!status) retire(h);
        break;
    case COPY_ABORT:
        if (r.offset != r.size) { r.failed = true; break; }
        status = pgm_copy_abort(h->pointer, NULL, 0, &error); break;
    case TELEMETRY: {
        if (r.offset != r.size) { r.failed = true; break; }
        pgm_instance_telemetry t = PGM_INSTANCE_TELEMETRY_INIT;
        status = pgm_instance_get_telemetry(h->pointer, &t, &error);
        put(&payload, t.executor_worker_count);
        number64(&payload, t.connection_count); number64(&payload, t.active_request_count);
        number64(&payload, t.running_session_count); number64(&payload, t.pinned_session_count);
        number64(&payload, t.runnable_session_count); number64(&payload, t.queued_request_count);
        number64(&payload, t.execution_token_rejections); break;
    }
    case STOP:
        if (r.offset != r.size) { r.failed = true; break; }
        for (unsigned i = 0; i < SLOT_COUNT; i++) if (handles[i].kind) status = PGM_STATUS_BUSY;
        stop = !status; break;
    default: r.failed = true;
    }
done:
    if (r.failed) status = PGM_STATUS_INVALID_ARGUMENT;
    if (payload.failed) status = PGM_STATUS_OUT_OF_MEMORY;
    put(&reply, WIRE_VERSION); put(&reply, sequence); put(&reply, op); put(&reply, status);
    for (unsigned i = 1; i <= 17; i++) text(&reply, pgm_error_field(error, i));
    if (!status) bytes(&reply, payload.data, payload.size);
    free(payload.data); pgm_error_free(error);
    return stop;
}
static void *run(void *unused)
{
    (void)unused;
    for (;;) {
        if (atomic_load_explicit(&state, memory_order_acquire) != 1) { usleep(1000); continue; }
        bool stop = command();
        atomic_store_explicit(&state, reply.failed ? -1 : 2, memory_order_release);
        if (stop || reply.failed) return NULL;
    }
}
EMSCRIPTEN_KEEPALIVE void *postgamma_wasm_alloc(unsigned size) { return size <= WIRE_LIMIT ? malloc(size) : NULL; }
EMSCRIPTEN_KEEPALIVE void postgamma_wasm_free(void *p) { free(p); }
EMSCRIPTEN_KEEPALIVE int postgamma_wasm_start(void)
{
    if (started) return EALREADY;
    int result = pthread_create(&driver, NULL, run, NULL);
    if (!result) started = true;
    return result;
}
EMSCRIPTEN_KEEPALIVE int postgamma_wasm_submit(void *data, unsigned size)
{
    if (!started || atomic_load(&state) != 0) return EBUSY;
    if (!data || size < 12 || size > WIRE_LIMIT) return EINVAL;
    input = data; input_size = size;
    atomic_store_explicit(&state, 1, memory_order_release);
    return 0;
}
EMSCRIPTEN_KEEPALIVE int postgamma_wasm_poll(void) { return atomic_load_explicit(&state, memory_order_acquire); }
EMSCRIPTEN_KEEPALIVE void *postgamma_wasm_reply(void) { return reply.data; }
EMSCRIPTEN_KEEPALIVE unsigned postgamma_wasm_reply_size(void) { return reply.size; }
EMSCRIPTEN_KEEPALIVE void postgamma_wasm_ack(void)
{
    free(input); input = NULL; free(reply.data); reply = (Writer){0};
    atomic_store_explicit(&state, 0, memory_order_release);
}
EMSCRIPTEN_KEEPALIVE int postgamma_wasm_join(void)
{
    int result = pthread_tryjoin_np(driver, NULL);
    if (!result) { started = false; atomic_store(&state, 3); }
    return result;
}
