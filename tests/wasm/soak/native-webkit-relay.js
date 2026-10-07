/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
const nativeWorker = Worker, controls = new Map(), pthreads = new Set();
let created = 0, terminated = 0, pthreadCreated = 0, pthreadTerminated = 0;
let nativeOwnerPosts = 0;
const ownership = () => ({active: [...controls.keys()], activePthreads: [...pthreads],
  created, terminated, pthreadCreated, pthreadTerminated, nativeOwnerPosts});
window.Worker = class extends nativeWorker {
  constructor(source, options) {
    const url = new URL(source, location.href);
    const id = /\/src\/worker\/sql\.js$/.test(url.pathname) ? String(++created) : undefined;
    if (id) url.hash = new URLSearchParams({soakControl: id}).toString();
    const pthreadId = new URLSearchParams(url.hash.slice(1)).get('postgammaPthread');
    const target = id ? new URL('/__control-probe.js?source=' + encodeURIComponent(url.pathname + url.hash), location.href) : url;
    super(target, options); this.controlId = id; this.pthreadId = pthreadId;
    if (id) controls.set(id, this);
    if (pthreadId) {pthreads.add(pthreadId); pthreadCreated++;}
  }
  terminate() {
    super.terminate();
    if (this.controlId && controls.delete(this.controlId)) terminated++;
    if (this.pthreadId && pthreads.delete(this.pthreadId)) pthreadTerminated++;
  }
  postMessage(...args) {nativeOwnerPosts++; return super.postMessage(...args);}
};
const socket = new WebSocket(new URL('/__soak', location.href).href.replace(/^http/, 'ws'));
let sequence = 0, lastSample = 0;
const callbacks = new Map();
const ready = new Promise((resolve, reject) => {
  socket.addEventListener('open', resolve, {once: true});
  socket.addEventListener('error', () => reject(new Error('Soak observer connection failed')), {once: true});
});
socket.addEventListener('message', ({data}) => {
  const message = JSON.parse(data), entry = callbacks.get(message.sequence);
  if (!entry) throw new Error('Unknown soak observer acknowledgement');
  callbacks.delete(message.sequence);
  if (message.error) entry.reject(new Error(message.error)); else entry.resolve();
});
socket.addEventListener('close', () => {
  for (const entry of callbacks.values()) entry.reject(new Error('Soak observer disconnected'));
  callbacks.clear();
});
async function snapshot() {
  if (controls.size !== 1 || pthreads.size !== 13) throw new Error('Incomplete live Worker ownership');
  const worker = controls.values().next().value, channel = new MessageChannel();
  try {
    return await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('Control snapshot deadline')), 2000);
      channel.port1.onmessage = ({data}) => {
        clearTimeout(timer);
        if (data.error) reject(new Error(data.error)); else resolve(data.snapshot);
      };
      worker.postMessage({postgammaSoakSnapshot: true}, [channel.port2]);
    });
  } finally {channel.port1.close();channel.port2.close();}
}
async function emit(event) {
  await ready;
  const now = performance.now();
  const sampled = ['ready', 'round', 'reopen'].includes(event.kind) &&
    (now - lastSample > 10000 || event.kind === 'ready' || event.kind === 'round' && event.iteration % window.soakReopenEvery === 0);
  if (event.kind === 'ready') window.soakReopenEvery = event.reopenEvery;
  const controlSnapshot = sampled ? await snapshot() : undefined;
  if (sampled) lastSample = performance.now();
  const id = ++sequence;
  return new Promise((resolve, reject) => {
    callbacks.set(id, {resolve, reject});
    socket.send(JSON.stringify({sequence: id, event: {...event, ownership: ownership(), controlSnapshot}}));
  });
}
window.soakEvent = emit;
addEventListener('error', event => {void emit({kind: 'page-error', error: event.message});});
addEventListener('unhandledrejection', event => {void emit({kind: 'page-error', error: String(event.reason)});});
const timer = setInterval(async () => {
  if (!window.probeResult) return;
  clearInterval(timer);
  await emit({kind: 'result', result: window.probeResult});
}, 100);
