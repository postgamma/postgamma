/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {createVectorDatabase, queryVectors} from '../vector-example.js';
import {assert} from '../tests/helpers.js';

export async function vectorExample(checks: string[], metrics: unknown[]) {
  const name = 'vector-example-' + crypto.randomUUID();
  const first = await createVectorDatabase(name);
  assert((await queryVectors(first, [1, 1, 1]))[0][1] === 'alpha', 'Browser example finds the first document');
  metrics.push(await first.close());
  const reopened = await createVectorDatabase(name);
  assert((await queryVectors(reopened, [3, 3, 3]))[0][1] === 'gamma', 'Browser example queries a reopened persistent database');
  let invalid = false;
  try {await queryVectors(reopened, [1, 2]);} catch {invalid = true;}
  assert(invalid, 'Browser example rejects a vector with the wrong dimension');
  metrics.push(await reopened.close());
  checks.push('example-parameterized-search', 'example-opfs-reopen', 'example-input-validation');
}
