/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {assert} from '../tests/helpers.js';
import {limits, pthreadPoolSize} from '../src/protocol/limits.js';

export function storageBudget(modules: any[], events: any[]) {
  const stacks: Record<string, number> = {dedicated: 0, client: 0, parallel: 0};
  for (const m of modules) {
    assert(m.samples >= 2 && m.peakThreads <= pthreadPoolSize && m.peakMemory <= 536870912, 'OPFS kernel resource bounds');
    assert(m.returnedWorkers === pthreadPoolSize && m.runningWorkers === 0 && m.droppedLogs === 0, 'OPFS healthy Worker drain');
    assert(Object.values(m.resources).every(n => n === 0), 'OPFS native resource release');
    assert(m.peakQueuedBytes <= limits.workerQueueBytes && m.peakPending <= limits.maxPendingCommands &&
      m.peakHostQueuedBytes <= limits.connectionQueueBytes * limits.maxConnections, 'OPFS bridge queue bounds');
    assert(m.ioStackBytes === 2097152, 'Measured dedicated I/O stack');
    let classes = 0;
    for (const line of m.output) if (line.includes('POSTGAMMA_STACK ')) {
      const row = Object.fromEntries(line.split('POSTGAMMA_STACK ')[1].split(' ').map((s: string) => s.split('=')));
      for (const key of ['observation_failures', 'depth_validation_failures', 'accounting_failures', 'active_reservation_bytes'])
        assert(row[key] === '0', 'OPFS kernel stack accounting: ' + key);
      stacks[row.class] = Math.max(stacks[row.class], Number(row.peak_reservation_bytes)); classes++;
    }
    assert(classes === 3, 'All kernel stack classes measured');
  }
  const closed = events.filter(e => e.kind === 'storage-control' && e.closed && !e.failed);
  assert(modules.length > 0 && closed.length > 0, 'OPFS close measurements are present');
  for (const e of closed) assert(e.handles === 0 && e.virtualOpens === 0 && e.metrics.peakHandles <= 63 &&
    e.metrics.peakJournalBytes <= 16777216 && e.metrics.journalGeneration > 0 &&
    e.metrics.flushes > 0 && e.metrics.directoryFlushes > 0, 'OPFS actual barriers and bounded physical handles');
  const stackBound = Object.values(stacks).reduce((a, b) => a + b, 0) + 2 * 4194304 + 2097152;
  assert(stackBound <= 85983232, 'OPFS stacks fit the role reservations');
  return {scope: {browser: 'chromium', storage: 'opfs'}, healthyModules: modules.length,
    peakLinearMemory: Math.max(...modules.map(m => m.peakMemory)), peakPthreads: Math.max(...modules.map(m => m.peakThreads)),
    stackClassPeaks: stacks, stackReservationUpperBound: stackBound, ioStackBytes: 2097152,
    peakDataHandles: Math.max(...closed.map(e => e.metrics.peakHandles)), journalHandles: 2,
    totalHandleLimit: 65, peakJournalBytes: Math.max(...closed.map(e => e.metrics.peakJournalBytes)),
    namespaceJournalLimit: 16777216, namespaceNodeLimit: 16384,
    healthyNativeAndStorageResourcesReleased: true, outsideLinearMemoryRssMeasured: false};
}
