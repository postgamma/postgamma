/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database} from './src/api/database.js';

// The same three documents as examples/python/vector_search.py.
export async function createVectorDatabase(name = 'vector-example'): Promise<Database> {
  const db = await Database.open({storage: {type: 'opfs', name}});
  try {
    const c = await db.connect('vector-example');
    try {
      await c.query('CREATE EXTENSION IF NOT EXISTS vector');
      await c.query('CREATE TABLE IF NOT EXISTS documents (id bigint PRIMARY KEY, label text NOT NULL, embedding vector(3) NOT NULL)');
      await c.query("INSERT INTO documents VALUES (1,'alpha','[1,1,1]'), (2,'beta','[2,2,2]'), (3,'gamma','[3,3,3]') ON CONFLICT (id) DO NOTHING");
      await c.query('CREATE INDEX IF NOT EXISTS documents_embedding_hnsw ON documents USING hnsw (embedding vector_l2_ops)');
    } finally {await c.close();}
    return db;
  } catch (error) {await db.close(); throw error;}
}

export async function queryVectors(db: Database, coordinates: number[]) {
  if (coordinates.length !== 3 || !coordinates.every(Number.isFinite)) throw new Error('Enter three finite numbers.');
  const c = await db.connect('vector-search');
  try {
    const results = await c.query('SELECT id, label, embedding <-> $1::vector AS distance FROM documents ORDER BY embedding <-> $1::vector LIMIT 3',
      {parameters: [{typeOid: 0, value: JSON.stringify(coordinates)}]});
    return results[0].rows;
  } finally {await c.close();}
}

const form = document.querySelector<HTMLFormElement>('#vector-search');
if (form) {
  const open = document.querySelector<HTMLButtonElement>('#open')!, close = document.querySelector<HTMLButtonElement>('#close')!;
  const search = document.querySelector<HTMLButtonElement>('#search')!, status = document.querySelector<HTMLElement>('#status')!;
  const output = document.querySelector<HTMLElement>('#results')!;
  let db: Database | undefined;
  async function work(action: () => Promise<void>) {
    open.disabled = close.disabled = search.disabled = true;
    try {await action();} catch (error) {status.textContent = String(error);}
    finally {open.disabled = !!db; close.disabled = search.disabled = !db;}
  }
  open.onclick = () => void work(async () => {status.textContent = 'Opening database…'; db = await createVectorDatabase(); status.textContent = 'Ready. Your documents are stored in this browser.';});
  close.onclick = () => void work(async () => {await db?.close(); db = undefined; status.textContent = 'Database closed. Open it again to reuse your documents.';});
  form.onsubmit = event => {
    event.preventDefault();
    void work(async () => {
      status.textContent = 'Searching…';
      const values = JSON.parse(document.querySelector<HTMLInputElement>('#coordinates')!.value);
      if (!Array.isArray(values)) throw new Error('Enter a JSON array of three numbers.');
      const rows = await queryVectors(db!, values); output.replaceChildren();
      for (const row of rows) {
        const tr = document.createElement('tr');
        for (const value of row) {const td = document.createElement('td'); td.textContent = String(value); tr.append(td);}
        output.append(tr);
      }
      status.textContent = 'Search complete.';
    });
  };
}
