/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {rollupMemory, smapsMemory} from './processes.mjs';
const mapping = (id, {rss, pss, anonymous, swap = 0, swapPss = 0}) =>
  `${id.toString(16)}000-${id.toString(16)}fff rw-p 00000000 08:01 ${id} /lib/example.so\n` +
  `Rss: ${rss} kB\nPss: ${pss} kB\nAnonymous: ${anonymous} kB\nSwap: ${swap} kB\nSwapPss: ${swapPss} kB\n`;
const exact = rollupMemory('Rss: 100 kB\nPss: 80 kB\nSwapPss: 4 kB\nPss_Anon: 40 kB\nPss_Shmem: 24 kB\nPss_File: 16 kB\n');
assert.equal(exact.accountedMemoryLowerBytes, 68 * 1024);
assert.equal(exact.accountedMemoryLowerBytes, exact.accountedMemoryUpperBytes);
assert.throws(() => rollupMemory('Rss: 100 kB\nPss: 80 kB\nSwapPss: 4 kB\n'), /Pss_Anon/);
assert.throws(() => smapsMemory(''), /mappings/);
assert.throws(() => smapsMemory(mapping(1, {rss: 4, pss: 8, anonymous: 4})), /Consistent/);
assert.throws(() => smapsMemory(mapping(1, {rss: 4, pss: 4, anonymous: 4}).replace('SwapPss: 0 kB\n', '')), /SwapPss/);
assert.throws(() => smapsMemory(mapping(1, {rss: 4, pss: 4, anonymous: 4}) + 'Pss: 4 kB\n'), /Exactly one/);
// A file name cannot identify its COW pages. Resident Anonymous is also not PSS.
const cow = smapsMemory(mapping(1, {rss: 100, pss: 30, anonymous: 40}), () => 'file');
assert.equal(cow.anonymousPssBytes, null);
assert(cow.accountedMemoryLowerBytes <= 20 * 1024 && cow.accountedMemoryUpperBytes >= 20 * 1024);
const unknown = smapsMemory(mapping(1, {rss: 100, pss: 60, anonymous: 0}));
assert(unknown.accountedMemoryUpperBytes >= 60 * 1024, 'Unknown backing may be shmem');
// Page-level oracle: independently apportion resident and swapped pages with
// different sharing counts. The old-kernel interval must contain the three
// separately rounded rollup categories, including mixed file/COW mappings.
for (const share of [1, 2, 3, 7, 16, 1024]) {
  const rows = [], kinds = new Map(); let anon = 0, shmem = 0, swapped = 0;
  for (let id = 1; id <= 20; id++) {
    const kind = ['file', 'shmem', 'unknown'][id % 3]; kinds.set(String(id), kind);
    const anonPages = id % 5, otherPages = id % 7, swapPages = id % 3;
    const anonShare = share, otherShare = id % 2 ? 1 : share;
    const anonPss = anonPages * 4096 / anonShare, otherPss = otherPages * 4096 / otherShare;
    anon += anonPss;
    if (kind !== 'file') shmem += otherPss;
    swapped += swapPages * 4096 / share;
    rows.push(mapping(id, {rss: (anonPages + otherPages) * 4,
      pss: Math.floor((anonPss + otherPss) / 1024), anonymous: anonPages * 4,
      swap: swapPages * 4, swapPss: Math.floor(swapPages * 4 / share)}));
  }
  const total = (Math.floor(anon / 1024) + Math.floor(shmem / 1024) + Math.floor(swapped / 1024)) * 1024;
  const measured = smapsMemory(rows.join(''), ({inode}) => kinds.get(inode));
  assert(measured.accountedMemoryLowerBytes <= total && total <= measured.accountedMemoryUpperBytes,
    `Category-weighted page oracle lies within bounds at sharing count ${share}`);
}
console.log('Process memory: rollup, old-kernel bounds, COW, sharing, rounding and invalid inputs passed');
