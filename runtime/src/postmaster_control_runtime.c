/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postmaster_control_runtime.c
 *    Embedded postmaster control-provider binding.
 *
 *-------------------------------------------------------------------------
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "postgamma/postmaster_control_runtime.h"

#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>


#define POSTGAMMA_POSTMASTER_CONTROL_MAGIC \
	UINT64_C(0x5047504D4354524C)
#define POSTGAMMA_REQUIRED_HOST_CAPABILITIES \
	(POSTGAMMA_KERNEL_HOST_CAP_CONTROL_WAKE_FD | \
	 POSTGAMMA_KERNEL_HOST_CAP_ASYNC_NOTIFICATION | \
	 POSTGAMMA_KERNEL_HOST_CAP_FAIL_STOP)
#define POSTGAMMA_LOCALE_NAME_CAPACITY 128


typedef enum PostgammaControlLocaleCategory
{
	POSTGAMMA_CONTROL_LOCALE_COLLATE = 0,
	POSTGAMMA_CONTROL_LOCALE_CTYPE,
	POSTGAMMA_CONTROL_LOCALE_MESSAGES,
	POSTGAMMA_CONTROL_LOCALE_MONETARY,
	POSTGAMMA_CONTROL_LOCALE_NUMERIC,
	POSTGAMMA_CONTROL_LOCALE_TIME,
	POSTGAMMA_CONTROL_LOCALE_CATEGORY_COUNT
} PostgammaControlLocaleCategory;


struct PostgammaPostmasterControlRuntime
{
	uint64_t	magic;
	uint64_t	generation;
	PostgammaInstanceRuntime *instance_runtime;
	PostgammaKernelHostProvider host;
	int			wake_fd;
	sigset_t	logical_signal_mask;
	locale_t	thread_locale;
	locale_t	previous_thread_locale;
	char		locale_names[POSTGAMMA_CONTROL_LOCALE_CATEGORY_COUNT]
		[POSTGAMMA_LOCALE_NAME_CAPACITY];
	_Atomic bool bound;
	bool		recovering_reported;
	bool		ready_reported;
	bool		locale_names_initialized;
};


static POSTGAMMA_THREAD_LOCAL PostgammaPostmasterControlRuntime *
	PostgammaCurrentPostmasterControlRuntime;


static bool control_runtime_is_valid(
	const PostgammaPostmasterControlRuntime *control_runtime);
static bool host_provider_is_valid(const PostgammaKernelHostProvider *host);
static PostgammaPostmasterControlRuntime *current_control_runtime(void);
static int locale_category_index(int category);
static int locale_category_mask(int category);
static const char *effective_locale_name(
	int category, const char *locale_name);
static int initialize_locale_names(
	PostgammaPostmasterControlRuntime *control_runtime);
static int copy_locale_name(char *destination, const char *source);


int
postgamma_postmaster_control_runtime_create(
	PostgammaPostmasterControlRuntime **control_runtime,
	PostgammaInstanceRuntime *instance_runtime,
	const PostgammaKernelHostProvider *host)
{
	PostgammaPostmasterControlRuntime *created;
	uint64_t	generation;
	int			status;
	int			wake_fd = -1;

	if (control_runtime == NULL || instance_runtime == NULL ||
		!host_provider_is_valid(host) ||
		postgamma_instance_runtime_profile(instance_runtime) !=
		POSTGAMMA_RUNTIME_PROFILE_EMBEDDED)
		return EINVAL;
	*control_runtime = NULL;
	generation = postgamma_instance_runtime_generation(instance_runtime);
	if (generation == 0)
		return EINVAL;
	status = host->control_wake_fd(host->context, generation, &wake_fd);
	if (status != 0)
		return status;
	if (wake_fd < 0)
		return EPROTO;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->magic = POSTGAMMA_POSTMASTER_CONTROL_MAGIC;
	created->generation = generation;
	created->instance_runtime = instance_runtime;
	created->host = *host;
	created->wake_fd = wake_fd;
	if (sigemptyset(&created->logical_signal_mask) != 0)
	{
		status = errno != 0 ? errno : EIO;
		memset(created, 0, sizeof(*created));
		free(created);
		return status;
	}
	atomic_init(&created->bound, false);
	*control_runtime = created;
	return 0;
}


