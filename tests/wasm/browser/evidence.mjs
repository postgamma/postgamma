/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */

export function classifyPageErrors(errors, probe, url) {
  const expected = [], unexpected = [];
  const used = new Set();
  for (const error of errors) {
    const matched = (probe?.overflow || []).find((item) =>
      !used.has(item) && item.mode === 'pthread' && item.status === 'passed' &&
      item.bounds?.size === 65536 && item.bounds.base - item.bounds.end === 65536 &&
      /^stack overflow \(Attempt to set SP to 0x[0-9a-f]+, with stack limits /i.test(item.diagnostic) &&
      item.events?.some((event) => event.kind === 'armed' && event.sharedMemory) &&
      item.events?.some((event) => event.kind === 'abort' && event.armed && event.reason === item.diagnostic) &&
      error.name === 'RuntimeError' && error.message === `Aborted(${item.diagnostic})` &&
      error.stack.includes(new URL('stack-overflow.wasm', url).href));
    if (matched) {
      used.add(matched);
      expected.push(error);
    } else {
      unexpected.push(error);
    }
  }
  return {expected, unexpected};
}
