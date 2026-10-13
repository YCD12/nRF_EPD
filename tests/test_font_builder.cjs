const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const codec = require('../html/js/font_builder.js');
const builder = require('../html/js/resource_builder.js');
const store = require('../html/js/resource_store.js');
const template = require('../html/resources/default-project.json');

const baseFonts = () => builder.open(store.prepareBundle(template.files.map(file => ({
  name: file.name, bytes: new Uint8Array(Buffer.from(file.base64, 'base64')),
})))).fonts;
const word = (bytes, offset) => bytes[offset] * 256 + bytes[offset + 1];

test('batch generation reports progress, yields, and cancellation leaves the original fonts intact', async () => {
  const input = baseFonts(), original = input.map(font => font.slice()), signal = { aborted: false }, progress = [];
  let yields = 0;
  await assert.rejects(codec.extendFonts(input, [{ slot: 0, text: '新书桌' }], { signal, batchSize: 1,
    rasterize: async () => ({ width: 1, height: 1, x: 0, y: 0, advance: 2, pixels: Uint8Array.of(1) }),
    onProgress: info => progress.push(info), yield: async () => { yields++; signal.aborted = true; } }), /取消/);
  assert.equal(yields, 1); assert.equal(progress[0].completed, 0); assert.equal(progress[1].completed, 1);
  assert.deepEqual(input, original);
});

test('redrawing a base slot replaces existing glyphs and retains all other slots byte for byte', async () => {
  const input = baseFonts(), seen = new Set();
  const result = await codec.extendFonts(input, [{ slot: 2, text: '012' }], { rebuildSlots: [2],
    rasterize: async code => { seen.add(code); return { width: 1, height: 1, x: 0, y: 0, advance: 2, pixels: Uint8Array.of(1) }; } });
  assert.ok(seen.has(48)); assert.ok(seen.has(57));
  assert.deepEqual(result.fonts[0], input[0]); assert.deepEqual(result.fonts[1], input[1]);
  assert.notDeepEqual(result.fonts[2], input[2]);
  assert.equal(codec.glyph(result.fonts[2], 48, 2).advance, 2);
  assert.doesNotThrow(() => codec.validateGlyphs(result.fonts[2], 2));
});

