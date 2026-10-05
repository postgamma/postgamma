/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgamma/private/postgres_static_modules.h"
#include "postgamma/contract_runtime.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct PostgammaProductModuleSymbol
{
	const char *logical_name;
	void	   *address;
} PostgammaProductModuleSymbol;

typedef struct PostgammaProductModuleResource
{
	const char *logical_path;
	bool		required;
} PostgammaProductModuleResource;

typedef struct PostgammaProductModule
{
	const char *id;
	const char *logical_name;
	const char *version;
	uint32_t	postgresql_major;
	uint32_t	sdk_abi_version;
	uint64_t	capabilities;
	uint32_t	lifecycle_phases;
	bool		sdk_contract;
	const PostgammaProductModuleSymbol *symbols;
	size_t		symbol_count;
	const PostgammaProductModuleResource *resources;
	size_t		resource_count;
} PostgammaProductModule;

typedef struct PostgammaProductRegistration
{
	const pgmex_descriptor *descriptor;
	int			library_state;
} PostgammaProductRegistration;


#include "postgamma_embedded_static_modules.inc"


static bool product_module_open(
	void *context, const char *library_name, const void **handle);
static bool product_module_owns(
	void *context, const void *handle);
static void *product_module_symbol(
	void *context, const void *handle, const char *symbol_name);
static int product_module_register_extension(
	void *context, const pgmex_descriptor *descriptor);
static size_t product_bundled_extension_count(void *context);
static const char *product_bundled_extension_name(
	void *context, size_t index);
static const char *module_basename(
	const char *library_name, size_t *name_length);
static const PostgammaProductModule *module_by_id(const char *id);
static bool descriptor_matches_module(
	const pgmex_descriptor *descriptor,
	const PostgammaProductModule *module);
static bool module_declares_resource(
	const PostgammaProductModule *module, const char *logical_path);
static uint32_t descriptor_lifecycle_phases(
	const pgmex_descriptor *descriptor);


static pthread_mutex_t PostgammaRegistrationMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t PostgammaRegistrationCondition = PTHREAD_COND_INITIALIZER;
static PostgammaProductRegistration *PostgammaRegistrations;


static const PostgammaStaticModuleProvider PostgammaProductModuleProvider =
{
	.struct_size = sizeof(PostgammaStaticModuleProvider),
	.abi_tag = POSTGAMMA_STATIC_MODULE_PROVIDER_ABI_TAG,
	.context = NULL,
	.open = product_module_open,
	.owns = product_module_owns,
	.symbol = product_module_symbol,
	.register_extension = product_module_register_extension,
	.bundled_extension_count = product_bundled_extension_count,
	.bundled_extension_name = product_bundled_extension_name,
};


const PostgammaStaticModuleProvider *
postgamma_product_static_module_provider(void)
{
	return &PostgammaProductModuleProvider;
}


size_t
postgamma_product_static_module_count(void)
{
	return PostgammaProductModuleCount;
}


bool
postgamma_product_static_module_info(
	size_t index, PostgammaProductStaticModuleInfo *info)
{
	const PostgammaProductModule *module;

	if (index >= PostgammaProductModuleCount || info == NULL)
		return false;
	module = &PostgammaProductModules[index];
	info->id = module->id;
	info->logical_name = module->logical_name;
	info->version = module->version;
	info->postgresql_major = module->postgresql_major;
	info->sdk_abi_version = module->sdk_abi_version;
	info->capabilities = module->capabilities;
	info->sdk_contract = module->sdk_contract;
	return true;
}


static size_t
product_bundled_extension_count(void *context)
{
	size_t		count = 0;
	size_t		index;

	(void) context;
	for (index = 0; index < PostgammaProductModuleCount; index++)
	{
		if (PostgammaProductModules[index].sdk_contract)
			count++;
	}
	return count;
}


