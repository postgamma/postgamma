#include "postgamma/guc_runtime.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>


typedef enum AllocationKind
{
	ALLOCATION_NONE = 0,
	ALLOCATION_CALLOC,
	ALLOCATION_MALLOC
} AllocationKind;


static AllocationKind FailureKind;
static size_t FailureIndex;
static size_t CallocCalls;
static size_t MallocCalls;


void *__real_calloc(size_t count, size_t size);
void *__real_malloc(size_t size);


void *
__wrap_calloc(size_t count, size_t size)
{
	CallocCalls++;
	if (FailureKind == ALLOCATION_CALLOC && CallocCalls == FailureIndex)
		return NULL;
	return __real_calloc(count, size);
}


void *
__wrap_malloc(size_t size)
{
	MallocCalls++;
	if (FailureKind == ALLOCATION_MALLOC && MallocCalls == FailureIndex)
		return NULL;
	return __real_malloc(size);
}


static void
arm_failure(AllocationKind kind, size_t index)
{
	FailureKind = kind;
	FailureIndex = index;
	CallocCalls = 0;
	MallocCalls = 0;
}


static void
disarm_failure(void)
{
	FailureKind = ALLOCATION_NONE;
	FailureIndex = 0;
}


static bool
object_is_zero(const void *object, size_t size)
{
	const unsigned char *bytes = object;

	for (size_t index = 0; index < size; index++)
	{
		if (bytes[index] != 0)
			return false;
	}
	return true;
}


static void
test_instance_allocation_failures(void)
{
	PostgammaInstanceContext context;

	for (size_t index = 1; index <= 3; index++)
	{
		memset(&context, 0xa5, sizeof(context));
		arm_failure(ALLOCATION_CALLOC, index);
		assert(postgamma_instance_context_init(&context) == ENOMEM);
		disarm_failure();
		assert(object_is_zero(&context, sizeof(context)));
	}
	memset(&context, 0xa5, sizeof(context));
	arm_failure(ALLOCATION_MALLOC, 1);
	assert(postgamma_instance_context_init(&context) == ENOMEM);
	disarm_failure();
	assert(object_is_zero(&context, sizeof(context)));
	assert(postgamma_instance_context_init(&context) == 0);
	postgamma_instance_context_destroy(&context);
}


static void
test_role_and_execution_allocation_failures(void)
{
	PostgammaInstanceContext instance;
	PostgammaRoleContext role;
	PostgammaExecutionContext execution;

	assert(postgamma_instance_context_init(&instance) == 0);
	for (size_t index = 1; index <= 3; index++)
	{
		memset(&role, 0xa5, sizeof(role));
		arm_failure(ALLOCATION_CALLOC, index);
		assert(postgamma_role_context_init(&role, &instance) == ENOMEM);
		disarm_failure();
		assert(object_is_zero(&role, sizeof(role)));
	}
	assert(postgamma_role_context_init(&role, &instance) == 0);
	for (size_t index = 1; index <= 3; index++)
	{
		memset(&execution, 0xa5, sizeof(execution));
		arm_failure(ALLOCATION_CALLOC, index);
		assert(postgamma_execution_context_init(
			&execution, &instance, &role) == ENOMEM);
		disarm_failure();
		assert(object_is_zero(&execution, sizeof(execution)));
	}
	assert(postgamma_execution_context_init(
		&execution, &instance, &role) == 0);
	postgamma_execution_context_destroy(&execution);
	postgamma_role_context_destroy(&role);
	postgamma_instance_context_destroy(&instance);
}


static void
test_shared_instance_initialization_does_not_allocate(void)
{
	PostgammaInstanceContext owner;
	PostgammaInstanceContext calloc_borrower;
	PostgammaInstanceContext malloc_borrower;

	assert(postgamma_instance_context_init(&owner) == 0);
	arm_failure(ALLOCATION_CALLOC, 1);
	postgamma_instance_context_init_shared(&calloc_borrower, &owner);
	assert(CallocCalls == 0);
	disarm_failure();
	arm_failure(ALLOCATION_MALLOC, 1);
	postgamma_instance_context_init_shared(&malloc_borrower, &owner);
	assert(MallocCalls == 0);
	disarm_failure();
	assert(calloc_borrower.postgres_backend_state ==
		owner.postgres_backend_state);
	assert(malloc_borrower.postgres_backend_state ==
		owner.postgres_backend_state);
	postgamma_instance_context_destroy(&malloc_borrower);
	postgamma_instance_context_destroy(&calloc_borrower);
	postgamma_instance_context_destroy(&owner);
}


int
main(void)
{
	test_instance_allocation_failures();
	test_role_and_execution_allocation_failures();
	test_shared_instance_initialization_does_not_allocate();
	return 0;
}
