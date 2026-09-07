#define _XOPEN_SOURCE 700

#include "postgamma/guc_runtime.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>


typedef struct FakeTransactionState
{
	int		marker;
	unsigned char padding[116];
} FakeTransactionState;


typedef struct FakeList
{
	struct FakeList *next;
	struct FakeList *previous;
} FakeList;


static int backend_pid_template;
static int instance_mode_template = 077;
static FakeTransactionState top_transaction_template = {.marker = 41};
static FakeTransactionState *current_transaction_template =
	&top_transaction_template;
static FakeList self_referencing_template = {
	&self_referencing_template,
	&self_referencing_template,
};
_Alignas(4096) static unsigned char aligned_block_template[8192];
static jmp_buf exit_jump;
static int exit_code_seen;
_Alignas(64) static uint64_t extension_state_template = UINT64_C(17);


static void
capture_execution_exit(void *argument, int code)
{
	int	   *expected = argument;

	assert(code == *expected);
	exit_code_seen = code;
	longjmp(exit_jump, 1);
}


typedef struct Worker
{
	PostgammaInstanceContext instance;
	PostgammaRoleContext role;
	PostgammaExecutionContext context;
	pthread_barrier_t *barrier;
	int work_mem;
	bool enable_hashjoin;
	int backend_value;
} Worker;


typedef struct InstanceStateWorker
{
	PostgammaInstanceContext *instance;
	pthread_barrier_t *barrier;
	int		   *address;
	int		   *fast_path_address;
	int			observed;
} InstanceStateWorker;


static void *
run_instance_state_worker(void *argument)
{
	InstanceStateWorker *worker = argument;
	PostgammaRoleContext role;
	PostgammaExecutionContext execution;
	PostgammaExecutionContext *previous;
	int barrier_result;

	assert(postgamma_role_context_init(&role, worker->instance) == 0);
	assert(postgamma_execution_context_init(
		&execution, worker->instance, &role) == 0);
	previous = postgamma_execution_context_bind(&execution);
	barrier_result = pthread_barrier_wait(worker->barrier);
	assert(barrier_result == 0 || barrier_result == PTHREAD_BARRIER_SERIAL_THREAD);
	worker->address = postgamma_backend_state_address_by_id(
		"external:pg_mode_mask", &instance_mode_template);
	worker->observed = *worker->address;
	worker->fast_path_address = postgamma_backend_state_address_by_id(
		"external:pg_mode_mask", &instance_mode_template);
	postgamma_execution_context_restore(&execution, previous);
	postgamma_execution_context_destroy(&execution);
	postgamma_role_context_destroy(&role);
	return NULL;
}


