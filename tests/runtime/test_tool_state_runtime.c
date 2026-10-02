#include "postgamma/tool_state_runtime.h"

#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>


static jmp_buf FailureJump;
static int FailureStatus;
static volatile int FailureCalls;


static void
relocation_failure(void *argument, int status)
{
	(void) argument;
	FailureCalls++;
	FailureStatus = status;
	longjmp(FailureJump, 1);
}


static void
store_word(unsigned char *bytes, size_t offset, uintptr_t value)
{
	memcpy(bytes + offset, &value, sizeof(value));
}


static uintptr_t
load_word(const unsigned char *bytes, size_t offset)
{
	uintptr_t value;

	memcpy(&value, bytes + offset, sizeof(value));
	return value;
}


static void
release_state(void **state)
{
	assert(postgamma_tool_state_unbind(*state) == 0);
	postgamma_tool_state_destroy(state);
	assert(*state == NULL);
}


static void
test_exact_and_interior_relocation(void)
{
	_Alignas(uintptr_t) unsigned char target[32] = {0};
	_Alignas(uintptr_t) unsigned char owner[48] = {0};
	PostgammaToolStateTemplate targets[] = {
		{POSTGAMMA_TOOL_STATE_SLOT_TARGET, target, 2},
	};
	unsigned char *relocated_owner;
	unsigned char *relocated_target;
	void *state = NULL;

	FailureCalls = 0;
	store_word(owner, 0, (uintptr_t) target);
	store_word(owner, 8, (uintptr_t) (target + 7));
	assert(postgamma_tool_state_create(&state) == 0);
	assert(postgamma_tool_state_bind(
		state, relocation_failure, NULL) == 0);
	relocated_owner = postgamma_tool_state_address_relocated(
		POSTGAMMA_TOOL_STATE_SLOT_OWNER, owner, targets, 1);
	relocated_target = postgamma_tool_state_address(
		POSTGAMMA_TOOL_STATE_SLOT_TARGET, target);
	assert(FailureCalls == 0);
	assert(load_word(relocated_owner, 0) == (uintptr_t) relocated_target);
	assert(load_word(relocated_owner, 8) ==
		(uintptr_t) (relocated_target + 7));
	release_state(&state);
}


static void
expect_relocation_failure(
	unsigned char *target, unsigned char *owner, size_t expected_matches)
{
	PostgammaToolStateTemplate targets[] = {
		{POSTGAMMA_TOOL_STATE_SLOT_TARGET, target, expected_matches},
	};
	void *state = NULL;

	FailureCalls = 0;
	FailureStatus = 0;
	assert(postgamma_tool_state_create(&state) == 0);
	assert(postgamma_tool_state_bind(
		state, relocation_failure, NULL) == 0);
	if (setjmp(FailureJump) == 0)
	{
		(void) postgamma_tool_state_address_relocated(
			POSTGAMMA_TOOL_STATE_SLOT_OWNER, owner, targets, 1);
		assert(!"relocation contract failure was not raised");
	}
	assert(FailureCalls == 1);
	assert(FailureStatus == EPROTO);
	release_state(&state);
}


static void
test_pointer_like_integer_is_rejected(void)
{
	_Alignas(uintptr_t) unsigned char target[32] = {0};
	_Alignas(uintptr_t) unsigned char owner[48] = {0};

	store_word(owner, 0, (uintptr_t) target);
	store_word(owner, 8, (uintptr_t) (target + 7));
	store_word(owner, 16, (uintptr_t) (target + 11));
	expect_relocation_failure(target, owner, 2);
}


static void
test_unaligned_pointer_is_rejected(void)
{
	_Alignas(uintptr_t) unsigned char target[32] = {0};
	_Alignas(uintptr_t) unsigned char owner[48] = {0};
	uintptr_t unaligned = (uintptr_t) (target + 11);

	store_word(owner, 0, (uintptr_t) target);
	store_word(owner, 8, (uintptr_t) (target + 7));
	memcpy(owner + 17, &unaligned, sizeof(unaligned));
	expect_relocation_failure(target, owner, 3);
}


static void
test_one_past_pointer_is_rejected(void)
{
	_Alignas(uintptr_t) unsigned char target[32] = {0};
	_Alignas(uintptr_t) unsigned char owner[48] = {0};

	store_word(owner, 0, (uintptr_t) target);
	store_word(owner, 8, (uintptr_t) (target + 7));
	store_word(owner, 16, (uintptr_t) (target + sizeof(target)));
	expect_relocation_failure(target, owner, 3);
}


int
main(void)
{
	test_exact_and_interior_relocation();
	test_pointer_like_integer_is_rejected();
	test_unaligned_pointer_is_rejected();
	test_one_past_pointer_is_rejected();
	return 0;
}
