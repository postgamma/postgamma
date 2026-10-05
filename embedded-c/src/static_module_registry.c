/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/private/static_module_registry.h"

#include <string.h>

static int
postgamma_valid_registry(const PostgammaStaticModuleRegistry *registry)
{
	return registry != NULL &&
		registry->abi_tag == POSTGAMMA_STATIC_REGISTRY_ABI_TAG &&
		(registry->module_count == 0 || registry->modules != NULL);
}

static int
postgamma_valid_module(const PostgammaStaticModuleDefinition *module)
{
	size_t index;

	if (module == NULL || module->logical_name == NULL ||
		(module->symbol_count != 0 && module->symbols == NULL))
		return 0;
	for (index = 0; index < module->symbol_count; index++)
	{
		const PostgammaStaticModuleSymbol *symbol = &module->symbols[index];

		if (symbol->logical_name == NULL || symbol->function == NULL ||
			(index != 0 && strcmp(module->symbols[index - 1].logical_name,
							   symbol->logical_name) >= 0))
			return 0;
	}
	return 1;
}

static const PostgammaStaticModuleDefinition *
postgamma_module_from_handle(const PostgammaStaticModuleHandle *handle)
{
	if (handle == NULL || handle->abi_tag != POSTGAMMA_STATIC_MODULE_ABI_TAG ||
		!postgamma_valid_registry(handle->registry) ||
		handle->module_index >= handle->registry->module_count)
		return NULL;
	return &handle->registry->modules[handle->module_index];
}

int
postgamma_static_module_open(const PostgammaStaticModuleRegistry *registry,
								 const char *logical_name,
								 PostgammaStaticModuleHandle *handle)
{
	size_t low = 0;
	size_t high;

	if (handle == NULL)
		return 0;
	handle->abi_tag = 0;
	handle->registry = NULL;
	handle->module_index = 0;
	if (!postgamma_valid_registry(registry) || logical_name == NULL)
		return 0;
	for (low = 0; low < registry->module_count; low++)
	{
		if (!postgamma_valid_module(&registry->modules[low]) ||
			(low != 0 &&
			 strcmp(registry->modules[low - 1].logical_name,
					registry->modules[low].logical_name) >= 0))
			return 0;
	}

	low = 0;
	high = registry->module_count;
	while (low < high)
	{
		size_t middle = low + (high - low) / 2;
		const PostgammaStaticModuleDefinition *module = &registry->modules[middle];
		int comparison;

		if (!postgamma_valid_module(module))
			return 0;
		comparison = strcmp(logical_name, module->logical_name);
		if (comparison == 0)
		{
			handle->abi_tag = POSTGAMMA_STATIC_MODULE_ABI_TAG;
			handle->registry = registry;
			handle->module_index = middle;
			return 1;
		}
		if (comparison < 0)
			high = middle;
		else
			low = middle + 1;
	}
	return 0;
}

PostgammaStaticFunction
postgamma_static_module_symbol(const PostgammaStaticModuleHandle *handle,
							   const char *symbol_name)
{
	const PostgammaStaticModuleDefinition *module;
	size_t low = 0;
	size_t high;

	module = postgamma_module_from_handle(handle);
	if (!postgamma_valid_module(module) || symbol_name == NULL)
		return NULL;

	high = module->symbol_count;
	while (low < high)
	{
		size_t middle = low + (high - low) / 2;
		const PostgammaStaticModuleSymbol *symbol = &module->symbols[middle];
		int comparison;

		comparison = strcmp(symbol_name, symbol->logical_name);
		if (comparison == 0)
			return symbol->function;
		if (comparison < 0)
			high = middle;
		else
			low = middle + 1;
	}
	return NULL;
}

int
postgamma_static_module_close(PostgammaStaticModuleHandle *handle)
{
	if (!postgamma_valid_module(postgamma_module_from_handle(handle)))
		return 0;
	handle->abi_tag = 0;
	handle->registry = NULL;
	handle->module_index = 0;
	return 1;
}

size_t
postgamma_static_module_dynamic_load_attempts(
	const PostgammaStaticModuleRegistry *registry)
{
	return postgamma_valid_registry(registry) ? 0 : SIZE_MAX;
}