static const char *
product_bundled_extension_name(void *context, size_t requested_index)
{
	size_t		visible_index = 0;
	size_t		index;

	(void) context;
	for (index = 0; index < PostgammaProductModuleCount; index++)
	{
		const PostgammaProductModule *module = &PostgammaProductModules[index];

		if (!module->sdk_contract)
			continue;
		if (visible_index++ == requested_index)
			return module->logical_name;
	}
	return NULL;
}


static const char *
module_basename(const char *library_name, size_t *name_length)
{
	const char *basename;
	size_t		length;

	if (library_name == NULL || name_length == NULL)
		return NULL;
	basename = strrchr(library_name, '/');
	basename = basename != NULL ? basename + 1 : library_name;
	length = strlen(basename);
	if (length > 3 && strcmp(basename + length - 3, ".so") == 0)
		length -= 3;
	if (length == 0)
		return NULL;
	*name_length = length;
	return basename;
}


static bool
product_module_open(
	void *context, const char *library_name, const void **handle)
{
	const char *basename;
	size_t		name_length;
	size_t		low = 0;
	size_t		high = PostgammaProductModuleCount;

	(void) context;
	if (handle == NULL)
		return false;
	*handle = NULL;
	basename = module_basename(library_name, &name_length);
	if (basename == NULL)
		return false;
	while (low < high)
	{
		size_t middle = low + (high - low) / 2;
		const PostgammaProductModule *module =
			&PostgammaProductModules[middle];
		size_t logical_length = strlen(module->logical_name);
		int comparison;

		comparison = strncmp(basename, module->logical_name,
			name_length < logical_length ? name_length : logical_length);
		if (comparison == 0)
		{
			if (name_length < logical_length)
				comparison = -1;
			else if (name_length > logical_length)
				comparison = 1;
		}
		if (comparison == 0)
		{
			*handle = module;
			return true;
		}
		if (comparison < 0)
			high = middle;
		else
			low = middle + 1;
	}
	return false;
}


static bool
product_module_owns(void *context, const void *handle)
{
	size_t		index;

	(void) context;
	for (index = 0; index < PostgammaProductModuleCount; index++)
	{
		if (handle == &PostgammaProductModules[index])
			return true;
	}
	return false;
}


static void *
product_module_symbol(
	void *context, const void *handle, const char *symbol_name)
{
	const PostgammaProductModule *module = handle;
	size_t		low = 0;
	size_t		high;

	(void) context;
	if (!product_module_owns(NULL, handle) || symbol_name == NULL)
		return NULL;
	high = module->symbol_count;
	while (low < high)
	{
		size_t middle = low + (high - low) / 2;
		const PostgammaProductModuleSymbol *symbol = &module->symbols[middle];
		int comparison = strcmp(symbol_name, symbol->logical_name);

		if (comparison == 0)
			return symbol->address;
		if (comparison < 0)
			high = middle;
		else
			low = middle + 1;
	}
	return NULL;
}


static int
product_module_register_extension(
	void *context, const pgmex_descriptor *descriptor)
{
	const PostgammaProductModule *module;
	PostgammaProductRegistration *registration;
	size_t		index;
	int			status;

	(void) context;
	if (descriptor == NULL || descriptor->id == NULL)
		return EINVAL;
	module = module_by_id(descriptor->id);
	if (module == NULL || !descriptor_matches_module(descriptor, module))
		return ENOTSUP;
	index = (size_t) (module - PostgammaProductModules);
	status = pthread_mutex_lock(&PostgammaRegistrationMutex);
	if (status != 0)
		return status;
	if (PostgammaRegistrations == NULL)
	{
		PostgammaRegistrations = calloc(
			PostgammaProductModuleCount, sizeof(*PostgammaRegistrations));
		if (PostgammaRegistrations == NULL)
		{
			(void) pthread_mutex_unlock(&PostgammaRegistrationMutex);
			return ENOMEM;
		}
	}
	registration = &PostgammaRegistrations[index];
	if (registration->descriptor != NULL &&
		registration->descriptor != descriptor)
	{
		(void) pthread_mutex_unlock(&PostgammaRegistrationMutex);
		return EEXIST;
	}
	registration->descriptor = descriptor;
	while (registration->library_state == 1)
	{
		status = pthread_cond_wait(
			&PostgammaRegistrationCondition, &PostgammaRegistrationMutex);
		if (status != 0)
		{
			(void) pthread_mutex_unlock(&PostgammaRegistrationMutex);
			return status;
		}
	}
	if (registration->library_state == 2)
	{
		(void) pthread_mutex_unlock(&PostgammaRegistrationMutex);
		return 0;
	}
	registration->library_state = 1;
	status = pthread_mutex_unlock(&PostgammaRegistrationMutex);
	if (status != 0)
		postgamma_runtime_contract_violation();
	status = descriptor->library_initialize();
	if (pthread_mutex_lock(&PostgammaRegistrationMutex) != 0)
		postgamma_runtime_contract_violation();
	registration->library_state = status == 0 ? 2 : 0;
	if (status == 0)
		(void) fprintf(
			stderr,
			"POSTGAMMA_EXTENSION_LIBRARY id=%s initializations=1\n",
			descriptor->id);
	(void) pthread_cond_broadcast(&PostgammaRegistrationCondition);
	(void) pthread_mutex_unlock(&PostgammaRegistrationMutex);
	return status > 0 ? status : (status == 0 ? 0 : EPROTO);
}


