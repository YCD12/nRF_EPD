'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const todoSync = require('../html/js/todo_sync.js');
const todo = require('../html/js/todo_calendar.js');
const store = require('../html/js/resource_store.js');
const builder = require('../html/js/resource_builder.js');
const fontSync = require('../html/js/font_sync.js');
const glyphs = require('../html/js/font_builder.js');
const ROOT = path.resolve(__dirname, '..');

const OLD_PARTITIONS = [
  [1, 0x00000, 4096], [2, 0x01000, 4096], [3, 0x02000, 4096],
  [4, 0x03000, 4096], [5, 0x04000, 16384], [6, 0x08000, 65536],
  [7, 0x18000, 262144], [8, 0x58000, 163840],
].map(([id, offset, size]) => ({ id, type: id, offset, size, schema: 1, flags: 0 }));

function tableBytes(partitions, revision = 41) {
  const payload = new Uint8Array(partitions.length * 16), view = new DataView(payload.buffer);
  partitions.forEach((p, i) => {
    const at = i * 16;
    view.setUint16(at, p.id, true); view.setUint16(at + 2, p.type, true);
    view.setUint32(at + 4, p.offset, true); view.setUint32(at + 8, p.size, true);
    view.setUint16(at + 12, p.schema, true); view.setUint16(at + 14, p.flags, true);
  });
  return builder.wrap(1, payload, revision);
}
function currentPartitions() {
  return todoSync.tableWithTodos(store.parsePackage(tableBytes(OLD_PARTITIONS))).partitions;
}
function config(text = '待办', resident = true, timing = { leadMinutes: 10, retainMinutes: 5 }) {
  return { limit: 5, fontSize: 12, fontBold: false, fontItalic: false, ...timing,
    items: [{ id: 1, text, resident, completed: false,
    start: resident ? 0 : 100, end: resident ? 0 : 200 }] };
}
const payload = () => todo.pack(config());
const validate = bytes => todo.unpack(bytes);

