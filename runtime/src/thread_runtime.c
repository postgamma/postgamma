/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * thread_runtime.c
 *    POSIX implementation of PostGamma's provider-neutral thread runtime.
 *
 *-------------------------------------------------------------------------
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "postgamma/thread_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif
#include <time.h>
#include <unistd.h>


#define POSTGAMMA_THREAD_NAME_MAX 15
#define POSTGAMMA_NS_PER_SECOND UINT64_C(1000000000)
#define POSTGAMMA_THREAD_START_PENDING (-1)
#define POSTGAMMA_KIBIBYTE ((size_t) 1024)
#define POSTGAMMA_MEBIBYTE (POSTGAMMA_KIBIBYTE * POSTGAMMA_KIBIBYTE)

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define POSTGAMMA_SANITIZED_THREAD_STACK 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define POSTGAMMA_SANITIZED_THREAD_STACK 1
#endif
#ifndef POSTGAMMA_SANITIZED_THREAD_STACK
#define POSTGAMMA_SANITIZED_THREAD_STACK 0
#endif

#if POSTGAMMA_SANITIZED_THREAD_STACK
#define POSTGAMMA_STACK_SCALE ((size_t) 2)
#else
#define POSTGAMMA_STACK_SCALE ((size_t) 1)
#endif

#define POSTGAMMA_GENERAL_STACK_SIZE \
	(2 * POSTGAMMA_MEBIBYTE * POSTGAMMA_STACK_SCALE)
#define POSTGAMMA_SUPERVISOR_STACK_SIZE \
	(4 * POSTGAMMA_MEBIBYTE * POSTGAMMA_STACK_SCALE)
#define POSTGAMMA_CLIENT_STACK_SIZE \
	(8 * POSTGAMMA_MEBIBYTE * POSTGAMMA_STACK_SCALE)
#define POSTGAMMA_DEDICATED_STACK_SIZE \
	(4 * POSTGAMMA_MEBIBYTE * POSTGAMMA_STACK_SCALE)
#define POSTGAMMA_PARALLEL_STACK_SIZE \
	(8 * POSTGAMMA_MEBIBYTE * POSTGAMMA_STACK_SCALE)
#define POSTGAMMA_FRONTEND_STACK_SIZE \
	(4 * POSTGAMMA_MEBIBYTE * POSTGAMMA_STACK_SCALE)
#define POSTGAMMA_STACK_GUARD_SIZE (64 * POSTGAMMA_KIBIBYTE)


struct PostgammaThread
{
	pthread_t	thread;
	PostgammaThreadMain main_function;
	void	   *argument;
	PostgammaThreadStackInfo stack_info;
	_Atomic int startup_status;
	bool		joined;
	char		name[POSTGAMMA_THREAD_NAME_MAX + 1];
};

struct PostgammaMutex
{
	pthread_mutex_t mutex;
};

struct PostgammaCondition
{
	pthread_cond_t condition;
};

typedef enum PostgammaWakeProvider
{
	POSTGAMMA_WAKE_EVENTFD,
	POSTGAMMA_WAKE_PIPE
} PostgammaWakeProvider;

struct PostgammaWakeTarget
{
	PostgammaWakeProvider provider;
	int			read_fd;
	int			write_fd;
};


static _Thread_local PostgammaThread *PostgammaCurrentThread;


static bool thread_role_is_valid(PostgammaThreadRole role);
static int resolve_stack_policy(
	const PostgammaThreadAttributes *attributes,
	PostgammaThreadStackInfo *stack_info);
static int query_current_stack(PostgammaThreadStackInfo *stack_info);
static void *thread_start(void *argument);
static int prepare_child_signal_mask(sigset_t *mask);
static int set_fd_flags(int descriptor);
static void add_wake_count(uint64_t *total, uint64_t increment);


