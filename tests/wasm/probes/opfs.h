/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
#ifndef POSTGAMMA_OPFS_PROBE_H
#define POSTGAMMA_OPFS_PROBE_H

#include <pthread.h>
#include <stdatomic.h>

enum OpfsEvent { OP_READ = 1, OP_WRITE, OP_FLUSH, OP_OPEN, OP_CLOSE,
                 OP_BLOB, OP_DIR_BEGIN, OP_DIR_END };
enum OpfsFault { SHORT_READ = 1, SHORT_WRITE, READ_TYPE_ERROR, WRITE_QUOTA,
                 FLUSH_ERROR, WRITE_GATE, DIR_DELAY };
extern _Atomic int opfs_counts[9], opfs_owner, opfs_owner_errors;
extern _Atomic int opfs_fault, opfs_faults_seen, opfs_gate, opfs_gate_entered;
extern _Atomic int opfs_stage, opfs_failures;

double opfs_now(void);
void opfs_check(const char *name, int passed, long result, int error,
                const char *capability);
void opfs_benchmark(void);

#endif
