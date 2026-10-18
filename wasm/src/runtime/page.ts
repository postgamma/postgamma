/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
/** A frozen page cannot finish asynchronous shutdown. Stop its owned Workers
 * synchronously; browser handle release and WAL recovery follow asynchronously. */
export class PageLifetime {
  private controller = new AbortController();
  readonly signal = this.controller.signal;
  private remove = () => {};
  constructor(enabled: boolean, reason: Error, discard: (reason: Error) => void) {
    if (!enabled || typeof window === 'undefined') return;
    const hidden = (event: PageTransitionEvent) => {
      if (!event.persisted) return;
      this.dispose();
      this.controller.abort(reason);
      discard(reason);
    };
    window.addEventListener('pagehide', hidden, true);
    this.remove = () => window.removeEventListener('pagehide', hidden, true);
  }
  dispose() {this.remove(); this.remove = () => {};}
}
