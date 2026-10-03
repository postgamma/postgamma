/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#include "postgamma/wasm_storage.h"
#include "postgamma/wasm_storage_protocol.h"
#include "backend.h"
#include "wasmfs.h"
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/wasmfs.h>
#include <cerrno>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <fcntl.h>
#include <pthread.h>

// JS receives explicit scalar arguments and byte buffers, never C++ layouts.
// The buffer belongs to a synchronously waiting caller until storage_finish.
EM_JS_DEPS(postgamma_storage_deps, "$UTF8ToString,$PThread");
EM_JS(void, postgamma_storage_dispatch,
      (void *context, int op, int id, int parent, const char *name,
       unsigned char *data, size_t length, double offset, int mode,
       double *result, int *error), {
    if (globalThis.postgammaStorageRetired) return;
    const work = (async () => {
      let adapter;
      try {
        if (!globalThis.postgammaStorageModule)
          globalThis.postgammaStorageModule = await import(new URL('src/storage/opfs.js', import.meta.url));
        adapter = globalThis.postgammaStorageModule;
        if (globalThis.postgammaStorageRetired) return;
        const text = name ? UTF8ToString(name) : "";
        let value = 0;
        if (op === adapter.StorageOp.mount) {
          globalThis.postgammaStore = new adapter.Store();
          await globalThis.postgammaStore.mount(text, data ? UTF8ToString(data) : "");
        } else {
          value = await globalThis.postgammaStore.call(op, {id, parent, name: text,
            data: new Uint8Array(wasmMemory.buffer, data, length), offset, mode});
        }
        if (globalThis.postgammaStorageRetired) return;
        new DataView(wasmMemory.buffer).setFloat64(result, value, true);
        new DataView(wasmMemory.buffer).setInt32(error, 0, true);
      } catch (cause) {
        // C initializes EIO so even an adapter import failure has a status.
        if (adapter && !globalThis.postgammaStorageRetired)
          new DataView(wasmMemory.buffer).setInt32(error, adapter.storageErrorCode(cause), true);
        err('OPFS: ' + String(cause));
      } finally {
        // The fatal path has terminated callers and must not re-enter Wasm
        // or dereference their stack-resident Completion objects.
        if (!globalThis.postgammaStorageRetired) _postgamma_storage_finish(context);
      }
    })();
    globalThis.postgammaStoragePending.set(work, {operation: op, id, bytes: length, offset});
    work.finally(() => globalThis.postgammaStoragePending.delete(work));
});

