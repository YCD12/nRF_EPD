const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const script = fs.readFileSync(require.resolve('../html/js/todo_calendar.js'), 'utf8');

function fixture({ dom = false, storage = new Map(), installer = {}, connected = true, version = 0x3d, fonts = [], timing = { leadMinutes: 0, retainMinutes: 0 } } = {}) {
  const nodes = new Map(), calls = [], logs = [], events = new Map();
  class Element {
    constructor(tag = 'div') {
      this.tagName = tag.toUpperCase(); this.children = []; this.value = ''; this.checked = false;
      this.disabled = false; this.hidden = false; this.dataset = {}; this.style = {}; this.attributes = {}; this.listeners = new Map();
      const classes = new Set();
      this.classList = { add: name => classes.add(name), remove: name => classes.delete(name),
        contains: name => classes.has(name), toggle: (name, enabled) => enabled ? classes.add(name) : classes.delete(name) };
    }
    set textContent(value) { this._text = String(value); this.children = []; }
    get textContent() { return (this._text || '') + this.children.map(child => child.textContent).join(''); }
    appendChild(child) { this.children.push(child); return child; }
    replaceChildren(...children) { this._text = ''; this.children = children; }
    setAttribute(name, value) { this.attributes[name] = String(value); }
    getAttribute(name) { return this.attributes[name] || null; }
    addEventListener(event, callback) { this.listeners.set(event, callback); }
    dispatch(event, args = {}) { return this.listeners.get(event)?.({ target: this, preventDefault() {}, ...args }); }
    focus() { this.focused = true; }
  }
  const element = id => {
    if (!nodes.has(id)) nodes.set(id, new Element());
    return nodes.get(id);
  };
  const owner = { kind: 'todo', generation: 1 };
  const context = {
    Uint8Array, DataView, TextEncoder, TextDecoder, Date, Set, Map,
    EPDUIModel: require('../html/js/ui_tools/ui_model.js'), EPDUIRenderer: require('../html/js/ui_tools/ui_preview.js'), EPDResourceBuilder: require('../html/js/resource_builder.js'),
    epdCharacteristic: connected ? {} : null,
    appVersion: version,
    addLog: message => logs.push(message),
    localStorage: { getItem: key => storage.get(key) || null, setItem: (key, value) => storage.set(key, value) },
    DeviceOperations: { current: null, assert(actual) { assert.equal(actual, owner); assert.equal(this.current, owner); } },
    ExternalResourceInstaller: {
      busy: () => false,
      saveTodo: async (...args) => { calls.push(['save', ...args]); return true; },
      readTodo: async (...args) => { calls.push(['read', ...args]); return null; },
      ...installer,
    },
  };
  context.runDeviceOperation = async (kind, action, parent = null) => {
    calls.push(['operation', kind]);
    if (context.DeviceOperations.current && context.DeviceOperations.current !== parent) return false;
    const operation = context.DeviceOperations.current = parent || owner;
    try { return await action(operation); }
    finally { if (!parent && context.DeviceOperations.current === operation) context.DeviceOperations.current = null; }
  };
  if (dom) {
    context.document = { readyState: 'loading', hidden: false, getElementById: element,
      createElement: tag => new Element(tag), addEventListener: (name, callback) => events.set(name, callback) };
    element('font-family').options = fonts;
    context.setInterval = () => 1;
  }
  vm.runInNewContext(script, context, { filename: 'todo_calendar.js' });
  const api = context.TodoCalendar;
  if (timing) Object.assign(api, timing);
  if (dom) api.init();
  return { api, context, nodes, element, storage, calls, logs, owner,
    field(id, value, event = 'input') { const node = element(id); node.value = value; node.dispatch(event); } };
}

const start = 1791187200;
function item(id, overrides = {}) {
  return { id, start, end: start + 3600, resident: false, completed: false, text: `事项 ${id}`, ...overrides };
}
function plain(value) { return JSON.parse(JSON.stringify(value)); }

// Legacy wire fixtures exercise readback; production saves always use v4.
function legacyPack(api, config = api) {
  const modern = api.pack(config), trailer = 16 + new DataView(modern.buffer).getUint16(6, true) * 80;
  const version = api.usesTypography(config) ? 3 : config.fontFamily ? 2 : 1;
  const tail = version === 3 ? 1 : 0, bytes = new Uint8Array(trailer + tail + modern[11]);
  bytes.set(modern.subarray(0, trailer)); bytes[4] = version;
  if (tail) bytes[trailer] = modern[trailer];
  bytes.set(modern.subarray(trailer + 5), trailer + tail);
  return bytes;
}

test('wall-clock times round-trip without UTC-offset or DST conversion', () => {
  const { api } = fixture();
  const value = api.parseTime('2026-10-05T09:30:17');
  assert.equal(value, Date.UTC(2026, 9, 5, 9, 30, 17) / 1000);
  assert.equal(api.formatTime(value), '2026-10-05T09:30:17');
  assert.equal(api.localNow({ getFullYear: () => 2026, getMonth: () => 9, getDate: () => 5,
    getHours: () => 9, getMinutes: () => 30, getSeconds: () => 17 }), value);
  assert.equal(api.parseTime('2024-02-29T00:00'), Date.UTC(2024, 1, 29) / 1000);
  for (const invalid of ['2026-02-29T10:00', '2026-10-05T24:00', '2026-13-01T00:00',
    '2026-10-05', '2026-10-05T10:00Z', '2100-01-01T00:00', '', -1, 4294967296, 1.5])
    assert.throws(() => api.parseTime(invalid), /时间|日期|填写/);
});

test('UTF-8 limits preserve valid BMP text and reject unsafe or unsupported text', () => {
  const { api } = fixture();
  assert.equal(api.assertText(' 汉字 ABC '), '汉字 ABC');
  assert.equal(api.assertText('a'.repeat(64)).length, 64);
  assert.equal(api.textByteLength(api.assertText('汉'.repeat(21))), 63);
  assert.throws(() => api.assertText('汉'.repeat(22)), /64/);
  assert.throws(() => api.assertText('a'.repeat(65)), /64/);
  for (const text of ['😀', '\ud800', '\udfff', 'a\0b', 'a\nb', '\nabc', '\txyz', '\uffff', 'a\u202eb', '\ufefftext'])
    assert.throws(() => api.assertText(text), /暂不支持/);
  assert.throws(() => api.assertText('  '), /填写/);
  assert.throws(() => api.assertText({ toString: () => 'text' }), /文字/);
});

test('wire package carries canonical metadata, little-endian records and zero padding', () => {
  const { api } = fixture();
  api.items = [item(22), item(11, { resident: true, start: 0, end: 0, text: '常驻', completed: true })];
  api.limit = 8; api.fontSize = 9;
  const bytes = api.pack(), view = new DataView(bytes.buffer);
  assert.equal(bytes.length, 16 + 2 * 80 + 5);
  assert.equal(Buffer.from(bytes.subarray(0, 4)).toString('ascii'), 'TODO');
  assert.equal(bytes[4], 4); assert.equal(bytes[5], 80);
  assert.equal(view.getUint16(6, true), 2); assert.equal(bytes[8], 8); assert.equal(bytes[9], 9);
  assert.ok(bytes.subarray(10, 16).every(value => value === 0));
  assert.equal(view.getUint32(16, true), 11); assert.equal(bytes[28], 3); assert.equal(bytes[29], 6);
  assert.equal(view.getUint32(96, true), 22); assert.equal(view.getUint32(100, true), start);
  assert.equal(view.getUint32(104, true), start + 3600);
  const decoded = api.unpack(bytes);
  assert.deepEqual(plain(decoded), { items: [api.items[1], api.items[0]], limit: 8, fontSize: 9, fontBold: false, fontItalic: false, leadMinutes: 0, retainMinutes: 0 });
  assert.deepEqual(Array.from(api.pack(decoded)), Array.from(bytes));
  const wrapped = new Uint8Array(bytes.length + 10); wrapped.set(bytes, 5);
  assert.deepEqual(plain(api.unpack(wrapped.subarray(5, 5 + bytes.length))), plain(decoded));
});