int
postgamma_thread_role_stack_policy(
	PostgammaThreadRole role, size_t *stack_size, size_t *guard_size)
{
	if (!thread_role_is_valid(role) || stack_size == NULL || guard_size == NULL)
		return EINVAL;
	switch (role)
	{
		case POSTGAMMA_THREAD_ROLE_GENERAL:
			*stack_size = POSTGAMMA_GENERAL_STACK_SIZE;
			break;
		case POSTGAMMA_THREAD_ROLE_SUPERVISOR:
			*stack_size = POSTGAMMA_SUPERVISOR_STACK_SIZE;
			break;
		case POSTGAMMA_THREAD_ROLE_CLIENT:
			*stack_size = POSTGAMMA_CLIENT_STACK_SIZE;
			break;
		case POSTGAMMA_THREAD_ROLE_DEDICATED:
			*stack_size = POSTGAMMA_DEDICATED_STACK_SIZE;
			break;
		case POSTGAMMA_THREAD_ROLE_PARALLEL:
			*stack_size = POSTGAMMA_PARALLEL_STACK_SIZE;
			break;
		case POSTGAMMA_THREAD_ROLE_FRONTEND:
			*stack_size = POSTGAMMA_FRONTEND_STACK_SIZE;
			break;
		case POSTGAMMA_THREAD_ROLE_COUNT:
			return EINVAL;
	}
	*guard_size = POSTGAMMA_STACK_GUARD_SIZE;
	return 0;
}


int
postgamma_thread_create(PostgammaThread **thread,
						const PostgammaThreadAttributes *attributes,
						PostgammaThreadMain main_function,
						void *argument)
{
	PostgammaThread *created;
	pthread_attr_t native_attributes;
	sigset_t	all_signals;
	sigset_t	old_signals;
	int			status;
	int			restore_status;
	bool		attributes_initialized = false;
	bool		signal_mask_changed = false;

	if (thread == NULL || main_function == NULL)
		return EINVAL;
	*thread = NULL;

	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	status = resolve_stack_policy(attributes, &created->stack_info);
	if (status != 0)
	{
		free(created);
		return status;
	}
	created->main_function = main_function;
	created->argument = argument;
	atomic_init(&created->startup_status, POSTGAMMA_THREAD_START_PENDING);
	if (attributes != NULL && attributes->name != NULL)
	{
		size_t		length = strlen(attributes->name);

		if (length > POSTGAMMA_THREAD_NAME_MAX)
		{
			free(created);
			return ENAMETOOLONG;
		}
		memcpy(created->name, attributes->name, length + 1);
	}

	status = pthread_attr_init(&native_attributes);
	if (status != 0)
		goto fail;
	attributes_initialized = true;
	status = pthread_attr_setguardsize(
		&native_attributes, created->stack_info.configured_guard_size);
	if (status != 0)
		goto fail;
	status = pthread_attr_setstacksize(
		&native_attributes, created->stack_info.configured_stack_size);
	if (status != 0)
		goto fail;

	status = prepare_child_signal_mask(&all_signals);
	if (status != 0)
		goto fail;
	status = pthread_sigmask(SIG_SETMASK, &all_signals, &old_signals);
	if (status != 0)
		goto fail;
	signal_mask_changed = true;
	status = pthread_create(&created->thread, &native_attributes,
						 thread_start, created);
	restore_status = pthread_sigmask(SIG_SETMASK, &old_signals, NULL);
	signal_mask_changed = false;
	if (restore_status != 0)
		abort();
	if (status != 0)
		goto fail;

	(void) pthread_attr_destroy(&native_attributes);
	attributes_initialized = false;
	while ((status = atomic_load_explicit(
				&created->startup_status, memory_order_acquire)) ==
		   POSTGAMMA_THREAD_START_PENDING)
		(void) sched_yield();
	if (status != 0)
	{
		int			join_status = pthread_join(created->thread, NULL);

		free(created);
		return join_status != 0 ? join_status : status;
	}
	*thread = created;
	return 0;

fail:
	if (signal_mask_changed)
	{
		restore_status = pthread_sigmask(SIG_SETMASK, &old_signals, NULL);
		if (restore_status != 0)
			abort();
	}
	if (attributes_initialized)
		(void) pthread_attr_destroy(&native_attributes);
	free(created);
	return status;
}


int
postgamma_thread_join(PostgammaThread *thread, void **result)
{
	int			status;

	if (thread == NULL || thread->joined)
		return EINVAL;
	status = pthread_join(thread->thread, result);
	if (status == 0)
		thread->joined = true;
	return status;
}


int
postgamma_thread_destroy(PostgammaThread *thread)
{
	if (thread == NULL)
		return EINVAL;
	if (!thread->joined)
		return EBUSY;
	free(thread);
	return 0;
}


bool
postgamma_thread_is_current(const PostgammaThread *thread)
{
	return thread != NULL && pthread_equal(thread->thread, pthread_self()) != 0;
}


int
postgamma_thread_stack_info(
	const PostgammaThread *thread, PostgammaThreadStackInfo *stack_info)
{
	if (thread == NULL || stack_info == NULL)
		return EINVAL;
	if (atomic_load_explicit(&thread->startup_status, memory_order_acquire) != 0)
		return EPROTO;
	*stack_info = thread->stack_info;
	return 0;
}


