/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
const median = rows => [...rows].sort((a, b) => a - b)[Math.floor(rows.length / 2)];
export function resourceGate(samples, rssBudgetBytes) {
  const fields = ['elapsedSeconds', 'rssBytes', 'pssBytes', 'anonymousPssBytes', 'sharedMemoryPssBytes',
    'filePssBytes', 'swapPssBytes', 'threads', 'fileDescriptors', 'virtualOpens', 'retiredWorkerMaxAgeMs'];
  if (!Number.isFinite(rssBudgetBytes) || rssBudgetBytes < 0 ||
      samples.some(row => fields.some(name => !Number.isFinite(row[name]) || row[name] < 0)))
    return {status: 'failed', reason: 'Missing or invalid process/storage resource measurement'};
  if (samples.some(row => row.retiredWorkerMaxAgeMs > 10000))
    return {status: 'failed', reason: 'Retired control or pthread Worker survived for more than ten seconds'};
  // VACUUM warms PostgreSQL's virtual file cache. Compare the same point in
  // each instance lifetime, immediately before reopen, not cold vs warm caches.
  const steady = samples.filter(row => row.elapsedSeconds >= 60 && row.cycleEnd);
  if (steady.length < 10) return {status: 'not-run', reason: 'Fewer than ten complete workload cycles after warmup', samples: steady.length};
  const before = steady.slice(0, 5), after = steady.slice(-5);
  const delta = value => median(after.map(value)) - median(before.map(value));
  const growth = name => delta(row => row[name]);
  const result = {status: 'passed', warmupSeconds: 60, phase: 'before-reopen', samples: steady.length,
    rssGrowthBytes: growth('rssBytes'),
    // File PSS is apportioned across every mapping on the host, including
    // unrelated browsers. Their exit can raise our PSS without an allocation.
    // Keep total PSS observable; anonymous/shared memory and swap track our
    // allocations, and RSS still gates resident growth including file pages.
    accountedMemoryGrowthBytes: delta(row => row.anonymousPssBytes + row.sharedMemoryPssBytes + row.swapPssBytes),
    totalPssGrowthBytes: growth('pssBytes'), filePssGrowthBytes: growth('filePssBytes'),
    threadGrowth: growth('threads'), fdGrowth: growth('fileDescriptors'),
    virtualOpenGrowth: growth('virtualOpens'), rssBudgetBytes, threadBudget: 32, fdBudget: 128, virtualOpenBudget: 8};
  if (result.rssGrowthBytes > rssBudgetBytes || result.accountedMemoryGrowthBytes > rssBudgetBytes ||
      result.threadGrowth > 32 || result.fdGrowth > 128 || result.virtualOpenGrowth > 8)
    result.status = 'failed';
  return result;
}
