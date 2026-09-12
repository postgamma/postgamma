/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/file_platform.h"
#include "wasmfs.h"
#include <cerrno>
#include <fcntl.h>
#include <new>
#include <unistd.h>

namespace {
struct Claim {
    std::shared_ptr<wasmfs::File> file;
    uint64_t generation;
    const void *owner;
    Claim *next;
};
std::mutex claims_mutex;
Claim *claims;

std::shared_ptr<wasmfs::File> file_object(int fd) {
    auto entry = wasmfs::wasmFS.getFileTable().locked().getEntry(fd);
    if (!entry) { errno = EBADF; return nullptr; }
    return entry->locked().getFile();
}

std::shared_ptr<wasmfs::File> directory(int fd) {
    auto file = file_object(fd);
    if (file && !file->is<wasmfs::Directory>()) { errno = ENOTDIR; return nullptr; }
    return file;
}
}

int postgamma_platform_directory_claim(int fd, uint64_t generation, const void *owner) {
    if (!directory(fd)) return errno;
    return postgamma_platform_file_claim(fd, generation, owner);
}

int postgamma_platform_file_claim(int fd, uint64_t generation, const void *owner) {
    if (!generation || !owner) return EINVAL;
    auto file = file_object(fd);
    if (!file) return errno;
    std::lock_guard<std::mutex> lock(claims_mutex);
    for (Claim *c = claims; c; c = c->next) {
        if (c->file == file) return EBUSY;
        if (c->owner == owner) return EALREADY;
    }
    auto created = new (std::nothrow) Claim{file, generation, owner, claims};
    if (!created) return ENOMEM;
    claims = created;
    return 0;
}

int postgamma_platform_file_release(uint64_t generation, const void *owner) {
    if (!generation || !owner) return EINVAL;
    std::lock_guard<std::mutex> lock(claims_mutex);
    for (Claim **link = &claims; *link; link = &(*link)->next) {
        Claim *c = *link;
        if (c->owner != owner) continue;
        if (c->generation != generation) return ESTALE;
        *link = c->next;
        delete c;
        return 0;
    }
    return ENOENT;
}

bool postgamma_platform_directory_owned(int fd, uint64_t generation) {
    auto file = directory(fd);
    if (!file || !generation) return false;
    std::lock_guard<std::mutex> lock(claims_mutex);
    for (Claim *c = claims; c; c = c->next)
        if (c->file == file && c->generation == generation) return true;
    return false;
}

int postgamma_platform_directory_match(int fd, int directory_fd, const char *path) {
    if (!path) return EINVAL;
    auto expected = directory(fd);
    if (!expected) return errno;
    int other_fd = openat(directory_fd, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (other_fd < 0) return errno;
    auto actual = directory(other_fd);
    int status = actual ? (actual == expected ? 0 : ESTALE) : errno;
    if (close(other_fd) && !status) status = errno;
    return status;
}