function fixture(options = {}) {
  let partitions = options.partitions || (options.legacy ? OLD_PARTITIONS : currentPartitions());
  const packages = new Map([[1, tableBytes(partitions, options.tableRevision ?? 41)]]);
  if (!options.legacy && !options.missingTodo) packages.set(9, builder.wrap(9, payload(), options.todoRevision ?? 7));
  if (options.fontPackage) packages.set(7, options.fontPackage.slice());
  const calls = [], uploaded = [], changed = new Set();
  let leaveFailures = options.leaveFailures || 0;
  let enterAttempts = 0;
  const client = {
    closed: false, entered: false,
    async status() {
      calls.push('status');
      const reply = Uint8Array.of(0xa7, 0, 0, options.statusCode || 0,
        options.flags ?? 0x13, 1, options.active || 0, Number(this.entered || !!options.entered),
        0, 0, 0, 0, 0, 0, 8, 0);
      if (options.disconnectAt === 'status') this.closed = true;
      return reply;
    },
    async partitions() { calls.push('partitions'); return store.parseTable(store.parsePackage(packages.get(1))); },
    async readPackage(partition) {
      calls.push(`read:${partition.id}`);
      if (options.disconnectAt === `read:${partition.id}`) this.closed = true;
      const source = packages.get(partition.id);
      if (!source) throw new Error('package absent');
      const bytes = source.slice();
      if (options.corruptReadId === partition.id) bytes[32] ^= 0x80;
      return bytes;
    },
    async readRange(offset, length) {
      calls.push(`header:${offset}`);
      assert.equal(length, 32);
      const part = partitions.find(p => p.offset === offset);
      assert.ok(part, `partition at ${offset}`);
      if (options.disconnectAt === 'header') this.closed = true;
      const source = packages.get(part.id), original = store.parsePackage(source);
      if (part.id === 1 && options.staleTable) {
        let bytes = original.payload;
        if (options.staleTable === 'crc') { bytes = bytes.slice(); bytes[1] ^= 1; }
        if (options.staleTable === 'length') bytes = Uint8Array.from([...bytes, ...new Uint8Array(16)]);
        return builder.wrap(1, bytes, original.revision + Number(options.staleTable === 'revision')).slice(0, 32);
      }
      return source.slice(0, 32);
    },
    async inspect(id) {
      calls.push(`verify:${id}`);
      if (options.inspectCode && !uploaded.includes(id)) return { code: options.inspectCode };
      if (!packages.has(id)) return { code: 4 };
      const pack = store.parsePackage(packages.get(id));
      const info = { code: 0, revision: pack.revision, length: pack.payload.length, crc: pack.crc };
      const mismatch = uploaded.includes(id) ? options.verifyMismatch : options.readMismatch;
      if (mismatch === 'code') info.code = 3;
      if (mismatch === 'revision') info.revision++;
      if (mismatch === 'length') info.length++;
      if (mismatch === 'crc') info.crc ^= 1;
      if (mismatch === 'null') return null;
      return info;
    },
    async upload(bytes, progress, knownPartitions) {
      const pack = store.parsePackage(bytes);
      store.validateTarget(pack, knownPartitions);
      calls.push(`upload:${pack.id}`);
      uploaded.push(pack.id);
      this.entered = true;
      if (options.uploadFailId === pack.id) throw new Error(`upload ${pack.id} failed`);
      if (options.disconnectAt === 'upload') this.closed = true;
      progress(pack.payload.length, pack.payload.length);
      packages.set(pack.id, bytes.slice());
      changed.add(pack.id);
      if (pack.id === 1) partitions = store.parseTable(pack);
    },
    async leave() {
      calls.push('leave');
      if (leaveFailures-- > 0) throw new Error('reload failed');
      for (const id of changed) calls.push(`apply:${id}`);
      changed.clear();
      this.entered = false;
    },
    async request(op, body) {
      calls.push(`request:${op}`);
      assert.ok(op === 5 || op === 6 || op === 2);
      if (op === 2) {
        assert.equal(body.length, 13); assert.equal(body[0], 9);
        packages.delete(9); changed.add(9);
      }
      if (op === 6) {
        assert.deepEqual(body, Uint8Array.of(77, 73, 71, 49));
        if (enterAttempts++ < (options.enterBusy || 0)) throw Object.assign(new Error('panel busy'), { code: 2 });
        if (options.enterFailCode) throw Object.assign(new Error('enter failed'), { code: options.enterFailCode });
        this.entered = true;
      }
    },
  };
  return { client, calls, uploaded, packages, get partitions() { return partitions; } };
}
const guard = f => () => { if (f.client.closed) throw new Error('connection changed'); };

test('legacy eight-partition migration carves only reserved space and preserves all installed addresses', async () => {
  const old = store.parsePackage(tableBytes(OLD_PARTITIONS, 41));
  const plan = todoSync.tableWithTodos(old), updated = store.parsePackage(plan.bytes);
  assert.equal(updated.revision, 42);
  assert.equal(plan.partitions.length, 9);
  for (const id of [1, 2, 3, 4, 5, 6, 7]) {
    assert.deepEqual(plan.partitions.find(p => p.id === id), OLD_PARTITIONS.find(p => p.id === id));
  }
  assert.deepEqual(plan.partitions.find(p => p.id === 9),
    { id: 9, type: 9, offset: 0x58000, size: 4096, schema: 1, flags: 0 });
  assert.deepEqual(plan.partitions.find(p => p.id === 8),
    { id: 8, type: 8, offset: 0x59000, size: 159744, schema: 1, flags: 0 });
  const f = fixture({ legacy: true });
  await todoSync.save(f.client, payload(), [{ slot: 1, text: '待办' }], {
    validate, ensureFonts: async () => f.calls.push('fonts:applied'),
  });
  assert.deepEqual(f.uploaded, [1, 9]);
  assert.ok(f.calls.indexOf('fonts:applied') < f.calls.indexOf('upload:1'));
  assert.ok(f.calls.indexOf('upload:1') < f.calls.indexOf('upload:9'));
  assert.equal(store.parsePackage(f.packages.get(9)).revision, 1);
  assert.deepEqual(validate(await todoSync.read(f.client, { validate })), config());
});

