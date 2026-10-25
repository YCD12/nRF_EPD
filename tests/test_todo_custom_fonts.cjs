'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const builder = require('../html/js/resource_builder.js');
const store = require('../html/js/resource_store.js');
const glyphs = require('../html/js/font_builder.js');
const fontSync = require('../html/js/font_sync.js');

const baseFonts = () => ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn']
  .map(name => new Uint8Array(fs.readFileSync(`tools/dist/u8g2_font_${name}.bin`)));
const request = (sourceSlot = 0, fontFamily = 'SimSun', text = ' …年月A') => ({ slot: 3, sourceSlot, fontFamily, text });
const rasterize = async () => ({ width: 3, height: 3, x: 0, y: -1, advance: 4,
  pixels: Uint8Array.of(1, 0, 1, 0, 1, 0, 1, 0, 1) });
const plain = value => JSON.parse(JSON.stringify(value));

function legacyPack(api, config = api) {
  const modern = api.pack(config), trailer = 16 + new DataView(modern.buffer).getUint16(6, true) * 80;
  const version = api.usesTypography(config) ? 3 : config.fontFamily ? 2 : 1;
  const tail = version === 3 ? 1 : 0, bytes = new Uint8Array(trailer + tail + modern[11]);
  bytes.set(modern.subarray(0, trailer)); bytes[4] = version;
  if (tail) bytes[trailer] = modern[trailer];
  bytes.set(modern.subarray(trailer + 5), trailer + tail);
  return bytes;
}

test('resource editor imports the seven-package template and retains custom fonts when generating a project', async () => {
  const template = require('../html/resources/default-project.json');
  const fonts = (await glyphs.extendFonts(baseFonts(), [request()], { rasterize })).fonts;
  const entries = template.files.map(file => ({ name: file.name, bytes: new Uint8Array(Buffer.from(file.base64, 'base64')) }));
  const fontEntry = entries.find(entry => store.parsePackage(entry.bytes).id === 7);
  fontEntry.bytes = builder.wrap(7, builder.encodeFonts(fonts), 8);
  const base = store.prepareBundle(entries), elements = new Map();
  const canvasContext = { clearRect() {}, fillRect() {}, strokeRect() {}, fillText() {} };
  function element(id) {
    if (id === 'resourceFont3') return null;
    if (!elements.has(id)) elements.set(id, { value: id === 'resourcePreviewMode' ? '2' : '', files: [],
      classList: { toggle() {} }, getContext: () => canvasContext, addEventListener(name, listener) { this[name] = listener; } });
    return elements.get(id);
  }
  const loaded = [], context = { EPDUIModel: require('../html/js/ui_tools/ui_model.js'), EPDUIRenderer: require('../html/js/ui_tools/ui_preview.js'), EPDResourceBuilder: builder, EPDResourceStore: store,
    EPDResourcePack: require('../html/js/ui_tools/resource_pack.js'),
    EPDFontStoreTool: require('../html/js/ui_tools/font_store_tool.js'),
    Uint8Array, document: { getElementById: element }, atob: value => Buffer.from(value, 'base64').toString('binary'),
    fetch: async url => ({ ok: true, arrayBuffer: async () =>
      new Uint8Array(fs.readFileSync(require('node:path').join(__dirname, '../html', url))).buffer }),
    EPDWebFonts: { extendFonts: (input, requests) => glyphs.extendFonts(input, requests, { rasterize }) },
    ExternalResourceInstaller: { busy: () => false, getBundle: () => base, loadBundle: async entries => loaded.push(entries) },
  };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_editor.js'), 'utf8'), context);
  await context.ExternalResourceEditor.loadDefault();
  assert.equal(loaded.length, 1, element('resourceEditorStatus').textContent);
  assert.equal(loaded[0].length, 7);
  const generated = await context.ExternalResourceEditor.generate();
  assert.ok(generated, element('resourceEditorStatus').textContent);
  const decoded = builder.decodeFonts(generated.packages.find(item => item.pack.id === 7).pack.payload);
  assert.equal(decoded.customKey, fonts.customKey); assert.deepEqual(decoded[3], fonts[3]);
});

