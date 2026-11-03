/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
export interface ControlOptions {signal?: AbortSignal; timeoutMs?: number}

/** Owns listeners and a deadline for one admitted operation, including queue time. */
export class OperationControl {
  private controller = new AbortController();
  private cleanups: (() => void)[] = [];
  private timer?: ReturnType<typeof setTimeout>;
  get signal(): AbortSignal {return this.controller.signal;}
  constructor(options: ControlOptions = {}, parent?: AbortSignal) {
    if (options.timeoutMs !== undefined && (!Number.isInteger(options.timeoutMs) || options.timeoutMs < 0 || options.timeoutMs > 2147483647))
      throw new RangeError('timeoutMs must be an integer from 0 to 2147483647');
    for (const signal of new Set([parent, options.signal])) if (signal) {
      const abort = () => this.abort(signal.reason);
      if (signal.aborted) abort();
      else {signal.addEventListener('abort', abort, {once: true}); this.cleanups.push(() => signal.removeEventListener('abort', abort));}
    }
    if (!this.signal.aborted && options.timeoutMs !== undefined) {
      const expire = () => this.abort(new DOMException('Database operation timed out', 'TimeoutError'));
      if (options.timeoutMs === 0) expire();
      else this.timer = setTimeout(expire, options.timeoutMs);
    }
  }
  abort(reason?: unknown) {this.controller.abort(reason === undefined ? new DOMException('Database operation aborted', 'AbortError') : reason);}
  check() {if (this.signal.aborted) throw this.signal.reason;}
  dispose() {clearTimeout(this.timer); for (const cleanup of this.cleanups.splice(0)) cleanup();}
}

/** Observe cancellation of application callbacks without leaving a rejected promise unhandled. */
export function abortable<T>(work: Promise<T>, signal: AbortSignal): Promise<T> {
  return new Promise((resolve, reject) => {
    const abort = () => {signal.removeEventListener('abort', abort); reject(signal.reason);};
    signal.addEventListener('abort', abort, {once: true});
    work.then(resolve, reject).finally(() => signal.removeEventListener('abort', abort));
    if (signal.aborted) {signal.removeEventListener('abort', abort); reject(signal.reason);}
  });
}
