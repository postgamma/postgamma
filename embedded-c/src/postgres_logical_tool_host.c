/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgres_fe.h"

#include "postgamma/private/logical_tool_host.h"
#include "postgamma/private/public_runtime.h"
#include "postgamma/tool_state_runtime.h"

#include "libpq-fe.h"
#include "port.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


#define POSTGAMMA_LOGICAL_TOOL_CLOSE_TIMEOUT_MS INT64_C(30000)
#define POSTGAMMA_LOGICAL_TOOL_ARGUMENT_CAPACITY 24
#define POSTGAMMA_LOGICAL_TOOL_WORKER_MAGIC UINT64_C(0x50474D4C54574B31)
#define POSTGAMMA_LOGICAL_TOOL_STACK_SIZE (8U * 1024U * 1024U)
#define POSTGAMMA_LOGICAL_TOOL_GUARD_SIZE (64U * 1024U)
#define POSTGAMMA_LOGICAL_TOOL_DIAGNOSTIC_CAPACITY 1024U


struct PostgammaLogicalToolWorker
{
	uint64_t	magic;
	pthread_mutex_t mutex;
	pthread_t	thread;
	PostgammaLogicalToolOptions options;
	PostgammaLogicalToolKind kind;
	PostgammaLogicalToolCompletionFunction completion;
	void	   *completion_argument;
	PostgammaPrivateLibpqConnection *active_private_connection;
	uint64_t	active_request_generation;
	bool		cancel_requested;
};


typedef struct PostgammaLogicalToolContext
{
	const PostgammaLogicalToolOptions *options;
	PostgammaLogicalToolResult *result;
	PostgammaLogicalToolKind kind;
	PostgammaLogicalToolWorker *worker;
	void	   *tool_state;
	pgm_connection *connection;
	PostgammaPrivateLibpqConnection *private_connection;
	PGconn	   *native_connection;
	uint64_t	request_generation;
	mode_t		logical_umask;
	FILE	   *diagnostic_stream;
	char		diagnostic_data[POSTGAMMA_LOGICAL_TOOL_DIAGNOSTIC_CAPACITY];
	size_t		diagnostic_size;
	sigjmp_buf	exit_jump;
	int			exit_code;
	int			failure_status;
	bool		jump_ready;
	bool		tool_state_bound;
	bool		connection_borrowed;
	bool		archive_opened;
	bool		unwinding;
} PostgammaLogicalToolContext;


static _Thread_local PostgammaLogicalToolContext *PostgammaCurrentLogicalTool;
static pthread_mutex_t PostgammaLogicalToolExecutionMutex =
	PTHREAD_MUTEX_INITIALIZER;


extern int postgamma_pg19_pg_dump_main(int argc, char **argv);
extern int postgamma_pg19_pg_restore_main(int argc, char **argv);


static bool logical_tool_options_valid(
	const PostgammaLogicalToolOptions *options);
static pg_noreturn void logical_tool_fail(int status, int exit_code);
static void logical_tool_state_failure(void *argument, int status);
static const char *logical_tool_keyword(
	const char *const *keywords, const char *const *values,
	const char *name);
static int logical_tool_close_connection(PostgammaLogicalToolContext *context);
static void logical_tool_cleanup(PostgammaLogicalToolContext *context);
static int logical_tool_run(
	PostgammaLogicalToolKind kind,
	const PostgammaLogicalToolOptions *options,
	PostgammaLogicalToolResult *result,
	PostgammaLogicalToolWorker *worker);
static void logical_tool_set_result(
	PostgammaLogicalToolResult *result, int status,
	int postgres_exit_code, uint64_t request_generation,
	const char *message);
static void logical_tool_capture_result(
	PostgammaLogicalToolContext *context, int status);
static int logical_tool_open_diagnostic(
	PostgammaLogicalToolContext *context);
static ssize_t logical_tool_diagnostic_write(
	void *cookie, const char *buffer, size_t size);
static int logical_tool_diagnostic_close(void *cookie);
static ssize_t logical_tool_cookie_read(
	void *cookie, char *buffer, size_t size);
static ssize_t logical_tool_cookie_write(
	void *cookie, const char *buffer, size_t size);
static int logical_tool_cookie_seek(
	void *cookie, off64_t *offset, int whence);
static int logical_tool_cookie_close(void *cookie);
static void *logical_tool_worker_main(void *argument);
static bool logical_tool_worker_valid(
	const PostgammaLogicalToolWorker *worker);