static void *
run_worker(void *argument)
{
	Worker *worker = argument;
	PostgammaExecutionContext *previous;
	int barrier_result;
	int i;

	assert(postgamma_instance_context_init(&worker->instance) == 0);
	assert(postgamma_role_context_init(
		&worker->role, &worker->instance) == 0);
	assert(postgamma_execution_context_init(
		&worker->context, &worker->instance, &worker->role) == 0);
	previous = postgamma_execution_context_bind(&worker->context);
	assert(previous == NULL);

	POSTGAMMA_GUC_VALUE(work_mem) = worker->work_mem;
	POSTGAMMA_GUC_VALUE(enable_hashjoin) = worker->enable_hashjoin;
	POSTGAMMA_BACKEND_STATE_VALUE(
		POSTGAMMA_BACKEND_STATE_SLOT_8D69A00A31D3AF16,
		backend_pid_template) = worker->backend_value;
	assert((uintptr_t) postgamma_backend_state_address(
			   POSTGAMMA_BACKEND_STATE_SLOT_50CFB784F38076EA,
			   aligned_block_template) % 4096 == 0);
	{
		FakeTransactionState *top =
			&POSTGAMMA_BACKEND_STATE_VALUE(
				POSTGAMMA_BACKEND_STATE_SLOT_BF87053C70C878C4,
				top_transaction_template);
		PostgammaBackendStateTemplate targets[] = {
			{POSTGAMMA_BACKEND_STATE_SLOT_BF87053C70C878C4,
			 &top_transaction_template},
		};
		FakeTransactionState **current = postgamma_backend_state_address_relocated(
			POSTGAMMA_BACKEND_STATE_SLOT_99EE0B8E2B9438FC,
			&current_transaction_template, targets, 1);

		assert(top->marker == 41);
		assert(*current == top);
	}
	{
		PostgammaBackendStateTemplate targets[] = {
			{POSTGAMMA_BACKEND_STATE_SLOT_3E5E677E95EECC0B,
			 &self_referencing_template},
		};
		FakeList *list = postgamma_backend_state_address_relocated(
			POSTGAMMA_BACKEND_STATE_SLOT_3E5E677E95EECC0B,
			&self_referencing_template, targets, 1);

		assert(list->next == list);
		assert(list->previous == list);
	}
	barrier_result = pthread_barrier_wait(worker->barrier);
	assert(barrier_result == 0 || barrier_result == PTHREAD_BARRIER_SERIAL_THREAD);

	for (i = 0; i < 100000; i++)
	{
		assert(POSTGAMMA_GUC_VALUE(work_mem) == worker->work_mem);
		assert(POSTGAMMA_GUC_VALUE(enable_hashjoin) == worker->enable_hashjoin);
		assert(POSTGAMMA_BACKEND_STATE_VALUE(
				   POSTGAMMA_BACKEND_STATE_SLOT_8D69A00A31D3AF16,
				   backend_pid_template) == worker->backend_value);
	}

	postgamma_execution_context_restore(&worker->context, previous);
	assert(postgamma_execution_context_current() == NULL);
	postgamma_execution_context_destroy(&worker->context);
	postgamma_role_context_destroy(&worker->role);
	postgamma_instance_context_destroy(&worker->instance);
	return NULL;
}


