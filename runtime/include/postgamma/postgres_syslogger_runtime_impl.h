/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_syslogger_runtime_impl.h
 *    Descriptor-transfer bridge for the threaded PostgreSQL logger.
 *
 * A forked logger inherits independent references to the log files and pipe.
 * A logger thread must duplicate those descriptors before the postmaster
 * closes or repurposes its copies.  This implementation header is included
 * exactly once, by syslogger.c in the generated tree, so it can also close
 * the file-local resources when the logger role exits.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_POSTGRES_SYSLOGGER_RUNTIME_IMPL_H
#define POSTGAMMA_POSTGRES_SYSLOGGER_RUNTIME_IMPL_H

#define POSTGAMMA_SYSLOGGER_STATE(name) \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE( \
		"internal:src/backend/postmaster/syslogger.c:" \
		"src/backend/postmaster/syslogger.c::" #name, name)
#define POSTGAMMA_SYSLOGGER_PIPE_STATE \
	POSTGAMMA_BACKEND_STATE_NAMED_VALUE("external:syslogPipe", syslogPipe)

#ifndef WIN32
#define POSTGAMMA_SYSLOGGER_INVALID_DESCRIPTOR (-1)

static int
postgamma_syslogger_duplicate_descriptor(int inherited_descriptor)
{
	int			duplicated_descriptor;
	int			descriptor_flags;

	if (inherited_descriptor < 0)
		return -1;
	duplicated_descriptor = dup(inherited_descriptor);
	if (duplicated_descriptor < 0)
		return -1;
	descriptor_flags = fcntl(duplicated_descriptor, F_GETFD);
	if (descriptor_flags < 0 ||
		fcntl(duplicated_descriptor, F_SETFD,
			  descriptor_flags | FD_CLOEXEC) < 0)
	{
		int			save_errno = errno;

		(void) close(duplicated_descriptor);
		errno = save_errno;
		return -1;
	}
	return duplicated_descriptor;
}

static bool
postgamma_syslogger_descriptor_is_valid(int descriptor)
{
	return descriptor >= 0;
}

static void
postgamma_syslogger_close_descriptor(int descriptor)
{
	(void) close(descriptor);
}
#else
#define POSTGAMMA_SYSLOGGER_INVALID_DESCRIPTOR 0

static int
postgamma_syslogger_duplicate_descriptor(int inherited_descriptor)
{
	HANDLE		duplicated_handle;

	if (inherited_descriptor == 0)
		return 0;
	if (!DuplicateHandle(GetCurrentProcess(),
						 (HANDLE) (intptr_t) inherited_descriptor,
						 GetCurrentProcess(), &duplicated_handle,
						 0, TRUE, DUPLICATE_SAME_ACCESS))
	{
		_dosmaperr(GetLastError());
		return 0;
	}
	return (int) (intptr_t) duplicated_handle;
}

static bool
postgamma_syslogger_descriptor_is_valid(int descriptor)
{
	return descriptor != 0;
}

static void
postgamma_syslogger_close_descriptor(int descriptor)
{
	(void) CloseHandle((HANDLE) (intptr_t) descriptor);
}
#endif

static void
postgamma_syslogger_close_startup_descriptors(
	SysloggerStartupData *startup_data)
{
	int		   *descriptors[] = {
		&startup_data->syslogFile,
		&startup_data->csvlogFile,
		&startup_data->jsonlogFile,
	};

	for (size_t index = 0; index < lengthof(descriptors); index++)
	{
		if (postgamma_syslogger_descriptor_is_valid(*descriptors[index]))
			postgamma_syslogger_close_descriptor(*descriptors[index]);
		*descriptors[index] = POSTGAMMA_SYSLOGGER_INVALID_DESCRIPTOR;
	}
}

