/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {OperationControl} from './control.js';
import type {ControlOptions} from './control.js';
interface Job {
  action?: (signal: AbortSignal) => Promise<unknown>;
  control: OperationControl;
  resolve: (value: unknown) => void;
  reject: (error: unknown) => void;
  finish: () => void;
}
/** Removable pending jobs keep aborted large queries out of the retained queue. */
export class SerialQueue {
  private pending = new Set<Job>();
  private active?: Job;
  private closed = false;
  private closeReason: unknown;
  private waiters: (() => void)[] = [];
  private parent?: AbortSignal;
  constructor(parent?: AbortSignal) {this.parent = parent;}
  get size() {return this.pending.size + Number(!!this.active);}
  run<T>(action: (signal: AbortSignal) => Promise<T>, options: ControlOptions = {}, onFinish: () => void = () => {}): Promise<T> {
    if (this.closed) {onFinish(); return Promise.reject(this.closeReason);}
    let control: OperationControl;
    try {control = new OperationControl(options, this.parent); control.check();}
    catch (error) {control!?.dispose(); onFinish(); return Promise.reject(error);}
    return new Promise<T>((resolve, reject) => {
      let finished = false;
      const job: Job = {action, control, resolve: value => resolve(value as T), reject, finish: () => {
        if (finished) return; finished = true;
        control.signal.removeEventListener('abort', abort); control.dispose(); job.action = undefined; onFinish();
      }};
      const abort = () => {
        if (this.pending.delete(job)) {job.finish(); reject(control.signal.reason); this.notifyIdle();}
        // An active operation settles only after its native request is retired.
      };
      control.signal.addEventListener('abort', abort, {once: true});
      this.pending.add(job);
      if (control.signal.aborted) abort();
      queueMicrotask(() => this.dispatch());
    });
  }
  idle(): Promise<void> {return this.size ? new Promise(resolve => this.waiters.push(resolve)) : Promise.resolve();}
  stop(reason: unknown) {
    this.closed = true; this.closeReason = reason;
    for (const job of [...this.pending]) job.control.abort(reason);
    this.active?.control.abort(reason);
  }
  private notifyIdle() {if (!this.size) for (const resolve of this.waiters.splice(0)) resolve();}
  private dispatch() {
    if (this.active) return;
    const job = this.pending.values().next().value as Job | undefined;
    if (!job) {this.notifyIdle(); return;}
    this.pending.delete(job); this.active = job;
    const complete = () => {job.finish(); this.active = undefined; this.dispatch();};
    Promise.resolve().then(() => {job.control.check(); return job.action!(job.control.signal);})
      .then(value => {complete(); job.resolve(value);}, error => {complete(); job.reject(error);});
  }
}
