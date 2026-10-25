/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "postgamma/guc_runtime.h"
#include "postgamma/memory_block_runtime.h"
#include "../check.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int Value = 42;
static int Lazy = 11;
static volatile int Pending = 0;
static void *Session;
static void *A;
static void *B = &A;
static struct { void *next; void *previous; } Self = {&Self, &Self};
static long FailAllocation = -1;
static size_t AllocationLimit = SIZE_MAX;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);

static bool
allocation_fails(void)
{
	return FailAllocation >= 0 && FailAllocation-- == 0;
}

void *__wrap_malloc(size_t size)
{
	return size > AllocationLimit || allocation_fails() ? NULL : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
	return (size != 0 && count > AllocationLimit / size) || allocation_fails() ?
		NULL : __real_calloc(count, size);
}

static const volatile void *template_a(void) { return &A; }
static const volatile void *template_b(void) { return &B; }
static const volatile void *template_value(void) { return &Value; }
static const volatile void *template_self(void) { return &Self; }
static const volatile void *template_null(void) { return NULL; }

static const PostgammaBackendStateSlot TargetsA[] = {S_B};
static const PostgammaBackendStateSlot TargetsB[] = {S_A};
static const PostgammaBackendStateSlot TargetsSelf[] = {S_SELF};
static unsigned BridgeCalls;

static void *
bridge_a(void)
{
	static const PostgammaBackendStateTemplate targets[] = {{S_B, &B}};

	BridgeCalls++;
	return postgamma_backend_state_address_relocated_cached(S_A, &A, targets, 1);
}

static void *
bridge_lazy(void)
{
	BridgeCalls++;
	return postgamma_backend_state_address_cached(S_LAZY, &Lazy);
}

static const PostgammaBackendStatePreparationEntry Entries[] = {
	{S_A, template_a, TargetsA, 1, false},
	{S_B, template_b, TargetsB, 1, false},
	{S_LARGE_ZERO, NULL, NULL, 0, true},
	{S_VALUE, template_value, NULL, 0, false},
	{S_SELF, template_self, TargetsSelf, 1, false},
	{S_VOLATILE, NULL, NULL, 0, true},
	{S_ZERO, NULL, NULL, 0, true},
};
const PostgammaBackendStatePreparation TestPreparation = {
	"prepared-fixture", POSTGAMMA_BACKEND_STATE_LAYOUT_ID,
	Entries, sizeof(Entries) / sizeof(Entries[0])
};

static PreparationFixtureData *
data(PostgammaRoleContext *role)
{
	return postgamma_backend_state_prepared_data(role->postgres_backend_state, &TestPreparation);
}

static void
check_view(PostgammaExecutionContext *execution, int value)
{
	PreparationFixtureData *state = data(execution->role);

	CHECK(postgamma_execution_context_current() == execution);
	CHECK(state != NULL && state->value == value && state->zero == NULL);
	CHECK(state->large_zero[0] == 0 && state->large_zero[sizeof(state->large_zero) - 1] == 0);
	CHECK(state->a == &state->b && state->b == &state->a);
	CHECK(state->self.next == &state->self && state->self.previous == &state->self);
	CHECK(&POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) == &state->value);
	CHECK(POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) == value);
#ifdef POSTGAMMA_DIRECT_ROLE_STATE
	CHECK(&POSTGAMMA_BACKEND_STATE_VALUE_IN((unsigned char *) state, S_VALUE, Value) == &state->value);
	CHECK(&POSTGAMMA_BACKEND_STATE_SCOPE_VALUE((unsigned char *) state,
		S_VALUE, Value) == &state->value);
	CHECK(&POSTGAMMA_BACKEND_STATE_SCOPE_VALUE(NULL,
		S_VALUE, Value) == &state->value);
	CHECK(POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE_IN(
		(unsigned char *) state, S_A, A, bridge_a) == &state->b);
#endif
	CHECK(POSTGAMMA_BACKEND_STATE_VALUE(S_A, A) == &state->b);
	{
		unsigned before = BridgeCalls;

		CHECK(POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE(S_A, A, bridge_a) == &state->b);
#ifdef POSTGAMMA_DIRECT_ROLE_STATE
		CHECK(BridgeCalls == before);
#else
		CHECK(BridgeCalls == before + 1);
#endif
	}
	CHECK(postgamma_backend_state_address_by_id("nonzero", &Value) == &state->value);
	CHECK(*(int *) postgamma_backend_state_address_by_id("nonzero", &Value) == value);
}

