/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import createProbe from './stack-overflow.mjs';

let armed = false;
const output = [];
const stackDiagnostic = /stack overflow/i;
// A pthread abort is forwarded through its owning Worker's error handler.
addEventListener('error', (event) => {
  // libpthread rethrows an ErrorEvent, so the outer message can be only
  // "Uncaught [object ErrorEvent]". Inspect its nested diagnostic explicitly.
  const message = [event.message, event.error?.message, event.error?.error?.message]
    .filter(Boolean).join('\n');
  if (armed && stackDiagnostic.test(message)) {
    event.preventDefault();
    postMessage({kind: 'expected-error', message});
  }
});

try {
  const module = await createProbe({
    print: (text) => output.push(text),
    printErr: (text) => output.push(text),
    onAbort: (reason) => postMessage({kind: 'abort', armed, reason: String(reason), output}),
  });
  const mode = new URL(location.href).searchParams.get('mode');
  armed = true;
  postMessage({kind: 'armed', mode, sharedMemory: module.HEAPU8.buffer instanceof SharedArrayBuffer});
  const rc = module._probe_overflow(mode === 'pthread' ? 1 : 0);
  if (rc !== 0 || mode !== 'pthread')
    postMessage({kind: 'fatal', error: `overflow call unexpectedly returned: ${rc}`, output});
} catch (error) {
  const message = String(error.stack || error);
  postMessage({kind: armed && stackDiagnostic.test(message) ? 'expected-error' : 'fatal', error: message, output});
}
