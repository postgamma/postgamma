/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import test from 'node:test';
import assert from 'node:assert/strict';
import {assess, scenarios} from './deployment-evidence.mjs';

test('missing capability differs from an unexplained runtime failure', () => {
  const baseline = scenarios[0];
  assert.equal(assess(baseline, {status: 'rejected', code: 'ISOLATION_REQUIRED',
    capabilities: {isolated: false}}, [], []), 'unsupported');
  assert.equal(assess(baseline, {status: 'rejected', code: 'RUNTIME_START_FAILED'}, [], []), 'failed');
  assert.equal(assess(baseline, {status: 'rejected', code: 'ISOLATION_REQUIRED'}, [], []), 'failed');
  assert.equal(assess(baseline, {status: 'rejected', code: 'ISOLATION_REQUIRED',
    capabilities: {isolated: true}}, [], []), 'failed');
});

test('asset fault must be the intended rejection before C execution', () => {
  const scenario = scenarios.find((s) => s.name === 'missing-wasm');
  const result = {status: 'rejected', code: 'WASM_LOAD_FAILED', probeStarted: false};
  const request = {url: 'http://localhost/deployment.wasm', status: 404};
  assert.equal(assess(scenario, result, [request], []), 'passed');
  assert.equal(assess(scenario, {...result, probeStarted: true}, [request], []), 'failed');
  assert.equal(assess(scenario, {...result, code: 'WORKER_LOAD_FAILED'}, [request], []), 'failed');
  assert.equal(assess(scenario, result, [request, {url: 'http://localhost/unrelated.mjs', status: 404}], []), 'failed');
  assert.equal(assess(scenario, result, [request], ['unrelated exception']), 'failed');
});

test('a page result alone does not prove working independent pthreads', () => {
  const threads = [1, 2, 3].map((i) => ({done: true, value: i, expected: i, tlsAddress: i * 16}));
  const result = {status: 'passed', isolated: true, worker: true, sharedMemory: true,
    rounds: [1, 2].map(() => ({controlReplies: 3, threads: structuredClone(threads)}))};
  assert.equal(assess(scenarios[0], result, [], []), 'passed');
  result.rounds[1].threads[1].tlsAddress = result.rounds[1].threads[0].tlsAddress;
  assert.equal(assess(scenarios[0], result, [], []), 'failed');
});
