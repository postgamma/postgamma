/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
/* Driver-owned, bounded filesystem traversal after PostgreSQL has shut down.
 * Records contain only relative directories, regular files and 256 KiB data
 * chunks. Restore never follows links or replaces an existing path. */
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#define ARCHIVE_CHUNK (256 * 1024)
#define ARCHIVE_DEPTH 64
static char *database_path;
static bool archive_ready, restoring, restore_complete;
static DIR *archive_dirs[ARCHIVE_DEPTH];
static char archive_paths[ARCHIVE_DEPTH][4096];
static unsigned archive_depth;
static int archive_fd = -1;
static uint64_t archive_remaining;
static bool archive_started, archive_ended;

static void archive_reset(void)
{
    if (archive_fd >= 0) close(archive_fd);
    archive_fd = -1;
    while (archive_depth) closedir(archive_dirs[--archive_depth]);
}

static bool archive_path(const char *relative, char *absolute, size_t capacity)
{
    if (!relative || !*relative || *relative == '/' || strlen(relative) > 2048) return false;
    const char *part = relative;
    for (const char *p = relative;; p++) {
        if (*p == '\\' || (unsigned char)*p >= 128) return false;
        if (*p == '/' || !*p) {
            size_t n = p - part;
            if (!n || (n == 1 && part[0] == '.') || (n == 2 && !memcmp(part, "..", 2))) return false;
            part = p + 1;
        }
        if (!*p) break;
        if ((unsigned char)*p < 32) return false;
    }
    return snprintf(absolute, capacity, "%s/%s", database_path, relative) < (int)capacity;
}

static int archive_read(Writer *out)
{
    if (!archive_ready || restoring || !database_path || archive_ended) return EINVAL;
    if (archive_fd >= 0) {
        if (archive_remaining) {
            unsigned char *data = malloc(ARCHIVE_CHUNK);
            if (!data) return ENOMEM;
            size_t count = archive_remaining > ARCHIVE_CHUNK ? ARCHIVE_CHUNK : archive_remaining;
            ssize_t n = read(archive_fd, data, count);
            if (n <= 0) {int code = n ? errno : EIO; free(data); return code;}
            archive_remaining -= n;
            put(out, 3); value(out, data, n, false); free(data); return 0;
        }
        int code = close(archive_fd); archive_fd = -1;
        if (code) return errno;
        put(out, 4); return 0;
    }
    if (!archive_started) {
        archive_dirs[0] = opendir(database_path);
        if (!archive_dirs[0]) return errno;
        archive_paths[0][0] = 0; archive_depth = 1; archive_started = true;
    }
    while (archive_depth) {
        unsigned level = archive_depth - 1;
        errno = 0;
        struct dirent *entry = readdir(archive_dirs[level]);
        if (!entry) {
            if (errno) return errno;
            closedir(archive_dirs[level]); archive_depth--; continue;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char relative[4096], absolute[4096];
        if (snprintf(relative, sizeof(relative), "%s%s%s", archive_paths[level],
                     *archive_paths[level] ? "/" : "", entry->d_name) >= sizeof(relative) ||
            !archive_path(relative, absolute, sizeof(absolute))) return EINVAL;
        struct stat st;
        if (lstat(absolute, &st)) return errno;
        if (S_ISDIR(st.st_mode)) {
            if (archive_depth == ARCHIVE_DEPTH) return EOVERFLOW;
            DIR *dir = opendir(absolute);
            if (!dir) return errno;
            archive_dirs[archive_depth] = dir;
            strcpy(archive_paths[archive_depth++], relative);
            put(out, 1); text(out, relative); return 0;
        }
        if (!S_ISREG(st.st_mode) || st.st_size < 0) return EINVAL;
        archive_fd = open(absolute, O_RDONLY | O_NOFOLLOW);
        if (archive_fd < 0) return errno;
        archive_remaining = st.st_size;
        put(out, 2); text(out, relative); number64(out, archive_remaining); return 0;
    }
    archive_ended = true; put(out, 0); return 0;
}

static int archive_write(Reader *r)
{
    if (!restoring || restore_complete || !database_path) return EINVAL;
    unsigned kind = get(r);
    if (kind == 1 || kind == 2) {
        char *relative = string(r), absolute[4096];
        uint64_t size = 0;
        if (kind == 2) {size = get(r); size |= (uint64_t)get(r) << 32;}
        bool valid = !r->failed && r->offset == r->size && archive_fd < 0 &&
            size <= INT64_MAX && archive_path(relative, absolute, sizeof(absolute));
        free(relative);
        if (!valid) return EINVAL;
        /* Only directories created by earlier validated records exist here;
         * neither symlinks nor a preexisting destination are admitted. */
        if (kind == 1) return mkdir(absolute, 0700) ? errno : 0;
        archive_fd = open(absolute, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        archive_remaining = size;
        return archive_fd < 0 ? errno : 0;
    }
    if (kind == 3) {
        size_t size; const void *data = blob(r, &size);
        if (r->failed || r->offset != r->size || archive_fd < 0 || !size ||
            size > ARCHIVE_CHUNK || size > archive_remaining) return EINVAL;
        size_t offset = 0;
        while (offset < size) {
            ssize_t n = write(archive_fd, (const char *)data + offset, size - offset);
            if (n <= 0) return n ? errno : EIO;
            offset += n;
        }
        archive_remaining -= size; return 0;
    }
    if (r->failed || r->offset != r->size) return EINVAL;
    if (kind == 4) {
        if (archive_fd < 0 || archive_remaining) return EINVAL;
        if (fsync(archive_fd)) return errno;
        int code = close(archive_fd); archive_fd = -1;
        return code ? errno : 0;
    }
    if (kind != 0 || archive_fd >= 0) return EINVAL;
    restore_complete = true; return 0;
}