int
postgamma_prepare_syslogger_startup_data(
	void *startup_data, size_t startup_data_length)
{
	SysloggerStartupData *inherited = startup_data;
	SysloggerStartupData prepared = {
		POSTGAMMA_SYSLOGGER_INVALID_DESCRIPTOR,
		POSTGAMMA_SYSLOGGER_INVALID_DESCRIPTOR,
		POSTGAMMA_SYSLOGGER_INVALID_DESCRIPTOR,
	};
	const int  *inherited_descriptors[3];
	int		   *prepared_descriptors[3];

	if (startup_data == NULL || startup_data_length != sizeof(*inherited))
		return EINVAL;
	inherited_descriptors[0] = &inherited->syslogFile;
	inherited_descriptors[1] = &inherited->csvlogFile;
	inherited_descriptors[2] = &inherited->jsonlogFile;
	prepared_descriptors[0] = &prepared.syslogFile;
	prepared_descriptors[1] = &prepared.csvlogFile;
	prepared_descriptors[2] = &prepared.jsonlogFile;

	for (size_t index = 0; index < lengthof(inherited_descriptors); index++)
	{
		if (!postgamma_syslogger_descriptor_is_valid(
				*inherited_descriptors[index]))
			continue;
		*prepared_descriptors[index] =
			postgamma_syslogger_duplicate_descriptor(
				*inherited_descriptors[index]);
		if (!postgamma_syslogger_descriptor_is_valid(
				*prepared_descriptors[index]))
		{
			int			status = errno != 0 ? errno : EIO;

			postgamma_syslogger_close_startup_descriptors(&prepared);
			return status;
		}
	}
	*inherited = prepared;
	return 0;
}

void
postgamma_discard_syslogger_startup_data(
	void *startup_data, size_t startup_data_length)
{
	if (startup_data == NULL ||
		startup_data_length != sizeof(SysloggerStartupData))
		return;
	postgamma_syslogger_close_startup_descriptors(startup_data);
}

int
postgamma_prepare_syslogger_pipe_state(void)
{
	int			inherited_descriptor = POSTGAMMA_SYSLOGGER_PIPE_STATE[0];
	int			prepared_descriptor;

	if (!postgamma_syslogger_descriptor_is_valid(inherited_descriptor))
		return EINVAL;
	prepared_descriptor =
		postgamma_syslogger_duplicate_descriptor(inherited_descriptor);
	if (!postgamma_syslogger_descriptor_is_valid(prepared_descriptor))
		return errno != 0 ? errno : EIO;
	POSTGAMMA_SYSLOGGER_PIPE_STATE[0] = prepared_descriptor;
	POSTGAMMA_SYSLOGGER_PIPE_STATE[1] =
		POSTGAMMA_SYSLOGGER_INVALID_DESCRIPTOR;
	return 0;
}

void
postgamma_discard_syslogger_pipe_state(void)
{
	if (postgamma_syslogger_descriptor_is_valid(
			POSTGAMMA_SYSLOGGER_PIPE_STATE[0]))
		postgamma_syslogger_close_descriptor(
			POSTGAMMA_SYSLOGGER_PIPE_STATE[0]);
	POSTGAMMA_SYSLOGGER_PIPE_STATE[0] =
		POSTGAMMA_SYSLOGGER_INVALID_DESCRIPTOR;
	POSTGAMMA_SYSLOGGER_PIPE_STATE[1] =
		POSTGAMMA_SYSLOGGER_INVALID_DESCRIPTOR;
}

void
postgamma_shutdown_syslogger_file_access(void)
{
	if (POSTGAMMA_SYSLOGGER_STATE(syslogFile) != NULL)
	{
		(void) fclose(POSTGAMMA_SYSLOGGER_STATE(syslogFile));
		POSTGAMMA_SYSLOGGER_STATE(syslogFile) = NULL;
	}
	if (POSTGAMMA_SYSLOGGER_STATE(csvlogFile) != NULL)
	{
		(void) fclose(POSTGAMMA_SYSLOGGER_STATE(csvlogFile));
		POSTGAMMA_SYSLOGGER_STATE(csvlogFile) = NULL;
	}
	if (POSTGAMMA_SYSLOGGER_STATE(jsonlogFile) != NULL)
	{
		(void) fclose(POSTGAMMA_SYSLOGGER_STATE(jsonlogFile));
		POSTGAMMA_SYSLOGGER_STATE(jsonlogFile) = NULL;
	}
	postgamma_discard_syslogger_pipe_state();
}

#undef POSTGAMMA_SYSLOGGER_INVALID_DESCRIPTOR
#undef POSTGAMMA_SYSLOGGER_PIPE_STATE
#undef POSTGAMMA_SYSLOGGER_STATE

#endif /* POSTGAMMA_POSTGRES_SYSLOGGER_RUNTIME_IMPL_H */