function todoFixture(read = null) {
  const owner = {}, context = { Uint8Array, DataView, TextEncoder, TextDecoder, Date, Set, Map,
    EPDUIModel: require('../html/js/ui_tools/ui_model.js'), EPDUIRenderer: require('../html/js/ui_tools/ui_preview.js'), EPDResourceBuilder: builder, appVersion: 0x3b, epdCharacteristic: {},
    ExternalResourceInstaller: { busy: () => false, readTodo: async () => read, saveTodo: async () => true },
    DeviceOperations: { current: null, assert(actual) { assert.equal(actual, owner); assert.equal(this.current, owner); } },
  };
  context.runDeviceOperation = async (_, action) => {
    context.DeviceOperations.current = owner;
    try { return await action(owner); } finally { context.DeviceOperations.current = null; }
  };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/todo_calendar.js'), 'utf8'), context);
  return context.TodoCalendar;
}

test('custom todo fonts regenerate existing base glyphs in slot 3 without changing the three base fonts', async () => {
  const fonts = baseFonts(), before = fonts.map(font => font.slice()), calls = [];
  assert.ok(builder.validateFont(fonts[0], 0).has('年'.codePointAt(0)));
  const req = request(0, 'SimSun', ' …年年');
  const result = await glyphs.extendFonts(fonts, [req], { rasterize: async (code, slot, header, options) => {
    calls.push({ code, slot, family: options.fontFamily });
    assert.deepEqual(header.subarray(9, 17), fonts[0].subarray(9, 17));
    return rasterize();
  } });
  assert.deepEqual(calls.map(call => call.code), [32, 0x2026, 0x5e74]);
  assert.ok(calls.every(call => call.slot === 0 && call.family === 'SimSun'));
  for (let slot = 0; slot < 3; slot++) {
    assert.equal(result.fonts[slot], fonts[slot]);
    assert.deepEqual(fonts[slot], before[slot]);
  }
  assert.equal(result.fonts.length, 4);
  assert.equal(result.fonts.customKey, builder.fontKey('SimSun', 0));
  assert.deepEqual([...builder.validateFont(result.fonts[3], 3)].sort((a, b) => a - b), [32, 0x2026, 0x5e74]);
});

test('the same family and size reuse generated glyphs, while changing either regenerates the requested glyphs', async () => {
  const req = request(), initial = await glyphs.extendFonts(baseFonts(), [req], { rasterize });
  const reused = await glyphs.extendFonts(initial.fonts, [req], { rasterize: async () => assert.fail('already available glyphs must be reused') });
  assert.deepEqual(reused.added, []);
  assert.equal(reused.fonts[3], initial.fonts[3]);
  assert.equal(reused.fonts.customKey, initial.fonts.customKey);
  for (const next of [request(0, 'KaiTi'), request(1, 'SimSun')]) {
    let calls = 0;
    const changed = await glyphs.extendFonts(initial.fonts, [next], { rasterize: async (_, slot, header, options) => {
      ++calls; assert.equal(slot, next.sourceSlot); assert.equal(options.fontFamily, next.fontFamily);
      assert.deepEqual(header.subarray(9, 17), initial.fonts[next.sourceSlot].subarray(9, 17));
      return rasterize();
    } });
    assert.equal(calls, new Set(next.text).size);
    assert.notEqual(changed.fonts.customKey, initial.fonts.customKey);
    assert.notEqual(changed.fonts[3], initial.fonts[3]);
    for (let slot = 0; slot < 3; slot++) assert.equal(changed.fonts[slot], initial.fonts[slot]);
    assert.equal(initial.fonts.customKey, builder.fontKey('SimSun', 0));
  }
});

