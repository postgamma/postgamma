/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
/* Browser host for the unmodified upstream psql frontend. SQL and backslash
 * commands execute in psql; only process I/O and libpq admission are adapted.
 * This closure is namespaced and single-use, never shared by SQL sessions. */
#include "postgres_fe.h"
#include "psql.h"
#include "postgamma/private/public_runtime.h"
#include "postgamma/private/libpq_memory_adapter.h"
#define POSTGAMMA_WASM_LOGICAL_ALLOC_IMPLEMENTATION
#include "logical_alloc.h"
#include "libpq-fe.h"
#include "port.h"
#include "fe_utils/cancel.h"
#include "common.h"
#include <errno.h>
#include <locale.h>
#include <getopt.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/time.h>

/* Explicit calls into the separately namespaced private libpq. */
extern __typeof__(PQconnectStartParams) native_connect __asm__("postgamma_wasm_libpq_PQconnectStartParams");
extern __typeof__(PQfinish) native_finish __asm__("postgamma_wasm_libpq_PQfinish");
extern __typeof__(PQerrorMessage) native_error __asm__("postgamma_wasm_libpq_PQerrorMessage");
extern __typeof__(PQgetResult) native_result __asm__("postgamma_wasm_libpq_PQgetResult");
extern int postgamma_psql_main(int, char **);
#define IO_BYTES 65536
typedef struct Ring { unsigned char data[IO_BYTES]; size_t start, size; } Ring;
typedef struct Connection {
    pgm_connection *public;
    PGconn *native;
    uint64_t generation;
    struct Connection *next;
} Connection;
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    pgm_instance *instance;
    Ring input, output;
    bool started, closing, finished, joined, waiting, interrupt, query_interrupt;
    int code;
    FILE *in, *out;
    Connection *connections, *active;
    PGconn *bad;
    char error[1024];
    sigjmp_buf exit_jump;
} terminal = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

static size_t take(Ring *ring, void *data, size_t size) {
    if (size > ring->size) size = ring->size;
    for (size_t i = 0; i < size; i++) ((unsigned char *)data)[i] = ring->data[(ring->start + i) % IO_BYTES];
    ring->start = (ring->start + size) % IO_BYTES; ring->size -= size;
    return size;
}
static void append(Ring *ring, const void *data, size_t size) {
    for (size_t i = 0; i < size; i++) ring->data[(ring->start + ring->size + i) % IO_BYTES] = ((const unsigned char *)data)[i];
    ring->size += size;
}
static ssize_t output_write(void *unused, const char *data, size_t size) {
    (void)unused;
    size_t offset = 0;
    pthread_mutex_lock(&terminal.mutex);
    while (offset < size && !terminal.closing) {
        while (terminal.output.size == IO_BYTES && !terminal.closing) pthread_cond_wait(&terminal.changed, &terminal.mutex);
        size_t count = IO_BYTES - terminal.output.size;
        if (count > size - offset) count = size - offset;
        append(&terminal.output, data + offset, count); offset += count;
    }
    pthread_mutex_unlock(&terminal.mutex);
    return size; /* Closing discards output, unblocking the frontend. */
}
static ssize_t input_read(void *unused, char *data, size_t size) {
    (void)unused;
    pthread_mutex_lock(&terminal.mutex);
    terminal.waiting = true;
    while (!terminal.input.size && !terminal.closing && !terminal.interrupt) pthread_cond_wait(&terminal.changed, &terminal.mutex);
    terminal.waiting = false;
    bool interrupt = terminal.interrupt; terminal.interrupt = false;
    size_t count = terminal.closing ? 0 : take(&terminal.input, data, size);
    pthread_mutex_unlock(&terminal.mutex);
    if (interrupt && sigint_interrupt_enabled) {
        sigint_interrupt_enabled = false;
        siglongjmp(sigint_interrupt_jmp, 1);
    }
    return count;
}
FILE *postgamma_psql_stdin(void) {return terminal.in;}
FILE *postgamma_psql_stdout(void) {return terminal.out;}
int postgamma_psql_fileno(FILE *file) {
    return file == terminal.in ? 0 : file == terminal.out ? 1 : fileno(file);
}
char *postgamma_psql_fgets(char *data, int capacity, FILE *file) {
    if (file != terminal.in) return fgets(data, capacity, file);
    /* Avoid longjmp across a libc FILE lock when canceling multiline input. */
    int count = 0;
    while (count < capacity - 1) {
        char value;
        if (input_read(NULL, &value, 1) == 0) break;
        data[count++] = value;
        if (value == '\n') break;
    }
    data[count] = 0; return count ? data : NULL;
}
int isatty(int fd) {return fd >= 0 && fd <= 2;}

