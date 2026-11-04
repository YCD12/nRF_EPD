'use strict';
/**
 * REVIEW TEST (fails at HEAD, on purpose) — Web console bug hunt 2026-11-04.
 *
 * html/js/font_sync.js `ensure()` marks itself as "entered" *before* it calls
 * `client.upload()`. The first thing upload() does is send RESOURCE ENTER (0x26 op 6).
 * The firmware answers ENTER with RS_BUSY (2) whenever the panel is between frames
 * (epd_resource_idle() is false), e.g. during a clock refresh. The error path then
 * runs the destructive rollback path even though no Flash write has happened:
 * it sends ABORT + re-uploads the *original* font package, and if the refresh is
 * still running it reports "旧字库恢复未完成 ... 请重新连接并修复设备资源" although
 * nothing was ever modified.
 *
 * todo_sync.js already retries ENTER/RS_BUSY (acquire(), 90 s budget); font_sync.js
 * did not. FIXED (W-01): font_sync.js now acquires the maintenance session with
 * the same 250 ms / ~90 s RS_BUSY retry before marking itself entered, and a
 * pre-write RS_BUSY releases the abandoned session without re-uploading the old
 * font package. These tests verify the fixed behavior and that a real post-write
 * failure still performs the full original-package rollback.
 *
 * Evidence/logs: output/work-20261104/web-bugs/probes/probe_font_sync_busy.cjs
 * Report: output/work-20261104/web-bugs/report.md (W-01).
 *
 * Runs without a browser: a small simulator implements EPD/resource_protocol.c
 * semantics (STATUS/ENTER/BEGIN/DATA/COMMIT/ABORT/VERIFY/LEAVE) over a 512 KiB
 * flash image and delivers A7 replies asynchronously, like BLE notifications.
 */
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..');
const store = require(path.join(ROOT, 'html/js/resource_store.js'));
const builder = require(path.join(ROOT, 'html/js/resource_builder.js'));
const sync = require(path.join(ROOT, 'html/js/font_sync.js'));

const CAPACITY = 524288;
const LAYOUT = [
  [1, 1, 0x00000, 4096], [2, 2, 0x01000, 4096], [3, 3, 0x02000, 4096],
  [4, 4, 0x03000, 4096], [5, 5, 0x04000, 16384], [6, 6, 0x08000, 65536],
  [7, 7, 0x18000, 262144], [9, 9, 0x58000, 4096], [8, 8, 0x59000, 159744]
];
const RS = { OK: 0, INVALID: 1, BUSY: 2, CRC: 3, NOT_FOUND: 4, IO: 5, INCOMPATIBLE: 6, WRITE_PROTECTED: 7 };
const BASE_FONTS = ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn']
  .map(name => new Uint8Array(fs.readFileSync(path.join(ROOT, 'tools/dist/u8g2_font_' + name + '.bin'))));

const Queue = { tasks: [], scheduled: false, deliver(task) { this.tasks.push(task); this.schedule(); },
  schedule() { if (this.scheduled) return; this.scheduled = true; setImmediate(() => { this.scheduled = false; const tasks = this.tasks; this.tasks = []; for (const task of tasks) task(); }); } };

function partitionTablePayload() {
  const payload = new Uint8Array(LAYOUT.length * 16), view = new DataView(payload.buffer);
  LAYOUT.forEach(([id, type, offset, size], index) => {
    const at = index * 16;
    view.setUint16(at, id, true); view.setUint16(at + 2, type, true);
    view.setUint32(at + 4, offset, true); view.setUint32(at + 8, size, true);
    view.setUint16(at + 12, 1, true); view.setUint16(at + 14, 0, true);
  });
  return payload;
}