test('imported-font glyph caches reuse preview bitmaps and enforce the font budget during generation', async () => {
  const input = baseFonts(), glyphCache = new Map(); let calls = 0;
  const options = { rebuildSlots: [2], slotOptions: () => ({ glyphCache }),
    rasterize: async () => { calls++; return { width: 1, height: 1, x: 0, y: 0, advance: 2, pixels: Uint8Array.of(1) }; } };
  const first = await codec.extendFonts(input, [], options), count = calls;
  const second = await codec.extendFonts(input, [], options);
  assert.equal(calls, count); assert.deepEqual(first.fonts, second.fonts);
  await assert.rejects(codec.extendFonts(input, [{ slot: 0, text: '新书' }], { ...options, rebuildSlots: [], maxPayloadBytes: 1 }), /容量/);
});
test('generated offsets fit the fixed device box without clipping pixels or changing advances', async () => {
  const fonts = baseFonts(), bitmap = { ...glyph(), y: -1 }, original = structuredClone(bitmap);
  assert.throws(() => codec.encodeGlyph(fonts[0], 0x65b0, bitmap), /偏移.*边界/);
  const result = await codec.extendFonts(fonts, [{ slot: 0, text: '新' }], { rasterize: async () => bitmap });
  const decoded = codec.glyph(result.fonts[0], 0x65b0, 0);
  assert.equal(decoded.y, -2); assert.equal(decoded.height + decoded.y, 10);
  assert.deepEqual(decoded.pixels, original.pixels); assert.equal(decoded.advance, original.advance);
  assert.deepEqual(bitmap, original); assert.doesNotThrow(() => codec.validateGlyphs(result.fonts[0], 0));
  // Corrupt just the biased Y field of a valid encoded record, retaining
  // its compression stream. Import decoding must also check the boundary.
  const record = codec.encodeGlyph(fonts[0], 0x65b0, { ...bitmap, y: -2 });
  const bit = 24 + fonts[0][4] + fonts[0][5] + fonts[0][6];
  const storedY = (2 ** (fonts[0][7] - 1)) - 1;
  for (let i = 0; i < fonts[0][7]; i++) {
    const index = (bit + i) >> 3, mask = 1 << ((bit + i) & 7);
    record[index] = (record[index] & ~mask) | (((storedY >> i) & 1) ? mask : 0);
  }
  assert.throws(() => codec.decodeGlyph(fonts[0], 0x65b0, record), /偏移.*边界/);
  const legacy = fonts.slice(); legacy[0] = fixture(fonts[0], [...records(result.fonts[0])].map(([code, bytes]) => [code, code === 0x65b0 ? record : bytes]));
  const repaired = codec.normalizeFonts(legacy);
  assert.deepEqual(repaired.changedSlots, [0]); assert.equal(codec.glyph(repaired.fonts[0], 0x65b0, 0).y, -2);
  assert.deepEqual(codec.glyph(repaired.fonts[0], 0x65b0, 0).pixels, original.pixels);
  assert.deepEqual(codec.normalizeFonts(repaired.fonts).changedSlots, []);
});
test('supplementing punctuation widens metric fields while preserving every existing glyph', async () => {
  const fonts = baseFonts(), before = codec.glyphs(fonts[1], 1), original = fonts[1].slice();
  const bitmap = { ...glyph(2, 2), x: 5, y: 9, advance: 2 ** (original[8] - 1) };
  const result = await codec.extendFonts(fonts, [{ slot: 1, text: '"' }], { rasterize: async () => bitmap });
  const after = codec.glyphs(result.fonts[1], 1);
  assert.deepEqual(after.get(34), bitmap);
  for (const [code, shape] of before) assert.deepEqual(after.get(code), shape);
  assert.deepEqual(result.fonts[1].subarray(9, 17), original.subarray(9, 17));
  assert.ok(result.fonts[1][7] > original[7]); assert.ok(result.fonts[1][8] > original[8]);
  assert.deepEqual(fonts[1], original);
  assert.equal(result.fonts[0], fonts[0]); assert.equal(result.fonts[2], fonts[2]);
  assert.doesNotThrow(() => codec.validateGlyphs(result.fonts[1], 1));
  await assert.rejects(codec.extendFonts(fonts, [{ slot: 1, text: '"' }], {
    rasterize: async () => bitmap, maxPayloadBytes: 8 + 3 * 16 + fonts.reduce((sum, font) => sum + font.length, 0) + 1,
  }), /容量/);
  const browser = browserFixture({ '"': bitmap, tofu: glyph(4, 4) });
  assert.deepEqual(await codec.rasterizeGlyph(34, 1, fonts[1], { document: browser.document }), bitmap);
  const merged = codec.mergeFonts(fonts, result.fonts);
  assert.deepEqual(codec.glyph(merged.fonts[1], 34, 1), bitmap);
  for (const [code, shape] of before) assert.deepEqual(codec.glyph(merged.fonts[1], code, 1), shape);
});
function records(font) {
  const result = new Map();
  function walk(offset, unicode) {
    for (;;) {
      const code = unicode ? word(font, offset) : font[offset];
      const size = font[offset + (unicode ? 2 : 1)];
      if (unicode ? !code : !size) return;
      result.set(code, font.slice(offset, offset + size)); offset += size;
    }
  }
  walk(23, false);
  walk(23 + word(font, 17), false);
  walk(23 + word(font, 19), false);
  const table = 23 + word(font, 21);
  if (word(font, table + 2)) walk(table + word(font, table), true);
  return result;
}

// Independently follows the C decoder's bit cursor, biased metrics, run pairs
// and repeat flag. No decoder from font_builder is used to verify encoding.
function decode(header, code, record) {
  let bit = (code <= 255 ? 2 : 3) * 8;
  function read(count) {
    assert.ok(bit + count <= record.length * 8, 'record contains every read');
    let result = 0;
    for (let index = 0; index < count; index++) {
      if (record[Math.floor(bit / 8)] & (1 << (bit % 8))) result += 2 ** index;
      bit++;
    }
    return result;
  }
  const width = read(header[4]), height = read(header[5]);
  const x = read(header[6]) - 2 ** (header[6] - 1);
  const y = read(header[7]) - 2 ** (header[7] - 1);
  const advance = read(header[8]) - 2 ** (header[8] - 1), pixels = [];
  let guard = 10000;
  while (pixels.length < width * height) {
    const zeros = read(header[2]), ones = read(header[3]);
    do {
      assert.ok(--guard > 0 && zeros + ones > 0, 'runs make progress');
      pixels.push(...Array(zeros).fill(0), ...Array(ones).fill(1));
    } while (read(1));
  }
  assert.equal(pixels.length, width * height, 'runs do not overflow the bitmap');
  return { width, height, x, y, advance, pixels: Uint8Array.from(pixels) };
}
const glyph = (width = 12, height = 12, pattern = (x, y) => (x + y) % 2) => ({
  width, height, x: 0, y: -2, advance: width,
  pixels: Uint8Array.from({ length: width * height }, (_, i) => pattern(i % width, Math.floor(i / width))),
});