int
postgamma_postmaster_control_runtime_destroy(
	PostgammaPostmasterControlRuntime *control_runtime)
{
	if (!control_runtime_is_valid(control_runtime))
		return EINVAL;
	if (atomic_load_explicit(&control_runtime->bound, memory_order_acquire))
		return EBUSY;
	control_runtime->magic = 0;
	memset(control_runtime, 0, sizeof(*control_runtime));
	free(control_runtime);
	return 0;
}


int
postgamma_postmaster_control_runtime_bind(
	PostgammaPostmasterControlRuntime *control_runtime)
{
	bool		expected = false;
	locale_t	previous_locale;

	if (!control_runtime_is_valid(control_runtime))
		return EINVAL;
	if (PostgammaCurrentPostmasterControlRuntime != NULL)
		return EALREADY;
	if (!atomic_compare_exchange_strong_explicit(
			&control_runtime->bound, &expected, true,
			memory_order_acq_rel, memory_order_acquire))
		return EBUSY;
	previous_locale = uselocale((locale_t) 0);
	if (previous_locale == (locale_t) 0)
	{
		int status = errno != 0 ? errno : EIO;

		atomic_store_explicit(
			&control_runtime->bound, false, memory_order_release);
		return status;
	}
	control_runtime->previous_thread_locale = previous_locale;
	PostgammaCurrentPostmasterControlRuntime = control_runtime;
	return 0;
}


int
postgamma_postmaster_control_runtime_unbind(
	PostgammaPostmasterControlRuntime *control_runtime)
{
	if (control_runtime_is_valid(control_runtime) &&
		PostgammaCurrentPostmasterControlRuntime == control_runtime &&
		control_runtime->thread_locale != (locale_t) 0)
	{
		if (uselocale(control_runtime->previous_thread_locale) ==
			(locale_t) 0)
			return errno != 0 ? errno : EIO;
		freelocale(control_runtime->thread_locale);
		control_runtime->thread_locale = (locale_t) 0;
	}
	if (!control_runtime_is_valid(control_runtime) ||
		PostgammaCurrentPostmasterControlRuntime != control_runtime)
		return EINVAL;
	control_runtime->previous_thread_locale = (locale_t) 0;
	control_runtime->locale_names_initialized = false;
	PostgammaCurrentPostmasterControlRuntime = NULL;
	if (!atomic_exchange_explicit(
			&control_runtime->bound, false, memory_order_acq_rel))
		return EPROTO;
	return 0;
}


bool
postgamma_postmaster_control_runtime_is_bound(void)
{
	return current_control_runtime() != NULL;
}


int
postgamma_postmaster_control_current_wake_fd(int *wake_fd)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		current_control_runtime();

	if (wake_fd == NULL)
		return EINVAL;
	*wake_fd = -1;
	if (control_runtime == NULL)
		return ENOENT;
	*wake_fd = control_runtime->wake_fd;
	return 0;
}


int
postgamma_postmaster_control_current_wake_drain(uint64_t *wake_count)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		current_control_runtime();

	if (wake_count == NULL)
		return EINVAL;
	*wake_count = 0;
	if (control_runtime == NULL)
		return ENOENT;
	return control_runtime->host.control_wake_drain(
		control_runtime->host.context,
		control_runtime->generation,
		wake_count);
}


int
postgamma_postmaster_control_current_take_signals(
	uint64_t *pending_signals)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		current_control_runtime();

	if (pending_signals == NULL)
		return EINVAL;
	*pending_signals = 0;
	if (control_runtime == NULL)
		return ENOENT;
	return postgamma_instance_runtime_take_supervisor_signals(
		control_runtime->instance_runtime,
		control_runtime->generation,
		pending_signals);
}


int
postgamma_postmaster_control_current_take(
	PostgammaKernelControl *control)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		current_control_runtime();

	if (control == NULL)
		return EINVAL;
	memset(control, 0, sizeof(*control));
	if (control_runtime == NULL)
		return ENOENT;
	return control_runtime->host.control_take(
		control_runtime->host.context,
		control_runtime->generation,
		control);
}


int
postgamma_postmaster_control_current_complete(
	PostgammaKernelControl *control,
	int operation_status)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		current_control_runtime();

	if (control_runtime == NULL)
		return ENOENT;
	if (control == NULL || control->generation != control_runtime->generation)
		return EINVAL;
	return control_runtime->host.control_complete(
		control_runtime->host.context,
		control,
		operation_status);
}


int
postgamma_postmaster_control_current_mark_recovering(void)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		current_control_runtime();
	int			status;

	if (control_runtime == NULL)
		return ENOENT;
	if (control_runtime->recovering_reported)
		return 0;
	status = control_runtime->host.mark_recovering(
		control_runtime->host.context,
		control_runtime->generation);
	if (status == 0)
		control_runtime->recovering_reported = true;
	return status;
}