static void release_active(void) {
    pthread_mutex_lock(&terminal.mutex);
    Connection *connection = terminal.active;
    terminal.active = NULL;
    if (connection && postgamma_private_libpq_tool_release(connection->public->private_connection, connection->generation)) abort();
    pthread_mutex_unlock(&terminal.mutex);
}
static void borrow(Connection *connection) {
    if (!connection) return;
    void *native = NULL;
    if (postgamma_private_libpq_tool_borrow(connection->public->private_connection, connection->generation, &native)) abort();
    pthread_mutex_lock(&terminal.mutex);
    terminal.active = connection;
    pthread_mutex_unlock(&terminal.mutex);
}
static const char *keyword(const char *const *keys, const char *const *values, const char *name) {
    const char *result = NULL;
    for (size_t i = 0; keys && keys[i]; i++) if (!strcmp(keys[i], name) && values[i] && *values[i]) result = values[i];
    return result;
}
PGconn *PQconnectdbParams(const char *const *keys, const char *const *values, int expand) {
    pgm_connection_options options = PGM_CONNECTION_OPTIONS_INIT;
    pgm_error *error = NULL;
    Connection *previous = terminal.active, *connection = calloc(1, sizeof(*connection));
    release_active();
    if (!connection) abort();
    options.database = keyword(keys, values, "dbname");
    options.user = keyword(keys, values, "user");
    options.application_name = "psql";
    if (!options.database) options.database = "postgres";
    if (!options.user) options.user = "postgamma";
    /* This terminal connects only to its owning in-memory instance. */
    const char *host = keyword(keys, values, "host");
    bool unsupported = keyword(keys, values, "hostaddr") || (host && strcmp(host, "postgamma-memory.invalid"));
    if (expand && (strchr(options.database, '=') || strstr(options.database, "://"))) unsupported = true;
    pgm_status status = unsupported ? PGM_STATUS_INVALID_ARGUMENT : pgm_connection_open(terminal.instance, &options, &connection->public, &error);
    if (status) {
        snprintf(terminal.error, sizeof(terminal.error), "%s\n", unsupported ?
            "This browser terminal accepts a local database name; network connection strings are unavailable" :
            error ? (*pgm_error_detail(error) ? pgm_error_detail(error) : pgm_error_message(error)) : "Could not open database");
        pgm_error_free(error); free(connection);
        /* Construct a genuine failed PGconn without attempting network I/O. */
        const char *bad_keys[] = {"postgamma_invalid_option", NULL}, *bad_values[] = {"1", NULL};
        terminal.bad = native_connect(bad_keys, bad_values, 0);
        borrow(previous); return terminal.bad;
    }
    connection->generation = atomic_fetch_add(&terminal.instance->next_request_generation, 1);
    void *native = NULL;
    if (postgamma_private_libpq_tool_borrow(connection->public->private_connection, connection->generation, &native)) abort();
    connection->native = native;
    connection->next = terminal.connections; terminal.connections = connection;
    pthread_mutex_lock(&terminal.mutex); terminal.active = connection; pthread_mutex_unlock(&terminal.mutex);
    return connection->native;
}
PGconn *PQconnectStartParams(const char *const *keys, const char *const *values, int expand) {return PQconnectdbParams(keys, values, expand);}
/* Admission above completes the handshake on the psql thread. No network fd
 * is exposed to psql's asynchronous connection loop. */
int PQsocket(const PGconn *connection) {(void)connection; return -1;}
char *PQerrorMessage(const PGconn *connection) {return connection == terminal.bad && connection ? terminal.error : native_error(connection);}
void PQfinish(PGconn *native) {
    if (!native) return;
    Connection *active = terminal.active;
    release_active();
    if (native == terminal.bad) {native_finish(native); terminal.bad = NULL;}
    else {
        Connection **slot = &terminal.connections;
        while (*slot && (*slot)->native != native) slot = &(*slot)->next;
        if (!*slot) abort();
        Connection *connection = *slot; *slot = connection->next;
        if (active == connection) active = NULL;
        if (pgm_connection_close(connection->public, 10000, NULL)) abort();
        free(connection);
    }
    borrow(active);
}
PGcancel *PQgetCancel(PGconn *connection) {(void)connection; return NULL;}
void PQfreeCancel(PGcancel *cancel) {(void)cancel;}
PGresult *PQgetResult(PGconn *connection) {
    PGresult *result = native_result(connection);
    pthread_mutex_lock(&terminal.mutex);
    bool canceled = terminal.query_interrupt; terminal.query_interrupt = false;
    pthread_mutex_unlock(&terminal.mutex);
    if (canceled) cancel_pressed = true; /* Only the owning psql thread writes this global. */
    return result;
}
/* Do not install process signal handlers, mutate process locale/environment,
 * or attempt to launch a shell/editor from a browser frontend. */
