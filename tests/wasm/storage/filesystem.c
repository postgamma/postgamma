/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include <fcntl.h>
#include <sys/stat.h>

static _Atomic int storage_relay_probe;
static _Atomic int storage_relay_result;

/* Model a short synchronous SDK call on the control Worker. The I/O owner
 * must drain its own queue even while that Worker cannot relay JS messages. */
static void storage_relay_parent_wait(void)
{
    if (atomic_load(&storage_relay_probe) == 1) {
        double deadline = emscripten_get_now() + 1000;
        atomic_store(&storage_relay_probe, 2);
        while (atomic_load(&storage_relay_probe) != 3 && emscripten_get_now() < deadline) usleep(1000);
        atomic_store(&storage_relay_result, atomic_load(&storage_relay_probe) == 3 ? 1 : -1);
    }
}

static void *storage_pending_write(void *argument)
{
    int *fd = argument;
    ssize_t written = pwrite(*fd, "inflight", 8, 0);
    *fd = close(*fd) == 0 && written == 8 ? 0 : -1;
    return NULL;
}

static int storage_filesystem_probe(unsigned phase)
{
    int checks = 0, a, b, duplicate, directory, replacement;
    char buffer[16];
    struct stat old, current;
#define STORAGE_CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "storage filesystem: line %d: %s; errno=%d\n", __LINE__, #expr, errno); \
    return -1; } checks++; } while (0)
    if (phase == 15) {
        a = open("/persistent/storage-test-b/kept", O_RDONLY);
        STORAGE_CHECK(a >= 0);
        atomic_store(&storage_relay_result, 0);
        atomic_store(&storage_relay_probe, 1);
        double deadline = emscripten_get_now() + 5000;
        while (atomic_load(&storage_relay_probe) == 1 && emscripten_get_now() < deadline) usleep(1000);
        STORAGE_CHECK(atomic_load(&storage_relay_probe) == 2 && fsync(a) == 0);
        atomic_store(&storage_relay_probe, 3);
        while (atomic_load(&storage_relay_result) == 0 && emscripten_get_now() < deadline) usleep(1000);
        STORAGE_CHECK(close(a) == 0);
        STORAGE_CHECK(atomic_load(&storage_relay_result) == 1);
        atomic_store(&storage_relay_probe, 0);
        return checks;
    }
    if (phase >= 12 && phase <= 14) {
        if (phase == 12) {
            STORAGE_CHECK(mkdir("/persistent/recovery", 0700) == 0 || errno == EEXIST);
            STORAGE_CHECK(unlink("/persistent/recovery/after") == 0 || errno == ENOENT);
            a = open("/persistent/recovery/before", O_CREAT | O_TRUNC | O_RDWR, 0600);
            STORAGE_CHECK(a >= 0 && write(a, "identity", 8) == 8 && fsync(a) == 0 && close(a) == 0);
        } else if (phase == 13) {
            STORAGE_CHECK(rename("/persistent/recovery/before", "/persistent/recovery/after") == 0);
        } else {
            int before = stat("/persistent/recovery/before", &old) == 0;
            int after = stat("/persistent/recovery/after", &current) == 0;
            STORAGE_CHECK(before != after);
            a = open(before ? "/persistent/recovery/before" : "/persistent/recovery/after", O_RDONLY);
            STORAGE_CHECK(a >= 0 && read(a, buffer, sizeof(buffer)) == 8 && memcmp(buffer, "identity", 8) == 0 && close(a) == 0);
        }
        return checks;
    }
    if (phase >= 4 && phase <= 11) {
        if (phase == 4) {
            STORAGE_CHECK(mkdir("/persistent/faults", 0700) == 0);
            a = open("/persistent/faults/data", O_CREAT | O_RDWR, 0600);
            STORAGE_CHECK(a >= 0 && pwrite(a, "short-write", 11, 0) == 7);
            STORAGE_CHECK(pread(a, buffer, sizeof(buffer), 0) == 7 && memcmp(buffer, "short-w", 7) == 0);
        } else if (phase == 5) {
            a = open("/persistent/faults/quota", O_CREAT | O_RDWR, 0600);
            STORAGE_CHECK(a < 0 && errno == ENOSPC);
            return checks;
        } else if (phase == 10) {
            STORAGE_CHECK(rename("/persistent/faults/data", "/persistent/faults/renamed") < 0 && errno == EIO);
            return checks;
        } else if (phase == 11) {
            a = open("/persistent/faults/renamed", O_RDONLY);
            if (a < 0) a = open("/persistent/faults/data", O_RDONLY);
            STORAGE_CHECK(a >= 0 && read(a, buffer, sizeof(buffer)) == 7 && memcmp(buffer, "short-w", 7) == 0);
        } else {
            a = open(phase == 9 ? "/persistent/faults" : "/persistent/faults/data", O_RDONLY);
            STORAGE_CHECK(a >= 0);
            if (phase == 6) {STORAGE_CHECK(fsync(a) < 0 && errno == EIO);}
            if (phase == 7) {STORAGE_CHECK(fdatasync(a) < 0 && errno == EIO);}
            if (phase == 8) {STORAGE_CHECK(truncate("/persistent/faults/data", 1) < 0 && errno == EIO);}
            if (phase == 9) {STORAGE_CHECK(fsync(a) < 0 && errno == EIO);}
        }
        STORAGE_CHECK(close(a) == 0);
        return checks;
    }
    if (phase == 3) {
        pthread_t writer;
        a = open("/persistent/pending-write", O_CREAT | O_RDWR, 0600);
        b = open("/persistent/pending-write", O_RDWR);
        STORAGE_CHECK(a >= 0 && b >= 0 && pthread_create(&writer, NULL, storage_pending_write, &b) == 0);
        double deadline = emscripten_get_now() + 5000;
        while (!postgamma_wasm_storage_pending_writes() && emscripten_get_now() < deadline) usleep(1000);
        STORAGE_CHECK(postgamma_wasm_storage_pending_writes() == 1);
        STORAGE_CHECK(close(a) == 0);
        STORAGE_CHECK(pthread_join(writer, NULL) == 0 && b == 0);
        return checks;
    }
    if (phase == 2) {
        a = open("/persistent/storage-test-b/kept", O_RDONLY);
        STORAGE_CHECK(a >= 0);
        STORAGE_CHECK(read(a, buffer, 16) == 7 && memcmp(buffer, "persist", 7) == 0);
        STORAGE_CHECK(close(a) == 0);
        return checks;
    }
    STORAGE_CHECK(phase == 1);
    STORAGE_CHECK(mkdir("/persistent/storage-test-a", 0700) == 0);
    directory = open("/persistent/storage-test-a", O_RDONLY | O_DIRECTORY);
    STORAGE_CHECK(directory >= 0);
    a = openat(directory, "open", O_RDWR | O_CREAT | O_EXCL, 0600);
    b = openat(directory, "open", O_RDONLY);
    duplicate = dup(a);
    STORAGE_CHECK(a >= 0 && b >= 0 && duplicate >= 0);
    STORAGE_CHECK(write(a, "abcdef", 6) == 6);
    STORAGE_CHECK(lseek(duplicate, 1, SEEK_SET) == 1 && read(a, buffer, 2) == 2 && memcmp(buffer, "bc", 2) == 0);
    STORAGE_CHECK(read(b, buffer, 3) == 3 && memcmp(buffer, "abc", 3) == 0);
    STORAGE_CHECK(pwrite(a, "XY", 2, 2) == 2 && pread(b, buffer, 6, 0) == 6 && memcmp(buffer, "abXYef", 6) == 0);
    STORAGE_CHECK(fstat(a, &old) == 0);
    STORAGE_CHECK(rename("/persistent/storage-test-a/open", "/persistent/storage-test-a/moved") == 0);
    STORAGE_CHECK(stat("/persistent/storage-test-a/moved", &current) == 0 && current.st_ino == old.st_ino);
    STORAGE_CHECK(unlink("/persistent/storage-test-a/moved") == 0);
    replacement = openat(directory, "moved", O_CREAT | O_RDWR, 0600);
    STORAGE_CHECK(replacement >= 0 && fstat(replacement, &current) == 0 && current.st_ino != old.st_ino);
    STORAGE_CHECK(pread(b, buffer, 6, 0) == 6 && memcmp(buffer, "abXYef", 6) == 0);
    STORAGE_CHECK(close(a) == 0 && close(b) == 0 && close(duplicate) == 0 && close(replacement) == 0);
    a = openat(directory, "kept", O_CREAT | O_RDWR, 0600);
    STORAGE_CHECK(a >= 0 && write(a, "persist-extra", 13) == 13 && close(a) == 0);
    a = openat(directory, "kept", O_RDONLY);
    STORAGE_CHECK(a >= 0 && truncate("/persistent/storage-test-a/kept", 7) == 0);
    STORAGE_CHECK(fstat(a, &current) == 0 && current.st_size == 7);
    STORAGE_CHECK(fsync(a) == 0 && fdatasync(a) == 0);
    STORAGE_CHECK(rename("/persistent/storage-test-a", "/persistent/storage-test-b") == 0);
    STORAGE_CHECK(open("/persistent/storage-test-a", O_RDONLY) < 0 && errno == ENOENT);
    b = openat(directory, "kept", O_RDONLY);
    STORAGE_CHECK(b >= 0 && read(b, buffer, 16) == 7 && memcmp(buffer, "persist", 7) == 0);
    {
        int i;
        /* Evict the cached size and access handle while the descriptor lives. */
        for (i = 0; i < 80; ++i) {
            char name[32];
            int fd;
            snprintf(name, sizeof(name), "cache-%d", i);
            fd = openat(directory, name, O_CREAT | O_RDWR, 0600);
            STORAGE_CHECK(fd >= 0 && pwrite(fd, "tail", 4, 16) == 4 && close(fd) == 0);
        }
        STORAGE_CHECK(fstat(a, &current) == 0 && current.st_size == 7);
        replacement = openat(directory, "cache-0", O_RDWR);
        STORAGE_CHECK(replacement >= 0 && fstat(replacement, &current) == 0 && current.st_size == 20);
        STORAGE_CHECK(pread(replacement, buffer, 4, 16) == 4 && memcmp(buffer, "tail", 4) == 0);
        STORAGE_CHECK(ftruncate(replacement, 2) == 0 && fstat(replacement, &current) == 0 && current.st_size == 2);
        STORAGE_CHECK(close(replacement) == 0);
    }
    STORAGE_CHECK(fsync(directory) == 0 && fdatasync(directory) == 0);
    STORAGE_CHECK(close(a) == 0 && close(b) == 0 && close(directory) == 0);
    return checks;
#undef STORAGE_CHECK
}
