# PostgreSQL in your app, agent, and browser { #pg-v3-heading .pg-document-title }

<div class="pg-home pg-home-v3">

<nav class="pg-product-nav" aria-label="Homepage navigation">
  <a class="pg-brand" href="./" aria-label="postgamma home">
    <span class="pg-brand-mark" aria-hidden="true"></span>
    <span class="pg-brand-wordmark">postgamma</span>
  </a>
  <div class="pg-product-links">
    <a href="#architecture">Architecture</a>
    <a href="#sdks">Run modes</a>
    <a href="#use-cases">Use cases</a>
    <a href="getting-started/">Documentation</a>
  </div>
  <div class="pg-product-actions">
    <a class="pg-github-link" href="https://github.com/postgamma/postgamma" target="_blank" rel="noopener noreferrer" aria-label="postgamma on GitHub, 0 stars" title="postgamma on GitHub" data-pg-github-stars data-pg-repository-api="https://api.github.com/repos/postgamma/postgamma">
      <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false">
        <path d="M 12 2C6.477 2 2 6.477 2 12c0 4.42 2.865 8.17 6.839 9.493.5.09.682-.217.682-.482 0-.237-.009-.866-.014-1.7-2.782.604-3.369-1.342-3.369-1.342-.454-1.154-1.11-1.462-1.11-1.462-.908-.62.069-.608.069-.608 1.003.07 1.531 1.03 1.531 1.03.892 1.529 2.341 1.087 2.91.831.091-.646.349-1.087.635-1.337-2.221-.253-4.555-1.111-4.555-4.943 0-1.091.39-1.984 1.029-2.683-.103-.253-.446-1.269.098-2.647 0 0 .84-.269 2.75 1.025A9.564 9.564 0 0 1 12 6.838a9.59 9.59 0 0 1 2.504.337c1.909-1.294 2.748-1.025 2.748-1.025.546 1.378.203 2.394.1 2.647.64.699 1.028 1.592 1.028 2.683 0 3.842-2.337 4.687-4.566 4.935.359.309.679.92.679 1.855 0 1.338-.012 2.419-.012 2.747 0 .267.18.577.688.479C19.138 20.167 22 16.419 22 12c0-5.523-4.477-10-10-10Z"/>
      </svg>
      <span class="pg-github-stars" aria-hidden="true"><span class="pg-github-star-icon">★</span><span data-pg-star-count>0</span></span>
    </a>
    <button class="pg-theme-toggle" type="button" data-pg-theme-toggle aria-label="Switch to light mode">
      <span class="pg-theme-icon pg-theme-icon--sun" aria-hidden="true"></span>
      <span class="pg-theme-icon pg-theme-icon--moon" aria-hidden="true"></span>
    </button>
    <a class="pg-nav-cta" href="getting-started/">Quickstart <span aria-hidden="true">↗</span></a>
  </div>
</nav>

