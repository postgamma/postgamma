/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "opfs.h"

#include <dirent.h>
#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct Case { const char *name, *capability; int checks, failed, error; long result; };
static struct Case cases[96];
static int case_count;
static pthread_mutex_t case_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t coordinator;
static _Atomic int close_started, close_done, gate_round;
static int close_fd, close_rc, close_error;
static pthread_barrier_t file_barrier;
static char json[32768];
_Atomic int opfs_stage, opfs_failures;

void
opfs_check(const char *name, int passed, long result, int error, const char *capability)
{
    pthread_mutex_lock(&case_lock);
    int i;
    for (i = 0; i < case_count; i++) if (strcmp(cases[i].name, name) == 0) break;
    if (i == case_count) {
        if (case_count == 96) __builtin_trap();
        cases[case_count++] = (struct Case){.name = name, .capability = capability};
    }
    cases[i].checks++;
    if (!passed) { cases[i].failed++; atomic_fetch_add(&opfs_failures, 1); }
    cases[i].result = result;
    cases[i].error = error;
    pthread_mutex_unlock(&case_lock);
}

#define CHECK(name, expression) opfs_check(name, (expression), 0, 0, "supported")
#define OBSERVE(name, expression, expected, error_code, capability) do { \
    errno = 0; long result = (expression); int error = errno; \
    opfs_check(name, result == (expected) && error == (error_code), result, error, capability); \
} while (0)

static int
checked_open(const char *path, int flags)
{
    int fd = open(path, flags, 0600);
    opfs_check("fixture open", fd >= 0, fd, fd < 0 ? errno : 0, "supported");
    if (fd < 0) __builtin_trap();
    return fd;
}

