/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
export interface KernelModule {
  HEAPU8: Uint8Array;
  PThread: {pthreads: Record<string, unknown>; unusedWorkers: unknown[]; terminateAllThreads(): void};
  _postgamma_wasm_smoke_start(): number;
  _postgamma_wasm_smoke_poll(): number;
}

export async function runKernelSmoke(module: KernelModule): Promise<{
  rounds: number; ticks: number; runningWorkers: number; unusedWorkers: number;
}> {
  const status = module._postgamma_wasm_smoke_start();
  if (status !== 0) throw new Error(`Kernel driver creation failed: ${status}`);
  return new Promise((resolve, reject) => {
    let ticks = 0;
    let joined = 0;
    const poll = () => {
      try {
        ticks++;
        const rounds = joined || module._postgamma_wasm_smoke_poll();
        if (rounds < 0) throw new Error(`Kernel lifecycle failed: ${rounds}`);
        if (rounds === 0) { setTimeout(poll, 1); return; }
        if (rounds !== 3) throw new Error(`Incomplete kernel lifecycle: ${rounds}`);
        joined = rounds;
        const runningWorkers = Object.keys(module.PThread.pthreads).length;
        if (runningWorkers !== 0) { setTimeout(poll, 1); return; }
        resolve({rounds, ticks, runningWorkers,
                 unusedWorkers: module.PThread.unusedWorkers.length});
      } catch (error) { reject(error); }
    };
    poll();
  });
}