<header class="pg-v3-hero" aria-labelledby="pg-v3-heading">
  <div class="pg-v3-hero-copy">
    <p class="pg-v3-eyebrow"><span></span> PostgreSQL 19 · native + browser</p>
    <div class="pg-v3-display" aria-hidden="true"><span class="pg-v3-morph" data-pg-word-morph>PostgreSQL</span><strong>In your app, agent, and browser</strong></div>
    <p class="pg-v3-lead">Give your apps and AI agents PostgreSQL. Embed it with Python or C, run it as a threaded server, or use it in your browser.</p>
    <div class="pg-v3-actions">
      <a class="pg-action pg-action--primary" href="#live-demo" data-pg-try-live>Try it live <span aria-hidden="true">↗</span></a>
      <a class="pg-action pg-action--secondary" href="getting-started/">Build your first app</a>
    </div>
    <ul class="pg-v3-facts" aria-label="Supported platform and license">
      <li><span aria-hidden="true"></span> Embedded · Server · Browser</li>
      <li><span aria-hidden="true"></span> Server source build · Browser preview</li>
      <li><span aria-hidden="true"></span> PostGamma · Apache-2.0</li>
    </ul>
  </div>

  <div id="live-demo" class="pg-v3-demo" aria-label="PostGamma live demo and SDK quickstarts" data-pg-code-switcher>
    <div class="pg-demo-head">
      <div class="pg-demo-dots" aria-hidden="true"><i></i><i></i><i></i></div>
      <div class="pg-code-tabs" role="tablist" aria-label="Choose an SDK quickstart" data-pg-code-tabs>
        <button id="pg-tab-browser" type="button" role="tab" aria-selected="true" aria-controls="pg-panel-browser" tabindex="0" data-pg-code-tab>Browser Live</button>
        <button id="pg-tab-python" type="button" role="tab" aria-selected="false" aria-controls="pg-panel-python" tabindex="-1" data-pg-code-tab>Python</button>
        <button id="pg-tab-static" type="button" role="tab" aria-selected="false" aria-controls="pg-panel-static" tabindex="-1" data-pg-code-tab>Static C</button>
      </div>
      <button type="button" class="pg-live-expand" data-pg-live-expand aria-label="Expand demo" title="Expand demo">⤢</button>
    </div>
    <div>
      <div id="pg-panel-browser" class="pg-code-panel pg-live" role="tabpanel" aria-labelledby="pg-tab-browser" data-pg-live>
        <div class="pg-psql" data-pg-psql>
          <div class="pg-psql-frame">
            <div class="pg-psql-screen" data-pg-psql-terminal aria-label="Interactive psql terminal" aria-busy="true">Loading PostgreSQL…</div>
            <div class="pg-psql-loading" data-pg-psql-loading>
              <div class="pg-psql-loading-title"><span class="pg-psql-spinner" aria-hidden="true"></span><strong data-pg-psql-loading-title>Starting PostgreSQL</strong></div>
              <div class="pg-psql-percentage" data-pg-psql-loading-percent>~2%</div>
              <div class="pg-psql-progress" data-pg-psql-progress role="progressbar" aria-label="Estimated startup progress" aria-valuemin="0" aria-valuemax="100" aria-valuenow="2"><span></span></div>
              <p data-pg-psql-loading-detail>Preparing your browser</p>
              <div class="pg-psql-loading-note"><span>Estimated</span><span aria-hidden="true">·</span><span data-pg-psql-loading-time>0s</span></div>
            </div>
          </div>
          <noscript>Enable JavaScript to use the psql terminal.</noscript>
          <div class="pg-live-footer pg-psql-footer">
            <span data-pg-psql-status role="status" aria-live="polite" data-state="loading">Loading PostgreSQL…</span>
            <span class="pg-psql-hint">In-memory · \? for help</span>
            <div class="pg-psql-controls">
              <button type="button" data-pg-psql-cancel disabled>Ctrl+C</button>
              <button type="button" data-pg-psql-reset disabled title="Discard this database and start a new one">Reset</button>
              <button type="button" data-pg-psql-close disabled>Close</button>
            </div>
          </div>
        </div>
        <div data-pg-legacy hidden>
        <div class="pg-live-toolbar">
          <label for="pg-live-example">Explore</label>
          <select id="pg-live-example" aria-label="SQL example" data-pg-live-example>
            <option value="sql">SQL &amp; aggregation</option>
            <option value="json">JSONB documents</option>
            <option value="transaction">Transactions</option>
            <option value="vector">Vector search</option>
          </select>
          <span class="pg-live-badge">Preview</span>
        </div>
        <label class="pg-live-sr" for="pg-live-sql">SQL editor</label>
        <textarea id="pg-live-sql" data-pg-live-sql spellcheck="false" autocapitalize="off" autocomplete="off" aria-describedby="pg-live-hint">SELECT category,
       count(*) AS orders,
       sum(total) AS revenue
FROM demo_orders
GROUP BY category
ORDER BY revenue DESC;</textarea>
        <div class="pg-live-actions">
          <button type="button" class="pg-live-run" data-pg-live-run>Start &amp; run</button>
          <button type="button" data-pg-live-cancel disabled>Cancel</button>
          <button type="button" data-pg-live-reset disabled title="Create a fresh in-memory database">Reset</button>
          <button type="button" data-pg-live-close disabled title="Close the database and release its resources">Close</button>
          <span id="pg-live-hint">Ctrl / ⌘ ↵</span>
        </div>
        <div class="pg-live-results" data-pg-live-results tabindex="0" aria-label="Query results">
          <div class="pg-live-empty"><span class="pg-live-prompt" aria-hidden="true">&gt;_</span><strong>Your browser. A real PostgreSQL database.</strong><p>Edit the SQL, then start the demo.<br>Sample data stays in memory for this visit.</p></div>
        </div>
        <div class="pg-live-footer"><span data-pg-live-status role="status" aria-live="polite" data-state="idle">Not started</span><span data-pg-live-detail>Runs locally in your browser</span></div>
        </div>
        <noscript><p>Enable JavaScript to run the browser demo. The Python and C quickstarts are available in the documentation.</p></noscript>
      </div>
      <div id="pg-panel-python" class="pg-code-panel" role="tabpanel" aria-labelledby="pg-tab-python" tabindex="0" hidden>