int
postgamma_postmaster_control_current_mark_ready(void)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		current_control_runtime();
	int			status;

	if (control_runtime == NULL)
		return ENOENT;
	if (control_runtime->ready_reported)
		return 0;
	status = control_runtime->host.mark_ready(
		control_runtime->host.context,
		control_runtime->generation);
	if (status == 0)
		control_runtime->ready_reported = true;
	return status;
}


int
postgamma_postmaster_control_current_fail(int failure_status)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		current_control_runtime();

	if (control_runtime == NULL)
		return ENOENT;
	if (failure_status == 0)
		failure_status = EIO;
	return control_runtime->host.fail(
		control_runtime->host.context,
		control_runtime->generation,
		failure_status);
}


int
postgamma_postmaster_control_current_sigprocmask(
	int how,
	const sigset_t *set,
	sigset_t *old_set)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		current_control_runtime();

	if (control_runtime == NULL)
		return ENOENT;
	if (old_set != NULL)
		*old_set = control_runtime->logical_signal_mask;
	if (set == NULL)
		return 0;
	switch (how)
	{
		case SIG_BLOCK:
			for (int signal_number = 1;
				 signal_number <= POSTGAMMA_INSTANCE_SIGNAL_MAX;
				 signal_number++)
			{
				if (sigismember(set, signal_number) == 1)
					sigaddset(
						&control_runtime->logical_signal_mask,
						signal_number);
			}
			break;
		case SIG_UNBLOCK:
			for (int signal_number = 1;
				 signal_number <= POSTGAMMA_INSTANCE_SIGNAL_MAX;
				 signal_number++)
			{
				if (sigismember(set, signal_number) == 1)
					sigdelset(
						&control_runtime->logical_signal_mask,
						signal_number);
			}
			break;
		case SIG_SETMASK:
			control_runtime->logical_signal_mask = *set;
			break;
		default:
			return EINVAL;
	}
	return 0;
}


char *
postgamma_postmaster_control_current_setlocale(
	int category, const char *locale_name)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		current_control_runtime();
	locale_t	base;
	locale_t	updated;
	locale_t	old_locale;
	const char *resolved;
	char		resolved_name[POSTGAMMA_LOCALE_NAME_CAPACITY];
	int			category_index;
	int			category_mask;
	int			status;

	if (control_runtime == NULL)
	{
		errno = ENOENT;
		return NULL;
	}
	category_index = locale_category_index(category);
	category_mask = locale_category_mask(category);
	if (category_index < 0 || category_mask == 0)
	{
		errno = EINVAL;
		return NULL;
	}
	status = initialize_locale_names(control_runtime);
	if (status != 0)
	{
		errno = status;
		return NULL;
	}
	if (locale_name == NULL)
		return control_runtime->locale_names[category_index];
	resolved = effective_locale_name(category, locale_name);
	status = copy_locale_name(resolved_name, resolved);
	if (status != 0)
	{
		errno = status;
		return NULL;
	}

	old_locale = control_runtime->thread_locale;
	base = duplocale(old_locale != (locale_t) 0 ?
		old_locale : control_runtime->previous_thread_locale);
	if (base == (locale_t) 0)
		return NULL;
	updated = newlocale(category_mask, locale_name, base);
	if (updated == (locale_t) 0)
	{
		status = errno;
		freelocale(base);
		errno = status;
		return NULL;
	}
	if (uselocale(updated) == (locale_t) 0)
	{
		status = errno;
		freelocale(updated);
		errno = status;
		return NULL;
	}
	control_runtime->thread_locale = updated;
	if (old_locale != (locale_t) 0)
		freelocale(old_locale);
	memcpy(control_runtime->locale_names[category_index], resolved_name,
		strlen(resolved_name) + 1);
	return control_runtime->locale_names[category_index];
}


static bool
control_runtime_is_valid(
	const PostgammaPostmasterControlRuntime *control_runtime)
{
	return control_runtime != NULL &&
		control_runtime->magic == POSTGAMMA_POSTMASTER_CONTROL_MAGIC &&
		control_runtime->generation != 0 &&
		control_runtime->instance_runtime != NULL;
}


