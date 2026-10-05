/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "postgamma/external_process.h"
#include "postgamma/contract_runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

struct PostgammaExternalPipe
{
	FILE	   *stream;
	pid_t		pid;
	int			wait_status;
	bool		stream_closed;
	bool		child_reaped;
	struct PostgammaExternalPipe *next;
};

static int postgamma_initialize_shell_spawn_attributes(
	posix_spawnattr_t *attributes, bool reset_sigpipe);
static int postgamma_set_close_on_exec(int descriptor);
static void postgamma_system_child_finished(
	PostgammaExternalProcessState *state, pid_t pid);
static void postgamma_external_pipe_finished(
	PostgammaExternalProcessState *state,
	PostgammaExternalPipe *pipe, int wait_status);
static int postgamma_wait_external_child(PostgammaExternalProcessState *state, pid_t pid, int *wait_status);






int
postgamma_external_system(PostgammaExternalProcessState *state, const char *command)
{
	PostgammaInstanceRuntime *runtime = state->runtime;
	posix_spawnattr_t attributes;
	char	   *const arguments[] = {
		(char *) "sh", (char *) "-c", (char *) command, NULL
	};
	pid_t		pid;
	int			result;
	int			status;
	struct timespec delay = {0, 10 * 1000 * 1000};

	if (runtime != NULL && postgamma_instance_runtime_profile(runtime) ==
		POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
	{
		errno = ENOTSUP;
		return -1;
	}
	if (command == NULL)
		return access("/bin/sh", X_OK) == 0;
	if (state->system_child_pid != 0)
	{
		errno = EDEADLK;
		return -1;
	}
	result = postgamma_initialize_shell_spawn_attributes(&attributes, false);
	if (result != 0)
	{
		errno = result;
		return -1;
	}
	result = postgamma_instance_runtime_reserve_external_process(state->runtime);
	if (result != 0)
	{
		(void) posix_spawnattr_destroy(&attributes);
		errno = result;
		return -1;
	}

	/*
	 * libc system() changes SIGINT and SIGQUIT dispositions for the entire
	 * process while it waits.  A role thread must never mutate those host
	 * dispositions, so spawn the shell with explicit child-only attributes.
	 * Reserve the child slot before spawning so postmaster waitpid(-1,
	 * WNOHANG) cannot reap this role-owned child between spawn and registration.
	 */
	result = posix_spawn(&pid, "/bin/sh", NULL, &attributes, arguments, environ);
	if (result == 0)
	{
		int			commit_status =
			postgamma_instance_runtime_commit_external_process(state->runtime);

		if (commit_status != 0)
			postgamma_runtime_contract_violation();
		state->system_child_pid = pid;
	}
	else if (postgamma_instance_runtime_cancel_external_process(
				 state->runtime) != 0)
		postgamma_runtime_contract_violation();
	(void) posix_spawnattr_destroy(&attributes);
	if (result != 0)
	{
		errno = result;
		return -1;
	}

	for (;;)
	{
		result = waitpid(pid, &status, WNOHANG);
		if (result == pid)
			break;
		if (result < 0 && errno != EINTR)
		{
			status = -1;
			break;
		}
		state->dispatch();
		(void) nanosleep(&delay, NULL);
	}
	postgamma_system_child_finished(state, pid);
	return status;
}

FILE *
postgamma_external_popen(PostgammaExternalProcessState *state, const char *command, const char *mode)
{
	PostgammaInstanceRuntime *runtime = state->runtime;
	PostgammaExternalPipe *external_pipe = NULL;
	posix_spawn_file_actions_t file_actions;
	posix_spawnattr_t attributes;
	char	   *const arguments[] = {
		(char *) "sh", (char *) "-c", (char *) command, NULL
	};
	int			pipe_fds[2] = {-1, -1};
	int			parent_fd;
	int			child_fd;
	int			child_target;
	int			result;
	bool		read_mode;
	bool		file_actions_initialized = false;
	bool		attributes_initialized = false;
	bool		external_process_reserved = false;

	if (runtime != NULL && postgamma_instance_runtime_profile(runtime) ==
		POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
	{
		errno = ENOTSUP;
		return NULL;
	}
	if (command == NULL || mode == NULL ||
		(mode[0] != 'r' && mode[0] != 'w') ||
		(mode[1] != '\0' && !(mode[1] == 'e' && mode[2] == '\0')))
	{
		errno = EINVAL;
		return NULL;
	}
	read_mode = mode[0] == 'r';
	if (pipe(pipe_fds) != 0)
		return NULL;
	for (int index = 0; index < 2; index++)
	{
		if (pipe_fds[index] <= STDERR_FILENO)
		{
			int			moved_fd = fcntl(pipe_fds[index], F_DUPFD,
									 STDERR_FILENO + 1);

			if (moved_fd < 0)
				goto fail;
			(void) close(pipe_fds[index]);
			pipe_fds[index] = moved_fd;
		}
		result = postgamma_set_close_on_exec(pipe_fds[index]);
		if (result != 0)
		{
			errno = result;
			goto fail;
		}
	}
	parent_fd = pipe_fds[read_mode ? 0 : 1];
	child_fd = pipe_fds[read_mode ? 1 : 0];
	child_target = read_mode ? STDOUT_FILENO : STDIN_FILENO;

	external_pipe = calloc(1, sizeof(*external_pipe));
	if (external_pipe == NULL)
		goto fail;
	external_pipe->stream = fdopen(parent_fd, read_mode ? "r" : "w");
	if (external_pipe->stream == NULL)
		goto fail;

	result = posix_spawn_file_actions_init(&file_actions);
	if (result != 0)
	{
		errno = result;
		goto fail;
	}
	file_actions_initialized = true;
	result = posix_spawn_file_actions_adddup2(
		&file_actions, child_fd, child_target);
	if (result == 0)
		result = posix_spawn_file_actions_addclose(&file_actions, pipe_fds[0]);
	if (result == 0)
		result = posix_spawn_file_actions_addclose(&file_actions, pipe_fds[1]);
	if (result != 0)
	{
		errno = result;
		goto fail;
	}
	result = postgamma_initialize_shell_spawn_attributes(&attributes, true);
	if (result != 0)
	{
		errno = result;
		goto fail;
	}
	attributes_initialized = true;
	result = postgamma_instance_runtime_reserve_external_process(state->runtime);
	if (result != 0)
	{
		errno = result;
		goto fail;
	}
	external_process_reserved = true;

	/*
	 * libc popen() hides its child PID, so the postmaster's process-wide
	 * waitpid(-1, WNOHANG) can reap a role-owned command before pclose().
	 * Reserve instance ownership before the spawn, then commit the PID to role
	 * state only after the child exists.
	 */
	result = posix_spawn(&external_pipe->pid, "/bin/sh", &file_actions,
						 &attributes, arguments, environ);
	if (result == 0)
	{
		int			commit_status =
			postgamma_instance_runtime_commit_external_process(state->runtime);

		if (commit_status != 0)
			postgamma_runtime_contract_violation();
		external_process_reserved = false;
		external_pipe->next = state->external_pipes;
		state->external_pipes = external_pipe;
	}
	else
	{
		if (postgamma_instance_runtime_cancel_external_process(
				state->runtime) != 0)
			postgamma_runtime_contract_violation();
		external_process_reserved = false;
	}
	(void) posix_spawnattr_destroy(&attributes);
	(void) posix_spawn_file_actions_destroy(&file_actions);
	attributes_initialized = false;
	file_actions_initialized = false;
	(void) close(child_fd);
	pipe_fds[read_mode ? 1 : 0] = -1;
	pipe_fds[read_mode ? 0 : 1] = -1;
	if (result != 0)
	{
		errno = result;
		goto fail;
	}
	return external_pipe->stream;

fail:
	{
		int			save_errno = errno;

		if (attributes_initialized)
			(void) posix_spawnattr_destroy(&attributes);
		if (file_actions_initialized)
			(void) posix_spawn_file_actions_destroy(&file_actions);
		if (external_process_reserved &&
			postgamma_instance_runtime_cancel_external_process(
				state->runtime) != 0)
			postgamma_runtime_contract_violation();
		if (external_pipe != NULL && external_pipe->stream != NULL)
		{
			(void) fclose(external_pipe->stream);
			pipe_fds[read_mode ? 0 : 1] = -1;
		}
		for (int index = 0; index < 2; index++)
		{
			if (pipe_fds[index] >= 0)
				(void) close(pipe_fds[index]);
		}
		free(external_pipe);
		errno = save_errno;
		return NULL;
	}
}

int
postgamma_external_pclose(PostgammaExternalProcessState *state, FILE *stream)
{
	PostgammaExternalPipe **link;
	PostgammaExternalPipe *external_pipe;
	int			close_errno = 0;
	int			wait_errno = 0;
	int			status;

	for (link = &state->external_pipes; *link != NULL; link = &(*link)->next)
	{
		if ((*link)->stream == stream)
			break;
	}
	if (*link == NULL)
	{
		if (postgamma_instance_runtime_profile(state->runtime) ==
			POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
		{
			errno = EINVAL;
			return -1;
		}
		return pclose(stream);
	}
	external_pipe = *link;
	if (!external_pipe->stream_closed)
	{
		if (fclose(stream) != 0)
			close_errno = errno;
		external_pipe->stream_closed = true;
	}
	if (!external_pipe->child_reaped)
	{
		wait_errno = postgamma_wait_external_child(
			state, external_pipe->pid, &status);
		postgamma_external_pipe_finished(
			state, external_pipe, wait_errno == 0 ? status : -1);
	}
	status = external_pipe->wait_status;
	*link = external_pipe->next;
	free(external_pipe);
	if (close_errno != 0 || wait_errno != 0)
	{
		errno = close_errno != 0 ? close_errno : wait_errno;
		return -1;
	}
	return status;
}

static int
postgamma_initialize_shell_spawn_attributes(
	posix_spawnattr_t *attributes, bool reset_sigpipe)
{
	short		flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
		POSIX_SPAWN_SETSIGMASK;
	sigset_t	default_signals;
	sigset_t	empty_mask;
	int			result;

	result = posix_spawnattr_init(attributes);
	if (result != 0)
		return result;
	sigemptyset(&default_signals);
	sigaddset(&default_signals, SIGINT);
	sigaddset(&default_signals, SIGQUIT);
	if (reset_sigpipe)
		sigaddset(&default_signals, SIGPIPE);
	sigemptyset(&empty_mask);
	result = posix_spawnattr_setsigdefault(attributes, &default_signals);
	if (result == 0)
		result = posix_spawnattr_setsigmask(attributes, &empty_mask);
	if (result == 0)
		result = posix_spawnattr_setpgroup(attributes, 0);
	if (result == 0)
		result = posix_spawnattr_setflags(attributes, flags);
	if (result != 0)
		(void) posix_spawnattr_destroy(attributes);
	return result;
}

static int
postgamma_set_close_on_exec(int descriptor)
{
	int			flags = fcntl(descriptor, F_GETFD);

	if (flags < 0)
		return errno != 0 ? errno : EIO;
	if (fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0)
		return errno != 0 ? errno : EIO;
	return 0;
}

static void
postgamma_system_child_finished(
	PostgammaExternalProcessState *state, pid_t pid)
{
	if (state == NULL || state->system_child_pid != pid)
		postgamma_runtime_contract_violation();
	state->system_child_pid = 0;
	if (postgamma_instance_runtime_record_external_process_finish(
			state->runtime) != 0 ||
		postgamma_instance_runtime_notify_completion(state->runtime) != 0)
		postgamma_runtime_contract_violation();
}

static void
postgamma_external_pipe_finished(
	PostgammaExternalProcessState *state,
	PostgammaExternalPipe *external_pipe, int wait_status)
{
	if (state == NULL || external_pipe == NULL ||
		external_pipe->child_reaped || external_pipe->pid == 0)
		postgamma_runtime_contract_violation();
	external_pipe->pid = 0;
	external_pipe->wait_status = wait_status;
	external_pipe->child_reaped = true;
	if (postgamma_instance_runtime_record_external_process_finish(
			state->runtime) != 0 ||
		postgamma_instance_runtime_notify_completion(state->runtime) != 0)
		postgamma_runtime_contract_violation();
}

static int
postgamma_wait_external_child(PostgammaExternalProcessState *state, pid_t pid, int *wait_status)
{
	struct timespec delay = {0, 10 * 1000 * 1000};

	for (;;)
	{
		pid_t		result = waitpid(pid, wait_status, WNOHANG);

		if (result == pid)
			return 0;
		if (result < 0 && errno != EINTR)
			return errno != 0 ? errno : ECHILD;
		state->dispatch();
		(void) nanosleep(&delay, NULL);
	}
}

void
postgamma_cleanup_external_children(PostgammaExternalProcessState *state)
{
	pid_t		pid = state->system_child_pid;
	int			status;

	if (pid != 0)
	{
		if (kill(-pid, SIGKILL) != 0 && errno != ESRCH)
			postgamma_runtime_contract_violation();
		while (waitpid(pid, &status, 0) < 0)
		{
			if (errno != EINTR && errno != ECHILD)
				postgamma_runtime_contract_violation();
			if (errno == ECHILD)
				break;
		}
		postgamma_system_child_finished(state, pid);
	}
	for (PostgammaExternalPipe *external_pipe = state->external_pipes;
		 external_pipe != NULL; external_pipe = external_pipe->next)
	{
		if (!external_pipe->stream_closed)
		{
			(void) fclose(external_pipe->stream);
			external_pipe->stream_closed = true;
		}
		if (external_pipe->child_reaped)
			continue;
		pid = external_pipe->pid;
		if (kill(-pid, SIGKILL) != 0 && errno != ESRCH)
			postgamma_runtime_contract_violation();
		while (waitpid(pid, &status, 0) < 0)
		{
			if (errno != EINTR && errno != ECHILD)
				postgamma_runtime_contract_violation();
			if (errno == ECHILD)
			{
				status = -1;
				break;
			}
		}
		postgamma_external_pipe_finished(state, external_pipe, status);
	}
}

void
postgamma_release_external_pipes(PostgammaExternalProcessState *state)
{
	while (state->external_pipes != NULL)
	{
		PostgammaExternalPipe *external_pipe = state->external_pipes;

		state->external_pipes = external_pipe->next;
		if (!external_pipe->stream_closed)
			(void) fclose(external_pipe->stream);
		if (!external_pipe->child_reaped)
			postgamma_runtime_contract_violation();
		free(external_pipe);
	}
}
