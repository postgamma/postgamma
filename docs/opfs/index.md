# OPFS persistence check

Save a small PostgreSQL record on this device, then check it after restarting
your browser. This page uses **OPFS**, independently of the homepage's memory database.

<div class="pg-opfs" data-pg-opfs>
  <div class="pg-opfs-status" data-opfs-state="idle" role="status" aria-live="polite">
    <strong data-opfs-status>Ready to test</strong>
    <span data-opfs-elapsed></span>
  </div>
  <p class="pg-opfs-instruction" data-opfs-instruction>First, write a test record. Then quit your browser, return to this exact address, and verify it.</p>
  <label for="opfs-note">Your test message</label>
  <input id="opfs-note" data-opfs-note type="text" maxlength="160" value="Hello from my browser — 你好 👋" autocomplete="off">
  <div class="pg-opfs-actions">
    <button class="pg-opfs-primary" type="button" data-opfs-write>1. Write &amp; verify</button>
    <button type="button" data-opfs-verify disabled>2. Verify saved data</button>
    <button type="button" data-opfs-cancel hidden>Cancel</button>
  </div>
  <div class="pg-opfs-evidence">
    <div><span>Expected marker</span><code data-opfs-expected>No saved test yet</code></div>
    <div><span>Read from PostgreSQL</span><code data-opfs-actual>Not read in this page visit</code></div>
    <div><span>Stored message</span><span data-opfs-message>—</span></div>
  </div>
  <p class="pg-opfs-error" data-opfs-error role="alert" hidden></p>
  <details class="pg-opfs-details">
    <summary>Recovery test and report</summary>
    <p><strong>Test interrupted write</strong> changes the record inside an uncommitted transaction, then reloads without closing the database. Recovery should keep the original committed record.</p>
    <div class="pg-opfs-actions">
      <button type="button" data-opfs-interrupt disabled>Test interrupted write</button>
      <button type="button" data-opfs-close disabled>Close database</button>
      <button type="button" data-opfs-copy>Copy report</button>
    </div>
    <p data-opfs-copy-status aria-live="polite"></p>
    <pre data-opfs-report tabindex="0" aria-label="Test report"></pre>
  </details>
  <noscript>Enable JavaScript to run the persistence check.</noscript>
</div>

Use a normal browser window and keep the same website address and browser
profile. Clearing site data also deletes this test database. Keep only one copy
of this test page open.

**Write & verify** saves a unique marker, closes the database, and checks the
record through a fresh database instance. **Verify saved data** only opens an
existing database: it never recreates the test table or inserts replacement data.
The expected marker is kept separately as a small browser-local receipt; the
actual marker is always queried from PostgreSQL.

Returning after quitting Safari and verifying the same marker tests persistence
on your device. The interrupted-write check tests page interruption and rollback,
not hardware power loss. These focused checks do not establish full Safari/iOS
release qualification.

The first start downloads the same browser package as the homepage. Static hosts
such as GitHub Pages may reload once to prepare the browser; no database server
is involved. See the [browser guide](../getting-started/browser.md) for SDK usage.

From a source checkout, run this page's automated checks with:

```sh
make docs-live-check DOCS_LIVE_CLIENTS=opfs
```

The checks cover Chromium, Firefox and WebKit, with both response-header
isolation and a GitHub Pages-style static host. Linux WebKit does not replace
testing on an actual Safari device.