static void
file_semantics(void)
{
    const char *path = "/opfs/data";
    char data[64] = {0};
    struct stat a, b;
    int opened = atomic_load(&opfs_counts[OP_OPEN]);
    int closed = atomic_load(&opfs_counts[OP_CLOSE]);
    int first = checked_open(path, O_CREAT | O_EXCL | O_RDWR);
    int second = checked_open(path, O_RDWR);
    int duplicate = dup(first);
    CHECK("dup succeeds", duplicate >= 0);
    CHECK("same file shares one access handle", atomic_load(&opfs_counts[OP_OPEN]) == opened + 1);
    CHECK("same file identity", fstat(first, &a) == 0 && fstat(second, &b) == 0 && a.st_ino == b.st_ino);
    CHECK("initial write", write(first, "abcdefghijklmnop", 16) == 16);
    CHECK("independent open offsets", read(second, data, 3) == 3 && memcmp(data, "abc", 3) == 0 &&
          lseek(first, 0, SEEK_CUR) == 16 && lseek(second, 0, SEEK_CUR) == 3);
    CHECK("dup shares offset", lseek(first, 0, SEEK_SET) == 0 && read(duplicate, data, 2) == 2 &&
          memcmp(data, "ab", 2) == 0 && lseek(first, 0, SEEK_CUR) == 2);
    CHECK("pread preserves offset", pread(second, data, 3, 8) == 3 && memcmp(data, "ijk", 3) == 0 &&
          lseek(second, 0, SEEK_CUR) == 3);
    CHECK("pwrite preserves offset and updates peer", pwrite(second, "XYZ", 3, 4) == 3 &&
          lseek(second, 0, SEEK_CUR) == 3 && read(duplicate, data, 5) == 5 && memcmp(data, "cdXYZ", 5) == 0);
    CHECK("close original retains duplicate", close(first) == 0 &&
          atomic_load(&opfs_counts[OP_CLOSE]) == closed && pread(duplicate, data, 3, 4) == 3);
    CHECK("close dup retains independent descriptor", close(duplicate) == 0 &&
          atomic_load(&opfs_counts[OP_CLOSE]) == closed && pread(second, data, 3, 4) == 3);
    CHECK("last close releases physical handle", close(second) == 0 &&
          atomic_load(&opfs_counts[OP_CLOSE]) == closed + 1);

    first = checked_open(path, O_RDONLY);
    opened = atomic_load(&opfs_counts[OP_OPEN]);
    second = checked_open(path, O_RDWR);
    CHECK("readonly snapshot upgrades to shared access", atomic_load(&opfs_counts[OP_OPEN]) == opened + 1 &&
          pwrite(second, "new", 3, 0) == 3 && pread(first, data, 3, 0) == 3 && memcmp(data, "new", 3) == 0);
    CHECK("truncate shrink visible to peer", ftruncate(second, 5) == 0 && fstat(first, &a) == 0 && a.st_size == 5);
    CHECK("EOF gives short read", pread(first, data, 64, 3) == 2 && pread(first, data, 64, 5) == 0);
    memset(data, 1, sizeof(data));
    CHECK("truncate extension reads zero", ftruncate(second, 8192) == 0 && pread(first, data, 64, 8000) == 64);
    int zero = 1;
    for (size_t i = 0; i < sizeof(data); i++) if (data[i]) zero = 0;
    CHECK("extended bytes are zero", zero);
    for (int i = 0; i < 32; i++) {
        off_t offset = (i * 251 + 37) % (8192 - 32);
        memset(data, i + 1, 32);
        CHECK("random partial page write", pwrite(second, data, 32, offset) == 32);
        memset(data, 0, 32);
        CHECK("random partial page read", pread(first, data, 32, offset) == 32);
        int correct = 1;
        for (int j = 0; j < 32; j++) if (data[j] != i + 1) correct = 0;
        CHECK("random partial page contents", correct);
    }

    atomic_store(&opfs_fault, SHORT_READ);
    OBSERVE("injected short read is preserved", pread(second, data, 32, 0), 7, 0, "supported");
    CHECK("short read retains unread buffer", data[7] == 32);
    memset(data, 'S', 32);
    atomic_store(&opfs_fault, SHORT_WRITE);
    OBSERVE("injected short write is preserved", pwrite(second, data, 32, 0), 7, 0, "supported");
    CHECK("short write updates only returned bytes", pread(second, data, 8, 0) == 8 &&
          memcmp(data, "SSSSSSS", 7) == 0 && data[7] != 'S');
    atomic_store(&opfs_fault, READ_TYPE_ERROR);
    OBSERVE("read TypeError maps to EINVAL", pread(second, data, 32, 0), -1, EINVAL, "error-mapping");
    atomic_store(&opfs_fault, WRITE_QUOTA);
    OBSERVE("quota error loses ENOSPC", pwrite(second, data, 32, 0), -1, EIO, "adapter-required");
    atomic_store(&opfs_fault, FLUSH_ERROR);
    OBSERVE("flush failure reaches caller", fsync(second), -1, EIO, "error-mapping");
    int flushed = atomic_load(&opfs_counts[OP_FLUSH]);
    CHECK("fsync and fdatasync call access flush", fsync(second) == 0 && fdatasync(second) == 0 &&
          atomic_load(&opfs_counts[OP_FLUSH]) == flushed + 2);
    CHECK("I/O recovers after injected errors", pwrite(second, "recovered", 9, 0) == 9 &&
          pread(first, data, 9, 0) == 9 && memcmp(data, "recovered", 9) == 0);
    CHECK("close upgraded descriptors", close(second) == 0 && close(first) == 0);

    first = checked_open(path, O_RDONLY);
    OBSERVE("path truncate with readonly blob open", truncate(path, 3), -1, EIO, "adapter-required");
    flushed = atomic_load(&opfs_counts[OP_FLUSH]);
    OBSERVE("readonly blob fsync has no storage flush", fsync(first), 0, 0, "adapter-required");
    CHECK("readonly flush is unimplemented", atomic_load(&opfs_counts[OP_FLUSH]) == flushed);
    CHECK("close readonly blob", close(first) == 0);
    CHECK("closed-file truncate", truncate(path, 3) == 0 && stat(path, &a) == 0 && a.st_size == 3);
    OBSERVE("missing file errno", open("/opfs/absent", O_RDONLY), -1, ENOENT, "error-mapping");
    OBSERVE("invalid descriptor errno", fsync(-1), -1, EBADF, "error-mapping");
}

