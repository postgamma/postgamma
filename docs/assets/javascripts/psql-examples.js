/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
// Loaded only when the live terminal is expanded. Every command goes through
// the existing psql session; examples do not open another database or connection.
const catalog = [
  ['sql', 'SQL & aggregation', 'Group a small order table and calculate revenue.', 'Books: 64 · Tools: 138 · Music: 42'],
  ['json', 'JSONB documents', 'Filter JSON documents with PostgreSQL containment and extract fields.', 'The local-first customer is Ada.'],
  ['transaction', 'Transactions', 'Update a balance, then undo it with a savepoint.', 'Balance: 100 → 60 → 100'],
  ['vector', 'Vector search', 'Rank three local vectors with pgvector. No embedding service needed.', 'Nearest match: PostgreSQL in the browser'],
  ['parallel', 'Parallel query', 'Build 20,000 rows and inspect an actual parallel aggregate.', 'EXPLAIN ANALYZE must show 2 workers planned and launched.']
];

function example(id) {
  const name = 'pg_live_' + crypto.randomUUID().replaceAll('-', '').slice(0, 16);
  const table = (id === 'parallel' ? 'public.' : '') + name;
  const steps = [['Begin', 'BEGIN;'], ['Bound query time', "SET LOCAL statement_timeout = '15s';"]];
  const verify = condition => ['Check result', `DO $$ BEGIN IF (${condition}) IS DISTINCT FROM TRUE THEN RAISE EXCEPTION 'Example result differs from expected'; END IF; END $$;`];
  let cleanup = ['ROLLBACK;'];
  if (id === 'sql') steps.push(
    ['Create sample', `CREATE TEMP TABLE ${table} (category text, total integer);`],
    ['Insert orders', `INSERT INTO ${table} VALUES ('Books',29),('Books',35),('Tools',89),('Tools',49),('Music',18),('Music',24);`],
    ['Aggregate', `SELECT category, count(*) AS orders, sum(total) AS revenue FROM ${table} GROUP BY category ORDER BY revenue DESC;`],
    verify(`(SELECT sum(total) FROM ${table}) = 244 AND (SELECT count(DISTINCT category) FROM ${table}) = 3`));
  if (id === 'json') steps.push(
    ['Create documents', `CREATE TEMP TABLE ${table} (document jsonb);`],
    ['Insert documents', `INSERT INTO ${table} VALUES ('{"name":"Ada","tags":["local-first","postgres"]}'), ('{"name":"Lin","tags":["analytics"]}');`],
    ['Find documents', `SELECT document->>'name' AS customer, document->'tags' AS tags FROM ${table} WHERE document @> '{"tags":["local-first"]}';`],
    verify(`(SELECT string_agg(document->>'name', ',') FROM ${table} WHERE document @> '{"tags":["local-first"]}') = 'Ada'`));
  if (id === 'transaction') steps.push(
    ['Create account', `CREATE TEMP TABLE ${table} (balance integer);`],
    ['Initial balance', `INSERT INTO ${table} VALUES (100);`],
    ['Show balance', `SELECT balance FROM ${table};`],
    ['Savepoint', 'SAVEPOINT before_purchase;'],
    ['Purchase', `UPDATE ${table} SET balance = balance - 40 RETURNING balance;`],
    ['Undo purchase', 'ROLLBACK TO before_purchase;'],
    ['Restored balance', `SELECT balance FROM ${table};`],
    verify(`(SELECT balance FROM ${table}) = 100`));
  if (id === 'vector') steps.push(
    ['Enable pgvector', 'CREATE EXTENSION IF NOT EXISTS vector;'],
    ['Create vectors', `CREATE TEMP TABLE ${table} (title text, embedding vector(3));`],
    ['Insert vectors', `INSERT INTO ${table} VALUES ('PostgreSQL in the browser','[1,0,0]'),('Local vector search','[0.8,0.2,0]'),('Native applications','[0,0,1]');`],
    ['Nearest neighbours', `SELECT title, round((embedding <-> '[1,0,0]')::numeric, 3) AS distance FROM ${table} ORDER BY embedding <-> '[1,0,0]';`],
    verify(`(SELECT title FROM ${table} ORDER BY embedding <-> '[1,0,0]' LIMIT 1) = 'PostgreSQL in the browser'`));
  if (id === 'parallel') {
    // Parallel workers cannot scan temporary tables. Commit this uniquely named
    // sample first, and drop only that sample during cleanup, including on cancel.
    steps.unshift(
      ['Create sample', `CREATE TABLE ${table} AS SELECT i AS id FROM generate_series(1, 20000) i;`],
      ['Request two workers', `ALTER TABLE ${table} SET (parallel_workers = 2);`],
      ['Collect statistics', `ANALYZE ${table};`]);
    steps.push(
      ['Enable parallel plan', 'SET LOCAL max_parallel_workers = 2; SET LOCAL max_parallel_workers_per_gather = 2; SET LOCAL min_parallel_table_scan_size = 0; SET LOCAL parallel_setup_cost = 0; SET LOCAL parallel_tuple_cost = 0;'],
      ['Inspect actual workers', `EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT sum(id) FROM ${table};`, output => /Workers Planned: 2\b/.test(output) && /Workers Launched: 2\b/.test(output)],
      ['Show aggregate', `SELECT sum(id) FROM ${table};`],
      verify(`(SELECT sum(id) FROM ${table}) = 200010000`));
    cleanup.push(`DROP TABLE IF EXISTS ${table};`);
  }
  return {id, steps, cleanup};
}

