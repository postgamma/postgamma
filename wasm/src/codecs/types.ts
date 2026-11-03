/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import type {Parameter} from '../api/database.js';
import type {Column, Result} from './results.js';

export type RawValue = string | Uint8Array;
export interface TypeCodec {
  decode(value: RawValue, column: Column): unknown;
  encode?(value: unknown): RawValue;
}
export interface DecodedResult extends Omit<Result, 'rows'> {rows: unknown[][]}
export interface CodecOptions {
  /** Text preserves every JSON number. JavaScript uses JSON.parse semantics. */
  json?: 'text' | 'javascript';
}
export interface ArrayValue {values: unknown[]; lowerBounds: number[]; dimensions: number[]}
const arrayTypes = new Map([[1000, 16], [1001, 17], [1016, 20], [1005, 21], [1007, 23],
  [1009, 25], [1028, 26], [1021, 700], [1022, 701], [1014, 1042], [1015, 1043],
  [199, 114], [3807, 3802], [1182, 1082], [1183, 1083], [1115, 1114], [1185, 1184],
  [1231, 1700], [2951, 2950]]);
const decoder = new TextDecoder('utf-8', {fatal: true});
const textOids = new Set([18, 19, 25, 1042, 1043, 2950]);
const temporalOids = new Set([1082, 1083, 1114, 1184, 1186, 1266]);

function integer(value: unknown, min: bigint, max: bigint): bigint {
  if (typeof value === 'number' && !Number.isSafeInteger(value))
    throw new TypeError('Integer numbers must be safe integers; use bigint or text');
  if (!['number', 'bigint', 'string'].includes(typeof value) || !/^[+-]?\d+$/.test(String(value)))
    throw new TypeError('Expected an integer');
  const n = BigInt(String(value));
  if (n < min || n > max) throw new RangeError('Integer is outside the PostgreSQL type range');
  return n;
}
function bytesText(value: string): Uint8Array {
  if (value.startsWith('\\x')) {
    if (!/^(?:[0-9a-fA-F]{2})*$/.test(value.slice(2))) throw new TypeError('Invalid bytea hex');
    return Uint8Array.from(value.slice(2).match(/../g) ?? [], byte => parseInt(byte, 16));
  }
  const bytes: number[] = [];
  for (let i = 0; i < value.length; i++) {
    if (value[i] !== '\\') {if (value.charCodeAt(i) > 127) throw new TypeError('Invalid bytea escape'); bytes.push(value.charCodeAt(i));}
    else if (value[i + 1] === '\\') {bytes.push(92); i++;}
    else {
      const octal = value.slice(i + 1, i + 4);
      if (!/^[0-3][0-7]{2}$/.test(octal)) throw new TypeError('Invalid bytea escape');
      bytes.push(parseInt(octal, 8)); i += 3;
    }
  }
  return new Uint8Array(bytes);
}
function shape(values: unknown[], depth = 0): number[] {
  if (depth >= 6) throw new TypeError('PostgreSQL arrays have at most six dimensions');
  if (!values.length) return [0];
  const nested = Array.isArray(values[0]);
  if (values.some(value => Array.isArray(value) !== nested)) throw new TypeError('Array dimensions must be rectangular');
  if (!nested) return [values.length];
  const child = shape(values[0] as unknown[], depth + 1);
  for (const value of values) if (String(shape(value as unknown[], depth + 1)) !== String(child))
    throw new TypeError('Array dimensions must be rectangular');
  return [values.length, ...child];
}
function parseArray(source: string, element: (value: string) => unknown): ArrayValue {
  let offset = 0;
  const lowerBounds: number[] = [], declared: number[] = [];
  while (source[offset] === '[') {
    const match = /^\[(-?\d+):(-?\d+)\]/.exec(source.slice(offset));
    if (!match) throw new TypeError('Invalid array bounds');
    const low = Number(integer(match[1], -2147483648n, 2147483647n));
    const high = Number(integer(match[2], -2147483648n, 2147483647n));
    lowerBounds.push(low); declared.push(high - low + 1); offset += match[0].length;
  }
  if (declared.length && source[offset++] !== '=') throw new TypeError('Invalid array bounds separator');
  function parse(depth: number): unknown[] {
    if (depth > 6 || source[offset++] !== '{') throw new TypeError('Invalid array nesting');
    const values: unknown[] = [];
    if (source[offset] === '}') {offset++; return values;}
    for (;;) {
      if (source[offset] === '{') values.push(parse(depth + 1));
      else {
        let value = '', quoted = false, escaped = false;
        if (source[offset] === '"') {
          quoted = true; offset++;
          for (;;) {
            if (offset >= source.length) throw new TypeError('Unterminated array value');
            const char = source[offset++];
            if (char === '"') break;
            if (char === '\\') {if (offset >= source.length) throw new TypeError('Invalid array escape'); value += source[offset++];}
            else value += char;
          }
        } else {
          while (offset < source.length && source[offset] !== ',' && source[offset] !== '}') {
            const char = source[offset++];
            if (char === '\\') {escaped = true; if (offset >= source.length) throw new TypeError('Invalid array escape'); value += source[offset++];}
            else value += char;
          }
          if (!value) throw new TypeError('Empty unquoted array value');
        }
        values.push(!quoted && !escaped && value === 'NULL' ? null : element(value));
      }
      const separator = source[offset++];
      if (separator === '}') break;
      if (separator !== ',') throw new TypeError('Invalid array separator');
    }
    return values;
  }
  const values = parse(1), dimensions = values.length ? shape(values) : [];
  if (offset !== source.length || (declared.length && String(declared) !== String(dimensions)))
    throw new TypeError('Array bounds do not match its values');
  return {values, dimensions, lowerBounds: declared.length ? lowerBounds : dimensions.map(() => 1)};
}