test('same-family extensions retain earlier custom glyphs and mismatched slot geometry forces regeneration', async () => {
  const first = (await glyphs.extendFonts(baseFonts(), [request(0, 'SimSun', ' …年')], { rasterize })).fonts;
  const extended = await glyphs.extendFonts(first, [request(0, 'SimSun', ' …月')], { rasterize });
  assert.deepEqual(extended.added, [{ slot: 3, codePoint: '月'.codePointAt(0) }]);
  assert.ok(builder.validateFont(extended.fonts[3], 3).has('年'.codePointAt(0)));
  const wrongGeometry = first.slice(); wrongGeometry.customKey = first.customKey; wrongGeometry[3] = first[1];
  const corrected = await glyphs.extendFonts(wrongGeometry, [request(0, 'SimSun', ' …年')], { rasterize });
  assert.equal(corrected.added.length, 3);
  assert.deepEqual(corrected.fonts[3].subarray(9, 17), first[0].subarray(9, 17));
});

test('failed custom rasterization and conflicting custom requests leave the installed fonts unchanged', async () => {
  const fonts = (await glyphs.extendFonts(baseFonts(), [request()], { rasterize })).fonts;
  const before = fonts.map(font => font.slice()), oldKey = fonts.customKey;
  let calls = 0;
  await assert.rejects(glyphs.extendFonts(fonts, [request(1, 'KaiTi')], { rasterize: async () => {
    if (++calls === 2) throw new Error('unsupported font'); return rasterize();
  } }), /unsupported font/);
  fonts.forEach((font, index) => assert.deepEqual(font, before[index]));
  assert.equal(fonts.customKey, oldKey);
  for (const requests of [[request(), request(1)], [request(), request(0, 'KaiTi')],
    [{ ...request(), sourceSlot: 2 }], [{ ...request(), fontFamily: '' }]])
    await assert.rejects(glyphs.extendFonts(fonts, requests, { rasterize }));
});

test('four-slot FPK packages round-trip the custom font key and reject malformed directories, keys or geometry', async () => {
  const fonts = (await glyphs.extendFonts(baseFonts(), [request(1, 'SimSun')], { rasterize })).fonts;
  const bytes = builder.encodeFonts(fonts), view = new DataView(bytes.buffer);
  assert.equal(view.getUint16(6, true), 4); assert.equal(view.getUint32(68, true), fonts.customKey);
  const decoded = builder.decodeFonts(bytes);
  assert.equal(decoded.customKey, fonts.customKey); decoded.forEach((font, index) => assert.deepEqual(font, fonts[index]));
  assert.deepEqual(builder.encodeFonts(decoded), bytes);
  for (const mutate of [
    (copy, v) => v.setUint32(68, 0, true),
    (copy, v) => v.setUint32(20, 123, true),
    (copy, v) => v.setUint16(6, 5, true),
    (copy, v) => v.setUint32(56, 56, true),
    (copy, v) => { const at = v.getUint32(56, true), len = v.getUint32(60, true); copy[at + 9] = 65;
      v.setUint32(64, store.crc32(copy.subarray(at, at + len)), true); },
    (copy, v) => copy[v.getUint32(56, true) + 23] ^= 1,
  ]) {
    const copy = bytes.slice(); mutate(copy, new DataView(copy.buffer)); assert.throws(() => builder.decodeFonts(copy));
  }
  const missingKey = fonts.slice(); assert.throws(() => builder.encodeFonts(missingKey), /标识/);
});

test('ordinary three-slot font upgrades preserve the custom slot and its identity exactly', async () => {
  const fonts = (await glyphs.extendFonts(baseFonts(), [request(1, 'SimSun')], { rasterize })).fonts;
  const installed = builder.wrap(7, builder.encodeFonts(fonts), 8);
  const incomingFonts = baseFonts(), incoming = builder.wrap(7, builder.encodeFonts(incomingFonts), 9);
  const result = fontSync.preserve(incoming, installed, [{ id: 7, type: 7, size: 262144 }]);
  const pack = store.parsePackage(result.bytes), decoded = builder.decodeFonts(pack.payload);
  assert.equal(pack.revision, 9); assert.equal(decoded.length, 4); assert.equal(decoded.customKey, fonts.customKey);
  assert.deepEqual(pack.payload, store.parsePackage(installed).payload);
  assert.deepEqual(decoded[3], fonts[3]);
  for (let slot = 0; slot < 3; slot++) assert.deepEqual(decoded[slot], incomingFonts[slot]);
  assert.ok(result.added.some(item => item.slot === 3));
  assert.equal(fontSync.preserve(result.bytes, installed, [{ id: 7, type: 7, size: 262144 }]).bytes, result.bytes);
});

