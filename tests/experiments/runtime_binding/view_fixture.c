/* Copyright 2026 Shujie Zhang. SPDX-License-Identifier: Apache-2.0 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime preparation tests separately validate these real accessors. */
_Thread_local unsigned char *PostgammaCurrentPreparedRoleState;
static inline int *checked_view(unsigned char *view);
#define POSTGAMMA_BACKEND_STATE_VALUE(slot, original) \
	(*checked_view(PostgammaCurrentPreparedRoleState))
#define POSTGAMMA_BACKEND_STATE_VALUE_IN(view, slot, original) (*checked_view(view))
#define POSTGAMMA_BACKEND_STATE_SCOPE_VALUE(view, slot, original) \
	POSTGAMMA_BACKEND_STATE_VALUE_IN( \
		(view) != NULL ? (view) : PostgammaCurrentPreparedRoleState, slot, original)

static int outer = 11;
static int nested = 22;
static jmp_buf exit_boundary;
static int calls;

#include "view_fixture_inline.h"

static inline int *
checked_view(unsigned char *view)
{
	if (view == NULL)
		abort();
	return (int *) view;
}

static inline int inline_main_read(void);

static int
inline_callback(void (*callback)(void))
{
	callback();
	return inline_main_read();
}

static inline int
inline_main_read(void)
{
	return inline_state_read();
}

static int *
inline_identity(int enabled)
{
	return inline_static_identity(enabled);
}

static int *
inline_address(int enabled)
{
	return inline_state_address(enabled);
}

static int
inline_direct_and_indirect(void)
{
	int (*entry)(void) = inline_state_read;
	int (*line_entry)(void) = inline_line_identity;

	if (inline_line_identity() != line_entry())
		abort();
	return inline_state_read() + entry();
}

static int
inline_annotated(int enabled)
{
	return inline_annotated_read(&enabled);
}

#include "view_fixture_late.h"

static int
read_late_header(void)
{
	return late_state_read().value;
}

#define NESTED_VALUE() POSTGAMMA_BACKEND_STATE_VALUE(0, outer)
#define NESTED_ADDRESS() (&NESTED_VALUE())
#define BIND_NESTED() (PostgammaCurrentPreparedRoleState = (unsigned char *) &nested)

static int *
nested_macro_address(void)
{
	return NESTED_ADDRESS();
}

static int
macro_binding_boundary(void)
{
	BIND_NESTED();
	return NESTED_VALUE();
}

#define CALL_BINDING_BOUNDARY() macro_binding_boundary()

static int
binding_wrapper(void)
{
	(void) CALL_BINDING_BOUNDARY();
	return NESTED_VALUE();
}

static unsigned char *
read_binding(void)
{
	return PostgammaCurrentPreparedRoleState;
}

static int
read_binding_wrapper(void)
{
	return read_binding() != NULL ? NESTED_VALUE() : 0;
}

static int
nested_macro_callback(void (*callback)(void))
{
	callback();
	return NESTED_VALUE();
}

static int
optional_read(int enabled)
{
	return enabled ? POSTGAMMA_BACKEND_STATE_VALUE(0, outer) : 17;
}

static void
optional_write(int enabled)
{
	if (enabled)
		POSTGAMMA_BACKEND_STATE_VALUE(0, outer)++;
	calls++;
}

static int *
state_address(void)
{
	return &POSTGAMMA_BACKEND_STATE_VALUE(0, outer);
}

static int
bind_then_read(void (*callback)(void))
{
	callback();
	return POSTGAMMA_BACKEND_STATE_VALUE(0, outer);
}

static void
bind_outer(void)
{
	PostgammaCurrentPreparedRoleState = (unsigned char *) &outer;
}

static int *
static_identity(int enabled)
{
	static int value;
	if (enabled)
		value = POSTGAMMA_BACKEND_STATE_VALUE(0, outer);
	return &value;
}

static const char *
string_identity(int enabled)
{
	if (enabled)
		POSTGAMMA_BACKEND_STATE_VALUE(0, outer)++;
	return "same object";
}

static int
compound_identity(int enabled)
{
	int *values = (int []) { 7, 9 };
	return enabled ? POSTGAMMA_BACKEND_STATE_VALUE(0, outer) : values[1];
}