function fixture(header, glyphRecords) {
  const sorted = Array.from(glyphRecords).sort((a, b) => a[0] - b[0]);
  const ascii = sorted.filter(([code]) => code <= 255), unicode = sorted.filter(([code]) => code > 255);
  const asciiLength = ascii.reduce((n, [, bytes]) => n + bytes.length, 0) + 2;
  const bytes = new Uint8Array(23 + asciiLength + 6 + unicode.reduce((n, [, bytes]) => n + bytes.length, 0));
  bytes.set(header.subarray(0, 23)); bytes[0] = Math.min(254, sorted.length);
  let at = 23, upper = null, lower = null;
  for (const [code, record] of ascii) {
    if (upper === null && code >= 65) upper = at - 23;
    if (lower === null && code >= 97) lower = at - 23;
    bytes.set(record, at); at += record.length;
  }
  [[17, upper ?? at - 23], [19, lower ?? at - 23], [21, asciiLength]].forEach(([offset, value]) => {
    bytes[offset] = value >> 8; bytes[offset + 1] = value & 255;
  });
  at += 2; bytes.set([0, 4, 255, 255], at); at += 4;
  for (const [, record] of unicode) { bytes.set(record, at); at += record.length; }
  return bytes;
}

test('missing requirements deduplicate per slot and reject unsupported text before generation', () => {
  const fonts = baseFonts();
  assert.deepEqual(codec.gatherMissing(fonts, [
    { slot: 0, text: '年月龘龘' }, { slot: 1, text: '龘' }, { slot: 0, text: '龘' },
  ]), [{ slot: 0, codePoint: 0x9f98 }, { slot: 1, codePoint: 0x9f98 }]);
  for (const text of ['\0', '\n', '\u007f', '\u009f', '\u200d', '\ud800', '😀', '\uffff'])
    assert.throws(() => codec.gatherMissing(fonts, [{ slot: 0, text }]));
  assert.throws(() => codec.gatherMissing(fonts, [{ slot: 3, text: 'A' }]), /槽位/);
  assert.throws(() => codec.gatherMissing(fonts, [{ slot: 0, text: null }]), /文本/);
});

test('independent decoder verifies bit order, biased signed metrics, repeated pairs and split runs', () => {
  const fonts = baseFonts();
  for (const [slot, width, height] of [[0, 12, 12], [1, 16, 16], [2, 13, 22]]) {
    const header = fonts[slot].slice(), halfX = 2 ** (header[6] - 1), halfY = 2 ** (header[7] - 1);
    // A deliberately wider fixture box exercises the complete signed bit
    // range without creating glyphs outside the declared geometry.
    header[9] = width + halfX; header[10] = height + 2 * halfY;
    header[11] = 256 - halfX; header[12] = 256 - halfY;
    const patterns = [() => 0, () => 1, (_, y) => y >= height / 2, (x, y) => (y * width + x) % 2, (x, y) => (x * 7 + y * 11) % 13 < 6];
    for (const pattern of patterns) {
      const bitmap = { ...glyph(width, height, pattern), x: -halfX, y: halfY - 1 };
      const record = codec.encodeGlyph(header, 0xe000, bitmap);
      assert.deepEqual(decode(header, 0xe000, record), bitmap);
      assert.deepEqual(codec.decodeGlyph(header, 0xe000, record), bitmap);
    }
    const alternating = codec.encodeGlyph(header, 0xe001, glyph(width, height, (x, y) => (y * width + x) % 2));
    assert.ok(alternating.length < 50, 'repeat flags compress a long alternating pixel stream');
  }
});

