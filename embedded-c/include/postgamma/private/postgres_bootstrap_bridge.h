/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_POSTGRES_BOOTSTRAP_BRIDGE_H
#define POSTGAMMA_PRIVATE_POSTGRES_BOOTSTRAP_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>


#ifdef __cplusplus
extern "C" {
#endif


int postgamma_postgres_bootstrap_memory(
	uint64_t generation,
	const char *data_directory,
	const char *resource_root,
	uint64_t wal_segment_size_bytes,
	bool data_checksums,
	mode_t logical_umask,
	const char *input,
	size_t input_length,
	int *postgres_exit_code);
int postgamma_postgres_single_user_memory(
	uint64_t generation,
	const char *data_directory,
	const char *executable_path,
	const char *resource_root,
	const char *username,
	mode_t logical_umask,
	const char *input,
	size_t input_length,
	int *postgres_exit_code);


#ifdef __cplusplus
}
#endif

#endif /* POSTGAMMA_PRIVATE_POSTGRES_BOOTSTRAP_BRIDGE_H */