static void
namespace_semantics(void)
{
    struct stat info;
    char data[32];
    CHECK("create directory", mkdir("/opfs/directory", 0700) == 0);
    OBSERVE("directory rename unsupported", rename("/opfs/directory", "/opfs/moved-directory"),
            -1, EBUSY, "adapter-required");
    CHECK("failed directory rename retains source", stat("/opfs/directory", &info) == 0 && S_ISDIR(info.st_mode));
    int dir = checked_open("/opfs/directory", O_RDONLY | O_DIRECTORY);
    int flushed = atomic_load(&opfs_counts[OP_FLUSH]);
    OBSERVE("directory fsync returns without storage flush", fsync(dir), 0, 0, "adapter-required");
    CHECK("directory flush is unimplemented", atomic_load(&opfs_counts[OP_FLUSH]) == flushed);
    CHECK("close directory descriptor", close(dir) == 0);

    int fd = checked_open("/opfs/open-file", O_CREAT | O_RDWR);
    CHECK("open namespace fixture data", write(fd, "identity", 8) == 8);
    OBSERVE("rename while access handle open", rename("/opfs/open-file", "/opfs/moved-file"),
            -1, EIO, "adapter-required");
    OBSERVE("unlink while access handle open", unlink("/opfs/open-file"), -1, EIO, "adapter-required");
    CHECK("failed namespace operation retains fd and path", pread(fd, data, 8, 0) == 8 &&
          memcmp(data, "identity", 8) == 0 && stat("/opfs/open-file", &info) == 0);
    CHECK("close namespace fixture", close(fd) == 0);
    CHECK("closed file rename across directories", rename("/opfs/open-file", "/opfs/directory/moved-file") == 0);
    OBSERVE("old path gone after rename", stat("/opfs/open-file", &info), -1, ENOENT, "supported");
    fd = checked_open("/opfs/directory/moved-file", O_RDONLY);
    CHECK("renamed file keeps contents", read(fd, data, 8) == 8 && memcmp(data, "identity", 8) == 0);
    CHECK("close renamed file", close(fd) == 0);
    OBSERVE("nonempty directory errno", rmdir("/opfs/directory"), -1, ENOTEMPTY, "error-mapping");
    CHECK("unlink closed file", unlink("/opfs/directory/moved-file") == 0);
    CHECK("remove empty directory", rmdir("/opfs/directory") == 0);

    fd = checked_open("/opfs/replacement", O_CREAT | O_RDWR);
    CHECK("prepare rename replacement", write(fd, "old", 3) == 3 && close(fd) == 0);
    CHECK("rename replaces closed destination", rename("/opfs/data", "/opfs/replacement") == 0);
    fd = checked_open("/opfs/replacement", O_RDONLY);
    CHECK("replacement contains source", read(fd, data, 3) == 3 && memcmp(data, "rec", 3) == 0 && close(fd) == 0);

    pthread_mutex_t caller_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&caller_lock);
    atomic_store(&opfs_fault, DIR_DELAY);
    double start = opfs_now();
    CHECK("async directory completes with caller mutex held", mkdir("/opfs/async-directory", 0700) == 0);
    CHECK("async owner timer actually ran", atomic_load(&opfs_counts[OP_DIR_END]) == 1 && opfs_now() - start >= 140);
    pthread_mutex_unlock(&caller_lock);
    pthread_mutex_destroy(&caller_lock);
    DIR *directory = opendir("/opfs");
    CHECK("list directory", directory != NULL);
    int found = 0;
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory))) if (strcmp(entry->d_name, "async-directory") == 0) found++;
        closedir(directory);
    }
    CHECK("async namespace visible in enumeration", found == 1);
}

