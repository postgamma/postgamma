/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
(function () {
  "use strict";
  const script = document.currentScript.src;
  const siteBase = new URL("../../", script), assetBase = new URL("../wasm/", script);
  const workerURL = new URL("live-worker.js", siteBase);
  // Different project sites on the same origin get separate test databases.
  let hash = 5381;
  for (const char of siteBase.pathname) hash = ((hash * 33) ^ char.charCodeAt(0)) >>> 0;
  const databaseName = "docs-opfs-check-" + hash.toString(16);
  const receiptKey = "postgamma:opfs-receipt:" + siteBase.pathname;
  const resumeKey = "postgamma:opfs-resume:" + siteBase.pathname;

  function initialize() {
    const panel = document.querySelector("[data-pg-opfs]");
    if (!panel) return;
    const el = name => panel.querySelector("[data-opfs-" + name + "]");
    const status = el("status"), instruction = el("instruction"), errorBox = el("error");
    let database, connection, operation, receipt, metadata, reloading = false;
    const report = {database: databaseName, origin: location.origin, userAgent: navigator.userAgent,
      package: null, checks: {}, expected: null, actual: null, error: null};

    function refreshReport() {
      report.environment = {secure: isSecureContext, isolated: crossOriginIsolated,
        sharedMemory: typeof SharedArrayBuffer === "function", opfs: !!navigator.storage?.getDirectory,
        webLocks: !!navigator.locks};
      el("report").textContent = JSON.stringify(report, null, 2);
    }
    function state(text, kind, detail) {
      status.textContent = text;
      status.parentElement.dataset.opfsState = kind;
      if (detail) instruction.textContent = detail;
    }
    function controls() {
      const busy = !!operation;
      el("write").disabled = busy;
      el("verify").disabled = busy || !receipt;
      el("interrupt").disabled = busy || !receipt;
      el("close").disabled = busy || !database;
      el("note").disabled = busy;
      el("cancel").hidden = !busy;
    }
    function readReceipt() {
      const text = localStorage.getItem(receiptKey);
      if (!text) return null;
      const saved = JSON.parse(text);
      if (saved.version !== 1 || saved.database !== databaseName || typeof saved.marker !== "string" ||
          typeof saved.note !== "string" || typeof saved.savedAt !== "string" ||
          (saved.pending !== null && typeof saved.pending !== "string"))
        throw new Error("The saved verification receipt is invalid. Write a new test record to start again.");
      return saved;
    }
    function saveReceipt(saved) {
      localStorage.setItem(receiptKey, JSON.stringify(saved));
      receipt = saved;
      report.expected = saved;
      el("expected").textContent = saved.marker;
      refreshReport();
    }
    function remember(action) {
      sessionStorage.setItem(resumeKey, JSON.stringify({action, note: el("note").value, at: Date.now()}));
    }
    async function isolate(action, signal, resumed) {
      if (crossOriginIsolated) return true;
      if (!isSecureContext) throw new Error("Open this page over HTTPS or localhost.");
      if (resumed) throw new Error("Browser isolation was not enabled after reloading. Use a host with COOP/COEP headers.");
      if (!navigator.serviceWorker) throw new Error("This host needs a service worker or cross-origin isolation headers.");
      state("Preparing browser…", "loading", "The page will reload once, then continue your test.");
      const previous = await navigator.serviceWorker.getRegistration(siteBase.href);
      const previousWorker = previous?.active || previous?.waiting || previous?.installing;
      if (previousWorker && previousWorker.scriptURL !== workerURL.href)
        throw new Error("Another service worker owns this site. Serve it with cross-origin isolation headers.");
      const registration = await navigator.serviceWorker.register(workerURL, {scope: siteBase.pathname, updateViaCache: "none"});
      await new Promise((resolve, reject) => {
        const worker = registration.installing || registration.waiting || registration.active;
        const timer = setTimeout(() => finish(new Error("Browser preparation timed out.")), 15000);
        function finish(error) {
          clearTimeout(timer);
          worker?.removeEventListener("statechange", changed);
          navigator.serviceWorker.removeEventListener("controllerchange", changed);
          signal.removeEventListener("abort", aborted);
          error ? reject(error) : resolve();
        }
        function changed() {
          if (worker?.state === "redundant") finish(new Error("The browser preparation worker could not activate."));
          else if (worker?.state === "activated" && navigator.serviceWorker.controller?.scriptURL === workerURL.href) finish();
        }
        function aborted() {finish(signal.reason);}
        worker?.addEventListener("statechange", changed);
        navigator.serviceWorker.addEventListener("controllerchange", changed);
        signal.addEventListener("abort", aborted, {once: true});
        if (signal.aborted) aborted(); else changed();
      });
      signal.throwIfAborted();
      remember(action); reloading = true; location.reload();
      return false;
    }
    async function release() {
      if (connection) {await connection.close(); connection = undefined;}
      if (database) {await database.close(); database = undefined;}
    }
    async function open(create, signal) {
      if (!navigator.storage?.getDirectory || !navigator.locks || typeof BroadcastChannel !== "function")
        throw new Error("This browser does not expose the OPFS, Web Locks and BroadcastChannel APIs required by this build.");
      state("Starting PostgreSQL…", "loading", "The first start downloads the browser package. You can cancel while it loads.");
      if (!metadata) {
        const response = await fetch(new URL("demo.json", assetBase), {signal, cache: "no-cache"});
        const info = response.ok ? await response.json() : null;
        if (!info?.available)
          throw new Error("This documentation build does not include the browser package.");
        metadata = info;
        report.package = metadata;
      }
      const {Database} = await import(new URL("src/index.js", assetBase));
      signal.throwIfAborted();
      database = await Database.open({assetBase, create, storage: {type: "opfs", name: databaseName},
        lockTimeoutMs: 8000, signal});
      connection = await database.connect("opfs-browser-check");
      await connection.exec("SET statement_timeout = '15s'; SET synchronous_commit = on;", {signal, timeoutMs: 20000});
      const settings = (await connection.query("SELECT current_setting('fsync'), current_setting('synchronous_commit')",
        {signal, timeoutMs: 20000}))[0].rows[0];
      if (settings[0] !== "on" || settings[1] !== "on") throw new Error("The test requires fsync and synchronous_commit to be enabled.");
      report.checks.durabilitySettings = "passed";
      refreshReport();
    }
    function parameters(...values) {return values.map(value => ({typeOid: 25, value}));}
    async function readRow(signal) {
      const rows = (await connection.query("SELECT marker, note, saved_at::text FROM public.postgamma_opfs_check WHERE slot = 1",
        {signal, timeoutMs: 20000}))[0].rows;
      if (rows.length !== 1) throw new Error("The saved PostgreSQL record is missing.");
      const [marker, note, savedAt] = rows[0];
      return {marker, note, savedAt};
    }
    async function verify(signal) {
      state("Reading saved data…", "loading");
      const row = await readRow(signal);
      report.actual = row;
      el("actual").textContent = row.marker;
      el("message").textContent = row.note;
      if (row.marker !== receipt.marker || row.note !== receipt.note || row.savedAt !== receipt.savedAt)
        throw new Error("The PostgreSQL record does not match the saved receipt. No replacement data has been written.");
      report.checks.reopen = "passed";
      if (receipt.pending) {
        report.checks.interruptedTransaction = "passed";
        saveReceipt({...receipt, pending: null});
        state("Recovery verified", "passed", "The committed record survived; the uncommitted change was rolled back.");
      } else {
        state("Saved data verified", "passed", "Now quit your browser, return to this exact address, and tap “2. Verify saved data”.");
      }
    }
    async function run(action, resumed = false) {
      if (operation) return;
      const controller = new AbortController(); operation = controller;
      const signal = controller.signal, started = Date.now();
      const elapsed = setInterval(() => {el("elapsed").textContent = Math.floor((Date.now() - started) / 1000) + "s";}, 500);
      const deadline = setTimeout(() => controller.abort(new Error("The test exceeded two minutes. Cancelled to avoid an indefinite wait.")), 120000);
      errorBox.hidden = true; report.error = null;
      delete report.cleanupError;
      if (action !== "close") report.checks = {};
      controls();
      try {
        if (action === "close") {
          state("Closing database…", "loading");
          await release();
          state("Database closed", "idle", "The record remains in OPFS. Reopen this page and verify it when you are ready.");
          return;
        }
        if (action !== "write") {
          receipt = readReceipt();
          if (!receipt) throw new Error("There is no saved verification receipt. Write a test record first.");
          report.expected = receipt; el("expected").textContent = receipt.marker;
        }
        if (!await isolate(action, signal, resumed)) return;
        await release();
        report.actual = null; report.checks.reopen = "not run";
        el("actual").textContent = "Not read in this page visit"; el("message").textContent = "—";
        await open(action === "write", signal);
        if (action === "write") {
          state("Writing test record…", "loading");
          await connection.exec("CREATE TABLE IF NOT EXISTS public.postgamma_opfs_check (" +
            "slot integer PRIMARY KEY CHECK (slot = 1), marker text NOT NULL, note text NOT NULL, saved_at timestamptz NOT NULL)",
            {signal, timeoutMs: 20000});
          const marker = crypto.randomUUID(), note = el("note").value;
          const rows = (await connection.query("INSERT INTO public.postgamma_opfs_check VALUES (1, $1, $2, clock_timestamp()) " +
            "ON CONFLICT (slot) DO UPDATE SET marker = excluded.marker, note = excluded.note, saved_at = excluded.saved_at " +
            "RETURNING marker, note, saved_at::text", {parameters: parameters(marker, note), signal, timeoutMs: 20000}))[0].rows;
          if (rows.length !== 1) throw new Error("The insert did not return its committed record.");
          saveReceipt({version: 1, database: databaseName, marker: rows[0][0], note: rows[0][1], savedAt: rows[0][2], pending: null});
          report.checks.write = "passed";
          state("Reopening OPFS database…", "loading");
          await release();
          await open(false, signal);
        }
        await verify(signal);
        if (action === "interrupt") {
          state("Preparing interrupted write…", "loading");
          const pending = crypto.randomUUID();
          // Leave this transaction open. Navigation deliberately skips close()
          // and ROLLBACK so a fresh instance exercises PostgreSQL recovery.
          await connection.exec("BEGIN", {signal, timeoutMs: 20000});
          await connection.query("UPDATE public.postgamma_opfs_check SET marker = $1 WHERE slot = 1",
            {parameters: parameters(pending), signal, timeoutMs: 20000});
          if ((await readRow(signal)).marker !== pending) throw new Error("The uncommitted update did not execute.");
          remember("verify");
          signal.throwIfAborted();
          saveReceipt({...receipt, pending});
          state("Reloading with an open transaction…", "loading");
          reloading = true; location.reload();
        }
      } catch (error) {
        try {sessionStorage.removeItem(resumeKey);} catch (_) {}
        const cause = signal.aborted ? signal.reason : error;
        report.error = {message: cause.message || String(cause), sqlstate: error.sqlstate, code: error.code};
        state(signal.aborted ? "Test cancelled" : "Check failed", "error",
          "The test has not passed. The report below includes the error and browser details.");
        errorBox.textContent = report.error.message; errorBox.hidden = false;
        try {await release();} catch (cleanup) {report.cleanupError = String(cleanup);}
      } finally {
        clearInterval(elapsed); clearTimeout(deadline);
        el("elapsed").textContent = ((Date.now() - started) / 1000).toFixed(1) + "s";
        operation = null; refreshReport(); controls();
        if (reloading) for (const button of panel.querySelectorAll("button")) button.disabled = true;
      }
    }
    for (const action of ["write", "verify", "interrupt", "close"])
      el(action).addEventListener("click", () => void run(action));
    el("cancel").addEventListener("click", () => operation?.abort(new Error("Cancelled by user.")));
    el("copy").addEventListener("click", async () => {
      try {await navigator.clipboard.writeText(el("report").textContent); el("copy-status").textContent = "Report copied.";}
      catch (_) {el("copy-status").textContent = "Copy is unavailable. Select and copy the report below.";}
    });
    try {
      receipt = readReceipt();
      if (receipt) {
        report.expected = receipt; el("expected").textContent = receipt.marker; el("note").value = receipt.note;
        state("Saved test found", "idle", "Tap “2. Verify saved data” to read your existing OPFS database.");
      }
    } catch (error) {errorBox.textContent = error.message; errorBox.hidden = false;}
    refreshReport(); controls();
    let resume;
    try {resume = JSON.parse(sessionStorage.getItem(resumeKey)); sessionStorage.removeItem(resumeKey);} catch (_) {}
    if (resume && ["write", "verify", "interrupt"].includes(resume.action) && Date.now() - resume.at < 120000) {
      if (typeof resume.note === "string") el("note").value = resume.note;
      void run(resume.action, true);
    }
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", initialize, {once: true});
  else initialize();
})();