test('insufficient reserved space or incompatible existing TODO partitions cannot trigger writes', async () => {
  const fullTable = OLD_PARTITIONS.map(p => p.id === 8 ? { ...p, size: 8192 } : p).concat(
    Array.from({ length: 24 }, (_, i) => ({ id: i + 10, type: 10, offset: 0x5a000 + i * 4096,
      size: 4096, schema: 1, flags: 0 })));
  for (const partitions of [
    OLD_PARTITIONS.filter(p => p.id !== 8),
    OLD_PARTITIONS.map(p => p.id === 8 ? { ...p, size: 4096 } : p),
    OLD_PARTITIONS.map(p => p.id === 8 ? { ...p, type: 10 } : p),
    currentPartitions().map(p => p.id === 9 ? { ...p, type: 10 } : p),
    fullTable,
  ]) {
    const f = fixture({ legacy: true, partitions });
    await assert.rejects(todoSync.save(f.client, payload(), [], { validate }));
    assert.deepEqual(f.uploaded, []);
    assert.equal(f.client.entered, false);
  }
});

test('existing partition avoids table migration, advances only TODO revision, and verifies before applying', async () => {
  const f = fixture(), originalTable = f.packages.get(1).slice();
  assert.equal(await todoSync.save(f.client, payload(), [], { validate }), true);
  assert.deepEqual(f.uploaded, [9]);
  assert.deepEqual(f.packages.get(1), originalTable);
  assert.equal(store.parsePackage(f.packages.get(9)).revision, 8);
  assert.deepEqual(f.calls.slice(-3), ['verify:9', 'leave', 'apply:9']);
  assert.equal(f.client.entered, false);
  const missing = fixture({ missingTodo: true });
  await todoSync.save(missing.client, payload(), [], { validate });
  assert.deepEqual(missing.uploaded, [9]);
  assert.equal(store.parsePackage(missing.packages.get(9)).revision, 1);
});

test('exhausted revisions, invalid payloads, unsupported firmware and busy devices fail before writes', async () => {
  for (const options of [{ todoRevision: 0xffffffff }, { legacy: true, tableRevision: 0xffffffff },
    { flags: 1 }, { flags: 3 }, { flags: 17 }, { active: 9 }, { entered: true }, { statusCode: 3 }, { inspectCode: 3 }]) {
    const f = fixture(options);
    await assert.rejects(todoSync.save(f.client, payload(), [], { validate }));
    assert.deepEqual(f.uploaded, [], JSON.stringify(options));
  }
  const f = fixture();
  await assert.rejects(todoSync.save(f.client, Uint8Array.of(1), [], { validate }));
  assert.deepEqual(f.calls, []);
});

test('table revision, payload CRC and length are rechecked after font preparation and before erasing', async () => {
  for (const staleTable of ['revision', 'crc', 'length']) {
    const f = fixture({ legacy: true, staleTable });
    await assert.rejects(todoSync.save(f.client, payload(), [], { validate }), /分区表已变化/);
    assert.deepEqual(f.uploaded, []);
    assert.equal(f.client.entered, false);
  }
});

test('persistent write and recovery verification errors stay in maintenance and report incomplete recovery', async () => {
  for (const options of [{ uploadFailId: 9 }, { legacy: true, uploadFailId: 1 },
    { verifyMismatch: 'code' }, { verifyMismatch: 'revision' }, { verifyMismatch: 'length' },
    { verifyMismatch: 'crc' }, { verifyMismatch: 'null' }]) {
    const f = fixture(options);
    await assert.rejects(todoSync.save(f.client, payload(), [], { validate }), /恢复未完成/);
    assert.ok(f.calls.includes('request:5'), JSON.stringify(options));
    assert.equal(f.calls.includes('leave'), false);
    assert.equal(f.client.entered, true);
  }
});