test('ordinary four-slot upgrades preserve the personal TODO dependency while merging incoming base fonts', async () => {
  const record = (font, code) => {
    const word = at => font[at] * 256 + font[at + 1];
    let at = 23 + word(21); at += word(at);
    while (word(at)) {
      const length = font[at + 2];
      if (word(at) === code) return font.slice(at, at + length);
      at += length;
    }
    assert.fail(`Missing glyph U+${code.toString(16)}`);
  };
  const installedFonts = (await glyphs.extendFonts(baseFonts(), [request(0, 'SimSun', ' ...A'),
    { slot: 0, text: '\uE101\uE103' }], { rasterize })).fonts;
  const newerGlyph = async () => ({ ...(await rasterize()), pixels: Uint8Array.of(0, 1, 0, 1, 1, 1, 0, 1, 0) });
  const incomingFonts = (await glyphs.extendFonts(baseFonts(), [request(0, 'KaiTi', ' ...B'),
    { slot: 0, text: '\uE101\uE102' }], { rasterize: newerGlyph })).fonts;
  const installed = builder.wrap(7, builder.encodeFonts(installedFonts), 8);
  const incoming = builder.wrap(7, builder.encodeFonts(incomingFonts), 9);
  const result = fontSync.preserve(incoming, installed, [{ id: 7, type: 7, size: 262144 }]);
  const decoded = builder.decodeFonts(store.parsePackage(result.bytes).payload);
  assert.equal(result.customPreserved, true);
  assert.equal(decoded.customKey, installedFonts.customKey);
  assert.deepEqual(decoded[3], installedFonts[3]);
  assert.equal(glyphs.gatherMissing(decoded, [{ slot: 0, text: '\uE101\uE102\uE103' }]).length, 0);
  assert.deepEqual(glyphs.decodeGlyph(decoded[0], 0xE101, record(decoded[0], 0xE101)),
    glyphs.decodeGlyph(incomingFonts[0], 0xE101, record(incomingFonts[0], 0xE101)));
  assert.deepEqual(glyphs.decodeGlyph(decoded[0], 0xE103, record(decoded[0], 0xE103)),
    glyphs.decodeGlyph(installedFonts[0], 0xE103, record(installedFonts[0], 0xE103)));
  assert.equal(fontSync.preserve(result.bytes, installed, [{ id: 7, type: 7, size: 262144 }]).bytes, result.bytes);
});

test('personal TODO preservation retains installed glyph bytes even when the incoming custom identity matches', async () => {
  const installedFonts = (await glyphs.extendFonts(baseFonts(), [request(0, 'SimSun', ' ...A')], { rasterize })).fonts;
  const incomingFonts = (await glyphs.extendFonts(baseFonts(), [request(0, 'SimSun', ' ...A')], {
    rasterize: async () => ({ ...(await rasterize()), pixels: Uint8Array.of(0, 1, 0, 1, 1, 1, 0, 1, 0) }),
  })).fonts;
  const installed = builder.wrap(7, builder.encodeFonts(installedFonts), 8);
  const incoming = builder.wrap(7, builder.encodeFonts(incomingFonts), 9);
  const decoded = builder.decodeFonts(store.parsePackage(fontSync.preserve(incoming, installed,
    [{ id: 7, type: 7, size: 262144 }]).bytes).payload);
  assert.equal(decoded.customKey, installedFonts.customKey);
  assert.deepEqual(decoded[3], installedFonts[3]);
  assert.notDeepEqual(decoded[3], incomingFonts[3]);
});

