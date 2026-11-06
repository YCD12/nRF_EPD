const { test } = require('node:test');
const assert = require('node:assert/strict');
const store = require('../html/js/resource_store.js');
const builder = require('../html/js/resource_builder.js');
const packer = require('../html/js/ui_tools/resource_pack.js');
const fontTool = require('../html/js/ui_tools/font_store_tool.js');
const template = require('../html/resources/default-project.json');
const fs = require('node:fs'), vm = require('node:vm');
const bundle = () => store.prepareBundle(template.files.map(f => ({ name: f.name, bytes: new Uint8Array(Buffer.from(f.base64, 'base64')) })));
const fontResponse = url => ({ ok: true, arrayBuffer: async () => {
  assert.match(url, /^resources\/fonts\/u8g2_font_[a-zA-Z0-9_]+\.bin$/);
  return new Uint8Array(fs.readFileSync(require('node:path').join(__dirname, '../html', url))).buffer;
} });

test('browser CAL1/UIR1/FPK1 encoders reproduce the canonical Python payloads byte for byte', () => {
  const b = bundle(), model = builder.open(b);
  for (const [id, payload] of [[5, builder.encodeCalendar(model.calendar)], [6, builder.encodeUI(model.ui)], [7, builder.encodeFonts(model.fonts)]])
    assert.deepEqual(payload, b.packages.find(p => p.pack.id === id).pack.payload);
  const unchanged = builder.build(b, model, 12345);
  unchanged.packages.forEach((p, i) => assert.deepEqual(p.bytes, b.packages[i].bytes));
});

test('edits generate readable packages and bump only the changed modules', () => {
  const b = bundle(), model = builder.open(b);
  model.calendar.holidays = [{ month: 10, day: 1, work: false }, { month: 10, day: 4, work: true }];
  model.calendar.solar_festivals[0].name = '新年';
  model.ui.components[0].x = 12;
  const next = builder.build(b, model, 12345), decoded = builder.open(next);
  assert.deepEqual(decoded.calendar, model.calendar);
  assert.deepEqual(decoded.ui, model.ui);
  next.packages.forEach(p => {
    const old = b.packages.find(q => q.pack.id === p.pack.id);
    assert.equal(p.pack.revision, old.pack.revision + ([5, 6].includes(p.pack.id) ? 1 : 0));
    if ([5, 6].includes(p.pack.id)) assert.equal(p.pack.builtAt, 12345);
    else assert.deepEqual(p.bytes, old.bytes);
    assert.doesNotThrow(() => store.parsePackage(p.bytes));
  });
});

test('rejects UTF-8 overflow, wrong table years, duplicate dates and incompatible clock geometry', () => {
  const edits = [
    m => { m.calendar.weekday_names[0] = '星期日'; },
    m => { m.calendar.month_names[0] = '月\0'; },
    m => { m.calendar.year_max++; },
    m => { m.calendar.lunar_month_days[0]++; },
    m => { m.calendar.holidays.push({ ...m.calendar.holidays[0] }); },
    m => { m.calendar.holidays[0].work = 1; },
    m => { m.ui.components[11].x = 1; },
    m => { m.ui.components[16].value0 = 9; },
    m => { m.ui.components[0].fonts[1] = 255; },
    m => { m.ui.components[0].flags = 16; },
    m => { m.ui.components[18].style.baseline = 999; },
    m => { m.ui.components[0].width = 801; },
  ];
  for (const edit of edits) {
    const b = bundle(), model = builder.open(b); edit(model);
    assert.throws(() => builder.build(b, model, 12345));
  }
});

test('font replacement checks metrics, bounded records, CRC and overlapping directories', () => {
  const { fonts } = builder.open(bundle());
  assert.ok(builder.validateFont(fonts[0], 0).has('年'.codePointAt(0)));
  for (const edit of [b => { b[9]=65; }, b => { b[2] = 0; }, b => { b[24] = 1; }, b => { b[21] = 255; b[22] = 255; }]) {
    const corrupt = fonts[0].slice(); edit(corrupt);
    assert.throws(() => builder.validateFont(corrupt, 0));
  }
  const packed = builder.encodeFonts(fonts);
  packed[packed.length - 1] ^= 1;
  assert.throws(() => builder.decodeFonts(packed), /CRC/);
  const overlap = builder.encodeFonts(fonts);
  new DataView(overlap.buffer).setUint32(24, 56, true);
  assert.throws(() => builder.decodeFonts(overlap), /重叠/);
});

