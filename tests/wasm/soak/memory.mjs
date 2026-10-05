/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {appendFileSync, mkdirSync, writeFileSync} from 'node:fs';
import {dirname, basename, join} from 'node:path';
import {bounded} from '../browser/diagnostics.mjs';

// Chromium-only attribution. Never used to qualify a resource gate: attaching
// profilers and taking snapshots can affect collection and object lifetimes.
export class MemoryDiagnostics {
  static async open(context, page, output) {
    const value = new MemoryDiagnostics();
    value.page = page;
    value.directory = join(dirname(output), basename(output, '.json') + '.memory');
    mkdirSync(value.directory, {recursive: true});
    value.root = await context.browser().newBrowserCDPSession();
    value.owner = await context.newCDPSession(page);
    await value.root.send('Tracing.start', {transferMode: 'ReturnAsStream', traceConfig: {
      recordMode: 'recordAsMuchAsPossible', excludedCategories: ['*'],
      includedCategories: ['disabled-by-default-memory-infra']}});
    value.sessions = new Map(); value.requests = new Map(); value.sequence = 0;
    value.root.on('Target.receivedMessageFromTarget', ({sessionId, message}) => {
      const packet = JSON.parse(message), session = value.sessions.get(sessionId);
      if (packet.id) {
        const pending = value.requests.get(sessionId + ':' + packet.id);
        if (!pending) return;
        value.requests.delete(sessionId + ':' + packet.id); clearTimeout(pending.timer);
        if (packet.error) pending.reject(new Error(JSON.stringify(packet.error)));
        else pending.resolve(packet.result);
      } else session?.event?.(packet.method, packet.params);
    });
    return value;
  }
  async command(sessionId, method, params = {}) {
    const id = ++this.sequence, key = sessionId + ':' + id;
    const response = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {this.requests.delete(key); reject(new Error('CDP timeout: ' + method));}, 30000);
      this.requests.set(key, {resolve, reject, timer});
    });
    try {
      await this.root.send('Target.sendMessageToTarget', {sessionId, message: JSON.stringify({id, method, params})});
    } catch (error) {
      const pending = this.requests.get(key);
      if (pending) {this.requests.delete(key); clearTimeout(pending.timer); pending.reject(error);}
    }
    return response;
  }
  async evaluate(send, expression) {
    const result = await send('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true});
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  }
  async sample(iteration) {
    if (!this.nativeSampling) {
      await this.owner.send('Memory.startSampling', {samplingInterval: 32768});
      this.nativeSampling = true;
    }
    const {targetInfos} = await this.root.send('Target.getTargets');
    const ownership = await this.page.evaluate(() => window.soakWorkerOwnership?.() ?? null);
    const targets = targetInfos.filter(target => {
      if (target.type !== 'worker') return false;
      if (!ownership) return target.url.endsWith('/opfs-memory.js');
      // Debug targets can survive Worker.terminate(). Only the current group
      // can service commands; never wait for a retired storage/control realm.
      const identity = new URLSearchParams(new URL(target.url).hash.slice(1));
      return ownership.active.includes(identity.get('soakControl')) ||
        ownership.activePthreads.includes(identity.get('postgammaPthread'));
    });
    targets.sort((a, b) => Number(/\/src\/worker\/sql\.js/.test(b.url)) - Number(/\/src\/worker\/sql\.js/.test(a.url)));
    const live = new Set(targets.map(target => target.targetId));
    for (const [id, session] of this.sessions) if (!live.has(session.targetId)) {
      this.sessions.delete(id);
      try {await this.root.send('Target.detachFromTarget', {sessionId: id});} catch {}
    }
    const realms = [{role: 'window', heap: await this.owner.send('Runtime.getHeapUsage'),
      isolateId: (await this.owner.send('Runtime.getIsolateId')).id}];
    let storageBrokerId;
    for (const target of targets) {
      const control = /\/src\/worker\/sql\.js/.test(target.url);
      const storage = target.url.endsWith('/opfs-memory.js') || storageBrokerId &&
        new URLSearchParams(new URL(target.url).hash.slice(1)).get('postgammaPthread')?.endsWith(':' + storageBrokerId);
      if (!control && !storage) {
        realms.push({role: 'pthread', targetId: target.targetId, url: target.url,
          inspection: 'Native pthread may be parked in futex; no JS evaluation. See memory-infra trace.'});
        continue;
      }
      let session = [...this.sessions.values()].find(value => value.targetId === target.targetId);
      if (!session) {
        const {sessionId} = await this.root.send('Target.attachToTarget', {targetId: target.targetId, flatten: false});
        session = {sessionId, targetId: target.targetId}; this.sessions.set(sessionId, session);
      }
      const send = (method, params) => this.command(session.sessionId, method, params);
      const state = await this.evaluate(send, `({control: typeof postgammaMemoryDiagnostic === 'function',
        storage: globalThis.postgammaStore?.diagnostics() ?? null})`);
      const row = {targetId: target.targetId, url: target.url,
        role: state.control ? 'control' : state.storage ? 'storage' : 'pthread',
        heap: await send('Runtime.getHeapUsage'), isolateId: (await send('Runtime.getIsolateId')).id, storage: state.storage};
      if (state.control) {
        row.native = await this.evaluate(send, 'postgammaMemoryDiagnostic()');
        storageBrokerId = row.native.storageBrokerId;
      }
      // Allocation sampling on the long-lived storage owner, with collected
      // allocations included, separates allocation churn from surviving objects.
      if (state.storage && !session.profiling) {
        await send('HeapProfiler.startSampling', {samplingInterval: 32768,
          includeObjectsCollectedByMajorGC: true, includeObjectsCollectedByMinorGC: true});
        session.profiling = true;
      }
      if (session.profiling) {
        const profile = await send('HeapProfiler.getSamplingProfile');
        const path = join(this.directory, `allocation-${iteration}-${target.targetId}.json`);
        writeFileSync(path, JSON.stringify(profile)); row.allocationProfile = path;
      }
      if (state.storage && (iteration === 0 || iteration === 2000 || iteration === 10000)) {
        row.snapshot = await this.snapshot(session, 'storage-' + iteration);
        row.heapAfterSnapshot = await send('Runtime.getHeapUsage');
      }
      realms.push(row);
    }
    const paths = {};
    for (const [name, method] of [['renderer', 'Memory.getSamplingProfile'], ['browser', 'Memory.getBrowserSamplingProfile']]) {
      const profile = await this.owner.send(method);
      const path = join(this.directory, `native-${name}-${iteration}.json`);
      writeFileSync(path, JSON.stringify(profile)); paths[name] = path;
    }
    const contexts = await this.page.evaluate(() => window.soakMemoryContexts?.() ?? null);
    const memoryDump = iteration % 1000 === 0 ? await bounded(() => this.root.send('Tracing.requestMemoryDump',
      {deterministic: false, levelOfDetail: 'detailed'}), 30000) : null;
    return {iteration, realms, memoryContexts: contexts, nativeProfiles: paths, memoryDump};
  }
  async snapshot(session, label) {
    const path = join(this.directory, label + '.heapsnapshot');
    writeFileSync(path, '');
    session.event = (method, params) => {
      if (method === 'HeapProfiler.addHeapSnapshotChunk') appendFileSync(path, params.chunk);
    };
    try {await this.command(session.sessionId, 'HeapProfiler.takeHeapSnapshot', {reportProgress: false});}
    finally {delete session.event;}
    return path;
  }
  async finish() {
    if (this.nativeSampling) await this.owner.send('Memory.stopSampling');
    const completed = new Promise(resolve => this.root.once('Tracing.tracingComplete', resolve));
    await this.root.send('Tracing.end');
    const {stream} = await bounded(() => completed, 30000), path = join(this.directory, 'memory-infra.json');
    writeFileSync(path, '');
    for (;;) {
      const chunk = await this.root.send('IO.read', {handle: stream});
      appendFileSync(path, chunk.base64Encoded ? Buffer.from(chunk.data, 'base64') : chunk.data);
      if (chunk.eof) break;
    }
    await this.root.send('IO.close', {handle: stream});
    await this.owner.detach();
    await this.root.detach();
    return {memoryTrace: path, scope: 'Diagnostic only; JS profilers may alter GC. Shared backing memory must not be summed across realms.'};
  }
}
