/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_IO_PLATFORM_H
#define POSTGAMMA_IO_PLATFORM_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

int postgamma_platform_pipe(int descriptors[2]);
ssize_t postgamma_platform_read(int fd, void *buffer, size_t size);
ssize_t postgamma_platform_write(int fd, const void *buffer, size_t size);
int postgamma_platform_close(int fd);
int postgamma_platform_poll(struct pollfd *fds, nfds_t count, int timeout);
/* Resume and logical state changes wake waiters; callers recheck predicates. */
void postgamma_platform_notify(void);
uint32_t postgamma_platform_notification_sequence(void);
int postgamma_platform_notification_wait(uint32_t expected, double timeout_ms);
size_t postgamma_platform_pipe_count(void);

#endif