test('font coverage rejects glyphs unreachable through the firmware-selected ASCII entry', () => {
  const { fonts } = builder.open(bundle()), original = fonts[0];
  const readWord = (font, at) => font[at] * 256 + font[at + 1];
  function firmwareHas(font, code) {
    let at = 23 + (code >= 97 ? readWord(font, 19) : code >= 65 ? readWord(font, 17) : 0);
    while (font[at + 1]) { if (font[at] === code) return true; at += font[at + 1]; }
    return false;
  }
  // Both an empty entry and a later valid record hide existing glyphs.
  let terminator = 23;
  while (original[terminator + 1]) terminator += original[terminator + 1];
  for (const entry of [17, 19]) for (const target of [terminator, 23 + readWord(original, entry) + original[23 + readWord(original, entry) + 1]]) {
    const corrupt = original.slice(), offset = target - 23;
    corrupt[entry] = offset >> 8; corrupt[entry + 1] = offset & 255;
    assert.equal(firmwareHas(corrupt, entry === 17 ? 65 : 97), false);
    assert.throws(() => builder.validateFont(corrupt, 0), /ASCII 入口.*设备端/);
    assert.throws(() => fontTool.checkSlot(0, corrupt), /ASCII 入口/);
    const changed = fonts.slice(); changed[0] = corrupt;
    assert.throws(() => builder.encodeFonts(changed), /ASCII 入口/);
  }
  // Earlier entry points are valid: the C decoder scans until the terminator,
  // so compatibility must not require a particular canonical offset.
  const legacy = original.slice(); legacy.fill(0, 17, 21);
  const present = builder.validateFont(legacy, 0);
  for (const code of present) if (code <= 255) assert.equal(firmwareHas(legacy, code), true);
  assert.doesNotThrow(() => require('../html/js/font_builder.js').validateGlyphs(legacy, 0));
});

test('Unicode records cannot claim ASCII characters that the device only searches in ASCII tables', () => {
  const font = builder.open(bundle()).fonts[0].slice(), v = new DataView(font.buffer);
  const table = 23 + v.getUint16(21), record = table + v.getUint16(table);
  v.setUint16(record, 65);
  assert.throws(() => builder.validateFont(font, 0), /Unicode.*ASCII/);
});

test('font records reject duplicate code points while allowing shared ASCII entry walks', () => {
  const original = builder.open(bundle()).fonts[0];
  const ascii = original.slice();
  ascii[23 + ascii[24]] = ascii[23];
  assert.throws(() => builder.validateFont(ascii, 0), /重复编码/);
  const unicode = original.slice(), v = new DataView(unicode.buffer);
  const table = 23 + v.getUint16(21), first = table + v.getUint16(table);
  v.setUint16(first + unicode[first + 2], v.getUint16(first));
  assert.throws(() => builder.validateFont(unicode, 0), /重复编码/);
  const shared = original.slice(); shared.fill(0, 17, 21);
  assert.deepEqual(builder.validateFont(shared, 0), builder.validateFont(original, 0));
});

test('TODO geometry accepts six point sizes only in the custom slot and keys distinguish every style', () => {
  const expected = [
    [9, [15,16,-3,-4,12,-4,12,-4]], [12, [20,22,-4,-6,16,-6,16,-6]],
    [16, [28,30,-6,-8,22,-8,22,-8]], [20, [34,36,-7,-9,27,-9,27,-9]],
    [24, [40,43,-8,-11,32,-11,32,-11]], [32, [54,58,-11,-15,43,-15,43,-15]],
  ];
  const fonts = builder.open(bundle()).fonts, keys = new Set();
  for (const [size, geometry] of expected) {
    assert.deepEqual(builder.todoFontGeometry(size), geometry);
    const custom = fonts[0].slice(); custom.set(geometry.map(n => n & 255), 9);
    assert.doesNotThrow(() => builder.validateFont(custom, 3));
    for (let slot = 0; slot < 3; ++slot) assert.doesNotThrow(() => builder.validateFont(custom, slot));
    for (const bold of [false, true]) for (const italic of [false, true]) {
      const key = builder.todoFontKey(' SimSun ', size, bold, italic);
      assert.equal(key, store.crc32(new TextEncoder().encode(`todo-v3:${size}:${+bold}:${+italic}:SimSun`)) || 1);
      assert.equal(key, builder.todoFontKey('SimSun', size, bold, italic)); keys.add(key);
    }
  }
  assert.equal(keys.size, 24);
  assert.notEqual(builder.todoFontKey('', 12), builder.todoFontKey('SimSun', 12));
  for (const size of [0, 8, 10, 36, '32']) assert.throws(() => builder.todoFontGeometry(size));
  for (const args of [[null, 12], ['', 12, 1], ['', 12, false, 'italic']])
    assert.throws(() => builder.todoFontKey(...args));
});

