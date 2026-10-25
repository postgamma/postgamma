/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/guc_runtime.h"
#include "../check.h"

#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct TransactionState
{
	int marker;
	unsigned char padding[116];
} TransactionState;

typedef struct List
{
	struct List *next;
	struct List *previous;
} List;

static TransactionState TopTemplate = {.marker = 41};
static TransactionState *CurrentTemplate = &TopTemplate;
static List BackgroundWorkerList = {&BackgroundWorkerList, &BackgroundWorkerList};
static void *session_timezone;
static _Thread_local unsigned RelocationCalls;

void *__real_postgamma_backend_state_address_relocated(
	PostgammaBackendStateSlot slot, const volatile void *template_address,
	const PostgammaBackendStateTemplate *targets, size_t target_count);
void *__wrap_postgamma_backend_state_address_relocated(
	PostgammaBackendStateSlot slot, const volatile void *template_address,
	const PostgammaBackendStateTemplate *targets, size_t target_count);

void *
__wrap_postgamma_backend_state_address_relocated(
	PostgammaBackendStateSlot slot, const volatile void *template_address,
	const PostgammaBackendStateTemplate *targets, size_t target_count)
{
	RelocationCalls++;
	return __real_postgamma_backend_state_address_relocated(
		slot, template_address, targets, target_count);
}

static TransactionState **
current_state(void)
{
	const PostgammaBackendStateTemplate targets[] = {
		{POSTGAMMA_BACKEND_STATE_SLOT_BF87053C70C878C4, &TopTemplate},
	};

	return postgamma_backend_state_address_relocated_cached(
		POSTGAMMA_BACKEND_STATE_SLOT_99EE0B8E2B9438FC, &CurrentTemplate,
		targets, sizeof(targets) / sizeof(targets[0]));
}

void *
postgamma_backend_state_relocated_3e5e677e95eecc0b(void)
{
	const PostgammaBackendStateTemplate targets[] = {
		{POSTGAMMA_BACKEND_STATE_SLOT_3E5E677E95EECC0B, &BackgroundWorkerList},
	};

	return postgamma_backend_state_address_relocated_cached(
		POSTGAMMA_BACKEND_STATE_SLOT_3E5E677E95EECC0B, &BackgroundWorkerList, targets, 1);
}

static List *
list_state(void)
{
	return &POSTGAMMA_BACKEND_STATE_GLOBAL(BackgroundWorkerList);
}

static void
check_unbound(void)
{
	CHECK(postgamma_execution_context_current() == NULL);
	CHECK(PostgammaCurrentBackendStateAddresses != NULL);
	for (int i = 0; i < POSTGAMMA_BACKEND_STATE_SLOT_COUNT; i++)
		CHECK(PostgammaCurrentBackendStateAddresses[i] == NULL);
}

static void
check_initialized_state(TransactionState **expected)
{
	unsigned calls = RelocationCalls;

	for (int i = 0; i < 100; i++)
		CHECK(current_state() == expected);
	CHECK(RelocationCalls == calls);
	CHECK(postgamma_backend_state_address_by_id(
		"internal:src/backend/access/transam/xact.c:"
		"src/backend/access/transam/xact.c::CurrentTransactionState",
		&CurrentTemplate) == expected);
}

static void *
check_on_another_carrier(void *argument)
{
	PostgammaExecutionContext *execution = argument;
	PostgammaExecutionContext *previous;
	TransactionState **current;

	check_unbound();
	previous = postgamma_execution_context_bind(execution);
	CHECK(previous == NULL);
	CHECK(RelocationCalls == 0);
	current = current_state();
	CHECK(RelocationCalls == 0);
	CHECK((*current)->marker == 73);
	CHECK(POSTGAMMA_BACKEND_STATE_GLOBAL(session_timezone) == execution);
	check_initialized_state(current);
	postgamma_execution_context_restore(execution, previous);
	check_unbound();
	return NULL;
}

