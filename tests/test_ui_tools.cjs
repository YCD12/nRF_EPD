'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const firmwarePolicy = require('../html/js/main.js');
const packer = require('../html/js/ui_tools/resource_pack.js');
const defaults = require('../html/js/ui_tools/resource_defaults.js');
const fontTool = require('../html/js/ui_tools/font_store_tool.js');
const builder = require('../html/js/resource_builder.js');
const store = require('../html/js/resource_store.js');
const todo = require('../html/js/todo_calendar.js');
const fonts = () => ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn'].map(name =>
  new Uint8Array(fs.readFileSync(path.join(__dirname, '../html/resources/fonts/u8g2_font_' + name + '.bin'))));
const create = () => packer.create({ fonts: fonts() }, { builtAt: 12345 });
const payload = (bundle, id) => bundle.packages.find(item => item.pack.id === id).pack.payload;
const json = (files, name) => JSON.parse(new TextDecoder().decode(files.find(file => file.name === name).bytes));

function browserResources() {
  const context = { Uint8Array, DataView, TextEncoder, TextDecoder, Date, atob,
    EpdFirmwarePolicy: firmwarePolicy,
    location: { protocol: 'file:' } };
  for (const name of ['resource_store.js', 'ui_tools/ui_data.js', 'ui_tools/ui_model.js', 'resource_builder.js', 'todo_calendar.js',
    'ui_tools/resource_defaults.js', 'ui_tools/default_fonts.js', 'ui_tools/font_store_tool.js', 'font_builder.js', 'ui_tools/resource_pack.js'])
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../html/js', name), 'utf8'), context);
  return context;
}

test('bundle validation reads TODO font settings from the package independently of the browser draft', async () => {
  const context = browserResources(), base = create();
  const timed = packer.create({ fonts: fonts(), todo: { items: [
    {id:1,text:'年',start:0,end:0,resident:true,completed:false}
  ], limit: 5, fontSize: 9 } });
  for (const draft of [
    { fontFamily: 'SimSun', fontBold: false, fontItalic: false },
    { fontFamily: '', fontBold: true, fontItalic: true },
    { fontFamily: 'KaiTi', fontBold: true, fontItalic: false }
  ]) {
    Object.assign(context.TodoCalendar, draft, { fontSize: 32, limit: 1 });
    for (const bundle of [base, timed]) {
      const inspected = context.EPDResourcePack.inspectBundle(bundle, { repairFontBounds: true });
      assert.equal(inspected.bundle.packages.length, 7);
      assert.equal(context.EPDResourcePack.dependencies(bundle).requests.filter(request => request.slot === 3).length, 0);
      assert.deepEqual(payload(inspected.bundle, 9), payload(bundle, 9));
    }
    const generated = await context.EPDResourcePack.generateDefault({ builtAt: 12345 });
    assert.deepEqual(payload(generated, 7), payload(base, 7));
    assert.deepEqual(payload(generated, 9), payload(base, 9));
    assert.equal(context.TodoCalendar.fontFamily, draft.fontFamily);
    assert.equal(context.TodoCalendar.fontBold, draft.fontBold);
    assert.equal(context.TodoCalendar.fontItalic, draft.fontItalic);
  }
});

