/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Database, NotificationOverflowError, version, type OpenOptions, type Parameter, type Result,
  type ScriptOptions, type Notification, type ArchiveOptions, type RestoreOptions} from '@postgamma/wasm';
const options: OpenOptions = {storage: {type: 'opfs', name: 'typed-consumer'}};
const parameter: Parameter = {typeOid: 20, value: '9223372036854775807'};
async function typedConsumer(): Promise<Result[]> {
  const db = await Database.open(options);
  const c = await db.connect(version);
  try {
    const script: ScriptOptions = {chunkRows: 4};
    const stop = c.onNotification((value: Notification) => {console.log(value.channel, value.payload);}, error => {
      if (error instanceof NotificationOverflowError) console.log(error.dropped);
    });
    await c.exec('SELECT 1; SELECT 2', script); stop();
    const statement = await c.prepare('SELECT $1::int8');
    const parameterTypes: readonly number[] = statement.parameterTypes;
    for await (const _ of await statement.execute({parameters: [parameter], chunkRows: 4})) {}
    await statement.close(); void parameterTypes;
    await c.transaction(async tx => {
      await tx.query('SELECT 1', {timeoutMs: 1000});
      await tx.exec('SAVEPOINT example; RELEASE example');
    }, {isolation: 'serializable', readOnly: true, signal: new AbortController().signal});
    for await (const _ of await c.executeScript('SELECT 3', script)) {}
    return await c.query('SELECT $1::int8', {parameters: [parameter]});
  }
  finally {await c.close(); await db.close();}
}
void typedConsumer;
async function typedArchive(db: Database, source: ReadableStream<Uint8Array>) {
  const archiveOptions: ArchiveOptions = {signal: new AbortController().signal};
  const restoreOptions: RestoreOptions = {...archiveOptions, storage: {type: 'opfs', name: 'restored'}};
  const stream: ReadableStream<Uint8Array> = await db.dumpDataDir(archiveOptions);
  await stream.cancel();
  const restored = await Database.loadDataDir(source, restoreOptions);
  await (await restored.clone()).close();
}
void typedArchive;
// @ts-expect-error A native pointer is not a supported parameter value.
const invalid: Parameter = {typeOid: 20, value: 123};
void invalid;