static const char *
function_identity(int enabled)
{
	if (enabled)
		POSTGAMMA_BACKEND_STATE_VALUE(0, outer)++;
	return __func__;
}

static int
read_value(int increment)
{
	return POSTGAMMA_BACKEND_STATE_VALUE(0, outer) + increment;
}

static void
write_value(int value)
{
	POSTGAMMA_BACKEND_STATE_VALUE(0, outer) = value;
}

int
recursive_api(int depth)
{
	return depth == 0 ? read_value(0) : recursive_api(depth - 1) + 1;
}

int
callback_api(int (*callback)(void))
{
	int before = read_value(1);
	int result = callback();
	return before + result + read_value(2);
}

static int
nested_callback(void)
{
	unsigned char *saved = PostgammaCurrentPreparedRoleState;
	int (*indirect)(int) = recursive_api;
	int result;
	PostgammaCurrentPreparedRoleState = (unsigned char *) &nested;
	result = indirect(3);
	PostgammaCurrentPreparedRoleState = saved;
	return result;
}

static int
exit_callback(void)
{
	PostgammaCurrentPreparedRoleState = (unsigned char *) &nested;
	longjmp(exit_boundary, 1);
}

int
main(int argc, char **argv)
{
	int (*public_entry)(int (*)(void)) = callback_api;

	if (argc == 2 && strcmp(argv[1], "unbound-address") == 0)
	{
		/* Taking the address must fail even without loading from the slot. */
		int *volatile address = state_address();
		(void) address;
		return 1;
	}
	if (argc == 2 && strcmp(argv[1], "unbound-nested-address") == 0)
	{
		int *volatile address = nested_macro_address();
		(void) address;
		return 1;
	}
	if (argc == 2 && strcmp(argv[1], "unbound-inline-address") == 0)
	{
		int *volatile address = inline_address(1);
		(void) address;
		return 1;
	}
	if (inline_address(0) != NULL || inline_annotated(0) != 0)
		abort();
	if (optional_read(0) != 17)
		abort();
	optional_write(0);
	if (calls != 1)
		abort();
	if (argc == 2 && strcmp(argv[1], "unbound-callback") == 0)
	{
		if (inline_callback(bind_outer) != outer || nested_macro_callback(bind_outer) != outer ||
			bind_then_read(bind_outer) != outer)
			abort();
		puts("unbound callback observes the binding it established");
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "identities") == 0)
	{
		int *before = static_identity(0);
		int *inline_before = inline_identity(0);
		const char *text = string_identity(0);
		const char *name = function_identity(0);
		const char *inline_name = inline_builtin_identity(0);

		bind_outer();
		if (static_identity(1) != before || *before != outer ||
			inline_identity(1) != inline_before || *inline_before != outer ||
			inline_builtin_identity(0) != inline_name || strcmp(inline_name, "inline_builtin_identity") != 0 ||
			string_identity(1) != text || function_identity(1) != name ||
			strcmp(name, "function_identity") != 0 || compound_identity(0) != 9)
			abort();
		puts("static, literal and function identities preserved");
		return 0;
	}
	PostgammaCurrentPreparedRoleState = (unsigned char *) &outer;
	if (read_binding_wrapper() != outer || nested_macro_address() != &outer ||
		read_late_header() != outer || inline_annotated(1) != outer ||
		inline_direct_and_indirect() != 2 * outer ||
		binding_wrapper() != nested || nested_macro_address() != &nested ||
		inline_direct_and_indirect() != 2 * nested)
		abort();
	PostgammaCurrentPreparedRoleState = (unsigned char *) &outer;
	optional_write(1);
	if (calls != 2 || outer != 12)
		abort();
	outer = 11;
	if (public_entry(nested_callback) != 50)
		abort();
	write_value(33);
	if (outer != 33 || nested != 22 || recursive_api(2) != 35)
		abort();
	if (setjmp(exit_boundary) == 0)
	{
		(void) public_entry(exit_callback);
		abort();
	}
	/* Nonlocal exits do not restore TLS; the catching boundary must do so. */
	PostgammaCurrentPreparedRoleState = (unsigned char *) &outer;
	if (recursive_api(1) != 34)
		abort();
	puts("callbacks, recursion, address-taken ABI and error restoration passed");
	return 0;
}