test('the post-font refresh can delay ENTER without starting TODO writes until the panel is idle', async () => {
  const f = fixture({ enterBusy: 2 }), pauses = [];
  await todoSync.save(f.client, payload(), [], { validate, pause: async ms => {
    pauses.push(ms); assert.deepEqual(f.uploaded, []);
  } });
  assert.deepEqual(pauses, [250, 250]);
  assert.equal(f.calls.filter(call => call === 'request:6').length, 3);
  assert.deepEqual(f.uploaded, [9]);
  for (const options of [{ enterBusy: 1 }, { enterFailCode: 5 }]) {
    const blocked = fixture(options);
    await assert.rejects(todoSync.save(blocked.client, payload(), [], { validate, busyTimeout: 0 }));
    assert.deepEqual(blocked.uploaded, []);
    assert.equal(blocked.calls.includes('request:5'), false);
  }
});

test('missing partition and uncommitted package return null, while corrupt bytes and invalid TODO data throw', async () => {
  for (const options of [{ legacy: true }, { missingTodo: true }]) {
    const f = fixture(options);
    assert.equal(await todoSync.read(f.client, { validate }), null);
    assert.deepEqual(f.uploaded, []);
  }
  for (const options of [{ inspectCode: 3 }, { corruptReadId: 9 }]) {
    const f = fixture(options);
    await assert.rejects(todoSync.read(f.client, { validate }));
  }
  const f = fixture();
  f.packages.set(9, builder.wrap(9, Uint8Array.of(1, 2, 3), 7));
  await assert.rejects(todoSync.read(f.client, { validate }));
  f.packages.set(9, builder.wrap(7, payload(), 7));
  await assert.rejects(todoSync.read(f.client, { validate }));
});

test('readback must match the revision, length and CRC that VERIFY approved', async () => {
  for (const readMismatch of ['revision', 'length', 'crc']) {
    const f = fixture({ readMismatch });
    await assert.rejects(todoSync.read(f.client, { validate }), /变化|校验|验证/);
  }
});

test('connection guards reject before writes and never send cleanup traffic through a replaced connection', async () => {
  for (const disconnectAt of ['status', 'read:1', 'header', 'upload']) {
    const f = fixture({ disconnectAt });
    await assert.rejects(todoSync.save(f.client, payload(), [], { validate, assertCurrent: guard(f) }), /connection changed/);
    assert.equal(f.calls.includes('request:5'), false);
    if (disconnectAt !== 'upload') assert.deepEqual(f.uploaded, []);
  }
  const f = fixture();
  f.client.closed = true;
  await assert.rejects(todoSync.save(f.client, payload(), [], { assertCurrent: guard(f) }), /connection changed/);
  assert.deepEqual(f.calls, []);
  const reading = fixture({ disconnectAt: 'read:9' });
  await assert.rejects(todoSync.read(reading.client, { validate, assertCurrent: guard(reading) }), /connection changed/);
  const fontChange = fixture({ legacy: true });
  await assert.rejects(todoSync.save(fontChange.client, payload(), [{ slot: 1, text: '待办' }], {
    validate, assertCurrent: guard(fontChange), ensureFonts: async () => { fontChange.client.closed = true; },
  }), /connection changed/);
  assert.deepEqual(fontChange.uploaded, []);
  assert.equal(fontChange.calls.some(call => call === 'read:1'), true, 'TODO preflight precedes font preparation');
});

let fontPackage;
function actualFontPackage() {
  if (fontPackage) return fontPackage.slice();
  const output = fs.mkdtempSync(path.join(os.tmpdir(), 'epd-todo-fonts-'));
  try {
    const python = process.env.PYTHON_BINARY || (process.platform === 'win32' ? 'python' : 'python3');
    const result = spawnSync(python, ['tools/resource_pack.py', '--check', '--fonts', '--output', output],
      { cwd: ROOT, encoding: 'utf8' });
    assert.equal(result.status, 0, result.error?.message || result.stderr || result.stdout);
    fontPackage = Uint8Array.from(fs.readFileSync(path.join(output, 'fonts.ers')));
    assert.equal(store.parsePackage(fontPackage).id, 7);
    return fontPackage.slice();
  } finally { fs.rmSync(output, { recursive: true, force: true }); }
}
const rasterize = async () => ({ width: 3, height: 3, x: 0, y: -1, advance: 4,
  pixels: Uint8Array.of(1, 0, 1, 0, 1, 0, 1, 0, 1) });