function createFirmware(options = {}) {
  const flash = new Uint8Array(CAPACITY).fill(0xff);
  flash.set(builder.wrap(1, partitionTablePayload(), 1), 0);
  flash.set(builder.wrap(7, builder.encodeFonts(BASE_FONTS), 3), 0x18000);
  let entered = false, active = null, client = null;
  const ops = [];
  let busyReplies = options.enterBusy ?? 0;
  let failedCommits = options.failCommits ?? 0;
  const partitionOf = id => LAYOUT.find(part => part[0] === id);
  function headerAt(id) {
    const part = partitionOf(id);
    if (!part) return null;
    const header = flash.subarray(part[2], part[2] + 32);
    const view = new DataView(header.buffer, header.byteOffset, 32);
    if (view.getUint32(0, true) !== 0x31535245 || view.getUint32(28, true) !== 0xa55ac33c ||
        view.getUint32(4, true) !== (0x10000 | id) || view.getUint32(20, true) !== CAPACITY ||
        store.crc32(header.subarray(0, 24)) !== view.getUint32(24, true)) return null;
    const length = view.getUint32(12, true);
    if (!length || length > part[3] - 32) return null;
    return { revision: view.getUint32(8, true), length, crc: view.getUint32(16, true) };
  }
  function reply(op, token, result, body = []) {
    const bytes = new Uint8Array(4 + body.length);
    bytes.set([0xa7, op, token, result]); bytes.set(body, 4);
    return bytes;
  }
  function execute(packet) {
    const op = packet[1], token = packet[2], body = packet.subarray(3);
    const view = new DataView(body.buffer, body.byteOffset, body.byteLength);
    switch (op) {
      case 0: {
        const out = reply(op, token, RS.OK, new Uint8Array(12));
        out[4] = 0x7f; out[5] = 1; out[6] = active ? active.id : 0; out[7] = entered ? 1 : 0;
        new DataView(out.buffer).setUint32(12, CAPACITY, true);
        return out;
      }
      case 1: {
        const address = view.getUint32(0, true), length = body[4];
        if (!length || address + length > CAPACITY) return reply(op, token, RS.INVALID);
        return reply(op, token, RS.OK, flash.slice(address, address + length));
      }
      case 2: {
        if (!entered) return reply(op, token, RS.WRITE_PROTECTED);
        const part = partitionOf(body[0]);
        if (!part || active) return reply(op, token, RS.INVALID);
        const length = view.getUint32(1, true);
        if (!length || length > part[3] - 32) return reply(op, token, RS.INVALID);
        active = { id: body[0], length, crc: view.getUint32(5, true), revision: view.getUint32(9, true),
          received: 0, data: new Uint8Array(length) };
        flash.fill(0xff, part[2], part[2] + 32);
        ops.push({ op, id: body[0] });
        return reply(op, token, RS.OK);
      }
      case 3: case 10: {
        if (!entered || !active) return reply(op, token, RS.WRITE_PROTECTED);
        const offset = view.getUint32(0, true), data = body.subarray(4);
        if (offset + data.length > active.length) return reply(op, token, RS.INVALID);
        active.data.set(data, offset);
        active.received = Math.max(active.received, offset + data.length);
        if (op === 10) {
          const out = reply(op, token, RS.OK, new Uint8Array(8));
          new DataView(out.buffer).setUint32(4, active.received, true);
          new DataView(out.buffer).setUint32(8, active.received, true);
          return out;
        }
        return reply(op, token, RS.OK);
      }
      case 4: {
        if (!entered || !active) return reply(op, token, RS.WRITE_PROTECTED);
        if (failedCommits > 0) {
          // Firmware keeps the failed transaction active until ABORT/LEAVE.
          failedCommits--;
          ops.push({ op, failed: true });
          return reply(op, token, RS.IO);
        }
        if (active.received !== active.length || store.crc32(active.data) !== active.crc) return reply(op, token, RS.CRC);
        flash.set(builder.wrap(active.id, active.data, active.revision), partitionOf(active.id)[2]);
        active = null;
        return reply(op, token, RS.OK);
      }
      case 5: active = null; ops.push({ op }); return reply(op, token, RS.OK);
      case 6:
        if (busyReplies > 0) { busyReplies--; ops.push({ op, busy: true }); return reply(op, token, RS.BUSY); }
        entered = true; ops.push({ op });
        return reply(op, token, RS.OK);
      case 7: entered = false; active = null; ops.push({ op }); return reply(op, token, RS.OK);
      case 9: {
        const info = headerAt(body[0]);
        if (!info) return reply(op, token, RS.NOT_FOUND);
        const out = reply(op, token, RS.OK, new Uint8Array(12)), outView = new DataView(out.buffer);
        outView.setUint32(4, info.revision, true); outView.setUint32(8, info.length, true); outView.setUint32(12, info.crc, true);
        return out;
      }
      default: return reply(op, token, RS.INVALID);
    }
  }
  return {
    ops, flash,
    header: headerAt,
    attach(next) { client = next; return client; },
    write(packet) {
      const out = execute(packet);
      Queue.deliver(() => client && client.receive(out));
      return Promise.resolve(true);
    }
  };
}