// A direct channel keeps I/O independent of synchronous control-Worker calls.
// Only a queued request sends a notification; idle owners have no polling timer.
EM_JS(void, postgamma_storage_channel_name, (char *name), {
    const bytes = crypto.getRandomValues(new Uint8Array(16));
    const text = Array.from(bytes, n => n.toString(16).padStart(2, "0")).join("");
    for (let i = 0; i < text.length; i++) HEAPU8[name + i] = text.charCodeAt(i);
    HEAPU8[name + text.length] = 0;
});
EM_JS(void, postgamma_storage_listen, (const char *name, void *queue), {
    const channel = new BroadcastChannel('postgamma-io-' + UTF8ToString(name));
    channel.onmessage = () => {
      if (!globalThis.postgammaStorageRetired) _emscripten_proxy_execute_queue(queue);
    };
    globalThis.postgammaIOListener = channel;
    globalThis.postgammaStoragePending = new Map();
    globalThis.postgammaStorageRetired = false;
    const listen = event => {
      if (!event.data?.postgammaStorageControl) return;
      const port = event.ports[0];
      globalThis.postgammaStorageControl = port;
      port.onmessage = async ({data}) => {
        if (data?.diagnostic !== undefined) {
          port.postMessage({diagnostic: data.diagnostic, snapshot: {
            retired: globalThis.postgammaStorageRetired,
            pending: [...globalThis.postgammaStoragePending.values()],
            store: globalThis.postgammaStore?.diagnostics(),
          }});
          return;
        }
        if (data?.discard !== true) return;
        globalThis.postgammaStorageRetired = true;
        channel.onmessage = null;
        channel.close();
        // Disable SDK mailbox delivery before draining browser I/O. No more
        // native proxy work may run in this discarded module.
        globalThis.onmessage = () => {};
        globalThis.postgammaStore?.interrupt();
        // Reaching this event-loop turn proves the previous native callback
        // has returned. Callers can now be terminated without stranding us
        // inside one of their native completion or proxy-queue locks.
        port.postMessage({quiesced: true});
        try {
          await Promise.allSettled([...globalThis.postgammaStoragePending.keys()]);
          const handles = await globalThis.postgammaStore?.discard() ?? 0;
          port.postMessage({released: true, handles});
        } catch (cause) {
          port.postMessage({released: false, error: String(cause)});
        }
      };
    };
    globalThis.postgammaStorageControlListener = listen;
    globalThis.addEventListener('message', listen);
});
EM_JS(void, postgamma_storage_notify, (const char *name), {
    const key = 'postgamma-io-' + UTF8ToString(name);
    if (globalThis.postgammaIOSender?.name !== key) {
      globalThis.postgammaIOSender?.close();
      globalThis.postgammaIOSender = new BroadcastChannel(key);
    }
    globalThis.postgammaIOSender.postMessage(0);
});
EM_JS(void, postgamma_storage_unlisten, (), {
    globalThis.postgammaIOListener.close();
    delete globalThis.postgammaIOListener;
    globalThis.removeEventListener('message', globalThis.postgammaStorageControlListener);
    globalThis.postgammaStorageControl?.close();
    delete globalThis.postgammaStorageControl;
});

namespace {
using namespace wasmfs;
struct Completion {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    void finish() {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        // Notify before unlocking: the caller can destroy this object as soon
        // as it observes completion and acquires the mutex.
        condition.notify_one();
    }
};
thread_local int creation_error;
std::atomic<unsigned> pending_writes;
std::atomic<unsigned> observed_io_stack_bytes;
class Storage;
Storage *active;

struct StorageRequest {
    PostgammaStorageOperation operation;
    int id = 0;
    int parent = 0;
    const char *name = nullptr;
    void *data = nullptr;
    size_t length = 0;
    double offset = 0;
    int mode = 0;
};

// Register direct delivery before publishing readiness. SDK mailbox events may
// arrive later and harmlessly observe an already drained proxying queue.
class Owner {
    emscripten::ProxyingQueue queue;
    pthread_t thread{};
    char channel[33]{};
    std::mutex mutex;
    std::condition_variable started;
    bool ready = false;
public:
    int error = 0;
    Owner() {
        postgamma_storage_channel_name(channel);
        pthread_attr_t attributes;
        error = pthread_attr_init(&attributes);
        if (error) return;
        error = pthread_attr_setstacksize(&attributes, POSTGAMMA_WASM_IO_STACK_BYTES);
        if (!error) error = pthread_create(&thread, &attributes, [](void *argument) -> void * {
            auto self = static_cast<Owner *>(argument);
            pthread_attr_t observed;
            if (pthread_getattr_np(pthread_self(), &observed) == 0) {
                size_t bytes = 0;
                if (pthread_attr_getstacksize(&observed, &bytes) == 0)
                    observed_io_stack_bytes.store(bytes, std::memory_order_release);
                pthread_attr_destroy(&observed);
            }
            postgamma_storage_listen(self->channel, self->queue.queue);
            // Register while the host waits for readiness, before any mount
            // can acquire a physical OPFS handle. Cleanup uses a JS-only port.
            MAIN_THREAD_EM_ASM({
                Module['postgammaStorageOwner']?.(PThread.pthreads[$0]);
            }, (uintptr_t) pthread_self());
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->ready = true;
            }
            self->started.notify_one();
            emscripten_exit_with_live_runtime();
            return nullptr;
        }, this);
        pthread_attr_destroy(&attributes);
        if (!error) {
            std::unique_lock<std::mutex> lock(mutex);
            started.wait(lock, [&] {return ready;});
        }
    }
    ~Owner() {
        if (!error) {
            call([](Completion *completion) {
                postgamma_storage_unlisten();
                MAIN_THREAD_EM_ASM({ Module['postgammaStorageOwner']?.(); });
                completion->finish();
            });
            pthread_cancel(thread);
            pthread_join(thread, nullptr);
        }
    }
    template<typename Function> bool call(Function function) {
        Completion completion;
        if (!queue.proxyAsync(thread, [function, &completion] {function(&completion);})) return false;
        postgamma_storage_notify(channel);
        std::unique_lock<std::mutex> lock(completion.mutex);
        completion.condition.wait(lock, [&] {return completion.done;});
        return true;
    }
};

