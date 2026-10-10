/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
// Collect only after a failed gate. A passing acceptance run never sends a
// memory-report signal; diagnostic allocations cannot affect its budget.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync, readdirSync, mkdirSync} from 'node:fs';
import {join, dirname} from 'node:path';
import {gunzipSync} from 'node:zlib';

export async function firefoxFailureMemory(report, {pid, temporary, output, timeoutMs = 15000,
  readStatus = () => readFileSync(`/proc/${pid}/status`, 'utf8'), signal = () => process.kill(pid, 34)}) {
  assert.equal(report.status, 'failed', 'A native memory report is allowed only after failure');
  assert(Number.isInteger(timeoutMs) && timeoutMs >= 0 && timeoutMs <= 15000);
  const receipt = report.failureMemory = {kind: 'postgamma.wasm-failure-memory-diagnostic',
    productionAcceptance: false, phase: 'after-failed-gate', forcedGC: false, status: 'failed', pid};
  try {
    // On the pinned Linux Firefox, SIGRTMIN (34) dumps without minimizing.
    // Check its handler before signalling: never signal a different process.
    const mask = readStatus().match(/^SigCgt:\s+([0-9a-f]+)$/mi)?.[1];
    if (!mask || !(BigInt('0x' + mask) & (1n << 33n))) {
      receipt.status = 'not-run';receipt.reason = 'Native Firefox memory signal handler is unavailable';return;
    }
    const before = new Set(readdirSync(temporary));
    signal();receipt.signal = 34;
    const deadline = performance.now() + timeoutMs;
    do {
      for (const name of readdirSync(temporary)) {
        if (before.has(name) || !/^unified-memory-report-.*\.json\.gz$/.test(name)) continue;
        try {
          const bytes = readFileSync(join(temporary, name)), value = JSON.parse(gunzipSync(bytes));
          if (!Array.isArray(value.reports) || value.reports.length === 0) continue;
          mkdirSync(dirname(output), {recursive: true});writeFileSync(output, bytes);
          Object.assign(receipt, {status: 'collected', path: output, bytes: bytes.length,
            sha256: createHash('sha256').update(bytes).digest('hex'), reports: value.reports.length});return;
        } catch (error) {
          // Firefox can still be writing the gzip stream. Retain the original
          // failed result even when the diagnostic writer never finishes.
          receipt.lastReadError = String(error);
        }
      }
      if (performance.now() >= deadline) break;
      await new Promise(resolve => setTimeout(resolve, 100));
    } while (performance.now() <= deadline);
    receipt.error = 'Native Firefox failure memory report deadline';
  } catch (error) {receipt.error = String(error);}
}