static void *
same_file_caller(void *argument)
{
    int slot = (int) (uintptr_t) argument;
    unsigned char buffer[8192];
    int fd = checked_open("/opfs/concurrent", O_CREAT | O_RDWR);
    pthread_barrier_wait(&file_barrier);
    for (int round = 0; round < 16; round++) {
        memset(buffer, slot * 16 + round + 1, sizeof(buffer));
        CHECK("concurrent same-file positional write", pwrite(fd, buffer, sizeof(buffer), slot * 8192) == 8192);
        memset(buffer, 0, sizeof(buffer));
        CHECK("concurrent same-file positional read", pread(fd, buffer, sizeof(buffer), slot * 8192) == 8192);
        int correct = 1;
        for (size_t i = 0; i < sizeof(buffer); i++) if (buffer[i] != slot * 16 + round + 1) correct = 0;
        CHECK("concurrent same-file contents", correct);
    }
    CHECK("concurrent descriptors retain independent offsets", lseek(fd, 0, SEEK_CUR) == 0);
    pthread_barrier_wait(&file_barrier);
    int peer = (slot + 1) % 3;
    CHECK("concurrent caller sees peer page", pread(fd, buffer, sizeof(buffer), peer * 8192) == 8192 &&
          buffer[0] == peer * 16 + 16 && buffer[8191] == peer * 16 + 16);
    pthread_barrier_wait(&file_barrier);
    CHECK("close concurrent file descriptor", close(fd) == 0);
    return NULL;
}

static void
concurrent_file(void)
{
    pthread_t peers[2];
    int opened = atomic_load(&opfs_counts[OP_OPEN]);
    int closed = atomic_load(&opfs_counts[OP_CLOSE]);
    pthread_barrier_init(&file_barrier, NULL, 3);
    for (int i = 0; i < 2; i++)
        if (pthread_create(&peers[i], NULL, same_file_caller, (void *) (uintptr_t) (i + 1))) __builtin_trap();
    same_file_caller(NULL);
    for (int i = 0; i < 2; i++) pthread_join(peers[i], NULL);
    pthread_barrier_destroy(&file_barrier);
    CHECK("concurrent opens use exactly one physical handle", atomic_load(&opfs_counts[OP_OPEN]) == opened + 1 &&
          atomic_load(&opfs_counts[OP_CLOSE]) == closed + 1);
}

static void *
close_during_write(void *unused)
{
    (void) unused;
    while (!atomic_load(&opfs_gate_entered)) emscripten_thread_sleep(1);
    atomic_store(&close_started, 1);
    errno = 0;
    close_rc = close(close_fd);
    close_error = errno;
    atomic_store(&close_done, 1);
    return NULL;
}

static void
inflight_close(void)
{
    for (int round = 1; round <= 2; round++) {
        pthread_t closer;
        char buffer[16] = {0};
        atomic_store(&opfs_gate, 0);
        atomic_store(&opfs_gate_entered, 0);
        atomic_store(&close_started, 0);
        atomic_store(&close_done, 0);
        close_fd = checked_open("/opfs/inflight", O_CREAT | O_RDWR);
        int peer = round == 1 ? checked_open("/opfs/inflight", O_RDWR) : -1;
        int rc = pthread_create(&closer, NULL, close_during_write, NULL);
        if (rc) __builtin_trap();
        atomic_store(&gate_round, round);
        atomic_store(&opfs_stage, 3);
        atomic_store(&opfs_fault, WRITE_GATE);
        CHECK("inflight write completes", pwrite(close_fd, "inflight", 8, 0) == 8);
        pthread_join(closer, NULL);
        opfs_check("concurrent close completes", close_rc == 0, close_rc, close_error, "supported");
        if (peer < 0) peer = checked_open("/opfs/inflight", O_RDWR);
        CHECK("peer or reopen sees completed write", pread(peer, buffer, 8, 0) == 8 && memcmp(buffer, "inflight", 8) == 0);
        CHECK("close retained peer", close(peer) == 0);
    }
}