test('partition capacity and revision overflow are checked before download or upload', () => {
  const b = bundle(), model = builder.open(b);
  model.ui.components[0].x++;
  b.packages.find(p => p.pack.id === 6).pack.revision = 0xffffffff;
  assert.throws(() => builder.build(b, model), /版本/);
  const other = bundle(), changed = builder.open(other);
  changed.calendar.year_max = 4095;
  const years = 4095 - changed.calendar.year_min + 1;
  changed.calendar.lunar_month_days = [changed.calendar.year_min - 1, ...Array(years + 1).fill(0)];
  changed.calendar.solar_new_year = changed.calendar.lunar_month_days.slice();
  changed.calendar.jieqi_year_bits = Array((years + 1) * 3).fill(0);
  assert.throws(() => builder.build(other, changed), /容量/);
});

function editorFixture(options = {}) {
  const nodes = new Map(), drawings = [], listeners = new Map(), requests = [];
  const node = id => {
    if (!nodes.has(id)) nodes.set(id, { value: id === 'resourcePreviewMode' ? '1' : '', files: [],
      addEventListener(type, callback) { listeners.set(`${id}:${type}`, callback); }, classList: { toggle() {} },
      width: 800, height: 160,
      getContext: () => ({ clearRect() {}, fillRect() {}, strokeRect: (...args) => drawings.push(args), fillText() {},
        beginPath() {}, moveTo() {}, lineTo() {}, stroke() {},
        createImageData(w,h) {return {data:new Uint8ClampedArray(w*h*4)};},putImageData(image) {drawings.push(image);} }) });
    return nodes.get(id);
  };
  let current = null, busy = false, saved = null, version = 0;
  const installer = {
    busy: () => busy, getBundle: () => current, selectionVersion: () => version,
    controls() { context.ExternalResourceEditor?.controls(); },
    async loadLatestOutput() { if (options.latest) await options.latest(installer); },
    async loadBundle(entries) { current = store.prepareBundle(entries); ++version; context.ExternalResourceEditor.load(current); },
  };
  const context = { EPDUIModel: require('../html/js/ui_tools/ui_model.js'), EPDUIRenderer: require('../html/js/ui_tools/ui_preview.js'), EPDResourceBuilder: builder, EPDResourceStore: store, EPDResourcePack: packer,
    EPDFontStoreTool: fontTool, ExternalResourceInstaller: installer,
    EPDWebFonts: { ...require('../html/js/font_builder.js'), extendFonts: options.extendFonts || ((fonts, requests, settings) => require('../html/js/font_builder.js').extendFonts(fonts, requests,
      { ...settings, rasterize: async () => ({ width: 2, height: 2, x: 0, y: 0, advance: 3, pixels: Uint8Array.of(1,0,0,1) }) })) },
    EPDResourceDraft: options.draft ? { create: () => options.draft } : undefined,
    EPDResourceProject: require('../html/js/ui_tools/resource_project.js'),
    EPDFontSources: options.fontSources,
    EPDResourceArchive: require('../html/js/resource_archive.js'), DeviceOperations: { current: null },
    Uint8Array, TextEncoder, Blob, setTimeout: fn => fn(), btoa: b => Buffer.from(b, 'binary').toString('base64'),
    atob: b => Buffer.from(b, 'base64').toString('binary'),
    fetch: async (...args) => { requests.push(args); return options.fetch ? options.fetch(...args) : fontResponse(args[0]); },
    document: { getElementById: node, querySelectorAll: () => [...nodes.values()], createElement: () => ({ click() {} }) },
    URL: { createObjectURL: blob => { saved = blob; return 'blob:test'; }, revokeObjectURL() {} } };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_editor.js'), 'utf8'), context);
  return { editor: context.ExternalResourceEditor, context, node, installer, drawings, requests,
    busy: value => { busy = value; installer.controls(); }, saved: () => saved,
    input: id => listeners.get(`${id}:input`)(),
    import: (id, data) => listeners.get(`${id}:change`)({ target: { files: [{ name: 'data.json', size: 100, text: async () => JSON.stringify(data) }] } }) };
}