class Storage : public wasmfs::Backend {
public:
    std::unique_ptr<Owner> worker;
    Storage() : worker(std::make_unique<Owner>()) {}
    double call(const StorageRequest &request) {
        if (!worker) return -EIO;
        if (worker->error) return -worker->error;
        double result = 0;
        int error = EIO;
        if (request.operation == PGW_STORAGE_WRITE) pending_writes.fetch_add(1, std::memory_order_release);
        bool dispatched = worker->call([&](Completion *completion) {
            postgamma_storage_dispatch(completion, request.operation, request.id, request.parent, request.name,
                static_cast<unsigned char *>(request.data), request.length, request.offset, request.mode, &result, &error);
        });
        if (request.operation == PGW_STORAGE_WRITE) pending_writes.fetch_sub(1, std::memory_order_release);
        return !dispatched ? -EIO : error ? -error : result;
    }
    double call(PostgammaStorageOperation operation, int id = 0) {return call(StorageRequest{operation, id});}
    std::shared_ptr<File> node(const uint32_t *info, off_t known_size = -1);
    std::shared_ptr<DataFile> createFile(mode_t) override {return nullptr;}
    std::shared_ptr<Directory> createDirectory(mode_t mode) override;
    std::shared_ptr<Symlink> createSymlink(std::string) override {return nullptr;}
};

class Data : public DataFile {
    // WasmFS preserves File identity in its dcache and holds File::mutex for
    // these methods. The exclusive storage owner has no out-of-band writers.
    // Avoid an owner roundtrip for every stat/seek of an unchanged file.
    off_t size;
public:
    Storage &storage;
    int id;
    Data(Storage &storage, int id, mode_t mode, off_t known_size)
        : DataFile(mode, &storage), size(known_size), storage(storage), id(id) {}
    off_t getSize() override {
        if (size < 0) size = storage.call(PGW_STORAGE_SIZE, id);
        return size;
    }
    int setSize(off_t size) override {
        StorageRequest request{PGW_STORAGE_TRUNCATE, id};
        request.offset = size;
        int result = storage.call(request);
        this->size = result == 0 ? size : -1;
        return result;
    }
    int open(oflags_t) override {return storage.call(PGW_STORAGE_OPEN, id);}
    int close() override {return storage.call(PGW_STORAGE_CLOSE, id);}
    ssize_t read(uint8_t *buffer, size_t length, off_t offset) override {
        StorageRequest request{PGW_STORAGE_READ, id};
        request.data = buffer; request.length = length; request.offset = offset;
        return storage.call(request);
    }
    ssize_t write(const uint8_t *buffer, size_t length, off_t offset) override {
        StorageRequest request{PGW_STORAGE_WRITE, id};
        request.data = const_cast<uint8_t *>(buffer); request.length = length; request.offset = offset;
        ssize_t result = storage.call(request);
        // A failed browser write can have changed the file before throwing.
        // Refresh on the next stat rather than retaining an uncertain length.
        if (result < 0) size = -1;
        else if (size >= 0 && result > 0) size = std::max(size, offset + result);
        return result;
    }
    int flush() override {return storage.call(PGW_STORAGE_FLUSH, id);}
};