test('real fonts.ers and font_sync commit glyphs and TODO v4 custom timing then reload once in a shared session', async () => {
  const f = fixture({ legacy: true, fontPackage: actualFontPackage() });
  const preferences = config('\uE345', true, { leadMinutes: 37, retainMinutes: 11 });
  const requests = [{ slot: 1, text: '\uE345' }], data = todo.pack(preferences);
  const old = store.parsePackage(f.packages.get(7));
  assert.equal(glyphs.gatherMissing(builder.decodeFonts(old.payload), requests).length, 1);
  await todoSync.save(f.client, data, requests, { validate, assertCurrent: guard(f),
    ensureFonts: need => fontSync.ensure(f.client, need, { rasterize, assertCurrent: guard(f), deferApply: true }) });
  assert.deepEqual(f.uploaded, [7, 1, 9]);
  assert.ok(f.calls.indexOf('verify:7') < f.calls.indexOf('apply:7'));
  assert.ok(f.calls.indexOf('apply:7') > f.calls.indexOf('upload:1'));
  assert.ok(f.calls.indexOf('apply:7') > f.calls.indexOf('upload:9'));
  assert.equal(f.calls.filter(call => call === 'leave').length, 1);
  assert.equal(f.calls.includes('request:6'), false);
  const next = store.parsePackage(f.packages.get(7));
  assert.equal(next.revision, old.revision + 1);
  assert.equal(glyphs.gatherMissing(builder.decodeFonts(next.payload), requests).length, 0);
  const readback = await todoSync.read(f.client, { validate });
  const view = new DataView(readback.buffer, readback.byteOffset, readback.byteLength);
  assert.equal(readback[4], 4); assert.equal(view.getUint16(97, true), 37); assert.equal(view.getUint16(99, true), 11);
  assert.deepEqual(validate(readback), preferences);
});

test('failed glyph generation, font verification and font application never commit TODO or migrate its table', async () => {
  for (const options of [{ rasterFailure: true }, { verifyMismatch: 'crc' }, { leaveFailures: 1 }]) {
    const f = fixture({ legacy: true, fontPackage: actualFontPackage(), ...options });
    const oldTable = f.packages.get(1).slice();
    await assert.rejects(todoSync.save(f.client, todo.pack(config('\uE345')), [{ slot: 1, text: '\uE345' }], {
      validate, ensureFonts: requests => fontSync.ensure(f.client, requests, {
        rasterize: options.rasterFailure ? async () => { throw new Error('font unavailable'); } : rasterize,
      }),
    }));
    assert.equal(f.uploaded.includes(1), false);
    assert.equal(f.uploaded.includes(9), false);
    assert.deepEqual(f.packages.get(1), oldTable);
    if (options.rasterFailure) assert.deepEqual(f.uploaded, []);
  }
});

test('custom family glyphs and TODO v4 read back after one shared-session activation', async () => {
  const f = fixture({ fontPackage: actualFontPackage() });
  const custom = { ...config('日历 A'), fontFamily: 'SimSun', fontSize: 9 };
  const requests = [{ slot: 3, sourceSlot: 0, fontFamily: custom.fontFamily, text: ' ...日历 A' }];
  const base = builder.decodeFonts(store.parsePackage(f.packages.get(7)).payload);
  const generated = [];
  assert.equal(await todoSync.save(f.client, todo.pack(custom), requests, {
    validate, ensureFonts: need => fontSync.ensure(f.client, need, { deferApply: true,
      rasterize: async (code, slot, header, options) => {
        generated.push(code); assert.equal(slot, 0); assert.equal(options.fontFamily, 'SimSun'); return rasterize();
      } }),
  }), true);
  assert.deepEqual(f.uploaded, [7, 9]);
  assert.equal(f.calls.filter(call => call === 'leave').length, 1);
  assert.equal(f.calls.includes('request:6'), false);
  assert.ok(f.calls.indexOf('upload:9') < f.calls.indexOf('apply:7'));
  assert.ok(generated.includes('A'.codePointAt(0)), 'existing base glyph must be regenerated in the selected family');
  const next = builder.decodeFonts(store.parsePackage(f.packages.get(7)).payload);
  for (let slot = 0; slot < 3; ++slot) assert.deepEqual(next[slot], base[slot]);
  assert.equal(next.customKey, builder.fontKey('SimSun', 0));
  assert.ok(builder.validateFont(next[3], 3).has(46), 'long-title ellipsis uses ASCII period glyphs');
  const readback = await todoSync.read(f.client, { validate });
  assert.equal(readback[4], 4);
  assert.deepEqual(todo.unpack(readback), custom);
});