test('editor loads, previews, generates, exports a complete reusable project and respects the operation lock', async () => {
  const f = editorFixture();
  f.editor.load(bundle());
  assert.equal(f.drawings.length, 1);
  assert.equal(f.drawings[0].data.length,800*480*4);
  const calendar = JSON.parse(f.node('resourceCalendarSource').value);
  calendar.solar_festivals[0].name = '新年';
  f.node('resourceCalendarSource').value = JSON.stringify(calendar);
  const result = await f.editor.generate();
  assert.equal(result.packages.length, 7);
  assert.deepEqual(result.packages.find(item => item.pack.id === 9).pack.payload,
    bundle().packages.find(item => item.pack.id === 9).pack.payload);
  assert.ok(result.packages.every(item => item.pack.builtAt > 0));
  assert.equal(builder.open(result).calendar.solar_festivals[0].name, '新年');
  assert.equal(builder.validateFont(builder.open(result).fonts[0], 0).has('新'.codePointAt(0)), true);
  assert.equal(builder.validateFont(builder.open(result).fonts[1], 1).has('新'.codePointAt(0)), true);
  await f.editor.exportProject();
  const project = JSON.parse(await f.saved().text());
  assert.equal(project.format, 'EPD-RESOURCE-PROJECT-2');
  const imported = store.prepareBundle(project.files.map(file => ({ name: file.name, bytes: new Uint8Array(Buffer.from(file.base64, 'base64')) })));
  assert.deepEqual(builder.open(imported).calendar, calendar);
  f.busy(true);
  assert.equal(await f.editor.generate(), null);
  assert.match(f.node('resourceEditorStatus').textContent, /等待/);
});

test('invalid editor input never replaces the prepared upload bundle', async () => {
  const f = editorFixture();
  await f.installer.loadBundle(bundle().packages);
  const before = f.installer.getBundle();
  f.node('resourceCalendarSource').value = '{broken';
  assert.equal(await f.editor.generate(), null);
  assert.equal(f.installer.getBundle(), before);
});

test('full generation stamps all seven files while preserving unchanged payloads and module versions', () => {
  const original = bundle(), next = builder.build(original, builder.open(original), 12345, { refreshStamp: true });
  assert.deepEqual(next.packages.map(item => item.pack.id), [1, 5, 6, 7, 2, 3, 9]);
  for (const item of next.packages) {
    const old = original.packages.find(pack => pack.pack.id === item.pack.id);
    assert.deepEqual(item.pack.payload, old.pack.payload);
    assert.equal(item.pack.revision, old.pack.revision);
    assert.equal(item.pack.builtAt, 12345);
  }
  const versioned = builder.build(original, builder.open(original), 23456, { revision: 17 });
  assert.ok(versioned.packages.every(item => item.pack.revision === 17 && item.pack.builtAt === 23456));
  for (const revision of [0, -1, 1.5, 0x100000000, '2'])
    assert.throws(() => builder.build(original, builder.open(original), 12345, { revision }), /版本/);
});

function migrationFixture() {
  const config = bundle().packages.find(item => item.pack.id === 2).pack.payload.slice(16);
  config[13] = 1;
  const annotations = new Uint8Array(32), view = new DataView(annotations.buffer);
  view.setUint32(0, 0x4f4e4e41, true); annotations[4] = 1; annotations[5] = 16;
  view.setUint16(6, 1, true); view.setUint16(16, 2026, true);
  annotations.set([10, 1, 1, 3, ...new TextEncoder().encode('新')], 18);
  return { config, annotations, json: { format: 'EPD-MIG1',
    epd_config_hex: Buffer.from(config).toString('hex'), calendar_annotations_hex: Buffer.from(annotations).toString('hex') } };
}

test('migration backup generates REC1 envelopes with the original device bytes and their CRC', () => {
  const original = bundle(), migration = migrationFixture();
  const result = builder.build(original, builder.open(original), 12345, { migration: migration.json });
  assert.equal(builder.decodeMigration(migration.json).text, '新');
  for (const [id, magic, data] of [[2, 0x31435045, migration.config], [3, 0x31434e41, migration.annotations]]) {
    const pack = result.packages.find(item => item.pack.id === id).pack, view = new DataView(pack.payload.buffer, pack.payload.byteOffset);
    assert.equal(view.getUint32(0, true), magic);
    assert.equal(view.getUint16(4, true), 1); assert.equal(view.getUint16(6, true), 16);
    assert.equal(view.getUint16(8, true), data.length); assert.equal(view.getUint16(10, true), 0);
    assert.equal(view.getUint32(12, true), store.crc32(data));
    assert.deepEqual(pack.payload.subarray(16), data);
    assert.equal(pack.revision, 2);
  }
  assert.deepEqual(result.packages.find(item => item.pack.id === 9).bytes, original.packages.find(item => item.pack.id === 9).bytes);
});

test('bad migration backups are rejected before generating records', () => {
  const migration = migrationFixture();
  for (const data of [null, {}, { ...migration.json, format: 'other' },
    { ...migration.json, epd_config_hex: 'a' }, { ...migration.json, epd_config_hex: '00' },
    { ...migration.json, calendar_annotations_hex: 'xx' }, { ...migration.json, calendar_annotations_hex: '00' }])
    assert.throws(() => builder.decodeMigration(data));
  for (const edit of [bytes => { bytes[0] = 0; }, bytes => { bytes[6] = 65; }, bytes => { bytes[8] = 1; },
    bytes => { bytes[18] = 0; }, bytes => { bytes[20] = 0; }, bytes => { bytes[21] = 10; }, bytes => { bytes[31] = 1; }]) {
    const bytes = migration.annotations.slice(); edit(bytes);
    assert.throws(() => builder.decodeMigration({ ...migration.json, calendar_annotations_hex: Buffer.from(bytes).toString('hex') }));
  }
});