static void *
run_suite(void *argument)
{
    int verify = (int) (uintptr_t) argument;
    atomic_store(&opfs_stage, 1);
    backend_t backend = wasmfs_create_opfs_backend();
    CHECK("create SDK OPFS backend", backend != NULL);
    CHECK("mount OPFS backend", wasmfs_create_directory("/opfs", 0700, backend) == 0);
    if (!verify) {
        atomic_store(&opfs_stage, 2);
        file_semantics();
        concurrent_file();
        inflight_close();
        namespace_semantics();
        atomic_store(&opfs_stage, 6);
        opfs_benchmark();
        int fd = checked_open("/opfs/reopen-marker", O_CREAT | O_RDWR);
        CHECK("write and explicitly flush reopen marker", write(fd, "OPFS-probe-marker", 17) == 17 && fsync(fd) == 0);
        CHECK("close reopen marker", close(fd) == 0);
        CHECK("all injected operations were reached", atomic_load(&opfs_faults_seen) == 8 && atomic_load(&opfs_fault) == 0);
    } else {
        int fd = checked_open("/opfs/reopen-marker", O_RDONLY);
        char buffer[32] = {0};
        struct stat info;
        CHECK("fresh module reads flushed file", read(fd, buffer, 32) == 17 && memcmp(buffer, "OPFS-probe-marker", 17) == 0);
        CHECK("fresh module closes file", close(fd) == 0);
        CHECK("fresh module sees namespace", stat("/opfs/async-directory", &info) == 0 && S_ISDIR(info.st_mode));
        fd = checked_open("/opfs/replacement", O_RDONLY);
        CHECK("fresh module sees renamed contents", read(fd, buffer, 3) == 3 && memcmp(buffer, "rec", 3) == 0);
        CHECK("fresh module closes renamed file", close(fd) == 0);
    }
    CHECK("all handles use one I/O owner", atomic_load(&opfs_owner) != 0 && !atomic_load(&opfs_owner_errors));
    CHECK("I/O owner differs from blocking C caller", atomic_load(&opfs_owner) != (int) (uintptr_t) pthread_self());
    CHECK("all access handles closed", atomic_load(&opfs_counts[OP_OPEN]) == atomic_load(&opfs_counts[OP_CLOSE]));
    atomic_store(&opfs_stage, 7);
    return NULL;
}

EMSCRIPTEN_KEEPALIVE int
opfs_start(int verify)
{
    if (atomic_load(&opfs_stage)) return EBUSY;
    atomic_store(&opfs_stage, 1);
    return pthread_create(&coordinator, NULL, run_suite, (void *) (uintptr_t) verify);
}

EMSCRIPTEN_KEEPALIVE const char *
opfs_state(void)
{
    snprintf(json, sizeof(json), "{\"stage\":%d,\"gateRound\":%d,\"gateEntered\":%d,\"closeStarted\":%d,\"closeDone\":%d,\"directoryPending\":%d}",
             atomic_load(&opfs_stage), atomic_load(&gate_round), atomic_load(&opfs_gate_entered),
             atomic_load(&close_started), atomic_load(&close_done),
             atomic_load(&opfs_counts[OP_DIR_BEGIN]) > atomic_load(&opfs_counts[OP_DIR_END]));
    return json;
}

EMSCRIPTEN_KEEPALIVE void
opfs_release(void)
{
    atomic_store(&opfs_gate, 1);
    emscripten_futex_wake(&opfs_gate, 1);
}

EMSCRIPTEN_KEEPALIVE const char *
opfs_report(void)
{
    if (atomic_load(&opfs_stage) != 7) return "{}";
    pthread_join(coordinator, NULL);
    size_t used = (size_t) snprintf(json, sizeof(json),
        "{\"failures\":%d,\"owner\":%d,\"counts\":[%d,%d,%d,%d,%d,%d,%d,%d],\"faultsReached\":%d,\"cases\":[",
        atomic_load(&opfs_failures), atomic_load(&opfs_owner),
        atomic_load(&opfs_counts[1]), atomic_load(&opfs_counts[2]), atomic_load(&opfs_counts[3]),
        atomic_load(&opfs_counts[4]), atomic_load(&opfs_counts[5]), atomic_load(&opfs_counts[6]),
        atomic_load(&opfs_counts[7]), atomic_load(&opfs_counts[8]), atomic_load(&opfs_faults_seen));
    for (int i = 0; i < case_count; i++) {
        struct Case *c = &cases[i];
        used += (size_t) snprintf(json + used, sizeof(json) - used,
            "%s{\"name\":\"%s\",\"capability\":\"%s\",\"checks\":%d,\"failed\":%d,\"result\":%ld,\"errno\":%d}",
            i ? "," : "", c->name, c->capability, c->checks, c->failed, c->result, c->error);
    }
    snprintf(json + used, sizeof(json) - used, "]}");
    return json;
}
