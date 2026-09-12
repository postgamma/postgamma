/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include <fcntl.h>
#include <sys/stat.h>

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
    STORAGE_CHECK(fsync(a) == 0 && fdatasync(a) == 0);
    STORAGE_CHECK(rename("/persistent/storage-test-a", "/persistent/storage-test-b") == 0);
    STORAGE_CHECK(open("/persistent/storage-test-a", O_RDONLY) < 0 && errno == ENOENT);
    b = openat(directory, "kept", O_RDONLY);
    STORAGE_CHECK(b >= 0 && read(b, buffer, 16) == 7 && memcmp(buffer, "persist", 7) == 0);
    STORAGE_CHECK(fsync(directory) == 0 && fdatasync(directory) == 0);
    STORAGE_CHECK(close(a) == 0 && close(b) == 0 && close(directory) == 0);
    return checks;
#undef STORAGE_CHECK
}