test('calendar-data imports packed CLI holidays and requires matching calculation and holiday tables', () => {
  const calendar = builder.open(bundle()).calendar;
  const next = builder.importCalendarData(calendar, { holidays: [0x0101, 0x120e] });
  assert.deepEqual(next.holidays, [{ month: 1, day: 1, work: false }, { month: 2, day: 14, work: true }]);
  assert.deepEqual(next.month_names, calendar.month_names);
  assert.equal(calendar.holidays.length, 39);
  assert.throws(() => builder.importCalendarData(calendar, { year_max: 2028 }), /三个/);
  assert.throws(() => builder.importCalendarData(calendar, { holiday_year: 2027 }), /holidays/);
  assert.throws(() => builder.importCalendarData(calendar, { holidays: [0x2001] }));
  assert.throws(() => builder.importCalendarData(calendar, { holidays: [0x0101, 0x0101] }), /重复/);
});

test('opening resources generates and auto-loads seven default files once and keeps later edits', async () => {
  const f = editorFixture();
  await Promise.all([f.editor.ensureReady(), f.editor.ensureReady()]);
  const prepared = f.installer.getBundle();
  assert.equal(prepared.packages.length, 7); assert.equal(f.requests.length, 3);
  assert.ok(prepared.packages.every(item => item.pack.builtAt > 0));
  assert.match(f.node('resourceEditorStatus').textContent, /自动载入 7/);
  for (const item of prepared.packages) assert.deepEqual(item.pack.payload,
    bundle().packages.find(old => old.pack.id === item.pack.id).pack.payload);
  f.node('resourceCalendarSource').value = '{draft';
  await f.editor.ensureReady();
  assert.equal(f.installer.getBundle(), prepared);
  assert.equal(f.node('resourceCalendarSource').value, '{draft');
  assert.equal(f.requests.length, 3);
});

test('automatic preparation only reads raw fonts and can retry a failed fetch', async () => {
  const local = editorFixture({ latest: () => assert.fail('automatic generation must not load Python output') });
  await local.editor.ensureReady();
  assert.equal(local.requests.length, 3); assert.equal(local.installer.getBundle().packages.length, 7);
  let ok = false;
  const f = editorFixture({ fetch: async url => ok ? fontResponse(url) : { ok: false } });
  await f.editor.ensureReady();
  assert.equal(f.installer.getBundle(), null);
  assert.match(f.node('resourceEditorStatus').textContent, /基础字模/);
  ok = true; await f.editor.ensureReady();
  assert.equal(f.installer.getBundle().packages.length, 7);
});

test('calendar and migration file imports regenerate and auto-load a custom version with required glyphs', async () => {
  const f = editorFixture(); await f.editor.ensureReady();
  await f.import('resourceCalendarImport', { holidays: [0x0101, 0x120e] });
  await f.import('resourceMigrationImport', migrationFixture().json);
  f.node('resourceBuildRevision').value = '9';
  const result = await f.editor.generate();
  assert.ok(result);
  assert.deepEqual(f.installer.getBundle().packages.map(item => item.bytes), result.packages.map(item => item.bytes));
  assert.ok(result.packages.every(item => item.pack.revision === 9));
  assert.deepEqual(builder.open(result).calendar.holidays, [{ month: 1, day: 1, work: false }, { month: 2, day: 14, work: true }]);
  assert.deepEqual(result.packages.find(item => item.pack.id === 3).pack.payload.subarray(16), migrationFixture().annotations);
  assert.ok(builder.validateFont(builder.open(result).fonts[0], 0).has('新'.codePointAt(0)));
  const before = f.installer.getBundle();
  f.node('resourceMigrationSource').value = '{}';
  assert.equal(await f.editor.generate(), null); assert.equal(f.installer.getBundle(), before);
});

function unzipStored(bytes) {
  const files = [], view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let at = 0; view.getUint32(at, true) === 0x04034b50;) {
    const size = view.getUint32(at + 18, true), nameLength = view.getUint16(at + 26, true), extraLength = view.getUint16(at + 28, true);
    const name = new TextDecoder().decode(bytes.subarray(at + 30, at + 30 + nameLength)), start = at + 30 + nameLength + extraLength;
    const data = bytes.slice(start, start + size);
    assert.equal(store.crc32(data), view.getUint32(at + 14, true));
    files.push({ name, bytes: data }); at = start + size;
  }
  return files;
}

