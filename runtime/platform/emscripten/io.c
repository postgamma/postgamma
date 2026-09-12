/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/io_platform.h"
#include "postgamma/thread_platform.h"
#include "postgamma/thread_runtime.h"

#include <emscripten/threading.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define PIPE_CAPACITY 4096
#define NS_PER_MS UINT64_C(1000000)

/* Both endpoints share this lock: WasmFS pipe endpoints do not share a lock. */
typedef struct PostgammaPipe
{
    int reader, writer;
    size_t head, length;
    unsigned char bytes[PIPE_CAPACITY];
    struct PostgammaPipe *next;
} PostgammaPipe;

static pthread_mutex_t pipes_lock = PTHREAD_MUTEX_INITIALIZER;
static PostgammaPipe *pipes;
static _Atomic uint32_t epoch;

void
postgamma_platform_notify(void)
{
    atomic_fetch_add_explicit(&epoch, 1, memory_order_release);
    emscripten_futex_wake((void *) &epoch, INT_MAX);
}

static PostgammaPipe *
find_pipe(int fd)
{
    if (fd < 0) return NULL;
    for (PostgammaPipe *p = pipes; p; p = p->next)
        if (p->reader == fd || p->writer == fd)
            return p;
    return NULL;
}

size_t
postgamma_platform_pipe_count(void)
{
    size_t count = 0;
    pthread_mutex_lock(&pipes_lock);
    for (PostgammaPipe *p = pipes; p; p = p->next)
        count++;
    pthread_mutex_unlock(&pipes_lock);
    return count;
}

int
postgamma_platform_pipe(int descriptors[2])
{
    PostgammaPipe *p;
    int saved;
    if (descriptors == NULL) { errno = EFAULT; return -1; }
    p = calloc(1, sizeof(*p));
    if (!p) { errno = ENOMEM; return -1; }
    /* Reserve real descriptors so SDK file allocation cannot alias our IDs. */
    p->reader = open("/dev/null", O_RDONLY);
    p->writer = p->reader < 0 ? -1 : open("/dev/null", O_WRONLY);
    if (p->writer < 0) {
        saved = errno;
        if (p->reader >= 0) close(p->reader);
        free(p);
        errno = saved;
        return -1;
    }
    pthread_mutex_lock(&pipes_lock);
    p->next = pipes;
    pipes = p;
    descriptors[0] = p->reader;
    descriptors[1] = p->writer;
    pthread_mutex_unlock(&pipes_lock);
    return 0;
}

uint32_t
postgamma_platform_notification_sequence(void)
{
    return atomic_load_explicit(&epoch, memory_order_acquire);
}

int
postgamma_platform_notification_wait(uint32_t expected, double milliseconds)
{
    int status;
    if (!postgamma_platform_thread_can_block()) { errno = EWOULDBLOCK; return -1; }
    status = emscripten_futex_wait((void *) &epoch, expected, milliseconds);
    if (status == 0 || status == -EWOULDBLOCK || status == -ETIMEDOUT)
        return 0;
    errno = -status;
    return -1;
}

ssize_t
postgamma_platform_read(int fd, void *buffer, size_t size)
{
    for (;;) {
        uint32_t expected = atomic_load_explicit(&epoch, memory_order_acquire);
        pthread_mutex_lock(&pipes_lock);
        PostgammaPipe *p = find_pipe(fd);
        if (!p) { pthread_mutex_unlock(&pipes_lock); return read(fd, buffer, size); }
        if (fd != p->reader) { pthread_mutex_unlock(&pipes_lock); errno = EBADF; return -1; }
        size_t copied = size < p->length ? size : p->length;
        if (copied || !size || p->writer < 0) {
            for (size_t i = 0; i < copied; i++)
                ((unsigned char *) buffer)[i] = p->bytes[(p->head + i) % PIPE_CAPACITY];
            p->head = (p->head + copied) % PIPE_CAPACITY;
            p->length -= copied;
            pthread_mutex_unlock(&pipes_lock);
            if (copied) postgamma_platform_notify();
            return (ssize_t) copied;
        }
        int flags = fcntl(fd, F_GETFL);
        pthread_mutex_unlock(&pipes_lock);
        if (flags < 0) return -1;
        if (flags & O_NONBLOCK) { errno = EAGAIN; return -1; }
        if (postgamma_platform_notification_wait(expected, __builtin_inf()) < 0) return -1;
    }
}