const rasterize = async () => ({ width: 3, height: 3, x: 0, y: -1, advance: 4,
  pixels: Uint8Array.of(1, 0, 1, 0, 1, 0, 1, 0, 1) });
const requests = [{ slot: 0, text: '生日' }, { slot: 1, text: '生日' }];

test('font sync: a transient ENTER RS_BUSY is retried instead of triggering font recovery', async () => {
  const firmware = createFirmware({ enterBusy: 1 });
  const client = firmware.attach(new store.ResourceClient(bytes => firmware.write(bytes), 5000, () => 244));
  const result = await sync.ensure(client, requests, { rasterize });
  assert.equal(result.changed, true, 'font package should have been updated after the busy retry');
  const begins = firmware.ops.filter(op => op.op === 2);
  assert.equal(begins.length, 1, 'exactly one font transaction, not a busy-triggered rollback re-upload');
  assert.equal(begins[0].id, 7);
  assert.equal(firmware.header(7).revision, 4, 'the new font revision is committed');
});

test('font sync: repeated transient RS_BUSY answers are waited out within the 90 s budget', async () => {
  // Previously the second BUSY happened inside the unnecessary rollback and was
  // reported as "旧字库恢复未完成 ... 请修复设备资源" although nothing was written.
  const firmware = createFirmware({ enterBusy: 2 });
  const client = firmware.attach(new store.ResourceClient(bytes => firmware.write(bytes), 5000, () => 244));
  const result = await sync.ensure(client, requests, { rasterize });
  assert.equal(result.changed, true, 'the font update is retried after both busy replies');
  const begins = firmware.ops.filter(op => op.op === 2);
  assert.equal(begins.length, 1, 'no rollback re-upload of the untouched package');
  assert.equal(firmware.ops.filter(op => op.busy).length, 2, 'both RS_BUSY answers were observed first');
  assert.equal(firmware.header(7).revision, 4);
});

test('font sync: a real post-BEGIN failure still restores the original font package', async () => {
  const firmware = createFirmware({ failCommits: 1 });
  const original = firmware.header(7);
  const client = firmware.attach(new store.ResourceClient(bytes => firmware.write(bytes), 5000, () => 244));
  await assert.rejects(sync.ensure(client, requests, { rasterize }), error => {
    assert.notEqual(error.recoveryIncomplete, true, 'a successful rollback is not an incomplete recovery');
    return true;
  });
  const begins = firmware.ops.filter(op => op.op === 2);
  assert.equal(begins.length, 2, 'the failed update and the original-package rollback each begin once');
  assert.ok(firmware.ops.some(op => op.op === 5), 'the failed transaction is aborted before restoring');
  assert.ok(firmware.ops.some(op => op.op === 7), 'the maintenance session is released');
  assert.deepEqual(firmware.header(7), original, 'the original package is byte-restored and committed');
});