static void
test_rebind_on_one_thread(void)
{
	PostgammaInstanceContext instance;
	PostgammaRoleContext first_role;
	PostgammaRoleContext second_role;
	PostgammaRoleContext inherited_role;
	PostgammaExecutionContext first;
	PostgammaExecutionContext second;
	PostgammaExecutionContext same_role;
	PostgammaExecutionContext inherited;
	PostgammaExecutionContext *previous;
	const char *inherited_ids[] = {"external:MyProcPid"};

	assert(postgamma_instance_context_init(&instance) == 0);
	assert(postgamma_role_context_init(&first_role, &instance) == 0);
	assert(postgamma_role_context_init(&second_role, &instance) == 0);
	assert(postgamma_role_context_init(&inherited_role, &instance) == 0);
	assert(postgamma_execution_context_init(
		&first, &instance, &first_role) == 0);
	assert(postgamma_execution_context_init(
		&same_role, &instance, &first_role) == 0);
	assert(postgamma_execution_context_init(
		&second, &instance, &second_role) == 0);
	assert(postgamma_execution_context_init(
		&inherited, &instance, &inherited_role) == 0);

	previous = postgamma_execution_context_bind(&first);
	assert(previous == NULL);
	POSTGAMMA_GUC_VALUE(work_mem) = 8192;
	POSTGAMMA_GUC_VALUE(enable_hashjoin) = false;
	POSTGAMMA_GUC_VALUE(in_hot_standby_guc) = true;
	POSTGAMMA_GUC_VALUE(MaxConnections) = 100;
	POSTGAMMA_GUC_VALUE(block_size) = 8192;
	POSTGAMMA_BACKEND_STATE_VALUE(
		POSTGAMMA_BACKEND_STATE_SLOT_8D69A00A31D3AF16,
		backend_pid_template) = 11;
	postgamma_execution_context_restore(&first, previous);

	previous = postgamma_execution_context_bind(&second);
	assert(POSTGAMMA_GUC_VALUE(work_mem) == 0);
	assert(!POSTGAMMA_GUC_VALUE(enable_hashjoin));
	assert(!POSTGAMMA_GUC_VALUE(in_hot_standby_guc));
	assert(POSTGAMMA_GUC_VALUE(MaxConnections) == 100);
	assert(POSTGAMMA_GUC_VALUE(block_size) == 8192);
	assert(&POSTGAMMA_GUC_VALUE(work_mem) != &first.session_guc.work_mem);
	assert(POSTGAMMA_BACKEND_STATE_VALUE(
			   POSTGAMMA_BACKEND_STATE_SLOT_8D69A00A31D3AF16,
			   backend_pid_template) == 0);
	POSTGAMMA_BACKEND_STATE_VALUE(
		POSTGAMMA_BACKEND_STATE_SLOT_8D69A00A31D3AF16,
		backend_pid_template) = 22;
	POSTGAMMA_GUC_VALUE(work_mem) = 16384;
	postgamma_execution_context_restore(&second, previous);

	previous = postgamma_execution_context_bind(&same_role);
	assert(POSTGAMMA_GUC_VALUE(in_hot_standby_guc));
	assert(POSTGAMMA_GUC_VALUE(work_mem) == 0);
	assert(POSTGAMMA_BACKEND_STATE_VALUE(
			   POSTGAMMA_BACKEND_STATE_SLOT_8D69A00A31D3AF16,
			   backend_pid_template) == 11);
	postgamma_execution_context_restore(&same_role, previous);

	previous = postgamma_execution_context_bind(&first);
	assert(POSTGAMMA_GUC_VALUE(work_mem) == 8192);
	assert(!POSTGAMMA_GUC_VALUE(enable_hashjoin));
	assert(POSTGAMMA_BACKEND_STATE_VALUE(
			   POSTGAMMA_BACKEND_STATE_SLOT_8D69A00A31D3AF16,
			   backend_pid_template) == 11);
	postgamma_execution_context_restore(&first, previous);
	postgamma_backend_state_context_copy_ids(
		inherited_role.postgres_backend_state,
		first_role.postgres_backend_state,
		inherited_ids,
		1);
	previous = postgamma_execution_context_bind(&inherited);
	assert(POSTGAMMA_BACKEND_STATE_VALUE(
			   POSTGAMMA_BACKEND_STATE_SLOT_8D69A00A31D3AF16,
			   backend_pid_template) == 11);
	postgamma_execution_context_restore(&inherited, previous);

	assert(second.session_guc.work_mem == 16384);
	postgamma_execution_context_destroy(&inherited);
	postgamma_execution_context_destroy(&second);
	postgamma_execution_context_destroy(&same_role);
	postgamma_execution_context_destroy(&first);
	postgamma_role_context_destroy(&second_role);
	postgamma_role_context_destroy(&inherited_role);
	postgamma_role_context_destroy(&first_role);
	postgamma_instance_context_destroy(&instance);
}


static void
test_extension_role_state(void)
{
	PostgammaInstanceContext instance;
	PostgammaRoleContext first_role;
	PostgammaRoleContext second_role;
	PostgammaExecutionContext first;
	PostgammaExecutionContext same_role;
	PostgammaExecutionContext second;
	PostgammaExecutionContext *previous;
	uint64_t   *value;
	uint64_t   *alias;

	errno = 0;
	assert(postgamma_extension_role_state_address(
		"probe:value", &extension_state_template,
		sizeof(extension_state_template),
		_Alignof(__typeof__(extension_state_template))) == NULL);
	assert(errno == ENODEV);
	assert(postgamma_instance_context_init(&instance) == 0);
	assert(postgamma_role_context_init(&first_role, &instance) == 0);
	assert(postgamma_role_context_init(&second_role, &instance) == 0);
	assert(postgamma_execution_context_init(
		&first, &instance, &first_role) == 0);
	assert(postgamma_execution_context_init(
		&same_role, &instance, &first_role) == 0);
	assert(postgamma_execution_context_init(
		&second, &instance, &second_role) == 0);

	previous = postgamma_execution_context_bind(&first);
	value = postgamma_extension_role_state_address(
		"probe:value", &extension_state_template,
		sizeof(extension_state_template), 64);
	assert(value != NULL && (uintptr_t) value % 64 == 0 && *value == 17);
	*value = 41;
	alias = postgamma_extension_role_state_address(
		"probe:value", &extension_state_template,
		sizeof(extension_state_template), 64);
	assert(alias == value && *alias == 41);
	errno = 0;
	assert(postgamma_extension_role_state_address(
		"probe:value", &extension_state_template,
		sizeof(extension_state_template) - 1, 64) == NULL);
	assert(errno == EINVAL);
	postgamma_execution_context_restore(&first, previous);

	previous = postgamma_execution_context_bind(&same_role);
	value = postgamma_extension_role_state_address(
		"probe:value", &extension_state_template,
		sizeof(extension_state_template), 64);
	assert(*value == 41);
	postgamma_execution_context_restore(&same_role, previous);

	previous = postgamma_execution_context_bind(&second);
	value = postgamma_extension_role_state_address(
		"probe:value", &extension_state_template,
		sizeof(extension_state_template), 64);
	assert(*value == 17);
	postgamma_execution_context_restore(&second, previous);

	postgamma_execution_context_destroy(&second);
	postgamma_execution_context_destroy(&same_role);
	postgamma_execution_context_destroy(&first);
	postgamma_role_context_destroy(&second_role);
	postgamma_role_context_destroy(&first_role);
	postgamma_instance_context_destroy(&instance);
}


