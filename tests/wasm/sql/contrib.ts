/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database} from '../src/api/database.js';
import type {Connection} from '../src/api/database.js';
import {assert, scalar} from './helpers.js';

async function verify(c: Connection) {
  await c.query('SET enable_seqscan = off');
  const matches = await c.query("SELECT name FROM contrib_trgm WHERE name % 'test' ORDER BY name");
  assert(JSON.stringify(matches[0].rows) === JSON.stringify([['test1'], ['test2']]), 'Trigram indexed matching');
  assert(String(scalar(await c.query("EXPLAIN (FORMAT JSON) SELECT name FROM contrib_trgm WHERE name % 'test'"))).includes('contrib_trgm_gin'), 'Actual GIN plan');
  assert(String(scalar(await c.query("EXPLAIN (FORMAT JSON) SELECT name FROM contrib_gist ORDER BY name <-> 'test' LIMIT 2"))).includes('contrib_trgm_gist'), 'Actual GiST KNN plan');
  assert(scalar(await c.query("SELECT name FROM contrib_gist ORDER BY name <-> 'test', name LIMIT 1")) === 'test1', 'GiST distance ordering');
  assert(scalar(await c.query("SELECT id FROM contrib_hstore WHERE data @> 'name=>test1'::hstore")) === '1', 'Hstore GIN lookup');
  assert(String(scalar(await c.query("EXPLAIN (FORMAT JSON) SELECT id FROM contrib_hstore WHERE data @> 'name=>test1'::hstore"))).includes('contrib_hstore_gin'), 'Actual hstore GIN plan');
  assert(scalar(await c.query("SELECT (data->'name') FROM contrib_hstore WHERE id=1")) === 'test1', 'Hstore value');
  assert(scalar(await c.query("SELECT ('a=>NULL, b=>\"\", unicode=>雪'::hstore->'a') IS NULL")) === 't', 'Hstore NULL');
  assert(scalar(await c.query("SELECT 'a=>NULL, b=>\"\", unicode=>雪'::hstore->'unicode'")) === '雪', 'Hstore Unicode');
  await c.query('RESET enable_seqscan');
}

export async function contrib(checks: string[], metrics: unknown[]) {
  const storage = {type: 'opfs' as const, name: 'contrib-' + crypto.randomUUID()};
  const db = await Database.open({storage});
  const connections = await Promise.all(Array.from({length: 4}, () => db.connect()));
  const [a] = connections;
  await a.exec('BEGIN; CREATE EXTENSION pg_trgm; CREATE EXTENSION hstore; ROLLBACK');
  assert(scalar(await a.query("SELECT count(*) FROM pg_extension WHERE extname IN ('pg_trgm','hstore')")) === '0', 'CREATE EXTENSION rolls back');
  await a.exec(`CREATE EXTENSION pg_trgm; CREATE EXTENSION hstore;
    CREATE TABLE contrib_trgm(name text);
    INSERT INTO contrib_trgm VALUES ('test1'), ('test2'), ('text3');
    CREATE INDEX contrib_trgm_gin ON contrib_trgm USING gin(name gin_trgm_ops);
    CREATE TABLE contrib_gist AS TABLE contrib_trgm;
    CREATE INDEX contrib_trgm_gist ON contrib_gist USING gist(name gist_trgm_ops);
    CREATE TABLE contrib_hstore(id integer PRIMARY KEY, data hstore);
    INSERT INTO contrib_hstore VALUES (1,'name=>test1'),(2,'name=>test2'),(3,'name=>test3');
    CREATE INDEX contrib_hstore_gin ON contrib_hstore USING gin(data);`);
  await verify(a);
  const thresholds = [0.1, 0.3, 0.7, 0.9];
  for (let i = 0; i < connections.length; i++) {
    const c = connections[i];
    await c.query("SELECT similarity('test','test')");
    await c.exec(`SET pg_trgm.similarity_threshold = ${thresholds[i]};
      SET pg_trgm.word_similarity_threshold = ${thresholds[i]};
      SET pg_trgm.strict_word_similarity_threshold = ${thresholds[i]}`);
  }
  for (let round = 0; round < 24; round++) await Promise.all(connections.map(async (c, i) => {
    const row = (await c.query(`SELECT pg_sleep(0.01), 'test1' % 'test',
      current_setting('pg_trgm.similarity_threshold'), current_setting('pg_trgm.word_similarity_threshold'),
      current_setting('pg_trgm.strict_word_similarity_threshold')`))[0].rows[0];
    assert(row[1] === (thresholds[i] < 0.57 ? 't' : 'f'), 'Trigram predicate uses logical session GUC');
    assert(row.slice(2).every(value => Number(value) === thresholds[i]), 'All three GUCs remain isolated across carriers');
  }));
  await a.exec('BEGIN; SET LOCAL pg_trgm.similarity_threshold = 0.95; ROLLBACK');
  assert(scalar(await a.query("SHOW pg_trgm.similarity_threshold")) === '0.1', 'Local GUC rolls back');
  await Promise.all(connections.map(c => c.close()));
  const fresh = await db.connect();
  await fresh.query("SELECT similarity('test','test')");
  assert(scalar(await fresh.query('SHOW pg_trgm.similarity_threshold')) === '0.3', 'Reused connection gets the original template');
  await verify(fresh); await fresh.close();
  metrics.push(await db.close());
  const reopened = await Database.open({storage, create: false}), c = await reopened.connect();
  await verify(c); await c.close();
  const clone = await reopened.clone(); metrics.push(await reopened.close());
  const copied = await clone.connect(); await verify(copied); await copied.close(); metrics.push(await clone.close());
  checks.push('contrib-extension-ddl-rollback', 'contrib-pg-trgm-gin', 'contrib-pg-trgm-gist',
    'contrib-hstore-gin', 'contrib-null-unicode', 'contrib-four-session-guc-isolation',
    'contrib-local-guc-rollback', 'contrib-session-reuse', 'contrib-opfs-reopen', 'contrib-archive-roundtrip');
}