test('unsigned and signed bounds, geometry, bitmap values and record capacity are enforced', () => {
  const header = baseFonts()[0].slice(0, 23), bitmap = glyph();
  for (const edit of [g => { g.x = 4; }, g => { g.x = -5; }, g => { g.y = 8; }, g => { g.y = -9; },
    g => { g.advance = 16; }, g => { g.advance = -17; }, g => { g.width = -1; },
    g => { g.width = 13; g.pixels = new Uint8Array(156); }, g => { g.height = 13; g.pixels = new Uint8Array(156); },
    g => { g.pixels = Uint8Array.of(1); }, g => { g.pixels[0] = 2; }, g => { g.width = 0; g.pixels = []; }]) {
    const changed = { ...bitmap, pixels: bitmap.pixels.slice() }; edit(changed);
    assert.throws(() => codec.encodeGlyph(header, 0xe000, changed));
  }
  const narrow = header.slice(); narrow[4] = 3;
  assert.throws(() => codec.encodeGlyph(narrow, 0xe000, glyph(8, 4)), /宽度/);
  const signed = { ...bitmap, x: 3, y: -8, advance: -16 };
  const signedBox = header.slice(); signedBox[9] = 16; signedBox[10] = 32; signedBox[12] = 248;
  assert.deepEqual(decode(signedBox, 0xe000, codec.encodeGlyph(signedBox, 0xe000, signed)), signed);
  const large = header.slice(); large[2] = large[3] = large[4] = large[5] = 8; large[9] = large[10] = 255;
  assert.throws(() => codec.encodeGlyph(large, 0xe000, { ...glyph(255, 255), advance: 12 }), /255 字节/);
});

test('extension preserves existing records, handles Latin-1 and Unicode, and fixes all lookup offsets', async () => {
  const fonts = baseFonts(), snapshots = fonts.map(font => font.slice()), before = fonts.map(records);
  const requests = [{ slot: 0, text: '¡\uE000' }, { slot: 1, text: '!AZaz¡\uE000' }];
  const result = await codec.extendFonts(fonts, requests, { rasterize: async (code, slot) => glyph(slot ? 16 : 12, slot ? 16 : 12) });
  assert.equal(result.added.length, 9);
  assert.deepEqual(result.added, result.missing);
  assert.equal(result.fonts[2], fonts[2]);
  for (let slot = 0; slot < 3; slot++) {
    assert.deepEqual(fonts[slot], snapshots[slot], 'input font bytes are immutable');
    const after = records(result.fonts[slot]);
    for (const [code, bytes] of before[slot]) assert.deepEqual(after.get(code), bytes, `original U+${code.toString(16)} stays byte-identical`);
    assert.deepEqual(result.fonts[slot].subarray(1, 17), fonts[slot].subarray(1, 17));
    const present = builder.validateFont(result.fonts[slot], slot);
    for (const { slot: requested, codePoint } of result.added) if (requested === slot) assert.ok(present.has(codePoint));
    const table = 23 + word(result.fonts[slot], 21);
    assert.deepEqual(Array.from(result.fonts[slot].subarray(table, table + 4)), [0, 4, 255, 255]);
  }
  const font = result.fonts[1];
  assert.equal(font[23 + word(font, 17)], 'A'.charCodeAt(0));
  assert.equal(font[23 + word(font, 19)], 'a'.charCodeAt(0));
  assert.deepEqual(builder.decodeFonts(builder.encodeFonts(result.fonts)), result.fonts);
});

test('more than 255 total glyphs saturate metadata at 254 without losing Unicode glyphs', async () => {
  const fonts = baseFonts();
  const text = Array.from({ length: 300 }, (_, i) => String.fromCodePoint(0xe000 + i)).join('');
  const result = await codec.extendFonts(fonts, [{ slot: 0, text }], { rasterize: async () => glyph() });
  assert.equal(result.fonts[0][0], 254);
  const available = builder.validateFont(result.fonts[0], 0);
  assert.ok(available.size > 255);
  for (const char of text) assert.ok(available.has(char.codePointAt(0)));
  assert.deepEqual(codec.gatherMissing(result.fonts, [{ slot: 0, text }]), []);
});

