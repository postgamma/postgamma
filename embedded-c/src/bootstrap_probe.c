/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgres.h"

#include <pthread.h>
#include <string.h>

#include "fmgr.h"
#include "postgamma/private/bootstrap_probe.h"
#include "postgamma/private/static_module_registry.h"
#include "utils/palloc.h"

#include "static_module_facts.inc"

typedef int (*PostgammaBootstrapIntFunction)(void);

static pthread_once_t bootstrap_extension_init_once = PTHREAD_ONCE_INIT;
static int bootstrap_extension_init_status;

static uintptr_t
postgamma_bootstrap_palloc_address(void)
{
	void *(*allocator)(Size) = palloc;
	uintptr_t address = 0;

	_Static_assert(sizeof(allocator) <= sizeof(address),
				   "function pointer does not fit in the bootstrap address probe");
	memcpy(&address, &allocator, sizeof(allocator));
	return address;
}

static void
postgamma_bootstrap_initialize_extension(void)
{
	PostgammaStaticModuleHandle module;
	PostgammaStaticFunction init_function;

	if (!postgamma_static_module_open(&postgamma_generated_registry,
									 "bootstrap_extension", &module))
	{
		bootstrap_extension_init_status = -1;
		return;
	}
	init_function = postgamma_static_module_symbol(&module, "_PG_init");
	if (init_function == NULL)
	{
		bootstrap_extension_init_status = -1;
		return;
	}
	init_function();
	if (!postgamma_static_module_close(&module))
	{
		bootstrap_extension_init_status = -1;
		return;
	}
	bootstrap_extension_init_status = 1;
}

static pgm_bootstrap_probe_result
postgamma_bootstrap_failure(unsigned int checks, const char *detail)
{
	pgm_bootstrap_probe_result result = {
		.status = PGM_BOOTSTRAP_PROBE_FAIL,
		.checks = checks,
		.dynamic_load_attempts = (unsigned int)
			postgamma_static_module_dynamic_load_attempts(
				&postgamma_generated_registry),
		.init_calls = 0,
		.kernel_symbol_address = 0,
		.detail = detail
	};

	return result;
}

pgm_bootstrap_probe_result
pgm_bootstrap_probe_static_module(void)
{
	PostgammaStaticModuleHandle module;
	PostgammaStaticModuleHandle unknown_module;
	PostgammaStaticFunction magic_function;
	PostgammaStaticFunction init_count_function;
	PostgammaStaticFunction link_probe_function;
	const Pg_magic_struct *module_magic;
	unsigned int checks = 0;
	int init_calls;
	int link_probe;

	checks++;
	if (!postgamma_static_module_open(&postgamma_generated_registry,
									 "bootstrap_extension", &module))
		return postgamma_bootstrap_failure(checks, "registered module was not found");

	checks++;
	if (postgamma_static_module_open(&postgamma_generated_registry,
									"unregistered", &unknown_module))
		return postgamma_bootstrap_failure(checks, "unknown module did not fail closed");

	magic_function = postgamma_static_module_symbol(&module, "Pg_magic_func");
	checks++;
	if (magic_function == NULL)
		return postgamma_bootstrap_failure(checks, "module magic symbol was not found");
	module_magic = ((PGModuleMagicFunction) magic_function) ();
	checks++;
	if (module_magic == NULL || module_magic->len != sizeof(Pg_magic_struct) ||
		module_magic->abi_fields.version != PG_VERSION_NUM / 100)
		return postgamma_bootstrap_failure(checks, "module magic is incompatible");

	checks++;
	if (postgamma_static_module_symbol(&module, "bootstrap_extension_answer") == NULL ||
		postgamma_static_module_symbol(
			&module, "pg_finfo_bootstrap_extension_answer") == NULL)
		return postgamma_bootstrap_failure(checks, "SQL function symbols were not found");

	checks++;
	if (postgamma_static_module_symbol(&module, "missing") != NULL)
		return postgamma_bootstrap_failure(checks, "unknown symbol did not fail closed");

	if (pthread_once(&bootstrap_extension_init_once,
					 postgamma_bootstrap_initialize_extension) != 0 ||
		bootstrap_extension_init_status != 1)
		return postgamma_bootstrap_failure(checks, "module initialization failed");
	init_count_function = postgamma_static_module_symbol(
		&module, "pgm_bootstrap_extension_init_count");
	checks++;
	if (init_count_function == NULL)
		return postgamma_bootstrap_failure(checks, "init counter symbol was not found");
	init_calls = ((PostgammaBootstrapIntFunction) init_count_function) ();
	checks++;
	if (init_calls != 1)
		return postgamma_bootstrap_failure(checks, "module init did not run exactly once");

	link_probe_function = postgamma_static_module_symbol(
		&module, "pgm_bootstrap_extension_link_probe");
	checks++;
	if (link_probe_function == NULL)
		return postgamma_bootstrap_failure(checks, "link probe symbol was not found");
	link_probe = ((PostgammaBootstrapIntFunction) link_probe_function) ();
	checks++;
	if (link_probe != 1)
		return postgamma_bootstrap_failure(checks, "hidden PostgreSQL link probe failed");

	checks++;
	if (!postgamma_static_module_close(&module))
		return postgamma_bootstrap_failure(checks, "static module close rejected its handle");
	checks++;
	if (postgamma_static_module_dynamic_load_attempts(
			&postgamma_generated_registry) != 0)
		return postgamma_bootstrap_failure(checks, "dynamic module loader was invoked");

	return (pgm_bootstrap_probe_result) {
		.status = PGM_BOOTSTRAP_PROBE_PASS,
		.checks = checks,
		.dynamic_load_attempts = 0,
		.init_calls = (unsigned int) init_calls,
		.kernel_symbol_address = postgamma_bootstrap_palloc_address(),
		.detail = "static module registry passed"
	};
}
