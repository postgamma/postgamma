/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_STATIC_MODULE_PROVIDER_H
#define POSTGAMMA_STATIC_MODULE_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "postgamma/postgamma_extension.h"


#ifdef __cplusplus
extern "C" {
#endif


typedef struct PostgammaStaticModuleProvider
{
	size_t		struct_size;
	uint64_t	abi_tag;
	void	   *context;
	bool	  (*open)(
		void *context, const char *library_name, const void **handle);
	bool	  (*owns)(void *context, const void *handle);
	void	 *(*symbol)(
		void *context, const void *handle, const char *symbol_name);
	int	  (*register_extension)(
		void *context, const pgmex_descriptor *descriptor);
	size_t	  (*bundled_extension_count)(void *context);
	const char *(*bundled_extension_name)(void *context, size_t index);
} PostgammaStaticModuleProvider;


#define POSTGAMMA_STATIC_MODULE_PROVIDER_ABI_TAG \
	UINT64_C(0x50474d5354415433)
#define POSTGAMMA_STATIC_MODULE_PROVIDER_INIT \
	{ \
		sizeof(PostgammaStaticModuleProvider), \
		POSTGAMMA_STATIC_MODULE_PROVIDER_ABI_TAG, NULL, NULL, NULL, NULL, NULL, \
		NULL, NULL \
	}


int postgamma_static_module_provider_install(
	const PostgammaStaticModuleProvider *provider);
int postgamma_static_module_register_extension(
	const pgmex_descriptor *descriptor);


#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_STATIC_MODULE_PROVIDER_H */
