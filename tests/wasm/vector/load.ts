/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Database} from '../src/api/database.js';
import {assert, scalar} from '../tests/helpers.js';

export async function loadVector(db: Database, checks: string[]) {
  const c = await db.connect('vector-load');
  assert(scalar(await c.query("SELECT default_version FROM pg_available_extensions WHERE name = 'vector'")) === '0.8.6',
    'Matching pgvector control and SQL resources are available');
  await c.query('CREATE EXTENSION vector');
  assert(scalar(await c.query("SELECT extversion FROM pg_extension WHERE extname = 'vector'")) === '0.8.6',
    'Pinned static pgvector initializes through the extension SDK');
  assert(scalar(await c.query("SELECT '[1,2,3]'::vector <-> '[4,6,3]'::vector")) === '5', 'Scalar vector distance');
  assert(scalar(await c.query("SELECT '[1,2,3]'::halfvec <-> '[4,6,3]'::halfvec")) === '5', 'Scalar halfvec dispatch');
  assert(scalar(await c.query("SELECT B'1010' <~> B'1001'")) === '2', 'Scalar bit dispatch');
  assert(scalar(await c.query("SELECT '{1:1,3:2}/4'::sparsevec <-> '{1:4,3:6}/4'::sparsevec")) === '5',
    'Scalar sparsevec distance');
  await c.close();
  checks.push('pinned-extension-resources', 'static-extension-registration', 'vector-halfvec-bit-sparsevec-scalar');
}