test('no-op extension needs no canvas and a failed rasterizer never partially mutates the input', async () => {
  const fonts = baseFonts(), snapshots = fonts.map(font => font.slice());
  const noop = await codec.extendFonts(fonts, [{ slot: 0, text: '年月' }]);
  assert.deepEqual(noop.added, []); noop.fonts.forEach((font, i) => assert.equal(font, fonts[i]));
  let calls = 0;
  await assert.rejects(codec.extendFonts(fonts, [{ slot: 0, text: '\uE000\uE001' }], { rasterize: async () => {
    if (++calls === 2) throw new Error('unsupported font'); return glyph();
  } }), /unsupported font/);
  assert.deepEqual(fonts, snapshots);
});

test('merge retains device-only generated glyphs and gives incoming glyphs priority', async () => {
  const incoming = baseFonts();
  const installed = (await codec.extendFonts(incoming, [{ slot: 0, text: '\uE000' }, { slot: 1, text: '\uE001' }], { rasterize: async (_, slot) => glyph(slot ? 16 : 12, 12) })).fonts;
  const merged = codec.mergeFonts(incoming, installed);
  assert.deepEqual(merged.added, [{ slot: 0, codePoint: 0xe000 }, { slot: 1, codePoint: 0xe001 }]);
  for (let slot = 0; slot < 3; slot++) {
    const after = records(merged.fonts[slot]);
    for (const [code, bytes] of records(incoming[slot])) assert.deepEqual(after.get(code), bytes);
    for (const [code, bytes] of records(installed[slot])) assert.deepEqual(after.get(code), bytes);
  }
  const again = codec.mergeFonts(merged.fonts, installed);
  assert.deepEqual(again.added, []); again.fonts.forEach((font, slot) => assert.equal(font, merged.fonts[slot]));
});

test('merge transcodes installed-only glyphs across bit layouts and retains compatible offsets', () => {
  const incoming = baseFonts(), installed = baseFonts();
  const changedHeader = installed[0].slice(0, 23); changedHeader[2] = 4; changedHeader[6] = 4;
  const bitmap = { ...glyph(2, 2), x: 1, y: 0 };
  installed[0] = fixture(changedHeader, [[0xe000, codec.encodeGlyph(changedHeader, 0xe000, bitmap)]]);
  const merged = codec.mergeFonts(incoming, installed), record = records(merged.fonts[0]).get(0xe000);
  assert.deepEqual(decode(incoming[0], 0xe000, record), bitmap);
  assert.notDeepEqual(record, records(installed[0]).get(0xe000));
  const outside = { ...bitmap, x: 7 };
  installed[0] = fixture(changedHeader, [[0xe000, codec.encodeGlyph(changedHeader, 0xe000, outside)]]);
  const widened = codec.mergeFonts(incoming, installed);
  assert.deepEqual(decode(widened.fonts[0], 0xe000, records(widened.fonts[0]).get(0xe000)), outside);
  for (const [code, record] of records(incoming[0]))
    assert.deepEqual(decode(widened.fonts[0], code, records(widened.fonts[0]).get(code)), decode(incoming[0], code, record));
});

test('merging different bit widths also retains control-code records already in source fonts', () => {
  const installed = baseFonts(), incoming = baseFonts(), header = incoming[0].slice(0, 23);
  header[2] = 4;
  incoming[0] = fixture(header, [[65, codec.encodeGlyph(header, 65, glyph())]]);
  const result = codec.mergeFonts(incoming, installed), inherited = records(installed[0]).get(128);
  assert.ok(inherited, 'canonical WQY9 contains an inherited U+0080 record');
  assert.deepEqual(decode(header, 128, records(result.fonts[0]).get(128)), decode(installed[0], 128, inherited));
  assert.throws(() => codec.encodeGlyph(header, 128, glyph()), /控制字符/);
});

test('decoder refuses truncated streams, zero-progress pairs and overflowing repeats', () => {
  const header = baseFonts()[0], record = codec.encodeGlyph(header, 0xe000, glyph());
  const truncated = record.slice(0, 6); truncated[2] = truncated.length;
  assert.throws(() => codec.decodeGlyph(header, 0xe000, truncated));
  const zero = record.slice(); zero.fill(0, 6);
  assert.throws(() => codec.decodeGlyph(header, 0xe000, zero));
  const overflowing = record.slice(); overflowing[overflowing.length - 1] |= 255;
  assert.throws(() => codec.decodeGlyph(header, 0xe000, overflowing));
});