test('matching custom TODO fonts remain valid with a different browser draft and mismatches still fail', async () => {
  const config = { items: [], limit: 5, fontSize: 9, fontFamily: 'SimSun' };
  const customFonts = (await require('../html/js/font_builder.js').extendFonts(fonts(),
    [{ slot: 3, sourceSlot: 0, text: ' ...', fontFamily: config.fontFamily }],
    { rasterize: async () => ({ width: 1, height: 1, x: 0, y: 0, advance: 2, pixels: Uint8Array.of(1) }) })).fonts;
  const timed = todo.pack(config), legacy = timed.slice(0, 16 + timed[11]);
  legacy[4] = 2; legacy.set(timed.subarray(21), 16);
  const context = browserResources();
  Object.assign(context.TodoCalendar, { fontFamily: 'KaiTi', fontBold: true, fontItalic: true, fontSize: 32, limit: 1 });
  for (const bytes of [timed, legacy]) {
    const base = packer.create({ fonts: customFonts, todo: bytes });
    const inspected = context.EPDResourcePack.inspectBundle(base, { repairFontBounds: true });
    assert.equal(inspected.model.fonts.customKey, builder.fontKey('SimSun', 0));
    assert.deepEqual(require('../html/js/font_builder.js').gatherMissing(inspected.model.fonts,
      context.EPDResourcePack.dependencies(inspected.bundle, inspected.model).requests), []);
    assert.deepEqual(payload(inspected.bundle, 9), bytes);
    const next = context.EPDResourcePack.build(inspected.bundle, inspected.model);
    assert.deepEqual(payload(next, 7), payload(base, 7));
    assert.deepEqual(payload(next, 9), bytes);
    const invalid = customFonts.slice(); invalid.customKey = (customFonts.customKey ^ 1) >>> 0;
    const mismatched = base.packages.map(item => item.pack.id === 7
      ? {name:item.name,bytes:builder.wrap(7,builder.encodeFonts(invalid),1)} : item);
    assert.throws(() => context.EPDResourcePack.inspectBundle(mismatched, { repairFontBounds: true }), /自定义字体/);
  }
});

test('all seven packages are built from raw sources and match the Python reference payloads', () => {
  // This saved fixture is only a test oracle. The generator never loads it.
  const reference = require('../html/resources/default-project.json');
  const generated = create();
  assert.deepEqual(generated.packages.map(item => item.pack.id), [1, 5, 6, 7, 2, 3, 9]);
  for (const file of reference.files) {
    const old = store.parsePackage(new Uint8Array(Buffer.from(file.base64, 'base64')));
    const item = generated.packages.find(item => item.pack.id === old.id);
    assert.deepEqual(item.pack.payload, old.payload);
    assert.equal(item.pack.revision, old.revision);
    assert.deepEqual(item.bytes, builder.wrap(old.id, old.payload, old.revision, 12345));
  }
  assert.equal(generated.total,reference.files.reduce((n,f)=>n+store.parsePackage(new Uint8Array(Buffer.from(f.base64,'base64'))).payload.length,0));
});

test('default source models are independent editable values', () => {
  const calendar = defaults.calendar(), layout = defaults.layout(), ui = defaults.ui(), config = defaults.config();
  calendar.solar_festivals[0].name = '新年'; layout.partitions[0].size_bytes = 8192;
  ui.components[0].x++; config[0] = 0;
  assert.equal(defaults.calendar().solar_festivals[0].name, '元旦节');
  assert.equal(defaults.layout().partitions[0].size_bytes, 4096);
  assert.equal(defaults.ui().components[0].x, 10); assert.equal(defaults.config()[0], 0x14);
});

test('layout generation accepts the canonical JSON and rejects gaps, overlap, ABI and type errors', () => {
  const canonical = JSON.parse(fs.readFileSync(path.join(__dirname, '../docs/external-flash-layout-v1.draft.json'), 'utf8'));
  assert.deepEqual(packer.tablePayload(canonical), payload(create(), 1));
  for (const edit of [layout => { layout.capacity_bytes++; }, layout => { layout.erase_sector_bytes = 2048; },
    layout => { layout.external_data_copies = 2; }, layout => { layout.partitions[1].offset += 4096; },
    layout => { layout.partitions[1].id = 1; }, layout => { layout.partitions[1].type = 'other'; },
    layout => { layout.partitions[1].schema = 2; }, layout => { layout.partitions[0].size_bytes = 8192; },
    layout => { layout.partitions.pop(); }]) {
    const layout = defaults.layout(); edit(layout); assert.throws(() => packer.tablePayload(layout));
  }
  const wrong = defaults.layout(); wrong.partitions[1].type = 3;
  assert.throws(() => packer.create({ fonts: fonts(), layout: wrong }), /类型/);
});