test('failure before TODO writes either skips fonts in preflight or releases an acquired deferred font session', async () => {
  for (const options of [{ staleTable: 'revision' }, { inspectCode: 3 }]) {
    const f = fixture(options);
    await assert.rejects(todoSync.save(f.client, payload(), [{ slot: 1, text: 'A' }], {
      validate, ensureFonts: async () => { f.client.entered = true; return { deferred: true }; },
    }));
    assert.deepEqual(f.uploaded, []);
    assert.equal(f.calls.includes('request:5'), !options.inspectCode);
    assert.equal(f.client.entered, false);
  }
});

async function coupledFixture() {
  const oldConfig = { ...config('旧清单 A'), fontFamily: 'SimSun', fontSize: 9 };
  const oldFonts = (await glyphs.extendFonts(builder.decodeFonts(store.parsePackage(actualFontPackage()).payload),
    [{ slot: 3, sourceSlot: 0, fontFamily: 'SimSun', text: ' ...旧清单 A' }], { rasterize })).fonts;
  const oldFont = builder.wrap(7, builder.encodeFonts(oldFonts), 4), oldTodo = builder.wrap(9, todo.pack(oldConfig), 7);
  const f = fixture({ fontPackage: oldFont });
  f.packages.set(9, oldTodo);
  const nextConfig = { ...config('新清单 B'), fontFamily: 'KaiTi', fontSize: 24, fontBold: true, fontItalic: true };
  const requests = [{ slot: 3, sourceSlot: 1, fontFamily: 'KaiTi', fontSize: 24,
    fontBold: true, fontItalic: true, text: ' ...新清单 B' }];
  return { ...f, oldFont, oldTodo, oldTable: f.packages.get(1).slice(), nextConfig, requests };
}

function saveCoupled(f, options = {}) {
  return todoSync.save(f.client, todo.pack(f.nextConfig), f.requests, { validate, assertCurrent: guard(f), ...options,
    ensureFonts: requests => fontSync.ensure(f.client, requests, { rasterize, assertCurrent: guard(f), deferApply: true }) });
}

test('deferred font and TODO failures restore both original packages before leaving maintenance', async () => {
  for (const stage of ['font-upload', 'todo-upload', 'todo-verify', 'leave-before', 'leave-after']) {
    const f = await coupledFixture();
    const upload = f.client.upload.bind(f.client), inspect = f.client.inspect.bind(f.client), leave = f.client.leave.bind(f.client);
    const request = f.client.request.bind(f.client);
    let failed = false, busy = 0;
    f.client.request = async (op, body) => {
      if (stage === 'leave-after' && failed && op === 6 && busy++ < 2)
        throw Object.assign(new Error('panel busy after successful apply'), { code: 2 });
      return request(op, body);
    };
    f.client.upload = async (bytes, ...args) => {
      const id = store.parsePackage(bytes).id;
      await upload(bytes, ...args);
      if (!failed && (stage === 'font-upload' && id === 7 || stage === 'todo-upload' && id === 9)) {
        failed = true;
        // Model an erased header or a lost commit ACK after an actual mutation.
        f.packages.set(id, new Uint8Array(32));
        throw new Error(`injected ${stage}`);
      }
    };
    f.client.inspect = async id => {
      const info = await inspect(id);
      if (!failed && stage === 'todo-verify' && id === 9 && f.uploaded.includes(9)) {
        failed = true; return { ...info, code: 3 };
      }
      return info;
    };
    f.client.leave = async () => {
      if (!failed && stage === 'leave-before') { failed = true; throw new Error(`injected ${stage}`); }
      await leave();
      if (!failed && stage === 'leave-after') { failed = true; throw new Error(`injected ${stage}`); }
    };
    await assert.rejects(saveCoupled(f, { pause: async () => {} }), new RegExp(`injected ${stage}|未确认待办校验通过`));
    assert.deepEqual(f.packages.get(1), f.oldTable, stage);
    assert.deepEqual(f.packages.get(7), f.oldFont, stage);
    assert.deepEqual(f.packages.get(9), f.oldTodo, stage);
    assert.equal(f.client.entered, false, stage);
    assert.ok(f.calls.includes('request:5'), stage);
    assert.ok(f.calls.includes('leave'), stage);
    if (stage === 'leave-after') assert.equal(busy, 3);
    const fonts = builder.decodeFonts(store.parsePackage(f.packages.get(7)).payload);
    const data = store.parsePackage(f.packages.get(9)).payload;
    assert.equal(fonts.customKey, new DataView(data.buffer, data.byteOffset).getUint32(12, true), stage);
  }
});