test('download all exports a reusable ZIP containing seven verified ERS files and edited source data', async () => {
  const f = editorFixture(); await f.editor.ensureReady();
  const calendar = JSON.parse(f.node('resourceCalendarSource').value); calendar.solar_festivals[0].name = '新年';
  f.node('resourceCalendarSource').value = JSON.stringify(calendar);
  await f.editor.downloadAll();
  assert.equal(f.saved().type, 'application/zip');
  const entries = unzipStored(new Uint8Array(await f.saved().arrayBuffer()));
  const files = entries.filter(item => item.name.endsWith('.ers')), saved = store.prepareBundle(files);
  assert.equal(files.length, 7);
  for (const file of files) assert.deepEqual(file.bytes, f.installer.getBundle().packages.find(item => item.name === file.name).bytes);
  assert.equal(builder.open(saved).calendar.solar_festivals[0].name, '新年');
  const manifest = JSON.parse(new TextDecoder().decode(entries.find(item => item.name === 'manifest.json').bytes));
  assert.equal(manifest.resources.length, 7);
  for (const [name, format] of [['fonts.json', 'FPK1'], ['calendar.json', 'CAL1'], ['ui.json', 'UIR2'],
    ['records.json', 'REC1'], ['todo.json', 'TODO']])
    assert.equal(JSON.parse(new TextDecoder().decode(entries.find(item => item.name === name).bytes)).format, format);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(entries.find(item => item.name === 'calendar-data.json').bytes)), calendar);
  assert.deepEqual(entries.find(item => item.name === 'partition-table.payload.bin').bytes, saved.packages[0].pack.payload);
});

function deferred() {
  let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve };
}

test('generation serializes work, disables editor inputs and refuses results after a device operation starts', async () => {
  const entered = deferred(), release = deferred();
  const f = editorFixture({ extendFonts: async fonts => { entered.resolve(); await release.promise; return { fonts, added: [] }; } });
  await f.editor.ensureReady(); const before = f.installer.getBundle();
  const generating = f.editor.generate(); await entered.promise;
  assert.equal(f.editor.busy(), true); assert.equal(f.node('resourceCalendarSource').disabled, true);
  assert.equal(await f.editor.generate(), null);
  f.context.DeviceOperations.current = {};
  release.resolve(); assert.equal(await generating, null);
  assert.equal(f.installer.getBundle(), before); assert.equal(f.editor.busy(), false);
  assert.match(f.node('resourceEditorStatus').textContent, /已变化/);
  f.context.DeviceOperations.current = null; f.editor.controls();
  assert.equal(f.node('resourceCalendarSource').disabled, false);
});

test('a late default fetch cannot replace a newer manually loaded project', async () => {
  const entered = deferred(), release = deferred();
  const f = editorFixture({ fetch: async url => { entered.resolve(); await release.promise; return fontResponse(url); } });
  const loading = f.editor.loadDefault(); await entered.promise;
  await f.installer.loadBundle(bundle().packages); const manual = f.installer.getBundle();
  release.resolve(); assert.equal(await loading, null);
  assert.equal(f.installer.getBundle(), manual);
  assert.match(f.node('resourceEditorStatus').textContent, /已变化/);
});

test('automatic preparation resumes after an in-progress device operation releases its lock', async () => {
  const f = editorFixture(); f.context.DeviceOperations.current = {};
  await f.editor.ensureReady();
  assert.equal(f.installer.getBundle(), null); assert.equal(f.requests.length, 0);
  f.context.DeviceOperations.current = null; f.installer.controls();
  await Promise.resolve(); await f.editor.ensureReady();
  assert.equal(f.installer.getBundle().packages.length, 7);
  assert.equal(f.requests.length, 3);
});

test('regenerating untouched defaults preserves every payload and revision without adding placeholder spaces', async () => {
  const f = editorFixture(); await f.editor.ensureReady(); const before = f.installer.getBundle();
  assert.equal(f.editor.isDirty(), false);
  const next = await f.editor.generate();
  for (const item of next.packages) {
    const old = before.packages.find(pack => pack.pack.id === item.pack.id);
    assert.equal(item.pack.revision, old.pack.revision); assert.deepEqual(item.pack.payload, old.pack.payload);
  }
  f.node('resourceBuildRevision').value = '9'; assert.equal(f.editor.isDirty(), true);
});