int
postgamma_thread_current_stack_info(PostgammaThreadStackInfo *stack_info)
{
	if (stack_info == NULL)
		return EINVAL;
	if (PostgammaCurrentThread == NULL)
		return ENOENT;
	return postgamma_thread_stack_info(PostgammaCurrentThread, stack_info);
}


int
postgamma_thread_current_stack_limit(size_t *stack_limit)
{
	PostgammaThreadStackInfo stack_info;
	int			status;

	if (stack_limit == NULL)
		return EINVAL;
	*stack_limit = 0;
	status = postgamma_thread_current_stack_info(&stack_info);
	if (status != 0)
		return status;
	if (stack_info.usable_stack_size <= POSTGAMMA_THREAD_STACK_RUNTIME_SLOP)
		return ERANGE;
	*stack_limit = stack_info.usable_stack_size -
		POSTGAMMA_THREAD_STACK_RUNTIME_SLOP;
	return 0;
}


int
postgamma_thread_set_current_name(const char *name)
{
	if (name == NULL)
		return EINVAL;
	if (strlen(name) > POSTGAMMA_THREAD_NAME_MAX)
		return ENAMETOOLONG;
#if defined(__linux__)
	return pthread_setname_np(pthread_self(), name);
#else
	(void) name;
	return 0;
#endif
}


int
postgamma_thread_signal(PostgammaThread *thread, int signal_number)
{
	if (thread == NULL || signal_number <= 0)
		return EINVAL;
	return pthread_kill(thread->thread, signal_number);
}


