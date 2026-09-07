# PostgreSQL inside your application. { #pg-v3-heading .pg-document-title }

<div class="pg-home pg-home-v3">

<nav class="pg-product-nav" aria-label="Homepage navigation">
  <a class="pg-brand" href="./" aria-label="postgamma home">
    <span class="pg-brand-mark" aria-hidden="true"></span>
    <span class="pg-brand-wordmark">postgamma</span>
  </a>
  <div class="pg-product-links">
    <a href="#architecture">Architecture</a>
    <a href="#sdks">SDKs</a>
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
    <p class="pg-v3-eyebrow"><span></span> PostgreSQL 19 · embedded runtime</p>
    <div class="pg-v3-display" aria-hidden="true"><span class="pg-v3-morph" data-pg-word-morph>PostgreSQL</span><strong>inside your application.</strong></div>
    <p class="pg-v3-lead">Run PostgreSQL in your application process—with transactions, WAL, crash recovery, and bundled extensions, but no database server to install or supervise.</p>
    <div class="pg-v3-actions">
      <a class="pg-action pg-action--primary" href="getting-started/">Build your first app <span aria-hidden="true">↗</span></a>
      <a class="pg-action pg-action--secondary" href="concepts/architecture/">Explore the architecture</a>
    </div>
    <ul class="pg-v3-facts" aria-label="Supported platform and license">
      <li><span aria-hidden="true"></span> Linux x86-64</li>
      <li><span aria-hidden="true"></span> glibc 2.28+</li>
      <li><span aria-hidden="true"></span> PostGamma · Apache-2.0</li>
    </ul>
  </div>

  <div class="pg-v3-demo" aria-label="PostGamma API quickstart" data-pg-code-switcher>
    <div class="pg-demo-head">
      <div class="pg-demo-dots" aria-hidden="true"><i></i><i></i><i></i></div>
      <div class="pg-code-tabs" role="tablist" aria-label="Choose an SDK quickstart" data-pg-code-tabs>
        <button id="pg-tab-python" type="button" role="tab" aria-selected="true" aria-controls="pg-panel-python" tabindex="0" data-pg-code-tab>Python</button>
        <button id="pg-tab-static" type="button" role="tab" aria-selected="false" aria-controls="pg-panel-static" tabindex="-1" data-pg-code-tab>Static C</button>
      </div>
      <span class="pg-demo-file">quickstart</span>
    </div>
    <div>
      <div id="pg-panel-python" class="pg-code-panel" role="tabpanel" aria-labelledby="pg-tab-python" tabindex="0">
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
    <div class="pg-demo-foot">
      <span><i aria-hidden="true"></i> Embedded cluster ready</span>
      <code>no SQL socket</code>
    </div>
    <div class="pg-runtime-line" aria-label="Your process contains the PostgreSQL engine">
      <span>Your process</span><i aria-hidden="true"></i><strong>PostgreSQL 19</strong>
    </div>
  </div>
</header>

<section class="pg-proof-strip" aria-label="Technical support baseline">
  <div><strong>19</strong><span>PostgreSQL core</span></div>
  <div><strong>3.10–3.14</strong><span>CPython wheels</span></div>
  <div><strong>2.28+</strong><span>glibc baseline</span></div>
  <div><strong>C ABI</strong><span>Static SDK</span></div>
  <div><strong>0.8.6</strong><span>Bundled pgvector</span></div>
</section>

<main>
  <section id="sdks" class="pg-v3-section pg-v3-sdks" aria-labelledby="sdks-heading">
    <div class="pg-v3-section-head pg-v3-section-head--compact">
      <div>
        <p class="pg-v3-kicker">Python and C</p>
        <h2 id="sdks-heading">Choose the interface that fits.</h2>
      </div>
      <p>Both SDKs bundle the same PostgreSQL kernel and version-matched resources. Neither requires an installed PostgreSQL server.</p>
    </div>
    <div class="pg-v3-sdk-grid">
      <article class="pg-v3-sdk pg-v3-sdk--python">
        <div class="pg-sdk-head"><span>PY</span><div><strong>Python SDK</strong><small>Native CPython wheel</small></div><em>3.10–3.14</em></div>
        <h3>Install once.<br>Import normally.</h3>
        <p>The wheel bundles the Python API, PostgreSQL kernel, and version-matched resources for each supported CPython ABI.</p>
        <div class="pg-install-line"><pre><code>python -m pip install postgamma</code></pre><span aria-hidden="true">$</span></div>
        <div class="pg-sdk-links"><a class="pg-action pg-action--primary" href="getting-started/">Python quickstart</a><a class="pg-inline-link" href="python/">API reference ↗</a></div>
      </article>
      <article class="pg-v3-sdk pg-v3-sdk--c">
        <div class="pg-sdk-head"><span>C</span><div><strong>Static C SDK</strong><small>Stable public ABI</small></div><em>Linux x86-64</em></div>
        <h3>Link PostgreSQL<br>into native software.</h3>
        <p>Link against <code>libpostgamma.a</code>; its stable public ABI keeps PostgreSQL implementation types private.</p>
        <div class="pg-install-line"><pre><code>pkg-config --static --libs postgamma</code></pre><span aria-hidden="true">$</span></div>
        <div class="pg-sdk-links"><a class="pg-action pg-action--secondary" href="getting-started/c-static/">Static C quickstart</a><a class="pg-inline-link" href="c/">C API ↗</a></div>
      </article>
    </div>
  </section>

  <section id="architecture" class="pg-v3-section pg-v3-architecture" aria-labelledby="architecture-heading">
    <div class="pg-v3-section-head">
      <div>
        <p class="pg-v3-kicker">The PostgreSQL core, reframed</p>
        <h2 id="architecture-heading">Keep the database semantics.<br><em>Lose the service boundary.</em></h2>
      </div>
      <p>PostGamma changes PostgreSQL’s deployment model while preserving its database behavior. Your application owns startup and shutdown; PostgreSQL still owns parsing, planning, catalogs, transactions, WAL, and recovery.</p>
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
        <div class="pg-v3-card-index"><span>02</span> In-process</div>
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
        <div class="pg-v3-card-index"><span>03</span> Explicit ownership</div>
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
      <p class="pg-v3-kicker">Built for local software</p>
      <h2 id="use-cases-heading">Put PostgreSQL where a separate service does not fit.</h2>
      <div class="pg-choice-note">
        <span>Choose deliberately</span>
        <p>PostGamma is not a universal replacement for a PostgreSQL server. Each cluster directory can be owned by only one live OS process at a time.</p>
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
