/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {readFileSync, readdirSync, statfsSync} from 'node:fs';
import assert from 'node:assert/strict';

const quantities = ['Rss', 'Pss', 'SwapPss'];
function fields(source, names) {
  return Object.fromEntries(names.map(name => {
    const matches = [...source.matchAll(new RegExp('^' + name + ':\\s+(\\d+) kB$', 'gm'))];
    assert.equal(matches.length, 1, 'Exactly one process memory field ' + name);
    const bytes = Number(matches[0][1]) * 1024;
    assert(Number.isSafeInteger(bytes) && bytes >= 0, 'Valid process memory field ' + name);
    return [name, bytes];
  }));
}

export function rollupMemory(source) {
  const value = fields(source, [...quantities, 'Pss_Anon', 'Pss_Shmem', 'Pss_File']);
  return {rssBytes: value.Rss, pssBytes: value.Pss, swapPssBytes: value.SwapPss,
    anonymousPssBytes: value.Pss_Anon, sharedMemoryPssBytes: value.Pss_Shmem,
    filePssBytes: value.Pss_File, memoryAccounting: 'smaps-rollup',
    accountedMemoryLowerBytes: value.Pss_Anon + value.Pss_Shmem + value.SwapPss,
    accountedMemoryUpperBytes: value.Pss_Anon + value.Pss_Shmem + value.SwapPss};
}

// Older kernels expose resident Anonymous bytes, not weighted anonymous PSS.
// For each mapping, non-anonymous pages contribute at most Rss - Anonymous:
// max(0, Pss - (Rss - Anonymous)) <= anonymous PSS <= min(Pss, Anonymous).
// A verified tmpfs mapping contains only anonymous or shmem pages. Unknown
// backing remains ambiguous. Do not infer page types from filenames: file
// mappings can contain anonymous copy-on-write pages. Include per-VMA kB
// truncation in the upper bound and per-category rounding in the lower bound.
export function smapsMemory(source, backing = () => 'unknown') {
  const mappings = [...source.matchAll(/^([0-9a-f]+-[0-9a-f]+)\s+[rwxps-]{4}\s+[0-9a-f]+\s+([0-9a-f]+:[0-9a-f]+)\s+(\d+)[^\n]*\n/gm)];
  assert(mappings.length, 'Process smaps contains mappings');
  const result = {rssBytes: 0, pssBytes: 0, swapPssBytes: 0,
    anonymousPssBytes: null, sharedMemoryPssBytes: null, filePssBytes: null,
    memoryAccounting: 'smaps-bounds', accountedMemoryLowerBytes: 0,
    accountedMemoryUpperBytes: 0, mappings: mappings.length, unknownBackingMappings: 0};
  for (let index = 0; index < mappings.length; index++) {
    const match = mappings[index], end = mappings[index + 1]?.index ?? source.length;
    const value = fields(source.slice(match.index + match[0].length, end), [...quantities, 'Anonymous', 'Swap']);
    assert(value.Anonymous <= value.Rss && value.Pss <= value.Rss && value.SwapPss <= value.Swap,
      'Consistent resident and swapped page counts');
    const kind = backing({address: match[1], device: match[2], inode: match[3]});
    assert(['file', 'shmem', 'unknown'].includes(kind), 'Known mapping classification');
    const pssUpper = Math.min(value.Rss, value.Pss + 1023);
    const lower = kind === 'shmem' ? value.Pss : Math.max(0, value.Pss - (value.Rss - value.Anonymous));
    const upper = kind === 'file' ? Math.min(pssUpper, value.Anonymous) : pssUpper;
    if (kind === 'unknown' && value.Rss > value.Anonymous) result.unknownBackingMappings++;
    result.rssBytes += value.Rss; result.pssBytes += value.Pss; result.swapPssBytes += value.SwapPss;
    result.accountedMemoryLowerBytes += lower + value.SwapPss;
    result.accountedMemoryUpperBytes += upper + Math.min(value.Swap, value.SwapPss + 1023);
  }
  result.accountedMemoryLowerBytes = Math.max(0, result.accountedMemoryLowerBytes - 3 * 1023);
  return result;
}

function memory(id) {
  try {return rollupMemory(readFileSync(`/proc/${id}/smaps_rollup`, 'utf8'));}
  catch (error) {if (error.code !== 'ENOENT') throw error;}
  const source = readFileSync(`/proc/${id}/smaps`, 'utf8'), kinds = new Map();
  return smapsMemory(source, mapping => {
    const key = mapping.device + ':' + mapping.inode;
    if (mapping.inode === '0') return 'unknown';
    if (!kinds.has(key)) {
      let kind = 'unknown';
      try {kind = statfsSync(`/proc/${id}/map_files/${mapping.address}`).type === 0x01021994 ? 'shmem' : 'file';}
      catch (error) {if (!['ENOENT', 'ESRCH', 'EPERM', 'EACCES'].includes(error.code)) throw error;}
      kinds.set(key, kind);
    }
    return kinds.get(key);
  });
}

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
    sharedMemoryPssBytes: 0, filePssBytes: 0, swapPssBytes: 0, threads: 0, fileDescriptors: 0, processRows: [],
    memoryAccounting: 'smaps-rollup', accountedMemoryLowerBytes: 0, accountedMemoryUpperBytes: 0};
  for (const id of selected) {
    if (id === process.pid) continue; // The Node test driver is recorded separately.
    const row = rows.get(id); if (!row) continue;
    try {
      const measured = memory(id);
      const descriptors = readdirSync(`/proc/${id}/fd`).length;
      result.processRows.push({...row, command: readFileSync(`/proc/${id}/cmdline`, 'utf8').split('\0').filter(Boolean),
        ...measured, fileDescriptors: descriptors});
      for (const name of ['rssBytes', 'pssBytes', 'swapPssBytes', 'accountedMemoryLowerBytes', 'accountedMemoryUpperBytes'])
        result[name] += measured[name];
      for (const name of ['anonymousPssBytes', 'sharedMemoryPssBytes', 'filePssBytes'])
        result[name] = result[name] === null || measured[name] === null ? null : result[name] + measured[name];
      if (measured.memoryAccounting === 'smaps-bounds') result.memoryAccounting = 'smaps-bounds';
      result.processes++; result.threads += row.threads;
      result.fileDescriptors += descriptors;
    } catch (error) {
      if (['ENOENT', 'ESRCH'].includes(error.code)) continue; // Exited during sampling.
      throw error;
    }
  }
  assert(result.processes >= 1 && result.rssBytes > 0, 'Browser process accounting is available');
  return result;
}