test('font validation reports signed metrics and requires every fixed UI glyph', () => {
  const data = fonts(), info = fontTool.checkSlot('WQY9', data[0]);
  assert.equal(info.header.y_offset, -2); assert.equal(info.header.start_unicode, 0x0423);
  assert.equal(info.crc32, store.crc32(data[0]));
  assert.deepEqual(fontTool.pack(data), payload(create(), 7));
  const missing = data[0].slice(), v = new DataView(missing.buffer), table = 23 + v.getUint16(21);
  for (let at = table + v.getUint16(table); v.getUint16(at); at += missing[at + 2]) {
    if (v.getUint16(at) === '年'.codePointAt(0)) { v.setUint16(at, 0x0378); break; }
  }
  assert.doesNotThrow(() => builder.validateFont(missing, 0));
  assert.throws(() => fontTool.checkSlot(0, missing), /必需字符：年/);
  const mode = data[0].slice(); mode[1] = 1;
  assert.throws(() => fontTool.checkSlot(0, mode), /边界框模式/);
});

test('browser globals generate resources by fetching only three raw fonts and reuse clean cached copies', async () => {
  const calls = [], context = { Uint8Array, DataView, TextEncoder, TextDecoder, Date,
    EpdFirmwarePolicy: firmwarePolicy,
    location: { protocol: 'https:' },
    fetch: async url => {
      calls.push(url); assert.match(url, /^resources\/fonts\/u8g2_font_[\w]+\.bin$/);
      return { ok: true, arrayBuffer: async () => new Uint8Array(fs.readFileSync(path.join(__dirname, '../html', url))).buffer };
    } };
  for (const name of ['resource_store.js', 'ui_tools/ui_data.js', 'ui_tools/ui_model.js', 'resource_builder.js', 'todo_calendar.js',
    'ui_tools/resource_defaults.js', 'ui_tools/default_fonts.js', 'ui_tools/font_store_tool.js', 'ui_tools/resource_pack.js'])
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../html/js', name), 'utf8'), context);
  const [first, second] = await Promise.all([context.EPDResourcePack.generateDefault({ builtAt: 12345 }),
    context.EPDResourcePack.generateDefault({ builtAt: 23456 })]);
  assert.equal(calls.length, 3); assert.equal(first.packages.length, 7);
  for (const item of first.packages) assert.deepEqual(item.pack.payload, payload(create(), item.pack.id));
  assert.ok(second.packages.every(item => item.pack.builtAt === 23456));
  const loaded = await context.EPDFontStoreTool.loadDefault(); loaded[0][0] = 0;
  assert.notEqual((await context.EPDFontStoreTool.loadDefault())[0][0], 0);
});

test('file URL generates the same seven packages without fetching BINs and returns independent fonts', async () => {
  let calls = 0;
  const context = { Uint8Array, DataView, TextEncoder, TextDecoder, Date, atob,
    EpdFirmwarePolicy: firmwarePolicy,
    location: { protocol: 'file:' },
    fetch: async () => { calls++; throw new TypeError('Failed to fetch'); } };
  for (const name of ['resource_store.js', 'ui_tools/ui_data.js', 'ui_tools/ui_model.js', 'resource_builder.js', 'todo_calendar.js',
    'ui_tools/resource_defaults.js', 'ui_tools/default_fonts.js', 'ui_tools/font_store_tool.js', 'ui_tools/resource_pack.js'])
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../html/js', name), 'utf8'), context);
  const result = await context.EPDResourcePack.generateDefault({ builtAt: 12345 });
  assert.equal(result.packages.length, 7);
  for (const item of result.packages) assert.deepEqual(item.bytes, create().packages.find(row => row.pack.id === item.pack.id).bytes);
  const first = await context.EPDFontStoreTool.loadDefault();
  first[0].fill(0);
  (await context.EPDFontStoreTool.loadDefault()).forEach((bytes, slot) => assert.deepEqual(bytes, fonts()[slot]));
  assert.equal(calls, 0);
  const html = fs.readFileSync(path.join(__dirname, '../html/index.html'), 'utf8');
  assert.ok(html.indexOf('src="js/ui_tools/default_fonts.js') < html.indexOf('src="js/ui_tools/font_store_tool.js'));
});