/** An application-owned codec registry. Raw query results remain unchanged. */
export class Codecs {
  private custom = new Map<number, TypeCodec>();
  private json: 'text' | 'javascript';
  constructor(options: CodecOptions = {}) {
    this.json = options.json ?? 'text';
    if (!['text', 'javascript'].includes(this.json)) throw new TypeError('Invalid JSON codec mode');
  }
  register(typeOid: number, codec: TypeCodec): this {
    if (!Number.isInteger(typeOid) || typeOid <= 0 || typeOid > 0xffffffff || typeof codec.decode !== 'function')
      throw new TypeError('Expected a positive type OID and a decoder');
    this.custom.set(typeOid, codec); return this;
  }
  decode(result: Result): DecodedResult {
    return {...result, rows: result.rows.map(row => row.map((value, index) => this.value(value, result.columns[index])))};
  }
  value(value: RawValue | null, column: Column): unknown {
    if (value === null) return null;
    const custom = this.custom.get(column.typeOid);
    if (custom) return custom.decode(value, column);
    const oid = column.typeOid;
    if (typeof value === 'string') {
      const element = arrayTypes.get(oid);
      if (element) return parseArray(value, item => this.value(item, {...column, typeOid: element, format: 0}));
      if (oid === 16) {if (value !== 't' && value !== 'f') throw new TypeError('Invalid boolean'); return value === 't';}
      if (oid === 17) return bytesText(value);
      if (oid === 20) return integer(value, -9223372036854775808n, 9223372036854775807n);
      if (oid === 21 || oid === 23 || oid === 26) return Number(integer(value,
        oid === 21 ? -32768n : oid === 23 ? -2147483648n : 0n,
        oid === 21 ? 32767n : oid === 23 ? 2147483647n : 4294967295n));
      if (oid === 700 || oid === 701) {
        if (value === 'NaN') return NaN;
        if (value === 'Infinity' || value === '-Infinity') return Number(value);
        if (!/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/.test(value)) throw new TypeError('Invalid floating point value');
        return Number(value);
      }
      if ((oid === 114 || oid === 3802) && this.json === 'javascript') return JSON.parse(value);
      // Numeric and temporal text retain precision, offsets, infinity and BC.
      return value;
    }
    const view = new DataView(value.buffer, value.byteOffset, value.byteLength);
    const lengths: Record<number, number> = {16: 1, 20: 8, 21: 2, 23: 4, 26: 4, 700: 4, 701: 8};
    if (oid in lengths && value.byteLength !== lengths[oid]) throw new TypeError('Invalid binary value width');
    if (oid === 16) {if (value[0] > 1) throw new TypeError('Invalid binary boolean'); return value[0] === 1;}
    if (oid === 20) return view.getBigInt64(0, false);
    if (oid === 21) return view.getInt16(0, false);
    if (oid === 23) return view.getInt32(0, false);
    if (oid === 26) return view.getUint32(0, false);
    if (oid === 700) return view.getFloat32(0, false);
    if (oid === 701) return view.getFloat64(0, false);
    if (oid === 114 || oid === 3802) {
      if (oid === 3802 && value[0] !== 1) throw new TypeError('Unsupported binary JSONB version');
      const json = decoder.decode(oid === 3802 ? value.subarray(1) : value);
      return this.json === 'javascript' ? JSON.parse(json) : json;
    }
    if (textOids.has(oid) && oid !== 2950) return decoder.decode(value);
    return value; // Unsupported binary formats and custom types remain lossless.
  }
  parameter(typeOid: number, value: unknown): Parameter {
    if (!Number.isInteger(typeOid) || typeOid <= 0 || typeOid > 0xffffffff) throw new TypeError('Expected a positive type OID');
    if (value === null) return {typeOid, value: null};
    const custom = this.custom.get(typeOid);
    if (custom?.encode) return this.encoded(typeOid, custom.encode(value));
    const element = arrayTypes.get(typeOid);
    if (element) {
      const array = Array.isArray(value) ? {values: value, lowerBounds: undefined} : value as Partial<ArrayValue>;
      if (!array || !Array.isArray(array.values)) throw new TypeError('Expected an array or ArrayValue');
      const dimensions = array.values.length ? shape(array.values) : [];
      if (dimensions.some(size => size === 0)) throw new TypeError('Nested empty arrays have no PostgreSQL dimensions');
      const bounds = array.lowerBounds ?? dimensions.map(() => 1);
      if (bounds.length !== dimensions.length || bounds.some(bound => !Number.isSafeInteger(bound)))
        throw new TypeError('Array lower bounds must match its dimensions');
      const encode = (items: unknown[]): string => '{' + items.map(item => {
        if (Array.isArray(item)) return encode(item);
        if (item === null) return 'NULL';
        const p = this.parameter(element, item);
        if (p.value instanceof Uint8Array && element !== 17)
          throw new TypeError('Array element encoders must produce text');
        const text = p.value instanceof Uint8Array ? '\\x' + [...p.value].map(byte => byte.toString(16).padStart(2, '0')).join('') : p.value!;
        return '"' + text.replace(/["\\]/g, '\\$&') + '"';
      }).join(',') + '}';
      const prefix = bounds.some(bound => bound !== 1) ? bounds.map((low, i) => {
        integer(low, -2147483648n, 2147483647n);
        const high = Number(integer(low + dimensions[i] - 1, -2147483648n, 2147483647n));
        return `[${low}:${high}]`;
      }).join('') + '=' : '';
      return {typeOid, value: prefix + encode(array.values)};
    }
    if (typeOid === 17) {if (!(value instanceof Uint8Array)) throw new TypeError('bytea expects Uint8Array'); return this.encoded(typeOid, value);}
    if (typeOid === 16) {if (typeof value !== 'boolean') throw new TypeError('bool expects boolean'); return {typeOid, value: value ? 't' : 'f'};}
    if (typeOid === 20 || typeOid === 21 || typeOid === 23 || typeOid === 26) return {typeOid, value: String(integer(value,
      typeOid === 20 ? -9223372036854775808n : typeOid === 21 ? -32768n : typeOid === 23 ? -2147483648n : 0n,
      typeOid === 20 ? 9223372036854775807n : typeOid === 21 ? 32767n : typeOid === 23 ? 2147483647n : 4294967295n))};
    if (typeOid === 114 || typeOid === 3802) {
      const text = JSON.stringify(value); if (text === undefined) throw new TypeError('Value is not JSON serializable');
      return {typeOid, value: text};
    }
    if (typeOid === 700 || typeOid === 701) {if (typeof value !== 'number') throw new TypeError('Float expects number'); return {typeOid, value: String(value)};}
    if (typeOid === 1700) {
      if (typeof value !== 'string' && typeof value !== 'bigint') throw new TypeError('numeric expects exact text or bigint');
      return {typeOid, value: String(value)};
    }
    if (typeOid === 1184 && value instanceof Date) return {typeOid, value: value.toISOString()};
    if (temporalOids.has(typeOid) && typeof value !== 'string') throw new TypeError('Temporal values expect PostgreSQL text');
    if (typeof value !== 'string') throw new TypeError('Expected PostgreSQL text or a registered encoder');
    return {typeOid, value};
  }
  private encoded(typeOid: number, value: RawValue): Parameter {
    if (typeof value !== 'string' && !(value instanceof Uint8Array)) throw new TypeError('Codec must return text or bytes');
    return {typeOid, value, format: value instanceof Uint8Array ? 1 : 0};
  }
}
