/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
const median = rows => [...rows].sort((a, b) => a - b)[Math.floor(rows.length / 2)];
export function resourceGate(samples, rssBudgetBytes) {
  const fields = ['elapsedSeconds', 'rssBytes', 'pssBytes', 'swapPssBytes',
    'threads', 'fileDescriptors', 'virtualOpens', 'retiredWorkerMaxAgeMs'];
  const valid = value => Number.isFinite(value) && value >= 0;
  const bounded = row => row.memoryAccounting === 'smaps-bounds';
  const validMemory = row => bounded(row) ?
    ['anonymousPssBytes', 'sharedMemoryPssBytes', 'filePssBytes'].every(name => row[name] === null) &&
      valid(row.accountedMemoryLowerBytes) && valid(row.accountedMemoryUpperBytes) &&
      row.accountedMemoryLowerBytes <= row.accountedMemoryUpperBytes :
    ['anonymousPssBytes', 'sharedMemoryPssBytes', 'filePssBytes'].every(name => valid(row[name]));
  if (!Number.isFinite(rssBudgetBytes) || rssBudgetBytes < 0 ||
      samples.some(row => fields.some(name => !valid(row[name])) || !validMemory(row)))
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
  const exact = row => row.anonymousPssBytes + row.sharedMemoryPssBytes + row.swapPssBytes;
  const lower = row => bounded(row) ? row.accountedMemoryLowerBytes : exact(row);
  const upper = row => bounded(row) ? row.accountedMemoryUpperBytes : exact(row);
  const hasBounds = steady.some(bounded);
  const growthLower = median(after.map(lower)) - median(before.map(upper));
  const growthUpper = median(after.map(upper)) - median(before.map(lower));
  const result = {status: 'passed', warmupSeconds: 60, phase: 'before-reopen', samples: steady.length,
    rssGrowthBytes: growth('rssBytes'),
    // File PSS is apportioned across every mapping on the host, including
    // unrelated browsers. Their exit can raise our PSS without an allocation.
    // Keep total PSS observable; anonymous/shared memory and swap track our
    // allocations, and RSS still gates resident growth including file pages.
    accountedMemoryGrowthBytes: growthUpper,
    accountedMemoryGrowthLowerBytes: growthLower, accountedMemoryGrowthUpperBytes: growthUpper,
    memoryAccounting: hasBounds ? 'smaps-bounds' : 'smaps-rollup',
    pssBreakdownGate: hasBounds ? {status: 'not-run', reason: 'Kernel lacks direct PSS category fields; budget uses conservative bounds'} : {status: 'passed'},
    totalPssGrowthBytes: growth('pssBytes'), filePssGrowthBytes: hasBounds ? null : growth('filePssBytes'),
    threadGrowth: growth('threads'), fdGrowth: growth('fileDescriptors'),
    virtualOpenGrowth: growth('virtualOpens'), rssBudgetBytes, threadBudget: 32, fdBudget: 128, virtualOpenBudget: 8};
  if (result.rssGrowthBytes > rssBudgetBytes || result.accountedMemoryGrowthBytes > rssBudgetBytes ||
      result.threadGrowth > 32 || result.fdGrowth > 128 || result.virtualOpenGrowth > 8)
    result.status = 'failed';
  return result;
}