/* Invalid boundaries must abort, including when compiled with NDEBUG. */
static void
check_contract_failure(int which, PostgammaExecutionContext *execution)
{
	pid_t child = fork();
	int status;

	CHECK(child >= 0);
	if (child == 0)
	{
		struct rlimit limit = {0, 0};

		CHECK(setrlimit(RLIMIT_CORE, &limit) == 0);
		switch (which)
		{
			case 0: /* No execution, ordinary lazy initialization. */
				(void) postgamma_backend_state_address_cached(
					POSTGAMMA_BACKEND_STATE_SLOT_BF87053C70C878C4, &TopTemplate);
				break;
			case 1: /* No execution, generated relocation initializer. */
				(void) current_state();
				break;
			case 2:
				(void) postgamma_backend_state_address_cached(
					(PostgammaBackendStateSlot) -1, &TopTemplate);
				break;
			case 3:
				(void) postgamma_backend_state_address_cached(
					POSTGAMMA_BACKEND_STATE_SLOT_COUNT, &TopTemplate);
				break;
			case 4: /* A warm slot does not make a NULL template valid. */
				(void) postgamma_backend_state_address_cached(
					POSTGAMMA_BACKEND_STATE_SLOT_BF87053C70C878C4, NULL);
				break;
			case 5:
				execution->magic = 0;
				(void) postgamma_execution_context_bind(execution);
				break;
			case 6:
				execution->role->instance = NULL;
				(void) postgamma_execution_context_bind(execution);
				break;
			case 7:
				PostgammaCurrentBackendStateAddresses = NULL;
				(void) postgamma_execution_context_bind(execution);
				break;
			case 8:
				PostgammaCurrentBackendStateAddresses = NULL;
				postgamma_execution_context_restore(execution, NULL);
				break;
			default:
				_exit(2);
		}
		_exit(0);
	}
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

int
main(void)
{
	PostgammaInstanceContext instance;
	PostgammaRoleContext roles[2];
	PostgammaExecutionContext executions[2];
	PostgammaExecutionContext same_role;
	PostgammaExecutionContext *previous;
	PostgammaExecutionContext *nested;
	TransactionState **first;
	TransactionState **second;
	List *list;
	pthread_t carrier;

	check_unbound();
	for (int i = 0; i < 4; i++)
		check_contract_failure(i, NULL);
	CHECK(postgamma_instance_context_init(&instance) == 0);
	for (int i = 0; i < 2; i++)
	{
		CHECK(postgamma_role_context_init(&roles[i], &instance) == 0);
		CHECK(postgamma_execution_context_init(&executions[i], &instance, &roles[i]) == 0);
	}
	CHECK(postgamma_execution_context_init(&same_role, &instance, &roles[0]) == 0);
	previous = postgamma_execution_context_bind(&executions[0]);
	CHECK(previous == NULL);
	CHECK(PostgammaCurrentBackendStateAddresses[POSTGAMMA_BACKEND_STATE_SLOT_99EE0B8E2B9438FC] == NULL);
	CHECK(RelocationCalls == 0);
	CHECK(POSTGAMMA_BACKEND_STATE_GLOBAL(session_timezone) == NULL);
	POSTGAMMA_BACKEND_STATE_GLOBAL(session_timezone) = &executions[0];
	first = current_state();
	CHECK(RelocationCalls == 1);
	CHECK(*first != &TopTemplate && (*first)->marker == 41);
	CHECK(*first == postgamma_backend_state_address_cached(
		POSTGAMMA_BACKEND_STATE_SLOT_BF87053C70C878C4, &TopTemplate));
	(*first)->marker = 73;
	check_initialized_state(first);
	for (int i = 4; i < 9; i++)
		check_contract_failure(i, &executions[0]);

	/* A cache miss must retain relocation, including a self-referencing slot. */
	list = list_state();
	CHECK(list != &BackgroundWorkerList && list->next == list && list->previous == list);
	CHECK(RelocationCalls == 2);
	CHECK(list_state() == list);
	CHECK(RelocationCalls == 2);

	nested = postgamma_execution_context_bind(&executions[1]);
	CHECK(nested == &executions[0]);
	second = current_state();
	CHECK(RelocationCalls == 3);
	CHECK(second != first && *second != *first && (*second)->marker == 41);
	/* Cached access reads the live variable rather than copying its template. */
	*second = NULL;
	check_initialized_state(second);
	CHECK(*current_state() == NULL);
	postgamma_execution_context_restore(&executions[1], nested);
	CHECK(current_state() == first && (*first)->marker == 73);
	CHECK(RelocationCalls == 3);
	/* Another execution of this role resolves the existing live state once. */
	nested = postgamma_execution_context_bind(&same_role);
	CHECK(POSTGAMMA_BACKEND_STATE_GLOBAL(session_timezone) == NULL);
	POSTGAMMA_BACKEND_STATE_GLOBAL(session_timezone) = &same_role;
	CHECK(current_state() == first && (*first)->marker == 73);
	CHECK(RelocationCalls == 4);
	check_initialized_state(first);
	postgamma_execution_context_restore(&same_role, nested);
	CHECK(POSTGAMMA_BACKEND_STATE_GLOBAL(session_timezone) == &executions[0]);
	CHECK(current_state() == first && RelocationCalls == 4);
	postgamma_execution_context_restore(&executions[0], previous);
	check_unbound();

	CHECK(pthread_create(&carrier, NULL, check_on_another_carrier, &executions[0]) == 0);
	CHECK(pthread_join(carrier, NULL) == 0);
	postgamma_execution_context_destroy(&same_role);
	for (int i = 0; i < 2; i++)
	{
		postgamma_execution_context_destroy(&executions[i]);
		postgamma_role_context_destroy(&roles[i]);
	}
	postgamma_instance_context_destroy(&instance);
	return 0;
}
