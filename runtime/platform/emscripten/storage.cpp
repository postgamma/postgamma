/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include "postgamma/wasm_storage.h"
#include "backend.h"
#include "wasmfs.h"
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/wasmfs.h>
#include <cerrno>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <fcntl.h>
#include <pthread.h>

// JS receives explicit scalar arguments and byte buffers, never C++ layouts.
// The buffer belongs to a synchronously waiting caller until proxy_finish.
EM_JS_DEPS(postgamma_storage_deps, "$UTF8ToString");
EM_JS(void, postgamma_storage_dispatch,
      (void *context, int op, int id, int parent, const char *name,
       unsigned char *data, size_t length, double offset, int mode,
       double *result, int *error), {
    (async () => {
      let value = 0, code = 0;
      try {
        const text = name ? UTF8ToString(name) : "";
        if (op === 0) {
          const {Store} = await import(new URL('src/storage/opfs.js', import.meta.url));
          globalThis.postgammaStore = new Store();
          await globalThis.postgammaStore.mount(text, data ? UTF8ToString(data) : "");
        } else {
          value = await globalThis.postgammaStore.call(op, id, parent, text,
            new Uint8Array(wasmMemory.buffer, data, length), offset, mode);
        }
      } catch (cause) {
        const codes = {QuotaExceededError: 51, NotFoundError: 44,
          NoModificationAllowedError: 10, InvalidStateError: 10,
          NotAllowedError: 2, TypeError: 28, NotSupportedError: 58};
        code = codes[cause.name] || 29;
        err('OPFS: ' + String(cause));
      }
      new DataView(wasmMemory.buffer).setFloat64(result, value, true);
      new DataView(wasmMemory.buffer).setInt32(error, code, true);
      _emscripten_proxy_finish(context);
    })();
});

namespace {
using namespace wasmfs;
thread_local int creation_error;
std::atomic<unsigned> pending_writes;
class Storage;
Storage *active;

// The control Worker always remains asynchronous, so proxy messages can make
// progress without a polling heartbeat. A timer surviving pthread cancellation
// would otherwise run on an idle or reused SDK Worker after close.
class Owner {
    emscripten::ProxyingQueue queue;
    pthread_t thread{};
    std::mutex mutex;
    std::condition_variable started;
    bool ready = false;
public:
    int error = 0;
    size_t stack_bytes = 0;
    Owner() {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
        error = pthread_create(&thread, &attributes, [](void *argument) -> void * {
            auto self = static_cast<Owner *>(argument);
            pthread_attr_t observed;
            if (pthread_getattr_np(pthread_self(), &observed) == 0) {
                pthread_attr_getstacksize(&observed, &self->stack_bytes);
                pthread_attr_destroy(&observed);
            }
            {std::lock_guard<std::mutex> lock(self->mutex); self->ready = true;}
            self->started.notify_one();
            emscripten_exit_with_live_runtime();
            return nullptr;
        }, this);
        pthread_attr_destroy(&attributes);
        if (!error) {std::unique_lock<std::mutex> lock(mutex); started.wait(lock, [&] {return ready;});}
    }
    ~Owner() {
        if (!error) {pthread_cancel(thread); pthread_join(thread, nullptr);}
    }
    template<typename Function> void call(Function function) {
        queue.proxySyncWithCtx(thread, function);
    }
};

class Storage : public wasmfs::Backend {
public:
    std::unique_ptr<Owner> worker;
    Storage() : worker(std::make_unique<Owner>()) {}
    double call(int op, int id = 0, int parent = 0, const char *name = nullptr,
                void *data = nullptr, size_t length = 0, double offset = 0, int mode = 0) {
        if (!worker) return -EIO;
        if (worker->error) return -worker->error;
        double result = 0;
        int error = 0;
        if (op == 10) pending_writes.fetch_add(1, std::memory_order_release);
        worker->call([&](auto context) {
            postgamma_storage_dispatch(context.ctx, op, id, parent, name,
                static_cast<unsigned char *>(data), length, offset, mode, &result, &error);
        });
        if (op == 10) pending_writes.fetch_sub(1, std::memory_order_release);
        return error ? -error : result;
    }
    std::shared_ptr<File> node(const uint32_t *info);
    std::shared_ptr<DataFile> createFile(mode_t) override {return nullptr;}
    std::shared_ptr<Directory> createDirectory(mode_t mode) override;
    std::shared_ptr<Symlink> createSymlink(std::string) override {return nullptr;}
};

class Data : public DataFile {
public:
    Storage &storage;
    int id;
    Data(Storage &storage, int id, mode_t mode) : DataFile(mode, &storage), storage(storage), id(id) {}
    off_t getSize() override {return storage.call(14, id);}
    int setSize(off_t size) override {return storage.call(15, id, 0, nullptr, nullptr, 0, size);}
    int open(oflags_t) override {return storage.call(7, id);}
    int close() override {return storage.call(8, id);}
    ssize_t read(uint8_t *buffer, size_t length, off_t offset) override {
        return storage.call(9, id, 0, nullptr, buffer, length, offset);
    }
    ssize_t write(const uint8_t *buffer, size_t length, off_t offset) override {
        return storage.call(10, id, 0, nullptr, const_cast<uint8_t *>(buffer), length, offset);
    }
    int flush() override {return storage.call(11, id);}
};

class Dir : public Directory {
public:
    Storage &storage;
    int id;
    Dir(Storage &storage, int id, mode_t mode) : Directory(mode, &storage), storage(storage), id(id) {}
    std::shared_ptr<File> getChild(const std::string &name) override {
        uint32_t info[3];
        double result = storage.call(1, id, 0, name.c_str(), info, sizeof(info));
        if (result < 0) creation_error = -result;
        return result > 0 ? storage.node(info) : nullptr;
    }
    std::shared_ptr<File> insert(int op, const std::string &name, mode_t mode) {
        uint32_t info[3];
        double result = storage.call(op, id, 0, name.c_str(), info, sizeof(info), 0, mode);
        if (result < 0) {creation_error = -result; return nullptr;}
        return storage.node(info);
    }
    std::shared_ptr<DataFile> insertDataFile(const std::string &name, mode_t mode) override {
        return std::static_pointer_cast<DataFile>(insert(2, name, mode));
    }
    std::shared_ptr<Directory> insertDirectory(const std::string &name, mode_t mode) override {
        return std::static_pointer_cast<Directory>(insert(3, name, mode));
    }
    std::shared_ptr<Symlink> insertSymlink(const std::string &, const std::string &) override {return nullptr;}
    int insertMove(const std::string &name, std::shared_ptr<File> file) override {
        if (file->getBackend() != &storage) return -EXDEV;
        int source = file->is<DataFile>() ? static_cast<Data *>(file.get())->id : static_cast<Dir *>(file.get())->id;
        return storage.call(4, source, id, name.c_str());
    }
    int removeChild(const std::string &name) override {return storage.call(5, 0, id, name.c_str());}
    MaybeEntries getEntries() override {
        int size = storage.call(6, id);
        if (size < 0) return {size};
        if (size > 4 * 1024 * 1024) return {-ENOMEM};
        std::vector<uint8_t> buffer(size);
        int result = size ? storage.call(6, id, 0, nullptr, buffer.data(), buffer.size()) : 0;
        if (result < 0) return {result};
        std::vector<Entry> entries;
        size_t at = 0;
        while (at < buffer.size()) {
            uint32_t info[4];
            if (buffer.size() - at < sizeof(info)) return {-EIO};
            memcpy(info, buffer.data() + at, sizeof(info)); at += sizeof(info);
            if (info[3] > buffer.size() - at) return {-EIO};
            entries.push_back({std::string(reinterpret_cast<char *>(buffer.data() + at), info[3]), FileKind(info[1]), info[0]});
            at += info[3];
        }
        return {entries};
    }
    ssize_t getNumEntries() override {
        auto entries = getEntries();
        return entries.getError() ? entries.getError() : entries->size();
    }
};

std::shared_ptr<File> Storage::node(const uint32_t *info) {
    if (info[1] == 1) return std::make_shared<Data>(*this, info[0], info[2]);
    return std::make_shared<Dir>(*this, info[0], info[2]);
}
std::shared_ptr<Directory> Storage::createDirectory(mode_t mode) {return std::make_shared<Dir>(*this, 1, mode);}
}

