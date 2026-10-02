/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * thread_runtime.h
 *    Provider-neutral thread primitives used by postgamma runtime code.
 *
 * Native pthread types stay private to the implementation.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGAMMA_THREAD_RUNTIME_H
#define POSTGAMMA_THREAD_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#ifndef POSTGAMMA_THREAD_LOCAL
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define POSTGAMMA_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define POSTGAMMA_THREAD_LOCAL __thread
#else
#error "postgamma requires compiler-supported thread-local storage"
#endif
#endif


typedef struct PostgammaThread PostgammaThread;
typedef struct PostgammaMutex PostgammaMutex;
typedef struct PostgammaCondition PostgammaCondition;
typedef struct PostgammaWakeTarget PostgammaWakeTarget;

typedef void *(*PostgammaThreadMain) (void *argument);

typedef enum PostgammaThreadRole
{
	POSTGAMMA_THREAD_ROLE_GENERAL = 0,
	POSTGAMMA_THREAD_ROLE_SUPERVISOR,
	POSTGAMMA_THREAD_ROLE_CLIENT,
	POSTGAMMA_THREAD_ROLE_DEDICATED,
	POSTGAMMA_THREAD_ROLE_PARALLEL,
	POSTGAMMA_THREAD_ROLE_FRONTEND,
	POSTGAMMA_THREAD_ROLE_COUNT
} PostgammaThreadRole;

#define POSTGAMMA_THREAD_STACK_RUNTIME_SLOP (256U * 1024U)
#define POSTGAMMA_THREAD_STACK_MINIMUM_USABLE (1024U * 1024U)

typedef struct PostgammaThreadAttributes
{
	const char *name;
	PostgammaThreadRole role;
	size_t		stack_size;
	size_t		guard_size;
} PostgammaThreadAttributes;

typedef struct PostgammaThreadStackInfo
{
	PostgammaThreadRole role;
	size_t		configured_stack_size;
	size_t		configured_guard_size;
	uintptr_t	stack_low_address;
	uintptr_t	stack_high_address;
	size_t		native_stack_size;
	size_t		native_guard_size;
	size_t		usable_stack_size;
} PostgammaThreadStackInfo;

int postgamma_thread_role_stack_policy(
	PostgammaThreadRole role, size_t *stack_size, size_t *guard_size);
int postgamma_thread_create(PostgammaThread **thread,
							const PostgammaThreadAttributes *attributes,
							PostgammaThreadMain main_function,
							void *argument);
int postgamma_thread_join(PostgammaThread *thread, void **result);
int postgamma_thread_destroy(PostgammaThread *thread);
bool postgamma_thread_is_current(const PostgammaThread *thread);
int postgamma_thread_stack_info(
	const PostgammaThread *thread, PostgammaThreadStackInfo *stack_info);
int postgamma_thread_current_stack_info(
	PostgammaThreadStackInfo *stack_info);
int postgamma_thread_current_stack_limit(size_t *stack_limit);
int postgamma_thread_set_current_name(const char *name);
int postgamma_thread_signal(PostgammaThread *thread, int signal_number);

int postgamma_mutex_create(PostgammaMutex **mutex);
int postgamma_mutex_destroy(PostgammaMutex *mutex);
int postgamma_mutex_lock(PostgammaMutex *mutex);
int postgamma_mutex_try_lock(PostgammaMutex *mutex);
int postgamma_mutex_unlock(PostgammaMutex *mutex);

int postgamma_condition_create(PostgammaCondition **condition);
int postgamma_condition_destroy(PostgammaCondition *condition);
int postgamma_condition_wait(PostgammaCondition *condition,
								 PostgammaMutex *mutex);
int postgamma_condition_timed_wait(PostgammaCondition *condition,
								   PostgammaMutex *mutex,
								   uint64_t deadline_ns);
int postgamma_condition_signal(PostgammaCondition *condition);
int postgamma_condition_broadcast(PostgammaCondition *condition);

int postgamma_wake_target_create(PostgammaWakeTarget **target);
int postgamma_wake_target_destroy(PostgammaWakeTarget *target);
int postgamma_wake_target_fd(const PostgammaWakeTarget *target);
int postgamma_wake_target_wake(PostgammaWakeTarget *target);
int postgamma_wake_target_drain(PostgammaWakeTarget *target,
								uint64_t *wake_count);

uint64_t postgamma_monotonic_now_ns(void);

#endif