test('empty configurations still carry display preferences', () => {
  const { api } = fixture();
  api.limit = 1; api.fontSize = 9;
  assert.equal(api.pack().length, 21);
  assert.deepEqual(plain(api.unpack(api.pack())), { items: [], limit: 1, fontSize: 9, fontBold: false, fontItalic: false, leadMinutes: 0, retainMinutes: 0 });
});

test('the complete unsigned wire time range round-trips through device data and backups', () => {
  const { api } = fixture();
  api.items = [item(1, { start: 0, end: 0xffffffff })];
  assert.deepEqual(plain(api.unpack(api.pack()).items), plain(api.items));
  const restored = fixture().api; restored.importJSON(api.exportJSON());
  assert.deepEqual(plain(restored.items), plain(api.items));
});

test('malformed device records never replace trusted local data', () => {
  const { api } = fixture();
  api.items = [item(1)];
  const source = api.pack(), corrupt = mutate => { const bytes = source.slice(); mutate(bytes); return bytes; };
  const malformed = [source.subarray(0, 15), source.subarray(0, source.length - 1),
    corrupt(b => b[0] = 0), corrupt(b => b[4] = 2), corrupt(b => b[5] = 16),
    corrupt(b => b[6] = 33), corrupt(b => b[8] = 0), corrupt(b => b[9] = 10),
    corrupt(b => b[10] = 1), corrupt(b => b[28] = 4), corrupt(b => b[29] = 65),
    corrupt(b => b[29] = 0), corrupt(b => b[94] = 1), corrupt(b => b[30] = 0xff),
    corrupt(b => b[28] = 1), corrupt(b => new DataView(b.buffer).setUint32(16, 0, true)),
    corrupt(b => new DataView(b.buffer).setUint32(24, start, true)),
  ];
  for (const bytes of malformed) assert.throws(() => api.unpack(bytes));
  assert.deepEqual(plain(api.items), [item(1)]);
});

test('validation enforces count, unique IDs, ordered deadlines and display settings', () => {
  const { api } = fixture();
  for (const config of [
    { items: [item(1), item(1)], limit: 5, fontSize: 12 },
    { items: Array.from({ length: 33 }, (_, index) => item(index + 1)), limit: 5, fontSize: 12 },
    { items: [item(1, { end: start })], limit: 5, fontSize: 12 },
    { items: [item(1, { end: start - 1 })], limit: 5, fontSize: 12 },
    { items: [item(1, { completed: 'false' })], limit: 5, fontSize: 12 },
    { items: [], limit: 9, fontSize: 12 }, { items: [], limit: 1.5, fontSize: 12 },
    { items: [], limit: 0, fontSize: 12 }, { items: [], limit: 5, fontSize: 11 },
  ]) assert.throws(() => api.pack(config));
  assert.equal(api.pack({ items: Array.from({ length: 32 }, (_, index) => item(index + 1)), limit: 1, fontSize: 9 }).length, 2581);
});

test('leading UTF-8 BOM cannot bypass wire text validation', () => {
  const { api } = fixture();
  api.items = [item(1)];
  const bytes = api.pack(); bytes.fill(0, 30, 96);
  const text = new TextEncoder().encode('\ufeff标题'); bytes[29] = text.length; bytes.set(text, 30);
  assert.throws(() => api.unpack(bytes), /格式字符/);
});

test('scheduler honors start inclusion, exclusive expiry and resident priority', () => {
  const { api } = fixture();
  api.items = [item(1), item(2, { start: start - 300, end: start + 500 }),
    item(3, { resident: true }), item(4, { completed: true }),
    item(5, { start: start + 1 }), item(6, { end: start })];
  api.limit = 2;
  assert.deepEqual(api.visibleItems(start).map(item => item.id), [3, 2]);
  assert.deepEqual(api.visibleItems(start + 500).map(item => item.id), [3, 1]);
  assert.deepEqual(api.visibleItems(start + 3600).map(item => item.id), [3]);
  assert.deepEqual(api.visibleItems(start - 301).map(item => item.id), [3]);
});

test('daily totals include unfinished resident and overlapping scheduled tasks independently of visibility', () => {
  const { api } = fixture(), day = api.parseTime('2026-10-05T00:00'), now = day + 12 * 3600;
  api.limit = 1;
  api.items = [item(1, { resident: true }), item(2, { resident: true, completed: true }),
    item(3, { start: now, end: now + 3600 }),
    item(4, { start: now + 3600, end: now + 7200 }),
    item(5, { start: day + 1, end: now }),
    item(6, { start: day - 3600, end: day }),
    item(7, { start: day + 86400, end: day + 90000 }),
    item(8, { start: day - 1, end: day + 1 }),
    item(9, { start: day + 86399, end: day + 86401 }),
    item(10, { start: now, end: now + 1, completed: true })];
  assert.equal(api.dailyCount(now), 6);
  assert.equal(api.dailyCount(day), 6);
  assert.equal(api.visibleItems(now).length, 1);
  api.limit = 8;
  assert.equal(api.dailyCount(now), 6);
  assert.equal(api.dailyCount(day - 1), 3);
  assert.equal(api.dailyCount(day + 86400), 3);
  api.items = [item(1, { start: 0xfffffffe, end: 0xffffffff }), item(2, { resident: true })];
  assert.equal(api.dailyCount(0xffffffff), 2);
});

test('queued items fill expired or completed slots and ties use deadline then ID', () => {
  const { api } = fixture();
  api.limit = 2;
  api.items = [item(9, { end: start + 300 }), item(8, { end: start + 200 }), item(7, { end: start + 200 }),
    item(6, { start: start + 1, end: start + 600 })];
  assert.deepEqual(api.visibleItems(start).map(item => item.id), [7, 8]);
  assert.deepEqual(api.visibleItems(start + 200).map(item => item.id), [9, 6]);
  api.toggleComplete(9);
  assert.deepEqual(api.visibleItems(start + 201).map(item => item.id), [6]);
  api.toggleComplete(9);
  assert.deepEqual(api.visibleItems(start + 201).map(item => item.id), [9, 6]);
});

test('font requests include queued, future, resident and completed text in the selected slot', () => {
  const { api } = fixture();
  assert.deepEqual(plain(api.fontRequests()), []);
  api.items = [item(1), item(2, { start: start + 1 }), item(3, { resident: true }), item(4, { completed: true })];
  const text = api.items.map(item => item.text).join('');
  assert.deepEqual(plain(api.fontRequests()), [{ slot: 1, text: '...'+text }]);
  api.fontSize = 9;
  assert.deepEqual(plain(api.fontRequests()), [{ slot: 0, text: '...'+text }]);
});

test('editor safely renders literal text, edits, completes and removes records', () => {
  const { api, field, element } = fixture({ dom: true });
  field('todoCalendarText', '<img src=x onerror=alert(1)>');
  element('todoCalendarResident').checked = true;
  assert.equal(api.addOrUpdate(), true);
  const row = element('todoCalendarList').children[0];
  assert.equal(row.children[0].children[0].textContent, '<img src=x onerror=alert(1)>');
  assert.equal(row.children[0].children[0].children.length, 0);
  assert.equal(row.children[2].children[1].getAttribute('aria-label'), '编辑 <img src=x onerror=alert(1)>');
  row.children[2].children[1].onclick();
  assert.equal(api._editingId, 1); assert.equal(element('todoCalendarText').focused, true);
  assert.equal(element('todoCalendarStart').disabled, true);
  field('todoCalendarText', '阅读');
  assert.equal(api.addOrUpdate(), true); assert.equal(api.items.length, 1);
  api.toggleComplete(1); assert.equal(api.visibleItems(start).length, 0);
  api.toggleComplete(1); assert.equal(api.visibleItems(start).length, 1);
  api.remove(1); assert.equal(api.items.length, 0);
});

