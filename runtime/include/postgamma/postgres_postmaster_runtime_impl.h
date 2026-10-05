/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_postmaster_runtime_impl.h
 *    PostgreSQL-local adapter for the embedded supervisor event source.
 *
 * This header is included once by postmaster.c after its private function
 * declarations.  It is intentionally small and reuses PostgreSQL's original
 * wait loop, signal handlers, child reaper, and state machine.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_POSTMASTER_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_POSTMASTER_RUNTIME_IMPL_H

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/un.h>

#include "postgamma/postmaster_control_runtime.h"
#include "postgamma/postgres_checkpoint_runtime.h"
#include "utils/guc_tables.h"


static char PostgammaPostmasterControlEventTag;


typedef struct PostgammaEmbeddedSetting
{
	const char *name;
	const char *value;
} PostgammaEmbeddedSetting;

#include "postgamma/embedded_guc_policy.inc"


static pg_noreturn void
postgamma_postmaster_control_error(const char *operation, int status)
{
	int			report_status = status != 0 ? status : EIO;

	(void) postgamma_postmaster_control_current_fail(report_status);
	ereport(FATAL,
			(errmsg_internal("postgamma embedded postmaster %s failed: %s",
							 operation, strerror(report_status))));
}


static WaitEventSet *
postgamma_postmaster_create_wait_event_set(
	ResourceOwner owner,
	int event_count)
{
	WaitEventSet *wait_set;
	int			wake_fd;
	int			status;

	status = postgamma_postmaster_control_current_wake_fd(&wake_fd);
	if (status == ENOENT)
		return (CreateWaitEventSet)(owner, event_count);
	if (status != 0)
		postgamma_postmaster_control_error("wake-fd lookup", status);
	if (event_count == INT_MAX)
		postgamma_postmaster_control_error("wait-set sizing", EOVERFLOW);
	wait_set = (CreateWaitEventSet)(owner, event_count + 1);
	AddWaitEventToSet(
		wait_set,
		WL_SOCKET_READABLE,
		wake_fd,
		NULL,
		&PostgammaPostmasterControlEventTag);
	return wait_set;
}


static void
postgamma_postmaster_complete_control(
	PostgammaKernelControl *control,
	int operation_status)
{
	int			status = postgamma_postmaster_control_current_complete(
		control, operation_status);

	if (status != 0)
		postgamma_postmaster_control_error("control completion", status);
}


static int
postgamma_postmaster_dispatch_connect(void *payload)
{
	PostgammaKernelConnectRequest *request = payload;
	ClientSocket client_socket;
	struct sockaddr_un *remote_address;
	int			status;
	int			launch_status;

	status = postgamma_postmaster_connect_begin(request);
	if (status != 0)
		return status;
	memset(&client_socket, 0, sizeof(client_socket));
	client_socket.sock = PGINVALID_SOCKET;
	remote_address = (struct sockaddr_un *) &client_socket.raddr.addr;
	remote_address->sun_family = AF_UNIX;
	client_socket.raddr.salen =
		(socklen_t) offsetof(struct sockaddr_un, sun_path) + 1;
	errno = 0;
	launch_status = BackendStartup(&client_socket) == STATUS_OK ? 0 :
		(errno != 0 ? errno : EIO);
	status = postgamma_postmaster_connect_end(request, launch_status);
	return status;
}


static int
postgamma_postmaster_dispatch_cancel(void *payload, uint64_t generation)
{
	PostgammaKernelCancelRequest *request = payload;

	if (request == NULL || request->struct_size != sizeof(*request) ||
		request->abi_version != POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION ||
		request->generation != generation || request->request_generation == 0 ||
		request->backend_pid <= 0 || request->dispatched != 0)
		return EINVAL;
	if (kill((pid_t) request->backend_pid, SIGINT) != 0)
		return errno != 0 ? errno : EIO;
	request->dispatched = 1;
	return 0;
}