extern "C" int postgamma_wasm_storage_mount(const char *name, const char *test_channel) {
    if (active) return EBUSY;
    auto storage = std::make_unique<Storage>();
    if (!storage->worker->error && storage->worker->stack_bytes != 2 * 1024 * 1024) return EIO;
    fprintf(stderr, "POSTGAMMA_STORAGE io_stack_bytes=%zu\n", storage->worker->stack_bytes);
    int status = storage->call(0, 0, 0, name, const_cast<char *>(test_channel));
    if (status < 0) return -status;
    active = storage.get();
    auto backend = wasmfs::wasmFS.addBackend(std::move(storage));
    status = wasmfs_create_directory("/persistent", 0700, reinterpret_cast<::backend_t>(backend));
    if (status < 0) {postgamma_wasm_storage_close(); return -status;}
    return 0;
}
extern "C" unsigned postgamma_wasm_storage_pending_writes(void) {
    return pending_writes.load(std::memory_order_acquire);
}
extern "C" int postgamma_wasm_storage_close(void) {
    if (!active) return 0;
    int status = active->call(12);
    if (status < 0) return -status;
    active->worker.reset(); active = nullptr;
    return 0;
}

// The fixed WasmFS directory interface has no flush virtual method. These
// target-only syscall wrappers supply it and retain backend creation errno.
extern "C" __wasi_errno_t __real___wasi_fd_sync(__wasi_fd_t);
extern "C" __wasi_errno_t __wrap___wasi_fd_sync(__wasi_fd_t fd) {
    auto entry = wasmfs::wasmFS.getFileTable().locked().getEntry(fd);
    if (entry) {
        auto file = entry->locked().getFile();
        if (active && file->getBackend() == active && file->is<wasmfs::Directory>())
            return -active->call(13, static_cast<Dir *>(file.get())->id);
    }
    return __real___wasi_fd_sync(fd);
}
extern "C" int __wrap___syscall_fdatasync(int fd) {return -__wrap___wasi_fd_sync(fd);}
extern "C" int __real___syscall_openat(int, const char *, int, ...);
extern "C" int __wrap___syscall_openat(int dirfd, const char *path, int flags, ...) {
    va_list args;
    va_start(args, flags); int mode = va_arg(args, int); va_end(args);
    creation_error = 0;
    int result = __real___syscall_openat(dirfd, path, flags, mode);
    return result < 0 && creation_error ? -creation_error : result;
}
extern "C" int __real___syscall_mkdirat(int, const char *, mode_t);
extern "C" int __wrap___syscall_mkdirat(int dirfd, const char *path, mode_t mode) {
    creation_error = 0;
    int result = __real___syscall_mkdirat(dirfd, path, mode);
    return result < 0 && creation_error ? -creation_error : result;
}