test('invalid time, text and unfinished IME input preserve the list', () => {
  const { api, field, element } = fixture({ dom: true });
  field('todoCalendarText', '报告');
  field('todoCalendarStart', '2026-10-05T12:00'); field('todoCalendarEnd', '2026-10-05T11:00');
  assert.equal(api.addOrUpdate(), false); assert.equal(api.items.length, 0);
  assert.match(element('todoCalendarStatus').textContent, /截止时间/);
  field('todoCalendarEnd', '2026-10-05T13:00');
  element('todoCalendarText').dispatch('compositionstart');
  assert.equal(api.addOrUpdate(), false); assert.equal(api.items.length, 0);
  element('todoCalendarText').dispatch('compositionend');
  assert.equal(api.addOrUpdate(), true);
  api.edit(1); field('todoCalendarText', '😀');
  assert.equal(api.addOrUpdate(), false); assert.equal(api.items[0].text, '报告');
});

test('local storage restores display options and an unfinished editor draft', () => {
  const storage = new Map(), first = fixture({ dom: true, storage });
  first.api.items = [item(1)]; first.api.changed(); first.api.edit(1);
  first.field('todoCalendarText', '尚未提交');
  first.field('todoCalendarEnd', '');
  first.field('todoCalendarLimit', '3', 'change');
  first.field('todoCalendarFontSize', '9', 'change');
  const second = fixture({ dom: true, storage });
  assert.equal(second.api.items[0].text, '事项 1'); assert.equal(second.api.limit, 3); assert.equal(second.api.fontSize, 9);
  assert.equal(second.api._editingId, 1);
  assert.equal(second.element('todoCalendarText').value, '尚未提交');
  assert.equal(second.element('todoCalendarEnd').value, '');
});