<pre><code><span class="pg-code-keyword">import</span> postgamma

<span class="pg-code-keyword">with</span> postgamma.connect(<span class="pg-code-string">"agent.pgm"</span>,
                       autocommit=<span class="pg-code-value">True</span>) <span class="pg-code-keyword">as</span> db:
    db.execute(<span class="pg-code-string">"create table notes "</span>
               <span class="pg-code-string">"(id bigint, body text)"</span>)
    db.execute(<span class="pg-code-string">"insert into notes values ($1, $2)"</span>,
               [<span class="pg-code-value">1</span>, <span class="pg-code-string">"local memory"</span>])
    rows = db.execute(<span class="pg-code-string">"select * from notes"</span>).fetchall()

<span class="pg-code-comment"># [(1, 'local memory')]</span></code></pre>
      </div>
      <div id="pg-panel-static" class="pg-code-panel" role="tabpanel" aria-labelledby="pg-tab-static" tabindex="0" hidden>
<pre><code><span class="pg-code-keyword">#include</span> &lt;postgamma/postgamma.h&gt;

<span class="pg-code-keyword">int</span> main(<span class="pg-code-keyword">void</span>) {
    pgm_instance_options opts = PGM_INSTANCE_OPTIONS_INIT;
    pgm_connection_options session = PGM_CONNECTION_OPTIONS_INIT;
    pgm_instance *engine = <span class="pg-code-value">NULL</span>;
    pgm_connection *db = <span class="pg-code-value">NULL</span>;
    pgm_result *rows = <span class="pg-code-value">NULL</span>;
    pgm_error *error = <span class="pg-code-value">NULL</span>;

    opts.path = <span class="pg-code-string">"agent.pgm"</span>;
    opts.create = <span class="pg-code-value">1</span>;
    pgm_instance_open(&amp;opts, &amp;engine, &amp;error);
    pgm_connection_open(engine, &amp;session, &amp;db, &amp;error);
    pgm_execute(db, <span class="pg-code-string">"select 'local memory'"</span>, <span class="pg-code-value">NULL</span>, <span class="pg-code-value">0</span>,
                PGM_FORMAT_TEXT, <span class="pg-code-value">-1</span>, &amp;rows, &amp;error);

    pgm_result_free(rows);
    pgm_connection_close(db, <span class="pg-code-value">-1</span>, &amp;error);
    pgm_instance_close(engine, PGM_SHUTDOWN_SMART, <span class="pg-code-value">-1</span>, &amp;error);
}</code></pre>
      </div>
    </div>
  </div>
</header>

<section class="pg-proof-strip" aria-label="Technical support baseline">
  <div><strong>19</strong><span>PostgreSQL core</span></div>
  <div><strong>3.10–3.14</strong><span>CPython wheels</span></div>
  <div><strong>2.17+</strong><span>Wheel glibc baseline</span></div>
  <div><strong>C ABI</strong><span>Static SDK</span></div>
  <div><strong>0.8.6</strong><span>Bundled pgvector</span></div>
</section>

