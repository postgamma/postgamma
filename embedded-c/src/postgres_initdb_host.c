/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/private/initdb_runtime.h"

#include "postgamma/private/postgres_bootstrap_bridge.h"
#include "postgamma/tool_state_runtime.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>


/*
 * This file is the host virtualization boundary for transformed upstream
 * initdb code.  It must not mutate process-global environment, locale, signal,
 * stdio, cwd, or umask state, and it must not create a process.  Only typed
 * bootstrap/check phases selected by the generated adapter may cross into the
 * private PostgreSQL bridges.  Unsupported upstream behavior fails closed.
 *
 * Upstream frontend state is invocation-owned, but libc getopt state is still
 * process-global on supported platforms.  The process mutex therefore covers
 * every call into the transformed initdb main function.  Cluster publication
 * remains independently concurrent and is serialized by a target-specific
 * advisory lock.
 */
#define POSTGAMMA_INITDB_MAX_CLEANUPS 16
#define POSTGAMMA_INITDB_CAPTURE_LIMIT (4U * 1024U * 1024U)
#define POSTGAMMA_INITDB_CAPTURE_HEAD 96U
#define POSTGAMMA_INITDB_DEFAULT_LOCK_TIMEOUT_NS UINT64_C(60000000000)
#define POSTGAMMA_INITDB_LOCK_RETRY_NS UINT64_C(1000000)
#define POSTGAMMA_INITDB_STAGING_SUFFIX_LENGTH 6U


typedef struct PostgammaEmbeddedSetting
{
	const char *name;
	const char *value;
} PostgammaEmbeddedSetting;

#include "postgamma/embedded_guc_policy.inc"


/*
 * The frontend libpq closure uses its public encoding spellings while initdb
 * deliberately uses the private libpgcommon spellings.  Keep both names
 * inside the localized initdb object without exposing either through pgm_*.
 */
#ifdef pg_char_to_encoding
#undef pg_char_to_encoding
#endif
#ifdef pg_encoding_to_char
#undef pg_encoding_to_char
#endif
extern int pg_char_to_encoding_private(const char *name);
extern const char *pg_encoding_to_char_private(int encoding);

int
pg_char_to_encoding(const char *name)
{
	return pg_char_to_encoding_private(name);
}


const char *
pg_encoding_to_char(int encoding)
{
	return pg_encoding_to_char_private(encoding);
}


typedef enum PostgammaInitdbPipeKind
{
	POSTGAMMA_INITDB_PIPE_NONE = 0,
	POSTGAMMA_INITDB_PIPE_BOOTSTRAP,
	POSTGAMMA_INITDB_PIPE_POST_BOOTSTRAP
} PostgammaInitdbPipeKind;

typedef struct PostgammaInitdbCapture
{
	char	   *data;
	size_t		length;
	size_t		capacity;
} PostgammaInitdbCapture;

typedef struct PostgammaInitdbContext
{
	const PostgammaInitdbOptions *options;
	void	   *tool_state;
	FILE	   *output_stream;
	FILE	   *error_stream;
	PostgammaInitdbCapture output_capture;
	PostgammaInitdbCapture error_capture;
	FILE	   *pipe_stream;
	char	   *pipe_data;
	size_t		pipe_length;
	PostgammaInitdbPipeKind pipe_kind;
	void	  (*cleanups[POSTGAMMA_INITDB_MAX_CLEANUPS]) (void);
	size_t		cleanup_count;
	mode_t		logical_umask;
	PostgammaInitdbPhase selected_phase;
	uint64_t	bootstrap_wal_segment_size_bytes;
	bool		bootstrap_data_checksums;
	sigjmp_buf	exit_jump;
	int			exit_code;
	int			failure_status;
	bool		jump_ready;
	bool		unwinding;
	bool		tool_state_bound;
} PostgammaInitdbContext;


static _Thread_local PostgammaInitdbContext *PostgammaCurrentInitdb;
static pthread_mutex_t PostgammaInitdbExecutionMutex = PTHREAD_MUTEX_INITIALIZER;


extern int postgamma_pg19_initdb_main(int argc, char **argv);


static bool initdb_options_valid(const PostgammaInitdbOptions *options);
static int check_fault(
	const PostgammaInitdbOptions *options, PostgammaInitdbFaultPoint point);
static PostgammaInitdbContext *current_initdb(void);
static pg_noreturn void initdb_fail(
	PostgammaInitdbContext *context, int status, int code);
static void tool_state_failure(void *argument, int status);
static void run_registered_cleanups(PostgammaInitdbContext *context);
static int build_initdb_arguments(
	const PostgammaInitdbOptions *options, int *argument_count,
	char ***arguments);
static int append_argument(
	char **arguments, size_t capacity, size_t *count, const char *value);
static int append_setting_argument(
	char **arguments, size_t capacity, size_t *count,
	const char *name, const char *value);
static void free_initdb_arguments(int argument_count, char **arguments);
static int execute_post_bootstrap_sql(
	PostgammaInitdbContext *context, const char *sql);
static void set_capture_result(
	PostgammaInitdbResult *result, int status, int postgres_exit_code,
	const PostgammaInitdbCapture *capture);
static ssize_t capture_write(
	void *argument, const char *buffer, size_t length);
static FILE *open_capture_stream(PostgammaInitdbCapture *capture);
static int run_initdb(
	const PostgammaInitdbOptions *options, PostgammaInitdbResult *result);
static int canonicalize_target(
	const char *target, char **parent, char **canonical_target,
	char **temporary_template);
static int monotonic_now_ns(uint64_t *now_ns);
static int resolve_deadline(uint64_t requested, uint64_t *deadline);
static int deadline_status(uint64_t deadline);
static int acquire_execution_lock(uint64_t deadline);
static int create_lock_name(const char *target, char **name);
static int acquire_creation_lock(
	int parent_descriptor, const char *target, uint64_t deadline,
	int *lock_descriptor);
static int inspect_target(
	const char *target, bool *exists, bool *empty, bool *valid_cluster);
static int staging_owner_path(const char *temporary, char **owner_path);
static int create_staging_owner(
	const char *temporary, const char *target, char **owner_path);
static int staging_owner_matches(
	const char *temporary, const char *target, bool *matches);
static int remove_stale_temporary_directories(
	const char *parent, const char *target);
static void set_result(
	PostgammaInitdbResult *result, int status, int postgres_exit_code,
	bool created, const char *phase, const char *message);


static bool
initdb_options_valid(const PostgammaInitdbOptions *options)
{
	size_t		index;

	if (options == NULL || options->struct_size != sizeof(*options) ||
		options->generation == 0 || options->data_directory == NULL ||
		options->data_directory[0] != '/' || options->executable_path == NULL ||
		options->executable_path[0] != '/' || options->resource_root == NULL ||
		options->resource_root[0] != '/' ||
		(options->setting_count != 0 && options->settings == NULL) ||
		options->logical_umask != 0077 ||
		(options->faults != NULL &&
		 (options->faults->struct_size != sizeof(*options->faults) ||
		  options->faults->check == NULL)))
		return false;
	for (index = 0; index < options->setting_count; index++)
	{
		if (options->settings[index].name == NULL ||
			options->settings[index].name[0] == '\0' ||
			options->settings[index].value == NULL)
			return false;
	}
	return true;
}