test('font merge replacement stays available while regular installation explicitly preserves personal TODO fonts', async () => {
  const installed = (await glyphs.extendFonts(baseFonts(), [request(0, 'SimSun')], { rasterize })).fonts;
  const incoming = (await glyphs.extendFonts(baseFonts(), [request(1, 'KaiTi')], { rasterize })).fonts;
  const replacing = glyphs.mergeFonts(incoming, installed);
  assert.equal(replacing.fonts.customKey, incoming.customKey);
  assert.deepEqual(replacing.fonts[3], incoming[3]);
  const preserving = glyphs.mergeFonts(incoming, installed, { preserveCustom: true });
  assert.equal(preserving.fonts.customKey, installed.customKey);
  assert.deepEqual(preserving.fonts[3], installed[3]);
});

test('an installed custom font with no glyph records is preserved even when the added list is empty', async () => {
  const installed = baseFonts(), empty = new Uint8Array(31);
  empty.set([1, 0, 4, 4, 7, 7, 8, 8, 8]);
  empty.set(builder.todoFontGeometry(32).map(n => n & 255), 9);
  empty[22] = 2; empty.set([0, 4, 255, 255], 25);
  installed[3] = empty; installed.customKey = builder.todoFontKey('SimSun', 32, true, true);
  const incoming = (await glyphs.extendFonts(baseFonts(), [request(0, 'KaiTi')], { rasterize })).fonts;
  const result = fontSync.preserve(builder.wrap(7, builder.encodeFonts(incoming), 9),
    builder.wrap(7, builder.encodeFonts(installed), 8), [{ id: 7, type: 7, size: 262144 }]);
  assert.equal(result.customPreserved, true);
  assert.deepEqual(result.added, []);
  const decoded = builder.decodeFonts(store.parsePackage(result.bytes).payload);
  assert.equal(decoded.customKey, installed.customKey);
  assert.deepEqual(decoded[3], empty);
});

test('TODO v2 round-trips the selected custom family and size, while default TODO remains byte-compatible v1', () => {
  const api = todoFixture(); api.items = [{ id: 3, start: 0, end: 0, resident: true, completed: false, text: '年会 A' }];
  for (const fontSize of [9, 12]) {
    api.fontFamily = 'SimSun'; api.fontSize = fontSize;
    const bytes = legacyPack(api), view = new DataView(bytes.buffer);
    assert.equal(bytes[4], 2); assert.equal(bytes[10], 3); assert.equal(bytes[11], 6);
    assert.equal(view.getUint32(12, true), builder.fontKey('SimSun', fontSize === 9 ? 0 : 1));
    assert.equal(bytes.length, 16 + 80 + 6);
    assert.equal(Buffer.from(bytes.subarray(96)).toString('utf8'), 'SimSun');
    const decoded = api.unpack(bytes);
    assert.deepEqual(plain(decoded), { items: api.items, limit: 5, fontSize, fontFamily: 'SimSun', leadMinutes: 0, retainMinutes: 0 });
    assert.deepEqual(legacyPack(api, decoded), bytes);
    const empty = legacyPack(api, { items: [], limit: 2, fontSize, fontFamily: '宋体' });
    assert.equal(api.unpack(empty).fontFamily, '宋体');
    api.fontFamily = '';
    const fallback = legacyPack(api); assert.equal(fallback[4], 1); assert.equal(fallback.length, 96);
    assert.equal(Object.hasOwn(api.unpack(fallback), 'fontFamily'), false);
  }
});

test('TODO v2 rejects wrong font slots, font keys, trailing families and record flags before applying data', () => {
  const api = todoFixture(); api.fontFamily = 'SimSun';
  api.items = [{ id: 1, start: 0, end: 0, resident: true, completed: false, text: '工作' }];
  const bytes = legacyPack(api);
  for (const mutate of [
    (copy, view) => { copy[10] = 1; }, (copy, view) => { copy[11] = 0; },
    (copy, view) => { view.setUint32(12, 0, true); },
    (copy, view) => { view.setUint32(12, builder.fontKey('SimSun', 0), true); },
    (copy, view) => { copy[96] = 'K'.charCodeAt(0); },
    (copy, view) => { copy[28] |= 4; },
    (copy, view) => { copy[78] = 1; },
  ]) {
    const copy = bytes.slice(); mutate(copy, new DataView(copy.buffer)); assert.throws(() => api.unpack(copy));
  }
  assert.throws(() => api.unpack(bytes.subarray(0, bytes.length - 1)));
  const extra = new Uint8Array(bytes.length + 1); extra.set(bytes); assert.throws(() => api.unpack(extra));
});

