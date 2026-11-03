/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import createProbe from './platform.mjs';

const output = [];
let randomCalls = 0, randomBytes = 0;
const originalRandom = crypto.getRandomValues.bind(crypto);
crypto.getRandomValues = (view) => {
  randomCalls++;
  randomBytes += view.byteLength;
  return originalRandom(view);
};

try {
  const module = await createProbe({
    print: (text) => output.push(text),
    printErr: (text) => output.push(text),
    onAbort: (reason) => postMessage({kind: 'fatal', error: String(reason), output}),
  });
  const decode = (pointer) => JSON.parse(module.UTF8ToString(pointer));
  onmessage = ({data: {id, command, round}}) => {
    try {
      let result;
      switch (command) {
        case 'sync': {
          const beforeCalls = randomCalls, beforeBytes = randomBytes;
          result = decode(module._probe_sync());
          const input = 0x20000000000001n, signedInput = -9007199254741111n;
          const unsignedRaw = module._probe_u64(input), signed = module._probe_i64(signedInput);
          const unsigned = BigInt.asUintN(64, unsignedRaw);
          result.bigInt = {
            input: String(input), unsignedRaw: String(unsignedRaw),
            unsigned: String(unsigned), signed: String(signed),
            passed: unsigned === (input ^ 0xfedcba9876543210n) && signed === signedInput + 17n,
          };
          result.entropy.provider = 'Web Crypto getRandomValues';
          result.entropy.providerCalls = randomCalls - beforeCalls;
          result.entropy.providerBytes = randomBytes - beforeBytes;
          break;
        }
        case 'start': result = {rc: module._probe_threads_start(round)}; break;
        case 'state': {
          const state = module._probe_threads_state();
          result = {ready: state >>> 8, done: state & 255};
          break;
        }
        case 'release': module._probe_threads_release(); result = {}; break;
        case 'finish': result = decode(module._probe_threads_finish()); break;
        default: throw new Error(`Unknown probe command: ${command}`);
      }
      postMessage({id, result, output: [...output]});
    } catch (error) {
      postMessage({id, error: String(error.stack || error), output});
    }
  };
  postMessage({kind: 'ready', isolated: crossOriginIsolated,
    worker: typeof document === 'undefined', sharedMemory: module.HEAPU8.buffer instanceof SharedArrayBuffer});
} catch (error) {
  postMessage({kind: 'fatal', error: String(error.stack || error), output});
}