test('file URL reports an incomplete webpage directory without attempting blocked fetches', async () => {
  let calls = 0;
  const context = { Uint8Array, atob, EPDResourceBuilder: builder, EPDResourceStore: store,
    location: { protocol: 'file:' }, fetch: async () => { calls++; throw new Error('Blocked'); } };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/ui_tools/font_store_tool.js'), 'utf8'), context);
  await assert.rejects(context.EPDFontStoreTool.loadDefault(), /网页随附字模缺失/);
  assert.equal(calls, 0);
});

test('complete rebuild preserves personal records and TODO bytes while updating changed versions', async () => {
  const config = { items: [{ id: 1, text: '看书', start: 0, end: 0, resident: true, completed: false }],
    limit: 5, fontSize: 12, leadMinutes: 10, retainMinutes: 5 };
  const extended = await require('../html/js/font_builder.js').extendFonts(fonts(), [{ slot: 1, text: '看书.' }],
    { rasterize: async () => ({ width: 1, height: 1, x: 0, y: 0, advance: 2, pixels: Uint8Array.of(1) }) });
  const base = packer.create({ fonts: extended.fonts, todo: config }, { builtAt: 12345 });
  const model = builder.open(base); model.ui.components[0].x++;
  const next = packer.build(base, model, { builtAt: 23456 });
  assert.deepEqual(todo.unpack(payload(next, 9)).items, config.items);
  for (const item of next.packages) {
    assert.equal(item.pack.revision, item.pack.id === 6 ? 3 : item.pack.id === 1 ? 2 : 1);
    assert.equal(item.pack.builtAt, 23456);
    if (item.pack.id !== 6) assert.deepEqual(item.pack.payload, payload(base, item.pack.id));
  }
  const versioned = packer.build(next, builder.open(next), { revision: 9, builtAt: 34567 });
  assert.ok(versioned.packages.every(item => item.pack.revision === 9 && item.pack.builtAt === 34567));
  for (const revision of [0, -1, 1.5, 0x100000000, '2']) assert.throws(() => packer.build(base, model, { revision }), /版本/);
  next.packages.find(item => item.pack.id === 6).bytes = builder.wrap(6, payload(next, 6), 0xffffffff);
  model.ui.components[0].x++;
  assert.throws(() => packer.build(next, model), /版本/);
});

test('records, migration and deleted records use compatible envelopes with verified CRCs', () => {
  const config = defaults.config(), annotations = packer.emptyAnnotations(); config[13] = 1;
  const migration = { format: 'EPD-MIG1', epd_config_hex: Buffer.from(config).toString('hex'),
    calendar_annotations_hex: Buffer.from(annotations).toString('hex') };
  const base = create(), next = packer.build(base, builder.open(base), { migration, builtAt: 23456 });
  assert.deepEqual(packer.readRecord(payload(next, 2), 0x31435045).data, config);
  const corrupt = payload(next, 2).slice(); corrupt[16] ^= 1;
  assert.throws(() => packer.readRecord(corrupt, 0x31435045), /CRC/);
  assert.throws(() => packer.create({ fonts: fonts(), config: new Uint8Array(13) }), /14/);
  const broken = annotations.slice(); broken[8] = 1;
  assert.throws(() => packer.create({ fonts: fonts(), annotations: broken }), /标注/);
  for (const [id, magic] of [[2, 0x31435045], [3, 0x31434e41]])
    base.packages.find(item => item.pack.id === id).bytes = builder.wrap(id, packer.recordPayload(magic, new Uint8Array(), true), 1);
  const deleted = packer.build(base, builder.open(base));
  assert.equal(packer.readRecord(payload(deleted, 2), 0x31435045).tombstone, true);
  assert.equal(json(packer.exportFiles(deleted), 'records.json').calendar_annotations.records, 0);
  assert.throws(() => packer.recordPayload(0x31435045, config, true), /删除标志/);
});

