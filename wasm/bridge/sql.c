/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include "postgamma/postgamma.h"
#include "wasm-sql-policy.h"
#include "postgamma/private/data_directory_lock.h"
#include "postgamma/private/wasm_events.h"
#include "postgamma/private/public_runtime.h"
#include "postgamma/shmem_platform.h"
#include "postgamma/dsm_platform.h"
#include "postgamma/io_platform.h"
#include "postgamma/wasm_storage.h"
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <malloc.h>
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
 * calls. Connection startup, cancellation and retirement perform bounded
 * steps and yield the driver while tickets, transport or backends are pending. */
enum { OPEN = 1, CONNECT, EXECUTE, NEXT, RELEASE, DISCONNECT, CLOSE,
       CONNECTION_STATUS, CANCEL, TELEMETRY, STOP, COPY_READ, COPY_WRITE,
       COPY_FINISH, COPY_CLOSE, COPY_ABORT, CONNECT_POLL, SCRIPT, EVENTS,
       RESTORE_OPEN, ARCHIVE_READ, ARCHIVE_WRITE, RESTORE_FINISH,
       PREPARE, STATEMENT_EXECUTE, STATEMENT_INFO, STATEMENT_CLOSE, STATEMENT_FREE };
enum { INSTANCE = 1, CONNECTION, REQUEST, STREAM, CONNECTING, STATEMENT = 7 };
#define WIRE_VERSION 8
#define SLOT_COUNT 64
#define WIRE_LIMIT PGW_COMMAND_BYTES
typedef struct Handle {
    uint32_t kind, generation, parent;
    void *pointer;
    uint64_t ready_sequence;
    bool watching;
    bool closing;
    bool progress_watching;
    double cancel_deadline_ms;
    double close_deadline_ms, close_retry_ms;
} Handle;
typedef struct Reader { const unsigned char *data; size_t size, offset; bool failed; } Reader;
typedef struct Writer { unsigned char *data; size_t size, capacity; bool failed; } Writer;
static Handle handles[SLOT_COUNT];
static pthread_t driver;
static _Atomic int state; /* 0 idle, 1 submitted, 2 reply, 3 stopped, -1 allocation failure */
static bool started;
static unsigned char *input;
static size_t input_size;
static Writer reply;
static em_proxying_queue *notifications;
static _Atomic uint32_t wake_count;
/* One owned event and one outstanding edge. The native queue remains bounded
 * while JS is occupied; its overflow record is delivered instead of hidden. */
static pgm_event *pending_event;
static bool event_notified;
/* Lock-free failure snapshots must not call back into the waiting driver. */
static _Atomic unsigned driver_phase, active_sequence, active_operation, command_started;
static _Atomic unsigned startup_slot, startup_phase, startup_error, startup_backend;
extern uintptr_t postgamma_platform_notification_address(void);
extern unsigned postgamma_thread_diagnostic(unsigned thread, unsigned field);
#ifdef POSTGAMMA_WASM_TESTING
static _Atomic bool diagnostic_hold;
EMSCRIPTEN_KEEPALIVE void postgamma_wasm_resume_test(void)
{
    atomic_store(&diagnostic_hold, false);
    postgamma_platform_notify();
}
#endif

EM_JS(void, postgamma_bridge_event, (unsigned kind, unsigned id, unsigned generation), {
    Module['postgammaBridgeEvent'](kind, id, generation);
});