static bool
host_provider_is_valid(const PostgammaKernelHostProvider *host)
{
	return host != NULL &&
		host->struct_size == sizeof(*host) &&
		host->abi_version == POSTGAMMA_EMBEDDED_KERNEL_ABI_VERSION &&
		(host->capabilities & POSTGAMMA_REQUIRED_HOST_CAPABILITIES) ==
		POSTGAMMA_REQUIRED_HOST_CAPABILITIES &&
		host->context != NULL &&
		host->mark_recovering != NULL &&
		host->mark_ready != NULL &&
		host->control_wake_fd != NULL &&
		host->control_wake_drain != NULL &&
		host->control_notify != NULL &&
		host->control_take != NULL &&
		host->control_complete != NULL &&
		host->fail != NULL;
}


static PostgammaPostmasterControlRuntime *
current_control_runtime(void)
{
	PostgammaPostmasterControlRuntime *control_runtime =
		PostgammaCurrentPostmasterControlRuntime;

	return control_runtime_is_valid(control_runtime) ? control_runtime : NULL;
}


static int
locale_category_index(int category)
{
	switch (category)
	{
		case LC_COLLATE:
			return POSTGAMMA_CONTROL_LOCALE_COLLATE;
		case LC_CTYPE:
			return POSTGAMMA_CONTROL_LOCALE_CTYPE;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			return POSTGAMMA_CONTROL_LOCALE_MESSAGES;
#endif
		case LC_MONETARY:
			return POSTGAMMA_CONTROL_LOCALE_MONETARY;
		case LC_NUMERIC:
			return POSTGAMMA_CONTROL_LOCALE_NUMERIC;
		case LC_TIME:
			return POSTGAMMA_CONTROL_LOCALE_TIME;
	}
	return -1;
}


static int
locale_category_mask(int category)
{
	switch (category)
	{
		case LC_COLLATE:
			return LC_COLLATE_MASK;
		case LC_CTYPE:
			return LC_CTYPE_MASK;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			return LC_MESSAGES_MASK;
#endif
		case LC_MONETARY:
			return LC_MONETARY_MASK;
		case LC_NUMERIC:
			return LC_NUMERIC_MASK;
		case LC_TIME:
			return LC_TIME_MASK;
	}
	return 0;
}


static const char *
effective_locale_name(int category, const char *locale_name)
{
	const char *category_environment = NULL;
	const char *result;

	if (locale_name != NULL && locale_name[0] != '\0')
		return locale_name;
	result = getenv("LC_ALL");
	if (result != NULL && result[0] != '\0')
		return result;
	switch (category)
	{
		case LC_COLLATE:
			category_environment = "LC_COLLATE";
			break;
		case LC_CTYPE:
			category_environment = "LC_CTYPE";
			break;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			category_environment = "LC_MESSAGES";
			break;
#endif
		case LC_MONETARY:
			category_environment = "LC_MONETARY";
			break;
		case LC_NUMERIC:
			category_environment = "LC_NUMERIC";
			break;
		case LC_TIME:
			category_environment = "LC_TIME";
			break;
	}
	if (category_environment != NULL)
	{
		result = getenv(category_environment);
		if (result != NULL && result[0] != '\0')
			return result;
	}
	result = getenv("LANG");
	return result != NULL && result[0] != '\0' ? result : "C";
}


static int
initialize_locale_names(
	PostgammaPostmasterControlRuntime *control_runtime)
{
	static const int categories[POSTGAMMA_CONTROL_LOCALE_CATEGORY_COUNT] =
	{
		LC_COLLATE,
		LC_CTYPE,
#ifdef LC_MESSAGES
		LC_MESSAGES,
#else
		-1,
#endif
		LC_MONETARY,
		LC_NUMERIC,
		LC_TIME
	};

	if (control_runtime->locale_names_initialized)
		return 0;
	for (int index = 0;
		 index < POSTGAMMA_CONTROL_LOCALE_CATEGORY_COUNT;
		 index++)
	{
		const char *name = categories[index] < 0 ? "C" :
			effective_locale_name(categories[index], "");
		int status = copy_locale_name(
			control_runtime->locale_names[index], name);

		if (status != 0)
			return status;
	}
	control_runtime->locale_names_initialized = true;
	return 0;
}


static int
copy_locale_name(char *destination, const char *source)
{
	size_t length;

	if (destination == NULL || source == NULL)
		return EINVAL;
	length = strlen(source);
	if (length >= POSTGAMMA_LOCALE_NAME_CAPACITY)
		return ENAMETOOLONG;
	memcpy(destination, source, length + 1);
	return 0;
}