test('exhausted TODO revision and invalid old TODO payload fail before any font preparation', async () => {
  for (const invalid of ['revision', 'payload']) {
    const f = await coupledFixture();
    f.packages.set(9, builder.wrap(9, invalid === 'payload' ? Uint8Array.of(1, 2, 3)
      : store.parsePackage(f.oldTodo).payload, invalid === 'revision' ? 0xffffffff : 7));
    await assert.rejects(saveCoupled(f));
    assert.deepEqual(f.uploaded, [], invalid);
    assert.equal(f.calls.includes('read:7'), false, invalid);
    assert.deepEqual(f.packages.get(7), f.oldFont, invalid);
  }
});

test('a disconnected deferred font update never replays recovery snapshots to another connection', async () => {
  const f = await coupledFixture(), upload = f.client.upload.bind(f.client);
  f.client.upload = async (bytes, ...args) => {
    await upload(bytes, ...args);
    f.client.closed = true;
  };
  await assert.rejects(saveCoupled(f), /connection changed.*恢复未完成/);
  assert.deepEqual(f.uploaded, [7]);
  assert.equal(f.calls.includes('request:5'), false);
  assert.equal(f.calls.includes('leave'), false);
  assert.deepEqual(f.packages.get(9), f.oldTodo);
});

test('failed first TODO installation restores the original table or original absent record and old fonts', async () => {
  for (const legacy of [false, true]) {
    const f = fixture({ legacy, missingTodo: true, fontPackage: actualFontPackage() });
    const oldTable = f.packages.get(1).slice(), oldFont = f.packages.get(7).slice();
    const preferences = { ...config('首次事项'), fontFamily: 'KaiTi', fontSize: 24, fontBold: true };
    const requests = [{ slot: 3, sourceSlot: 1, text: ' ...首次事项', fontFamily: 'KaiTi', fontSize: 24, fontBold: true }];
    const inspect = f.client.inspect.bind(f.client);
    let failed = false;
    f.client.inspect = async id => {
      const info = await inspect(id);
      if (!failed && id === 9 && f.uploaded.includes(9)) { failed = true; return { ...info, crc: info.crc ^ 1 }; }
      return info;
    };
    await assert.rejects(todoSync.save(f.client, todo.pack(preferences), requests, {
      validate, ensureFonts: need => fontSync.ensure(f.client, need, { rasterize, deferApply: true }),
    }), /未确认待办校验通过/);
    assert.deepEqual(f.packages.get(1), oldTable);
    assert.deepEqual(f.packages.get(7), oldFont);
    assert.equal(f.client.entered, false);
    assert.equal(f.partitions.some(partition => partition.id === 9), !legacy);
    assert.equal(f.calls.includes('request:2'), !legacy);
    if (!legacy) assert.equal((await f.client.inspect(9)).code, 4);
  }
});