test('legacy six-package projects regenerate without altering their existing partition layout', () => {
  const base = create(), layout = defaults.layout(); layout.partitions = layout.partitions.filter(part => part.id !== 9);
  const reserved = layout.partitions.find(part => part.id === 8); reserved.offset = 0x58000; reserved.size_bytes += 4096;
  const entries = base.packages.filter(item => item.pack.id !== 9).map(item => ({ name: item.name,
    bytes: item.pack.id === 1 ? builder.wrap(1, packer.tablePayload(layout), 1) : item.bytes }));
  const legacy = store.prepareBundle(entries), next = packer.build(legacy, builder.open(legacy));
  assert.equal(next.packages.length, 6); assert.deepEqual(payload(next, 1), payload(legacy, 1));
  assert.equal(packer.exportFiles(next).some(file => file.name === 'todo.json'), false);
});

test('exported sidecars describe the actual binary resources and section CRCs', () => {
  const base = create(), files = packer.exportFiles(base), calendar = json(files, 'calendar.json');
  assert.equal(files.length, 13); assert.equal(new Set(files.map(file => file.name)).size, files.length);
  assert.equal(calendar.sections.length, 16); assert.equal(calendar.payload_bytes, payload(base, 5).length);
  for (const section of calendar.sections) {
    const data = payload(base, 5).subarray(section.offset, section.offset + section.length);
    assert.equal(section.length, section.count * section.item_size);
    assert.equal(Number(section.crc32), store.crc32(data));
  }
  assert.equal(json(files, 'ui.json').components.length, 46);
  assert.equal(json(files, 'fonts.json').slots.length, 3);
  assert.equal(json(files, 'records.json').epd_config.bytes, 14);
  assert.equal(json(files, 'todo.json').record_size, 80);
  assert.deepEqual(files.find(file => file.name === 'partition-table.payload.bin').bytes, payload(base, 1));
});

test('resource capacity and malformed TODO payloads are checked before producing a bundle', () => {
  const layout = defaults.layout(); layout.partitions[4].size_bytes -= 4096;
  layout.partitions[5].offset -= 4096; layout.partitions[5].size_bytes += 4096;
  const calendar = defaults.calendar(); calendar.year_max = 4095;
  const years = calendar.year_max - calendar.year_min + 1;
  calendar.lunar_month_days = [calendar.year_min - 1, ...Array(years + 1).fill(0)];
  calendar.solar_new_year = calendar.lunar_month_days.slice(); calendar.jieqi_year_bits = Array((years + 1) * 3).fill(0);
  assert.throws(() => packer.create({ fonts: fonts(), calendar, layout }), /容量/);
  const broken = packer.todoPayload(); broken[5] = 16;
  assert.throws(() => packer.create({ fonts: fonts(), todo: broken }), /格式/);
});