export class PsqlExamples {
  constructor(panel, hooks) {
    this.hooks = hooks;
    this.el = name => panel.querySelector('[data-pg-example-' + name + ']');
    this.results = []; this.run = null;
    const list = panel.querySelector('[data-pg-examples-list]');
    for (const [id, title] of catalog) {
      const button = document.createElement('button'); button.type = 'button';
      button.textContent = title; button.dataset.example = id;
      button.addEventListener('click', () => this.select(id)); list.append(button);
    }
    this.buttons = [...list.children];
    this.el('insert').addEventListener('click', () => void this.insert());
    this.el('run').addEventListener('click', () => void this.start([this.selected.id]));
    this.el('check').addEventListener('click', () => void this.start(['sql', 'json', 'transaction', 'vector']));
    this.select('sql');
  }
  get busy() {return !!this.run;}
  snapshot() {return {running: this.busy, results: this.results.map(row => ({...row}))};}
  select(id) {
    if (this.busy) return;
    this.selected = example(id);
    const [, , description, expected] = catalog.find(row => row[0] === id);
    for (const button of this.buttons) button.setAttribute('aria-pressed', String(button.dataset.example === id));
    this.el('description').textContent = description;
    this.el('expected').textContent = expected;
    this.el('sql').textContent = this.commands(this.selected).join('\n');
    this.refresh();
  }
  commands(item) {return [...item.steps.map(step => step[1]), ...item.cleanup];}
  refresh() {
    const reason = this.hooks.unavailable(), disabled = this.busy || !!reason;
    for (const name of ['insert', 'run', 'check']) {
      const button = this.el(name); if (button.disabled !== disabled) button.disabled = disabled;
    }
    for (const button of this.buttons) if (button.disabled !== this.busy) button.disabled = this.busy;
    if (!this.busy) {
      const text = reason || 'Uses this session. Sample data is cleaned up after running.';
      if (this.el('status').textContent !== text) this.el('status').textContent = text;
    }
  }
  cancel() {this.run?.abort();}
  async insert() {
    if (this.busy || this.hooks.unavailable()) return;
    try {
      // One editable line: inserting a script never submits its earlier lines.
      await this.hooks.insert(this.commands(this.selected).join(' '));
      this.hooks.focus();
    } catch (error) {this.el('status').textContent = error.message;}
  }
  renderResults() {
    const list = this.el('results'); list.hidden = false; list.replaceChildren();
    for (const row of this.results) {
      const li = document.createElement('li'); li.dataset.state = row.status;
      const title = catalog.find(item => item[0] === row.id)[1];
      li.textContent = (row.status === 'passed' ? '✓ ' : row.status === 'running' ? '◌ ' : '× ') + title + ' — ' + row.status +
        (row.elapsedMs === undefined ? '' : ' · ' + (row.elapsedMs / 1000).toFixed(1) + 's');
      if (row.error) {const detail = document.createElement('small'); detail.textContent = row.error; li.append(detail);}
      list.append(li);
    }
    list.scrollIntoView({block: 'nearest', inline: 'nearest'});
  }
  async start(ids) {
    if (this.busy || this.hooks.unavailable()) return;
    const controller = new AbortController(), owner = this.hooks.owner(); this.run = controller; this.results = [];
    this.refresh();
    try {
      for (const id of ids) {
        if (controller.signal.aborted) break;
        const item = ids.length === 1 ? this.selected : example(id);
        const row = {id, status: 'running', step: 'Starting'}; this.results.push(row); this.renderResults();
        const started = performance.now();
        try {
          for (const [label, sql, check] of item.steps) {
            controller.signal.throwIfAborted(); row.step = label;
            this.el('status').textContent = catalog.find(item => item[0] === id)[1] + ' · ' + label + '…';
            const output = await this.hooks.execute(sql, owner);
            controller.signal.throwIfAborted();
            if (check && !check(output)) throw new Error('The plan did not launch two parallel workers. See EXPLAIN output.');
          }
          row.status = 'passed';
        } catch (error) {
          row.status = controller.signal.aborted ? 'cancelled' : 'failed';
          row.error = controller.signal.aborted ? 'Interrupted by user.' : error.message.slice(0, 600);
        } finally {
          // Keep ownership of terminal input until cleanup finishes. Never run
          // cleanup in a replacement session after Reset or Close.
          if (this.hooks.connected(owner)) {
            try {for (const sql of item.cleanup) await this.hooks.execute(sql, owner);}
            catch (error) {row.status = 'failed'; row.error = (row.error ? row.error + ' ' : '') + 'Cleanup: ' + error.message.slice(0, 300);}
          }
          if (controller.signal.aborted && row.status === 'passed') {
            row.status = 'cancelled'; row.error = 'Interrupted by user.';
          }
          row.elapsedMs = Math.round(performance.now() - started); this.renderResults();
        }
        if (row.status !== 'passed') break;
      }
    } finally {
      this.run = null; this.refresh();
    }
  }
}
