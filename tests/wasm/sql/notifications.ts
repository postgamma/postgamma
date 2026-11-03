/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {NotificationOverflowError} from '../src/api/database.js';
import type {Connection, Database, Notification} from '../src/api/database.js';
import {assert, scalar} from './helpers.js';
class Inbox {
  values: Notification[] = [];
  errors: unknown[] = [];
  private waiters = new Set<() => void>();
  readonly stop: () => void;
  constructor(c: Connection) {
    this.stop = c.onNotification(value => {this.values.push(value); this.changed();}, error => {this.errors.push(error); this.changed();});
  }
  private changed() {for (const wake of [...this.waiters]) wake();}
  wait(predicate: () => boolean): Promise<void> {
    if (predicate()) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const done = () => {if (predicate()) {clearTimeout(timer); this.waiters.delete(done); resolve();}};
      const timer = setTimeout(() => {this.waiters.delete(done); reject(new Error('Notification delivery timed out'));}, 5000);
      this.waiters.add(done); done();
    });
  }
}
// Same SQL/channel/payload fixtures as PGlite notify.test.ts. PostGamma uses
// an explicit connection subscription plus SQL LISTEN/UNLISTEN.
export async function notificationCase(db: Database, c: Connection, mode: string) {
  const sender = await db.connect('notification-sender'), inbox = new Inbox(c);
  try {
    await c.exec('LISTEN test; LISTEN barrier');
    if (mode === 'basic') {
      await sender.exec("NOTIFY test, '321'");
      await inbox.wait(() => inbox.values.length === 1);
      assert(inbox.values[0].channel === 'test' && inbox.values[0].payload === '321' && inbox.values[0].pid > 0, 'Idle listener receives payload and sender PID');
      inbox.values.length = 0;
      await c.exec("NOTIFY test, '123'");
      await inbox.wait(() => inbox.values.length === 1);
      assert(inbox.values[0].payload === '123', 'Executing connection also receives its own notification');
    } else if (mode === 'unlisten') {
      await c.exec('UNLISTEN test');
      await sender.exec("NOTIFY test, 'ignored'; NOTIFY barrier, 'done'");
      await inbox.wait(() => inbox.values.some(n => n.channel === 'barrier'));
      assert(inbox.values.length === 1, 'UNLISTEN removes server subscription');
      inbox.stop(); await sender.exec("NOTIFY barrier, 'unsubscribed'");
      await c.query('SELECT 1'); await db.telemetry();
      assert(inbox.values.length === 1, 'Removed callback receives no further events');
    } else if (mode === 'quoted') {
      await c.exec('LISTEN PostgresDefaultLower; LISTEN "tesT2"; LISTEN "Quoted Channel With Spaces"; LISTEN "test&me"');
      await sender.exec("NOTIFY postgresdefaultlower, 'payload1'; NOTIFY \"tesT2\", 'paYloAd2'; NOTIFY test2, 'ignored'; NOTIFY \"Quoted Channel With Spaces\", 'payload1'; NOTIFY \"test&me\", 'paYloAd2'; NOTIFY barrier, 'done'");
      await inbox.wait(() => inbox.values.some(n => n.channel === 'barrier'));
      assert(inbox.values.map(n => n.channel).join('|') === 'postgresdefaultlower|tesT2|Quoted Channel With Spaces|test&me|barrier', 'PostgreSQL channel case and quoting');
    } else if (mode === 'transaction') {
      const status = await c.status();
      assert(status.pinReasons === 0 && status.carrierRetained === 0, 'An idle listener releases its carrier');
      // Separate commits can signal while the listener is yielding after the
      // first delivery. No listener SQL or telemetry may repair a lost wake.
      for (let round = 0; round < 32; round++) {
        await sender.exec("BEGIN; NOTIFY test, 'rollback'; ROLLBACK; BEGIN; NOTIFY test, 'commit'; COMMIT; NOTIFY barrier, 'done'");
        await inbox.wait(() => inbox.values.some(n => n.channel === 'barrier'));
        assert(inbox.values.map(n => n.payload).join(',') === 'commit,done', `Only committed notifications are delivered (round ${round})`);
        inbox.values.length = 0;
      }
      await c.exec('BEGIN'); await sender.exec("NOTIFY test, 'deferred'");
      await c.query('SELECT 1'); await db.telemetry();
      assert(inbox.values.length === 0, 'Listening transaction defers notification');
      await c.exec('COMMIT'); await inbox.wait(() => inbox.values.length === 1);
      assert(inbox.values[0].payload === 'deferred', 'Listener commit releases deferred notification');
    } else if (mode === 'callbacks') {
      let errors = 0;
      const remove = c.onNotification(() => {throw new Error('callback fixture');}, () => {errors++;});
      const removeAsync = c.onNotification(async () => {throw new Error('async callback fixture');}, () => {errors++;});
      await sender.exec("NOTIFY test, '123'"); await inbox.wait(() => inbox.values.length === 1);
      remove(); removeAsync();
      const result = await c.query('SELECT 14');
      assert(errors === 2 && scalar(result) === '14', 'Throwing and rejecting callbacks leave delivery and SQL intact');
    } else throw new Error('Unknown notification case');
    assert(inbox.errors.length === 0, 'Ordinary notification delivery has no overflow');
  } finally {inbox.stop(); await c.query('UNLISTEN *'); await sender.close();}
}
export async function notifications(db: Database, checks: string[]) {
  const c = await db.connect('notification-listener');
  try {
    for (const mode of ['basic', 'unlisten', 'quoted', 'transaction', 'callbacks']) await notificationCase(db, c, mode);
    const sender = await db.connect(), inbox = new Inbox(c);
    try {
      await c.query('LISTEN bounded');
      // Hold only JS event consumption. Real SQL, native input and the bounded
      // router keep progressing, so this exercises observable overflow.
      const transport = (db as unknown as {transport: {eventsRunning: boolean; events(): Promise<void>}}).transport;
      transport.eventsRunning = true;
      try {
        await sender.query("DO $$ BEGIN FOR i IN 1..1000 LOOP PERFORM pg_notify('bounded', i::text); END LOOP; END $$");
        await c.query('SELECT 1');
      } finally {transport.eventsRunning = false; await transport.events();}
      await inbox.wait(() => inbox.errors.some(e => e instanceof NotificationOverflowError));
      const dropped = inbox.errors.reduce<bigint>((n, e) => n + (e instanceof NotificationOverflowError ? e.dropped : 0n), 0n);
      assert(BigInt(inbox.values.length) + dropped === 1000n, 'Delivered plus reported drops accounts for every notification');
      inbox.errors.length = 0;
      await sender.exec("NOTIFY bounded, 'after-overflow'");
      await inbox.wait(() => inbox.values.some(n => n.payload === 'after-overflow'));
      assert(scalar(await c.query('SELECT 15')) === '15', 'Notification queue recovers after overflow');
      await db.telemetry(); const before = db.sample()!.driverWakeCount;
      await new Promise(resolve => setTimeout(resolve, 500)); await db.telemetry();
      const wakes = (db.sample()!.driverWakeCount - before) >>> 0;
      assert(wakes < 50, 'Idle listener does not busy-poll');
      (window as unknown as {probeEvents: unknown[]}).probeEvents.push({kind: 'notification-queue', dropped: String(dropped), idleWakes: wakes});
    } finally {inbox.stop(); await sender.close();}
    checks.push('idle-listen-notify', 'notify-transaction-commit-rollback', 'notify-channel-quoting',
      'notify-unsubscribe', 'notify-callback-isolation', 'notify-bounded-overflow', 'notify-idle-sleep');
  } finally {await c.close();}
  const fresh = await db.connect(), sender = await db.connect();
  const inbox = new Inbox(fresh);
  try {
    await fresh.query('LISTEN barrier');
    await sender.exec("NOTIFY bounded, 'old-generation'; NOTIFY barrier, 'new-generation'");
    await inbox.wait(() => inbox.values.length > 0);
    assert(inbox.values.length === 1 && inbox.values[0].payload === 'new-generation', 'Closing listener retires subscription and handle generation');
    checks.push('notify-closed-generation');
  } finally {inbox.stop(); await fresh.close(); await sender.close();}
}