ssize_t
postgamma_platform_write(int fd, const void *buffer, size_t size)
{
    for (;;) {
        uint32_t expected = atomic_load_explicit(&epoch, memory_order_acquire);
        pthread_mutex_lock(&pipes_lock);
        PostgammaPipe *p = find_pipe(fd);
        if (!p) { pthread_mutex_unlock(&pipes_lock); return write(fd, buffer, size); }
        if (fd != p->writer) { pthread_mutex_unlock(&pipes_lock); errno = EBADF; return -1; }
        if (p->reader < 0) { pthread_mutex_unlock(&pipes_lock); errno = EPIPE; return -1; }
        size_t space = PIPE_CAPACITY - p->length;
        size_t copied = size < space ? size : space;
        /* Writes up to PIPE_CAPACITY are atomic, including all wake writes. */
        if (size <= PIPE_CAPACITY && size > space) copied = 0;
        if (copied || !size) {
            for (size_t i = 0; i < copied; i++)
                p->bytes[(p->head + p->length + i) % PIPE_CAPACITY] = ((const unsigned char *) buffer)[i];
            p->length += copied;
            pthread_mutex_unlock(&pipes_lock);
            if (copied) postgamma_platform_notify();
            return (ssize_t) copied;
        }
        int flags = fcntl(fd, F_GETFL);
        pthread_mutex_unlock(&pipes_lock);
        if (flags < 0) return -1;
        if (flags & O_NONBLOCK) { errno = EAGAIN; return -1; }
        if (postgamma_platform_notification_wait(expected, __builtin_inf()) < 0) return -1;
    }
}

int
postgamma_platform_close(int fd)
{
    int status;
    if (fd < 0) { errno = EBADF; return -1; }
    pthread_mutex_lock(&pipes_lock);
    PostgammaPipe **link = &pipes;
    while (*link && (*link)->reader != fd && (*link)->writer != fd)
        link = &(*link)->next;
    if (*link) {
        PostgammaPipe *p = *link;
        if (p->reader == fd) p->reader = -1;
        else p->writer = -1;
        if (p->reader < 0 && p->writer < 0) { *link = p->next; free(p); }
    }
    status = close(fd);
    pthread_mutex_unlock(&pipes_lock);
    postgamma_platform_notify();
    return status;
}

int
postgamma_platform_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    uint64_t start = postgamma_monotonic_now_ns();
    uint64_t duration = timeout < 0 ? 0 : (uint64_t) timeout * NS_PER_MS;
    for (;;) {
        uint32_t expected = atomic_load_explicit(&epoch, memory_order_acquire);
        int ready = 0;
        pthread_mutex_lock(&pipes_lock);
        for (nfds_t i = 0; i < count; i++) {
            struct pollfd *f = &fds[i];
            f->revents = 0;
            if (f->fd < 0) continue;
            PostgammaPipe *p = find_pipe(f->fd);
            if (!p) {
                if (poll(f, 1, 0) < 0) { pthread_mutex_unlock(&pipes_lock); return -1; }
            } else if (f->fd == p->reader) {
                if (p->length) f->revents |= f->events & POLLIN;
                if (p->writer < 0) f->revents |= POLLHUP;
            } else {
                if (p->length < PIPE_CAPACITY) f->revents |= f->events & POLLOUT;
                if (p->reader < 0) f->revents |= POLLERR;
            }
            ready += f->revents != 0;
        }
        pthread_mutex_unlock(&pipes_lock);
        if (ready || timeout == 0) return ready;
        uint64_t elapsed = postgamma_monotonic_now_ns() - start;
        if (timeout >= 0 && elapsed >= duration) return 0;
        double milliseconds = timeout < 0 ? __builtin_inf() : (double) (duration - elapsed) / NS_PER_MS;
        if (postgamma_platform_notification_wait(expected, milliseconds) < 0) return -1;
        /* Recheck all predicates after mismatch, wake, suspension or timeout. */
    }
}