test('device readback applies custom family and clears it for default or absent TODO resources', async () => {
  const source = todoFixture(); source.fontFamily = 'SimSun'; source.fontSize = 9;
  source.items = [{ id: 1, start: 0, end: 0, resident: true, completed: false, text: '准备资料' }];
  const custom = todoFixture(source.pack()); assert.equal(await custom.loadFromDevice(), true);
  assert.equal(custom.fontFamily, 'SimSun'); assert.equal(custom.fontSize, 9);
  assert.deepEqual(plain(custom.fontRequests()), [request(0, 'SimSun', ' ...准备资料')]);
  source.fontFamily = '';
  for (const bytes of [source.pack(), null]) {
    const fallback = todoFixture(bytes); fallback.fontFamily = 'KaiTi';
    assert.equal(await fallback.loadFromDevice(), true); assert.equal(fallback.fontFamily, '');
  }
  const malformed = source.pack(); malformed[28] |= 4;
  const rejected = todoFixture(malformed); rejected.fontFamily = 'KaiTi';
  assert.equal(await rejected.loadFromDevice(), false); assert.equal(rejected.fontFamily, 'KaiTi');
});

test('TODO v3 round-trips every larger size and styled legacy size with default or selected browser families', () => {
  const api = todoFixture(), items = [{ id: 3, start: 0, end: 0, resident: true, completed: false, text: '资料 fj' }];
  for (const fontSize of [9, 12, 16, 20, 24, 32]) for (const fontBold of [false, true])
    for (const fontItalic of [false, true]) for (const fontFamily of ['', 'SimSun']) {
    if (fontSize <= 12 && !fontBold && !fontItalic) continue;
    const config = { items, limit: 5, fontSize, fontFamily, fontBold, fontItalic }, bytes = legacyPack(api, config);
    const view = new DataView(bytes.buffer), trailer = 16 + 80;
    assert.equal(bytes[4], 3); assert.equal(bytes[10], 3); assert.equal(bytes[11], fontFamily.length);
    assert.equal(bytes.length, trailer + 1 + fontFamily.length);
    assert.equal(bytes[trailer], Number(fontBold) | (Number(fontItalic) << 1));
    assert.equal(view.getUint32(12, true), builder.todoFontKey(fontFamily, fontSize, fontBold, fontItalic));
    const decoded = api.unpack(bytes); assert.deepEqual(plain(decoded), { ...config, leadMinutes: 0, retainMinutes: 0 });
    assert.deepEqual(legacyPack(api, decoded), bytes);
  }
});

test('TODO v3 rejects unsupported flags, point sizes, mismatched style keys, trailing data and noncanonical versions', () => {
  const api = todoFixture(), config = { items: [], limit: 5, fontSize: 32, fontFamily: '', fontBold: true, fontItalic: true };
  const bytes = legacyPack(api, config);
  for (const mutate of [
    copy => { copy[9] = 18; }, copy => { copy[10] = 1; }, copy => { copy[16] |= 4; },
    copy => { copy[16] = 1; }, copy => { copy[11] = 1; }, copy => { copy[4] = 2; },
    copy => { new DataView(copy.buffer).setUint32(12, builder.fontKey('SimSun', 1), true); },
  ]) { const copy = bytes.slice(); mutate(copy); assert.throws(() => api.unpack(copy)); }
  assert.throws(() => api.unpack(bytes.subarray(0, 16)));
  const extra = new Uint8Array(bytes.length + 1); extra.set(bytes); assert.throws(() => api.unpack(extra));
  const ordinary = legacyPack(api, { ...config, fontSize: 12 }); ordinary[16] = 0;
  new DataView(ordinary.buffer).setUint32(12, builder.todoFontKey('', 12), true);
  assert.throws(() => api.unpack(ordinary), /版本与设置/);
});