static void
postgamma_postmaster_dispatch_signal(int signal_number)
{
	switch (signal_number)
	{
		case SIGHUP:
			handle_pm_reload_request_signal(SIGHUP, NULL);
			break;
		case SIGINT:
			handle_pm_shutdown_request_signal(SIGINT, NULL);
			break;
		case SIGQUIT:
			handle_pm_shutdown_request_signal(SIGQUIT, NULL);
			break;
		case SIGTERM:
			handle_pm_shutdown_request_signal(SIGTERM, NULL);
			break;
		case SIGUSR1:
			handle_pm_pmsignal_signal(SIGUSR1, NULL);
			break;
		case SIGCHLD:
			handle_pm_child_exit_signal(SIGCHLD, NULL);
			break;
		case SIGALRM:
		case SIGPIPE:
		case SIGUSR2:
#ifdef SIGTTIN
		case SIGTTIN:
#endif
#ifdef SIGTTOU
		case SIGTTOU:
#endif
#ifdef SIGXFSZ
		case SIGXFSZ:
#endif
			break;
		default:
			postgamma_postmaster_control_error(
				"virtual signal dispatch", ENOTSUP);
	}
}


static void
postgamma_postmaster_dispatch_kernel_control(
	PostgammaKernelControl *control)
{
	int			operation_status = 0;

	switch (control->kind)
	{
		case POSTGAMMA_KERNEL_CONTROL_CONNECT:
			operation_status = postgamma_postmaster_dispatch_connect(
				control->payload);
			break;
		case POSTGAMMA_KERNEL_CONTROL_CANCEL:
			operation_status = postgamma_postmaster_dispatch_cancel(
				control->payload, control->generation);
			break;
		case POSTGAMMA_KERNEL_CONTROL_RELOAD:
			postgamma_postmaster_dispatch_signal(SIGHUP);
			break;
		case POSTGAMMA_KERNEL_CONTROL_ROLE_COMPLETION:
			postgamma_postmaster_dispatch_signal(SIGCHLD);
			break;
		case POSTGAMMA_KERNEL_CONTROL_CHECKPOINT:
			operation_status = postgamma_postgres_checkpoint_start(
				control->payload);
			break;
		case POSTGAMMA_KERNEL_CONTROL_SHUTDOWN:
			switch (control->shutdown_mode)
			{
				case POSTGAMMA_KERNEL_SHUTDOWN_SMART:
					postgamma_postmaster_dispatch_signal(SIGTERM);
					break;
				case POSTGAMMA_KERNEL_SHUTDOWN_FAST:
					postgamma_postmaster_dispatch_signal(SIGINT);
					break;
				case POSTGAMMA_KERNEL_SHUTDOWN_IMMEDIATE:
					postgamma_postmaster_dispatch_signal(SIGQUIT);
					break;
				default:
					operation_status = EINVAL;
					break;
			}
			break;
		case POSTGAMMA_KERNEL_CONTROL_DISCONNECT:
			operation_status = ENOTSUP;
			break;
		default:
			operation_status = EINVAL;
			break;
	}
	{
		PostgammaKernelCheckpointRequest *checkpoint_request =
			control->kind == POSTGAMMA_KERNEL_CONTROL_CHECKPOINT ?
			(PostgammaKernelCheckpointRequest *) control->payload : NULL;

		postgamma_postmaster_complete_control(control, operation_status);
		/* A failed start must become observable after its ticket completes. */
		if (checkpoint_request != NULL && operation_status != 0 &&
			checkpoint_request->tracker != NULL)
			(void) postgamma_instance_checkpoint_tracker_finish(
				(PostgammaCheckpointTracker *) checkpoint_request->tracker,
				checkpoint_request->generation, operation_status);
	}
}