int
postgamma_mutex_create(PostgammaMutex **mutex)
{
	PostgammaMutex *created;
	int			status;

	if (mutex == NULL)
		return EINVAL;
	*mutex = NULL;
	created = malloc(sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	status = pthread_mutex_init(&created->mutex, NULL);
	if (status != 0)
	{
		free(created);
		return status;
	}
	*mutex = created;
	return 0;
}


int
postgamma_mutex_destroy(PostgammaMutex *mutex)
{
	int			status;

	if (mutex == NULL)
		return EINVAL;
	status = pthread_mutex_destroy(&mutex->mutex);
	if (status == 0)
		free(mutex);
	return status;
}


int
postgamma_mutex_lock(PostgammaMutex *mutex)
{
	return mutex == NULL ? EINVAL : pthread_mutex_lock(&mutex->mutex);
}


int
postgamma_mutex_try_lock(PostgammaMutex *mutex)
{
	return mutex == NULL ? EINVAL : pthread_mutex_trylock(&mutex->mutex);
}


int
postgamma_mutex_unlock(PostgammaMutex *mutex)
{
	return mutex == NULL ? EINVAL : pthread_mutex_unlock(&mutex->mutex);
}


int
postgamma_condition_create(PostgammaCondition **condition)
{
	PostgammaCondition *created;
	pthread_condattr_t attributes;
	int			status;

	if (condition == NULL)
		return EINVAL;
	*condition = NULL;
	created = malloc(sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	status = pthread_condattr_init(&attributes);
	if (status != 0)
		goto fail;
	status = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (status == 0)
		status = pthread_cond_init(&created->condition, &attributes);
	(void) pthread_condattr_destroy(&attributes);
	if (status != 0)
		goto fail;
	*condition = created;
	return 0;

fail:
	free(created);
	return status;
}


int
postgamma_condition_destroy(PostgammaCondition *condition)
{
	int			status;

	if (condition == NULL)
		return EINVAL;
	status = pthread_cond_destroy(&condition->condition);
	if (status == 0)
		free(condition);
	return status;
}


int
postgamma_condition_wait(PostgammaCondition *condition,
						 PostgammaMutex *mutex)
{
	if (condition == NULL || mutex == NULL)
		return EINVAL;
	return pthread_cond_wait(&condition->condition, &mutex->mutex);
}


int
postgamma_condition_timed_wait(PostgammaCondition *condition,
							   PostgammaMutex *mutex,
							   uint64_t deadline_ns)
{
	struct timespec deadline;

	if (condition == NULL || mutex == NULL)
		return EINVAL;
	deadline.tv_sec = (time_t) (deadline_ns / POSTGAMMA_NS_PER_SECOND);
	deadline.tv_nsec = (long) (deadline_ns % POSTGAMMA_NS_PER_SECOND);
	return pthread_cond_timedwait(&condition->condition, &mutex->mutex,
								  &deadline);
}


int
postgamma_condition_signal(PostgammaCondition *condition)
{
	return condition == NULL ? EINVAL :
		pthread_cond_signal(&condition->condition);
}


int
postgamma_condition_broadcast(PostgammaCondition *condition)
{
	return condition == NULL ? EINVAL :
		pthread_cond_broadcast(&condition->condition);
}


int
postgamma_wake_target_create(PostgammaWakeTarget **target)
{
	PostgammaWakeTarget *created;
	int			descriptors[2];
	int			status;

	if (target == NULL)
		return EINVAL;
	*target = NULL;
	created = malloc(sizeof(*created));
	if (created == NULL)
		return ENOMEM;

#ifdef __linux__
	created->read_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (created->read_fd >= 0)
	{
		created->provider = POSTGAMMA_WAKE_EVENTFD;
		created->write_fd = created->read_fd;
		*target = created;
		return 0;
	}
	if (errno != ENOSYS && errno != EINVAL)
	{
		status = errno;
		free(created);
		return status;
	}
#endif

	if (pipe(descriptors) != 0)
	{
		status = errno;
		free(created);
		return status;
	}
	status = set_fd_flags(descriptors[0]);
	if (status == 0)
		status = set_fd_flags(descriptors[1]);
	if (status != 0)
	{
		(void) close(descriptors[0]);
		(void) close(descriptors[1]);
		free(created);
		return status;
	}
	created->provider = POSTGAMMA_WAKE_PIPE;
	created->read_fd = descriptors[0];
	created->write_fd = descriptors[1];
	*target = created;
	return 0;
}


int
postgamma_wake_target_destroy(PostgammaWakeTarget *target)
{
	int			status = 0;

	if (target == NULL)
		return EINVAL;
	if (close(target->read_fd) != 0)
		status = errno;
	if (target->write_fd != target->read_fd &&
		close(target->write_fd) != 0 && status == 0)
		status = errno;
	free(target);
	return status;
}


int
postgamma_wake_target_fd(const PostgammaWakeTarget *target)
{
	return target == NULL ? -1 : target->read_fd;
}


int
postgamma_wake_target_wake(PostgammaWakeTarget *target)
{
	ssize_t		written;

	if (target == NULL)
		return EINVAL;
	for (;;)
	{
		if (target->provider == POSTGAMMA_WAKE_EVENTFD)
		{
			uint64_t	one = 1;

			written = write(target->write_fd, &one, sizeof(one));
		}
		else
		{
			unsigned char one = 1;

			written = write(target->write_fd, &one, sizeof(one));
		}
		if (written > 0)
			return 0;
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		return written < 0 ? errno : EIO;
	}
}


int
postgamma_wake_target_drain(PostgammaWakeTarget *target,
							uint64_t *wake_count)
{
	uint64_t	total = 0;

	if (target == NULL || wake_count == NULL)
		return EINVAL;
	for (;;)
	{
		ssize_t		bytes_read;

		if (target->provider == POSTGAMMA_WAKE_EVENTFD)
		{
			uint64_t	value;

			bytes_read = read(target->read_fd, &value, sizeof(value));
			if (bytes_read == (ssize_t) sizeof(value))
			{
				add_wake_count(&total, value);
				continue;
			}
		}
		else
		{
			unsigned char buffer[256];

			bytes_read = read(target->read_fd, buffer, sizeof(buffer));
			if (bytes_read > 0)
			{
				add_wake_count(&total, (uint64_t) bytes_read);
				continue;
			}
		}
		if (bytes_read < 0 && errno == EINTR)
			continue;
		if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			*wake_count = total;
			return 0;
		}
		return bytes_read < 0 ? errno : EIO;
	}
}


uint64_t
postgamma_monotonic_now_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t) now.tv_sec * POSTGAMMA_NS_PER_SECOND +
		(uint64_t) now.tv_nsec;
}


static void *
thread_start(void *argument)
{
	PostgammaThread *thread = argument;
	void	   *result;
	int			status;

	PostgammaCurrentThread = thread;
	status = query_current_stack(&thread->stack_info);
	atomic_store_explicit(&thread->startup_status, status, memory_order_release);
	if (status != 0)
	{
		PostgammaCurrentThread = NULL;
		return NULL;
	}
	if (thread->name[0] != '\0')
		(void) postgamma_thread_set_current_name(thread->name);
	result = thread->main_function(thread->argument);
	PostgammaCurrentThread = NULL;
	return result;
}


