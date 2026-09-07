/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_backend_runtime.h
 *    PostgreSQL-facing boundary for threaded backend executions.
 *
 * This header is installed only in generated server trees.  The public
 * function signatures deliberately use stable C and PostgreSQL base types;
 * version-specific launch facts remain in generated include files.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_BACKEND_RUNTIME_H
#define POSTGAMMA_POSTGRES_BACKEND_RUNTIME_H

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef WIN32
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "postgamma/backend_execution_runtime.h"
#include "postgamma/embedded_kernel.h"
#include "postgamma/extension_runtime.h"
#include "postgamma/path_runtime.h"
#include "postgamma/static_module_provider.h"


typedef void (*PostgammaPostgresBackendMain) (
	const void *startup_data, size_t startup_data_length);
struct ErrorData;

int postgamma_threaded_server_completion_notify(
	void *argument, uint64_t generation);
pid_t postgamma_postmaster_child_launch(
	int backend_type, int child_slot,
	void *startup_data, size_t startup_data_length,
	const void *client_socket,
	PostgammaPostgresBackendMain main_function,
	bool shared_memory_access, const char *thread_name);
int postgamma_postmaster_connect_begin(
	PostgammaKernelConnectRequest *request);
int postgamma_postmaster_connect_end(
	PostgammaKernelConnectRequest *request,
	int launch_status);

bool postgamma_in_backend_thread(void);
bool postgamma_backend_transport_is_bound(void);
bool postgamma_backend_transport_read(
	void *buffer, size_t length, ssize_t *result);
bool postgamma_backend_transport_write(
	const void *buffer, size_t length, ssize_t *result);
int postgamma_backend_transport_getsockname(
	int socket_descriptor,
	struct sockaddr *address,
	socklen_t *address_length);
bool postgamma_locale_environment_is_private(void);
int postgamma_backend_current_wake_fd(void);
int postgamma_backend_current_wake_drain(uint64_t *wake_count);
ssize_t postgamma_backend_stack_depth_limit(void);
void postgamma_backend_log_telemetry(void);
void postgamma_embedded_emit_log(struct ErrorData *error_data);
bool postgamma_backend_pqsignal(int signal_number, pqsigfunc handler);
int postgamma_bootstrap_input_bind(const char *input, size_t length);
int postgamma_bootstrap_input_unbind(const char *expected_input);
int postgamma_bootstrap_scanner_bind(void *scanner);
int postgamma_standalone_input_bind(const char *input, size_t length);
int postgamma_standalone_input_unbind(const char *expected_input);
bool postgamma_standalone_input_getc(int *result);
bool postgamma_embedded_skip_system_collation_import(void);
bool postgamma_embedded_load_external_function(
	const char *filename, const char *function_name, bool signal_not_found,
	void **file_handle, void **result);
bool postgamma_embedded_lookup_external_function(
	void *file_handle, const char *function_name, void **result);
bool postgamma_embedded_load_file(const char *filename, bool restricted);
void postgamma_load_bundled_extensions(void);
int postgamma_backend_printf(const char *format, ...)
	pg_attribute_printf(1, 2);
void postgamma_dispatch_pending_signals(void);
int postgamma_sigprocmask(int how, const sigset_t *set, sigset_t *old_set);
#ifndef WIN32
char *postgamma_backend_setlocale(int category, const char *locale);
int postgamma_backend_setitimer(int which, const struct itimerval *new_value,
								   struct itimerval *old_value);
#endif
long postgamma_backend_adjust_wait_timeout(long requested_timeout);
bool postgamma_backend_timeout_wait_expired(int wait_result);
bool postgamma_postgres_quantum_begin(
	volatile bool *send_ready_for_query,
	volatile bool *idle_in_transaction_timeout_enabled,
	volatile bool *idle_session_timeout_enabled);
void postgamma_postgres_quantum_initialized(void);
bool postgamma_postgres_quantum_yield_ready(
	bool send_ready_for_query,
	bool idle_in_transaction_timeout_enabled,
	bool idle_session_timeout_enabled);
bool postgamma_postgres_quantum_yield_command_read(
	bool send_ready_for_query,
	bool idle_in_transaction_timeout_enabled,
	bool idle_session_timeout_enabled);
void postgamma_postgres_blocking_begin(void);
void postgamma_postgres_blocking_end(void);
void postgamma_postgres_result_budget_begin(void);
void postgamma_postgres_result_budget_check(
	const void *data, size_t length);
