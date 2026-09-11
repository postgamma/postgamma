/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

struct capture {
    char bytes[128];
    size_t position, length;
    unsigned reads, writes, seeks, closes;
    bool fail;
};

static ssize_t
capture_write(void *opaque, const char *data, size_t size)
{
    struct capture *c = opaque;
    c->writes++;
    if (c->fail || size > sizeof(c->bytes) - c->position) {
        errno = EIO;
        return -1;
    }
    memcpy(c->bytes + c->position, data, size);
    c->position += size;
    if (c->position > c->length)
        c->length = c->position;
    return (ssize_t) size;
}

static ssize_t
capture_read(void *opaque, char *data, size_t size)
{
    struct capture *c = opaque;
    c->reads++;
    if (size > c->length - c->position)
        size = c->length - c->position;
    memcpy(data, c->bytes + c->position, size);
    c->position += size;
    return (ssize_t) size;
}

static int
capture_seek(void *opaque, off_t *offset, int whence)
{
    struct capture *c = opaque;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t) c->position : (off_t) c->length;
    c->seeks++;
    if (*offset < -base || *offset > (off_t) c->length - base) {
        errno = EINVAL;
        return -1;
    }
    *offset += base;
    c->position = (size_t) *offset;
    return 0;
}

static int
capture_close(void *opaque)
{
    ((struct capture *) opaque)->closes++;
    return 0;
}

static void
streams(void)
{
    char *data = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&data, &length);
    struct capture c = {0}, failed = {.fail = true}, closing = {0};
    cookie_io_functions_t callbacks = {
        .read = capture_read, .write = capture_write,
        .seek = capture_seek, .close = capture_close,
    };
    char readback[16] = {0};
    int flush_rc = 0, flush_errno = 0, error_flag = 0, close_rc = 0;

    CHECK(stream != NULL);
    if (stream) {
        CHECK(fwrite("alpha", 1, 5, stream) == 5);
        CHECK(fflush(stream) == 0);
        CHECK(length == 5 && memcmp(data, "alpha\0", 6) == 0);
        CHECK(fseeko(stream, 0, SEEK_SET) == 0);
        CHECK(fwrite("A", 1, 1, stream) == 1);
        CHECK(fseeko(stream, 0, SEEK_END) == 0);
        CHECK(fwrite("-beta", 1, 5, stream) == 5);
        CHECK(fclose(stream) == 0);
        CHECK(length == 10 && memcmp(data, "Alpha-beta\0", 11) == 0);
        free(data);
    }
    stream = fopencookie(&c, "w+", callbacks);
    CHECK(stream != NULL);
    if (stream) {
        CHECK(fwrite("cookie", 1, 6, stream) == 6);
        CHECK(fflush(stream) == 0);
        CHECK(c.length == 6 && c.writes > 0);
        CHECK(fseeko(stream, 0, SEEK_SET) == 0);
        CHECK(fread(readback, 1, 6, stream) == 6);
        CHECK(memcmp(readback, "cookie", 6) == 0);
        CHECK(fseeko(stream, 0, SEEK_END) == 0);
        CHECK(fwrite("!", 1, 1, stream) == 1);
        CHECK(fclose(stream) == 0);
        CHECK(c.length == 7 && memcmp(c.bytes, "cookie!", 7) == 0);
        CHECK(c.reads > 0 && c.seeks > 0 && c.closes == 1);
    }
    /* Match the initdb write-only cookie, including failure deferred to flush. */
    callbacks.read = NULL;
    callbacks.seek = NULL;
    stream = fopencookie(&failed, "w", callbacks);
    CHECK(stream != NULL);
    if (stream) {
        CHECK(setvbuf(stream, NULL, _IOFBF, BUFSIZ) == 0);
        CHECK(fwrite("bad", 1, 3, stream) == 3);
        errno = 0;
        flush_rc = fflush(stream);
        flush_errno = errno;
        error_flag = ferror(stream);
        CHECK(flush_rc == EOF && flush_errno == EIO && error_flag != 0);
        fclose(stream);
        CHECK(failed.closes == 1);
    }
    stream = fopencookie(&closing, "w", callbacks);
    CHECK(stream != NULL);
    if (stream) {
        CHECK(setvbuf(stream, NULL, _IOFBF, BUFSIZ) == 0);
        CHECK(fwrite("bad", 1, 3, stream) == 3);
        closing.fail = true;
        close_rc = fclose(stream);
        CHECK(close_rc == EOF && closing.closes == 1);
    }
    probe_report("\"streams\":{\"memstreamLength\":%zu,\"cookieReads\":%u,"
                 "\"cookieWrites\":%u,\"cookieSeeks\":%u,\"cookieCloses\":%u,"
                 "\"failedFlush\":%d,\"failedFlushErrno\":%d,\"ferror\":%d,\"failedClose\":%d},",
                 length, c.reads, c.writes, c.seeks, c.closes,
                 flush_rc, flush_errno, error_flag, close_rc);
}