static void
postgamma_postmaster_dispatch_control_event(WaitEvent *event)
{
	uint64_t	pending_signals = 0;
	uint64_t	wake_count = 0;
	int			status;

	if (event->user_data != &PostgammaPostmasterControlEventTag)
		return;

	/* WL_SOCKET_ACCEPT aliases WL_SOCKET_READABLE on Unix. */
	event->events = 0;
	status = postgamma_postmaster_control_current_wake_drain(&wake_count);
	if (status != 0)
		postgamma_postmaster_control_error("wake drain", status);

	/* A role completion and a host control share the same coalesced wake. */
	postgamma_postmaster_dispatch_signal(SIGCHLD);
	status = postgamma_postmaster_control_current_take_signals(
		&pending_signals);
	if (status != 0 && status != EAGAIN)
		postgamma_postmaster_control_error("virtual signal take", status);
	if (status == 0)
	{
		for (int signal_number = 1;
			 signal_number <= POSTGAMMA_INSTANCE_SIGNAL_MAX;
			 signal_number++)
		{
			uint64_t	bit = UINT64_C(1) << (signal_number - 1);

			if ((pending_signals & bit) != 0)
				postgamma_postmaster_dispatch_signal(signal_number);
		}
	}

	for (;;)
	{
		PostgammaKernelControl control;

		status = postgamma_postmaster_control_current_take(&control);
		if (status == EAGAIN)
			break;
		if (status != 0)
			postgamma_postmaster_control_error("control take", status);
		postgamma_postmaster_dispatch_kernel_control(&control);
	}
	(void) wake_count;
}


static void
postgamma_postmaster_report_state(PMState new_state)
{
	int			status;

	if (!postgamma_postmaster_control_runtime_is_bound())
		return;
	if (new_state == PM_STARTUP || new_state == PM_RECOVERY)
	{
		status = postgamma_postmaster_control_current_mark_recovering();
		if (status != 0)
			postgamma_postmaster_control_error(
				"recovery-state report", status);
	}
	else if (new_state == PM_RUN)
	{
		status = postgamma_postmaster_control_current_mark_ready();
		if (status != 0)
			postgamma_postmaster_control_error("ready-state report", status);
	}
}


static void
postgamma_postmaster_apply_embedded_defaults(void)
{
	if (!postgamma_postmaster_control_runtime_is_bound())
		return;
	for (size_t index = 0;
		 index < lengthof(PostgammaEmbeddedDefaults);
		 index++)
	{
		const PostgammaEmbeddedSetting *setting =
			&PostgammaEmbeddedDefaults[index];

		SetConfigOption(
			setting->name, setting->value,
			PGC_POSTMASTER, PGC_S_DYNAMIC_DEFAULT);
	}
}


static void
postgamma_postmaster_enforce_safety_setting(
	const PostgammaEmbeddedSetting *setting)
{
	struct config_generic *record;
	char	   *actual;

	record = find_option(setting->name, false, false, ERROR);
	if (record == NULL)
		postgamma_postmaster_control_error("safety-setting lookup", ENOENT);
	actual = ShowGUCOption(record, true);
	if (strcmp(actual, setting->value) != 0 &&
		record->source > PGC_S_DYNAMIC_DEFAULT)
	{
		if (record->sourcefile != NULL)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsafe setting \"%s\" is not allowed in embedded mode",
							setting->name),
					 errdetail("Value \"%s\" came from %s at \"%s\" line %d; embedded mode requires \"%s\".",
							   actual, GucSource_Names[record->source],
							   record->sourcefile, record->sourceline,
							   setting->value)));
		else
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsafe setting \"%s\" is not allowed in embedded mode",
							setting->name),
					 errdetail("Value \"%s\" came from %s; embedded mode requires \"%s\".",
							   actual, GucSource_Names[record->source],
							   setting->value)));
	}
	pfree(actual);
	SetConfigOption(
		setting->name, setting->value,
		PGC_POSTMASTER, PGC_S_OVERRIDE);
}


static void
postgamma_postmaster_enforce_embedded_policy(void)
{
	if (!postgamma_postmaster_control_runtime_is_bound())
		return;
	for (size_t index = 0;
		 index < lengthof(PostgammaEmbeddedSafetySettings);
		 index++)
		postgamma_postmaster_enforce_safety_setting(
			&PostgammaEmbeddedSafetySettings[index]);

	/* These string GUCs use NULL as the no-action fast path in postmaster.c. */
	POSTGAMMA_GUC_VALUE(ListenAddresses) = NULL;
	POSTGAMMA_GUC_VALUE(Unix_socket_directories) = NULL;
	POSTGAMMA_GUC_VALUE(external_pid_file) = NULL;
}


