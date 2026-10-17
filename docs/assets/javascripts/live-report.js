/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
(function () {
  "use strict";
  const limit = (value, size = 8192) => String(value ?? "").slice(0, size);
  const elapsed = start => Math.round(performance.now() - start);
  function url(value) {
    try {const parsed = new URL(value, location.href); return parsed.origin + parsed.pathname;}
    catch (_) {return limit(value, 512);}
  }
  function errorInfo(error, depth = 0) {
    const result = {name: limit(error?.name || "Error", 128), message: limit(error?.message ?? error),
      stack: limit(error?.stack), sqlstate: error?.sqlstate || null};
    if (error?.cause && depth < 2) result.cause = errorInfo(error.cause, depth + 1);
    return result;
  }
  globalThis.PostGammaLiveReport = class {
    constructor(panel, options) {
      this.options = options;
      this.started = performance.now(); this.startedAt = new Date().toISOString();
      this.firstPromptAt = null;
      this.currentPhase = "initializing-page"; this.phaseAt = this.started;
      this.history = []; this.errors = []; this.metadata = null;
      const el = name => panel.querySelector("[data-pg-psql-" + name + "]");
      this.button = el("report-open"); this.box = el("report"); this.text = el("report-text");
      this.include = el("report-terminal"); this.notice = el("report-notice");
      this.button.addEventListener("click", () => this.show());
      el("report-refresh").addEventListener("click", () => this.refresh());
      el("report-copy").addEventListener("click", () => void this.copy());
      el("report-back").addEventListener("click", () => this.hide());
      this.include.addEventListener("change", () => this.refresh());
      this.box.addEventListener("keydown", event => {
        if (event.key === "Escape") {event.preventDefault(); event.stopPropagation(); this.hide();}
      });
      window.addEventListener("error", event => {
        if (event.error) this.failure(event.error, "window");
        else if (event.target?.src) this.failure(new Error("Resource failed: " + url(event.target.src)), "resource");
      }, true);
      window.addEventListener("unhandledrejection", event => this.failure(event.reason, "unhandled-rejection"));
    }
    get visible() {return !this.box.hidden;}
    phase(name) {
      if (name === this.currentPhase) return;
      this.currentPhase = name; this.phaseAt = performance.now();
      if (name === "ready" && this.firstPromptAt === null) this.firstPromptAt = Math.round(this.phaseAt);
      this.history.push({phase: name, elapsedMs: elapsed(this.started)});
      if (this.history.length > 32) this.history.shift();
    }
    failure(error, source = this.currentPhase) {
      this.errors.push({source, phase: this.currentPhase, elapsedMs: elapsed(this.started), ...errorInfo(error)});
      if (this.errors.length > 10) this.errors.shift();
      this.button.dataset.error = "true";
    }
    transcript() {
      const term = this.options.screen.terminal;
      if (!term) return {text: limit(this.options.screen.textContent, 16000), truncated: false};
      const buffer = term.buffer.active, end = buffer.baseY + buffer.cursorY;
      const start = Math.max(0, end - 199); let text = "";
      for (let row = start; row <= end; row++) {
        const line = buffer.getLine(row);
        if (!line) continue;
        if (row > start && !line.isWrapped) text += "\n";
        text += line.translateToString(row !== end, 0, row === end ? buffer.cursorX : undefined);
      }
      return {text: text.slice(-16000), truncated: start > 0 || text.length > 16000};
    }
    refresh() {
      const options = this.options, term = options.screen.terminal;
      const snapshot = options.snapshot();
      const navigation = performance.getEntriesByType("navigation")[0];
      const paint = performance.getEntriesByName("first-contentful-paint")[0];
      const entries = performance.getEntriesByType("resource").filter(entry =>
        entry.name.startsWith(options.assetBase.href) || entry.name.startsWith(options.scriptBase.href) ||
        entry.name === options.workerURL.href).slice(-20);
      const report = {
        kind: "postgamma.live-diagnostics", diagnosticsVersion: 1,
        generatedAt: new Date().toISOString(), startedAt: this.startedAt,
        page: url(location.href), phase: this.currentPhase, elapsedMs: elapsed(this.started),
        phaseElapsedMs: elapsed(this.phaseAt), package: this.metadata,
        pageTiming: {
          // Navigation-relative times also cover work before this script ran,
          // including stylesheets, fonts and module preloads.
          elapsedMs: Math.round(performance.now()), firstPsqlPromptMs: this.firstPromptAt,
          domContentLoadedMs: navigation?.domContentLoadedEventEnd ? Math.round(navigation.domContentLoadedEventEnd) : null,
          firstContentfulPaintMs: paint ? Math.round(paint.startTime) : null
        },
        environment: {
          userAgent: navigator.userAgent, language: navigator.language,
          secure: isSecureContext, isolated: globalThis.crossOriginIsolated === true,
          webAssembly: typeof WebAssembly === "object", sharedMemory: typeof SharedArrayBuffer === "function",
          worker: typeof Worker === "function", webCrypto: !!globalThis.crypto?.subtle,
          opfs: !!navigator.storage?.getDirectory, webLocks: !!navigator.locks,
          broadcastChannel: typeof BroadcastChannel === "function",
          hardwareConcurrency: navigator.hardwareConcurrency ?? null, deviceMemoryGiB: navigator.deviceMemory ?? null,
          online: navigator.onLine, visibility: document.visibilityState,
          viewport: {width: innerWidth, height: innerHeight, pixelRatio: devicePixelRatio},
          serviceWorker: navigator.serviceWorker?.controller ? {
            script: url(navigator.serviceWorker.controller.scriptURL), state: navigator.serviceWorker.controller.state
          } : null
        },
        runtime: snapshot, terminal: {columns: term?.cols ?? null, rows: term?.rows ?? null,
          included: this.include.checked, ...(this.include.checked ? this.transcript() : {})},
        phases: this.history.slice(), errors: this.errors.slice(),
        resources: entries.map(entry => ({url: url(entry.name), type: entry.initiatorType,
          durationMs: Math.round(entry.duration), transferBytes: entry.transferSize,
          decodedBytes: entry.decodedBodySize, responseStatus: entry.responseStatus ?? null}))
      };
      this.text.value = JSON.stringify(report, (_, value) => typeof value === "bigint" ? String(value) : value, 2);
      this.notice.textContent = "Created locally. Review before sharing.";
    }
    show() {
      this.refresh(); this.box.hidden = false; this.button.setAttribute("aria-expanded", "true");
      this.options.visibility(); this.text.focus({preventScroll: true}); this.text.scrollTop = 0;
    }
    hide() {
      this.box.hidden = true; this.button.setAttribute("aria-expanded", "false");
      this.options.visibility(); this.button.focus({preventScroll: true});
    }
    async copy() {
      try {
        await navigator.clipboard.writeText(this.text.value);
        this.notice.textContent = "Copied. Paste this report into your message.";
      } catch (_) {
        this.text.focus({preventScroll: true}); this.text.select();
        let copied = false;
        try {copied = document.execCommand("copy");} catch (_) {}
        this.notice.textContent = copied ? "Copied. Paste this report into your message." :
          "Text selected. Use Copy or Ctrl/Cmd+C.";
      }
    }
  };
}());
