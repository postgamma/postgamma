/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "postgamma/private/public_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Fake only private I/O. Public COPY/request/lease ownership stays real. */
static size_t unread;
static unsigned reads, drains;
static bool copy_released;
bool public_instance_reentrant(const pgm_instance *instance) {(void) instance; return false;}
void postgamma_public_dispatch_progress_events(pgm_request *request) {(void) request;}
void postgamma_public_event_forget_request(pgm_request *request) {(void) request;}
void postgamma_public_event_notify_connection(pgm_connection *connection, uint32_t events)
{(void) connection; (void) events;}

PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_read(
    PostgammaPrivateLibpqOperation *operation, void *buffer, size_t capacity,
    size_t *produced, PostgammaPrivateCopyProgress *progress)
{
    (void) operation; (void) buffer;
    assert(capacity <= 8192 && !copy_released); reads++;
    *produced = unread < capacity ? unread : capacity; unread -= *produced;
    *progress = *produced ? POSTGAMMA_PRIVATE_COPY_PROGRESS : POSTGAMMA_PRIVATE_COPY_END;
    return POSTGAMMA_PRIVATE_LIBPQ_OK;
}
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_release(PostgammaPrivateLibpqOperation *operation)
{(void) operation; assert(unread == 0 && !copy_released); copy_released = true; return POSTGAMMA_PRIVATE_LIBPQ_OK;}
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_direction(
    PostgammaPrivateLibpqOperation *operation, PostgammaPrivateCopyDirection *direction)
{(void) operation; *direction = copy_released ? POSTGAMMA_PRIVATE_COPY_NONE : POSTGAMMA_PRIVATE_COPY_OUT; return POSTGAMMA_PRIVATE_LIBPQ_OK;}
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_progress(
    PostgammaPrivateLibpqOperation *operation, PostgammaPrivateLibpqProgress *progress,
    PostgammaPrivateOwnedResult **result)
{
    (void) operation; assert(copy_released); *result = NULL;
    *progress = ++drains == 3 ? POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_COMPLETE : POSTGAMMA_PRIVATE_LIBPQ_PROGRESS_WAITING;
    return POSTGAMMA_PRIVATE_LIBPQ_OK;
}
void postgamma_private_libpq_operation_free(PostgammaPrivateLibpqOperation **operation)
{assert(drains == 3); *operation = NULL;}
void postgamma_private_libpq_result_free(PostgammaPrivateOwnedResult *result) {assert(result == NULL);}
const char *postgamma_private_libpq_status_name(PostgammaPrivateLibpqStatus status) {(void) status; return "fixture";}
PostgammaPrivateLibpqStatus postgamma_private_libpq_operation_wait(PostgammaPrivateLibpqOperation *op, int64_t deadline)
{(void) op; (void) deadline; assert(!"synchronous wait"); return POSTGAMMA_PRIVATE_LIBPQ_TIMEOUT;}
PostgammaPrivateLibpqStatus postgamma_private_libpq_cancel(PostgammaPrivateLibpqConnection *connection, uint64_t generation)
{(void) connection; (void) generation; assert(!"synchronous cancellation"); return POSTGAMMA_PRIVATE_LIBPQ_OK;}
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_finish(PostgammaPrivateLibpqOperation *op,
    const char *message, PostgammaPrivateCopyProgress *progress)
{(void) op; (void) message; (void) progress; assert(!"COPY IN"); return POSTGAMMA_PRIVATE_LIBPQ_OK;}
PostgammaPrivateLibpqStatus postgamma_private_libpq_copy_pump(PostgammaPrivateLibpqOperation *op,
    PostgammaPrivateCopyProgress *progress)
{(void) op; (void) progress; assert(!"unexpected pump"); return POSTGAMMA_PRIVATE_LIBPQ_OK;}

int main(void)
{
    pgm_instance instance = {.magic = PGM_INSTANCE_MAGIC, .owner_pid = getpid()};
    pgm_connection connection = {.magic = PGM_CONNECTION_MAGIC, .owner_pid = getpid(), .instance = &instance};
    pgm_request *request = calloc(1, sizeof(*request));
    pgm_copy *copy = calloc(1, sizeof(*copy));
    PostgammaPublicResultLease *lease = calloc(1, sizeof(*lease));
    bool done = false;
    assert(request && copy && lease);
    assert(postgamma_mutex_create(&connection.mutex) == 0);
    assert(postgamma_mutex_create(&request->mutex) == 0);
    assert(postgamma_mutex_create(&copy->mutex) == 0);
    instance.active_request_count = 1;
    connection.active_request = request;
    request->magic = PGM_REQUEST_MAGIC; request->owner_pid = getpid();
    request->connection = &connection; request->state = PGM_REQUEST_RUNNING;
    request->free_pending = request->cancel_requested = request->cancel_dispatched = true;
    request->cancel_next_ns = UINT64_MAX; request->result_lease = lease; request->active_copy = copy;
    copy->magic = PGM_COPY_MAGIC; copy->owner_pid = getpid(); copy->request = request;
    copy->direction = POSTGAMMA_PRIVATE_COPY_OUT; copy->state = POSTGAMMA_PUBLIC_COPY_ACTIVE; copy->lease = lease;
    atomic_init(&copy->io_owner_active, false);
    lease->owner = request; atomic_init(&lease->references, 3); atomic_init(&lease->outstanding, true);
    unread = 20000;
    for (unsigned step = 0; !done; step++) {
        unsigned previous_reads = reads, previous_drains = drains;
        pgm_error *error = NULL;
        assert(step < 10);
        assert(postgamma_public_copy_close_step(copy, &done, &error) == PGM_STATUS_OK && error == NULL);
        assert(reads - previous_reads <= 1 && drains - previous_drains <= 1);
        if (!done) assert(connection.active_request == request && lease->owner == request);
    }
    assert(reads == 4 && drains == 3 && connection.active_request == NULL);
    assert(lease->owner == NULL && !atomic_load(&lease->outstanding) && atomic_load(&lease->references) == 1);
    result_lease_release(lease);
    assert(postgamma_mutex_destroy(connection.mutex) == 0);
    puts("lifecycle COPY: bounded drain, deferred request free and surviving lease passed");
    return 0;
}