test('replacement base fonts regain glyphs needed by preserved annotations and TODO items', async () => {
  const f = editorFixture(), api = require('../html/js/font_builder.js'), migration = migrationFixture();
  const original = bundle(), extended = await api.extendFonts(builder.open(original).fonts,
    [{ slot: 0, text: '新' }, { slot: 1, text: '新书.' }], { rasterize: async () => ({ width: 1, height: 1, x: 0, y: 0, advance: 2, pixels: Uint8Array.of(1) }) });
  const source = packer.create({ fonts: extended.fonts, annotations: migration.annotations,
    todo: { items: [{ id: 1, text: '书', start: 0, end: 0, resident: true, completed: false }], limit: 5, fontSize: 12 } });
  await f.installer.loadBundle(source.packages);
  for (let i = 0; i < 2; i++) f.node('resourceFont' + i).files = [{ name: 'base.bin', arrayBuffer: async () => builder.open(original).fonts[i].buffer }];
  const next = await f.editor.generate(); assert.ok(next);
  const fonts = builder.open(next).fonts;
  assert.ok(builder.validateFont(fonts[0], 0).has('新'.codePointAt(0)));
  assert.ok(builder.validateFont(fonts[1], 1).has('书'.codePointAt(0)));
  assert.deepEqual(next.packages.find(item => item.pack.id === 3).pack.payload, source.packages.find(item => item.pack.id === 3).pack.payload);
  assert.deepEqual(next.packages.find(item => item.pack.id === 9).pack.payload, source.packages.find(item => item.pack.id === 9).pack.payload);
});

test('drafts retain invalid in-progress JSON and refresh restores it without fetching default assets', async () => {
  let snapshot = null;
  const draft = { save: async data => { snapshot = structuredClone(data); }, read: async () => structuredClone(snapshot), clear: async () => { snapshot = null; } };
  const first = editorFixture({ draft }); await first.editor.ensureReady();
  first.node('resourceCalendarSource').value = '{unfinished'; first.input('resourceCalendarSource');
  await first.editor.saveDraft(); assert.equal(snapshot.values.resourceCalendarSource, '{unfinished');
  const second = editorFixture({ draft }); await second.editor.ensureReady();
  assert.equal(second.requests.length, 0); assert.equal(second.node('resourceCalendarSource').value, '{unfinished');
  assert.equal(second.editor.isDirty(), true); const before = second.installer.getBundle();
  assert.equal(await second.editor.generate(), null); assert.equal(second.installer.getBundle(), before);
  await second.editor.clearDraft(); assert.equal(snapshot, null);
});

test('editor cancellation discards completed font results before replacing the upload bundle', async () => {
  const entered = deferred(), release = deferred();
  const f = editorFixture({ extendFonts: async fonts => { entered.resolve(); await release.promise; return { fonts, added: [] }; } });
  await f.editor.ensureReady(); const before = f.installer.getBundle();
  const generating = f.editor.generate(); await entered.promise;
  assert.equal(f.node('resourceGenerationCancel').disabled, false);
  f.editor.cancel(); release.resolve(); assert.equal(await generating, null);
  assert.equal(f.installer.getBundle(), before); assert.match(f.node('resourceEditorStatus').textContent, /取消/);
});

test('failed draft restoration never generates defaults or overwrites the preserved source and can be retried', async () => {
  const first = editorFixture(); first.editor.load(bundle());
  const values = Object.fromEntries(require('../html/js/ui_tools/resource_project.js').fields.map(id => [id, first.node(id).value]));
  values.resourceExtraText = '保留的草稿'; values.resourceCalendarSource = '{unfinished';
  const snapshot = { version: 1, savedAt: 123, files: bundle().packages, values, bins: [], font: { name: 'user.ttf', bytes: Uint8Array.of(1, 2) } };
  let writes = 0, fail = true, current = null;
  const f = editorFixture({ draft: { read: async () => snapshot, save: async () => { writes++; } },
    fontSources: { create: () => ({ prepare: async file => { if (fail) throw new Error('FontFace failed'); return { ...file, family: 'LocalFont' }; },
      commit: font => { current = font; return font; }, get: () => current, clear: () => { current = null; } }) } });
  await f.editor.ensureReady(); assert.equal(writes, 0); assert.equal(f.requests.length, 0);
  assert.equal(f.installer.getBundle(), null); assert.match(f.node('resourceDraftStatus').textContent, /原草稿已保留/);
  await f.editor.exportProject(true);
  const exported = JSON.parse(await f.saved().text());
  assert.equal(exported.values.resourceCalendarSource, '{unfinished'); assert.equal(exported.font.name, 'user.ttf');
  assert.equal(exported.values.resourceExtraText, '保留的草稿');
  await f.editor.loadDefault(); await f.editor.saveDraft(); assert.equal(writes, 0);
  fail = false; await f.editor.restoreDraft(); assert.equal(f.node('resourceCalendarSource').value, '{unfinished');
  assert.equal(current.name, 'user.ttf'); await f.editor.saveDraft(); assert.equal(writes, 1);
});