test('changing larger TODO family, size, bold or italic regenerates available glyphs with a new identity', async () => {
  const req = { ...request(1, '', ' ...年'), fontSize: 24, fontBold: true, fontItalic: false };
  const initial = (await glyphs.extendFonts(baseFonts(), [req], { rasterize })).fonts;
  const reused = await glyphs.extendFonts(initial, [req], { rasterize: async () => assert.fail('already available') });
  assert.deepEqual(reused.added, []); assert.equal(reused.fonts[3], initial[3]);
  for (const change of [{ fontFamily: 'SimSun' }, { fontSize: 32 }, { fontBold: false }, { fontItalic: true }]) {
    const next = { ...req, ...change }, calls = [];
    const changed = await glyphs.extendFonts(initial, [next], { rasterize: async (code, slot, header, options) => {
      calls.push(code); assert.equal(slot, 1); assert.equal(options.fontSize, next.fontSize);
      assert.equal(options.fontFamily, next.fontFamily); assert.equal(options.fontBold, next.fontBold);
      assert.equal(options.fontItalic, next.fontItalic); return rasterize();
    } });
    assert.equal(calls.length, new Set(next.text).size);
    assert.notEqual(changed.fonts.customKey, initial.customKey);
    assert.equal(changed.fonts.customKey, builder.todoFontKey(next.fontFamily, next.fontSize, next.fontBold, next.fontItalic));
    for (let slot = 0; slot < 3; ++slot) assert.equal(changed.fonts[slot], initial[slot]);
  }
  const extension = await glyphs.extendFonts(initial, [{ ...req, text: ' ...月' }], { rasterize });
  assert.deepEqual(extension.added, [{ slot: 3, codePoint: '月'.codePointAt(0) }]);
  assert.ok(builder.validateFont(extension.fonts[3], 3).has('年'.codePointAt(0)));
  const wrong = initial.slice(); wrong.customKey = initial.customKey; wrong[3] = wrong[1];
  const corrected = await glyphs.extendFonts(wrong, [req], { rasterize });
  assert.equal(corrected.added.length, new Set(req.text).size);
  assert.deepEqual(Array.from(corrected.fonts[3].subarray(9, 17)), builder.todoFontGeometry(24).map(n => n & 255));
});

test('larger TODO font requests, device readback and ordinary font upgrades preserve styles and canonical geometry', async () => {
  const source = todoFixture(); Object.assign(source, { fontSize: 32, fontFamily: '', fontBold: true, fontItalic: true,
    items: [{ id: 1, start: 0, end: 0, resident: true, completed: false, text: '准备资料 fj' }] });
  const read = todoFixture(source.pack()); assert.equal(await read.loadFromDevice(), true);
  assert.equal(read.fontSize, 32); assert.equal(read.fontBold, true); assert.equal(read.fontItalic, true);
  const requests = plain(read.fontRequests());
  assert.deepEqual(requests, [{ slot: 3, sourceSlot: 1, fontFamily: '', fontSize: 32,
    fontBold: true, fontItalic: true, text: ' ...准备资料 fj' }]);
  const fonts = (await glyphs.extendFonts(baseFonts(), requests, { rasterize })).fonts;
  assert.equal(fonts.customKey, new DataView(source.pack().buffer).getUint32(12, true));
  const installed = builder.wrap(7, builder.encodeFonts(fonts), 8);
  const incoming = builder.wrap(7, builder.encodeFonts(baseFonts()), 9);
  const preserved = fontSync.preserve(incoming, installed, [{ id: 7, type: 7, size: 262144 }]);
  const decoded = builder.decodeFonts(store.parsePackage(preserved.bytes).payload);
  assert.equal(decoded.customKey, fonts.customKey); assert.deepEqual(decoded[3], fonts[3]);
  const plainSource = todoFixture(); const fallback = todoFixture(plainSource.pack());
  fallback.fontBold = fallback.fontItalic = true; assert.equal(await fallback.loadFromDevice(), true);
  assert.equal(fallback.fontBold, false); assert.equal(fallback.fontItalic, false);
});