function browserFixture(shapes) {
  const events = [];
  let selected = '', width = 0, height = 0;
  const context = {
    clearRect() {}, fillText(text) { selected = text; events.push('draw:' + text); },
    measureText: text => ({ width: (shapes[text] || shapes.tofu).advance }),
    getImageData() {
      const shape = shapes[selected] || shapes.tofu, size = width, data = new Uint8ClampedArray(width * height * 4);
      const origin = width / 3;
      for (let y = 0; y < shape.height; y++) for (let x = 0; x < shape.width; x++) {
        if (!shape.pixels[y * shape.width + x]) continue;
        data[((origin - shape.y - shape.height + y) * size + origin + shape.x + x) * 4 + 3] = 255;
      }
      return { data };
    },
  };
  const canvas = { getContext: () => context, get width() { return width; }, set width(value) { width = value; }, get height() { return height; }, set height(value) { height = value; } };
  const document = { fonts: { async load(css, text) { events.push('load:' + text); }, ready: Promise.resolve().then(() => events.push('ready')) }, createElement: () => canvas };
  return { document, events, context };
}

test('browser rasterizer awaits fonts, uses compact baseline metrics and detects blank/tofu glyphs', async () => {
  const header = baseFonts()[0], target = { ...glyph(3, 4, (x, y) => (x + y) % 2), x: 1, y: -1, advance: 5 };
  const f = browserFixture({ '龘': target, tofu: glyph(4, 4, () => 1), ' ': { width: 0, height: 0, x: 0, y: 0, advance: 4, pixels: [] } });
  assert.deepEqual(await codec.rasterizeGlyph(0x9f98, 0, header, { document: f.document }), target);
  assert.ok(f.events.indexOf('load:龘') < f.events.indexOf('draw:龘'));
  assert.ok(f.events.indexOf('ready') < f.events.indexOf('draw:龘'));
  assert.match(f.context.font, /^12px /);
  assert.equal((await codec.rasterizeGlyph(32, 0, header, { document: f.document })).width, 0);
  await assert.rejects(codec.rasterizeGlyph(0xe000, 0, header, { document: f.document }), /占位方框/);
  const blank = browserFixture({ '龘': { width: 0, height: 0, x: 0, y: 0, advance: 12, pixels: [] }, tofu: glyph(4, 4) });
  await assert.rejects(codec.rasterizeGlyph(0x9f98, 0, header, { document: blank.document }), /未生成/);
  await assert.rejects(codec.rasterizeGlyph(0x9f98, 0, header, { document: {} }), /Canvas/);
});

test('a generation job caches missing-glyph reference bitmaps and refreshes them for the next job', async () => {
  const fonts = baseFonts(), shapes = { '\uE000': glyph(2, 2), '\uE001': glyph(3, 3), tofu: glyph(4, 4) };
  const browser = browserFixture(shapes), requests = [{ slot: 0, text: '\uE000\uE001' }];
  await codec.extendFonts(fonts, requests, { document: browser.document });
  assert.equal(browser.events.filter(event => event.startsWith('draw:')).length, 8, 'two targets share six reference scans');
  assert.equal(browser.events.filter(event => event.startsWith('load:')).length, 2, 'each target still loads its own font unicode range');
  // A newly available font can change its .notdef shape between jobs.
  shapes.tofu = glyph(2, 2);
  await assert.rejects(codec.extendFonts(fonts, requests, { document: browser.document }), /占位方框/);
});

test('missing-glyph reference caches distinguish font family, style, size and document', async () => {
  const fonts = baseFonts(), bitmap = glyph(2, 2), browser = browserFixture({ '新': bitmap, tofu: glyph(4, 4) });
  const missingGlyphCache = new Map(), options = { document: browser.document, missingGlyphCache, fontFamily: 'TestOne' };
  await codec.rasterizeGlyph('新'.codePointAt(0), 0, fonts[0], options);
  await codec.rasterizeGlyph('新'.codePointAt(0), 0, fonts[0], options);
  assert.equal(browser.events.filter(event => event.startsWith('draw:')).length, 8);
  for (const extra of [{ fontFamily: 'TestTwo' }, { fontSize: 12 }, { fontSize: 12, fontBold: true }, { fontSize: 12, fontItalic: true }])
    await codec.rasterizeGlyph('新'.codePointAt(0), 0, fonts[0], { ...options, ...extra });
  assert.equal(browser.events.filter(event => event.startsWith('draw:')).length, 36);
  const other = browserFixture({ '新': bitmap, tofu: bitmap });
  await assert.rejects(codec.rasterizeGlyph('新'.codePointAt(0), 0, fonts[0], { ...options, document: other.document }), /占位方框/);
  // A CSS font name can resolve to a newly loaded unicode-range face during
  // the same job. Its reference bitmap must not inherit the previous face.
  const shapes = { '新': bitmap, tofu: glyph(4, 4) }, changing = browserFixture(shapes);
  let face = {};
  changing.document.fonts.load = async () => [face];
  await codec.rasterizeGlyph('新'.codePointAt(0), 0, fonts[0], { ...options, document: changing.document });
  face = {}; shapes.tofu = bitmap;
  await assert.rejects(codec.rasterizeGlyph('新'.codePointAt(0), 0, fonts[0], { ...options, document: changing.document }), /占位方框/);
});

