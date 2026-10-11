/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Database} from '../src/api/database.js';
import {Codecs} from '../src/codecs/types.js';
import type {ArrayValue} from '../src/codecs/types.js';
import {assert, scalar} from './helpers.js';
const encoded = (value: unknown) => JSON.stringify(value, (_, item) =>
  typeof item === 'bigint' ? {bigint: String(item)} : item instanceof Uint8Array ? {bytes: [...item]} : item);
const equal = (value: unknown, expected: unknown, message: string) => assert(encoded(value) === encoded(expected), message);

export async function types(db: Database, checks: string[]) {
  const c = await db.connect('typed-values'), codecs = new Codecs();
  await c.exec("SET timezone = 'UTC'; SET datestyle = 'ISO, YMD'");
  const sql = `SELECT true AS b, (-32768)::int2 AS small, 2147483647::int4 AS ordinary,
    9223372036854775807::int8 AS exact, 4294967295::oid AS oid,
    1.25::float4 AS f4, (-2.5)::float8 AS f8, decode('00017f80ff','hex') AS bytes,
    '雪 🐘'::text AS unicode, NULL::int8 AS missing`;
  const expected = [true, -32768, 2147483647, 9223372036854775807n, 4294967295,
    1.25, -2.5, new Uint8Array([0,1,127,128,255]), '雪 🐘', null];
  for (const binary of [false, true]) {
    const result = (await c.query(sql, {binary}))[0];
    equal(codecs.decode(result).rows[0], expected, 'Real PostgreSQL scalar ' + (binary ? 'binary' : 'text') + ' decoding');
    assert(binary ? result.rows[0][3] instanceof Uint8Array : result.rows[0][3] === '9223372036854775807',
      'Raw values remain unchanged after decoding');
  }
  checks.push('codecs-text-binary-scalars', 'codecs-raw-values-preserved');

  const precise = (await c.query(`SELECT 12345678901234567890.123456789::numeric,
    '2000-01-01 00:00:00.123456'::timestamp, 'infinity'::date,
    '{"n":9007199254740993}'::json, '{"n":9007199254740993}'::jsonb`))[0];
  equal(codecs.decode(precise).rows[0], ['12345678901234567890.123456789',
    '2000-01-01 00:00:00.123456', 'infinity', '{"n":9007199254740993}', '{"n": 9007199254740993}'],
    'Numeric, temporal and default JSON decoding preserve precision');
  const json = new Codecs({json: 'javascript'});
  const jsonResult = (await c.query(`SELECT '{"items":[1,null,"NULL"]}'::jsonb`, {binary: true}))[0];
  equal(json.decode(jsonResult).rows[0][0], {items: [1, null, 'NULL']}, 'Opt-in binary JSONB object decoder');
  checks.push('codecs-precision-policy', 'codecs-json-opt-in');

  const bounded: ArrayValue = {values: [[9007199254740993n, null], [1n, -2n]], dimensions: [2, 2], lowerBounds: [0, -2]};
  const escaped = [null, 'NULL', '', 'comma,', '{braces}', '"quoted"', '\\', ' π '];
  const bytea = [new Uint8Array([0, 92, 255]), null, new Uint8Array()];
  const statement = await c.prepare('SELECT $1::int8[], $2::text[], $3::bytea[], $4::text[], $5::int8, $6::numeric');
  try {
    const result = (await statement.query({parameters: [codecs.parameter(1016, bounded),
      codecs.parameter(1009, escaped), codecs.parameter(1001, bytea), codecs.parameter(1009, []),
      codecs.parameter(20, -9223372036854775808n), codecs.parameter(1700, '0.12345678901234567890')]}))[0];
    const row = codecs.decode(result).rows[0];
    equal(row[0], bounded, 'Array lower bounds and multidimensional values survive PostgreSQL');
    equal((row[1] as ArrayValue).values, escaped, 'Array quoting and SQL NULL survive PostgreSQL');
    equal((row[2] as ArrayValue).values, bytea, 'Binary bytea elements survive array text transport');
    equal(row[3], {values: [], dimensions: [], lowerBounds: []}, 'Empty array has no dimensions');
    equal(row.slice(4), [-9223372036854775808n, '0.12345678901234567890'], 'Prepared parameters preserve exact values');
  } finally {await statement.close();}
  checks.push('codecs-array-bounds', 'codecs-array-escaping-null', 'codecs-array-bytea', 'codecs-prepared-parameters');

  await c.query("CREATE TYPE sdk_mood AS ENUM ('ok', 'busy')");
  const oid = Number(scalar(await c.query("SELECT 'sdk_mood'::regtype::oid")));
  const custom = new Codecs().register(oid, {decode: value => ({mood: value}),
    encode: value => (value as {mood: string}).mood});
  const mood = (await c.query('SELECT $1, NULL::sdk_mood', {parameters: [custom.parameter(oid, {mood: 'busy'})]}))[0];
  equal(custom.decode(mood).rows[0], [{mood: 'busy'}, null], 'Custom codec uses the database-assigned type OID');
  equal(codecs.decode(mood).rows[0], ['busy', null], 'Custom registration does not affect another registry');
  checks.push('codecs-custom-type-isolation');
  await c.close();
}