static void *
migrate(void *argument)
{
	PostgammaExecutionContext *execution = argument;
	PostgammaExecutionContext *previous = postgamma_execution_context_bind(execution);

	CHECK(previous == NULL);
	check_view(execution, 88);
	data(execution->role)->value = 99;
	postgamma_execution_context_restore(execution, previous);
	CHECK(postgamma_execution_context_current() == NULL);
#ifdef POSTGAMMA_DIRECT_ROLE_STATE
	CHECK(PostgammaCurrentPreparedRoleState == NULL);
#endif
	return NULL;
}

static void
jump_from_nested_execution(void *argument, int code)
{
	sigjmp_buf *target = argument;

	siglongjmp(*target, code);
}

/* A nonlocal exit unwinds the C stack, not the runtime's TLS binding. */
static void
check_nested_exit(PostgammaExecutionContext *outer,
				  PostgammaExecutionContext *nested)
{
	sigjmp_buf target;
	PreparationFixtureData *outer_data = data(outer->role);
	PreparationFixtureData *nested_data = data(nested->role);
	PostgammaExecutionContext *previous;
	int code;

	previous = postgamma_execution_context_bind(outer);
	CHECK(previous == NULL);
	postgamma_execution_context_set_exit_handler(
		nested, jump_from_nested_execution, &target);
	code = sigsetjmp(target, 1);
	if (code == 0)
	{
		CHECK(postgamma_execution_context_bind(nested) == outer);
		CHECK(&POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) ==
			  &nested_data->value);
		POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) = 123;
		(void) postgamma_execution_context_dispatch_exit(17);
		CHECK(false);
	}
	CHECK(code == 17);
	CHECK(postgamma_execution_context_current() == nested);
	CHECK(nested_data->value == 123 && outer_data->value == 88);
	/* The boundary must restore explicitly before the caller uses its view. */
	postgamma_execution_context_restore(nested, outer);
	CHECK(&POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) ==
		  &outer_data->value);
	CHECK(POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) == 88);
	postgamma_execution_context_set_exit_handler(nested, NULL, NULL);
	postgamma_execution_context_restore(outer, previous);
	CHECK(postgamma_execution_context_current() == NULL);
	CHECK(!postgamma_execution_context_dispatch_exit(17));
}

static void
reject_invalid_plans(PostgammaRoleContext *role)
{
	PostgammaBackendStatePreparation plan = TestPreparation;
	PostgammaBackendStatePreparationEntry entries[sizeof(Entries) / sizeof(Entries[0])];
	size_t i;

	plan.layout_identity = "wrong-layout";
	CHECK(postgamma_backend_state_context_prepare_plan(role->postgres_backend_state, &plan) == EINVAL);
	plan = TestPreparation;
	for (i = 0; i < TestPreparation.count; i++)
		entries[i] = Entries[i];
	plan.entries = entries;
	entries[3].slot = S_SESSION;
	CHECK(postgamma_backend_state_context_prepare_plan(role->postgres_backend_state, &plan) == EINVAL);
	entries[3] = Entries[3];
	entries[0].provider = template_null;
	CHECK(postgamma_backend_state_context_prepare_plan(role->postgres_backend_state, &plan) == EINVAL);
	entries[0] = Entries[0];
	plan.entries = entries + 1;
	plan.count = TestPreparation.count - 1; /* B's target A is absent. */
	CHECK(postgamma_backend_state_context_prepare_plan(role->postgres_backend_state, &plan) == EINVAL);
	CHECK(data(role) == NULL);
}

