/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_LIBPQ_MEMORY_HOOKS_H
#define POSTGAMMA_PRIVATE_LIBPQ_MEMORY_HOOKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct pg_conn;

bool postgamma_libpq_memory_is_bound(struct pg_conn *connection);
bool postgamma_libpq_memory_connect_start(struct pg_conn *connection);
bool postgamma_libpq_memory_secure_read(
	struct pg_conn *connection,
	void *buffer,
	size_t length,
	ssize_t *result);
bool postgamma_libpq_memory_secure_write(
	struct pg_conn *connection,
	const void *buffer,
	size_t length,
	ssize_t *result);
bool postgamma_libpq_memory_socket_check(
	struct pg_conn *connection,
	int for_read,
	int for_write,
	int64_t end_time,
	int *result);

#define POSTGAMMA_LIBPQ_MEMORY_BOUND(connection) \
	postgamma_libpq_memory_is_bound((connection))

#define POSTGAMMA_LIBPQ_CONNECT_START_HOOK(connection) \
	do { \
		if (postgamma_libpq_memory_connect_start((connection))) \
			return 1; \
	} while (0)

#define POSTGAMMA_LIBPQ_SECURE_READ_HOOK(connection, buffer, length) \
	do { \
		ssize_t postgamma_libpq_hook_result; \
		if (postgamma_libpq_memory_secure_read( \
				(connection), (buffer), (length), \
				&postgamma_libpq_hook_result)) \
			return postgamma_libpq_hook_result; \
	} while (0)

#define POSTGAMMA_LIBPQ_SECURE_WRITE_HOOK(connection, buffer, length) \
	do { \
		ssize_t postgamma_libpq_hook_result; \
		if (postgamma_libpq_memory_secure_write( \
				(connection), (buffer), (length), \
				&postgamma_libpq_hook_result)) \
			return postgamma_libpq_hook_result; \
	} while (0)

#define POSTGAMMA_LIBPQ_SOCKET_CHECK_HOOK( \
		connection, for_read, for_write, end_time) \
	do { \
		int postgamma_libpq_hook_result; \
		if (postgamma_libpq_memory_socket_check( \
				(connection), (for_read), (for_write), (end_time), \
				&postgamma_libpq_hook_result)) \
			return postgamma_libpq_hook_result; \
	} while (0)

#endif
