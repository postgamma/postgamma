/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database} from '../src/api/database.js';
import {ArchiveEncoder} from '../src/storage/archive.js';
import {Writer} from '../src/protocol/wire.js';
import {assert, scalar} from '../tests/helpers.js';

async function rejected(work: Promise<unknown>, pattern: RegExp) {
  try {await work;} catch (error) {assert(pattern.test(String(error)), `Expected ${pattern}, got ${error}`); return;}
  throw new Error(`Expected rejection: ${pattern}`);
}
async function verify(db: Database, vector: boolean) {
  const c = await db.connect();
  assert(scalar(await c.query("SELECT md5(string_agg(v, '' ORDER BY id)) FROM archive_rows")) ===
    '9e107d9d372bb6826bd81d3542a419d6', 'Archived table contents');
  assert(scalar(await c.query("SELECT nextval('archive_sequence')")) === '42', 'Archived sequence state');
  assert(scalar(await c.query("SELECT encode(lo_get(12345), 'hex')")) === '0001feff', 'Archived large object');
  assert(scalar(await c.query("SELECT count(*) FROM pg_indexes WHERE indexname = 'archive_rows_pkey'")) === '1', 'Archived index');
  if (vector) assert(scalar(await c.query("SELECT v::text FROM archive_vectors ORDER BY v <-> '[1,2,3]' LIMIT 1")) === '[1,2,3]', 'Archived vector and HNSW index');
  await c.query("SELECT setval('archive_sequence', 41)"); await c.close();
}
export async function archive(checks: string[], metrics: unknown[]) {
  const db = await Database.open(), c = await db.connect();
  await c.query('BEGIN');
  await rejected(db.dumpDataDir(), /active transactions/);
  await c.query('ROLLBACK');
  const request = await c.execute('SELECT generate_series(1,1000)', {chunkRows: 1});
  await rejected(db.dumpDataDir(), /active transactions/); await request.close();
  await c.exec("CREATE TABLE archive_rows(id int PRIMARY KEY, v text); INSERT INTO archive_rows VALUES (1,'The quick brown fox jumps over the lazy dog'); CREATE SEQUENCE archive_sequence START 42; SELECT lo_from_bytea(12345, decode('0001feff','hex'))");
  const vector = scalar(await c.query("SELECT count(*) FROM pg_available_extensions WHERE name = 'vector'")) === '1';
  if (vector) await c.exec("CREATE EXTENSION vector; CREATE TABLE archive_vectors(v vector(3)); INSERT INTO archive_vectors VALUES ('[1,2,3]'), ('[4,5,6]'); CREATE INDEX ON archive_vectors USING hnsw(v vector_l2_ops)");
  const stream = await db.dumpDataDir();
  await rejected(c.query('SELECT 1'), /closed/);
  // Only the fixture collects a Blob for damaged/truncated archive variants.
  // The product API and clone below transfer bounded records with backpressure.
  const blob = await new Response(stream).blob();
  metrics.push(await db.close());
  assert(blob.size > 1024 * 1024, 'A complete physical database was archived');
  const storage = {type: 'opfs' as const, name: 'archive-' + crypto.randomUUID()};
  const restored = await Database.loadDataDir(blob.stream(), {storage});
  await verify(restored, vector);
  const clone = await restored.clone(); metrics.push(await restored.close());
  await verify(clone, vector);
  const copied = await clone.connect();
  await copied.query("INSERT INTO archive_rows VALUES (2, 'clone-only')");
  assert(scalar(await copied.query('SELECT count(*) FROM archive_rows')) === '2', 'Clone can diverge independently');
  await copied.close(); metrics.push(await clone.close());
  const original = await Database.open({storage, create: false});
  await verify(original, vector); metrics.push(await original.close());
  await rejected(Database.loadDataDir(blob.stream(), {storage}), /new OPFS database name/);
  const retained = await Database.open({storage, create: false});
  await verify(retained, vector); metrics.push(await retained.close());
  checks.push('archive-active-transaction-rejected', 'archive-active-request-rejected',
    'archive-ephemeral-to-opfs', 'archive-opfs-to-ephemeral-clone', 'archive-clone-independent-writes', 'archive-existing-destination-preserved');
  const bytes = new Uint8Array(await blob.arrayBuffer());
  bytes[bytes.length - 1] ^= 1;
  await rejected(Database.loadDataDir(new Blob([bytes]).stream()), /checksum/);
  await rejected(Database.loadDataDir(blob.slice(0, blob.size - 4).stream()), /Truncated/);
  await rejected(Database.loadDataDir(new Blob([blob, new Uint8Array([1])]).stream()), /Trailing/);
  // Header mismatch must be rejected before creating a destination/module.
  const encoder = new ArchiveEncoder();
  const wrong = await encoder.frame(new TextEncoder().encode('{"format":"wrong"}'));
  await rejected(Database.loadDataDir(new Blob([wrong]).stream()), /identity/);
  const headerSize = new DataView(bytes.buffer).getUint32(0, true);
  // Encode in the correct order so native path validation, not a checksum
  // mismatch, rejects this otherwise well-framed malicious archive record.
  const pathEncoder = new ArchiveEncoder();
  const pathHeader = await pathEncoder.frame(bytes.subarray(36, 36 + headerSize));
  const pathRecord = await pathEncoder.frame(new Uint8Array(new Writer().u32(1).text('../escape').finish()));
  await rejected(Database.loadDataDir(new Blob([pathHeader, pathRecord]).stream()), /Invalid argument/);
  const emptyEncoder = new ArchiveEncoder();
  const emptyHeader = await emptyEncoder.frame(bytes.subarray(36, 36 + headerSize));
  const emptyEnd = await emptyEncoder.frame(new Uint8Array(4));
  const invalidCluster = {type: 'opfs' as const, name: 'archive-invalid-' + crypto.randomUUID()};
  await rejected(Database.loadDataDir(new Blob([emptyHeader, emptyEnd]).stream(), {storage: invalidCluster}), /database|directory|cluster/i);
  await rejected(Database.open({storage: invalidCluster, create: false}), /incomplete restore/);
  const signal = new AbortController();
  let frames = 0, offset = 0;
  const cancelled = {type: 'opfs' as const, name: 'archive-cancel-' + crypto.randomUUID()};
  await rejected(Database.loadDataDir(new ReadableStream({
    async pull(controller) {
      if (++frames === 6) {signal.abort(new Error('fixture cancelled')); return;}
      if (offset === blob.size) {controller.close(); return;}
      const chunk = new Uint8Array(await blob.slice(offset, offset + 65536).arrayBuffer());
      offset += chunk.length; controller.enqueue(chunk);
    },
  }), {storage: cancelled, signal: signal.signal}), /cancelled/);
  await rejected(Database.open({storage: cancelled, create: false}), /incomplete restore/);
  const exportStorage = {type: 'opfs' as const, name: 'archive-export-cancel-' + crypto.randomUUID()};
  const exporting = await Database.open({storage: exportStorage});
  await (await exporting.dumpDataDir()).cancel(); metrics.push(await exporting.close());
  const reopened = await Database.open({storage: exportStorage, create: false});
  const preparing = reopened.dumpDataDir(), closing = reopened.close();
  const closedStream = await preparing; metrics.push(await closing);
  await rejected(closedStream.getReader().read(), /closed/);
  checks.push('archive-checksum', 'archive-truncation', 'archive-trailing-data', 'archive-build-identity',
    'archive-cancelled-restore-not-openable', 'archive-cancelled-export-reopen', 'archive-path-traversal', 'archive-concurrent-close',
    'archive-invalid-cluster-quarantined');
}
