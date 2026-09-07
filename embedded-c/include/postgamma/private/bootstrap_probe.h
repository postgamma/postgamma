/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_BOOTSTRAP_PROBE_H
#define POSTGAMMA_PRIVATE_BOOTSTRAP_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pgm_bootstrap_probe_status
{
	PGM_BOOTSTRAP_PROBE_NOT_RUN = 0,
	PGM_BOOTSTRAP_PROBE_PASS = 1,
	PGM_BOOTSTRAP_PROBE_FAIL = 2
} pgm_bootstrap_probe_status;

typedef struct pgm_bootstrap_probe_result
{
	pgm_bootstrap_probe_status status;
	unsigned int checks;
	unsigned int dynamic_load_attempts;
	unsigned int init_calls;
	uintptr_t kernel_symbol_address;
	const char *detail;
} pgm_bootstrap_probe_result;

typedef struct pgm_bootstrap_result
{
	pgm_bootstrap_probe_status status;
	uint64_t generation;
	unsigned int checks;
	unsigned int resources_remaining;
	int exit_code;
	const char *last_phase;
	const char *detail;
} pgm_bootstrap_result;

typedef struct pgm_bootstrap_private_libpq_result
{
	pgm_bootstrap_probe_status status;
	unsigned int checks;
	unsigned int queue_capacity;
	unsigned int startup_packets;
	unsigned int query_packets;
	unsigned int copy_packets;
	unsigned int copy_bytes_before_response;
	unsigned int copy_bytes_received;
	unsigned int simultaneous_queue_saturation;
	unsigned int secure_read_calls;
	unsigned int secure_write_calls;
	unsigned int socket_wait_calls;
	unsigned int notice_count;
	unsigned int cancel_dispatches;
	unsigned int network_connect_calls;
	unsigned int optional_security_calls;
	unsigned int backend_pid;
	uint64_t connection_generation;
	uint64_t request_generation;
	uint64_t active_transports_after_close;
	uint64_t endpoint_references_after_close;
	uint64_t allocated_bytes_after_close;
	unsigned int observed_result_status;
	int observed_rows;
	int observed_columns;
	int observed_transaction_status;
	char observed_value[64];
	char observed_command_status[64];
	const char *detail;
	char last_error[256];
} pgm_bootstrap_private_libpq_result;

#if defined(_WIN32)
#define PGM_BOOTSTRAP_PUBLIC __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define PGM_BOOTSTRAP_PUBLIC __attribute__((visibility("default")))
#else
#define PGM_BOOTSTRAP_PUBLIC
#endif

PGM_BOOTSTRAP_PUBLIC pgm_bootstrap_probe_result pgm_bootstrap_probe_static_module(void);
PGM_BOOTSTRAP_PUBLIC pgm_bootstrap_result pgm_bootstrap_run(
	uint64_t generation,
	const char *data_directory,
	const char *resource_root,
	const char *bootstrap_input);
PGM_BOOTSTRAP_PUBLIC pgm_bootstrap_private_libpq_result pgm_bootstrap_probe_private_libpq(void);

#ifdef __cplusplus
}
#endif

#endif