pid_t postgamma_getpid(void);
bool postgamma_backend_pid_is_compat(pid_t pid);
int postgamma_kill(pid_t pid, int signal_number);
pid_t postgamma_waitpid(pid_t pid, int *exit_status, int options);
int postgamma_system(const char *command);
FILE *postgamma_popen(const char *command, const char *mode);
int postgamma_pclose(FILE *stream);
int postgamma_atexit(void (*function) (void));
void postgamma_backend_exit(int code);
void postgamma_backend_panic(void);
pg_noreturn void postgamma_immediate_exit(int code);
pg_noreturn void postgamma_abort(void);
pid_t postgamma_forbidden_fork_process(void);

/* Narrow bridges implemented beside PostgreSQL's file-local wait state. */
void postgamma_shutdown_postmaster_support(void);
void postgamma_shutdown_latch_wait_set(void);
void postgamma_shutdown_wait_event_support(void);
void postgamma_shutdown_file_access(bool preserve_temporary_files);
void postgamma_shutdown_xlog_file_access(void);
void postgamma_shutdown_allocset_freelists(void);
void postgamma_shutdown_snapshot_storage(void);
void postgamma_shutdown_error_support(void);
void postgamma_shutdown_owned_data_directory(void);
int postgamma_prepare_syslogger_startup_data(
	void *startup_data, size_t startup_data_length);
void postgamma_discard_syslogger_startup_data(
	void *startup_data, size_t startup_data_length);
int postgamma_prepare_syslogger_pipe_state(void);
void postgamma_discard_syslogger_pipe_state(void);
void postgamma_shutdown_syslogger_file_access(void);
void postgamma_destroy_postgres_memory_contexts(bool invoke_callbacks);

#if !defined(POSTGAMMA_POSTGRES_RUNTIME_IMPLEMENTATION)
#ifndef WIN32
#define getpid() postgamma_getpid()
#define kill(pid, signal_number) \
	postgamma_kill((pid), (signal_number))
#define waitpid(pid, exit_status, options) \
	postgamma_waitpid((pid), (exit_status), (options))
#define system(command) postgamma_system((command))
#define popen(command, mode) postgamma_popen((command), (mode))
#define pclose(stream) postgamma_pclose((stream))
#define sigprocmask(how, set, old_set) \
	postgamma_sigprocmask((how), (set), (old_set))
#define setlocale(category, locale) \
	postgamma_backend_setlocale((category), (locale))
#define setitimer(which, new_value, old_value) \
	postgamma_backend_setitimer((which), (new_value), (old_value))
#define _exit(code) postgamma_immediate_exit((code))
#define open(...) postgamma_path_open(__VA_ARGS__)
#define openat(...) postgamma_path_openat(__VA_ARGS__)
#define fopen(path, mode) postgamma_path_fopen((path), (mode))
#define stat(path, status_buffer) \
	postgamma_path_stat((path), (status_buffer))
#define lstat(path, status_buffer) \
	postgamma_path_lstat((path), (status_buffer))
#define access(path, mode) postgamma_path_access((path), (mode))
#define unlink(path) postgamma_path_unlink((path))
#define remove(path) postgamma_path_remove((path))
#define rename(old_path, new_path) \
	postgamma_path_rename((old_path), (new_path))
#define link(old_path, new_path) \
	postgamma_path_link((old_path), (new_path))
#define symlink(target, link_path) \
	postgamma_path_symlink((target), (link_path))
#define readlink(path, buffer, buffer_size) \
	postgamma_path_readlink((path), (buffer), (buffer_size))
#define mkdir(path, mode) postgamma_path_mkdir((path), (mode))
#define rmdir(path) postgamma_path_rmdir((path))
#define chmod(path, mode) postgamma_path_chmod((path), (mode))
#define chown(path, owner, group) \
	postgamma_path_chown((path), (owner), (group))
#define truncate(path, length) postgamma_path_truncate((path), (length))
#define opendir(path) postgamma_path_opendir((path))
#define realpath(path, resolved_path) \
	postgamma_path_realpath((path), (resolved_path))
#define chdir(path) postgamma_path_chdir((path))
#define getcwd(buffer, size) postgamma_path_getcwd((buffer), (size))
#define umask(mask) postgamma_path_umask((mask))
#endif
#define atexit(function) postgamma_atexit((function))
#define abort() postgamma_abort()
#ifdef printf
#undef printf
#endif
#define printf(...) postgamma_backend_printf(__VA_ARGS__)
#endif

#endif /* POSTGAMMA_POSTGRES_BACKEND_RUNTIME_H */