static void
expect_contract_failure(PostgammaExecutionContext *execution, bool inheritance)
{
	pid_t pid = fork();
	int status;

	CHECK(pid >= 0);
	if (pid == 0)
	{
		struct rlimit limit = {0, 0};

		(void) setrlimit(RLIMIT_CORE, &limit);
		if (inheritance)
		{
			void *source = NULL;
			const char *ids[] = {"nonzero"};

			CHECK(postgamma_backend_state_context_init(POSTGAMMA_BACKEND_STATE_OWNER_ROLE, &source) == 0);
			postgamma_backend_state_context_copy_ids(execution->role->postgres_backend_state, source, ids, 1);
		}
		else
			(void) postgamma_execution_context_bind(execution);
		_exit(0);
	}
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

#ifdef POSTGAMMA_DIRECT_ROLE_STATE
static void
reject_invalid_access(PostgammaExecutionContext *execution, int scenario)
{
	pid_t pid = fork();
	int status;

	CHECK(pid >= 0);
	if (pid == 0)
	{
		struct rlimit limit = {0, 0};
		volatile int slot = POSTGAMMA_BACKEND_STATE_SLOT_COUNT;
		void *volatile address = NULL;

		(void) setrlimit(RLIMIT_CORE, &limit);
		if (scenario == 0)
			address = &POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value);
		else if (scenario == 1)
			address = &POSTGAMMA_BACKEND_STATE_VALUE(-1, Value);
		else if (scenario == 2)
			address = &POSTGAMMA_BACKEND_STATE_VALUE(slot, Value);
		else if (scenario == 3)
		{
			(void) postgamma_execution_context_bind(execution);
			address = &POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, *(int *) NULL);
		}
		else if (scenario == 4)
		{
			PostgammaCurrentPreparedRoleState = (unsigned char *) 1;
			(void) postgamma_execution_context_bind(execution);
		}
		else if (scenario == 5)
		{
			(void) postgamma_execution_context_bind(execution);
			PostgammaCurrentPreparedRoleState = (unsigned char *) 1;
			postgamma_execution_context_restore(execution, NULL);
		}
		else if (scenario == 6)
			address = &POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE(S_A, A, bridge_a);
		else if (scenario == 7)
			address = &POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE(slot, A, bridge_a);
		else
			address = &POSTGAMMA_BACKEND_STATE_VALUE_IN(NULL, S_VALUE, Value);
		(void) address;
		_exit(0);
	}
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif

#ifdef POSTGAMMA_ROLE_CACHE_BYTES
static void *
reuse_roles(void *argument)
{
	PostgammaInstanceContext *instance = argument;
	for (int i = 0; i < 100; i++)
	{
		PostgammaRoleContext role;
		PostgammaExecutionContext execution;
		CHECK(postgamma_role_context_init(&role, instance) == 0);
		CHECK(postgamma_execution_context_init(&execution, instance, &role) == 0);
		CHECK(postgamma_backend_state_context_prepare(role.postgres_backend_state) == 0);
		CHECK(data(&role)->value == 42 && data(&role)->zero == NULL);
		CHECK(data(&role)->a == &data(&role)->b && data(&role)->b == &data(&role)->a);
		CHECK(data(&role)->self.next == &data(&role)->self);
		CHECK(data(&role)->large_zero[0] == 0);
		CHECK(data(&role)->large_zero[sizeof(data(&role)->large_zero) - 1] == 0);
		CHECK(postgamma_execution_context_bind(&execution) == NULL);
		POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) = i + 100;
		CHECK(*(int *) postgamma_backend_state_address_by_id("nonzero", &Value) == i + 100);
		data(&role)->large_zero[0] = 17;
		data(&role)->large_zero[sizeof(data(&role)->large_zero) - 1] = 19;
		postgamma_execution_context_restore(&execution, NULL);
		postgamma_execution_context_destroy(&execution);
		postgamma_role_context_destroy(&role);
	}
	return NULL;
}

static void
check_role_reuse(void)
{
	PostgammaInstanceContext instance;
	const size_t count = POSTGAMMA_ROLE_CACHE_BYTES / sizeof(PreparationFixtureData) + 4;
	PostgammaRoleContext *roles = calloc(count, sizeof(*roles));
	pthread_t workers[4];
	size_t bytes, leases;
	CHECK(roles != NULL && postgamma_instance_context_init(&instance) == 0);
	for (long failure = 0; failure < 2; failure++)
	{
		FailAllocation = failure;
		CHECK(postgamma_role_context_init(&roles[0], &instance) == ENOMEM);
		FailAllocation = -1;
		CHECK(roles[0].postgres_backend_state == NULL);
		postgamma_backend_state_role_cache_usage(instance.postgres_backend_state, &bytes, &leases);
		CHECK(bytes == 0 && leases == 0);
	}
	for (size_t i = 0; i < count; i++)
	{
		CHECK(postgamma_role_context_init(&roles[i], &instance) == 0);
		CHECK(postgamma_backend_state_context_prepare(roles[i].postgres_backend_state) == 0);
		data(&roles[i])->value = 999;
		data(&roles[i])->zero = &Value;
		data(&roles[i])->large_zero[0] = 1;
	}
	postgamma_backend_state_role_cache_usage(instance.postgres_backend_state, &bytes, &leases);
	CHECK(bytes == 0 && leases == count);
	for (size_t i = 0; i < count; i++)
		postgamma_role_context_destroy(&roles[i]);
	postgamma_backend_state_role_cache_usage(instance.postgres_backend_state, &bytes, &leases);
	CHECK(bytes > 0 && bytes <= POSTGAMMA_ROLE_CACHE_BYTES && leases == 0);
	for (size_t i = 0; i < 4; i++)
		CHECK(pthread_create(&workers[i], NULL, reuse_roles, &instance) == 0);
	for (size_t i = 0; i < 4; i++)
		CHECK(pthread_join(workers[i], NULL) == 0);
	postgamma_backend_state_role_cache_usage(instance.postgres_backend_state, &bytes, &leases);
	CHECK(bytes > 0 && bytes <= POSTGAMMA_ROLE_CACHE_BYTES && leases == 0);
	postgamma_instance_context_destroy(&instance);
	free(roles);
}
#endif