static void
test_execution_exit_boundary(void)
{
	PostgammaInstanceContext instance;
	PostgammaRoleContext role;
	PostgammaExecutionContext context;
	PostgammaExecutionContext *previous;
	int		expected = 27;

	assert(postgamma_instance_context_init(&instance) == 0);
	assert(postgamma_role_context_init(&role, &instance) == 0);
	assert(postgamma_execution_context_init(&context, &instance, &role) == 0);
	previous = postgamma_execution_context_bind(&context);
	assert(previous == NULL);
	assert(!postgamma_execution_context_dispatch_exit(1));
	postgamma_execution_context_set_exit_handler(
		&context, capture_execution_exit, &expected);
	if (setjmp(exit_jump) == 0)
	{
		(void) postgamma_execution_context_dispatch_exit(expected);
		assert(false);
	}
	assert(exit_code_seen == expected);
	postgamma_execution_context_set_exit_handler(&context, NULL, NULL);
	postgamma_execution_context_restore(&context, previous);
	postgamma_execution_context_destroy(&context);
	postgamma_role_context_destroy(&role);
	postgamma_instance_context_destroy(&instance);
}


static void
test_instance_runtime_attachment(void)
{
	PostgammaInstanceContext instance;
	unsigned char runtime_owner;
	struct PostgammaInstanceRuntime *runtime =
		(struct PostgammaInstanceRuntime *) &runtime_owner;

	assert(postgamma_instance_context_init(&instance) == 0);
	assert(postgamma_instance_context_runtime(&instance) == NULL);
	postgamma_instance_context_attach_runtime(&instance, runtime);
	assert(postgamma_instance_context_runtime(&instance) == runtime);
	postgamma_instance_context_detach_runtime(&instance, runtime);
	assert(postgamma_instance_context_runtime(&instance) == NULL);
	postgamma_instance_context_destroy(&instance);
}


static void
test_concurrent_instance_backend_state_initialization(void)
{
	for (int iteration = 0; iteration < 64; iteration++)
	{
		PostgammaInstanceContext owner;
		PostgammaInstanceContext borrower;
		InstanceStateWorker workers[2];
		pthread_barrier_t initialization_barrier;
		pthread_t threads[2];
		int index;

		assert(postgamma_instance_context_init(&owner) == 0);
		postgamma_instance_context_init_shared(&borrower, &owner);
		assert(pthread_barrier_init(&initialization_barrier, NULL, 2) == 0);
		workers[0] = (InstanceStateWorker) {
			.instance = &owner,
			.barrier = &initialization_barrier,
		};
		workers[1] = (InstanceStateWorker) {
			.instance = &borrower,
			.barrier = &initialization_barrier,
		};
		for (index = 0; index < 2; index++)
			assert(pthread_create(
				&threads[index], NULL, run_instance_state_worker,
				&workers[index]) == 0);
		for (index = 0; index < 2; index++)
			assert(pthread_join(threads[index], NULL) == 0);
		assert(workers[0].address == workers[1].address);
		assert(workers[0].fast_path_address == workers[0].address);
		assert(workers[1].fast_path_address == workers[1].address);
		assert(workers[0].observed == 077);
		assert(workers[1].observed == 077);
		assert(pthread_barrier_destroy(&initialization_barrier) == 0);
		postgamma_instance_context_destroy(&borrower);
		postgamma_instance_context_destroy(&owner);
	}
}