test('semantic validation rejects inner REC1 corruption and unavailable or incompatible TODO fonts', async () => {
  const base = create(), config = payload(base, 2).slice(); config[16] ^= 1;
  const entries = base.packages.map(item => ({ ...item, bytes: item.pack.id === 2 ? builder.wrap(2, config, 1) : item.bytes }));
  assert.doesNotThrow(() => store.prepareBundle(entries));
  assert.throws(() => packer.validateBundle(entries), /REC1.*CRC/);
  assert.throws(() => packer.create({ fonts: fonts(), todo: { items: [], limit: 5, fontSize: 12, fontFamily: 'Custom' } }), /自定义字体/);
  const api = require('../html/js/font_builder.js'), configTodo = { items: [], limit: 5, fontSize: 16, fontFamily: 'Custom' };
  const requests = todo.fontRequests.call({ ...todo, ...todo.normalizeConfig(configTodo), fontFamily: configTodo.fontFamily });
  const extended = await api.extendFonts(fonts(), requests, { rasterize: async () => ({ width: 1, height: 1, x: 0, y: 0, advance: 2, pixels: Uint8Array.of(1) }) });
  assert.doesNotThrow(() => packer.create({ fonts: extended.fonts, todo: configTodo }));
  extended.fonts.customKey ^= 1;
  assert.throws(() => packer.create({ fonts: extended.fonts, todo: configTodo }), /标识/);
  assert.throws(() => packer.create({ fonts: fonts(), todo: { items: [{ id: 1, text: '新', start: 0, end: 0, resident: true, completed: false }], limit: 5, fontSize: 12 } }), /所需字符/);
});

test('UI text roles require matching glyphs and default placeholders create no missing glyphs', () => {
  const ui = defaults.ui(); ui.components[1].fonts[0] = 2;
  assert.throws(() => packer.create({ fonts: fonts(), ui }), /槽位 2.*缺少/);
  const base = create();
  assert.deepEqual(require('../html/js/font_builder.js').gatherMissing(fonts(), packer.dependencies(base).requests), []);
  const calendar = defaults.calendar(); calendar.solar_festivals[0].name = '新 年';
  assert.ok(fontTool.requests(calendar)[1].text.includes('新 年'));
});

test('legacy glyph-offset repair preserves every bitmap and only advances the changed font package', async () => {
  const base = create(), codec = require('../html/js/font_builder.js');
  const updated = (await codec.extendFonts(fonts(), [{ slot: 0, text: '新' }], {
    rasterize: async () => ({ width: 12, height: 12, x: 0, y: -2, advance: 12, pixels: new Uint8Array(144).fill(1) }) })).fonts;
  const font = updated[0]; let at = 23 + (font[21] * 256 + font[22]); at += font[at] * 256 + font[at + 1];
  while (font[at] * 256 + font[at + 1] !== 0x65b0) at += font[at + 2];
  const bit = at * 8 + 24 + font[4] + font[5] + font[6], stored = 2 ** (font[7] - 1) - 1;
  for (let i = 0; i < font[7]; i++) {
    const index = (bit + i) >> 3, mask = 1 << ((bit + i) & 7);
    font[index] = (font[index] & ~mask) | (((stored >> i) & 1) ? mask : 0);
  }
  const entries = base.packages.map(item => item.pack.id === 7
    ? { name: item.name, bytes: builder.wrap(7, builder.encodeFonts(updated), item.pack.revision) } : item);
  assert.throws(() => packer.validateBundle(entries), /偏移.*边界/);
  const repaired = packer.inspectBundle(entries, { repairFontBounds: true });
  assert.deepEqual(repaired.repairedSlots, [0]);
  assert.equal(repaired.bundle.packages.find(item => item.pack.id === 7).pack.revision,
    base.packages.find(item => item.pack.id === 7).pack.revision + 1);
  const glyph = codec.glyph(repaired.model.fonts[0], 0x65b0, 0);
  assert.equal(glyph.y, -2); assert.ok(glyph.pixels.every(n => n === 1));
  for (const item of repaired.bundle.packages) if (item.pack.id !== 7)
    assert.deepEqual(item.bytes, base.packages.find(old => old.pack.id === item.pack.id).bytes);
  assert.deepEqual(packer.inspectBundle(repaired.bundle, { repairFontBounds: true }).repairedSlots, []);
});