#ifdef POSTGAMMA_PRIVATE_MEMORY_BLOCKS
static void
check_role_pool_reuse(void)
{
	PostgammaInstanceContext instance;
	PostgammaMemoryPool pool;
	PostgammaRoleContext role;
	PreparationFixtureData *first;

	CHECK(postgamma_instance_context_init(&instance) == 0);
	CHECK(postgamma_memory_pool_init(&pool, 1024 * 1024) == 0);
	postgamma_memory_pool_acquire(&pool);
	for (long failure = 0; failure < 2; failure++)
	{
		FailAllocation = failure;
		CHECK(postgamma_role_context_init_in_pool(&role, &instance, &pool) == ENOMEM);
		FailAllocation = -1;
		CHECK(role.postgres_backend_state == NULL && pool.outstanding == 0);
	}
	CHECK(postgamma_role_context_init_in_pool(&role, &instance, &pool) == 0);
	CHECK(postgamma_backend_state_context_prepare(role.postgres_backend_state) == 0);
	first = data(&role);
	first->value = 991;
	first->pending = 23;
	first->large_zero[0] = 67;
	first->large_zero[sizeof(first->large_zero) - 1] = 68;
	postgamma_role_context_destroy(&role);
	CHECK(pool.outstanding == 0);
	CHECK(postgamma_role_context_init_in_pool(&role, &instance, &pool) == 0);
	CHECK(data(&role) == NULL);
	CHECK(postgamma_backend_state_context_prepare(role.postgres_backend_state) == 0);
#ifdef POSTGAMMA_MAPPED_MEMORY_BLOCKS
	CHECK(data(&role) == first);
#endif
	CHECK(data(&role)->value == 42 && data(&role)->pending == 0);
	CHECK(data(&role)->large_zero[0] == 0);
	CHECK(data(&role)->large_zero[sizeof(first->large_zero) - 1] == 0);
	CHECK(data(&role)->a == &data(&role)->b && data(&role)->b == &data(&role)->a);
	postgamma_role_context_destroy(&role);
	postgamma_memory_pool_release(&pool);
	CHECK(pool.bytes == 0 && pool.outstanding == 0);
	CHECK(postgamma_memory_pool_destroy(&pool) == 0);
	postgamma_instance_context_destroy(&instance);
}
#endif

