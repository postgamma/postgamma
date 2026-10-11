/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
(function () {
  "use strict";
  const scriptURL = document.currentScript.src;
  const assetBase = new URL("../wasm/", scriptURL);
  const siteBase = new URL("../../", scriptURL);
  const workerURL = new URL("live-worker.js", siteBase);
  const resumeKey = "postgamma-live-resume:" + siteBase.pathname;
  const examples = {
    sql: "SELECT category,\n       count(*) AS orders,\n       sum(total) AS revenue\nFROM demo_orders\nGROUP BY category\nORDER BY revenue DESC;",
    json: "SELECT id,\n       details->>'customer' AS customer,\n       details->'tags' AS tags\nFROM demo_orders\nWHERE details @> '{\"tags\":[\"local-first\"]}'\nORDER BY id;",
    transaction: "BEGIN;\nUPDATE demo_orders SET total = total + 10 WHERE id = 1;\nSELECT 'before rollback' AS stage, total FROM demo_orders WHERE id = 1;\nROLLBACK;\nSELECT 'after rollback' AS stage, total FROM demo_orders WHERE id = 1;",
    vector: "-- Three sample vectors; no embedding service needed.\nSELECT title,\n       round((embedding <-> '[1,0,0]')::numeric, 3) AS distance\nFROM demo_documents\nORDER BY embedding <-> '[1,0,0]'\nLIMIT 3;"
  };
  const seed = `
    CREATE TABLE demo_orders (id int PRIMARY KEY, category text, total numeric(10,2), details jsonb);
    INSERT INTO demo_orders VALUES
      (1, 'Books', 29.00, '{"customer":"Ada","tags":["local-first"]}'),
      (2, 'Tools', 89.00, '{"customer":"Lin","tags":["developer"]}'),
      (3, 'Books', 35.00, '{"customer":"Sam","tags":["developer","local-first"]}'),
      (4, 'Tools', 49.00, '{"customer":"Jo","tags":["local-first"]}'),
      (5, 'Music', 18.00, '{"customer":"Kim","tags":["offline"]}'),
      (6, 'Music', 24.00, '{"customer":"Alex","tags":["offline"]}');
    CREATE EXTENSION vector;
    CREATE TABLE demo_documents (title text, embedding vector(3));
    INSERT INTO demo_documents VALUES
      ('PostgreSQL in the browser', '[1,0,0]'),
      ('Local vector search', '[0.8,0.2,0]'),
      ('Native Python applications', '[0,0,1]');
  `;

  function initialize() {
    const panel = document.querySelector("[data-pg-live]");
    if (!panel) return;
    const element = name => panel.querySelector("[data-pg-live-" + name + "]");
    const editor = element("sql"), example = element("example"), output = element("results");
    const run = element("run"), cancel = element("cancel"), reset = element("reset"), close = element("close");
    const status = element("status"), detail = element("detail");
    let database, connection, operation, busy = false, metadata;

    function state(message, kind = "idle") {
      status.textContent = message;
      status.dataset.state = kind;
    }
    function controls(active) {
      busy = active;
      run.disabled = active;
      cancel.disabled = !active;
      reset.disabled = close.disabled = active || !database;
      example.disabled = active;
      editor.readOnly = active;
      run.textContent = database ? "Run SQL" : "Start & run";
      output.setAttribute("aria-busy", String(active));
    }
    function message(text, error = false) {
      const p = document.createElement("p");
      p.className = error ? "pg-live-error" : "pg-live-message";
      p.textContent = text;
      output.append(p);
    }
    function readResume() {
      try {
        const saved = sessionStorage.getItem(resumeKey);
        sessionStorage.removeItem(resumeKey);
        return saved ? JSON.parse(saved) : null;
      } catch (_) {return null;}
    }

    async function isolate(signal) {
      if (window.crossOriginIsolated) return true;
      if (!window.isSecureContext) throw new Error("Open this page over HTTPS or localhost to run the demo.");
      if (!("serviceWorker" in navigator)) throw new Error("This browser cannot start the live demo. See the browser guide for supported environments.");
      state("Preparing browser…", "loading");
      const existing = await navigator.serviceWorker.getRegistration(siteBase.href);
      const existingWorker = existing?.active || existing?.waiting || existing?.installing;
      if (existingWorker && existingWorker.scriptURL !== workerURL.href)
        throw new Error("This site already uses another service worker. Serve the demo with cross-origin isolation headers instead.");
      await navigator.serviceWorker.register(workerURL, {scope: siteBase.pathname, updateViaCache: "none"});
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => finish(new Error("Browser preparation timed out. Retry or open the browser guide.")), 15000);
        function finish(error) {
          clearTimeout(timer);
          navigator.serviceWorker.removeEventListener("controllerchange", changed);
          signal.removeEventListener("abort", aborted);
          error ? reject(error) : resolve();
        }
        function changed() {if (navigator.serviceWorker.controller?.scriptURL === workerURL.href) finish();}
        function aborted() {finish(signal.reason);}
        navigator.serviceWorker.addEventListener("controllerchange", changed);
        signal.addEventListener("abort", aborted, {once: true});
        if (signal.aborted) aborted(); else changed();
      });
      signal.throwIfAborted();
      // Preserve the user's edit across the one navigation needed on static hosts.
      // The resumed page never retries isolation automatically, avoiding reload loops.
      try {sessionStorage.setItem(resumeKey, JSON.stringify({sql: editor.value, example: example.value, at: Date.now()}));}
      catch (_) {throw new Error("Browser preparation needs session storage. Enable it or use a host with isolation headers.");}
      state("Preparing browser — reloading once…", "loading");
      location.reload();
      return false;
    }

    async function release() {
      // Retain handles when closing fails so Reset/Close can retry cleanup.
      if (connection) {await connection.close(); connection = undefined;}
      if (database) {await database.close(); database = undefined;}
    }

    async function open(signal) {
      if (database && connection) return;
      if (database) await release();
      if (!metadata) {
        const response = await fetch(new URL("demo.json", assetBase), {signal, cache: "no-store"});
        if (!response.ok) throw new Error("This site does not include the browser demo package yet. See the browser guide.");
        metadata = await response.json();
      }
      if (!metadata.available) throw new Error("This documentation build has no browser package. See the browser guide to build the live demo.");
      signal.throwIfAborted();
      state("Loading PostgreSQL…", "loading");
      detail.textContent = "Browser package · " + (metadata.bytes / 1048576).toFixed(1) + " MiB";
      const {Database} = await import(new URL("src/index.js", assetBase).href);
      signal.throwIfAborted();
      try {
        database = await Database.open({assetBase, signal});
        signal.throwIfAborted();
        state("Creating sample data…", "loading");
        connection = await database.connect();
        signal.throwIfAborted();
        await connection.exec("SET statement_timeout = '10s';" + seed, {signal, timeoutMs: 15000});
      } catch (error) {
        try {await release();} catch (cleanup) {throw new Error(error.message + " Cleanup failed: " + cleanup.message);}
        throw error;
      }
      detail.textContent = "In-memory · " + metadata.version;
    }

    function table(result, remaining, group) {
      // TUPLES_CHUNK (5) is followed by a TUPLES_OK (1) terminator. Keep one
      // table per statement, including truly empty SELECTs between statements.
      if (!group || ![1, 5].includes(result.kind)) {
        const section = document.createElement("section"), label = document.createElement("p");
        label.className = "pg-live-result-label";
        label.textContent = result.columns.length ? "Results" : "Statement complete";
        section.append(label);
        group = {label};
        if (result.columns.length) {
          const grid = document.createElement("table"), head = grid.createTHead().insertRow();
          group.body = grid.createTBody();
          result.columns.slice(0, 30).forEach(column => {
            const th = document.createElement("th"); th.textContent = column.name; th.scope = "col"; head.append(th);
          });
          section.append(grid);
        }
        output.append(section);
      }
      if (result.command) group.label.textContent = result.command;
      if (group.body) {
        result.rows.slice(0, remaining).forEach(values => {
          const row = group.body.insertRow();
          values.slice(0, 30).forEach(value => {
            const td = row.insertCell();
            if (value === null) {td.textContent = "NULL"; td.className = "pg-live-null";}
            else td.textContent = String(value).slice(0, 2000) + (String(value).length > 2000 ? "…" : "");
          });
        });
      }
      return result.kind === 5 ? group : undefined;
    }

    async function execute(fresh = false) {
      if (busy || !editor.value.trim()) return;
      controls(true);
      operation = new AbortController();
      const signal = operation.signal;
      output.replaceChildren();
      let request;
      try {
        if (!await isolate(signal)) return;
        signal.throwIfAborted();
        if (fresh) {state("Resetting database…", "loading"); await release();}
        await open(signal);
        state("Running SQL…", "loading");
        const started = performance.now();
        let rows = 0, batches = 0, limited = false, group;
        request = await connection.executeScript(editor.value, {signal, timeoutMs: 15000, chunkRows: 100});
        for await (const result of request) {
          if (result.copy) throw new Error("COPY streams are available through the SDK. Use SELECT for this demo.");
          const remaining = Math.max(0, 200 - rows);
          group = table(result, remaining, group);
          rows += Math.min(result.rows.length, remaining);
          batches += 1;
          if (rows >= 200 || batches >= 20 || result.columns.length > 30) {limited = true; break;}
        }
        await request.close(); request = undefined;
        const elapsed = (performance.now() - started).toFixed(1);
        state(limited ? "Output limit reached" : "Query complete", limited ? "idle" : "ready");
        detail.textContent = rows + " rows · " + elapsed + " ms";
        if (limited) message("Demo limit: 200 rows, 30 columns or 20 result batches. Execution was stopped; later statements may not have run. Reset to restore the sample data.");
        if (!batches) message("Statement complete.");
      } catch (error) {
        state(signal.aborted ? "Canceled" : "Could not complete", signal.aborted ? "idle" : "error");
        message(signal.aborted ? "Operation canceled. You can edit the SQL and try again." :
          (error.sqlstate ? "SQLSTATE " + error.sqlstate + ": " : "") + error.message, !signal.aborted);
        detail.textContent = database ? "In-memory database · Reset restores sample data" : "Database is not running";
      } finally {
        try {await request?.close();}
        catch (error) {state("Cleanup failed", "error"); message(error.message, true);}
        operation = undefined;
        controls(false);
      }
    }

    run.addEventListener("click", () => void execute());
    reset.addEventListener("click", () => void execute(true));
    cancel.addEventListener("click", () => {
      cancel.disabled = true;
      state("Canceling…", "loading");
      operation?.abort(new DOMException("Canceled by the user", "AbortError"));
    });
    close.addEventListener("click", async () => {
      if (busy) return;
      controls(true); cancel.disabled = true; state("Closing…", "loading");
      try {await release(); state("Database closed"); detail.textContent = "Sample data discarded";}
      catch (error) {state("Could not close", "error"); message(error.message, true);}
      finally {controls(false);}
    });
    example.addEventListener("change", () => {editor.value = examples[example.value]; editor.focus();});
    editor.addEventListener("keydown", event => {
      if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {event.preventDefault(); void execute();}
    });
    document.querySelectorAll("[data-pg-try-live]").forEach(link => link.addEventListener("click", () => {
      document.getElementById("pg-tab-browser").click(); run.focus({preventScroll: true});
    }));
    const widget = document.querySelector("#live-demo"), expand = widget.querySelector("[data-pg-live-expand]");
    let dialog;
    expand.addEventListener("click", () => {
      if (dialog) {dialog.close(); return;}
      const placeholder = document.createComment("live demo position");
      widget.before(placeholder);
      dialog = document.createElement("dialog"); dialog.className = "pg-live-dialog";
      dialog.setAttribute("aria-label", "PostGamma SQL playground");
      placeholder.after(dialog); dialog.append(widget);
      const overflow = document.body.style.overflow;
      document.body.style.overflow = "hidden";
      expand.setAttribute("aria-label", "Collapse demo"); expand.title = "Collapse demo";
      dialog.addEventListener("close", () => {
        placeholder.replaceWith(widget); dialog.remove(); dialog = undefined;
        document.body.style.overflow = overflow;
        expand.setAttribute("aria-label", "Expand demo"); expand.title = "Expand demo"; expand.focus();
      }, {once: true});
      dialog.showModal(); editor.focus();
    });
    const saved = readResume();
    if (saved && Date.now() - saved.at < 60000) {
      if (typeof saved.sql === "string") editor.value = saved.sql;
      if (Object.hasOwn(examples, saved.example)) example.value = saved.example;
      if (window.crossOriginIsolated) void execute();
      else {state("Browser setup unavailable", "error"); output.replaceChildren(); message("The browser could not enable the demo after reloading. Use a host with cross-origin isolation headers; see the browser guide.", true);}
    }
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", initialize, {once: true});
  else initialize();
}());
