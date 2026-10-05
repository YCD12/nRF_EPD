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
    const header = fonts[slot], halfX = 2 ** (header[6] - 1), halfY = 2 ** (header[7] - 1);
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
  assert.deepEqual(decode(header, 0xe000, codec.encodeGlyph(header, 0xe000, signed)), signed);
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

test('merge transcodes installed-only glyphs across bit layouts and rejects incompatible geometry', () => {
  const incoming = baseFonts(), installed = baseFonts();
  const changedHeader = installed[0].slice(0, 23); changedHeader[2] = 4; changedHeader[6] = 4;
  const bitmap = { ...glyph(), x: -3 };
  installed[0] = fixture(changedHeader, [[0xe000, codec.encodeGlyph(changedHeader, 0xe000, bitmap)]]);
  const merged = codec.mergeFonts(incoming, installed), record = records(merged.fonts[0]).get(0xe000);
  assert.deepEqual(decode(incoming[0], 0xe000, record), bitmap);
  assert.notDeepEqual(record, records(installed[0]).get(0xe000));
  const outside = { ...bitmap, x: 7 };
  installed[0] = fixture(changedHeader, [[0xe000, codec.encodeGlyph(changedHeader, 0xe000, outside)]]);
  assert.throws(() => codec.mergeFonts(incoming, installed), /X 偏移/);
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

test('browser UMD exposes the same API without Node dependencies', () => {
  const context = { EPDResourceBuilder: builder, Uint8Array };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/font_builder.js'), 'utf8'), context);
  assert.deepEqual(Object.keys(context.EPDWebFonts), Object.keys(codec));
  assert.equal(context.EPDWebFonts.gatherMissing(baseFonts(), [{ slot: 0, text: '龘' }])[0].codePoint, 0x9f98);
});