int
main(void)
{
	PostgammaInstanceContext instance;
	PostgammaRoleContext parent, child;
	PostgammaExecutionContext first, second, inherited;
	PostgammaExecutionContext *previous;
	const char *ids[] = {"nonzero", "volatile"};
	pthread_t thread;
	int i;

	A = &B;
#ifdef POSTGAMMA_PRIVATE_MEMORY_BLOCKS
	check_role_pool_reuse();
#endif
	CHECK(postgamma_instance_context_init(&instance) == 0);
	CHECK(postgamma_role_context_init(&parent, &instance) == 0);
	CHECK(postgamma_role_context_init(&child, &instance) == 0);
	CHECK(postgamma_execution_context_init(&first, &instance, &parent) == 0);
	CHECK(postgamma_execution_context_init(&second, &instance, &parent) == 0);
	CHECK(postgamma_execution_context_init(&inherited, &instance, &child) == 0);
	expect_contract_failure(&first, false);
	reject_invalid_plans(&parent);
	for (i = 0; i < 2; i++)
	{
		FailAllocation = i;
		CHECK(postgamma_backend_state_context_prepare(parent.postgres_backend_state) == ENOMEM);
		FailAllocation = -1;
		CHECK(data(&parent) == NULL);
		CHECK(first.backend_state_addresses[S_VALUE] == NULL);
	}
	/* Large already-zero fields must not require a second full-size image. */
	AllocationLimit = 1024;
	CHECK(postgamma_backend_state_context_prepare(parent.postgres_backend_state) == 0);
	AllocationLimit = SIZE_MAX;
	CHECK(data(&parent)->value == 42);
	CHECK(postgamma_execution_context_bind(&first) == NULL);
	POSTGAMMA_BACKEND_STATE_VALUE(S_VALUE, Value) = 77;
	CHECK(POSTGAMMA_BACKEND_STATE_VALUE(S_VOLATILE, Pending) == 0);
	POSTGAMMA_BACKEND_STATE_VALUE(S_VOLATILE, Pending) = 17;
	CHECK(*(volatile int *) postgamma_backend_state_address_by_id("volatile", &Pending) == 17);
	CHECK(Pending == 0);
#ifdef POSTGAMMA_DIRECT_ROLE_STATE
	CHECK(first.backend_state_addresses[S_VALUE] == NULL);
#endif
	postgamma_execution_context_restore(&first, NULL);
	/* Direct access must publish initialized to inheritance. */
	postgamma_backend_state_context_copy_ids(child.postgres_backend_state, parent.postgres_backend_state, ids, 2);
	CHECK(postgamma_backend_state_context_prepare(child.postgres_backend_state) == 0);
	CHECK(data(&child)->value == 77);
	CHECK(data(&child)->pending == 17);
	data(&child)->pending = 29;
	CHECK(data(&parent)->pending == 17);
	CHECK(data(&child)->a != data(&parent)->a);
#ifdef POSTGAMMA_DIRECT_ROLE_STATE
	for (i = 0; i < 9; i++)
		reject_invalid_access(&first, i);
#endif
	previous = postgamma_execution_context_bind(&first);
	CHECK(previous == NULL);
	check_view(&first, 77);
	CHECK(POSTGAMMA_BACKEND_STATE_VALUE(S_SESSION, Session) == NULL);
	POSTGAMMA_BACKEND_STATE_VALUE(S_SESSION, Session) = &Value;
	*(int *) postgamma_backend_state_address_by_id("nonzero", &Value) = 88;
	CHECK(data(&parent)->value == 88);
	CHECK(postgamma_backend_state_context_prepare(parent.postgres_backend_state) == 0);
	CHECK(data(&parent)->value == 88);
	Lazy = 23; /* Unqualified templates retain first-access semantics. */
	CHECK(POSTGAMMA_BACKEND_STATE_VALUE(S_LAZY, Lazy) == 23);
	CHECK(POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE(S_LAZY, Lazy, bridge_lazy) == 23);
	{
		volatile int slot = S_A;
		unsigned before = BridgeCalls;

		CHECK(POSTGAMMA_BACKEND_STATE_RELOCATED_SLOT_VALUE(slot++, A, bridge_a) == &data(&parent)->b);
		CHECK(slot == S_A + 1 && BridgeCalls == before + 1);
	}
	{
		volatile int slot = S_VALUE;

		CHECK(POSTGAMMA_BACKEND_STATE_VALUE(slot++, Value) == 88);
		CHECK(slot == S_VALUE + 1);
	}
	CHECK(postgamma_execution_context_bind(&inherited) == &first);
	check_view(&inherited, 77);
	CHECK(POSTGAMMA_BACKEND_STATE_VALUE(S_SESSION, Session) == NULL);
	postgamma_execution_context_restore(&inherited, &first);
	check_view(&first, 88);
	CHECK(POSTGAMMA_BACKEND_STATE_VALUE(S_SESSION, Session) == &Value);
	postgamma_execution_context_restore(&first, NULL);
	CHECK(postgamma_execution_context_bind(&second) == NULL);
	check_view(&second, 88);
	CHECK(POSTGAMMA_BACKEND_STATE_VALUE(S_SESSION, Session) == NULL);
	postgamma_execution_context_restore(&second, NULL);
	check_nested_exit(&first, &inherited);
	CHECK(pthread_create(&thread, NULL, migrate, &second) == 0);
	CHECK(pthread_join(thread, NULL) == 0);
	CHECK(data(&parent)->value == 99);
	expect_contract_failure(&first, true);
	postgamma_execution_context_destroy(&inherited);
	postgamma_execution_context_destroy(&second);
	postgamma_execution_context_destroy(&first);
	postgamma_role_context_destroy(&child);
	postgamma_role_context_destroy(&parent);
	postgamma_instance_context_destroy(&instance);
#ifdef POSTGAMMA_ROLE_CACHE_BYTES
	check_role_reuse();
#endif
	return 0;
}