<main>
  <section id="sdks" class="pg-v3-section pg-v3-sdks" aria-labelledby="sdks-heading">
    <div class="pg-v3-section-head pg-v3-section-head--compact">
      <div>
        <p class="pg-v3-kicker">Embedded, Server and Browser</p>
        <h2 id="sdks-heading">Choose how you run PostgreSQL.</h2>
      </div>
      <p>Own the database inside your app, connect to a threaded server, or run SQL locally in the browser.</p>
    </div>
    <div class="pg-v3-sdk-grid pg-sdk-three">
      <article class="pg-v3-sdk pg-v3-sdk--embedded">
        <div class="pg-sdk-head"><span>API</span><div><strong>Embedded</strong><small>Python + Static C</small></div><em>Alpha candidate</em></div>
        <h3>PostgreSQL inside<br>your application.</h3>
        <p>Import the Python package or link <code>libpostgamma.a</code>. Your app owns the database lifecycle, with no separate server to manage. See <a href="downloads/">downloads</a> for build and release availability.</p>
        <div class="pg-install-line"><pre><code>postgamma.connect("agent.pgm")</code></pre><span aria-hidden="true">&gt;</span></div>
        <div class="pg-sdk-links"><a class="pg-action pg-action--primary" href="getting-started/">Python guide</a><a class="pg-inline-link" href="getting-started/c-static/">C guide ↗</a></div>
      </article>
      <article class="pg-v3-sdk pg-v3-sdk--server">
        <div class="pg-sdk-head"><span>PG</span><div><strong>Server</strong><small>Threaded PostgreSQL</small></div><em>Source build</em></div>
        <h3>A familiar server.<br>A threaded engine.</h3>
        <p>Run PostgreSQL roles as threads in a standalone server. Connect with psql or a PostgreSQL driver. Build from source today; a separate server binary package is not yet available.</p>
        <div class="pg-install-line"><pre><code>make threaded-postgresql-build</code></pre><span aria-hidden="true">$</span></div>
        <div class="pg-sdk-links"><a class="pg-action pg-action--secondary" href="server/quickstart/">Build from source</a><a class="pg-inline-link" href="server/">Server guide ↗</a></div>
      </article>
      <article class="pg-v3-sdk pg-v3-sdk--browser">
        <div class="pg-sdk-head"><span>JS</span><div><strong>Browser</strong><small>WebAssembly + Workers</small></div><em>Preview</em></div>
        <h3>Real PostgreSQL.<br>Right in your browser.</h3>
        <p>Run SQL, JSONB, transactions and pgvector locally. Start in memory, or explicitly enable OPFS storage. The browser package is currently built from source.</p>
        <div class="pg-install-line"><pre><code>const db = await Database.open();</code></pre><span aria-hidden="true">&gt;</span></div>
        <div class="pg-sdk-links"><a class="pg-action pg-action--primary" href="#live-demo" data-pg-try-live>Try it live</a><a class="pg-inline-link" href="getting-started/browser/">Browser guide ↗</a></div>
      </article>
    </div>
  </section>

  <section id="architecture" class="pg-v3-section pg-v3-architecture" aria-labelledby="architecture-heading">
    <div class="pg-v3-section-head">
      <div>
        <p class="pg-v3-kicker">The PostgreSQL core, reframed</p>
        <h2 id="architecture-heading">Keep the database semantics.<br><em>Choose the runtime boundary.</em></h2>
      </div>
      <p>The threaded kernel powers each mode. PostgreSQL owns parsing, planning, catalogs, transactions, WAL, and recovery; you choose an embedded API, a server connection, or a browser Worker.</p>
    </div>

    <div class="pg-v3-bento">
      <article class="pg-v3-card pg-v3-card--core">
        <div class="pg-v3-card-index"><span>01</span> PostgreSQL behavior, preserved</div>
        <h3>Use the SQL and semantics you already know.</h3>
        <p>Preserved behavior includes PostgreSQL syntax and types, SQLSTATE diagnostics, prepared statements, COPY, notices, notifications, and logical dump and restore.</p>
        <div class="pg-capability-row" aria-label="PostgreSQL capabilities">
          <span>SQL</span><span>ACID</span><span>JSONB</span><span>COPY</span><span>WAL</span><span>Vector</span>
        </div>
        <div class="pg-query-proof" aria-label="Example query result">
          <div><b>postgres=#</b><code>select id, body from notes;</code></div>
          <div><span>1</span><span>local memory</span><i>1 row</i></div>
        </div>
        <a class="pg-inline-link" href="compatibility/postgresql/">Compatibility contract <span aria-hidden="true">↗</span></a>
      </article>

      <article class="pg-v3-card pg-v3-card--process">
        <div class="pg-v3-card-index"><span>02</span> Embedded mode</div>
        <h3>One process.<br>One lifecycle.</h3>
        <p>No database server to install, configure, or supervise.</p>
        <div class="pg-process-frame" aria-label="Application and PostgreSQL share one process">
          <span class="pg-process-label">OS process</span>
          <div><small>Host</small><strong>Your app</strong></div>
          <i aria-hidden="true"><span></span></i>
          <div><small>Engine</small><strong>PostgreSQL</strong></div>
        </div>
        <a class="pg-inline-link" href="concepts/architecture/">See the architecture <span aria-hidden="true">↗</span></a>
      </article>

      <article class="pg-v3-card pg-v3-card--lifecycle">
        <div class="pg-v3-card-index"><span>03</span> Embedded lifecycle</div>
        <h3>Open, connect, query, and close explicitly.</h3>
        <ol class="pg-mini-flow">
          <li><span>01</span><strong>Open</strong><small>initialize or recover</small></li>
          <li><span>02</span><strong>Connect</strong><small>create logical sessions</small></li>
          <li><span>03</span><strong>Query</strong><small>SQL, COPY, vectors</small></li>
          <li><span>04</span><strong>Close</strong><small>clean shutdown</small></li>
        </ol>
        <a class="pg-inline-link" href="getting-started/database-paths/">Paths and creation <span aria-hidden="true">↗</span></a>
      </article>

      <article class="pg-v3-card pg-v3-card--extension">
        <div class="pg-v3-card-index"><span>04</span> Reviewed extension surface</div>
        <div class="pg-extension-copy">
          <div>
            <h3>Vector search,<br>inside the same process.</h3>
            <p>pgvector 0.8.6 is bundled and validated with the embedded kernel. Only reviewed extensions are linked into each build.</p>
          </div>
          <div class="pg-vector-metric"><strong>0.8.6</strong><span>pgvector</span><i>bundled</i></div>
        </div>
        <a class="pg-inline-link" href="extensions/pgvector/">Build vector search <span aria-hidden="true">↗</span></a>
      </article>
    </div>
  </section>

  <section id="use-cases" class="pg-v3-section pg-v3-fit" aria-labelledby="use-cases-heading">
    <div class="pg-fit-intro">
      <p class="pg-v3-kicker">Embedded and browser use cases</p>
      <h2 id="use-cases-heading">Put PostgreSQL where a separate service does not fit.</h2>
      <div class="pg-choice-note">
        <span>Choose deliberately</span>
        <p>Use embedded mode when the app owns its database. For independent clients, explore the <a href="server/">threaded server source build</a>. Each native cluster directory still has one owner process.</p>
        <a class="pg-inline-link" href="getting-started/why-postgamma/">Read the selection guide <span aria-hidden="true">↗</span></a>
      </div>
    </div>
    <div class="pg-use-list">
      <article><span>01</span><div><h3>Agents and developer tools</h3><p>Structured state, task data, and vector search beside the application.</p></div><i aria-hidden="true">↗</i></article>
      <article><span>02</span><div><h3>Desktop and edge software</h3><p>A PostgreSQL data layer that ships with the application itself.</p></div><i aria-hidden="true">↗</i></article>
      <article><span>03</span><div><h3>Tests and reproducible jobs</h3><p>Isolated PostgreSQL clusters through an API, without provisioning a server.</p></div><i aria-hidden="true">↗</i></article>
    </div>
  </section>

  <section class="pg-v3-final" aria-labelledby="final-heading">
    <div>
      <p class="pg-v3-kicker">Start small. Keep PostgreSQL.</p>
      <h2 id="final-heading">Give your application<br>a path and ordinary SQL.</h2>
    </div>
    <div>
      <p>The quickstart shows what PostGamma creates, where the cluster lives, and how to reopen it safely.</p>
      <div class="pg-v3-actions"><a class="pg-action pg-action--primary" href="getting-started/">Build your first app <span aria-hidden="true">↗</span></a><a class="pg-action pg-action--secondary" href="downloads/">Download</a></div>
    </div>
  </section>
</main>

<footer class="pg-product-footer">
  <a class="pg-brand" href="./" aria-label="postgamma home"><span class="pg-brand-mark" aria-hidden="true"></span><span class="pg-brand-wordmark">postgamma</span></a>
  <p class="pg-product-copyright">Copyright &copy; 2026 Shujie Zhang</p>
  <div><a href="getting-started/">Docs</a><a href="downloads/">Downloads</a><a href="about/license/">Apache-2.0</a></div>
</footer>

</div>
