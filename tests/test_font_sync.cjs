'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const builder = require('../html/js/resource_builder.js');
const store = require('../html/js/resource_store.js');
const glyphs = require('../html/js/font_builder.js');
const sync = require('../html/js/font_sync.js');
const fontFiles = ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn'];
const baseFonts = () => fontFiles.map(name => new Uint8Array(fs.readFileSync(`tools/dist/u8g2_font_${name}.bin`)));
const rasterize = async () => ({ width: 3, height: 3, x: 0, y: -1, advance: 4,
  pixels: Uint8Array.of(1,0,1,0,1,0,1,0,1) });
const requests = [{ slot: 0, text: '生日' }, { slot: 1, text: '生日' }];

function fixture(options = {}) {
  const partition = { id: 7, type: 7, offset: 0x18000, size: options.size || 262144 };
  let bytes = builder.wrap(7, builder.encodeFonts(baseFonts()), 3), uploaded = null;
  const calls = [];
  const client = {
    closed: false,
    async status() {
      calls.push('status');
      const b = Uint8Array.of(0xa7,0,0,0,options.flags ?? 0x13,1,options.active || 0,options.entered || 0,0,0,0,0,0,0,8,0);
      return b;
    },
    async partitions() { return [partition]; },
    async readPackage() { calls.push('read'); return options.corrupt ? new Uint8Array(35) : bytes; },
    async readRange() {
      if (options.changed) return builder.wrap(7, store.parsePackage(bytes).payload, 4).slice(0, 32);
      return bytes.slice(0, 32);
    },
    async upload(next) { calls.push('upload'); uploaded = next; if (options.uploadFail) throw new Error('upload failed'); bytes = next; },
    async inspect() {
      calls.push('verify');
      const pack = store.parsePackage(bytes);
      return { code: options.verifyFail ? 3 : 0, revision: pack.revision, crc: pack.crc, length: pack.payload.length };
    },
    async leave() { calls.push('leave'); if (options.leaveFail) throw new Error('reload failed'); },
    async request(op) { calls.push(`request:${op}`); }
  };
  return { client, calls, get bytes() { return bytes; }, get uploaded() { return uploaded; } };
}

test('missing device glyphs are merged, CRC committed, verified and loaded before returning', async () => {
  const f = fixture(), progress = [], writes = [];
  const old = baseFonts();
  const result = await sync.ensure(f.client, requests, { rasterize, progress: message => progress.push(message), onWrite: () => writes.push(true) });
  assert.equal(result.changed, true);
  assert.equal(result.added.length, 2); // 日 is already in both device slots.
  assert.deepEqual(f.calls.slice(-3), ['upload', 'verify', 'leave']);
  assert.equal(writes.length, 1);
  const pack = store.parsePackage(f.bytes), fonts = builder.decodeFonts(pack.payload);
  assert.equal(pack.revision, 4);
  assert.equal(glyphs.gatherMissing(fonts, requests).length, 0);
  assert.deepEqual(fonts[2], old[2]);
  assert.ok(progress.some(message => message.includes('正在加载')));
  f.calls.length = 0;
  assert.equal((await sync.ensure(f.client, requests, { rasterize })).changed, false);
  assert.deepEqual(f.calls, ['status', 'read']);
});

test('empty labels do not require font resources or start a device transaction', async () => {
  const f = fixture();
  assert.deepEqual(await sync.ensure(f.client, []), { changed: false, added: [] });
  assert.deepEqual(f.calls, []);
});

test('busy device, legacy hot reload, bad CRC, capacity and changed source fail before writing', async () => {
  for (const options of [{ active: 7 }, { entered: 1 }, { flags: 1 }, { corrupt: true }, { size: 4096 }, { changed: true }]) {
    const f = fixture(options);
    await assert.rejects(sync.ensure(f.client, requests, { rasterize }));
    assert.equal(f.uploaded, null, JSON.stringify(options));
  }
});

test('failed rasterization and changed connection never reach Flash writes', async () => {
  const f = fixture();
  await assert.rejects(sync.ensure(f.client, requests, { rasterize: async () => { throw new Error('no font'); } }), /no font/);
  assert.equal(f.uploaded, null);
  let changed = false;
  await assert.rejects(sync.ensure(f.client, requests, { rasterize: async () => { changed = true; return rasterize(); },
    assertCurrent: () => { if (changed) throw new Error('connection changed'); } }), /connection changed/);
  assert.equal(f.uploaded, null);
});

test('upload, CRC verification and reload failures reject and attempt maintenance cleanup', async () => {
  for (const options of [{ uploadFail: true }, { verifyFail: true }, { leaveFail: true }]) {
    const f = fixture(options);
    await assert.rejects(sync.ensure(f.client, requests, { rasterize }));
    assert.ok(f.calls.includes('request:5'));
    assert.equal(f.calls.at(-1), 'leave');
  }
});

test('regular font resource upgrades retain device-only glyphs and incoming glyphs win', async () => {
  const f = fixture();
  await sync.ensure(f.client, requests, { rasterize });
  const incoming = builder.wrap(7, builder.encodeFonts(baseFonts()), 9);
  const retained = sync.preserve(incoming, f.bytes, [{ id: 7, type: 7, size: 262144 }]);
  assert.equal(retained.added.length, 2);
  const pack = store.parsePackage(retained.bytes);
  assert.equal(pack.revision, 10);
  assert.equal(glyphs.gatherMissing(builder.decodeFonts(pack.payload), requests).length, 0);
  assert.equal(sync.preserve(retained.bytes, f.bytes, [{ id: 7, type: 7, size: 262144 }]).bytes, retained.bytes);
});

test('valid incoming font resources repair CRC-valid malformed old fonts', () => {
  const incoming = builder.wrap(7, builder.encodeFonts(baseFonts()), 9);
  const malformed = builder.wrap(7, Uint8Array.of(1,2,3,4), 8);
  const result = sync.preserve(incoming, malformed, [{ id: 7, type: 7, size: 262144 }]);
  assert.equal(result.repair, true);
  assert.equal(result.bytes, incoming);
  assert.throws(() => sync.preserve(malformed, incoming, [{ id: 7, type: 7, size: 262144 }]));
});

test('deferred font commits expose a guarded rollback that verifies old bytes without applying independently', async () => {
  const f = fixture(), old = f.bytes.slice();
  let current = true;
  const result = await sync.ensure(f.client, requests, { rasterize, deferApply: true,
    assertCurrent: () => { if (!current) throw new Error('connection changed'); } });
  assert.equal(result.deferred, true); assert.equal(typeof result.rollback, 'function');
  assert.equal(f.calls.includes('leave'), false);
  const before = f.calls.length;
  current = false;
  await assert.rejects(result.rollback(), /connection changed/);
  assert.equal(f.calls.length, before);
  current = true;
  await result.rollback();
  assert.deepEqual(f.bytes, old);
  assert.equal(f.calls.filter(call => call === 'upload').length, 2);
  assert.equal(f.calls.includes('leave'), false);
});

test('unrecoverable deferred font write failure preserves the original error and avoids applying partial fonts', async () => {
  const f = fixture({ uploadFail: true });
  await assert.rejects(sync.ensure(f.client, requests, { rasterize, deferApply: true }), error => {
    assert.equal(error.recoveryIncomplete, true);
    assert.match(error.message, /upload failed.*旧字库恢复未完成/);
    return true;
  });
  assert.equal(f.calls.includes('request:5'), true);
  assert.equal(f.calls.includes('leave'), false);
});