test('compatibility checking reports all missing and oversized glyphs without committing partial fonts', async () => {
  const fonts = baseFonts(), snapshots = fonts.map(font => font.slice()), progress = [], seen = [];
  await assert.rejects(codec.extendFonts(fonts, [{ slot: 0, text: '\uE000\uE001\uE002' }], {
    collectIssues: true, onProgress: info => progress.push(info),
    slotOptions: () => ({ validateCharacter(code) { if (code === 0xe000) throw new Error('导入字体不包含 U+E000，请更换字体'); } }),
    rasterize: async code => { seen.push(code); return code === 0xe001 ? glyph(13, 12) : glyph(2, 2); },
  }), error => {
    assert.match(error.message, /2 个字符问题/);
    assert.deepEqual(error.fontIssues.map(issue => [issue.slot, issue.codePoint]), [[0, 0xe000], [0, 0xe001]]);
    assert.match(error.fontIssues[0].message, /不包含/); assert.match(error.fontIssues[1].message, /13×12.*12×12/);
    return true;
  });
  assert.deepEqual(seen, [0xe001, 0xe002]); assert.equal(progress.at(-1).completed, 3); assert.equal(progress.at(-1).issues, 2);
  assert.deepEqual(fonts, snapshots);
  let calls = 0;
  await assert.rejects(codec.extendFonts(fonts, [{ slot: 0, text: '\uE000\uE001' }], {
    collectIssues: true, rasterize: async () => { calls++; throw new Error('Canvas unavailable'); },
  }), /Canvas unavailable/);
  assert.equal(calls, 1, 'environment failures abort immediately');
  await assert.rejects(codec.extendFonts(fonts, [{ slot: 0, text: '\uE000\uE001' }], {
    collectIssues: true, maxPayloadBytes: 1, rasterize: async () => glyph(2, 2),
  }), /容量/);
  const signal = { aborted: false };
  await assert.rejects(codec.extendFonts(fonts, [{ slot: 0, text: '\uE000\uE001' }], {
    collectIssues: true, signal, batchSize: 1, rasterize: async () => glyph(13, 12), yield: async () => { signal.aborted = true; },
  }), error => error.name === 'AbortError');
});

test('font recovery discards only damaged glyph records and retains healthy custom glyphs', () => {
  const fallback = baseFonts(), installed = baseFonts(), header = installed[0], shape = glyph(3, 3);
  const broken = codec.encodeGlyph(header, 0xe000, shape), good = codec.encodeGlyph(header, 0xe001, shape);
  const metricsEnd = 24 + [...header.subarray(4, 9)].reduce((sum, bits) => sum + bits, 0);
  for (let bit = metricsEnd; bit < broken.length * 8; bit++) broken[bit >> 3] &= ~(1 << (bit & 7));
  installed[0] = fixture(header, [...records(header), [0xe000, broken], [0xe001, good]]);
  installed[3] = fixture(header, [[0xe000, broken], [0xe001, good]]); installed.customKey = 123;
  const snapshots = installed.map(font => font.slice()), result = codec.recoverFonts(installed, fallback);
  assert.deepEqual(result.damaged.map(issue => [issue.slot, issue.codePoint]), [[0, 0xe000], [3, 0xe000]]);
  assert.equal(result.fonts.customKey, 123); assert.ok(result.recovered > 2);
  for (const slot of [0, 3]) {
    assert.equal(codec.glyph(result.fonts[slot], 0xe000, slot), null);
    assert.deepEqual(codec.glyph(result.fonts[slot], 0xe001, slot), shape);
    assert.doesNotThrow(() => codec.validateGlyphs(result.fonts[slot], slot));
  }
  for (const [code, value] of codec.glyphs(header, 0)) assert.deepEqual(codec.glyph(result.fonts[0], code, 0), value);
  installed.forEach((font, slot) => assert.deepEqual(font, snapshots[slot]));
  installed[0] = fixture(header, [[0xe000, broken]]); installed[3] = installed[0];
  const empty = codec.recoverFonts(installed, fallback);
  assert.equal(empty.fonts[0], fallback[0]); assert.equal(empty.fonts.length, 3); assert.equal(empty.fonts.customKey, undefined);
  installed[1] = installed[1].slice(); installed[1][2] = 0;
  const structural = codec.recoverFonts(installed, fallback);
  assert.equal(structural.fonts[1], fallback[1]); assert.ok(structural.damaged.some(issue => issue.slot === 1 && issue.codePoint === undefined));
});

