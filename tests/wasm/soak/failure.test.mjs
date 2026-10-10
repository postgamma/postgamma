/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {mkdtempSync, writeFileSync, readFileSync, rmSync} from 'node:fs';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
import {gzipSync} from 'node:zlib';
import {createHash} from 'node:crypto';
import {firefoxFailureMemory} from './failure.mjs';

const temporary = mkdtempSync(join(tmpdir(), 'pgm-failure-memory-'));
let signals = 0;
const options = {pid: 999999, temporary, output: join(temporary, 'saved', 'memory.json.gz'), timeoutMs: 0,
  readStatus: () => 'SigCgt:\t0000000200000000\n', signal: () => {signals++;}};
try {
  await assert.rejects(firefoxFailureMemory({status: 'passed'}, options), /only after failure/);
  assert.equal(signals, 0, 'A passing gate cannot acquire memory-report overhead');

  const missing = {status: 'failed', error: 'original gate failure'};
  await firefoxFailureMemory(missing, {...options, readStatus: () => 'SigCgt:\t0000000000000000\n'});
  assert.equal(signals, 0, 'Do not signal a process without the pinned native handler');
  assert.equal(missing.failureMemory.status, 'not-run');
  assert.equal(missing.error, 'original gate failure');

  const bytes = gzipSync(JSON.stringify({reports: [{process: 'Main Process', path: 'heap-allocated', amount: 123}]}));
  const collected = {status: 'failed', error: 'original gate failure'};
  await firefoxFailureMemory(collected, {...options, signal: () => {
    signals++;writeFileSync(join(temporary, 'unified-memory-report-complete.json.gz'), bytes);
  }});
  assert.equal(collected.status, 'failed', 'Diagnostic success cannot change qualification failure');
  assert.equal(collected.failureMemory.status, 'collected');
  assert.equal(collected.failureMemory.forcedGC, false);
  assert.equal(collected.failureMemory.productionAcceptance, false);
  assert.equal(collected.failureMemory.signal, 34);
  assert.equal(collected.failureMemory.sha256, createHash('sha256').update(bytes).digest('hex'));
  assert.deepEqual(readFileSync(options.output), bytes);

  const incomplete = {status: 'failed', error: 'original gate failure'};
  await firefoxFailureMemory(incomplete, {...options, signal: () => {
    signals++;writeFileSync(join(temporary, 'unified-memory-report-incomplete.json.gz'), Buffer.from('partial'));
  }});
  assert.equal(incomplete.failureMemory.status, 'failed');
  assert.match(incomplete.failureMemory.error, /deadline/);
  assert.equal(incomplete.error, 'original gate failure');

  const exited = {status: 'failed', error: 'original gate failure'};
  const previous = signals;
  await firefoxFailureMemory(exited, {...options, readStatus: () => {throw new Error('process exited');}});
  assert.equal(signals, previous);
  assert.match(exited.failureMemory.error, /process exited/);
  assert.equal(exited.status, 'failed');
  console.log('Failure diagnostics: passing gates are untouched; failed gates retain native reports and original errors');
} finally {rmSync(temporary, {recursive: true, force: true});}