static void
test_instance_backend_state_sharing(void)
{
	PostgammaInstanceContext owner;
	PostgammaInstanceContext borrower;
	PostgammaInstanceContext independent;
	PostgammaRoleContext owner_role;
	PostgammaRoleContext borrower_role;
	PostgammaRoleContext independent_role;
	PostgammaExecutionContext owner_execution;
	PostgammaExecutionContext borrower_execution;
	PostgammaExecutionContext independent_execution;
	PostgammaExecutionContext *previous;
	int *value;

	assert(postgamma_instance_context_init(&owner) == 0);
	postgamma_instance_context_init_shared(&borrower, &owner);
	assert(postgamma_instance_context_init(&independent) == 0);
	assert(postgamma_role_context_init(&owner_role, &owner) == 0);
	assert(postgamma_role_context_init(&borrower_role, &borrower) == 0);
	assert(postgamma_role_context_init(
		&independent_role, &independent) == 0);
	assert(postgamma_execution_context_init(
		&owner_execution, &owner, &owner_role) == 0);
	assert(postgamma_execution_context_init(
		&borrower_execution, &borrower, &borrower_role) == 0);
	assert(postgamma_execution_context_init(
		&independent_execution, &independent, &independent_role) == 0);

	previous = postgamma_execution_context_bind(&owner_execution);
	value = postgamma_backend_state_address_by_id(
		"external:pg_mode_mask", &instance_mode_template);
	assert(*value == 077);
	*value = 0123;
	postgamma_execution_context_restore(&owner_execution, previous);

	previous = postgamma_execution_context_bind(&borrower_execution);
	value = postgamma_backend_state_address_by_id(
		"external:pg_mode_mask", &instance_mode_template);
	assert(*value == 0123);
	postgamma_execution_context_restore(&borrower_execution, previous);

	previous = postgamma_execution_context_bind(&independent_execution);
	value = postgamma_backend_state_address_by_id(
		"external:pg_mode_mask", &instance_mode_template);
	assert(*value == 077);
	*value = 0456;
	postgamma_execution_context_restore(&independent_execution, previous);

	previous = postgamma_execution_context_bind(&owner_execution);
	value = postgamma_backend_state_address_by_id(
		"external:pg_mode_mask", &instance_mode_template);
	assert(*value == 0123);
	postgamma_execution_context_restore(&owner_execution, previous);

	postgamma_execution_context_destroy(&independent_execution);
	postgamma_execution_context_destroy(&borrower_execution);
	postgamma_execution_context_destroy(&owner_execution);
	postgamma_role_context_destroy(&independent_role);
	postgamma_role_context_destroy(&borrower_role);
	postgamma_role_context_destroy(&owner_role);
	postgamma_instance_context_destroy(&independent);
	postgamma_instance_context_destroy(&borrower);
	postgamma_instance_context_destroy(&owner);
}


int
main(void)
{
	pthread_barrier_t barrier;
	pthread_t threads[2];
	Worker workers[2] = {
		{.work_mem = 2048, .enable_hashjoin = false, .backend_value = 101},
		{.work_mem = 65536, .enable_hashjoin = true, .backend_value = 202},
	};
	int i;

	test_rebind_on_one_thread();
	test_extension_role_state();
	test_execution_exit_boundary();
	test_instance_runtime_attachment();
	test_concurrent_instance_backend_state_initialization();
	test_instance_backend_state_sharing();
	assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
	for (i = 0; i < 2; i++)
	{
		workers[i].barrier = &barrier;
		assert(pthread_create(&threads[i], NULL, run_worker, &workers[i]) == 0);
	}
	for (i = 0; i < 2; i++)
		assert(pthread_join(threads[i], NULL) == 0);
	assert(pthread_barrier_destroy(&barrier) == 0);

	assert(workers[0].context.magic == 0);
	assert(workers[1].context.magic == 0);
	return 0;
}
