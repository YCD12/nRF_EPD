'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const test = require('node:test');

function load(extra = {}) {
  const context = {TextEncoder, TextDecoder, Uint8Array, DataView, ArrayBuffer, setTimeout, clearTimeout, ...extra};
  context.window = context;
  vm.runInNewContext(fs.readFileSync('html/js/calendar_annotations.js', 'utf8'), context);
  return context.CalendarAnnotations;
}

function ack(status, crc) {
  const packet = new Uint8Array(12);
  packet[0] = 0xA8;
  packet[1] = 3;
  packet[2] = status;
  new DataView(packet.buffer).setUint32(8, crc, true);
  return packet;
}

test('durable annotation save accepts only matching business CRC', async () => {
  const annotations = load();
  const bytes = annotations.pack();
  const expected = annotations.crc32(bytes);
  const completion = annotations.waitForCommit(bytes);
  assert.equal(annotations.handleNotification(ack(0, expected ^ 1)), true);
  assert.ok(annotations._commitWaiter);
  assert.equal(annotations.handleNotification(ack(0, expected)), true);
  assert.equal(await completion, true);
});

test('device failure does not become a successful save confirmation', async () => {
  const annotations = load();
  const bytes = annotations.pack();
  const completion = annotations.waitForCommit(bytes);
  annotations.handleNotification(ack(5, annotations.crc32(bytes)));
  assert.equal(await completion, false);
});

test('late cancelled annotation request cannot settle or cancel a replacement read', async () => {
  let completeOldWrite, writes = 0;
  const annotations = load({ addLog() {}, EpdCmd: {SET_ANNOTATIONS: 0x24},
    write: () => ++writes === 1 ? new Promise(resolve => { completeOldWrite = resolve; }) : Promise.resolve(true) });
  annotations.persist = annotations.render = () => {};
  const old = annotations._loadFromDeviceOnce(null);
  annotations.cancelLoad();
  const current = annotations._loadFromDeviceOnce(null), currentTimer = annotations._loadTimer;
  completeOldWrite(false);
  assert.equal(await old, false);
  assert.equal(annotations._loadTimer, currentTimer);
  assert.equal(typeof annotations._resolveLoad, 'function');
  const payload = annotations.pack();
  annotations.handleNotification(Uint8Array.of(0xa2, 0, 0, payload.length, 0, ...payload));
  assert.equal(await current, true);
  assert.equal(annotations._resolveLoad, null);
  assert.equal(annotations._loadTimer, null);
  annotations.annotations = [{ date: '2026-10-01', label: 'local' }];
  annotations.handleNotification(Uint8Array.of(0xa2, 0, 0, payload.length, 0, ...payload));
  assert.equal(annotations.annotations[0].label, 'local');
});
