/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
/* Included by sql.c: all callbacks execute synchronously on its owning driver.
 * The tool runs on one separately budgeted pthread, with a bounded channel. */
typedef struct LogicalIO {
    Writer output;
    const unsigned char *input;
    size_t size, consumed;
    bool end;
} LogicalIO;

static pgm_io_state logical_write(void *argument, const void *data, size_t size, size_t *consumed)
{
    LogicalIO *io = argument;
    bytes(&io->output, data, size);
    *consumed = io->output.failed ? 0 : size;
    return PGM_IO_PROGRESS;
}
static pgm_io_state logical_read(void *argument, void *data, size_t capacity, size_t *produced)
{
    LogicalIO *io = argument;
    size_t remaining = io->size - io->consumed;
    *produced = remaining < capacity ? remaining : capacity;
    if (*produced) {
        memcpy(data, io->input + io->consumed, *produced);
        io->consumed += *produced;
        return PGM_IO_PROGRESS;
    }
    return io->end ? PGM_IO_END : PGM_IO_AGAIN;
}

static pgm_status logical_command(unsigned op, Handle *h, Handle *slot, Reader *r,
                                 Writer *payload, pgm_error **error, const char **message)
{
    if (op == LOGICAL_DUMP || op == LOGICAL_RESTORE) {
        unsigned flags = get(r), sessions = 0, opening = 0;
        char *database = string(r), *user = string(r);
        if (r->failed || r->offset != r->size) {free(database); free(user); r->failed = true; return 0;}
        for (unsigned i = 0; i < SLOT_COUNT; i++) {
            if (handles[i].kind == LOGICAL) {
                free(database); free(user); *message = "A logical operation is already active"; return PGM_STATUS_BUSY;
            }
            if (handles[i].kind == CONNECTION || handles[i].kind == CONNECTING) sessions++;
            if (handles[i].kind == CONNECTING) opening++;
        }
        pgm_instance_telemetry capacity = PGM_INSTANCE_TELEMETRY_INIT;
        pgm_status status = pgm_instance_get_telemetry(h->pointer, &capacity, error);
        if (!status && (sessions >= PGW_MAX_CONNECTIONS ||
            capacity.pinned_session_count + capacity.active_request_count + opening >= capacity.executor_worker_count)) {
            status = PGM_STATUS_BUSY; *message = "Logical backup/restore requires one available SQL session and carrier";
        }
        LogicalIO *io = status ? NULL : calloc(1, sizeof(*io));
        pgm_operation *operation = NULL;
        if (!status && !io) status = PGM_STATUS_INTERNAL_ERROR;
        if (!status && op == LOGICAL_DUMP) {
            pgm_logical_dump_options options = PGM_LOGICAL_DUMP_OPTIONS_INIT;
            options.flags = flags; options.database = database; options.user = user;
            options.progress_quantum = PGW_COPY_CHUNK_BYTES;
            options.write = logical_write; options.user_data = io;
            status = pgm_instance_logical_dump_async(h->pointer, &options, &operation, error);
        } else if (!status) {
            pgm_logical_restore_options options = PGM_LOGICAL_RESTORE_OPTIONS_INIT;
            options.flags = flags; options.database = database; options.user = user;
            options.progress_quantum = PGW_COPY_CHUNK_BYTES;
            options.read = logical_read; options.user_data = io;
            status = pgm_instance_logical_restore_async(h->pointer, &options, &operation, error);
        }
        free(database); free(user);
        if (status) free(io);
        else publish(payload, slot, LOGICAL, h, operation);
        return status;
    }
    pgm_operation *operation = h->pointer;
    LogicalIO *io = operation->stream_user_data;
    if (op == LOGICAL_STEP) {
        unsigned end = get(r);
        size_t size;
        const void *input = blob(r, &size);
        if (r->failed || r->offset != r->size || end > 1 || size > PGW_COPY_CHUNK_BYTES ||
            (end && size) || (operation->kind == PGM_OPERATION_LOGICAL_DUMP && (size || end))) {
            r->failed = true; return 0;
        }
        io->input = input; io->size = size; io->consumed = 0; io->end = end;
        io->output.size = 0; io->output.failed = false;
        h->watching = false;
        pgm_operation_state state = PGM_OPERATION_PENDING;
        pgm_status status = pgm_operation_progress(operation, &state, error);
        io->input = NULL; /* No borrowed command bytes survive this call. */
        if (!status) {
            put(payload, state); put(payload, io->consumed);
            value(payload, io->output.data, io->output.size, false);
            h->watching = state < PGM_OPERATION_COMPLETED && !io->consumed && !io->output.size;
        }
        return status;
    }
    if (r->offset != r->size) {r->failed = true; return 0;}
    h->watching = h->progress_watching = false;
    if (!h->closing) {
        h->closing = true;
        h->close_deadline_ms = emscripten_get_now() + 30000;
        pgm_status status = pgm_operation_cancel(operation, error);
        if (status && status != PGM_STATUS_BUSY) return status;
        pgm_error_free(*error); *error = NULL;
    }
    /* A failed operation can still own a worker. Retain its callback storage
     * and reservation until completion, including cancellation during startup. */
    if (postgamma_mutex_lock(operation->mutex)) return PGM_STATUS_INTERNAL_ERROR;
    bool complete = operation->logical_worker == NULL;
    (void) postgamma_mutex_unlock(operation->mutex);
    if (!complete) {
        if (emscripten_get_now() >= h->close_deadline_ms) {
            *message = "Logical tool cleanup timed out; instance must be discarded";
            return PGM_STATUS_TIMEOUT;
        }
        progress_again(h, payload); return 0;
    }
    pgm_operation_free(operation); free(io->output.data); free(io);
    retire(h); put(payload, PGM_AVAILABILITY_END); return 0;
}