static const PostgammaProductModule *
module_by_id(const char *id)
{
	size_t		index;

	if (id == NULL)
		return NULL;
	for (index = 0; index < PostgammaProductModuleCount; index++)
	{
		if (strcmp(PostgammaProductModules[index].id, id) == 0)
			return &PostgammaProductModules[index];
	}
	return NULL;
}


static bool
descriptor_matches_module(
	const pgmex_descriptor *descriptor,
	const PostgammaProductModule *module)
{
	size_t		index;

	if (!module->sdk_contract ||
		descriptor->struct_size != sizeof(*descriptor) ||
		descriptor->abi_version != module->sdk_abi_version ||
		descriptor->postgresql_major != module->postgresql_major ||
		descriptor->capabilities != module->capabilities ||
		descriptor_lifecycle_phases(descriptor) != module->lifecycle_phases ||
		strcmp(descriptor->id, module->id) != 0 ||
		strcmp(descriptor->sql_name, module->logical_name) != 0 ||
		strcmp(descriptor->version, module->version) != 0 ||
		descriptor->resource_count != module->resource_count)
		return false;
	for (index = 0; index < descriptor->resource_count; index++)
	{
		if (!module_declares_resource(module, descriptor->resources[index]))
			return false;
	}
	return true;
}


static uint32_t
descriptor_lifecycle_phases(const pgmex_descriptor *descriptor)
{
	uint32_t	phases = 0;

	if (descriptor->library_initialize != NULL)
		phases |= UINT32_C(1) << PGMEX_PHASE_LIBRARY_INITIALIZE;
	if (descriptor->instance_request != NULL)
		phases |= UINT32_C(1) << PGMEX_PHASE_INSTANCE_REQUEST;
	if (descriptor->instance_startup != NULL)
		phases |= UINT32_C(1) << PGMEX_PHASE_INSTANCE_STARTUP;
	if (descriptor->instance_shutdown != NULL)
		phases |= UINT32_C(1) << PGMEX_PHASE_INSTANCE_SHUTDOWN;
	if (descriptor->session_initialize != NULL)
		phases |= UINT32_C(1) << PGMEX_PHASE_SESSION_INITIALIZE;
	if (descriptor->session_reset != NULL)
		phases |= UINT32_C(1) << PGMEX_PHASE_SESSION_RESET;
	if (descriptor->session_destroy != NULL)
		phases |= UINT32_C(1) << PGMEX_PHASE_SESSION_DESTROY;
	return phases;
}


static bool
module_declares_resource(
	const PostgammaProductModule *module, const char *logical_path)
{
	size_t		index;

	if (logical_path == NULL)
		return false;
	for (index = 0; index < module->resource_count; index++)
	{
		if (strcmp(module->resources[index].logical_path, logical_path) == 0)
			return true;
	}
	return false;
}