test('JSON backup round-trips and invalid imports are atomic', () => {
  const { api } = fixture();
  api.items = [item(1), item(2, { resident: true, start: 0, end: 0 })]; api.limit = 7; api.fontSize = 9;
  const backup = api.exportJSON();
  assert.match(backup, /"start": "2026-/);
  const other = fixture().api; other.importJSON(backup);
  assert.deepEqual(plain(other.items), plain(api.items)); assert.equal(other.limit, 7); assert.equal(other.fontSize, 9);
  const before = JSON.stringify(other.items);
  const corrupt = JSON.parse(backup); corrupt.items[1].text = '😀';
  assert.throws(() => other.importJSON(JSON.stringify(corrupt)));
  assert.equal(JSON.stringify(other.items), before);
  assert.throws(() => other.importJSON('{"version":9}'), /版本/);
});

test('image-workbench import preserves completion and validates every item before importing', () => {
  const { api } = fixture();
  api.items = [item(0xffffffff)];
  assert.equal(api.importPaintTodos({ todoItems: [{ text: '清单一', completed: false }, { text: '清单二', completed: true }] }), true);
  assert.deepEqual(plain(api.items.map(item => item.id)), [0xffffffff, 1, 2]);
  assert.equal(api.items[1].resident, true); assert.equal(api.items[1].start, 0); assert.equal(api.items[1].end, 0);
  assert.equal(api.items[2].completed, true);
  const before = JSON.stringify(api.items);
  assert.equal(api.importPaintTodos({ todoItems: [{ text: '有效' }, { text: '😀' }] }), false);
  assert.equal(JSON.stringify(api.items), before);
});

test('preview uses the same scheduling rules and supports future simulation', () => {
  const { api, element, field } = fixture({ dom: true });
  const now = api.parseTime('2026-10-05T12:00');
  api.items = [item(1, { start: now, end: now + 60 }), item(2, { start: now + 60, end: now + 120 })];
  api.limit = 1;
  field('todoCalendarPreviewTime', '2026-10-05T12:00');
  assert.match(element('todoCalendarPreviewItems').textContent, /事项 1/);
  assert.doesNotMatch(element('todoCalendarPreviewItems').textContent, /事项 2/);
  field('todoCalendarPreviewTime', '2026-10-05T12:01');
  assert.match(element('todoCalendarPreviewItems').textContent, /事项 2/);
  assert.doesNotMatch(element('todoCalendarPreviewItems').textContent, /事项 1/);
  assert.match(element('todoCalendarPreviewCalendar').textContent, /2026 年 10 月/);
});

test('preview heading tracks the simulated day and completion, without using the screen limit', () => {
  const { api, element, field } = fixture({ dom: true }), day = api.parseTime('2026-10-05T00:00');
  api.items = [item(1, { resident: true }), item(2, { start: day, end: day + 60 }),
    item(3, { start: day + 86399, end: day + 86401 })];
  api.limit = 1;
  field('todoCalendarPreviewTime', '2026-10-05T12:00');
  assert.equal(element('todoCalendarPreviewTitle').textContent, 'TODO 1/3');
  assert.equal(element('todoCalendarPreviewTitle').getAttribute('aria-label'), '当前显示 1 项，当天未完成待办总数 3');
  assert.match(element('todoCalendarPreviewItems').textContent, /事项 1/);
  api.toggleComplete(2);
  assert.equal(element('todoCalendarPreviewTitle').textContent, 'TODO 1/2');
  field('todoCalendarPreviewTime', '2026-10-06T12:00');
  assert.equal(element('todoCalendarPreviewTitle').textContent, 'TODO 1/2');
  field('todoCalendarPreviewTime', '2026-10-07T12:00');
  assert.equal(element('todoCalendarPreviewTitle').textContent, 'TODO 1/1');
  field('todoCalendarLayout', '5', 'change');
  assert.equal(element('todoCalendarPreviewTitle').textContent, 'TODO 1/1');
});

test('preview temperature starts unknown, supports signed readings and clears unavailable or invalid readings', () => {
  const { api, element, field } = fixture({ dom: true });
  const temperature = element('todoCalendarPreviewTemperature');
  assert.equal(temperature.textContent, '—℃');
  for (const value of [25, 0, -8, -128, 127]) {
    api.setTemperature(value);
    assert.equal(temperature.textContent, `${value}℃`);
    field('todoCalendarLayout', '5', 'change');
    assert.equal(temperature.textContent, `${value}℃`);
  }
  for (const unavailable of [null, undefined, NaN, Infinity, '25', 25.5, -129, 128]) {
    api.setTemperature(25); api.setTemperature(unavailable);
    assert.equal(temperature.textContent, '—℃');
  }
});

test('successful device saves pass the same operation owner with the snapshot and fonts', async () => {
  const { api, calls, owner, logs, context } = fixture();
  api.items = [item(1)];
  assert.equal(await api.save(), true);
  const saved = calls.find(call => call[0] === 'save');
  assert.equal(saved[3], owner); assert.equal(typeof saved[4], 'function');
  assert.deepEqual(plain(api.unpack(saved[1]).items), [item(1)]);
  assert.deepEqual(plain(saved[2]), [{ slot: 1, text: '...事项 1' }]);
  assert.match(logs.at(-1), /已保存 1 条/); assert.equal(context.DeviceOperations.current, null);
});

test('false, missing confirmation, font and transport failures never report save success', async () => {
  for (const saveTodo of [async () => false, async () => undefined, async () => ({}),
    async () => { throw new Error('字模生成失败'); }, async () => { throw new Error('Flash 校验失败'); }]) {
    const { api, logs, context } = fixture({ installer: { saveTodo } }); api.items = [item(1)];
    assert.equal(await api.save(), false);
    assert.equal(logs.some(message => /已保存 .*条|本次待办已保存/.test(message)), false);
    assert.match(logs.at(-1), /未确认|失败/); assert.equal(context.DeviceOperations.current, null);
  }
});

test('unsupported firmware, disconnection and invalid local data prevent device writes', async () => {
  for (const options of [{ connected: false }, { version: 0x39 }, { version: null }, { version: NaN }, { version: Infinity }]) {
    const { api, calls } = fixture(options); api.items = [item(1)];
    assert.equal(await api.save(), false); assert.equal(calls.length, 0);
  }
  const { api, calls } = fixture(); api.items = [item(1, { text: '😀' })];
  assert.equal(await api.save(), false); assert.equal(calls.some(call => call[0] === 'save'), false);
});

test('device readback replaces the draft only after validation and preserves it on failure', async () => {
  const first = fixture(); first.api.items = [item(1)]; first.api.limit = 3;
  const { api, calls, owner } = fixture({ installer: { readTodo: async actual => { assert.equal(actual, owner); return first.api.pack(); } } });
  api.items = [item(2)];
  assert.equal(await api.loadFromDevice(), true); assert.deepEqual(plain(api.items), [item(1)]); assert.equal(api.limit, 3);
  assert.equal(calls[0][1], 'todo');
  const malformed = fixture({ installer: { readTodo: async () => new Uint8Array(3) } });
  malformed.api.items = [item(2)];
  assert.equal(await malformed.api.loadFromDevice(), false); assert.deepEqual(plain(malformed.api.items), [item(2)]);
  const failure = fixture({ installer: { readTodo: async () => { throw new Error('读取失败'); } } });
  failure.api.items = [item(3)]; assert.equal(await failure.api.loadFromDevice(), false);
  assert.deepEqual(plain(failure.api.items), [item(3)]);
});

test('blank device data returns the empty defaults and a stale read does not discard edits', async () => {
  const blank = fixture(); blank.api.items = [item(1)]; blank.api.limit = 8;
  assert.equal(await blank.api.loadFromDevice(), true);
  assert.equal(blank.api.items.length, 0); assert.equal(blank.api.limit, 5);
  let resolve;
  const pendingRead = fixture({ dom: true, installer: { readTodo: () => new Promise(done => { resolve = done; }) } });
  pendingRead.api.items = [item(2)];
  const pending = pendingRead.api.loadFromDevice();
  pendingRead.field('todoCalendarText', '正在编辑');
  resolve(null);
  assert.equal(await pending, false); assert.deepEqual(plain(pendingRead.api.items), [item(2)]);
  assert.equal(pendingRead.element('todoCalendarText').value, '正在编辑');
});

test('save distinguishes its confirmed snapshot from changes made during the operation', async () => {
  let resolve;
  const { api, logs } = fixture({ installer: { saveTodo: () => new Promise(done => { resolve = done; }) } });
  api.items = [item(1)];
  const pending = api.save();
  api.items.push(item(2)); api.changed(); resolve(true);
  assert.equal(await pending, true); assert.match(logs.at(-1), /本地还有新修改/);
});

test('device controls respect version, connection and the global operation lock', () => {
  const { api, element, context } = fixture({ dom: true });
  for (const id of ['todoCalendarSave', 'todoCalendarRead', 'todocalendarmodebutton']) assert.equal(element(id).disabled, false);
  api.controls(true); assert.equal(element('todoCalendarSave').disabled, true);
  api.controls(); assert.equal(element('todoCalendarSave').disabled, false);
  context.appVersion = 0x39; api.controls(); assert.equal(element('todoCalendarSave').disabled, true);
  context.appVersion = 0x3a; context.DeviceOperations.current = {}; api.controls();
  assert.equal(element('todoCalendarRead').disabled, true);
  context.DeviceOperations.current = null; context.epdCharacteristic = null; api.controls();
  assert.equal(element('todocalendarmodebutton').disabled, true);
});

test('unfinished new or edited items prevent a misleading device save or backup', async () => {
  const { api, field, calls, element } = fixture({ dom: true });
  api.items = [item(1)]; api.changed();
  field('todoCalendarText', '还未添加');
  assert.equal(await api.save(), false);
  assert.equal(calls.some(call => call[0] === 'save'), false);
  assert.match(element('todoCalendarStatus').textContent, /添加事项.*更新事项/);
  assert.throws(() => api.exportJSON(), /尚未加入清单/);
  api.resetEditor(); api.edit(1);
  assert.equal(await api.save(), true);
  field('todoCalendarText', '尚未更新');
  assert.equal(await api.save(), false);
  assert.throws(() => api.exportJSON(), /尚未加入清单/);
  assert.equal(api.items[0].text, '事项 1');
  assert.equal(api.addOrUpdate(), true);
  assert.equal(await api.save(), true);
  assert.equal(JSON.parse(api.exportJSON()).items[0].text, '尚未更新');
});

test('periodic rendering preserves row buttons and unfinished display-number input', () => {
  const { api, element } = fixture({ dom: true });
  api.items = [item(1, { resident: true, start: 0, end: 0 })]; api.changed();
  const row = element('todoCalendarList').children[0];
  const preview = element('todoCalendarPreviewItems').children[0];
  const calendar = element('todoCalendarPreviewCalendar').children[0];
  const limit = element('todoCalendarLimit'); limit.value = '';
  api.render();
  assert.equal(element('todoCalendarList').children[0], row);
  assert.equal(element('todoCalendarPreviewItems').children[0], preview);
  assert.equal(element('todoCalendarPreviewCalendar').children[0], calendar);
  assert.equal(limit.value, '');
  limit.dispatch('change');
  assert.equal(api.limit, 5); assert.equal(limit.value, '5');
  assert.match(element('todoCalendarStatus').textContent, /1 至 8/);
});

test('time-boundary state changes still update cached preview and list rows', () => {
  const { api, field, element } = fixture({ dom: true });
  const now = api.parseTime('2026-10-05T12:00');
  api.items = [item(1, { start: now + 30, end: now + 60 })]; api.limit = 1;
  field('todoCalendarPreviewTime', '2026-10-05T12:00:29');
  assert.match(element('todoCalendarList').textContent, /未触发/);
  field('todoCalendarPreviewTime', '2026-10-05T12:00:30');
  assert.match(element('todoCalendarList').textContent, /正在显示/);
  assert.match(element('todoCalendarPreviewItems').textContent, /事项 1/);
  field('todoCalendarPreviewTime', '2026-10-05T12:01:00');
  assert.match(element('todoCalendarList').textContent, /已过期/);
  assert.match(element('todoCalendarPreviewItems').textContent, /暂无待办/);
});

test('late JSON-file selection cannot replace a newer import or overwrite its status', async () => {
  const { api, element } = fixture({ dom: true });
  const backup = id => JSON.stringify({ version: 1, items: [item(id)], limit: 5, fontSize: 12 });
  let resolveOld;
  const input = element('todoCalendarImport');
  input.files = [{ size: 400, text: () => new Promise(resolve => { resolveOld = resolve; }) }];
  const old = input.dispatch('change');
  input.files = [{ size: 400, text: async () => backup(2) }];
  await input.dispatch('change');
  const status = element('todoCalendarStatus').textContent;
  resolveOld('{broken'); await old;
  assert.deepEqual(plain(api.items), [item(2)]);
  assert.equal(element('todoCalendarStatus').textContent, status);
});

test('JSON-file loading preserves edits made after selecting the file', async () => {
  const { api, field, element } = fixture({ dom: true });
  api.items = [item(1)]; api.changed();
  let resolve;
  const input = element('todoCalendarImport');
  input.files = [{ size: 400, text: () => new Promise(done => { resolve = done; }) }];
  const pending = input.dispatch('change');
  field('todoCalendarText', '正在输入');
  resolve(JSON.stringify({ version: 1, items: [], limit: 5, fontSize: 12 }));
  await pending;
  assert.deepEqual(plain(api.items), [item(1)]);
  assert.equal(element('todoCalendarText').value, '正在输入');
  assert.match(element('todoCalendarStatus').textContent, /读取备份期间草稿已修改/);
});

test('late device progress and failures preserve the newer operation status and owner', async () => {
  let reject, progress;
  const { api, context, element } = fixture({ dom: true, installer: {
    saveTodo: (bytes, requests, owner, notify) => {
      progress = notify; return new Promise((resolve, fail) => { reject = fail; });
    },
  } });
  api.items = [item(1)]; api.changed();
  const pending = api.save();
  const current = context.DeviceOperations.current = {};
  element('todoCalendarStatus').textContent = '新的设备操作正在进行';
  progress('旧连接进度'); reject(new Error('旧连接失败'));
  assert.equal(await pending, false);
  assert.equal(element('todoCalendarStatus').textContent, '新的设备操作正在进行');
  assert.equal(context.DeviceOperations.current, current);
});

test('selecting an editor while device readback is pending preserves that editor', async () => {
  let resolve;
  const { api, element } = fixture({ dom: true, installer: { readTodo: () => new Promise(done => { resolve = done; }) } });
  api.items = [item(1)]; api.changed();
  const pending = api.loadFromDevice();
  api.edit(1); resolve(null);
  assert.equal(await pending, false);
  assert.equal(api._editingId, 1); assert.equal(element('todoCalendarText').value, '事项 1');
  assert.deepEqual(plain(api.items), [item(1)]);
});

test('split and full layouts switch the live preview without changing the shared todo payload', () => {
  const { api, field, element } = fixture({ dom: true });
  api.items = [item(1, { resident: true, start: 0, end: 0 })]; api.limit = 8; api.changed();
  const bytes = api.pack();
  assert.equal(element('todoCalendarPreviewScreen').dataset.layout, 'split');
  assert.equal(element('todoCalendarPreviewCalendar').hidden, false);
  assert.equal(element('todoCalendarPreviewItems').dataset.compact, 'true');
  field('todoCalendarLayout', '5', 'change');
  assert.equal(api.previewMode, 5);
  assert.equal(element('todoCalendarPreviewScreen').dataset.layout, 'full');
  assert.equal(element('todoCalendarPreviewCalendar').hidden, true);
  assert.equal(element('todoCalendarPreviewItems').dataset.compact, 'false');
  assert.equal(element('todoCalendarPreviewLayout').textContent, '全屏待办');
  assert.match(element('todoCalendarLayoutHint').textContent, /0x3B/);
  assert.deepEqual(Array.from(api.pack()), Array.from(bytes));
  assert.match(element('todoCalendarPreviewItems').textContent, /事项 1/);
  field('todoCalendarLayout', '4', 'change');
  assert.equal(element('todoCalendarPreviewCalendar').hidden, false);
  assert.equal(element('todoCalendarPreviewItems').dataset.compact, 'true');
});

test('layout choice persists locally and legacy drafts retain the split default', () => {
  const storage = new Map(), first = fixture({ dom: true, storage });
  first.field('todoCalendarLayout', '5', 'change');
  const second = fixture({ dom: true, storage });
  assert.equal(second.api.previewMode, 5); assert.equal(second.element('todoCalendarLayout').value, '5');
  const parsed = JSON.parse(storage.get(second.api.STORAGE_KEY)); delete parsed.previewMode;
  storage.set(second.api.STORAGE_KEY, JSON.stringify(parsed));
  assert.equal(fixture({ dom: true, storage }).api.previewMode, 4);
});

test('full-screen activation retains its 0x3B gate while new data saving requires 0x3D', async () => {
  const { api, context, field, element, calls } = fixture({ dom: true, version: 0x3a });
  const modes = []; context.syncTime = async mode => { modes.push(mode); return true; };
  field('todoCalendarLayout', '5', 'change');
  assert.equal(element('todoCalendarActivate').disabled, true);
  assert.equal(element('todofullmodebutton').disabled, true);
  assert.equal(element('todocalendarmodebutton').disabled, false);
  assert.equal(element('todoCalendarSave').disabled, true);
  assert.equal(await api.activate(), false); assert.deepEqual(modes, []);
  assert.match(element('todoCalendarStatus').textContent, /全屏待办.*0x3B/);
  assert.equal(await api.save(), false); assert.equal(calls.some(call => call[0] === 'save'), false);
  context.appVersion = 0x3b; api.controls();
  assert.equal(element('todoCalendarActivate').disabled, false);
  assert.equal(element('todofullmodebutton').disabled, false);
  assert.equal(await api.activate(), true); assert.deepEqual(modes, [5]);
  field('todoCalendarLayout', '4', 'change');
  assert.equal(await api.activate(), true); assert.deepEqual(modes, [5, 4]);
  context.syncTime = async () => false;
  assert.equal(await api.activate(), false);
  context.DeviceOperations.current = {}; api.controls();
  assert.equal(element('todoCalendarActivate').disabled, true);
  assert.equal(element('todofullmodebutton').disabled, true);
});

test('legacy import reads the retained draft API and appends without replacing existing items', () => {
  const { api } = fixture(); api.items = [item(1)];
  const retained = [{ text: '旧版清单事项', completed: true }];
  assert.equal(api.importPaintTodos({ todoItems: [], getLegacyTodoItems: () => retained }), true);
  assert.deepEqual(plain(api.items[0]), item(1));
  assert.equal(api.items[1].text, '旧版清单事项'); assert.equal(api.items[1].completed, true);
  assert.equal(api.items[1].resident, true); assert.equal(retained.length, 1);
});

test('long legacy text splits at BMP boundaries and preserves the original content', () => {
  const { api } = fixture();
  const original = '旧版待办内容'.repeat(15) + ' ASCII words '.repeat(10) + '结尾';
  const legacy = [{ text: original, completed: true }];
  assert.equal(api.importPaintTodos({ getLegacyTodoItems: () => legacy }), true);
  assert.ok(api.items.length > 1);
  assert.equal(api.items.map(entry => entry.text).join(''), original);
  assert.ok(api.items.every(entry => api.textByteLength(entry.text) <= 64 && entry.resident && entry.completed));
  assert.deepEqual(legacy, [{ text: original, completed: true }]);
});

test('an oversized or unsupported legacy batch leaves both the current and old draft untouched', () => {
  const { api, logs } = fixture();
  api.items = Array.from({ length: 31 }, (_, index) => item(index + 1));
  const before = JSON.stringify(api.items), legacy = [{ text: '中文'.repeat(40) }];
  assert.equal(api.importPaintTodos({ getLegacyTodoItems: () => legacy }), false);
  assert.equal(JSON.stringify(api.items), before); assert.equal(legacy[0].text, '中文'.repeat(40));
  assert.match(logs.at(-1), /当前清单和旧版草稿均已保留/);
  assert.equal(api.importPaintTodos({ getLegacyTodoItems: () => [{ text: 'abc😀' }] }), false);
  assert.equal(JSON.stringify(api.items), before);
});

test('font choices reuse the image text tool and apply only to the todo preview', () => {
  const { api, field, element } = fixture({ dom: true, fonts: [
    { value: 'SimSun', textContent: '宋体' }, { value: 'KaiTi', textContent: '楷体' },
  ] });
  assert.deepEqual(element('todoCalendarFontFamily').children.map(option => [option.value, option.textContent]),
    [['', '设备默认'], ['SimSun', '宋体'], ['KaiTi', '楷体']]);
  field('todoCalendarFontFamily', 'SimSun', 'change');
  assert.equal(api.fontFamily, 'SimSun');
  assert.equal(element('todoCalendarPreviewItems').style.fontFamily, 'SimSun');
  assert.equal(element('todoCalendarPreviewCalendar').style.fontFamily, undefined);
  assert.equal(element('todoCalendarPreviewScreen').style.fontFamily, undefined);
  field('todoCalendarFontFamily', '', 'change');
  assert.equal(element('todoCalendarPreviewItems').style.fontFamily, '');
});

test('custom font requests target slot 3 and retain the selected standard size metrics', () => {
  const { api } = fixture(); api.items = [item(1)]; api.fontFamily = 'SimSun';
  assert.deepEqual(plain(api.fontRequests()), [{ slot: 3, sourceSlot: 1, text: ' ...事项 1', fontFamily: 'SimSun' }]);
  api.fontSize = 9;
  assert.deepEqual(plain(api.fontRequests()), [{ slot: 3, sourceSlot: 0, text: ' ...事项 1', fontFamily: 'SimSun' }]);
  api.fontFamily = '';
  assert.deepEqual(plain(api.fontRequests()), [{ slot: 0, text: '...事项 1' }]);
});

test('font family survives local and JSON backups, with legacy data defaulting to the device font', () => {
  const storage = new Map(), first = fixture({ dom: true, storage });
  first.field('todoCalendarFontFamily', 'KaiTi', 'change');
  assert.equal(fixture({ dom: true, storage }).api.fontFamily, 'KaiTi');
  const restored = fixture().api; restored.importJSON(first.api.exportJSON());
  assert.equal(restored.fontFamily, 'KaiTi');
  const backup = JSON.parse(first.api.exportJSON()); delete backup.fontFamily;
  restored.importJSON(JSON.stringify(backup)); assert.equal(restored.fontFamily, '');
  backup.fontFamily = '<img onerror=alert(1)>';
  assert.throws(() => restored.importJSON(JSON.stringify(backup)), /字体名称/);
  assert.equal(restored.fontFamily, '');
});

test('custom fonts use the new save gate and keep readback available on old firmware', async () => {
  const { api, field, context, element, calls } = fixture({ dom: true, version: 0x3a });
  field('todoCalendarFontFamily', 'SimSun', 'change');
  assert.equal(element('todoCalendarSave').disabled, true);
  assert.equal(element('todoCalendarRead').disabled, false);
  assert.equal(await api.save(), false); assert.equal(calls.some(call => call[0] === 'save'), false);
  assert.match(element('todoCalendarStatus').textContent, /0x3D/);
  context.appVersion = 0x3d; api.controls();
  assert.equal(element('todoCalendarSave').disabled, false); assert.equal(await api.save(), true);
});

test('an empty custom-font list still requests its dedicated font metrics and ellipsis', () => {
  const { api } = fixture(); api.fontFamily = 'KaiTi';
  assert.deepEqual(plain(api.fontRequests()), [{ slot: 3, sourceSlot: 1, text: ' ...', fontFamily: 'KaiTi' }]);
});

test('reading a default or absent device list clears a previous custom font choice', async () => {
  const defaultList = fixture().api.pack();
  for (const payload of [defaultList, null]) {
    const { api } = fixture({ installer: { readTodo: async () => payload } });
    api.fontFamily = 'KaiTi';
    assert.equal(await api.loadFromDevice(), true); assert.equal(api.fontFamily, '');
  }
});

test('all larger sizes, styles and families round-trip through the new TODO v4 tail', () => {
  const { api } = fixture(), builder = require('../html/js/resource_builder.js');
  api.items = [item(1, { resident: true, start: 0, end: 0 })];
  for (const fontSize of [9, 12, 16, 20, 24, 32]) {
    for (const fontBold of [false, true]) for (const fontItalic of [false, true]) for (const fontFamily of ['', 'SimSun', '宋体']) {
      const config = { items: api.items, limit: Math.min(5, api.maxVisible({ fontSize, fontBold, fontItalic })),
        fontSize, fontBold, fontItalic, fontFamily };
      const bytes = api.pack(config), typography = fontSize > 12 || fontBold || fontItalic;
      assert.equal(bytes[4], 4);
      assert.deepEqual(api.pack(api.unpack(bytes)), bytes);
      const decoded = api.unpack(bytes);
      assert.equal(decoded.leadMinutes, 10); assert.equal(decoded.retainMinutes, 5);
      assert.equal(decoded.fontBold, fontBold); assert.equal(decoded.fontItalic, fontItalic);
      assert.equal(decoded.fontFamily || '', fontFamily);
      assert.equal(bytes[96], Number(fontBold) | (Number(fontItalic) << 1));
      assert.equal(Buffer.from(bytes.subarray(101)).toString('utf8'), fontFamily);
      if (typography) {
        assert.equal(bytes[10], 3); assert.equal(bytes[11], Buffer.byteLength(fontFamily));
        assert.equal(new DataView(bytes.buffer).getUint32(12, true), builder.todoFontKey(fontFamily, fontSize, fontBold, fontItalic));
      } else {
        if (fontFamily) assert.equal(new DataView(bytes.buffer).getUint32(12, true), builder.fontKey(fontFamily, fontSize === 9 ? 0 : 1));
        else assert.ok(bytes.subarray(10, 16).every(value => value === 0));
      }
    }
  }
});

test('TODO v4 validates styles, typography keys, lengths and font-specific visible limits before applying data', () => {
  const { api } = fixture(), builder = require('../html/js/resource_builder.js');
  api.items = [item(1)]; api.fontSize = 32; api.fontBold = true; api.fontItalic = true; api.fontFamily = 'SimSun'; api.limit = 6;
  const bytes = api.pack();
  for (const mutate of [
    copy => { copy[96] |= 4; }, copy => { copy[96] ^= 1; }, copy => { copy[9] = 30; },
    copy => { copy[8] = 7; }, copy => { copy[10] = 1; }, copy => { copy[11] = 65; },
    copy => { copy[101] = 0xff; }, copy => { copy[101] = 32; },
    copy => new DataView(copy.buffer).setUint16(97, 1441, true),
    copy => new DataView(copy.buffer).setUint16(99, 1441, true),
    copy => new DataView(copy.buffer).setUint32(12, 0, true),
    copy => new DataView(copy.buffer).setUint32(12, builder.todoFontKey('SimSun', 24, true, true), true),
  ]) { const copy = bytes.slice(); mutate(copy); assert.throws(() => api.unpack(copy)); }
  assert.throws(() => api.unpack(bytes.subarray(0, bytes.length - 1)));
  const extra = new Uint8Array(bytes.length + 1); extra.set(bytes); assert.throws(() => api.unpack(extra));
  assert.throws(() => api.pack({ items: [], limit: 8, fontSize: 24 }), /最多显示 7/);
  assert.throws(() => api.pack({ items: [], limit: 7, fontSize: 32 }), /最多显示 6/);
  for (const value of ['true', 1, null]) assert.throws(() => api.pack({ items: [], limit: 1, fontSize: 12, fontBold: value }), /字形/);
  const nonCanonical = legacyPack(api, { items: [], limit: 5, fontSize: 12, fontBold: true, fontFamily: '' });
  nonCanonical[16] = 0;
  new DataView(nonCanonical.buffer).setUint32(12, builder.todoFontKey('', 12, false, false), true);
  assert.throws(() => api.unpack(nonCanonical), /字形版本/);
  const oldVersion = api.pack({ items: [], limit: 5, fontSize: 12 }); oldVersion[9] = 16;
  assert.throws(() => api.unpack(oldVersion));
  assert.equal(api.fontSize, 32); assert.equal(api.fontBold, true);
});

test('large and styled text requests its own slot including default-family glyphs, spaces and truncation', () => {
  const { api } = fixture(); api.items = [item(1), item(2, { completed: true })];
  api.fontSize = 32; api.fontBold = true; api.fontItalic = true;
  assert.deepEqual(plain(api.fontRequests()), [{ slot: 3, sourceSlot: 1, text: ' ...事项 1事项 2',
    fontFamily: '', fontSize: 32, fontBold: true, fontItalic: true }]);
  api.items = []; api.fontSize = 9;
  assert.deepEqual(plain(api.fontRequests()), [{ slot: 3, sourceSlot: 0, text: ' ...',
    fontFamily: '', fontSize: 9, fontBold: true, fontItalic: true }]);
});

test('font size and style controls cap visible rows and keep the heading and calendar styles unchanged', () => {
  const { api, element, field } = fixture({ dom: true, version: 0x3c });
  api.items = Array.from({ length: 12 }, (_, index) => item(index + 1, { resident: true, start: 0, end: 0 }));
  field('todoCalendarLimit', '8', 'change'); field('todoCalendarFontSize', '32', 'change');
  assert.equal(api.fontSize, 32); assert.equal(api.limit, 6); assert.equal(element('todoCalendarLimit').value, '6');
  assert.equal(element('todoCalendarLimit').max, '6'); assert.match(element('todoCalendarStatus').textContent, /显示数量已调整为 6/);
  element('todoCalendarFontBold').checked = true; element('todoCalendarFontBold').dispatch('change');
  element('todoCalendarFontItalic').checked = true; element('todoCalendarFontItalic').dispatch('change');
  assert.equal(api.fontBold, true); assert.equal(api.fontItalic, true);
  assert.equal(element('todoCalendarPreviewItems').style.fontWeight, '700');
  assert.equal(element('todoCalendarPreviewItems').style.fontStyle, 'italic');
  assert.equal(element('todoCalendarPreviewTitle').style.fontWeight, undefined);
  assert.equal(element('todoCalendarPreviewCalendar').style.fontStyle, undefined);
  assert.equal(element('todoCalendarPreviewItems').children.length, 6);
  assert.equal(element('todoCalendarPreviewTitle').textContent, 'TODO 6/12');
  element('todoCalendarFontBold').checked = false; element('todoCalendarFontBold').dispatch('change');
  assert.equal(api.fontBold, false); assert.equal(element('todoCalendarPreviewItems').style.fontWeight, '400');
});

test('responsive typography preview follows device font geometry, line capacity and optional metadata', () => {
  const { api, element, field } = fixture({ dom: true });
  api.items = [item(1, { resident: true, start: 0, end: 0 })];
  const screen = element('todoCalendarPreviewScreen'), preview = element('todoCalendarPreviewItems');
  screen.clientWidth = 400;
  field('todoCalendarFontSize', '32', 'change'); field('todoCalendarLimit', '6', 'change');
  assert.equal(preview.style.fontSize, '21.5px'); assert.equal(preview.dataset.typography, 'true');
  assert.equal(preview.style['--todo-row-height'], '32px'); assert.equal(preview.dataset.lines, '1');
  assert.equal(preview.children[0].children[1].children.length, 1);
  field('todoCalendarLimit', '2', 'change');
  assert.equal(preview.dataset.lines, '2'); assert.equal(preview.children[0].children[1].children.length, 1);
  assert.equal(preview.style['--todo-row-height'], '96px');
  screen.clientWidth = 200; api.previewGeometry(); assert.equal(preview.style.fontSize, '10.75px');
});

test('styled typography persists in drafts and backups, and old backups reset styles to plain', () => {
  const storage = new Map(), first = fixture({ dom: true, storage });
  first.field('todoCalendarFontSize', '24', 'change');
  first.element('todoCalendarFontBold').checked = true; first.element('todoCalendarFontBold').dispatch('change');
  first.element('todoCalendarFontItalic').checked = true; first.element('todoCalendarFontItalic').dispatch('change');
  const restored = fixture({ dom: true, storage });
  assert.equal(restored.api.fontSize, 24); assert.equal(restored.api.fontBold, true); assert.equal(restored.api.fontItalic, true);
  assert.equal(restored.element('todoCalendarFontBold').checked, true);
  const other = fixture().api; other.importJSON(first.api.exportJSON());
  assert.equal(other.fontSize, 24); assert.equal(other.fontBold, true); assert.equal(other.fontItalic, true);
  other.importJSON(JSON.stringify({ version: 1, items: [], limit: 5, fontSize: 12 }));
  assert.equal(other.fontBold, false); assert.equal(other.fontItalic, false);
});

test('every new save requires 0x3D while readback remains available on earlier firmware', async () => {
  for (const typography of [{ fontSize: 16 }, { fontBold: true }, { fontItalic: true }]) {
    const { api, context, element, calls } = fixture({ dom: true, version: 0x3b });
    Object.assign(api, typography); api.changed();
    assert.equal(element('todoCalendarSave').disabled, true); assert.equal(element('todoCalendarRead').disabled, false);
    assert.equal(await api.save(), false); assert.equal(calls.some(call => call[0] === 'save'), false);
    assert.match(element('todoCalendarStatus').textContent, /0x3D/);
    context.appVersion = 0x3d; api.controls(); assert.equal(element('todoCalendarSave').disabled, false);
    assert.equal(await api.save(), true); assert.equal(calls.find(call => call[0] === 'save')[1][4], 4);
    Object.assign(api, { fontSize: 12, fontBold: false, fontItalic: false });
    context.appVersion = 0x3a; api.controls(); assert.equal(element('todoCalendarSave').disabled, true);
    assert.equal(await api.save(), false);
  }
});

test('device readback restores v3 styles and clears them for legacy or absent resources', async () => {
  const source = fixture().api; source.fontSize = 20; source.fontBold = true; source.fontItalic = true;
  source.fontFamily = 'KaiTi'; source.items = [item(1)];
  const styled = fixture({ installer: { readTodo: async () => source.pack() } });
  assert.equal(await styled.api.loadFromDevice(), true); assert.equal(styled.api.fontSize, 20);
  assert.equal(styled.api.fontBold, true); assert.equal(styled.api.fontItalic, true); assert.equal(styled.api.fontFamily, 'KaiTi');
  source.fontSize = 12; source.fontBold = false; source.fontItalic = false;
  for (const payload of [source.pack(), fixture().api.pack(), null]) {
    const { api } = fixture({ installer: { readTodo: async () => payload } });
    api.fontBold = true; api.fontItalic = true;
    assert.equal(await api.loadFromDevice(), true); assert.equal(api.fontBold, false); assert.equal(api.fontItalic, false);
  }
});

test('new drafts default to ten minutes early and five minutes after expiry, including migration of old local backups', () => {
  const { api, element } = fixture({ dom: true, timing: null });
  assert.equal(api.leadMinutes, 10); assert.equal(api.retainMinutes, 5);
  assert.equal(element('todoCalendarLeadMinutes').value, '10'); assert.equal(element('todoCalendarRetainMinutes').value, '5');
  assert.match(element('todoCalendarTimingHint').textContent, /提前 10 分钟.*保留 5 分钟.*0x3D/);
  const bytes = api.pack(), view = new DataView(bytes.buffer);
  assert.equal(bytes[4], 4); assert.equal(bytes.length, 21);
  assert.equal(view.getUint16(17, true), 10); assert.equal(view.getUint16(19, true), 5);
  const backup = { version: 1, items: [], limit: 5, fontSize: 12 };
  api.leadMinutes = 0; api.retainMinutes = 0; api.importJSON(JSON.stringify(backup));
  assert.equal(api.leadMinutes, 10); assert.equal(api.retainMinutes, 5);
  const storage = new Map([[api.STORAGE_KEY, JSON.stringify(backup)]]);
  const restored = fixture({ dom: true, storage, timing: null });
  assert.equal(restored.api.leadMinutes, 10); assert.equal(restored.api.retainMinutes, 5);
});

test('legacy device readback keeps its exact immediate-trigger and immediate-expiry behavior', async () => {
  const source = fixture().api;
  source.items = [item(1)];
  for (const preferences of [{}, { fontFamily: 'SimSun' }, { fontSize: 24, fontBold: true, fontItalic: true }]) {
    const bytes = legacyPack(source, { items: source.items, limit: 5, fontSize: 12, ...preferences });
    const { api } = fixture({ timing: null, installer: { readTodo: async () => bytes } });
    assert.equal(await api.loadFromDevice(), true);
    assert.equal(api.leadMinutes, 0); assert.equal(api.retainMinutes, 0);
    assert.equal(api.phase(api.items[0], start - 1), 'hidden');
    assert.equal(api.phase(api.items[0], start), 'active');
    assert.equal(api.phase(api.items[0], start + 3600), 'hidden');
    assert.equal(api.pack()[4], 4);
  }
});

test('phase transitions are exact at early display, trigger, expiry and final removal, including the unsigned range edges', () => {
  const { api } = fixture({ timing: null }), task = item(1);
  for (const [now, expected] of [[start - 601, 'hidden'], [start - 600, 'upcoming'], [start - 1, 'upcoming'],
    [start, 'active'], [task.end - 1, 'active'], [task.end, 'expired'], [task.end + 299, 'expired'], [task.end + 300, 'hidden']]) {
    assert.equal(api.phase(task, now), expected);
  }
  assert.equal(api.phase(item(2, { start: 300, end: 400 }), 0), 'upcoming');
  assert.equal(api.phase(item(2, { start: 0, end: 0xffffffff }), 0xffffffff), 'expired');
  assert.equal(api.phase(item(2, { resident: true }), 0xffffffff), 'resident');
  assert.equal(api.phase(item(2, { resident: true, completed: true }), 0), 'hidden');
  api.leadMinutes = 0; api.retainMinutes = 0;
  assert.equal(api.phase(task, start - 1), 'hidden'); assert.equal(api.phase(task, start), 'active');
  assert.equal(api.phase(task, task.end), 'hidden');
});

test('daily totals include early and grace windows that cross midnight and retain exclusive day boundaries', () => {
  const { api } = fixture({ timing: null }), day = api.parseTime('2026-10-05T00:00'), dayEnd = day + 86400;
  api.items = [item(1, { resident: true }),
    item(2, { start: dayEnd + 300, end: dayEnd + 900 }),
    item(3, { start: day - 900, end: day - 180 }),
    item(4, { start: dayEnd + 600, end: dayEnd + 1200 }),
    item(5, { start: day - 900, end: day - 300 }),
    item(6, { start: dayEnd + 300, end: dayEnd + 900, completed: true })];
  assert.equal(api.dailyCount(day + 12 * 3600), 3);
  api.leadMinutes = 0; api.retainMinutes = 0;
  assert.equal(api.dailyCount(day + 12 * 3600), 1);
});

test('preview changes phase styles without record changes, removes auxiliary captions and reports actual visible count', () => {
  const { api, element, field } = fixture({ dom: true, timing: null });
  api.items = [item(1), item(2, { resident: true, start: 0, end: 0 })]; api.limit = 8;
  const preview = element('todoCalendarPreviewItems'), title = element('todoCalendarPreviewTitle');
  for (const [now, phase] of [[start - 600, 'upcoming'], [start, 'active'], [start + 3600, 'expired']]) {
    field('todoCalendarPreviewTime', api.formatTime(now));
    assert.equal(title.textContent, 'TODO 2/2'); assert.equal(preview.children[1].dataset.phase, phase);
    assert.equal(preview.children[1].children[1].children.length, 1);
    assert.equal(preview.children[1].children[1].children[0].textContent, '事项 1');
    assert.doesNotMatch(preview.textContent, /截止|常驻|PIN/);
    if (phase === 'expired') assert.match(element('todoCalendarList').textContent, /已过期 · 保留显示/);
  }
  field('todoCalendarPreviewTime', api.formatTime(start + 3600 + 300));
  assert.equal(title.textContent, 'TODO 1/2'); assert.equal(preview.children.length, 1);
  api.toggleComplete(2); assert.equal(title.textContent, 'TODO 0/1');
  assert.match(element('todoCalendarList').textContent, /已过期 · 已隐藏/);
});

test('timing preferences persist in drafts, backups and device readback while invalid settings are atomic', async () => {
  const storage = new Map(), first = fixture({ dom: true, storage, timing: null });
  first.field('todoCalendarLeadMinutes', '1440', 'change'); first.field('todoCalendarRetainMinutes', '0', 'change');
  assert.equal(first.api.leadMinutes, 1440); assert.equal(first.api.retainMinutes, 0);
  const restored = fixture({ dom: true, storage, timing: null });
  assert.equal(restored.api.leadMinutes, 1440); assert.equal(restored.api.retainMinutes, 0);
  const imported = fixture({ timing: null }).api; imported.importJSON(first.api.exportJSON());
  assert.equal(imported.leadMinutes, 1440); assert.equal(imported.retainMinutes, 0);
  const device = fixture({ timing: null, installer: { readTodo: async () => first.api.pack() } });
  assert.equal(await device.api.loadFromDevice(), true);
  assert.equal(device.api.leadMinutes, 1440); assert.equal(device.api.retainMinutes, 0);
  for (const invalid of [-1, 1441, 2.5, NaN, Infinity, '10', null]) {
    assert.throws(() => imported.pack({ items: [], limit: 5, fontSize: 12, leadMinutes: invalid }), /0 至 1440/);
    assert.throws(() => imported.pack({ items: [], limit: 5, fontSize: 12, retainMinutes: invalid }), /0 至 1440/);
  }
  first.field('todoCalendarLeadMinutes', '1441', 'change');
  assert.equal(first.api.leadMinutes, 1440); assert.equal(first.element('todoCalendarLeadMinutes').value, '1440');
  const corrupted = JSON.parse(imported.exportJSON()); corrupted.retainMinutes = -1;
  assert.throws(() => imported.importJSON(JSON.stringify(corrupted))); assert.equal(imported.retainMinutes, 0);
});

test('v4 font header combinations reject missing generated fonts, false custom slots and illegal reserved metadata', () => {
  const { api } = fixture({ timing: null });
  const base = api.pack();
  for (const change of [copy => { copy[10] = 3; }, copy => { copy[11] = 1; },
    copy => { copy[12] = 1; }, copy => { copy[16] = 1; }, copy => { copy[9] = 24; }]) {
    const copy = base.slice(); change(copy); assert.throws(() => api.unpack(copy));
  }
  const styled = api.pack({ items: [], limit: 5, fontSize: 12, fontBold: true });
  const noSlot = styled.slice(); noSlot.fill(0, 10, 16); assert.throws(() => api.unpack(noSlot), /专用字库/);
  const withExtra = new Uint8Array(base.length + 1); withExtra.set(base); assert.throws(() => api.unpack(withExtra));
});

test('temperature uses separate numeric and unit nodes and fits the fraction heading at narrow screen widths', () => {
  const { api, element } = fixture({ dom: true, timing: null });
  const temperature = element('todoCalendarPreviewTemperature'), number = element('temperatureValue'), unit = element('temperatureUnit');
  unit.textContent = '℃'; temperature.replaceChildren(number, unit);
  temperature.querySelector = selector => selector === '.todo-calendar-temperature-value' ? number : unit;
  api.setTemperature(-128); assert.equal(temperature.textContent, '-128℃');
  api.setTemperature(null); assert.equal(temperature.textContent, '—℃');
  const screen = element('todoCalendarPreviewScreen'); screen.clientWidth = 232; api.previewGeometry();
  assert.equal(screen.style['--todo-title-size'], '9px');
  assert.equal(screen.style['--todo-temperature-number-size'], '11px');
  assert.equal(screen.style['--todo-temperature-unit-size'], '8px');
});