typedef struct ReadyEvent { uint32_t kind, id, generation; } ReadyEvent;
static void deliver_event(void *argument)
{
    ReadyEvent *event = argument;
    if (event) {
        postgamma_bridge_event(event->kind, event->id, event->generation);
        free(event);
    } else postgamma_bridge_event(0, 0, 0);
}
static void notify_control(void (*callback)(void *), void *argument)
{
    if (!emscripten_proxy_async(notifications, emscripten_main_runtime_thread_id(), callback, argument))
        abort();
}
static uint64_t ready_sequence(Handle *h)
{
    Handle *request = h->kind == STREAM ? &handles[h->parent - 1] : h;
    return postgamma_public_request_ready_sequence(request->pointer);
}
static void watch_begin(Handle *h)
{
    h->watching = false;
    // Capture before consuming: an edge between AGAIN and arming is retained.
    h->ready_sequence = ready_sequence(h);
}
static bool ready_now(Handle *h)
{
    if (ready_sequence(h) != h->ready_sequence) return true;
    // NEXT may consume only part of already readable input. The producer can
    // have finished before watch_begin, leaving no future notification edge.
    // Retain the level until the consumer advances, one bounded call per wake.
    // COPY has direction-specific progress and keeps its existing edge watch.
    return h->kind == REQUEST && postgamma_public_request_progress_ready(h->pointer);
}
static double notify_ready(void)
{
    double now = emscripten_get_now(), wait_ms = __builtin_inf();
    for (unsigned i = 0; i < SLOT_COUNT; i++) {
        Handle *h = &handles[i];
        if (!h->watching && !h->progress_watching) continue;
        if (h->progress_watching) {
            // Lifecycle tickets and backend release have no public waitable.
            // Retry only while a bounded lifecycle step is pending.
            double remaining = h->close_retry_ms - now;
            if (remaining > 0) {
                if (remaining < wait_ms) wait_ms = remaining;
                if (!h->watching || !ready_now(h)) continue;
            }
        } else if (!ready_now(h)) continue;
        ReadyEvent *event = malloc(sizeof(*event));
        if (!event) abort();
        *event = (ReadyEvent){h->kind, i + 1, h->generation};
        // One notification per armed handle; the next consuming call rearms.
        h->watching = false;
        h->progress_watching = false;
        notify_control(deliver_event, event);
    }
    return wait_ms;
}

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
    h->closing = false; h->close_deadline_ms = h->close_retry_ms = 0;
    h->progress_watching = false; h->cancel_deadline_ms = 0;
    h->parent = parent ? (uint32_t)(parent - handles + 1) : 0;
    put(w, kind); put(w, h - handles + 1); put(w, h->generation);
}
static bool children(Handle *h)
{
    for (unsigned i = 0; i < SLOT_COUNT; i++)
        if (handles[i].kind && handles[i].parent == (uint32_t)(h - handles + 1)) return true;
    return false;
}
static void retire(Handle *h) { h->watching = h->progress_watching = false; h->kind = 0; h->parent = 0; h->pointer = NULL; }
static void progress_again(Handle *h, Writer *payload)
{
    h->close_retry_ms = emscripten_get_now() + 1;
    h->progress_watching = true;
    put(payload, PGM_AVAILABILITY_AGAIN);
}
static void log_record(void *unused, const pgm_log_record *record)
{
    (void)unused;
    fprintf(stderr, "kernel: %s: %s\n", record->severity, record->message);
}
static pgm_status take_event(pgm_event **event, pgm_error **error)
{
    *event = NULL;
    for (unsigned i = 0; i < SLOT_COUNT; i++)
        if (handles[i].kind == INSTANCE)
            return postgamma_public_wasm_next_event(handles[i].pointer, event, error);
    return PGM_STATUS_OK;
}
static void notify_events(void)
{
    for (unsigned i = 0; i < SLOT_COUNT; i++)
        if (handles[i].kind == CONNECTION && !handles[i].closing)
            postgamma_public_wasm_consume_idle(handles[i].pointer);
    if (event_notified) return;
    if (!pending_event && take_event(&pending_event, NULL)) abort();
    if (pending_event) {
        ReadyEvent *event = malloc(sizeof(*event));
        if (!event) abort();
        *event = (ReadyEvent){6, 0, 0};
        event_notified = true;
        notify_control(deliver_event, event);
    }
}
static pgm_status marshal_events(Writer *w, pgm_error **error)
{
    Writer records = {0};
    unsigned count = 0;
    pgm_status status = PGM_STATUS_OK;
    for (unsigned batch = 0; batch < 16; batch++) {
        pgm_event *event = pending_event;
        pending_event = NULL;
        if (!event) status = take_event(&event, error);
        if (status || !event) break;
        pgm_event_kind kind = pgm_event_type(event);
        if (kind == PGM_EVENT_NOTIFICATION) {
            pgm_notification n = PGM_NOTIFICATION_INIT;
            status = pgm_event_notification(event, &n, error);
            for (unsigned i = 0; !status && i < SLOT_COUNT; i++) {
                Handle *h = &handles[i];
                if (h->kind != CONNECTION || h->closing ||
                    ((pgm_connection *)h->pointer)->identity != n.connection_id) continue;
                put(&records, 1); put(&records, CONNECTION);
                put(&records, i + 1); put(&records, h->generation);
                put(&records, n.virtual_backend_pid); text(&records, n.channel); text(&records, n.payload);
                count++; break;
            }
        } else if (kind == PGM_EVENT_OVERFLOW) {
            pgm_event_overflow_record overflow = PGM_EVENT_OVERFLOW_RECORD_INIT;
            status = pgm_event_overflow(event, &overflow, error);
            if (!status && overflow.dropped_kind == PGM_EVENT_NOTIFICATION) {
                put(&records, 2); number64(&records, overflow.dropped_count); count++;
            }
        } else if (kind == PGM_EVENT_LOG) {
            pgm_log_record record = PGM_LOG_RECORD_INIT;
            status = pgm_event_log(event, &record, error);
            if (!status) log_record(NULL, &record);
        }
        pgm_event_free(event);
        if (status || records.failed) break;
    }
    put(w, count); bytes(w, records.data, records.size);
    w->failed |= records.failed;
    free(records.data);
    event_notified = false;
    return status;
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
#include "archive.h"
#ifdef POSTGAMMA_WASM_TESTING
#include "../../tests/wasm/storage/filesystem.c"
#include "../../tests/wasm/sql/close_gate.c"
#include "../../tests/wasm/sql/control_gate.c"
#endif
static bool command(void)
{
    Reader r = {input, input_size, 0, false};
    Writer payload = {0};
    pgm_error *error = NULL;
    pgm_status status = PGM_STATUS_OK;
    const char *bridge_message = NULL;
    int rollback_error = 0;
    uint32_t version = get(&r), sequence = get(&r), op = get(&r);
    atomic_store(&active_sequence, sequence);
    atomic_store(&active_operation, op);
    atomic_store(&command_started, (unsigned) emscripten_get_now());
    bool stop = false;
    Handle *h = NULL, *slot = NULL;
    if (version != WIRE_VERSION) r.failed = true;
#ifdef POSTGAMMA_WASM_TESTING
    if (!r.failed && op == 65530) {
        if (r.offset != r.size) r.failed = true;
        else {
            atomic_store(&diagnostic_hold, true);
            while (atomic_load(&diagnostic_hold)) {
                uint32_t expected = postgamma_platform_notification_sequence();
                if (atomic_load(&diagnostic_hold))
                    postgamma_platform_notification_wait(expected, __builtin_inf());
            }
        }
        goto done;
    }
    if (!r.failed && op == 65531) {
        unsigned action = get(&r);
        h = lookup(&r, action == 1 ? INSTANCE : CONNECTING);
        unsigned capacity = get(&r);
        if (r.failed || r.offset != r.size || (action != 1 && action != 2)) r.failed = true;
        else if (action == 1 && capacity >= 16 && capacity <= 65536)
            ((pgm_instance *)h->pointer)->transport_queue_capacity = capacity;
        else if (action == 2)
            ((pgm_connection *)h->pointer)->connect_deadline_ns = postgamma_monotonic_now_ns() - 1;
        else r.failed = true;
        goto done;
    }
    if (!r.failed && op == 65532) {
        unsigned action = get(&r);
        if (r.failed || r.offset != r.size || action > 2) r.failed = true;
        else if (action == 2) put(&payload, control_gate_ticket != NULL);
        else {
            control_gate_armed = action == 1;
            control_gate_ticket = NULL;
        }
        goto done;
    }
    if (!r.failed && op == 65533) {
        h = lookup(&r, CONNECTION);
        unsigned hold = get(&r);
        if (r.failed || r.offset != r.size || hold > 1) r.failed = true;
        else if (close_gate(h->pointer, hold)) {
            status = PGM_STATUS_INTERNAL_ERROR; bridge_message = "Connection close gate failed";
        }
        goto done;
    }
    if (!r.failed && op == 65534) {
        uint32_t phase = get(&r);
        if (r.failed || r.offset != r.size) r.failed = true;
        else {
            int checks = storage_filesystem_probe(phase);
            if (checks < 0) {status = PGM_STATUS_INTERNAL_ERROR; bridge_message = "Storage filesystem check failed";}
            else put(&payload, checks);
        }
        goto done;
    }
    /* Test-only actual Wasm traps/abort on the owning host pthread. */
    if (!r.failed && op == 65535) {
        uint32_t fault = get(&r);
        if (r.failed || r.offset != r.size) r.failed = true;
        else if (fault == 1) __builtin_trap();
        else if (fault == 2) abort();
    }
#endif
    if (op != OPEN && op != STOP && op != EVENTS && (op < RESTORE_OPEN || op > RESTORE_FINISH))
        h = lookup(&r, op == CONNECT || op == CLOSE || op == TELEMETRY ? INSTANCE :
                   op == CONNECT_POLL ? CONNECTING :
                   op == NEXT || op == RELEASE || op == CANCEL ? REQUEST :
                   op >= STATEMENT_EXECUTE && op <= STATEMENT_FREE ? STATEMENT :
                   op >= COPY_READ && op <= COPY_ABORT ? STREAM : CONNECTION);
    if (op == OPEN || op == RESTORE_FINISH || op == CONNECT || op == EXECUTE || op == SCRIPT ||
        op == PREPARE || op == STATEMENT_EXECUTE || op == STATEMENT_CLOSE) {
        slot = reserve();
        if (!slot) status = PGM_STATUS_BUSY;
    }
    if (r.failed || status) goto done;
    if (h && h->closing && op != DISCONNECT && op != RELEASE && op != COPY_CLOSE &&
        op != STATEMENT_CLOSE && op != STATEMENT_FREE) {
        status = PGM_STATUS_BUSY; bridge_message = "Handle is closing"; goto done;
    }
    if (op == EXECUTE || op == SCRIPT || op == STATEMENT_EXECUTE || op == PREPARE) {
        unsigned opening = 0;
        for (unsigned i = 0; i < SLOT_COUNT; i++) if (handles[i].kind == CONNECTING) opening++;
        if (opening) {
            pgm_connection *connection = op == STATEMENT_EXECUTE ?
                ((pgm_statement *)h->pointer)->connection : h->pointer;
            pgm_instance_telemetry capacity = PGM_INSTANCE_TELEMETRY_INIT;
            pgm_connection_status_snapshot session = PGM_CONNECTION_STATUS_SNAPSHOT_INIT;
            status = pgm_instance_get_telemetry(connection->instance, &capacity, &error);
            if (!status) status = pgm_connection_get_status(connection, &session, &error);
            if (status) goto done;
            if (!session.carrier_retained && capacity.pinned_session_count + capacity.active_request_count + opening >= capacity.executor_worker_count) {
                status = PGM_STATUS_BUSY; bridge_message = "Carrier reserved for connection startup"; goto done;
            }
        }
    }
    switch (op) {
    case OPEN:
    case RESTORE_OPEN:
    case RESTORE_FINISH: {
        uint32_t create = get(&r), workers = get(&r);
        char *path = string(&r);
        uint32_t storage = get(&r);
        char *name = string(&r), *test_channel = string(&r);
        uint32_t sampling_interval = get(&r);
        pgm_instance_options o = PGM_INSTANCE_OPTIONS_INIT;
        const pgm_setting settings[] = {
            {"shared_buffers", "24MB"}, {"max_connections", "16"},
            {"max_worker_processes", "4"}, {"max_parallel_workers", "0"},
            {"autovacuum", "off"}, {"max_wal_senders", "0"},
            {"max_logical_replication_workers", "0"}, {"wal_level", "minimal"}
        };
        for (unsigned i = 0; i < SLOT_COUNT; i++) if (handles[i].kind == INSTANCE) status = PGM_STATUS_BUSY;
        if (!r.failed && !status && create <= 1 && storage <= 1 && workers >= 2 && workers <= 4 &&
            sampling_interval <= 60000 && r.offset == r.size) {
            pgm_instance *instance = NULL;
            if (op == RESTORE_FINISH && (!restoring || !restore_complete || create)) {
                r.failed = true; free(path); free(name); free(test_channel); break;
            }
            if (storage) {
                char *persistent_path = strdup("/persistent/database");
                if (!persistent_path) {
                    status = PGM_STATUS_OUT_OF_MEMORY;
                    bridge_message = "OPFS database path allocation failed";
                    free(path); free(name); free(test_channel); break;
                }
#ifndef POSTGAMMA_WASM_TESTING
                test_channel[0] = 0;
#endif
                int code = op == RESTORE_FINISH ? 0 : postgamma_wasm_storage_mount(name, test_channel);
                if (code) {
                    status = code == EBUSY ? PGM_STATUS_BUSY : PGM_STATUS_IO_ERROR;
                    bridge_message = "OPFS storage could not be opened";
                    free(persistent_path); free(path); free(name); free(test_channel); break;
                }
                free(path); path = persistent_path;
            }
            char marker[4096];
            if (!path || path[0] != '/' || strlen(path) > 2048 ||
                snprintf(marker, sizeof(marker), "%s/postgamma.restore-incomplete", path) >= sizeof(marker)) {
                r.failed = true; free(path); free(name); free(test_channel); break;
            }
            if (op == RESTORE_OPEN) {
                struct stat st;
                if (!lstat(path, &st) || errno != ENOENT) {
                    status = PGM_STATUS_INVALID_ARGUMENT; bridge_message = "Restore requires a new empty destination";
                } else if (mkdir(path, 0700)) {
                    status = PGM_STATUS_IO_ERROR; bridge_message = "Could not create restore destination";
                } else {
                    int fd = open(marker, O_CREAT | O_EXCL | O_WRONLY, 0600);
                    if (fd < 0 || fsync(fd)) status = PGM_STATUS_IO_ERROR;
                    if (fd >= 0) close(fd);
                    if (!status) {database_path = strdup(path); restoring = true;}
                }
                free(path); free(name); free(test_channel); break;
            }
            if (op == RESTORE_FINISH) {
                if (strcmp(database_path, path)) {status = PGM_STATUS_INVALID_ARGUMENT; bridge_message = "Restore path changed";}
            } else if (access(marker, F_OK) == 0) {
                status = PGM_STATUS_INVALID_ARGUMENT; bridge_message = "Destination contains an incomplete restore";
            }
            if (status) {free(path); free(name); free(test_channel); break;}
            o.create = create; o.path = path; o.executor_worker_count = workers;
            o.executable_path = "/postgamma/bin/postgres"; o.resource_root = "/postgamma";
            o.settings = settings; o.setting_count = sizeof(settings) / sizeof(settings[0]);
            o.result_buffer_limit = PGW_RESULT_BUFFER_BYTES; o.maximum_value_size = PGW_MAXIMUM_VALUE_BYTES;
            o.log_callback = log_record;
            status = pgm_instance_open(&o, &instance, &error);
            if (!status && op == RESTORE_FINISH) {
                /* Keep failed native validation quarantined, even if the
                 * framing/checksums describe an invalid PostgreSQL cluster. */
                int fd = open(path, O_RDONLY | O_DIRECTORY);
                int code = fd < 0 ? errno : fsync(fd) || unlink(marker) || fsync(fd) ? errno : 0;
                if (fd >= 0) close(fd);
                if (code) {
                    int marker_fd = open(marker, O_CREAT | O_WRONLY, 0600);
                    if (marker_fd >= 0) {fsync(marker_fd); close(marker_fd);}
                    pgm_status closed = pgm_instance_close(instance, PGM_SHUTDOWN_FAST, 10000, NULL);
                    if (closed) abort(); /* Fatal cleanup owns the live module. */
                    status = PGM_STATUS_IO_ERROR; bridge_message = "Could not commit restored storage";
                } else restoring = false;
            }
            if (!status) {
                free(database_path); database_path = strdup(path);
                archive_ready = false;
                publish(&payload, slot, INSTANCE, NULL, instance);
            }
            else if (storage) {
                // Roll back the mount owned by this OPEN. The control Worker
                // still sends STOP to join the driver and release its Web Lock.
                rollback_error = postgamma_wasm_storage_close();
            }
        } else if (!status) r.failed = true;
        free(path); free(name); free(test_channel); break;
    }
    case CONNECT: {
        unsigned connections = 0, opening = 0;
        for (unsigned i = 0; i < SLOT_COUNT; i++) {
            if (handles[i].kind == CONNECTION || handles[i].kind == CONNECTING) connections++;
            if (handles[i].kind == CONNECTING) opening++;
        }
        if (connections >= PGW_MAX_CONNECTIONS) {
            status = PGM_STATUS_BUSY; bridge_message = "Maximum live connections reached"; break;
        }
        /* Reserve one carrier for each startup across yielded driver turns. */
        pgm_instance_telemetry capacity = PGM_INSTANCE_TELEMETRY_INIT;
        status = pgm_instance_get_telemetry(h->pointer, &capacity, &error);
        if (status) break;
        if (capacity.pinned_session_count + capacity.active_request_count + opening >= capacity.executor_worker_count) {
            status = PGM_STATUS_BUSY; bridge_message = "No carrier is available for a new connection"; break;
        }
        char *name = string(&r);
        if (!r.failed && r.offset == r.size) {
            pgm_connection_options o = PGM_CONNECTION_OPTIONS_INIT;
            pgm_connection *connection = NULL;
            o.user = "postgamma"; o.database = "postgres"; o.application_name = name;
            status = postgamma_public_connection_begin(h->pointer, &o, &connection, &error);
            if (!status) publish(&payload, slot, CONNECTING, h, connection);
        } else r.failed = true;
        free(name); break;
    }
    case CONNECT_POLL: {
        bool complete = false;
        pgm_connection *connection = h->pointer;
        if (r.offset != r.size) {r.failed = true; break;}
        h->progress_watching = false;
        status = postgamma_public_connection_step(&connection, &complete, &error);
        h->pointer = connection;
        /* Sample on the owning driver; the observer reads only atomic copies,
         * even if startup retirement later frees the connection or stalls. */
        atomic_store(&startup_slot, h - handles + 1);
        atomic_store(&startup_phase, connection == NULL ? 6 :
            complete ? 5 : connection->connect_error ?
            (connection->private_connection != NULL ? 3 : 4) :
            connection->connect_ticket != NULL ? 1 : 2);
        atomic_store(&startup_error, connection != NULL ? connection->connect_error : 0);
        atomic_store(&startup_backend, connection != NULL ? connection->backend_pid : 0);
        if (complete && status) retire(h);
        else if (!status && complete) {
            h->kind = CONNECTION;
            put(&payload, PGM_AVAILABILITY_END);
            put(&payload, h->kind); put(&payload, h - handles + 1); put(&payload, h->generation);
        } else if (!status) progress_again(h, &payload);
        break;
    }
    case PREPARE: {
        unsigned free_slots = 0;
        for (unsigned i = 0; i < SLOT_COUNT; i++)
            if (!handles[i].kind && handles[i].generation != UINT32_MAX) free_slots++;
        if (free_slots < 2) {status = PGM_STATUS_BUSY; break;}
        char *sql = string(&r);
        uint32_t count = get(&r), types[64];
        if (count > 64) r.failed = true;
        for (uint32_t i = 0; i < count && !r.failed; i++) types[i] = get(&r);
        if (!r.failed && r.offset == r.size) {
            pgm_prepare_options options = PGM_PREPARE_OPTIONS_INIT;
            pgm_statement *statement = NULL;
            pgm_request *request = NULL;
            options.parameter_type_oids = types; options.parameter_count = count;
            status = postgamma_public_statement_prepare_async(h->pointer, sql, strlen(sql),
                &options, &statement, &request, &error);
            if (!status) {
                publish(&payload, slot, STATEMENT, h, statement);
                publish(&payload, reserve(), REQUEST, slot, request);
            }
        } else r.failed = true;
        free(sql); break;
    }
    case STATEMENT_INFO: {
        if (r.offset != r.size) {r.failed = true; break;}
        pgm_statement_description description = PGM_STATEMENT_DESCRIPTION_INIT;
        status = pgm_statement_describe(h->pointer, &description, &error);
        if (status) break;
        put(&payload, description.parameter_count);
        for (size_t i = 0; !status && i < description.parameter_count; i++) {
            uint32_t type = 0;
            status = pgm_statement_parameter_type(h->pointer, i, &type, &error);
            put(&payload, type);
        }
        put(&payload, description.column_count);
        for (size_t i = 0; !status && i < description.column_count; i++) {
            pgm_column col = PGM_COLUMN_INIT;
            status = pgm_statement_column(h->pointer, i, &col, &error);
            text(&payload, col.name); put(&payload, col.type_oid); put(&payload, col.format);
            put(&payload, col.table_oid); put(&payload, col.table_column);
            put(&payload, col.type_size); put(&payload, col.type_modifier);
        }
        break;
    }
    case STATEMENT_CLOSE: {
        if (r.offset != r.size) {r.failed = true; break;}
        if (children(h)) {status = PGM_STATUS_BUSY; break;}
        pgm_request *request = NULL;
        status = postgamma_public_statement_close_async(h->pointer, &request, &error);
        if (!status) {
            h->closing = true;
            put(&payload, request != NULL);
            if (request) publish(&payload, slot, REQUEST, h, request);
        }
        break;
    }
    case STATEMENT_FREE: {
        if (r.offset != r.size) {r.failed = true; break;}
        pgm_statement *statement = h->pointer;
        if (children(h) || (!statement->remote_closed && !statement->connection->failed)) {
            status = PGM_STATUS_BUSY; break;
        }
        status = pgm_statement_close(statement, &error);
        if (!status) retire(h);
        break;
    }
    case EXECUTE:
    case STATEMENT_EXECUTE:
    case SCRIPT: {
        char *sql = op == STATEMENT_EXECUTE ? NULL : string(&r);
        pgm_execute_options o = PGM_EXECUTE_OPTIONS_INIT;
        uint32_t format = op == SCRIPT ? 0 : get(&r), chunk_rows = get(&r);
        uint32_t count = op == SCRIPT ? 0 : get(&r);
        pgm_parameter parameters[64];
        if (count > 64 || format > 1 || chunk_rows > PGW_MAXIMUM_CHUNK_ROWS) r.failed = true;
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
            o.result_buffer_limit = PGW_RESULT_BUFFER_BYTES;
            if (op == SCRIPT) {
                pgm_script_options script = PGM_SCRIPT_OPTIONS_INIT;
                script.delivery_mode = o.delivery_mode;
                script.target_chunk_rows = chunk_rows;
                script.result_buffer_limit = o.result_buffer_limit;
                status = pgm_execute_script_async(h->pointer, sql, strlen(sql), &script, &request, &error);
            } else if (op == STATEMENT_EXECUTE)
                status = pgm_statement_execute_async(h->pointer, &o, &request, &error);
            else status = pgm_execute_async_ex(h->pointer, sql, strlen(sql), &o, &request, &error);
            if (!status) publish(&payload, slot, REQUEST, h, request);
        } else r.failed = true;
        free(sql); break;
    }
    case NEXT: {
        if (r.offset != r.size) { r.failed = true; break; }
        pgm_result *result = NULL;
        pgm_availability availability = PGM_AVAILABILITY_AGAIN;
        watch_begin(h);
        status = pgm_request_next_result(h->pointer, 0, &result, &availability, &error);
        if (!status && error) status = pgm_error_status(error);
        h->watching = !status && availability == PGM_AVAILABILITY_AGAIN;
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
    case DISCONNECT:
        if (r.offset != r.size) { r.failed = true; break; }
        if (children(h)) { status = PGM_STATUS_BUSY; break; }
        h->closing = true; h->watching = h->progress_watching = false;
        if (!h->close_deadline_ms) h->close_deadline_ms = emscripten_get_now() + 10000;
        status = pgm_connection_close(h->pointer, 0, &error);
        if (status == PGM_STATUS_TIMEOUT && emscripten_get_now() < h->close_deadline_ms) {
            pgm_error_free(error); error = NULL; status = PGM_STATUS_OK;
            progress_again(h, &payload);
        } else {
            h->close_deadline_ms = 0; // A failed close retains the handle for a new attempt.
            if (!status) {retire(h); put(&payload, PGM_AVAILABILITY_END);}
        }
        break;
    case CLOSE:
        if (r.offset != r.size) { r.failed = true; break; }
        if (children(h)) { status = PGM_STATUS_BUSY; break; }
        status = pgm_instance_close(h->pointer, PGM_SHUTDOWN_FAST, 10000, &error);
        if (!status) {
            archive_ready = true;
            pgm_event_free(pending_event); pending_event = NULL;
            event_notified = false;
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
    case RELEASE:
    case COPY_CLOSE:
    case COPY_ABORT: {
        bool complete = false;
        bool closing = op == RELEASE || op == COPY_CLOSE;
        double *deadline = closing ? &h->close_deadline_ms : &h->cancel_deadline_ms;
        if (r.offset != r.size) { r.failed = true; break; }
        if (closing && children(h)) {status = PGM_STATUS_BUSY; break;}
        if (closing) {h->closing = true; h->watching = false;}
        h->progress_watching = false;
        if (!*deadline) *deadline = emscripten_get_now() + 10000;
        if (op == CANCEL) status = postgamma_public_request_cancel_step(h->pointer, &complete, &error);
        else if (op == RELEASE) status = postgamma_public_request_release_step(h->pointer, &complete, &error);
        else if (op == COPY_CLOSE) status = postgamma_public_copy_close_step(h->pointer, &complete, &error);
        else status = postgamma_public_copy_abort_step(h->pointer, &complete, &error);
        if (!status && !complete && emscripten_get_now() >= *deadline) {
            status = PGM_STATUS_TIMEOUT; bridge_message = "Lifecycle operation timed out; handle retained for retry";
        }
        if (status || complete) {
            *deadline = 0;
            if (!status) {if (closing) retire(h); put(&payload, PGM_AVAILABILITY_END);}
        } else progress_again(h, &payload);
        break;
    }
    case COPY_READ: {
        uint32_t capacity = get(&r);
        unsigned char buffer[PGW_COPY_CHUNK_BYTES];
        size_t produced = 0;
        pgm_io_state progress = PGM_IO_AGAIN;
        if (r.offset != r.size || capacity == 0 || capacity > sizeof(buffer)) { r.failed = true; break; }
        watch_begin(h);
        status = pgm_copy_read(h->pointer, buffer, capacity, &produced, &progress, &error);
        h->watching = !status && progress == PGM_IO_AGAIN;
        put(&payload, progress); value(&payload, buffer, produced, false); break;
    }
    case COPY_WRITE: {
        size_t size, consumed = 0;
        const void *data = blob(&r, &size);
        pgm_io_state progress = PGM_IO_AGAIN;
        if (r.failed || r.offset != r.size || size == 0 || size > PGW_COPY_CHUNK_BYTES) { r.failed = true; break; }
        watch_begin(h);
        status = pgm_copy_write(h->pointer, data, size, &consumed, &progress, &error);
        h->watching = !status && progress == PGM_IO_AGAIN;
        put(&payload, progress); put(&payload, consumed); break;
    }
    case COPY_FINISH: {
        pgm_io_state progress = PGM_IO_AGAIN;
        if (r.offset != r.size) { r.failed = true; break; }
        watch_begin(h);
        status = pgm_copy_finish(h->pointer, NULL, 0, &progress, &error);
        h->watching = !status && progress == PGM_IO_AGAIN;
        put(&payload, progress); break;
    }
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
    case EVENTS:
        if (r.offset != r.size) {r.failed = true; break;}
        status = marshal_events(&payload, &error);
        break;
    case ARCHIVE_READ:
    case ARCHIVE_WRITE: {
        int code = 0;
        for (unsigned i = 0; i < SLOT_COUNT; i++) if (handles[i].kind) code = EBUSY;
        if (!code) code = op == ARCHIVE_READ ?
            (r.offset == r.size ? archive_read(&payload) : EINVAL) : archive_write(&r);
        if (code) {status = PGM_STATUS_IO_ERROR; bridge_message = strerror(code);}
        break;
    }
    case STOP:
        if (r.offset != r.size) { r.failed = true; break; }
        for (unsigned i = 0; i < SLOT_COUNT; i++) if (handles[i].kind) status = PGM_STATUS_BUSY;
        if (!status) archive_reset();
        if (!status && postgamma_wasm_storage_close()) {
            status = PGM_STATUS_IO_ERROR; bridge_message = "OPFS storage close failed";
        }
        stop = !status; break;
    default: r.failed = true;
    }
done:
    if (r.failed) status = PGM_STATUS_INVALID_ARGUMENT;
    if (payload.failed) status = PGM_STATUS_OUT_OF_MEMORY;
    put(&reply, WIRE_VERSION); put(&reply, sequence); put(&reply, op); put(&reply, status);
    for (unsigned i = 1; i <= 17; i++) {
        const char *field = error ? pgm_error_field(error, i) : i == PGM_DIAG_MESSAGE ? bridge_message : NULL;
        if (i == PGM_DIAG_DETAIL && rollback_error) {
            const char *prefix = "OPFS startup rollback failed: ", *cause = strerror(rollback_error);
            size_t existing = field ? strlen(field) : 0;
            put(&reply, existing + (existing ? 2 : 0) + strlen(prefix) + strlen(cause));
            if (existing) {bytes(&reply, field, existing); bytes(&reply, "; ", 2);}
            bytes(&reply, prefix, strlen(prefix)); bytes(&reply, cause, strlen(cause));
        } else text(&reply, field);
    }
    if (!status) bytes(&reply, payload.data, payload.size);
    free(payload.data); pgm_error_free(error);
    return stop;
}
static void *run(void *unused)
{
    (void)unused;
    for (;;) {
        atomic_store(&driver_phase, 1); /* readiness */
        uint32_t expected = postgamma_platform_notification_sequence();
        atomic_fetch_add_explicit(&wake_count, 1, memory_order_relaxed);
        double wait_ms = notify_ready();
        notify_events();
        if (atomic_load_explicit(&state, memory_order_acquire) != 1) {
            atomic_store(&driver_phase, 2); /* notification wait */
            if (postgamma_platform_notification_wait(expected, wait_ms) < 0) abort();
            continue;
        }
        atomic_store(&driver_phase, 3); /* native command */
        bool stop = command();
        bool failed = reply.failed;
        atomic_store_explicit(&state, failed ? -1 : 2, memory_order_release);
        atomic_store(&driver_phase, 4); /* reply publication */
        notify_control(deliver_event, NULL);
        if (stop || failed) {atomic_store(&driver_phase, 5); return NULL;}
    }
}
EMSCRIPTEN_KEEPALIVE void *postgamma_wasm_alloc(unsigned size) { return size <= WIRE_LIMIT ? malloc(size) : NULL; }
EMSCRIPTEN_KEEPALIVE void postgamma_wasm_free(void *p) { free(p); }
EMSCRIPTEN_KEEPALIVE int postgamma_wasm_start(void)
{
    if (started) return EALREADY;
    notifications = em_proxying_queue_create();
    if (!notifications) return ENOMEM;
    int result = pthread_create(&driver, NULL, run, NULL);
    if (!result) started = true;
    else { em_proxying_queue_destroy(notifications); notifications = NULL; }
    return result;
}
EMSCRIPTEN_KEEPALIVE int postgamma_wasm_submit(void *data, unsigned size)
{
    if (!started || atomic_load(&state) != 0) return EBUSY;
    if (!data || size < 12 || size > WIRE_LIMIT) return EINVAL;
    input = data; input_size = size;
    atomic_store_explicit(&state, 1, memory_order_release);
    postgamma_platform_notify();
    return 0;
}
EMSCRIPTEN_KEEPALIVE int postgamma_wasm_poll(void) {
    return atomic_load_explicit(&state, memory_order_acquire);
}
EMSCRIPTEN_KEEPALIVE unsigned postgamma_wasm_wake_count(void) { return atomic_load(&wake_count); }
/* Healthy, quiescent inspection only: mallinfo takes the allocator lock.
 * Failure snapshots deliberately use the separate atomic diagnostics below. */
EMSCRIPTEN_KEEPALIVE unsigned postgamma_wasm_allocator_live_bytes(void)
{
    return (unsigned) mallinfo().uordblks;
}
EMSCRIPTEN_KEEPALIVE unsigned postgamma_wasm_diagnostic(unsigned field)
{
    switch (field) {
    case 0: return atomic_load(&driver_phase);
    case 1: return atomic_load(&active_sequence);
    case 2: return atomic_load(&active_operation);
    case 3: return postgamma_platform_notification_sequence();
    case 4: return postgamma_platform_notification_address();
    case 5: return (uintptr_t) driver;
    case 6: return (unsigned) emscripten_get_now() - atomic_load(&command_started);
    case 8: return atomic_load(&startup_slot);
    case 9: return atomic_load(&startup_phase);
    case 10: return atomic_load(&startup_error);
    case 11: return atomic_load(&startup_backend);
#ifdef POSTGAMMA_WASM_TESTING
    case 7: return atomic_load(&diagnostic_hold);
#endif
    default: return 0;
    }
}
EMSCRIPTEN_KEEPALIVE unsigned postgamma_wasm_thread_diagnostic(unsigned thread, unsigned field)
{
    return postgamma_thread_diagnostic(thread, field);
}
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
    if (!result) {
        started = false;
        atomic_store(&state, 3);
        em_proxying_queue_destroy(notifications);
        notifications = NULL;
    }
    return result;
}