static bool
postgamma_postmaster_listener_required(void)
{
	return !postgamma_postmaster_control_runtime_is_bound();
}


static int
postgamma_postmaster_find_other_exec(
	const char *argv0, const char *target, const char *version,
	char *result_path)
{
	if (!postgamma_postmaster_control_runtime_is_bound())
		return (find_other_exec)(argv0, target, version, result_path);
	(void) target;
	(void) version;
	return find_my_exec(argv0, result_path);
}


static void
postgamma_postmaster_change_to_data_dir(void)
{
	if (!postgamma_postmaster_control_runtime_is_bound())
	{
		(ChangeToDataDir)();
		return;
	}
	if (postgamma_path_chdir(
			POSTGAMMA_BACKEND_STATE_GLOBAL(DataDir)) != 0)
		postgamma_postmaster_control_error(
			"virtual data-directory transition", errno);
}


static void
postgamma_postmaster_create_data_dir_lock_file(bool am_postmaster)
{
	if (!postgamma_postmaster_control_runtime_is_bound())
		(CreateDataDirLockFile)(am_postmaster);
}


static void
postgamma_postmaster_add_to_data_dir_lock_file(int target_line,
											   const char *string)
{
	if (!postgamma_postmaster_control_runtime_is_bound())
		(AddToDataDirLockFile)(target_line, string);
}


static bool
postgamma_postmaster_recheck_data_dir_lock_file(void)
{
	return postgamma_postmaster_control_runtime_is_bound() ? true :
		(RecheckDataDirLockFile)();
}


static void
postgamma_postmaster_touch_socket_lock_files(void)
{
	if (!postgamma_postmaster_control_runtime_is_bound())
		(TouchSocketLockFiles)();
}


#define POSTGAMMA_POSTMASTER_STATE(name) \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE( \
		"internal:src/backend/postmaster/postmaster.c:" \
		"src/backend/postmaster/postmaster.c::" #name, name)

#define POSTGAMMA_POSTMASTER_EXTERNAL_STATE(name) \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE("external:" #name, name)

void
postgamma_shutdown_postmaster_support(void)
{
	WaitEventSet *wait_set = POSTGAMMA_POSTMASTER_STATE(pm_wait_set);

	if (wait_set != NULL)
	{
		FreeWaitEventSet(wait_set);
		POSTGAMMA_POSTMASTER_STATE(pm_wait_set) = NULL;
	}
#ifndef WIN32
	for (int index = 0; index < 2; index++)
	{
		int			fd = POSTGAMMA_POSTMASTER_EXTERNAL_STATE(
			postmaster_alive_fds)[index];

		if (fd < 0)
			continue;
		(void) close(fd);
		POSTGAMMA_POSTMASTER_EXTERNAL_STATE(postmaster_alive_fds)[index] = -1;
		ReleaseExternalFD();
	}
#endif
}

#undef POSTGAMMA_POSTMASTER_EXTERNAL_STATE
#undef POSTGAMMA_POSTMASTER_STATE


#define CreateWaitEventSet(owner, event_count) \
	postgamma_postmaster_create_wait_event_set((owner), (event_count))
#define find_other_exec(argv0, target, version, result_path) \
	postgamma_postmaster_find_other_exec( \
		(argv0), (target), (version), (result_path))
#define ChangeToDataDir() postgamma_postmaster_change_to_data_dir()
#define CreateDataDirLockFile(am_postmaster) \
	postgamma_postmaster_create_data_dir_lock_file((am_postmaster))
#define AddToDataDirLockFile(target_line, string) \
	postgamma_postmaster_add_to_data_dir_lock_file((target_line), (string))
#define RecheckDataDirLockFile() \
	postgamma_postmaster_recheck_data_dir_lock_file()
#define TouchSocketLockFiles() \
	postgamma_postmaster_touch_socket_lock_files()

#endif /* POSTGAMMA_POSTGRES_POSTMASTER_RUNTIME_IMPL_H */
