/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
(function () {
  "use strict";
  const script = document.currentScript.src;
  const assetBase = new URL("../wasm/", script), siteBase = new URL("../../", script);
  const workerURL = new URL("live-worker.js", siteBase);
  const resumeKey = "postgamma-psql-resume:" + siteBase.pathname;
  async function initialize() {
    if (new URLSearchParams(location.search).get("client") === "js") return;
    const panel = document.querySelector("[data-pg-psql]");
    if (!panel) return;
    const element = name => panel.querySelector("[data-pg-psql-" + name + "]");
    const status = element("status"), cancel = element("cancel");
    const reset = element("reset"), close = element("close"), screen = element("terminal");
    const loading = element("loading");
    let session, opening, openingWork, stopping, view, loadingTimer, loadingAt, stageAt, loadingStep = 0;
    function state(text, kind = "idle") {
      if (status.textContent !== text) status.textContent = text;
      if (status.dataset.state !== kind) status.dataset.state = kind;
    }
    function startLoading() {
      loading.hidden = false; screen.inert = true; screen.setAttribute("aria-hidden", "true");
      if (loadingTimer) return;
      loadingAt = stageAt = performance.now(); loadingStep = 0;
      renderLoading(); loadingTimer = setInterval(renderLoading, 250);
    }
    function renderLoading() {
      // Stage-weighted estimate, not a byte counter. Approach each stage's cap
      // slowly; only reaching the real psql prompt completes startup.
      const [base, span, duration] = [[2, 10, 2000], [15, 75, 25000], [95, 4, 2000]][loadingStep];
      const percent = Math.round(base + span * (1 - Math.exp(-(performance.now() - stageAt) / duration)));
      element("loading-percent").textContent = "~" + percent + "%";
      element("progress").setAttribute("aria-valuenow", String(percent));
      element("progress").firstElementChild.style.width = percent + "%";
      element("loading-time").textContent = Math.floor((performance.now() - loadingAt) / 1000) + "s";
    }
    function loadingStage(step, title, detail) {
      loadingStep = step; stageAt = performance.now();
      element("loading-title").textContent = title; element("loading-detail").textContent = detail;
      renderLoading();
    }
    function stopLoading() {
      clearInterval(loadingTimer); loadingTimer = undefined;
      loading.hidden = true; screen.inert = false; screen.removeAttribute("aria-hidden");
    }
    function focusWhenReady() {
      if ((document.activeElement === document.body || panel.contains(document.activeElement)) &&
          !matchMedia("(pointer: coarse)").matches) view.focus();
    }
    function controls() {
      reset.disabled = !!opening || !!stopping;
      reset.textContent = session ? "Reset" : "Restart";
      cancel.disabled = !session;
      close.disabled = (!session && !opening) || !!stopping;
      screen.setAttribute("aria-busy", String(!!opening));
    }
    function failure(error) {stopLoading(); state(error.message, "error"); controls();}
    async function isolate(signal) {
      if (crossOriginIsolated) return true;
      if (!isSecureContext) throw new Error("Open this page over HTTPS or localhost to run psql.");
      if (!("serviceWorker" in navigator)) throw new Error("This browser needs a host with cross-origin isolation headers.");
      state("Preparing browser…", "loading");
      const registration = await navigator.serviceWorker.getRegistration(siteBase.href);
      const owner = registration?.active || registration?.waiting || registration?.installing;
      if (owner && owner.scriptURL !== workerURL.href) throw new Error("Another service worker owns this page. Use a host with isolation headers.");
      await navigator.serviceWorker.register(workerURL, {scope: siteBase.pathname, updateViaCache: "none"});
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => finish(new Error("Browser preparation timed out")), 15000);
        function finish(error) {
          clearTimeout(timer); navigator.serviceWorker.removeEventListener("controllerchange", changed);
          signal.removeEventListener("abort", aborted); error ? reject(error) : resolve();
        }
        function changed() {if (navigator.serviceWorker.controller?.scriptURL === workerURL.href) finish();}
        function aborted() {finish(signal.reason);}
        navigator.serviceWorker.addEventListener("controllerchange", changed);
        signal.addEventListener("abort", aborted, {once: true});
        if (signal.aborted) aborted(); else changed();
      });
      signal.throwIfAborted();
      // Refuse a second automatic reload if browser policy prevented isolation.
      try {sessionStorage.setItem(resumeKey, String(Date.now()));}
      catch (_) {throw new Error("Browser preparation needs session storage or a host with isolation headers.");}
      state("Preparing browser — reloading once…", "loading"); location.reload(); return false;
    }
    async function release() {
      if (stopping) return stopping;
      opening?.abort(new DOMException("Startup canceled", "AbortError"));
      const previous = session; session = undefined;
      if (previous) previous.active = false;
      stopLoading();
      view.pending = "";
      stopping = (async () => {await openingWork; await previous?.database.close();})();
      controls();
      try {await stopping; await view.enqueue(() => view.forget());}
      finally {stopping = undefined; controls();}
    }
    async function pump(owned) {
      const decoder = new TextDecoder();
      try {
        while (owned.active) {
          const revision = owned.revision, batch = await owned.terminal.read();
          if (!owned.active) return;
          await view.output(decoder.decode(batch.data, {stream: batch.state !== "exited"}),
            () => batch.state === "input" && owned.active && revision === owned.revision);
          if (!owned.active) return;
          if (view.active && !loading.hidden) {stopLoading(); focusWhenReady();}
          if (batch.state === "exited") {
            await release(); state("psql exited (" + batch.exitCode + ")", batch.exitCode ? "error" : "idle");
            await view.message("\r\nPress Enter to start a fresh session.\r\n"); return;
          }
          state(view.active ? "Ready" : "Running…", view.active ? "ready" : "loading");
          await new Promise(resolve => setTimeout(resolve, batch.data.length ? 0 : 30));
        }
      } catch (error) {
        if (!owned.active) return;
        await view.message("\r\n" + error.message + "\r\n");
        try {await release();} catch (cleanup) {failure(cleanup);}
        state("Terminal failed — press Enter to retry", "error");
      }
    }
    function open() {
      if (opening || session || stopping) return;
      opening = new AbortController(); const signal = opening.signal;
      startLoading(); loadingStage(0, "Starting PostgreSQL", "Preparing your browser");
      controls(); state("Loading PostgreSQL…", "loading");
      openingWork = (async () => {
        let database;
        try {
          await view.clear(); await view.message("Loading PostgreSQL…\r\n");
          if (!await isolate(signal)) return;
          loadingStage(1, "Loading PostgreSQL", "Downloading and starting the engine");
          const response = await fetch(new URL("demo.json", assetBase), {signal, cache: "no-cache"});
          if (!response.ok || !(await response.json()).available) throw new Error("The browser package has not been bundled for this preview.");
          const {Database} = await import(new URL("src/index.js", assetBase));
          signal.throwIfAborted();
          database = await Database.open({assetBase, signal});
          loadingStage(2, "Opening psql", "Connecting to your database");
          const terminal = await database.psql(); signal.throwIfAborted();
          await view.clear();
          session = {database, terminal, active: true, revision: 0}; database = undefined;
          void pump(session);
        } catch (error) {
          stopLoading();
          await view.message("\r\n" + error.message + "\r\nPress Enter to retry.\r\n");
          state(signal.aborted ? "Canceled" : "Could not start", signal.aborted ? "idle" : "error");
          await database?.close();
        } finally {opening = undefined; openingWork = undefined; controls();}
      })();
      void openingWork.catch(failure);
    }
    async function submit(text) {
      const owned = session;
      if (!owned || stopping) return;
      owned.revision++; state("Running…", "loading");
      await owned.terminal.write(text);
    }
    async function interrupt() {
      if (!session || stopping) return;
      const owned = session; owned.revision++;
      try {await view.cancel(); await owned.terminal.interrupt();}
      catch (error) {failure(error);}
    }
    async function shutdown() {
      state("Closing…", "loading");
      try {await release(); state("Database closed"); await view.message("\r\nSession closed. Press Enter to restart.\r\n");}
      catch (error) {failure(error);}
    }
    startLoading();
    try {
      const {PsqlScreen} = await import(new URL("psql-terminal.js", script));
      view = new PsqlScreen(screen, {line: submit, interrupt, eof: shutdown, restart: open,
        closed: () => !session && !opening && !stopping, error: failure});
    } catch (error) {
      screen.textContent = "Could not load the terminal: " + error.message; failure(error);
      reset.textContent = "Reload"; reset.addEventListener("click", () => location.reload()); return;
    }
    cancel.addEventListener("click", () => {void interrupt(); view.focus();});
    close.addEventListener("click", () => void shutdown());
    reset.addEventListener("click", () => {
      state("Resetting…", "loading"); void release().then(open, failure); view.focus();
    });
    document.querySelectorAll("[data-pg-try-live]").forEach(link => link.addEventListener("click", () => {
      document.getElementById("pg-tab-browser").click(); view.focus();
    }));
    const widget = document.getElementById("live-demo"), expand = widget.querySelector("[data-pg-live-expand]");
    let dialog;
    expand.addEventListener("click", () => {
      if (dialog) {dialog.close(); return;}
      const placeholder = document.createComment("live demo position"); widget.before(placeholder);
      dialog = document.createElement("dialog"); dialog.className = "pg-live-dialog"; dialog.setAttribute("aria-label", "psql terminal");
      placeholder.after(dialog); dialog.append(widget);
      const overflow = document.body.style.overflow; document.body.style.overflow = "hidden";
      expand.setAttribute("aria-label", "Collapse demo");
      dialog.addEventListener("close", () => {
        placeholder.replaceWith(widget); dialog.remove(); dialog = undefined; document.body.style.overflow = overflow;
        expand.setAttribute("aria-label", "Expand demo"); view.focus();
      }, {once: true});
      dialog.showModal(); view.focus();
    });
    let resumed;
    try {resumed = Number(sessionStorage.getItem(resumeKey)); sessionStorage.removeItem(resumeKey);} catch (_) {}
    if (resumed && Date.now() - resumed < 60000 && !crossOriginIsolated) {
      failure(new Error("Browser setup unavailable: use a host with isolation headers."));
      await view.message("This browser needs a host with cross-origin isolation headers.\r\n");
    } else open();
  }
  function start() {void initialize().catch(error => {const status = document.querySelector('[data-pg-psql-status]'); if (status) status.textContent = error.message;});}
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start, {once: true}); else start();
}());
