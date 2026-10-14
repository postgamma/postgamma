/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
// GitHub Pages cannot set COOP/COEP headers. Installed only when the visitor
// opens a demo or starts the OPFS check. No cache or SQL data is stored here.
"use strict";
const root = new URL(self.registration.scope);
self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", event => event.waitUntil(self.clients.claim()));
self.addEventListener("fetch", event => {
  const url = new URL(event.request.url);
  const demo = event.request.mode === "navigate" &&
    ["", "index.html", "opfs/", "opfs/index.html"].some(path => url.pathname === root.pathname + path);
  // Theme workers (including documentation search) also need the policy when
  // the homepage is isolated. WebKit rejects them without this response header.
  const asset = url.pathname.startsWith(root.pathname + "assets/");
  if (url.origin !== root.origin || (!demo && !asset)) return;
  event.respondWith((async () => {
    const response = await fetch(event.request);
    if (response.type === "opaque" || response.status === 0) return response;
    const headers = new Headers(response.headers);
    headers.set("Cross-Origin-Opener-Policy", "same-origin");
    headers.set("Cross-Origin-Embedder-Policy", "require-corp");
    // Copying a decoded Fetch body must not retain its wire encoding/length.
    headers.delete("Content-Encoding"); headers.delete("Content-Length");
    return new Response(response.body, {status: response.status, statusText: response.statusText, headers});
  })());
});