static void
semaphores(void)
{
    sem_t local, shared, full;
    int initial = -1, empty = -1, shared_rc, shared_errno;
    CHECK(sem_init(&local, 0, 2) == 0);
    CHECK(sem_getvalue(&local, &initial) == 0 && initial == 2);
    CHECK(sem_trywait(&local) == 0 && sem_trywait(&local) == 0);
    errno = 0;
    CHECK(sem_trywait(&local) == -1 && errno == EAGAIN);
    CHECK(sem_getvalue(&local, &empty) == 0 && empty == 0);
    CHECK(sem_post(&local) == 0 && sem_wait(&local) == 0);
    CHECK(sem_destroy(&local) == 0);
    errno = 0;
    shared_rc = sem_init(&shared, 1, 0);
    shared_errno = errno;
    /* The pinned SDK returns ENOTSUP directly without setting errno. */
    CHECK(shared_rc == ENOTSUP && shared_errno == 0);
    if (shared_rc == 0)
        sem_destroy(&shared);
    CHECK(sem_init(&full, 0, SEM_VALUE_MAX) == 0);
    errno = 0;
    CHECK(sem_post(&full) == -1 && errno == EOVERFLOW);
    CHECK(sem_destroy(&full) == 0);
    probe_report("\"semaphore\":{\"initialCount\":%d,\"emptyCount\":%d,"
                 "\"psharedReturn\":%d,\"psharedErrno\":%d,\"ENOTSUP\":%d,"
                 "\"processSharedSupported\":false},", initial, empty, shared_rc, shared_errno, ENOTSUP);
}

static void
entropy(void)
{
    unsigned char a[32] = {0}, b[32] = {0}, c[32] = {0}, too_large[257];
    int fd = open("/dev/urandom", O_RDONLY), entropy_rc, oversized_rc, oversized_errno;
    ssize_t bytes = -1;
    unsigned nonzero = 0;
    CHECK(fd >= 0);
    if (fd >= 0) {
        bytes = read(fd, a, sizeof(a));
        CHECK(bytes == (ssize_t) sizeof(a));
        CHECK(read(fd, b, sizeof(b)) == (ssize_t) sizeof(b));
        CHECK(close(fd) == 0);
    }
    entropy_rc = getentropy(c, sizeof(c));
    CHECK(entropy_rc == 0);
    for (size_t i = 0; i < sizeof(a); i++)
        nonzero |= a[i] | b[i] | c[i];
    CHECK(nonzero != 0 && memcmp(a, b, sizeof(a)) != 0 && memcmp(b, c, sizeof(b)) != 0);
    CHECK(getentropy(c, 0) == 0);
    errno = 0;
    oversized_rc = getentropy(too_large, sizeof(too_large));
    oversized_errno = errno;
    CHECK(oversized_rc == -1 && oversized_errno == EIO);
    probe_report("\"entropy\":{\"urandomBytes\":%zd,\"getentropyReturn\":%d,"
                 "\"oversizedReturn\":%d,\"oversizedErrno\":%d,\"distinctSamples\":%s},",
                 bytes, entropy_rc, oversized_rc, oversized_errno,
                 nonzero && memcmp(a, b, sizeof(a)) != 0 && memcmp(b, c, sizeof(b)) != 0 ? "true" : "false");
}

static void
locales(void)
{
    locale_t c = newlocale(LC_ALL_MASK, "C", (locale_t) 0);
    locale_t utf8 = newlocale(LC_ALL_MASK, "C.UTF-8", (locale_t) 0);
    locale_t named = newlocale(LC_ALL_MASK, "de_DE.UTF-8", (locale_t) 0);
    const char *accent = "\xc3\xa4";
    int c_order = 0, named_order = 0;
    wchar_t wide = 0;
    mbstate_t state = {0};
    size_t converted = 0;
    CHECK(c != (locale_t) 0 && utf8 != (locale_t) 0);
    CHECK(setlocale(LC_ALL, "C") != NULL);
    if (c && utf8) {
        locale_t previous = uselocale(utf8);
        CHECK(MB_CUR_MAX == 4);
        converted = mbrtowc(&wide, accent, 2, &state);
        CHECK(converted == 2 && wide == 0xe4);
        CHECK(uselocale(previous) == utf8);
        CHECK(MB_CUR_MAX == 1);
        c_order = strcoll_l(accent, "z", c);
        CHECK(c_order > 0 && localeconv()->decimal_point[0] == '.');
        if (named) {
            named_order = strcoll_l(accent, "z", named);
            CHECK(named_order > 0);
        }
    }
    probe_report("\"locale\":{\"provider\":\"Emscripten musl\",\"utf8Bytes\":%zu,"
                 "\"utf8Codepoint\":%u,\"namedLocaleAccepted\":%s,\"cCollation\":%d,"
                 "\"namedCollation\":%d,\"collation\":\"bytewise\",\"linguisticCollationValidated\":false},",
                 converted, (unsigned) wide, named ? "true" : "false", c_order, named_order);
    if (c) freelocale(c);
    if (utf8) freelocale(utf8);
    if (named) freelocale(named);
}

void
probe_libc(void)
{
    streams();
    semaphores();
    entropy();
    locales();
}