void postgamma_psql_set_pglocale_pgservice(const char *program, const char *domain) {
    /* psql is linked into this module, not installed as a host executable.
     * NLS is disabled, locale is fixed, and private memory libpq has no system
     * service directory. Skip the native executable-prefix probe at startup;
     * -X separately disables psqlrc discovery. Do not suppress diagnostics. */
    (void)program; (void)domain;
}
void pqsignal(int number, pqsigfunc handler) {(void)number; (void)handler;}
char *setlocale(int category, const char *locale) {(void)category; (void)locale; return "C";}
int setenv(const char *name, const char *value, int overwrite) {(void)name; (void)value; (void)overwrite; return 0;}
int unsetenv(const char *name) {(void)name; return 0;}
int system(const char *command) {(void)command; errno = ENOTSUP; return -1;}
FILE *popen(const char *command, const char *mode) {(void)command; (void)mode; errno = ENOTSUP; return NULL;}
int pclose(FILE *file) {(void)file; errno = ENOTSUP; return -1;}
int setitimer(int which, const struct itimerval *value, struct itimerval *old) {
    (void)which; (void)value; (void)old; errno = ENOTSUP; return -1;
}
int sigprocmask(int how, const sigset_t *set, sigset_t *old) {(void)how; (void)set; if (old) sigemptyset(old); return 0;}
int atexit(void (*callback)(void)) {(void)callback; return 0;}
pg_noreturn void exit(int code) {terminal.code = code; siglongjmp(terminal.exit_jump, 1);}

static void *run(void *unused) {
    (void)unused;
    postgamma_wasm_logical_alloc_begin();
    cookie_io_functions_t in = {.read = input_read}, out = {.write = output_write};
    terminal.in = fopencookie(NULL, "r", in); terminal.out = fopencookie(NULL, "w", out);
    if (!terminal.in || !terminal.out) terminal.code = 1;
    else {
        setvbuf(terminal.in, NULL, _IONBF, 0); setvbuf(terminal.out, NULL, _IONBF, 0);
        /* Isolate getopt state: no other frontend may run in this instance. */
        optind = 0;
        char *arguments[] = {"psql", "-X", "-w", "-U", "postgamma", "-d", "postgres", "-P", "pager=off", NULL};
        if (!sigsetjmp(terminal.exit_jump, 1)) terminal.code = postgamma_psql_main(9, arguments);
    }
    while (terminal.connections) PQfinish(terminal.connections->native);
    if (terminal.bad) PQfinish(terminal.bad);
    if (terminal.in) fclose(terminal.in);
    if (terminal.out) fclose(terminal.out);
    postgamma_wasm_logical_alloc_end();
    pthread_mutex_lock(&terminal.mutex); terminal.finished = true; terminal.waiting = false;
    pthread_cond_broadcast(&terminal.changed); pthread_mutex_unlock(&terminal.mutex);
    return NULL;
}
int postgamma_wasm_psql_start(pgm_instance *instance) {
    if (terminal.started) return EBUSY;
    terminal.instance = instance;
    pthread_attr_t attributes;
    int status = pthread_attr_init(&attributes);
    if (status) return status;
    status = pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
    if (!status) status = pthread_create(&terminal.thread, &attributes, run, NULL);
    pthread_attr_destroy(&attributes);
    if (!status) terminal.started = true;
    return status;
}
size_t postgamma_wasm_psql_read(void *data, size_t capacity, unsigned *state, int *code) {
    pthread_mutex_lock(&terminal.mutex);
    size_t size = take(&terminal.output, data, capacity);
    *state = terminal.finished && !terminal.output.size ? 2 : terminal.waiting && !terminal.output.size && !terminal.input.size ? 1 : 0;
    *code = terminal.finished ? terminal.code : 0;
    pthread_cond_broadcast(&terminal.changed); pthread_mutex_unlock(&terminal.mutex); return size;
}
int postgamma_wasm_psql_write(const void *data, size_t size) {
    pthread_mutex_lock(&terminal.mutex);
    int status = terminal.finished || terminal.closing ? EPIPE : size > IO_BYTES - terminal.input.size ? EAGAIN : 0;
    if (!status) append(&terminal.input, data, size);
    pthread_cond_broadcast(&terminal.changed); pthread_mutex_unlock(&terminal.mutex); return status;
}
void postgamma_wasm_psql_interrupt(void) {
    pthread_mutex_lock(&terminal.mutex);
    terminal.input.size = 0;
    if (terminal.waiting) terminal.interrupt = true;
    else if (terminal.active) {
        terminal.query_interrupt = true;
        postgamma_private_libpq_cancel(terminal.active->public->private_connection, terminal.active->generation);
    }
    pthread_cond_broadcast(&terminal.changed); pthread_mutex_unlock(&terminal.mutex);
}
int postgamma_wasm_psql_close(void) {
    if (!terminal.started || terminal.joined) return 0;
    pthread_mutex_lock(&terminal.mutex);
    terminal.closing = true;
    if (terminal.active) postgamma_private_libpq_cancel(terminal.active->public->private_connection, terminal.active->generation);
    bool finished = terminal.finished;
    pthread_cond_broadcast(&terminal.changed); pthread_mutex_unlock(&terminal.mutex);
    if (!finished) return EAGAIN;
    int status = pthread_tryjoin_np(terminal.thread, NULL);
    if (!status) terminal.joined = true;
    return status == EBUSY ? EAGAIN : status;
}
