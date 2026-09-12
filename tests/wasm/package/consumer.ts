/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, version, type OpenOptions, type Parameter, type Result} from '@postgamma/wasm';
const options: OpenOptions = {storage: {type: 'opfs', name: 'typed-consumer'}};
const parameter: Parameter = {typeOid: 20, value: '9223372036854775807'};
async function typedConsumer(): Promise<Result[]> {
  const db = await Database.open(options);
  const c = await db.connect(version);
  try {return await c.query('SELECT $1::int8', {parameters: [parameter]});}
  finally {await c.close(); await db.close();}
}
void typedConsumer;
// @ts-expect-error A native pointer is not a supported parameter value.
const invalid: Parameter = {typeOid: 20, value: 123};
void invalid;