static bool
logical_tool_options_valid(const PostgammaLogicalToolOptions *options)
{
	return options != NULL && options->struct_size == sizeof(*options) &&
		instance_is_valid(options->instance) && options->database != NULL &&
		options->database[0] != '\0' && options->user != NULL &&
		options->user[0] != '\0' && options->archive_path != NULL &&
		options->archive_path[0] == '/' &&
		(options->flags & ~PGM_LOGICAL_FLAGS_ALL) == 0 &&
		(options->flags & (PGM_LOGICAL_SCHEMA_ONLY |
			PGM_LOGICAL_DATA_ONLY)) !=
		(PGM_LOGICAL_SCHEMA_ONLY | PGM_LOGICAL_DATA_ONLY) &&
		(options->flags & (PGM_LOGICAL_CLEAN |
			PGM_LOGICAL_DATA_ONLY)) !=
		(PGM_LOGICAL_CLEAN | PGM_LOGICAL_DATA_ONLY) &&
		((options->archive_endpoint == NULL &&
			options->archive_generation == 0) ||
		 (options->archive_endpoint != NULL &&
			options->archive_generation != 0 &&
			strcmp(options->archive_path,
				POSTGAMMA_LOGICAL_TOOL_STREAM_PATH) == 0));
}


static pg_noreturn void
logical_tool_fail(int status, int exit_code)
{
	PostgammaLogicalToolContext *context = PostgammaCurrentLogicalTool;

	if (context == NULL || !context->jump_ready || context->unwinding)
		__builtin_trap();
	context->failure_status = status;
	context->exit_code = exit_code;
	siglongjmp(context->exit_jump, 1);
}


static void
logical_tool_state_failure(void *argument, int status)
{
	PostgammaLogicalToolContext *context = argument;

	if (context != PostgammaCurrentLogicalTool)
		__builtin_trap();
	logical_tool_fail(status, EXIT_FAILURE);
}


static const char *
logical_tool_keyword(
	const char *const *keywords, const char *const *values,
	const char *name)
{
	const char *value = NULL;
	size_t		index;

	if (keywords == NULL || values == NULL || name == NULL)
		return NULL;
	for (index = 0; keywords[index] != NULL; index++)
	{
		if (strcmp(keywords[index], name) == 0)
			value = values[index];
	}
	return value;
}


FILE *
postgamma_logical_tool_fopen(const char *path, const char *mode)
{
	PostgammaLogicalToolContext *context = PostgammaCurrentLogicalTool;
	cookie_io_functions_t functions;
	FILE	   *stream;

	if (context == NULL || context->options->archive_endpoint == NULL ||
		path == NULL ||
		strcmp(path, POSTGAMMA_LOGICAL_TOOL_STREAM_PATH) != 0)
		return fopen(path, mode);
	if (mode == NULL || context->archive_opened ||
		(context->kind == POSTGAMMA_LOGICAL_TOOL_DUMP && mode[0] != 'w') ||
		(context->kind == POSTGAMMA_LOGICAL_TOOL_RESTORE && mode[0] != 'r'))
	{
		errno = EINVAL;
		return NULL;
	}
	memset(&functions, 0, sizeof(functions));
	functions.read = context->kind == POSTGAMMA_LOGICAL_TOOL_RESTORE ?
		logical_tool_cookie_read : NULL;
	functions.write = context->kind == POSTGAMMA_LOGICAL_TOOL_DUMP ?
		logical_tool_cookie_write : NULL;
	functions.seek = logical_tool_cookie_seek;
	functions.close = logical_tool_cookie_close;
	stream = fopencookie(context, mode, functions);
	if (stream == NULL)
		return NULL;
	if (setvbuf(stream, NULL, _IONBF, 0) != 0)
	{
		(void) fclose(stream);
		errno = EIO;
		return NULL;
	}
	context->archive_opened = true;
	return stream;
}


FILE *
postgamma_logical_tool_stderr(void)
{
	PostgammaLogicalToolContext *context = PostgammaCurrentLogicalTool;

	return context != NULL && context->diagnostic_stream != NULL ?
		context->diagnostic_stream : stderr;
}


static int
logical_tool_open_diagnostic(PostgammaLogicalToolContext *context)
{
	cookie_io_functions_t functions;

	memset(&functions, 0, sizeof(functions));
	functions.write = logical_tool_diagnostic_write;
	functions.close = logical_tool_diagnostic_close;
	context->diagnostic_stream = fopencookie(context, "w", functions);
	return context->diagnostic_stream != NULL ? 0 : errno;
}


