/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_STATIC_MODULE_REGISTRY_H
#define POSTGAMMA_PRIVATE_STATIC_MODULE_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PostgammaStaticFunction)(void);

typedef struct PostgammaStaticModuleSymbol
{
	const char *logical_name;
	PostgammaStaticFunction function;
} PostgammaStaticModuleSymbol;

typedef struct PostgammaStaticModuleDefinition
{
	const char *logical_name;
	const PostgammaStaticModuleSymbol *symbols;
	size_t symbol_count;
} PostgammaStaticModuleDefinition;

typedef struct PostgammaStaticModuleRegistry
{
	uint64_t abi_tag;
	const PostgammaStaticModuleDefinition *modules;
	size_t module_count;
} PostgammaStaticModuleRegistry;

typedef struct PostgammaStaticModuleHandle
{
	uint64_t abi_tag;
	const PostgammaStaticModuleRegistry *registry;
	size_t module_index;
} PostgammaStaticModuleHandle;

#define POSTGAMMA_STATIC_MODULE_ABI_TAG UINT64_C(0x50474d4d4f44554c)
#define POSTGAMMA_STATIC_REGISTRY_ABI_TAG UINT64_C(0x50474d5245474953)

#define POSTGAMMA_STATIC_FUNCTION(symbol) ((PostgammaStaticFunction) (symbol))

#define POSTGAMMA_STATIC_MODULE_INITIALIZER(name, symbol_array) \
	{ \
		(name), (symbol_array), \
		sizeof(symbol_array) / sizeof((symbol_array)[0]) \
	}

#define POSTGAMMA_STATIC_REGISTRY_INITIALIZER(module_array) \
	{ \
		POSTGAMMA_STATIC_REGISTRY_ABI_TAG, (module_array), \
		sizeof(module_array) / sizeof((module_array)[0]) \
	}

int postgamma_static_module_open(
	const PostgammaStaticModuleRegistry *registry,
	const char *logical_name,
	PostgammaStaticModuleHandle *handle);

PostgammaStaticFunction postgamma_static_module_symbol(
	const PostgammaStaticModuleHandle *handle,
	const char *symbol_name);

int postgamma_static_module_close(PostgammaStaticModuleHandle *handle);

size_t postgamma_static_module_dynamic_load_attempts(
	const PostgammaStaticModuleRegistry *registry);

#ifdef __cplusplus
}
#endif

#endif