static int
prepare_child_signal_mask(sigset_t *mask)
{
	static const int synchronous_signals[] = {
		SIGABRT,
		SIGBUS,
		SIGFPE,
		SIGILL,
		SIGSEGV,
	};

	if (sigfillset(mask) != 0)
		return errno;
	for (size_t index = 0;
		 index < sizeof(synchronous_signals) / sizeof(synchronous_signals[0]);
		 index++)
	{
		if (sigdelset(mask, synchronous_signals[index]) != 0)
			return errno;
	}
	return 0;
}


static bool
thread_role_is_valid(PostgammaThreadRole role)
{
	return role >= POSTGAMMA_THREAD_ROLE_GENERAL &&
		role < POSTGAMMA_THREAD_ROLE_COUNT;
}


static int
resolve_stack_policy(
	const PostgammaThreadAttributes *attributes,
	PostgammaThreadStackInfo *stack_info)
{
	PostgammaThreadRole role = attributes == NULL ?
		POSTGAMMA_THREAD_ROLE_GENERAL : attributes->role;
	size_t		stack_size;
	size_t		guard_size;
	int			status;

	if (stack_info == NULL)
		return EINVAL;
	status = postgamma_thread_role_stack_policy(
		role, &stack_size, &guard_size);
	if (status != 0)
		return status;
	if (attributes != NULL)
	{
		if (attributes->stack_size != 0)
			stack_size = attributes->stack_size;
		if (attributes->guard_size != 0)
			guard_size = attributes->guard_size;
	}
	if (stack_size < (size_t) PTHREAD_STACK_MIN || stack_size <= guard_size ||
		stack_size - guard_size < POSTGAMMA_THREAD_STACK_MINIMUM_USABLE)
		return EINVAL;
	memset(stack_info, 0, sizeof(*stack_info));
	stack_info->role = role;
	stack_info->configured_stack_size = stack_size;
	stack_info->configured_guard_size = guard_size;
	return 0;
}


static int
query_current_stack(PostgammaThreadStackInfo *stack_info)
{
#if defined(__linux__)
	pthread_attr_t attributes;
	void	   *stack_address;
	uintptr_t	low_address;
	uintptr_t	high_address;
	uintptr_t	stack_marker_address;
	size_t		stack_size;
	size_t		guard_size;
	int			status;
#if !defined(__GNUC__) && !defined(__clang__)
	char		stack_marker;
#endif

#if defined(__GNUC__) || defined(__clang__)
	stack_marker_address = (uintptr_t) __builtin_frame_address(0);
#else
	stack_marker_address = (uintptr_t) &stack_marker;
#endif

	if (stack_info == NULL)
		return EINVAL;
	status = pthread_getattr_np(pthread_self(), &attributes);
	if (status != 0)
		return status;
	status = pthread_attr_getstack(&attributes, &stack_address, &stack_size);
	if (status == 0)
		status = pthread_attr_getguardsize(&attributes, &guard_size);
	{
		int			destroy_status = pthread_attr_destroy(&attributes);

		if (status == 0 && destroy_status != 0)
			status = destroy_status;
	}
	if (status != 0)
		return status;
	low_address = (uintptr_t) stack_address;
	if (stack_size > UINTPTR_MAX - low_address)
		return EOVERFLOW;
	high_address = low_address + stack_size;
	if (stack_marker_address < low_address ||
		stack_marker_address >= high_address ||
		stack_size < stack_info->configured_stack_size ||
		guard_size < stack_info->configured_guard_size ||
		stack_size <= guard_size ||
		stack_size - guard_size < POSTGAMMA_THREAD_STACK_MINIMUM_USABLE)
		return ERANGE;
	stack_info->stack_low_address = low_address;
	stack_info->stack_high_address = high_address;
	stack_info->native_stack_size = stack_size;
	stack_info->native_guard_size = guard_size;
	stack_info->usable_stack_size = stack_size - guard_size;
	return 0;
#else
	(void) stack_info;
	return ENOTSUP;
#endif
}


static int
set_fd_flags(int descriptor)
{
	int			flags;

	flags = fcntl(descriptor, F_GETFL);
	if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
		return errno;
	flags = fcntl(descriptor, F_GETFD);
	if (flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0)
		return errno;
	return 0;
}


static void
add_wake_count(uint64_t *total, uint64_t increment)
{
	if (UINT64_MAX - *total < increment)
		*total = UINT64_MAX;
	else
		*total += increment;
}