static int
check_fault(
	const PostgammaInitdbOptions *options, PostgammaInitdbFaultPoint point)
{
	int			status;

	if (options->faults == NULL)
		return 0;
	status = options->faults->check(options->faults->context, point);
	return status >= 0 ? status : EINVAL;
}


static PostgammaInitdbContext *
current_initdb(void)
{
	return PostgammaCurrentInitdb;
}


static void
run_registered_cleanups(PostgammaInitdbContext *context)
{
	context->unwinding = true;
	while (context->cleanup_count != 0)
	{
		void	  (*cleanup) (void) =
			context->cleanups[--context->cleanup_count];

		cleanup();
	}
	context->unwinding = false;
}


static pg_noreturn void
initdb_fail(PostgammaInitdbContext *context, int status, int code)
{
	if (context == NULL || !context->jump_ready)
		abort();
	context->failure_status = status != 0 ? status : EPROTO;
	context->exit_code = code;
	siglongjmp(context->exit_jump, 1);
}


static void
tool_state_failure(void *argument, int status)
{
	initdb_fail(argument, status, EXIT_FAILURE);
}


pg_noreturn void
postgamma_initdb_exit(int code)
{
	PostgammaInitdbContext *context = current_initdb();
	int			status = context != NULL && context->failure_status != 0 ?
		context->failure_status : (code == 0 ? EPROTO : EIO);

	initdb_fail(context, status, code);
}


int
postgamma_initdb_atexit(void (*function) (void))
{
	PostgammaInitdbContext *context = current_initdb();

	if (context == NULL || function == NULL ||
		context->unwinding ||
		context->cleanup_count == POSTGAMMA_INITDB_MAX_CLEANUPS)
	{
		errno = EINVAL;
		return -1;
	}
	context->cleanups[context->cleanup_count++] = function;
	return 0;
}


int
postgamma_initdb_fflush(FILE *stream)
{
	PostgammaInitdbContext *context = current_initdb();
	int			status = 0;

	if (context == NULL)
	{
		errno = EINVAL;
		return EOF;
	}
	if (stream != NULL)
		return fflush(stream);
	if (context->output_stream != NULL &&
		fflush(context->output_stream) != 0)
		status = EOF;
	if (context->error_stream != NULL &&
		fflush(context->error_stream) != 0)
		status = EOF;
	if (context->pipe_stream != NULL &&
		fflush(context->pipe_stream) != 0)
		status = EOF;
	return status;
}


int
postgamma_initdb_phase_select(
	PostgammaInitdbPhase phase, uint64_t wal_segment_size_bytes,
	bool data_checksums)
{
	PostgammaInitdbContext *context = current_initdb();

	if (context == NULL || context->selected_phase != POSTGAMMA_INITDB_PHASE_NONE ||
		phase <= POSTGAMMA_INITDB_PHASE_NONE ||
		phase >= POSTGAMMA_INITDB_PHASE_COUNT ||
		(phase == POSTGAMMA_INITDB_PHASE_BOOTSTRAP &&
		 (wal_segment_size_bytes == 0 || wal_segment_size_bytes > INT_MAX)) ||
		(phase != POSTGAMMA_INITDB_PHASE_BOOTSTRAP &&
		 (wal_segment_size_bytes != 0 || data_checksums)))
	{
		errno = EINVAL;
		return -1;
	}
	context->selected_phase = phase;
	context->bootstrap_wal_segment_size_bytes = wal_segment_size_bytes;
	context->bootstrap_data_checksums = data_checksums;
	return 0;
}


FILE *
postgamma_initdb_popen(const char *command, const char *mode)
{
	PostgammaInitdbContext *context = current_initdb();

	if (context == NULL || command == NULL || mode == NULL ||
		strcmp(mode, "w") != 0 || context->pipe_stream != NULL ||
		(context->selected_phase != POSTGAMMA_INITDB_PHASE_BOOTSTRAP &&
		 context->selected_phase != POSTGAMMA_INITDB_PHASE_POST_BOOTSTRAP))
	{
		errno = EINVAL;
		return NULL;
	}
	if (context->selected_phase == POSTGAMMA_INITDB_PHASE_BOOTSTRAP)
		context->pipe_kind = POSTGAMMA_INITDB_PIPE_BOOTSTRAP;
	else
		context->pipe_kind = POSTGAMMA_INITDB_PIPE_POST_BOOTSTRAP;
	context->selected_phase = POSTGAMMA_INITDB_PHASE_NONE;
	context->pipe_data = NULL;
	context->pipe_length = 0;
	context->pipe_stream =
		open_memstream(&context->pipe_data, &context->pipe_length);
	return context->pipe_stream;
}


int
postgamma_initdb_pclose_check(FILE *stream)
{
	PostgammaInitdbContext *context = current_initdb();
	PostgammaInitdbPipeKind kind;
	char	   *input;
	size_t		input_length;
	int			status;

	if (context == NULL || stream == NULL || stream != context->pipe_stream)
	{
		errno = EINVAL;
		return 1;
	}
	kind = context->pipe_kind;
	context->pipe_stream = NULL;
	context->pipe_kind = POSTGAMMA_INITDB_PIPE_NONE;
	status = fclose(stream) == 0 ? 0 : (errno != 0 ? errno : EIO);
	input = context->pipe_data;
	input_length = context->pipe_length;
	context->pipe_data = NULL;
	context->pipe_length = 0;
	if (status == 0 && (input == NULL || input_length == 0))
		status = EPROTO;
	if (status == 0 && kind == POSTGAMMA_INITDB_PIPE_BOOTSTRAP)
	{
		int			postgres_exit_code = 0;

			status = postgamma_postgres_bootstrap_memory(
				context->options->generation,
				context->options->data_directory,
				context->options->resource_root,
				context->bootstrap_wal_segment_size_bytes,
				context->bootstrap_data_checksums,
				context->logical_umask,
				input, input_length, &postgres_exit_code);
		if (postgres_exit_code != 0)
			context->exit_code = postgres_exit_code;
		if (status == 0)
			status = check_fault(
				context->options,
				POSTGAMMA_INITDB_FAULT_BOOTSTRAP_COMPLETE);
	}
	else if (status == 0 && kind == POSTGAMMA_INITDB_PIPE_POST_BOOTSTRAP)
	{
		status = execute_post_bootstrap_sql(context, input);
		if (status == 0)
			status = check_fault(
				context->options,
				POSTGAMMA_INITDB_FAULT_POST_BOOTSTRAP_COMPLETE);
	}
	free(input);
	if (status != 0)
	{
		context->failure_status = status;
		errno = status;
		return 1;
	}
	return 0;
}


int
postgamma_initdb_system(const char *command)
{
	PostgammaInitdbContext *context = current_initdb();

	if (context == NULL || command == NULL ||
		context->selected_phase != POSTGAMMA_INITDB_PHASE_CHECK)
	{
		errno = ENOTSUP;
		return -1;
	}
	context->selected_phase = POSTGAMMA_INITDB_PHASE_NONE;
	return 0;
}


