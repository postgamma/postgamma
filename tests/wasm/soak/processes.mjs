/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {readFileSync, readdirSync} from 'node:fs';
import assert from 'node:assert/strict';

export function processes() {
  const rows = new Map();
  for (const id of readdirSync('/proc').filter(name => /^\d+$/.test(name))) {
    try {
      const status = readFileSync(`/proc/${id}/status`, 'utf8');
      const field = name => Number(status.match(new RegExp('^' + name + ':\\s+(\\d+)', 'm'))?.[1] || 0);
      rows.set(Number(id), {pid: Number(id), parent: field('PPid'), rssBytes: field('VmRSS') * 1024, threads: field('Threads')});
    } catch { /* A process can exit between the directory and status reads. */ }
  }
  const selected = new Set([process.pid]);
  for (let changed = true; changed;) {
    changed = false;
    for (const row of rows.values()) if (selected.has(row.parent) && !selected.has(row.pid)) {selected.add(row.pid); changed = true;}
  }
  const result = {processes: 0, rssBytes: 0, pssBytes: 0, anonymousPssBytes: 0,
    sharedMemoryPssBytes: 0, filePssBytes: 0, swapPssBytes: 0, threads: 0, fileDescriptors: 0, processRows: []};
  for (const id of selected) {
    if (id === process.pid) continue; // The Node test driver is recorded separately.
    const row = rows.get(id); if (!row) continue;
    try {
      const rollup = readFileSync(`/proc/${id}/smaps_rollup`, 'utf8');
      const bytes = name => {
        const value = rollup.match(new RegExp('^' + name + ':\\s+(\\d+)', 'm'))?.[1];
        assert(value !== undefined, 'Missing browser process ' + name);
        return Number(value) * 1024;
      };
      const descriptors = readdirSync(`/proc/${id}/fd`).length;
      result.processRows.push({...row, command: readFileSync(`/proc/${id}/cmdline`, 'utf8').split('\0').filter(Boolean),
        pssBytes: bytes('Pss'), anonymousPssBytes: bytes('Pss_Anon'), sharedMemoryPssBytes: bytes('Pss_Shmem'),
        swapPssBytes: bytes('SwapPss'), filePssBytes: bytes('Pss_File'), fileDescriptors: descriptors});
      result.pssBytes += bytes('Pss'); result.swapPssBytes += bytes('SwapPss');
      result.anonymousPssBytes += bytes('Pss_Anon');
      result.sharedMemoryPssBytes += bytes('Pss_Shmem'); result.filePssBytes += bytes('Pss_File');
      result.processes++; result.rssBytes += row.rssBytes; result.threads += row.threads;
      result.fileDescriptors += descriptors;
    } catch (error) {
      if (['ENOENT', 'ESRCH'].includes(error.code)) continue; // Exited during sampling.
      throw error;
    }
  }
  assert(result.processes >= 1 && result.rssBytes > 0, 'Browser process accounting is available');
  return result;
}