test('large TODO headers encode full glyph bounds with negative italic bearings without touching base fonts', async () => {
  const original = baseFonts(), snapshots = original.map(font => font.slice());
  for (const size of [9, 12, 16, 20, 24, 32]) {
    const px = Math.ceil(size * 4 / 3), desc = Math.ceil(px / 3), overhang = Math.ceil(px / 4);
    const bitmap = { ...glyph(px + overhang, px + desc, (x, y) => x === y || x === px + overhang - 1 - y || y % 8 === 0),
      x: -overhang, y: -desc, advance: px + 1 };
    const request = { slot: 3, sourceSlot: 1, fontFamily: '', fontSize: size, fontBold: true, fontItalic: true, text: '年' };
    const result = await codec.extendFonts(original, [request], { rasterize: async (code, slot, header, options) => {
      assert.equal(slot, 1); assert.equal(options.fontFamily, ''); assert.equal(options.fontSize, size);
      assert.equal(options.fontBold, true); assert.equal(options.fontItalic, true);
      assert.deepEqual(Array.from(header.subarray(4, 9)), [7, 7, 8, 8, 8]); return bitmap;
    } });
    assert.equal(result.fonts.customKey, builder.todoFontKey('', size, true, true));
    const font = result.fonts[3], record = records(font).get('年'.codePointAt(0));
    assert.deepEqual(decode(font, '年'.codePointAt(0), record), bitmap);
    assert.deepEqual(builder.decodeFonts(builder.encodeFonts(result.fonts)), result.fonts);
    original.forEach((font, slot) => { assert.equal(result.fonts[slot], font); assert.deepEqual(font, snapshots[slot]); });
  }
});

test('browser TODO glyphs rasterize actual bold and italic at point-derived pixels and retain negative bearings', async () => {
  const fonts = (await codec.extendFonts(baseFonts(), [{ slot: 3, sourceSlot: 1, fontFamily: '', fontSize: 32,
    fontBold: true, fontItalic: true, text: '年' }], { rasterize: async () => glyph(1, 1, () => 1) })).fonts;
  const target = { ...glyph(3, 4, () => 1), x: -2, y: -1, advance: 30 };
  const f = browserFixture({ '年': target, tofu: glyph(4, 4, (x, y) => x === y) });
  const result = await codec.rasterizeGlyph('年'.codePointAt(0), 1, fonts[3], {
    document: f.document, fontFamily: 'SimSun', fontSize: 32, fontBold: true, fontItalic: true,
  });
  assert.deepEqual(result, target); assert.equal(f.context.font, 'italic bold 43px SimSun');
  const huge = browserFixture({ '年': { ...glyph(55, 44, () => 1), advance: 43 }, tofu: glyph(4, 4) });
  await assert.rejects(codec.rasterizeGlyph('年'.codePointAt(0), 1, fonts[3], {
    document: huge.document, fontSize: 32, fontBold: true, fontItalic: true,
  }), /超出字体槽位/);
});

test('browser UMD exposes the same API without Node dependencies', () => {
  const context = { EPDResourceBuilder: builder, Uint8Array };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/font_builder.js'), 'utf8'), context);
  assert.deepEqual(Object.keys(context.EPDWebFonts), Object.keys(codec));
  assert.equal(context.EPDWebFonts.gatherMissing(baseFonts(), [{ slot: 0, text: '龘' }])[0].codePoint, 0x9f98);
});