pqsigfunc
postgamma_initdb_pqsignal(int signal_number, pqsigfunc handler)
{
	if (current_initdb() == NULL || signal_number <= 0)
	{
		errno = EINVAL;
		return handler;
	}
	return handler;
}


mode_t
postgamma_initdb_umask(mode_t mask)
{
	PostgammaInitdbContext *context = current_initdb();
	mode_t		previous;

	if (context == NULL)
		return 0;
	previous = context->logical_umask;
	if ((mask & 0777) != context->logical_umask)
		initdb_fail(context, ENOTSUP, EXIT_FAILURE);
	return previous;
}


char *
postgamma_initdb_setlocale(int category, const char *locale)
{
	static _Thread_local char locale_name[] = "C";

	(void) category;
	if (current_initdb() == NULL)
		return NULL;
	if (locale == NULL || locale[0] == '\0' || strcmp(locale, "C") == 0 ||
		strcmp(locale, "POSIX") == 0)
		return locale_name;
	errno = EINVAL;
	return NULL;
}


int
postgamma_initdb_setenv(const char *name, const char *value, int overwrite)
{
	(void) overwrite;
	if (current_initdb() == NULL || name == NULL || value == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	return 0;
}


int
postgamma_initdb_unsetenv(const char *name)
{
	if (current_initdb() == NULL || name == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	return 0;
}


uid_t
postgamma_initdb_geteuid(void)
{
	uid_t		owner = geteuid();

	/*
	 * The library executes inside an already-privileged host rather than as a
	 * setuid server executable.  The embedded postmaster enters PostmasterMain
	 * directly and follows the same policy.  Native modules remain restricted
	 * to the product-owned static registry.
	 */
	return owner == 0 ? 1 : owner;
}


const char *
get_user_name_or_exit(const char *program_name)
{
	PostgammaInitdbContext *context = current_initdb();

	(void) program_name;
	if (context == NULL)
		return "postgamma";
	return context->options->username != NULL ?
		context->options->username : "postgamma";
}


int
postgamma_initdb_find_other_exec(
	const char *argv0, const char *target, const char *versionstr,
	char *retpath)
{
	PostgammaInitdbContext *context = current_initdb();

	(void) argv0;
	(void) target;
	(void) versionstr;
	if (context == NULL || retpath == NULL ||
		strlen(context->options->executable_path) >= MAXPGPATH)
		return -1;
	strlcpy(retpath, context->options->executable_path, MAXPGPATH);
	return 0;
}


void
postgamma_initdb_set_pglocale_pgservice(
	const char *argv0, const char *app)
{
	(void) argv0;
	(void) app;
}


FILE *
postgamma_initdb_stdout(void)
{
	PostgammaInitdbContext *context = current_initdb();

	return context != NULL && context->output_stream != NULL ?
		context->output_stream : stdout;
}


FILE *
postgamma_initdb_stderr(void)
{
	PostgammaInitdbContext *context = current_initdb();

	return context != NULL && context->error_stream != NULL ?
		context->error_stream : stderr;
}


int
postgamma_initdb_printf(const char *format, ...)
{
	PostgammaInitdbContext *context = current_initdb();
	va_list		arguments;
	int			result;

	if (context == NULL || context->output_stream == NULL || format == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	va_start(arguments, format);
	result = vfprintf(context->output_stream, format, arguments);
	va_end(arguments);
	return result;
}


int
postgamma_initdb_puts(const char *value)
{
	PostgammaInitdbContext *context = current_initdb();

	if (context == NULL || context->output_stream == NULL || value == NULL)
	{
		errno = EINVAL;
		return EOF;
	}
	if (fputs(value, context->output_stream) == EOF ||
		fputc('\n', context->output_stream) == EOF)
		return EOF;
	return 0;
}


static int
append_argument(
	char **arguments, size_t capacity, size_t *count, const char *value)
{
	if (*count >= capacity || value == NULL)
		return EOVERFLOW;
	arguments[*count] = strdup(value);
	if (arguments[*count] == NULL)
		return ENOMEM;
	(*count)++;
	return 0;
}


static int
append_setting_argument(
	char **arguments, size_t capacity, size_t *count,
	const char *name, const char *value)
{
	char	   *setting;
	size_t		length;
	int			status;

	if (name == NULL || value == NULL ||
		strlen(name) > SIZE_MAX - strlen(value) - 2)
		return EINVAL;
	length = strlen(name) + strlen(value) + 2;
	setting = malloc(length);
	if (setting == NULL)
		return ENOMEM;
	(void) snprintf(setting, length, "%s=%s", name, value);
	status = append_argument(arguments, capacity, count, "-c");
	if (status == 0)
	{
		if (*count >= capacity)
			status = EOVERFLOW;
		else
			arguments[(*count)++] = setting;
	}
	if (status != 0)
		free(setting);
	return status;
}


static int
build_initdb_arguments(
	const PostgammaInitdbOptions *options, int *argument_count,
	char ***arguments)
{
	const size_t default_count = lengthof(PostgammaEmbeddedDefaults);
	size_t		capacity;
	size_t		count = 0;
	size_t		index;
	char	  **created;
	char	   *share_directory;
	size_t		share_length;
	int			status = 0;

	if (options->setting_count > (SIZE_MAX - 32) / 2 ||
		default_count > (SIZE_MAX - 32) / 2 - options->setting_count)
		return EOVERFLOW;
	capacity = 32 + 2 *
		(options->setting_count + default_count);
	created = calloc(capacity + 1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	share_length = strlen(options->resource_root) + sizeof("/share");
	share_directory = malloc(share_length);
	if (share_directory == NULL)
	{
		free(created);
		return ENOMEM;
	}
	(void) snprintf(
		share_directory, share_length, "%s/share", options->resource_root);

#define APPEND(value) \
	do { if (status == 0) status = append_argument( \
		created, capacity, &count, (value)); } while (0)
	APPEND("postgamma-initdb");
	APPEND("-D");
	APPEND(options->data_directory);
	APPEND("-L");
	APPEND(share_directory);
	APPEND("--username");
	APPEND(options->username != NULL ? options->username : "postgamma");
	APPEND("--auth=trust");
	APPEND("--encoding=UTF8");
	APPEND("--locale=C");
	APPEND("--locale-provider=builtin");
	APPEND("--builtin-locale=C");
	APPEND("--no-instructions");
#undef APPEND
	for (index = 0; status == 0 && index < default_count; index++)
		status = append_setting_argument(
			created, capacity, &count, PostgammaEmbeddedDefaults[index].name,
			PostgammaEmbeddedDefaults[index].value);
	for (index = 0; status == 0 && index < options->setting_count; index++)
		status = append_setting_argument(
			created, capacity, &count, options->settings[index].name,
			options->settings[index].value);
	free(share_directory);
	if (status != 0 || count > INT_MAX)
	{
		free_initdb_arguments((int) count, created);
		return status != 0 ? status : EOVERFLOW;
	}
	*argument_count = (int) count;
	*arguments = created;
	return 0;
}


static void
free_initdb_arguments(int argument_count, char **arguments)
{
	int			index;

	if (arguments == NULL)
		return;
	for (index = 0; index < argument_count; index++)
		free(arguments[index]);
	free(arguments);
}


static int
execute_post_bootstrap_sql(PostgammaInitdbContext *context, const char *sql)
{
	int postgres_exit_code = 0;
	int status = postgamma_postgres_single_user_memory(
		context->options->generation,
		context->options->data_directory,
		context->options->executable_path,
		context->options->resource_root,
		context->options->username != NULL ?
			context->options->username : "postgamma",
		context->logical_umask,
		sql, strlen(sql), &postgres_exit_code);

	if (postgres_exit_code != 0)
		context->exit_code = postgres_exit_code;
	return status;
}


static void
set_result(
	PostgammaInitdbResult *result, int status, int postgres_exit_code,
	bool created, const char *phase, const char *message)
{
	result->status = status;
	result->postgres_exit_code = postgres_exit_code;
	result->created = created;
	(void) snprintf(
		result->phase, sizeof(result->phase), "%s",
		phase != NULL ? phase : "unknown");
	(void) snprintf(
		result->message, sizeof(result->message), "%s",
		message != NULL ? message : "");
}


static void
set_capture_result(
	PostgammaInitdbResult *result, int status, int postgres_exit_code,
	const PostgammaInitdbCapture *capture)
{
	static const char separator[] = "\n...\n";
	char		message[sizeof(result->message)];
	size_t		head_length;
	size_t		tail_length;
	size_t		available;

	if (capture == NULL || capture->data == NULL || capture->length == 0)
	{
		set_result(result, status, postgres_exit_code, false, "initdb",
			"in-process initdb failed");
		return;
	}
	if (capture->length < sizeof(message))
	{
		set_result(result, status, postgres_exit_code, false, "initdb",
			capture->data);
		return;
	}
	available = sizeof(message) - 1 - (sizeof(separator) - 1);
	head_length = POSTGAMMA_INITDB_CAPTURE_HEAD;
	if (head_length > available)
		head_length = available / 2;
	tail_length = available - head_length;
	memcpy(message, capture->data, head_length);
	memcpy(message + head_length, separator, sizeof(separator) - 1);
	memcpy(
		message + head_length + sizeof(separator) - 1,
		capture->data + capture->length - tail_length,
		tail_length);
	message[sizeof(message) - 1] = '\0';
	set_result(result, status, postgres_exit_code, false, "initdb", message);
}


static ssize_t
capture_write(void *argument, const char *buffer, size_t length)
{
	PostgammaInitdbCapture *capture = argument;
	size_t		required;
	size_t		capacity;
	char	   *expanded;

	if (capture == NULL || (buffer == NULL && length != 0) ||
		capture->length >= POSTGAMMA_INITDB_CAPTURE_LIMIT ||
		length > POSTGAMMA_INITDB_CAPTURE_LIMIT - capture->length - 1)
	{
		errno = EOVERFLOW;
		return -1;
	}
	required = capture->length + length + 1;
	if (required > capture->capacity)
	{
		capacity = capture->capacity == 0 ? 4096 : capture->capacity;
		while (capacity < required)
		{
			if (capacity > POSTGAMMA_INITDB_CAPTURE_LIMIT / 2)
			{
				capacity = POSTGAMMA_INITDB_CAPTURE_LIMIT;
				break;
			}
			capacity *= 2;
		}
		expanded = realloc(capture->data, capacity);
		if (expanded == NULL)
		{
			errno = ENOMEM;
			return -1;
		}
		capture->data = expanded;
		capture->capacity = capacity;
	}
	if (length != 0)
		memcpy(capture->data + capture->length, buffer, length);
	capture->length += length;
	capture->data[capture->length] = '\0';
	return (ssize_t) length;
}


static FILE *
open_capture_stream(PostgammaInitdbCapture *capture)
{
	cookie_io_functions_t functions = {
		.read = NULL,
		.write = capture_write,
		.seek = NULL,
		.close = NULL,
	};

	return fopencookie(capture, "w", functions);
}


static int
canonicalize_target(
	const char *target, char **parent, char **canonical_target,
	char **temporary_template)
{
	char	   *copy = NULL;
	char	   *slash;
	char	   *canonical_parent = NULL;
	const char *base;
	size_t		parent_length;
	size_t		target_length;
	size_t		template_length;
	int			status = 0;

	if (parent == NULL || canonical_target == NULL ||
		temporary_template == NULL)
		return EINVAL;
	*parent = NULL;
	*canonical_target = NULL;
	*temporary_template = NULL;
	if (target == NULL || target[0] != '/')
		return EINVAL;
	copy = strdup(target);
	if (copy == NULL)
		return ENOMEM;
	while (strlen(copy) > 1 && copy[strlen(copy) - 1] == '/')
		copy[strlen(copy) - 1] = '\0';
	slash = strrchr(copy, '/');
	if (slash == NULL || slash[1] == '\0' ||
		strcmp(slash + 1, ".") == 0 || strcmp(slash + 1, "..") == 0)
	{
		status = EINVAL;
		goto finish;
	}
	base = slash + 1;
	if (slash == copy)
		canonical_parent = realpath("/", NULL);
	else
	{
		*slash = '\0';
		canonical_parent = realpath(copy, NULL);
		*slash = '/';
	}
	if (canonical_parent == NULL)
	{
		status = errno != 0 ? errno : ENOENT;
		goto finish;
	}
	parent_length = strlen(canonical_parent);
	target_length = parent_length + strlen(base) + 2;
	template_length = parent_length + strlen(base) +
		sizeof("/.postgamma-create-.XXXXXX");
	if (target_length < parent_length || template_length < parent_length)
	{
		status = EOVERFLOW;
		goto finish;
	}
	*canonical_target = malloc(target_length);
	*temporary_template = malloc(template_length);
	if (*canonical_target == NULL || *temporary_template == NULL)
	{
		status = ENOMEM;
		goto finish;
	}
	(void) snprintf(
		*canonical_target, target_length, "%s/%s", canonical_parent, base);
	(void) snprintf(
		*temporary_template, template_length,
		"%s/.%s.postgamma-create.XXXXXX", canonical_parent, base);
	*parent = canonical_parent;
	canonical_parent = NULL;

finish:
	if (status != 0)
	{
		free(*temporary_template);
		free(*canonical_target);
		*temporary_template = NULL;
		*canonical_target = NULL;
	}
	free(canonical_parent);
	free(copy);
	return status;
}


static int
monotonic_now_ns(uint64_t *now_ns)
{
	struct timespec now;

	if (now_ns == NULL)
		return EINVAL;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return errno != 0 ? errno : EIO;
	if (now.tv_sec < 0 ||
		(uint64_t) now.tv_sec > UINT64_MAX / UINT64_C(1000000000))
		return EOVERFLOW;
	*now_ns = (uint64_t) now.tv_sec * UINT64_C(1000000000) +
		(uint64_t) now.tv_nsec;
	return 0;
}


static int
resolve_deadline(uint64_t requested, uint64_t *deadline)
{
	uint64_t	now;
	int			status;

	if (deadline == NULL)
		return EINVAL;
	if (requested != 0)
	{
		*deadline = requested;
		return 0;
	}
	status = monotonic_now_ns(&now);
	if (status != 0)
		return status;
	if (now > UINT64_MAX - POSTGAMMA_INITDB_DEFAULT_LOCK_TIMEOUT_NS)
		return EOVERFLOW;
	*deadline = now + POSTGAMMA_INITDB_DEFAULT_LOCK_TIMEOUT_NS;
	return 0;
}


static int
deadline_status(uint64_t deadline)
{
	uint64_t	now;
	int			status = monotonic_now_ns(&now);

	if (status != 0)
		return status;
	return now >= deadline ? ETIMEDOUT : 0;
}


static int
acquire_execution_lock(uint64_t deadline)
{
	for (;;)
	{
		struct timespec interval;
		uint64_t	now;
		uint64_t	remaining;
		int			status = pthread_mutex_trylock(
			&PostgammaInitdbExecutionMutex);

		if (status == 0)
			return 0;
		if (status != EBUSY)
			return status;
		status = monotonic_now_ns(&now);
		if (status != 0)
			return status;
		if (now >= deadline)
			return ETIMEDOUT;
		remaining = deadline - now;
		if (remaining > POSTGAMMA_INITDB_LOCK_RETRY_NS)
			remaining = POSTGAMMA_INITDB_LOCK_RETRY_NS;
		interval.tv_sec = (time_t) (remaining / UINT64_C(1000000000));
		interval.tv_nsec = (long) (remaining % UINT64_C(1000000000));
		while (nanosleep(&interval, &interval) != 0)
		{
			if (errno != EINTR)
				return errno != 0 ? errno : EIO;
		}
	}
}


static int
create_lock_name(const char *target, char **name)
{
	const char *base;
	size_t		length;

	if (target == NULL || name == NULL)
		return EINVAL;
	*name = NULL;
	base = strrchr(target, '/');
	if (base == NULL || base[1] == '\0')
		return EINVAL;
	base++;
	if (strlen(base) > SIZE_MAX - sizeof("..postgamma-create.lock"))
		return EOVERFLOW;
	length = strlen(base) + sizeof("..postgamma-create.lock");
	*name = malloc(length);
	if (*name == NULL)
		return ENOMEM;
	(void) snprintf(*name, length, ".%s.postgamma-create.lock", base);
	return 0;
}


static int
acquire_creation_lock(
	int parent_descriptor, const char *target, uint64_t deadline,
	int *lock_descriptor)
{
	char	   *name = NULL;
	struct stat lock_status;
	int			descriptor = -1;
	int			status;

	if (parent_descriptor < 0 || lock_descriptor == NULL)
		return EINVAL;
	*lock_descriptor = -1;
	status = create_lock_name(target, &name);
	if (status != 0)
		return status;
	descriptor = openat(
		parent_descriptor, name,
		O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
		S_IRUSR | S_IWUSR);
	if (descriptor < 0)
	{
		status = errno != 0 ? errno : EIO;
		goto finish;
	}
	if (fstat(descriptor, &lock_status) != 0)
	{
		status = errno != 0 ? errno : EIO;
		goto finish;
	}
	if (!S_ISREG(lock_status.st_mode) || lock_status.st_nlink != 1)
	{
		status = EINVAL;
		goto finish;
	}
	for (;;)
	{
		struct timespec interval;
		uint64_t	now;
		uint64_t	remaining;

		if (flock(descriptor, LOCK_EX | LOCK_NB) == 0)
		{
			*lock_descriptor = descriptor;
			descriptor = -1;
			status = 0;
			break;
		}
		if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
		{
			status = errno != 0 ? errno : EIO;
			break;
		}
		status = monotonic_now_ns(&now);
		if (status != 0)
			break;
		if (now >= deadline)
		{
			status = ETIMEDOUT;
			break;
		}
		remaining = deadline - now;
		if (remaining > POSTGAMMA_INITDB_LOCK_RETRY_NS)
			remaining = POSTGAMMA_INITDB_LOCK_RETRY_NS;
		interval.tv_sec = (time_t) (remaining / UINT64_C(1000000000));
		interval.tv_nsec = (long) (remaining % UINT64_C(1000000000));
		while (nanosleep(&interval, &interval) != 0)
		{
			if (errno != EINTR)
			{
				status = errno != 0 ? errno : EIO;
				goto finish;
			}
		}
	}

finish:
	if (descriptor >= 0)
		(void) close(descriptor);
	free(name);
	return status;
}


static int
inspect_target(
	const char *target, bool *exists, bool *empty, bool *valid_cluster)
{
	struct stat target_status;
	struct stat control_status;
	DIR		   *directory = NULL;
	struct dirent *entry;
	char	   *version_path = NULL;
	char	   *control_path = NULL;
	char		version[32];
	size_t		target_length;
	ssize_t		count;
	int			descriptor = -1;
	int			status = 0;

	if (target == NULL || exists == NULL || empty == NULL ||
		valid_cluster == NULL)
		return EINVAL;
	*exists = false;
	*empty = false;
	*valid_cluster = false;
	if (lstat(target, &target_status) != 0)
		return errno == ENOENT ? 0 : (errno != 0 ? errno : EIO);
	*exists = true;
	if (!S_ISDIR(target_status.st_mode))
		return ENOTDIR;
	directory = opendir(target);
	if (directory == NULL)
		return errno != 0 ? errno : EIO;
	*empty = true;
	errno = 0;
	while ((entry = readdir(directory)) != NULL)
	{
		if (strcmp(entry->d_name, ".") != 0 &&
			strcmp(entry->d_name, "..") != 0)
		{
			*empty = false;
			break;
		}
	}
	if (entry == NULL && errno != 0)
		status = errno;
	if (closedir(directory) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	if (status != 0 || *empty)
		return status;
	target_length = strlen(target);
	if (target_length > SIZE_MAX - sizeof("/global/pg_control"))
		return EOVERFLOW;
	version_path = malloc(target_length + sizeof("/PG_VERSION"));
	control_path = malloc(target_length + sizeof("/global/pg_control"));
	if (version_path == NULL || control_path == NULL)
	{
		status = ENOMEM;
		goto finish;
	}
	(void) snprintf(
		version_path, target_length + sizeof("/PG_VERSION"),
		"%s/PG_VERSION", target);
	(void) snprintf(
		control_path, target_length + sizeof("/global/pg_control"),
		"%s/global/pg_control", target);
	descriptor = open(version_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (descriptor < 0)
	{
		status = errno == ENOENT ? 0 : (errno != 0 ? errno : EIO);
		goto finish;
	}
	count = read(descriptor, version, sizeof(version) - 1);
	if (count < 0)
	{
		status = errno != 0 ? errno : EIO;
		goto finish;
	}
	version[count] = '\0';
	while (count > 0 && (version[count - 1] == '\n' ||
		version[count - 1] == '\r'))
		version[--count] = '\0';
	if (strcmp(version, PG_MAJORVERSION) != 0)
	{
		status = 0;
		goto finish;
	}
	if (lstat(control_path, &control_status) != 0)
	{
		status = errno == ENOENT ? 0 : (errno != 0 ? errno : EIO);
		goto finish;
	}
	if (!S_ISREG(control_status.st_mode))
	{
		status = 0;
		goto finish;
	}
	*valid_cluster = true;

finish:
	if (descriptor >= 0 && close(descriptor) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	free(control_path);
	free(version_path);
	return status;
}


static int
staging_owner_path(const char *temporary, char **owner_path)
{
	size_t		length;

	if (temporary == NULL || owner_path == NULL)
		return EINVAL;
	*owner_path = NULL;
	if (strlen(temporary) > SIZE_MAX - sizeof(".owner"))
		return EOVERFLOW;
	length = strlen(temporary) + sizeof(".owner");
	*owner_path = malloc(length);
	if (*owner_path == NULL)
		return ENOMEM;
	(void) snprintf(*owner_path, length, "%s.owner", temporary);
	return 0;
}


static int
create_staging_owner(
	const char *temporary, const char *target, char **owner_path)
{
	char	   *content = NULL;
	size_t		content_length;
	size_t		written = 0;
	int			descriptor = -1;
	int			status;

	if (target == NULL || owner_path == NULL ||
		strlen(target) > SIZE_MAX - 2)
		return EINVAL;
	status = staging_owner_path(temporary, owner_path);
	if (status != 0)
		return status;
	content_length = strlen(target) + 1;
	content = malloc(content_length);
	if (content == NULL)
	{
		status = ENOMEM;
		goto finish;
	}
	memcpy(content, target, content_length - 1);
	content[content_length - 1] = '\n';
	descriptor = open(
		*owner_path,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		S_IRUSR | S_IWUSR);
	if (descriptor < 0)
	{
		status = errno != 0 ? errno : EIO;
		goto finish;
	}
	status = 0;
	while (written < content_length)
	{
		ssize_t		count = write(
			descriptor, content + written, content_length - written);

		if (count > 0)
		{
			written += (size_t) count;
			continue;
		}
		if (count < 0 && errno == EINTR)
			continue;
		status = errno != 0 ? errno : EIO;
		break;
	}
	if (status == 0 && fsync(descriptor) != 0)
		status = errno != 0 ? errno : EIO;

finish:
	if (descriptor >= 0 && close(descriptor) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	if (status != 0)
	{
		if (*owner_path != NULL)
			(void) unlink(*owner_path);
		free(*owner_path);
		*owner_path = NULL;
	}
	free(content);
	return status;
}


static int
staging_owner_matches(
	const char *temporary, const char *target, bool *matches)
{
	char	   *owner_path = NULL;
	char	   *content = NULL;
	struct stat owner_status;
	size_t		target_length;
	ssize_t		count;
	int			descriptor = -1;
	int			status;

	if (target == NULL || matches == NULL ||
		strlen(target) > SIZE_MAX - 2)
		return EINVAL;
	*matches = false;
	status = staging_owner_path(temporary, &owner_path);
	if (status != 0)
		return status;
	descriptor = open(
		owner_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (descriptor < 0)
	{
		status = errno == ENOENT || errno == ELOOP ?
			0 : (errno != 0 ? errno : EIO);
		goto finish;
	}
	if (fstat(descriptor, &owner_status) != 0)
	{
		status = errno != 0 ? errno : EIO;
		goto finish;
	}
	if (!S_ISREG(owner_status.st_mode))
	{
		status = 0;
		goto finish;
	}
	target_length = strlen(target);
	content = malloc(target_length + 2);
	if (content == NULL)
	{
		status = ENOMEM;
		goto finish;
	}
	do
	{
		count = read(descriptor, content, target_length + 2);
	} while (count < 0 && errno == EINTR);
	if (count < 0)
	{
		status = errno != 0 ? errno : EIO;
		goto finish;
	}
	*matches = (size_t) count == target_length + 1 &&
		memcmp(content, target, target_length) == 0 &&
		content[target_length] == '\n';
	status = 0;

finish:
	if (descriptor >= 0 && close(descriptor) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	free(content);
	free(owner_path);
	return status;
}


static int
remove_stale_temporary_directories(const char *parent, const char *target)
{
	const char *base;
	char	   *prefix = NULL;
	char	   *path = NULL;
	char	   *owner_path = NULL;
	DIR		   *directory = NULL;
	struct dirent *entry;
	size_t		prefix_length;
	size_t		parent_length;
	int			status = 0;

	if (parent == NULL || target == NULL)
		return EINVAL;
	base = strrchr(target, '/');
	if (base == NULL || base[1] == '\0')
		return EINVAL;
	base++;
	prefix_length = strlen(base) + sizeof("..postgamma-create.");
	prefix = malloc(prefix_length);
	if (prefix == NULL)
		return ENOMEM;
	(void) snprintf(
		prefix, prefix_length, ".%s.postgamma-create.", base);
	directory = opendir(parent);
	if (directory == NULL)
	{
		status = errno != 0 ? errno : EIO;
		goto finish;
	}
	parent_length = strlen(parent);
	errno = 0;
	while ((entry = readdir(directory)) != NULL)
	{
		struct stat entry_status;
		bool		owned = false;
		size_t		name_length;
		size_t		path_length;

		if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0)
			continue;
		name_length = strlen(entry->d_name);
		if (name_length != strlen(prefix) + POSTGAMMA_INITDB_STAGING_SUFFIX_LENGTH &&
			name_length != strlen(prefix) +
			POSTGAMMA_INITDB_STAGING_SUFFIX_LENGTH + strlen(".owner"))
			continue;
		if (parent_length > SIZE_MAX - name_length - 2)
		{
			status = EOVERFLOW;
			break;
		}
		path_length = parent_length + name_length + 2;
		path = malloc(path_length);
		if (path == NULL)
		{
			status = ENOMEM;
			break;
		}
		(void) snprintf(path, path_length, "%s/%s", parent, entry->d_name);
		if (name_length == strlen(prefix) +
			POSTGAMMA_INITDB_STAGING_SUFFIX_LENGTH + strlen(".owner"))
		{
			size_t		temporary_length =
				strlen(path) - strlen(".owner");

			owner_path = path;
			path = malloc(temporary_length + 1);
			if (path == NULL)
			{
				status = ENOMEM;
				break;
			}
			memcpy(path, owner_path, temporary_length);
			path[temporary_length] = '\0';
			status = staging_owner_matches(path, target, &owned);
			if (status != 0)
				break;
			if (lstat(path, &entry_status) != 0)
			{
				if (errno != ENOENT)
				{
					status = errno != 0 ? errno : EIO;
					break;
				}
				if (owned && unlink(owner_path) != 0 && errno != ENOENT)
				{
					status = errno != 0 ? errno : EIO;
					break;
				}
			}
			free(path);
			path = NULL;
			free(owner_path);
			owner_path = NULL;
			errno = 0;
			continue;
		}
		if (lstat(path, &entry_status) != 0)
		{
			status = errno == ENOENT ? 0 : (errno != 0 ? errno : EIO);
			free(path);
			path = NULL;
			if (status != 0)
				break;
			errno = 0;
			continue;
		}
		if (!S_ISDIR(entry_status.st_mode))
		{
			free(path);
			path = NULL;
			errno = 0;
			continue;
		}
		status = staging_owner_matches(path, target, &owned);
		if (status != 0)
			break;
		if (!owned)
		{
			free(path);
			path = NULL;
			errno = 0;
			continue;
		}
		status = staging_owner_path(path, &owner_path);
		if (status != 0)
			break;
		if (!rmtree(path, true))
		{
			status = errno != 0 ? errno : EIO;
			free(path);
			path = NULL;
			break;
		}
		if (unlink(owner_path) != 0 && errno != ENOENT)
		{
			status = errno != 0 ? errno : EIO;
			free(path);
			path = NULL;
			free(owner_path);
			owner_path = NULL;
			break;
		}
		free(path);
		path = NULL;
		free(owner_path);
		owner_path = NULL;
		errno = 0;
	}
	if (entry == NULL && errno != 0 && status == 0)
		status = errno;

finish:
	free(owner_path);
	free(path);
	if (directory != NULL && closedir(directory) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	free(prefix);
	return status;
}


static int
run_initdb(
	const PostgammaInitdbOptions *options, PostgammaInitdbResult *result)
{
	PostgammaInitdbContext context;
	char	  **arguments = NULL;
	int			argument_count = 0;
	int			status;
	int			main_status = 0;
	bool		execution_locked = false;

	if (result == NULL || result->struct_size != sizeof(*result))
		return EINVAL;
	set_result(result, EINVAL, 0, false, "validate", "invalid initdb options");
	if (!initdb_options_valid(options) || PostgammaCurrentInitdb != NULL)
		return EINVAL;
	memset(&context, 0, sizeof(context));
	context.options = options;
	context.logical_umask = options->logical_umask;
	status = build_initdb_arguments(options, &argument_count, &arguments);
	if (status != 0)
	{
		set_result(result, status, 0, false, "arguments",
			"could not build initdb arguments");
		return status;
	}
	context.output_stream = open_capture_stream(&context.output_capture);
	context.error_stream = open_capture_stream(&context.error_capture);
	if (context.output_stream == NULL || context.error_stream == NULL)
	{
		status = errno != 0 ? errno : ENOMEM;
		goto finish;
	}
	status = postgamma_tool_state_create(&context.tool_state);
	if (status != 0)
		goto finish;
	status = acquire_execution_lock(options->deadline_ns);
	if (status != 0)
		goto finish;
	execution_locked = true;
	PostgammaCurrentInitdb = &context;
	status = postgamma_tool_state_bind(
		context.tool_state, tool_state_failure, &context);
	if (status != 0)
		goto finish;
	context.tool_state_bound = true;
	context.jump_ready = true;
	if (sigsetjmp(context.exit_jump, 1) == 0)
	{
		optind = 1;
		opterr = 0;
		main_status =
			postgamma_pg19_initdb_main(argument_count, arguments);
		if (main_status != 0)
		{
			context.failure_status = EIO;
			context.exit_code = main_status;
		}
	}
	run_registered_cleanups(&context);
	context.jump_ready = false;
	status = context.failure_status;
	if (context.output_stream != NULL && fflush(context.output_stream) != 0 &&
		status == 0)
		status = errno != 0 ? errno : EIO;
	if (context.error_stream != NULL && fflush(context.error_stream) != 0 &&
		status == 0)
		status = errno != 0 ? errno : EIO;
	if (status == 0 && main_status == 0)
		set_result(result, 0, 0, true, "complete", "cluster created in process");
	else
		set_capture_result(
			result, status != 0 ? status : EIO, context.exit_code,
			&context.error_capture);

finish:
	if (context.pipe_stream != NULL)
	{
		(void) fclose(context.pipe_stream);
		context.pipe_stream = NULL;
	}
	free(context.pipe_data);
	if (context.tool_state_bound)
	{
		int			unbind_status =
			postgamma_tool_state_unbind(context.tool_state);

		if (status == 0 && unbind_status != 0)
			status = unbind_status;
		context.tool_state_bound = false;
	}
	PostgammaCurrentInitdb = NULL;
	postgamma_tool_state_destroy(&context.tool_state);
	if (execution_locked)
	{
		int			unlock_status =
			pthread_mutex_unlock(&PostgammaInitdbExecutionMutex);

		if (status == 0 && unlock_status != 0)
			status = unlock_status;
	}
	if (context.output_stream != NULL && fclose(context.output_stream) != 0 &&
		status == 0)
		status = errno != 0 ? errno : EIO;
	if (context.error_stream != NULL && fclose(context.error_stream) != 0 &&
		status == 0)
		status = errno != 0 ? errno : EIO;
	if (status != 0 && (result->status == EINVAL || result->status == 0))
		set_result(result, status, context.exit_code, false, "runtime",
			"could not initialize the in-process initdb runtime");
	free(context.output_capture.data);
	free(context.error_capture.data);
	free_initdb_arguments(argument_count, arguments);
	return status;
}


static int
postgamma_initdb_create_internal(
	const PostgammaInitdbOptions *options, PostgammaInitdbResult *result,
	PostgammaDataDirectoryLock **data_lock)
{
	PostgammaInitdbOptions temporary_options;
	PostgammaInitdbResult run_result = POSTGAMMA_INITDB_RESULT_INIT;
	PostgammaDataDirectoryLockOptions data_lock_options =
		POSTGAMMA_DATA_DIRECTORY_LOCK_OPTIONS_INIT;
	PostgammaDataDirectoryLock *acquired_data_lock = NULL;
	char	   *parent = NULL;
	char	   *target = NULL;
	char	   *temporary = NULL;
	char	   *owner_path = NULL;
	int			parent_descriptor = -1;
	int			creation_lock_descriptor = -1;
	int			status;
	uint64_t	deadline = 0;
	bool		exists = false;
	bool		empty = false;
	bool		valid_cluster = false;
	bool		temporary_created = false;
	bool		owner_created = false;
	bool		committed = false;

	if (result == NULL || result->struct_size != sizeof(*result))
		return EINVAL;
	if (data_lock != NULL)
		*data_lock = NULL;
	set_result(result, EINVAL, 0, false, "validate", "invalid initdb options");
	if (!initdb_options_valid(options) || PostgammaCurrentInitdb != NULL)
		return EINVAL;
	status = resolve_deadline(options->deadline_ns, &deadline);
	if (status != 0)
	{
		set_result(result, status, 0, false, "deadline",
			"could not establish the cluster creation deadline");
		goto finish;
	}
	status = canonicalize_target(
		options->data_directory, &parent, &target, &temporary);
	if (status != 0)
	{
		set_result(result, status, 0, false, "path",
			"could not resolve the cluster parent directory");
		goto finish;
	}
	parent_descriptor = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent_descriptor < 0)
	{
		status = errno != 0 ? errno : EIO;
		set_result(result, status, 0, false, "lock",
			"could not open the cluster parent directory");
		goto finish;
	}
	status = acquire_creation_lock(
		parent_descriptor, target, deadline, &creation_lock_descriptor);
	if (status != 0)
	{
		set_result(result, status, 0, false, "lock",
			status == ETIMEDOUT ?
			"timed out waiting for the target cluster creation lock" :
			"could not acquire the target cluster creation lock");
		goto finish;
	}
	status = remove_stale_temporary_directories(parent, target);
	if (status != 0)
	{
		set_result(result, status, 0, false, "stale-cleanup",
			"could not remove an abandoned temporary cluster");
		goto finish;
	}
	status = inspect_target(target, &exists, &empty, &valid_cluster);
	if (status != 0)
	{
		set_result(result, status, 0, false, "inspect",
			"could not inspect the cluster destination");
		goto finish;
	}
	if (options->require_missing && exists)
	{
		status = EEXIST;
		set_result(result, status, 0, false, "inspect",
			"create-new mode requires the cluster destination to be absent");
		goto finish;
	}
	if (valid_cluster)
	{
		set_result(result, 0, 0, false, "complete",
			"cluster already exists");
		status = 0;
		goto acquire_data_lock;
	}
	if (exists && !empty)
	{
		status = ENOTEMPTY;
		set_result(result, status, 0, false, "inspect",
			"cluster destination is nonempty and is not a PostgreSQL 19 cluster");
		goto finish;
	}
	if (mkdtemp(temporary) == NULL)
	{
		status = errno != 0 ? errno : EIO;
		set_result(result, status, 0, false, "temporary-directory",
			"could not create the temporary cluster directory");
		goto finish;
	}
	temporary_created = true;
	status = create_staging_owner(temporary, target, &owner_path);
	if (status != 0)
	{
		set_result(result, status, 0, false, "temporary-owner",
			"could not record temporary cluster ownership");
		goto finish;
	}
	owner_created = true;
	status = check_fault(
		options, POSTGAMMA_INITDB_FAULT_TEMPORARY_CREATED);
	if (status != 0)
	{
		set_result(result, status, 0, false, "fault-injection",
			"cluster creation stopped after creating the temporary directory");
		goto finish;
	}
	status = deadline_status(deadline);
	if (status != 0)
	{
		set_result(result, status, 0, false, "deadline",
			"cluster creation deadline expired before initialization");
		goto finish;
	}
	temporary_options = *options;
	temporary_options.data_directory = temporary;
	temporary_options.deadline_ns = deadline;
	status = run_initdb(&temporary_options, &run_result);
	if (status != 0)
	{
		*result = run_result;
		goto finish;
	}
	status = deadline_status(deadline);
	if (status != 0)
	{
		set_result(result, status, 0, false, "deadline",
			"cluster creation deadline expired before publication");
		goto finish;
	}
	status = check_fault(options, POSTGAMMA_INITDB_FAULT_INITDB_COMPLETE);
	if (status != 0)
	{
		set_result(result, status, 0, false, "fault-injection",
			"cluster creation stopped after in-process initialization");
		goto finish;
	}
	status = check_fault(options, POSTGAMMA_INITDB_FAULT_PUBLISH_READY);
	if (status != 0)
	{
		set_result(result, status, 0, false, "fault-injection",
			"cluster creation stopped before atomic publication");
		goto finish;
	}
	if (exists && rmdir(target) != 0)
	{
		status = errno != 0 ? errno : EIO;
		set_result(result, status, 0, false, "commit",
			"empty cluster destination changed during creation");
		goto finish;
	}
	if (rename(temporary, target) != 0)
	{
		status = errno != 0 ? errno : EIO;
		set_result(result, status, 0, false, "commit",
			"could not atomically publish the new cluster");
		goto finish;
	}
	committed = true;
	temporary_created = false;
	status = check_fault(options, POSTGAMMA_INITDB_FAULT_PUBLISHED);
	if (status != 0)
	{
		set_result(result, status, 0, true, "fault-injection",
			"cluster creation stopped after atomic publication");
		goto finish;
	}
	if (unlink(owner_path) != 0 && errno != ENOENT)
	{
		status = errno != 0 ? errno : EIO;
		set_result(result, status, 0, true, "commit-cleanup",
			"cluster was created but its staging owner could not be removed");
		goto finish;
	}
	owner_created = false;
	status = check_fault(options, POSTGAMMA_INITDB_FAULT_OWNER_REMOVED);
	if (status != 0)
	{
		set_result(result, status, 0, true, "fault-injection",
			"cluster creation stopped after removing its staging owner");
		goto finish;
	}
	if (fsync(parent_descriptor) != 0)
	{
		status = errno != 0 ? errno : EIO;
		set_result(result, status, 0, true, "sync",
			"cluster was created but its parent directory could not be synchronized");
		goto finish;
	}
	status = 0;
	set_result(result, 0, 0, true, "complete",
		"cluster created in process and published atomically");

acquire_data_lock:
	if (data_lock != NULL)
	{
		data_lock_options.generation = options->generation;
		data_lock_options.path = target;
		status = postgamma_data_directory_lock_acquire(
			&data_lock_options, &acquired_data_lock);
		if (status != 0)
		{
			set_result(
				result, status, result->postgres_exit_code, result->created,
				"instance-lock",
				result->created ?
				"cluster was created but its instance lock could not be acquired" :
				"cluster is already open by another embedded instance");
			goto finish;
		}
	}

finish:
	if (temporary_created && !committed && temporary != NULL)
	{
		if (!rmtree(temporary, true))
		{
			if (status == 0)
				status = errno != 0 ? errno : EIO;
		}
		else
			temporary_created = false;
	}
	if (owner_created && !temporary_created && owner_path != NULL)
	{
		int			unlink_status = unlink(owner_path);

		if (unlink_status == 0 || errno == ENOENT)
			owner_created = false;
		else if (status == 0)
			status = errno != 0 ? errno : EIO;
	}
	if (creation_lock_descriptor >= 0 &&
		flock(creation_lock_descriptor, LOCK_UN) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	if (creation_lock_descriptor >= 0 &&
		close(creation_lock_descriptor) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	if (parent_descriptor >= 0 && close(parent_descriptor) != 0 && status == 0)
		status = errno != 0 ? errno : EIO;
	if (status != 0 && result->status == 0)
		set_result(result, status, result->postgres_exit_code,
			result->created, "cleanup", "cluster creation cleanup failed");
	if (status != 0 && acquired_data_lock != NULL)
	{
		(void) postgamma_data_directory_lock_release(acquired_data_lock);
		acquired_data_lock = NULL;
	}
	if (status == 0 && data_lock != NULL)
	{
		*data_lock = acquired_data_lock;
		acquired_data_lock = NULL;
	}
	free(temporary);
	free(owner_path);
	free(target);
	free(parent);
	return status;
}


int
postgamma_initdb_create(
	const PostgammaInitdbOptions *options, PostgammaInitdbResult *result)
{
	return postgamma_initdb_create_internal(options, result, NULL);
}


int
postgamma_initdb_create_locked(
	const PostgammaInitdbOptions *options, PostgammaInitdbResult *result,
	PostgammaDataDirectoryLock **data_lock)
{
	if (data_lock == NULL)
		return EINVAL;
	return postgamma_initdb_create_internal(options, result, data_lock);
}