class Dir : public Directory {
public:
    Storage &storage;
    int id;
    Dir(Storage &storage, int id, mode_t mode) : Directory(mode, &storage), storage(storage), id(id) {}
    std::shared_ptr<File> getChild(const std::string &name) override {
        uint32_t info[3];
        StorageRequest request{PGW_STORAGE_LOOKUP, id};
        request.name = name.c_str(); request.data = info; request.length = sizeof(info);
        double result = storage.call(request);
        if (result < 0) creation_error = -result;
        return result > 0 ? storage.node(info) : nullptr;
    }
    std::shared_ptr<File> insert(PostgammaStorageOperation op, const std::string &name, mode_t mode) {
        uint32_t info[3];
        StorageRequest request{op, id};
        request.name = name.c_str(); request.data = info; request.length = sizeof(info); request.mode = mode;
        double result = storage.call(request);
        if (result < 0) {creation_error = -result; return nullptr;}
        return storage.node(info, op == PGW_STORAGE_CREATE_FILE ? 0 : -1);
    }
    std::shared_ptr<DataFile> insertDataFile(const std::string &name, mode_t mode) override {
        return std::static_pointer_cast<DataFile>(insert(PGW_STORAGE_CREATE_FILE, name, mode));
    }
    std::shared_ptr<Directory> insertDirectory(const std::string &name, mode_t mode) override {
        return std::static_pointer_cast<Directory>(insert(PGW_STORAGE_CREATE_DIRECTORY, name, mode));
    }
    std::shared_ptr<Symlink> insertSymlink(const std::string &, const std::string &) override {return nullptr;}
    int insertMove(const std::string &name, std::shared_ptr<File> file) override {
        if (file->getBackend() != &storage) return -EXDEV;
        int source = file->is<DataFile>() ? static_cast<Data *>(file.get())->id : static_cast<Dir *>(file.get())->id;
        StorageRequest request{PGW_STORAGE_MOVE, source};
        request.parent = id; request.name = name.c_str();
        return storage.call(request);
    }
    int removeChild(const std::string &name) override {
        StorageRequest request{PGW_STORAGE_REMOVE};
        request.parent = id; request.name = name.c_str();
        return storage.call(request);
    }
    MaybeEntries getEntries() override {
        int size = storage.call(PGW_STORAGE_LIST, id);
        if (size < 0) return {size};
        if (size > 4 * 1024 * 1024) return {-ENOMEM};
        std::vector<uint8_t> buffer(size);
        StorageRequest request{PGW_STORAGE_LIST, id};
        request.data = buffer.data(); request.length = buffer.size();
        int result = size ? storage.call(request) : 0;
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

std::shared_ptr<File> Storage::node(const uint32_t *info, off_t known_size) {
    if (info[1] == 1) return std::make_shared<Data>(*this, info[0], info[2], known_size);
    return std::make_shared<Dir>(*this, info[0], info[2]);
}
std::shared_ptr<Directory> Storage::createDirectory(mode_t mode) {return std::make_shared<Dir>(*this, 1, mode);}
}

extern "C" void postgamma_storage_finish(void *context) {
    static_cast<Completion *>(context)->finish();
}

extern "C" int postgamma_wasm_storage_mount(const char *name, const char *test_channel) {
    if (active) return EBUSY;
    observed_io_stack_bytes.store(0, std::memory_order_release);
    auto storage = std::make_unique<Storage>();
    StorageRequest request{PGW_STORAGE_MOUNT};
    request.name = name; request.data = const_cast<char *>(test_channel);
    int status = storage->call(request);
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
extern "C" EMSCRIPTEN_KEEPALIVE unsigned postgamma_wasm_storage_stack_bytes(void) {
    return observed_io_stack_bytes.load(std::memory_order_acquire);
}
extern "C" int postgamma_wasm_storage_close(void) {
    if (!active) return 0;
    int status = active->call(PGW_STORAGE_SHUTDOWN);
    if (status < 0) return -status;
    active->worker.reset();
    active = nullptr;
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
            return -active->call(PGW_STORAGE_FLUSH_DIRECTORY, static_cast<Dir *>(file.get())->id);
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