static ssize_t
logical_tool_diagnostic_write(void *cookie, const char *buffer, size_t size)
{
	PostgammaLogicalToolContext *context = cookie;
	size_t available;
	size_t copied;

	if (context == NULL || buffer == NULL || size > (size_t) SSIZE_MAX)
	{
		errno = EINVAL;
		return -1;
	}
	available = sizeof(context->diagnostic_data) -
		context->diagnostic_size - 1;
	copied = size < available ? size : available;
	if (copied > 0)
	{
		memcpy(
			context->diagnostic_data + context->diagnostic_size,
			buffer, copied);
		context->diagnostic_size += copied;
		context->diagnostic_data[context->diagnostic_size] = '\0';
	}
	return (ssize_t) size;
}


static int
logical_tool_diagnostic_close(void *cookie)
{
	return cookie != NULL ? 0 : EOF;
}


static ssize_t
logical_tool_cookie_read(void *cookie, char *buffer, size_t size)
{
	PostgammaLogicalToolContext *context = cookie;

	if (context == NULL || buffer == NULL || size == 0 ||
		context != PostgammaCurrentLogicalTool ||
		context->kind != POSTGAMMA_LOGICAL_TOOL_RESTORE)
	{
		errno = EINVAL;
		return -1;
	}
	for (;;)
	{
		PostgammaMemoryIoResult io = postgamma_memory_endpoint_read(
			context->options->archive_endpoint,
			context->options->archive_generation, buffer, size);

		if (io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
			return (ssize_t) io.bytes;
		if (io.status == POSTGAMMA_MEMORY_STATUS_EOF)
			return 0;
		if (io.status != POSTGAMMA_MEMORY_STATUS_RETRY)
		{
			errno = io.abort_reason == POSTGAMMA_MEMORY_ABORT_CANCELLED ?
				ECANCELED : EIO;
			return -1;
		}
		{
			PostgammaMemoryWaitResult wait = postgamma_memory_endpoint_wait(
				context->options->archive_endpoint,
				context->options->archive_generation,
				POSTGAMMA_MEMORY_WAIT_READABLE |
				POSTGAMMA_MEMORY_WAIT_PEER_CLOSED,
				UINT64_C(0), POSTGAMMA_MEMORY_NO_DEADLINE);

			if (wait.status == POSTGAMMA_MEMORY_STATUS_OK)
				continue;
			if (wait.status == POSTGAMMA_MEMORY_STATUS_EOF)
				return 0;
			errno = wait.abort_reason == POSTGAMMA_MEMORY_ABORT_CANCELLED ?
				ECANCELED : EIO;
			return -1;
		}
	}
}


static ssize_t
logical_tool_cookie_write(void *cookie, const char *buffer, size_t size)
{
	PostgammaLogicalToolContext *context = cookie;
	size_t		written = 0;

	if (context == NULL || buffer == NULL || size == 0 ||
		context != PostgammaCurrentLogicalTool ||
		context->kind != POSTGAMMA_LOGICAL_TOOL_DUMP)
	{
		errno = EINVAL;
		return -1;
	}
	while (written < size)
	{
		PostgammaMemoryIoResult io = postgamma_memory_endpoint_write(
			context->options->archive_endpoint,
			context->options->archive_generation,
			buffer + written, size - written);

		if (io.status == POSTGAMMA_MEMORY_STATUS_PROGRESS)
		{
			written += io.bytes;
			continue;
		}
		if (io.status != POSTGAMMA_MEMORY_STATUS_RETRY)
		{
			errno = io.abort_reason == POSTGAMMA_MEMORY_ABORT_CANCELLED ?
				ECANCELED : EIO;
			return -1;
		}
		{
			PostgammaMemoryWaitResult wait = postgamma_memory_endpoint_wait(
				context->options->archive_endpoint,
				context->options->archive_generation,
				POSTGAMMA_MEMORY_WAIT_WRITABLE |
				POSTGAMMA_MEMORY_WAIT_PEER_CLOSED,
				UINT64_C(0), POSTGAMMA_MEMORY_NO_DEADLINE);

			if (wait.status == POSTGAMMA_MEMORY_STATUS_OK)
				continue;
			errno = wait.abort_reason == POSTGAMMA_MEMORY_ABORT_CANCELLED ?
				ECANCELED : EIO;
			return -1;
		}
	}
	return (ssize_t) written;
}


static int
logical_tool_cookie_seek(void *cookie, off64_t *offset, int whence)
{
	(void) cookie;
	(void) offset;
	(void) whence;
	errno = ESPIPE;
	return -1;
}


static int
logical_tool_cookie_close(void *cookie)
{
	PostgammaLogicalToolContext *context = cookie;

	if (context == NULL || context != PostgammaCurrentLogicalTool)
	{
		errno = EINVAL;
		return -1;
	}
	if (context->kind == POSTGAMMA_LOGICAL_TOOL_DUMP &&
		postgamma_memory_endpoint_half_close_write(
			context->options->archive_endpoint,
			context->options->archive_generation) !=
		POSTGAMMA_MEMORY_STATUS_OK)
	{
		errno = EIO;
		return -1;
	}
	return 0;
}


PGconn *
PQconnectdbParams(
	const char *const *keywords, const char *const *values,
	int expand_dbname)
{
	PostgammaLogicalToolContext *context = PostgammaCurrentLogicalTool;
	pgm_connection_options connection_options = PGM_CONNECTION_OPTIONS_INIT;
	pgm_error  *error = NULL;
	pgm_status	status;
	const char *database;
	const char *user;
	const char *application_name;
	void	   *native_connection = NULL;

	(void) expand_dbname;
	if (context == NULL || context->connection != NULL)
		logical_tool_fail(EPROTO, EXIT_FAILURE);
	database = logical_tool_keyword(keywords, values, "dbname");
	user = logical_tool_keyword(keywords, values, "user");
	application_name = logical_tool_keyword(
		keywords, values, "application_name");
	if (application_name == NULL)
		application_name = logical_tool_keyword(
			keywords, values, "fallback_application_name");
	if (database == NULL || database[0] == '\0')
		database = context->options->database;
	if (user == NULL || user[0] == '\0')
		user = context->options->user;
	connection_options.user = user;
	connection_options.database = database;
	connection_options.application_name = application_name != NULL ?
		application_name : "postgamma-logical-tool";
	status = pgm_connection_open(
		context->options->instance, &connection_options,
		&context->connection, &error);
	if (status != PGM_STATUS_OK)
	{
		logical_tool_set_result(
			context->result, EIO, EXIT_FAILURE,
			context->request_generation,
			error != NULL ? pgm_error_message(error) :
			"could not open the in-process logical-tool connection");
		pgm_error_free(error);
		logical_tool_fail(EIO, EXIT_FAILURE);
	}
	context->private_connection = context->connection->private_connection;
	if (postgamma_private_libpq_tool_borrow(
			context->private_connection, context->request_generation,
			&native_connection) != POSTGAMMA_PRIVATE_LIBPQ_OK ||
		native_connection == NULL)
	{
		(void) pgm_connection_close(
			context->connection, POSTGAMMA_LOGICAL_TOOL_CLOSE_TIMEOUT_MS,
			NULL);
		context->connection = NULL;
		context->private_connection = NULL;
		logical_tool_fail(EPROTO, EXIT_FAILURE);
	}
	context->native_connection = native_connection;
	context->connection_borrowed = true;
	if (context->worker != NULL)
	{
		bool cancel_requested;

		if (pthread_mutex_lock(&context->worker->mutex) != 0)
			logical_tool_fail(EPROTO, EXIT_FAILURE);
		context->worker->active_private_connection =
			context->private_connection;
		context->worker->active_request_generation =
			context->request_generation;
		cancel_requested = context->worker->cancel_requested;
		(void) pthread_mutex_unlock(&context->worker->mutex);
		if (cancel_requested)
			(void) postgamma_private_libpq_cancel(
				context->private_connection, context->request_generation);
	}
	return context->native_connection;
}


void
PQfinish(PGconn *connection)
{
	PostgammaLogicalToolContext *context = PostgammaCurrentLogicalTool;

	if (context == NULL || connection == NULL ||
		connection != context->native_connection ||
		logical_tool_close_connection(context) != 0)
		logical_tool_fail(EPROTO, EXIT_FAILURE);
}


PGcancel *
PQgetCancel(PGconn *connection)
{
	/*
	 * Logical-tool cancellation is generation-routed through the worker
	 * control block.  Never let upstream construct a socket-backed PGcancel
	 * object for an in-memory connection.
	 */
	(void) connection;
	return NULL;
}


void
PQfreeCancel(PGcancel *cancel)
{
	/* PQgetCancel() never allocates a cancel object in this closure. */
	(void) cancel;
}


int
PQcancel(PGcancel *cancel, char *error_buffer, int error_buffer_size)
{
	PostgammaLogicalToolContext *context = PostgammaCurrentLogicalTool;
	PostgammaPrivateLibpqStatus status;

	(void) cancel;
	if (error_buffer != NULL && error_buffer_size > 0)
		error_buffer[0] = '\0';
	if (context == NULL || context->private_connection == NULL ||
		!context->connection_borrowed)
		return 0;
	status = postgamma_private_libpq_cancel(
		context->private_connection, context->request_generation);
	if (status != POSTGAMMA_PRIVATE_LIBPQ_OK)
	{
		if (error_buffer != NULL && error_buffer_size > 0)
			(void) snprintf(
				error_buffer, (size_t) error_buffer_size,
				"in-process cancel failed");
		return 0;
	}
	return 1;
}


pg_noreturn void
exit(int code)
{
	logical_tool_fail(code == 0 ? 0 : EPROTO, code);
}


pg_noreturn void
_exit(int code)
{
	logical_tool_fail(ENOTSUP, code);
}


pg_noreturn void
abort(void)
{
	logical_tool_fail(EPROTO, EXIT_FAILURE);
}


void
pqsignal(int signal_number, pqsigfunc handler)
{
	(void) signal_number;
	(void) handler;
}


char *
setlocale(int category, const char *locale)
{
	static char c_locale[] = "C";

	(void) category;
	(void) locale;
	return c_locale;
}


int
setenv(const char *name, const char *value, int overwrite)
{
	(void) name;
	(void) value;
	(void) overwrite;
	return 0;
}


int
unsetenv(const char *name)
{
	(void) name;
	return 0;
}


mode_t
umask(mode_t mask)
{
	PostgammaLogicalToolContext *context = PostgammaCurrentLogicalTool;
	mode_t		old_mask;

	if (context == NULL)
		logical_tool_fail(EPROTO, EXIT_FAILURE);
	old_mask = context->logical_umask;
	context->logical_umask = mask;
	return old_mask;
}


void
set_pglocale_pgservice(const char *argv0, const char *app)
{
	(void) argv0;
	(void) app;
}


int
system(const char *command)
{
	(void) command;
	errno = ENOTSUP;
	return -1;
}


FILE *
popen(const char *command, const char *mode)
{
	(void) command;
	(void) mode;
	errno = ENOTSUP;
	return NULL;
}


pid_t
fork(void)
{
	errno = ENOTSUP;
	return (pid_t) -1;
}


int
kill(pid_t process_id, int signal_number)
{
	(void) process_id;
	(void) signal_number;
	errno = ENOTSUP;
	return -1;
}


int
raise(int signal_number)
{
	(void) signal_number;
	errno = ENOTSUP;
	return -1;
}


int
chdir(const char *path)
{
	(void) path;
	errno = ENOTSUP;
	return -1;
}


static int
logical_tool_close_connection(PostgammaLogicalToolContext *context)
{
	pgm_connection *connection;
	pgm_status	status;
	int			result = 0;

	if (context == NULL)
		return EINVAL;
	connection = context->connection;
	if (context->worker != NULL)
	{
		if (pthread_mutex_lock(&context->worker->mutex) != 0)
			return EPROTO;
		context->worker->active_private_connection = NULL;
		context->worker->active_request_generation = 0;
		(void) pthread_mutex_unlock(&context->worker->mutex);
	}
	if (context->connection_borrowed)
	{
		if (postgamma_private_libpq_tool_release(
				context->private_connection,
				context->request_generation) != POSTGAMMA_PRIVATE_LIBPQ_OK)
			result = EPROTO;
		context->connection_borrowed = false;
	}
	context->native_connection = NULL;
	context->private_connection = NULL;
	context->connection = NULL;
	if (connection != NULL)
	{
		status = pgm_connection_close(
			connection, POSTGAMMA_LOGICAL_TOOL_CLOSE_TIMEOUT_MS, NULL);
		if (status != PGM_STATUS_OK && result == 0)
			result = EIO;
	}
	return result;
}


static void
logical_tool_cleanup(PostgammaLogicalToolContext *context)
{
	if (context == NULL)
		return;
	context->unwinding = true;
	(void) logical_tool_close_connection(context);
	if (context->tool_state_bound)
	{
		(void) postgamma_tool_state_unbind(context->tool_state);
		context->tool_state_bound = false;
	}
	postgamma_tool_state_destroy(&context->tool_state);
	context->unwinding = false;
}


static int
logical_tool_run(
	PostgammaLogicalToolKind kind,
	const PostgammaLogicalToolOptions *options,
	PostgammaLogicalToolResult *result,
	PostgammaLogicalToolWorker *worker)
{
	PostgammaLogicalToolContext context;
	char	   *arguments[POSTGAMMA_LOGICAL_TOOL_ARGUMENT_CAPACITY];
	char		database_argument[512];
	char		user_argument[512];
	char		file_argument[PATH_MAX + 16];
	char	   *archive_argument;
	int			argument_count = 0;
	int			main_status = EXIT_FAILURE;
	int			status;

	if (result == NULL || result->struct_size != sizeof(*result))
		return EINVAL;
	logical_tool_set_result(result, EINVAL, EXIT_FAILURE, 0,
		"invalid logical-tool arguments");
	if (!logical_tool_options_valid(options) ||
		(kind != POSTGAMMA_LOGICAL_TOOL_DUMP &&
		 kind != POSTGAMMA_LOGICAL_TOOL_RESTORE) ||
		snprintf(database_argument, sizeof(database_argument),
			"--dbname=%s", options->database) >=
			(int) sizeof(database_argument) ||
		snprintf(user_argument, sizeof(user_argument),
			"--username=%s", options->user) >= (int) sizeof(user_argument) ||
		snprintf(file_argument, sizeof(file_argument),
			"--file=%s", options->archive_path) >= (int) sizeof(file_argument))
		return EINVAL;
	logical_tool_set_result(result, 0, EXIT_FAILURE, 0, "");
	memset(&context, 0, sizeof(context));
	context.options = options;
	context.result = result;
	context.kind = kind;
	context.worker = worker;
	context.logical_umask = 0077;
	context.request_generation = atomic_fetch_add_explicit(
		&options->instance->next_request_generation,
		UINT64_C(1), memory_order_relaxed);
	if (context.request_generation == 0 ||
		context.request_generation == UINT64_MAX)
	{
		logical_tool_set_result(
			result, EOVERFLOW, EXIT_FAILURE, 0,
			"logical-tool request generation is exhausted");
		return EOVERFLOW;
	}
	status = pthread_mutex_lock(&PostgammaLogicalToolExecutionMutex);
	if (status != 0)
	{
		logical_tool_set_result(
			result, status, EXIT_FAILURE, context.request_generation,
			"could not serialize frontend-tool execution");
		return status;
	}
	if (worker != NULL)
	{
		bool cancel_requested;

		status = pthread_mutex_lock(&worker->mutex);
		if (status != 0)
			goto done;
		cancel_requested = worker->cancel_requested;
		(void) pthread_mutex_unlock(&worker->mutex);
		if (cancel_requested)
		{
			status = ECANCELED;
			logical_tool_set_result(
				result, status, EXIT_FAILURE, context.request_generation,
				"logical tool was canceled while awaiting dispatch");
			goto done;
		}
	}
	status = postgamma_tool_state_create(&context.tool_state);
	if (status != 0)
		goto done;
	PostgammaCurrentLogicalTool = &context;
	status = logical_tool_open_diagnostic(&context);
	if (status != 0)
	{
		goto done;
	}
	status = postgamma_tool_state_bind(
		context.tool_state, logical_tool_state_failure, &context);
	if (status != 0)
		goto done;
	context.tool_state_bound = true;
	optind = 1;
	opterr = 0;
	if (kind == POSTGAMMA_LOGICAL_TOOL_DUMP)
	{
		arguments[argument_count++] = "postgamma-pg-dump";
		arguments[argument_count++] = "--format=custom";
		arguments[argument_count++] = "--compress=none";
		arguments[argument_count++] = "--no-sync";
		arguments[argument_count++] = "--no-password";
		arguments[argument_count++] = user_argument;
		arguments[argument_count++] = database_argument;
		archive_argument = file_argument;
	}
	else
	{
		arguments[argument_count++] = "postgamma-pg-restore";
		arguments[argument_count++] = "--exit-on-error";
		arguments[argument_count++] = "--format=custom";
		arguments[argument_count++] = "--no-password";
		arguments[argument_count++] = user_argument;
		arguments[argument_count++] = database_argument;
		archive_argument = (char *) options->archive_path;
	}
	if ((options->flags & PGM_LOGICAL_SCHEMA_ONLY) != 0)
		arguments[argument_count++] = "--schema-only";
	if ((options->flags & PGM_LOGICAL_DATA_ONLY) != 0)
		arguments[argument_count++] = "--data-only";
	if ((options->flags & PGM_LOGICAL_CLEAN) != 0)
		arguments[argument_count++] = "--clean";
	if ((options->flags & PGM_LOGICAL_CREATE) != 0)
		arguments[argument_count++] = "--create";
	if ((options->flags & PGM_LOGICAL_NO_OWNER) != 0)
		arguments[argument_count++] = "--no-owner";
	if ((options->flags & PGM_LOGICAL_NO_PRIVILEGES) != 0)
		arguments[argument_count++] = "--no-privileges";
	arguments[argument_count++] = archive_argument;
	arguments[argument_count] = NULL;
	context.jump_ready = true;
	if (sigsetjmp(context.exit_jump, 1) == 0)
	{
		main_status = kind == POSTGAMMA_LOGICAL_TOOL_DUMP ?
			postgamma_pg19_pg_dump_main(argument_count, arguments) :
			postgamma_pg19_pg_restore_main(argument_count, arguments);
		context.exit_code = main_status;
	}
	else
		main_status = context.exit_code;
	context.jump_ready = false;
	status = context.failure_status;
	if (status == 0 && main_status != EXIT_SUCCESS)
		status = EPROTO;

done:
	context.jump_ready = false;
	logical_tool_cleanup(&context);
	logical_tool_capture_result(&context, status);
	PostgammaCurrentLogicalTool = NULL;
	(void) pthread_mutex_unlock(&PostgammaLogicalToolExecutionMutex);
	result->status = status;
	result->postgres_exit_code = main_status;
	result->request_generation = context.request_generation;
	if (result->message[0] == '\0')
		(void) snprintf(
			result->message, sizeof(result->message), "%s",
			status == 0 ? "logical tool completed" :
			"logical tool failed inside the host-safe boundary");
	return status;
}


static void
logical_tool_capture_result(PostgammaLogicalToolContext *context, int status)
{
	size_t size;

	if (context->diagnostic_stream != NULL)
	{
		(void) fflush(context->diagnostic_stream);
		(void) fclose(context->diagnostic_stream);
		context->diagnostic_stream = NULL;
	}
	if (status == 0 || context->diagnostic_size == 0 ||
		context->result->message[0] != '\0')
		return;
	size = context->diagnostic_size;
	while (size > 0 &&
		(context->diagnostic_data[size - 1] == '\n' ||
		 context->diagnostic_data[size - 1] == '\r'))
		context->diagnostic_data[--size] = '\0';
	(void) snprintf(
		context->result->message, sizeof(context->result->message), "%s",
		context->diagnostic_data);
}


static void
logical_tool_set_result(
	PostgammaLogicalToolResult *result, int status,
	int postgres_exit_code, uint64_t request_generation,
	const char *message)
{
	if (result == NULL)
		return;
	result->status = status;
	result->postgres_exit_code = postgres_exit_code;
	result->request_generation = request_generation;
	(void) snprintf(
		result->message, sizeof(result->message), "%s",
		message != NULL ? message : "");
}


int
postgamma_logical_tool_dump_file(
	const PostgammaLogicalToolOptions *options,
	PostgammaLogicalToolResult *result)
{
	return logical_tool_run(
		POSTGAMMA_LOGICAL_TOOL_DUMP, options, result, NULL);
}


int
postgamma_logical_tool_restore_file(
	const PostgammaLogicalToolOptions *options,
	PostgammaLogicalToolResult *result)
{
	return logical_tool_run(
		POSTGAMMA_LOGICAL_TOOL_RESTORE, options, result, NULL);
}


int
postgamma_logical_tool_start(
	PostgammaLogicalToolKind kind,
	const PostgammaLogicalToolOptions *options,
	PostgammaLogicalToolCompletionFunction completion,
	void *completion_argument,
	PostgammaLogicalToolWorker **worker)
{
	PostgammaLogicalToolWorker *created;
	pthread_attr_t attributes;
	bool		attributes_initialized = false;
	int			status;

	if (worker == NULL)
		return EINVAL;
	*worker = NULL;
	if (!logical_tool_options_valid(options) ||
		options->archive_endpoint == NULL || completion == NULL ||
		(kind != POSTGAMMA_LOGICAL_TOOL_DUMP &&
		 kind != POSTGAMMA_LOGICAL_TOOL_RESTORE))
		return EINVAL;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	status = pthread_mutex_init(&created->mutex, NULL);
	if (status != 0)
	{
		free(created);
		return status;
	}
	created->magic = POSTGAMMA_LOGICAL_TOOL_WORKER_MAGIC;
	created->options = *options;
	created->kind = kind;
	created->completion = completion;
	created->completion_argument = completion_argument;
	status = pthread_attr_init(&attributes);
	if (status == 0)
		attributes_initialized = true;
	if (status == 0)
		status = pthread_attr_setdetachstate(
			&attributes, PTHREAD_CREATE_DETACHED);
	if (status == 0)
		status = pthread_attr_setguardsize(
			&attributes, POSTGAMMA_LOGICAL_TOOL_GUARD_SIZE);
	if (status == 0)
		status = pthread_attr_setstacksize(
			&attributes, POSTGAMMA_LOGICAL_TOOL_STACK_SIZE);
	if (status == 0)
		status = pthread_create(
			&created->thread, &attributes,
			logical_tool_worker_main, created);
	if (attributes_initialized)
		(void) pthread_attr_destroy(&attributes);
	if (status != 0)
	{
		created->magic = 0;
		(void) pthread_mutex_destroy(&created->mutex);
		free(created);
		return status;
	}
	*worker = created;
	return 0;
}


int
postgamma_logical_tool_cancel(PostgammaLogicalToolWorker *worker)
{
	PostgammaPrivateLibpqConnection *connection;
	uint64_t	generation;
	PostgammaMemoryStatus memory_status;
	PostgammaPrivateLibpqStatus libpq_status = POSTGAMMA_PRIVATE_LIBPQ_OK;
	int			status;

	if (!logical_tool_worker_valid(worker))
		return EINVAL;
	status = pthread_mutex_lock(&worker->mutex);
	if (status != 0)
		return status;
	worker->cancel_requested = true;
	connection = worker->active_private_connection;
	generation = worker->active_request_generation;
	if (connection != NULL && generation != 0)
		libpq_status = postgamma_private_libpq_cancel(connection, generation);
	(void) pthread_mutex_unlock(&worker->mutex);
	memory_status = postgamma_memory_endpoint_abort(
		worker->options.archive_endpoint,
		worker->options.archive_generation,
		POSTGAMMA_MEMORY_ABORT_CANCELLED);
	if (memory_status != POSTGAMMA_MEMORY_STATUS_OK &&
		memory_status != POSTGAMMA_MEMORY_STATUS_ABORTED)
		return EIO;
	return libpq_status == POSTGAMMA_PRIVATE_LIBPQ_OK ? 0 : EIO;
}


static void *
logical_tool_worker_main(void *argument)
{
	PostgammaLogicalToolWorker *worker = argument;
	PostgammaLogicalToolResult result = POSTGAMMA_LOGICAL_TOOL_RESULT_INIT;
	bool		canceled;

	if (!logical_tool_worker_valid(worker))
		return NULL;
	if (pthread_mutex_lock(&worker->mutex) != 0)
		canceled = true;
	else
	{
		canceled = worker->cancel_requested;
		(void) pthread_mutex_unlock(&worker->mutex);
	}
	if (canceled)
		logical_tool_set_result(
			&result, ECANCELED, EXIT_FAILURE, 0,
			"logical tool was canceled before dispatch");
	else
		(void) logical_tool_run(
			worker->kind, &worker->options, &result, worker);
	worker->completion(worker->completion_argument, worker, &result);
	(void) postgamma_memory_endpoint_release(
		&worker->options.archive_endpoint,
		worker->options.archive_generation);
	worker->magic = 0;
	(void) pthread_mutex_destroy(&worker->mutex);
	free(worker);
	return NULL;
}


static bool
logical_tool_worker_valid(const PostgammaLogicalToolWorker *worker)
{
	return worker != NULL &&
		worker->magic == POSTGAMMA_LOGICAL_TOOL_WORKER_MAGIC &&
		worker->options.archive_endpoint != NULL &&
		worker->options.archive_generation != 0 &&
		worker->completion != NULL;
}
