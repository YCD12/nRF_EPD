'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const test = require('node:test');

function load() {
  const context = {TextEncoder, Uint8Array, DataView, ArrayBuffer, setTimeout, clearTimeout};
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
