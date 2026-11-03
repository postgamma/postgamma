/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import test from 'node:test';
import {classifyPageErrors} from './evidence.mjs';

const url = 'http://127.0.0.1:8000/platform.html';
const diagnostic = 'stack overflow (Attempt to set SP to 0x00001000, with stack limits [0x00002000 - 0x00012000])';
const item = {mode: 'pthread', status: 'passed', diagnostic,
  bounds: {base: 73728, end: 8192, size: 65536},
  events: [{kind: 'armed', sharedMemory: true}, {kind: 'abort', armed: true, reason: diagnostic}]};
const error = {name: 'RuntimeError', message: `Aborted(${diagnostic})`,
  stack: 'at http://127.0.0.1:8000/stack-overflow.wasm:wasm-function[29]:0x123'};

test('requires the exact abort and an armed completed isolated overflow case', () => {
  assert.equal(classifyPageErrors([error], {overflow: [item]}, url).expected.length, 1);
  for (const patch of [{status: 'failed'}, {mode: 'control'}, {events: []}, {bounds: {size: 65536}}]) {
    assert.equal(classifyPageErrors([error], {overflow: [{...item, ...patch}]}, url).unexpected.length, 1);
  }
});

test('does not accept another module, origin, diagnostic, or generic stack error', () => {
  for (const patch of [
    {stack: 'at http://127.0.0.1:8000/platform.wasm:wasm-function[29]'},
    {stack: 'at http://127.0.0.1:9000/stack-overflow.wasm:wasm-function[29]'},
    {message: 'stack overflow'}, {name: 'Error'},
  ]) {
    assert.equal(classifyPageErrors([{...error, ...patch}], {overflow: [item]}, url).unexpected.length, 1);
  }
});

test('retains additional errors even when one expected abort occurred', () => {
  const result = classifyPageErrors([error, error, {name: 'Error', message: 'unrelated'}], {overflow: [item]}, url);
  assert.equal(result.expected.length, 1);
  assert.equal(result.unexpected.length, 2);
});

test('ordinary probes have no expected page errors', () => {
  assert.equal(classifyPageErrors([error], {}, url).unexpected.length, 1);
  assert.equal(classifyPageErrors([error], null, url).unexpected.length, 1);
});