test('full project export keeps unfinished edits, pending BINs and settings without generating or replacing the bundle', async () => {
  let generated = 0;
  const f = editorFixture({ extendFonts: async () => { generated++; throw new Error('must not generate'); } });
  await f.installer.loadBundle(bundle().packages); const before = f.installer.getBundle();
  f.node('resourceCalendarSource').value = '{unfinished'; f.node('resourceFontTarget').value = '1';
  f.node('resourceExtraText').value = '未来文字';
  const binary = Uint8Array.of(7, 8, 9);
  f.node('resourceFont1').files = [{ name: 'pending.bin', arrayBuffer: async () => binary.buffer }];
  await f.editor.exportProject(); const exported = JSON.parse(await f.saved().text());
  assert.equal(generated, 0); assert.equal(f.installer.getBundle(), before);
  const next = editorFixture(); await next.import('resourceProjectImport', exported);
  assert.equal(next.node('resourceCalendarSource').value, '{unfinished');
  assert.equal(next.node('resourceFontTarget').value, '1'); assert.equal(next.node('resourceExtraText').value, '未来文字');
  await next.editor.exportProject(); assert.deepEqual(JSON.parse(await next.saved().text()).bins, exported.bins);
  assert.equal(next.editor.isDirty(), true);
});

test('changing the clock text slot generates its voltage, temperature, SSID and sync URL glyphs', async () => {
  const f = editorFixture(); await f.editor.ensureReady();
  const layout = JSON.parse(f.node('resourceLayoutSource').value);
  for(const id of [36,37,43,44]) layout.components.find(c=>c.id===id).fonts[0]=1;
  f.node('resourceLayoutSource').value = JSON.stringify(layout);
  const result = await f.editor.generate(); assert.ok(result);
  const glyphs = builder.validateFont(builder.open(result).fonts[1], 1);
  for (const char of '-.V[]SYNC TIME!https://ycd12.github.io/EPD-nRF5_DYC/') assert.ok(glyphs.has(char.codePointAt(0)), char);
});

test('preview renders both selected slots and hides the second canvas for a single-slot preview', async () => {
  const f = editorFixture(); await f.editor.ensureReady();
  const before = f.installer.getBundle(); await f.editor.previewFonts();
  assert.equal(f.node('resourceFontPreviewSecond').hidden, false);
  assert.match(f.node('resourceFontPreviewInfo').textContent, /槽位 0/);
  assert.match(f.node('resourceFontPreviewSecondInfo').textContent, /槽位 1/);
  f.node('resourceFontTarget').value = '2'; await f.editor.previewFonts();
  assert.equal(f.node('resourceFontPreviewSecond').hidden, true); assert.equal(f.installer.getBundle(), before);
});

test('failed font preparation during project import retains the prior bundle and font source', async () => {
  const fontSource = { prepare: async () => { throw new Error('FontFace failed'); }, clear() {}, get: () => null };
  const f = editorFixture({ fontSources: { create: () => fontSource } }); await f.installer.loadBundle(bundle().packages);
  await f.editor.exportProject(); const data = JSON.parse(await f.saved().text()), before = f.installer.getBundle();
  const bytes = new Uint8Array(72), view = new DataView(bytes.buffer);
  view.setUint32(0, 0x10000); view.setUint16(4, 1); view.setUint32(12, 0x636d6170);
  view.setUint32(20, 28); view.setUint32(24, 44); view.setUint16(30, 1);
  view.setUint16(32, 3); view.setUint16(34, 10); view.setUint32(36, 12);
  view.setUint16(40, 12); view.setUint32(44, 28); view.setUint32(52, 1);
  view.setUint32(56, 48); view.setUint32(60, 57); view.setUint32(64, 1);
  data.font = { name: 'font.ttf', base64: Buffer.from(bytes).toString('base64') }; data.values.resourceFontFamily = 'imported';
  await f.import('resourceProjectImport', data); assert.equal(f.installer.getBundle(), before);
  assert.match(f.node('resourceEditorStatus').textContent, /FontFace failed/);
});

test('clearing a draft invalidates a pending file read without marking unchanged resources dirty', async () => {
  let snapshot = null;
  const draft = { read: async () => snapshot, save: async value => { snapshot = value; }, clear: async () => { snapshot = null; } };
  const f = editorFixture({ draft }); await f.editor.ensureReady();
  assert.equal(f.editor.isDirty(), false); await f.editor.clearDraft(); assert.equal(f.editor.isDirty(), false);
  const read = deferred(); f.node('resourceFont0').files = [{ name: 'pending.bin', arrayBuffer: () => read.promise }];
  const saving = f.editor.saveDraft(); await Promise.resolve(); await f.editor.clearDraft();
  read.resolve(Uint8Array.of(1).buffer); await saving; assert.equal(snapshot, null);
});
