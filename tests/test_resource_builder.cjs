const { test } = require('node:test');
const assert = require('node:assert/strict');
const store = require('../html/js/resource_store.js');
const builder = require('../html/js/resource_builder.js');
const template = require('../html/resources/default-project.json');
const fs = require('node:fs'), vm = require('node:vm');
const bundle = () => store.prepareBundle(template.files.map(f => ({ name: f.name, bytes: new Uint8Array(Buffer.from(f.base64, 'base64')) })));

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
    m => { m.ui.components[0].flags = 1; },
    m => { m.ui.components[18].value0 = 0; },
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
  for (const edit of [b => { b[9]++; }, b => { b[2] = 0; }, b => { b[24] = 1; }, b => { b[21] = 255; b[22] = 255; }]) {
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

function editorFixture() {
  const nodes = new Map(), drawings = [];
  const node = id => {
    if (!nodes.has(id)) nodes.set(id, { value: id === 'resourcePreviewMode' ? '1' : '', files: [],
      addEventListener() {}, classList: { toggle() {} },
      getContext: () => ({ clearRect() {}, fillRect() {}, strokeRect: (...args) => drawings.push(args), fillText() {} }) });
    return nodes.get(id);
  };
  let current = null, busy = false, saved = null;
  const installer = {
    busy: () => busy, getBundle: () => current,
    async loadBundle(entries) { current = store.prepareBundle(entries); context.ExternalResourceEditor.load(current); },
  };
  const context = { EPDResourceBuilder: builder, EPDResourceStore: store, ExternalResourceInstaller: installer,
    Uint8Array, Blob, setTimeout: fn => fn(), btoa: b => Buffer.from(b, 'binary').toString('base64'),
    document: { getElementById: node, createElement: () => ({ click() {} }) },
    URL: { createObjectURL: blob => { saved = blob; return 'blob:test'; }, revokeObjectURL() {} } };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_editor.js'), 'utf8'), context);
  return { editor: context.ExternalResourceEditor, node, installer, drawings, busy: value => { busy = value; }, saved: () => saved };
}

test('editor loads, previews, generates, exports a complete reusable project and respects the operation lock', async () => {
  const f = editorFixture();
  f.editor.load(bundle());
  assert.equal(f.drawings.length, 3);
  const calendar = JSON.parse(f.node('resourceCalendarSource').value);
  calendar.solar_festivals[0].name = '新年';
  f.node('resourceCalendarSource').value = JSON.stringify(calendar);
  const result = await f.editor.generate();
  assert.equal(result.packages.length, 6);
  assert.equal(builder.open(result).calendar.solar_festivals[0].name, '新年');
  await f.editor.exportProject();
  const project = JSON.parse(await f.saved().text());
  assert.equal(project.format, 'EPD-RESOURCE-PROJECT-1');
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
