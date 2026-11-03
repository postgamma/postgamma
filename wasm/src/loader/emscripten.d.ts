/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
declare module '*.mjs' {
  const createKernel: (options: Record<string, unknown>) => Promise<import('../bindings/kernel.js').KernelModule>;
  export default createKernel;
}
