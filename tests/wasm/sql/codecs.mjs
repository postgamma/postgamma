/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const {Codecs} = await import(pathToFileURL(process.argv[2]));
const codecs = new Codecs();
const column = (typeOid, format = 0) => ({name: 'value', typeOid, format, tableOid: 0,
  tableColumn: 0, typeSize: -1, typeModifier: -1});
const decode = (oid, value) => codecs.value(value, column(oid, value instanceof Uint8Array ? 1 : 0));
for (const [oid, text, expected] of [[16, 't', true], [16, 'f', false], [21, '-32768', -32768],
  [23, '2147483647', 2147483647], [26, '4294967295', 4294967295],
  [20, '-9223372036854775808', -9223372036854775808n], [20, '9223372036854775807', 9223372036854775807n],
  [700, '1.25', 1.25], [701, '-Infinity', -Infinity], [701, 'NaN', NaN],
  [1700, '12345678901234567890.123456789', '12345678901234567890.123456789'],
  [1114, '2000-01-01 00:00:00.123456', '2000-01-01 00:00:00.123456'], [1082, 'infinity', 'infinity']])
  assert.deepEqual(decode(oid, text), expected);
assert.equal(decode(20, null), null);
assert.throws(() => decode(16, 'true'), /boolean/);
assert.throws(() => decode(23, '2147483648'), /range/);
assert.throws(() => codecs.parameter(20, 9007199254740992), /safe integers/);
assert.throws(() => codecs.parameter(1700, 0.1), /exact/);
assert.throws(() => codecs.parameter(16, 'false'), /boolean/);
assert.deepEqual(codecs.parameter(20, 9223372036854775807n), {typeOid: 20, value: '9223372036854775807'});
assert.equal(codecs.parameter(1184, new Date('2020-01-02T03:04:05.678Z')).value, '2020-01-02T03:04:05.678Z');
assert.throws(() => codecs.parameter(1114, new Date()), /Temporal/);

const buffer = new Uint8Array(12);
new DataView(buffer.buffer).setBigInt64(2, -9007199254740993n, false);
assert.equal(decode(20, buffer.subarray(2, 10)), -9007199254740993n);
assert.throws(() => decode(20, buffer), /width/);
assert.throws(() => decode(16, new Uint8Array([2])), /boolean/);
assert.deepEqual(decode(17, '\\x00017f80ff'), new Uint8Array([0, 1, 127, 128, 255]));
assert.deepEqual(decode(17, 'a\\000\\377\\\\'), new Uint8Array([97, 0, 255, 92]));
assert.throws(() => decode(17, '\\x0'), /hex/);
assert.throws(() => decode(17, '\\400'), /escape/);
assert.deepEqual(codecs.parameter(17, buffer.subarray(2, 10)).value, buffer.subarray(2, 10));

const exactJson = '{"n":9007199254740993,"decimal":0.12345678901234567890}';
assert.equal(decode(3802, exactJson), exactJson, 'Default JSON preserves every numeric digit');
const javascript = new Codecs({json: 'javascript'});
assert.deepEqual(javascript.value('{"items":[1,null,"NULL"]}', column(114)), {items: [1, null, 'NULL']});
assert.equal(codecs.parameter(3802, {ok: true}).value, '{"ok":true}');
const jsonBytes = new Uint8Array([1, ...new TextEncoder().encode(exactJson)]);
assert.equal(decode(3802, jsonBytes), exactJson);
assert.throws(() => decode(3802, new Uint8Array([2])), /version/);

for (const values of [[null, 'NULL', '', 'comma,', '{braces}', '"quoted"', '\\', ' π '],
  [['a', null], ['b', 'NULL']], []]) {
  const encoded = codecs.parameter(1009, values);
  assert.deepEqual(decode(1009, encoded.value).values, values);
}
const bounded = decode(1016, '[0:1][-2:-1]={{9007199254740993,NULL},{1,-2}}');
assert.deepEqual(bounded, {values: [[9007199254740993n, null], [1n, -2n]], dimensions: [2, 2], lowerBounds: [0, -2]});
assert.deepEqual(decode(1016, codecs.parameter(1016, bounded).value), bounded);
assert.deepEqual(decode(1001, codecs.parameter(1001, [new Uint8Array([0, 92, 255]), null]).value).values,
  [new Uint8Array([0, 92, 255]), null]);
for (const malformed of ['{"unterminated}', '{,}', '[0:2]={1,2}', '{{1},{2,3}}', '{1}garbage'])
  assert.throws(() => decode(1007, malformed));
assert.throws(() => codecs.parameter(1007, [[1], [2, 3]]), /rectangular/);
assert.throws(() => codecs.parameter(1007, [[]]), /empty/);
let tooDeep = [1]; for (let i = 0; i < 6; i++) tooDeep = [tooDeep];
assert.throws(() => codecs.parameter(1007, tooDeep), /six/);

codecs.register(40000, {decode: value => ({custom: value}), encode: value => value.custom});
assert.deepEqual(decode(40000, 'hello'), {custom: 'hello'});
assert.deepEqual(codecs.parameter(40000, {custom: 'hello'}), {typeOid: 40000, value: 'hello', format: 0});
assert.equal(new Codecs().value('hello', column(40000)), 'hello', 'Custom codecs are registry-local');
assert.equal(codecs.value(null, column(40000)), null, 'NULL bypasses custom decoders');
assert.equal(decode(40001, buffer), buffer, 'Unknown binary values remain intact');
const raw = {byteLength: 80, kind: 1, command: 'SELECT 1', columns: [column(20)], rows: [['9007199254740993']]};
assert.equal(codecs.decode(raw).rows[0][0], 9007199254740993n);
assert.equal(raw.rows[0][0], '9007199254740993', 'Decoding does not mutate raw results');
console.log('codecs: exact numerics, binary views, JSON modes, array bounds, NULL and custom isolation passed');
