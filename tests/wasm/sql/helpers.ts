/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {DatabaseError} from '../src/api/database.js';
import type {Result} from '../src/api/database.js';
export function assert(value: unknown, message: string): asserts value {
  if (!value) throw new Error(message);
}
export function scalar(results: Result[]) {return results.find(r => r.rows.length)?.rows[0][0];}
export async function errorState(action: () => Promise<unknown>, sqlstate: string) {
  try {await action();} catch (error) {
    assert(error instanceof DatabaseError && error.sqlstate === sqlstate,
      `Expected ${sqlstate}, got ${String(error)} (${(error as DatabaseError).sqlstate})`); return error;
  }
  throw new Error(`Expected SQLSTATE ${sqlstate}`);
}
