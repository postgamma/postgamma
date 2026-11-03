/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, Codecs} from '@postgamma/wasm';
const output = document.querySelector('pre');
let db, result;
try {
  db = await Database.open({assetBase: new URL('./postgamma/', location.href)});
  const connection = await db.connect();
  try {
    const codecs = new Codecs();
    const rows = await connection.query('SELECT $1::int8 AS exact, version()', {
      parameters: [codecs.parameter(20, 9007199254740993n)], timeoutMs: 5000,
    });
    output.textContent = JSON.stringify(rows.map(row => codecs.decode(row)),
      (_, value) => typeof value === 'bigint' ? String(value) : value, 2);
    result = {status: 'passed', value: String(codecs.decode(rows[0]).rows[0][0])};
  } finally {await connection.close();}
} catch (error) {
  output.textContent = String(error); result = {status: 'failed', error: String(error)};
} finally {await db?.close(); window.postgammaExample = result;}
