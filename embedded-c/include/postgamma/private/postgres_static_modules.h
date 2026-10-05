/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_POSTGRES_STATIC_MODULES_H
#define POSTGAMMA_PRIVATE_POSTGRES_STATIC_MODULES_H

#include "postgamma/static_module_provider.h"


#ifdef __cplusplus
extern "C" {
#endif


const PostgammaStaticModuleProvider *
postgamma_product_static_module_provider(void);

typedef struct PostgammaProductStaticModuleInfo
{
	const char *id;
	const char *logical_name;
	const char *version;
	uint32_t	postgresql_major;
	uint32_t	sdk_abi_version;
	uint64_t	capabilities;
	bool		sdk_contract;
} PostgammaProductStaticModuleInfo;

size_t postgamma_product_static_module_count(void);
bool postgamma_product_static_module_info(
	size_t index, PostgammaProductStaticModuleInfo *info);


#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_PRIVATE_POSTGRES_STATIC_MODULES_H */
