'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const test = require('node:test');

function load(extra = {}) {
  const context = {TextEncoder, TextDecoder, Uint8Array, DataView, ArrayBuffer, setTimeout, clearTimeout, ...extra};
  context.window = context;
  vm.runInNewContext(fs.readFileSync('html/js/calendar_annotations.js', 'utf8'), context);
  return context.CalendarAnnotations;
}

function ack(status, crc) {
  const packet = new Uint8Array(12);
  packet[0] = 0xA8;
  packet[1] = 3;
  packet[2] = status;
  new DataView(packet.buffer).setUint32(8, crc, true);
  return packet;
}

test('durable annotation save accepts only matching business CRC', async () => {
  const annotations = load();
  const bytes = annotations.pack();
  const expected = annotations.crc32(bytes);
  const completion = annotations.waitForCommit(bytes);
  assert.equal(annotations.handleNotification(ack(0, expected ^ 1)), true);
  assert.ok(annotations._commitWaiter);
  assert.equal(annotations.handleNotification(ack(0, expected)), true);
  assert.equal(await completion, true);
});

test('device failure does not become a successful save confirmation', async () => {
  const annotations = load();
  const bytes = annotations.pack();
  const completion = annotations.waitForCommit(bytes);
  annotations.handleNotification(ack(5, annotations.crc32(bytes)));
  assert.equal(await completion, false);
});

test('late cancelled annotation request cannot settle or cancel a replacement read', async () => {
  let completeOldWrite, writes = 0;
  const annotations = load({ addLog() {}, EpdCmd: {SET_ANNOTATIONS: 0x24},
    write: () => ++writes === 1 ? new Promise(resolve => { completeOldWrite = resolve; }) : Promise.resolve(true) });
  annotations.persist = annotations.render = () => {};
  const old = annotations._loadFromDeviceOnce(null);
  annotations.cancelLoad();
  const current = annotations._loadFromDeviceOnce(null), currentTimer = annotations._loadTimer;
  completeOldWrite(false);
  assert.equal(await old, false);
  assert.equal(annotations._loadTimer, currentTimer);
  assert.equal(typeof annotations._resolveLoad, 'function');
  const payload = annotations.pack();
  annotations.handleNotification(Uint8Array.of(0xa2, 0, 0, payload.length, 0, ...payload));
  assert.equal(await current, true);
  assert.equal(annotations._resolveLoad, null);
  assert.equal(annotations._loadTimer, null);
  annotations.annotations = [{ date: '2026-10-01', label: 'local' }];
  annotations.handleNotification(Uint8Array.of(0xa2, 0, 0, payload.length, 0, ...payload));
  assert.equal(annotations.annotations[0].label, 'local');
});

function editorFixture(saved = [], today = null) {
  class Element {
    constructor(tag = 'div') {
      this.tagName = tag.toUpperCase();
      this.children = [];
      this.value = '';
      this.hidden = false;
      this._text = '';
      this._attributes = new Map();
      this._classes = new Set();
      this.listeners = new Map();
      this.classList = {
        toggle: (name, enabled) => enabled ? this._classes.add(name) : this._classes.delete(name),
        contains: name => this._classes.has(name),
      };
    }
    set className(value) { this._classes = new Set(value.split(/\s+/).filter(Boolean)); }
    get className() { return [...this._classes].join(' '); }
    set textContent(value) { this._text = String(value); this.children = []; }
    get textContent() { return this._text + this.children.map(child => child.textContent).join(''); }
    set innerHTML(value) {
      assert.equal(value, '', 'list rendering must not interpolate HTML');
      this._text = '';
      this.children = [];
    }
    appendChild(child) { this.children.push(child); return child; }
    setAttribute(name, value) { this._attributes.set(name, String(value)); }
    getAttribute(name) { return this._attributes.get(name) ?? null; }
    addEventListener(name, handler) {
      const handlers = this.listeners.get(name) || [];
      handlers.push(handler);
      this.listeners.set(name, handlers);
    }
    dispatch(name, extra = {}) {
      for (const handler of this.listeners.get(name) || []) handler({ type: name, ...extra });
    }
    focus() { this.focused = true; }
  }
  const ids = [
    'calendarAnnotationDate', 'calendarAnnotationLabel', 'calendarAnnotationFontSize',
    'calendarAnnotationColor', 'calendarAnnotationPosition', 'calendarAnnotationLabelHint',
    'calendarAnnotationsList', 'calendarAnnotationCount', 'calendarAnnotationsSummary',
    'calendarAnnotationEditorTitle', 'calendarAnnotationEditorNote', 'calendarAnnotationSubmit',
    'calendarAnnotationCancel', 'calendarAnnotationStatus', 'calendarAnnotationPreview',
    'calendarAnnotationPreviewDate', 'calendarAnnotationPreviewMonth', 'calendarAnnotationPreviewWeekday',
    'calendarAnnotationPreviewLabel', 'calendarAnnotationPreviewDot', 'calendarAnnotationPreviewLunar',
    'calendarAnnotationPreviewNote', 'calendarAnnotationColorHint',
  ];
  const elements = Object.fromEntries(ids.map(id => [id, new Element()]));
  ['calendarAnnotationFontSize', 'calendarAnnotationColor', 'calendarAnnotationPosition'].forEach(id => {
    elements[id].value = '0';
  });
  const values = new Map([['epd_calendar_annotations_v1', JSON.stringify(saved)]]);
  const logs = [];
  const annotations = load({
    document: { getElementById: id => elements[id] || null, createElement: tag => new Element(tag) },
    localStorage: { getItem: key => values.get(key) || null, setItem: (key, value) => values.set(key, value) },
    addLog: message => logs.push(message),
  });
  if (today) annotations.today = () => today;
  annotations.init();
  const field = (id, value, event = 'change') => {
    elements[id].value = value;
    elements[id].dispatch(event);
  };
  return { annotations, elements, values, logs, field };
}

test('local date validation rejects impossible and out-of-range dates without touching the list', () => {
  const { annotations, elements, field } = editorFixture();
  for (const date of ['', '2026-02-29', '2026-04-31', '2026-13-01', '1999-12-31', '2100-01-01', '2026-1-01']) {
    field('calendarAnnotationDate', date);
    annotations.addOrUpdate();
    assert.equal(annotations.annotations.length, 0, date);
    assert.ok(elements.calendarAnnotationStatus.classList.contains('error'));
  }
  assert.equal(annotations.dateParts('2000-02-29').day, 29);
  assert.equal(annotations.dateParts('2028-02-29').weekday, '周二');
  assert.equal(annotations.dateParts('2099-12-31').day, 31);
  field('calendarAnnotationDate', '2028-02-29');
  field('calendarAnnotationLabel', '休', 'input');
  annotations.addOrUpdate();
  assert.equal(annotations.annotations.length, 1);
  assert.equal(elements.calendarAnnotationStatus.classList.contains('error'), false);
});

test('new editor starts with today and a four-column empty list without a fabricated lunar date', () => {
  const { annotations, elements } = editorFixture();
  assert.equal(elements.calendarAnnotationDate.value, annotations.today());
  assert.equal(elements.calendarAnnotationCount.textContent, '0 / 64');
  assert.equal(elements.calendarAnnotationsSummary.textContent, '共 0 条');
  assert.equal(elements.calendarAnnotationsList.children[0].children[0].colSpan, 4);
  assert.equal(elements.calendarAnnotationPreviewLabel.hidden, true);
  assert.equal(elements.calendarAnnotationPreviewDot.hidden, false);
  assert.equal(elements.calendarAnnotationPreviewLunar.hidden, false);
  assert.equal(elements.calendarAnnotationCancel.hidden, true);
});

test('preview follows firmware rules for below labels, corner labels and dot-only annotations', () => {
  const { elements, field } = editorFixture();
  const preview = elements.calendarAnnotationPreview;
  field('calendarAnnotationDate', '2026-10-05');
  field('calendarAnnotationLabel', '休班', 'input');
  assert.equal(elements.calendarAnnotationPreviewDate.textContent, '05');
  assert.equal(elements.calendarAnnotationPreviewMonth.textContent, '2026 年 10 月');
  assert.equal(elements.calendarAnnotationPreviewWeekday.textContent, '星期一');
  assert.equal(elements.calendarAnnotationPreviewLabel.textContent, '休班');
  assert.equal(elements.calendarAnnotationPreviewLabel.hidden, false);
  assert.equal(elements.calendarAnnotationPreviewLunar.hidden, true);
  assert.equal(elements.calendarAnnotationPreviewDot.hidden, false);
  field('calendarAnnotationPosition', '1');
  assert.equal(preview.getAttribute('data-position'), 'corner');
  assert.equal(elements.calendarAnnotationPreviewLunar.hidden, false);
  assert.equal(elements.calendarAnnotationPreviewDot.hidden, true);
  field('calendarAnnotationLabel', '', 'input');
  assert.equal(elements.calendarAnnotationPreviewLabel.hidden, true);
  assert.equal(elements.calendarAnnotationPreviewLunar.hidden, false);
  assert.equal(elements.calendarAnnotationPreviewDot.hidden, false);
  assert.match(elements.calendarAnnotationPreviewNote.textContent, /只显示红点/);
});

test('preview reflects font and ink choices and explains low-contrast white text', () => {
  const { elements, field } = editorFixture([], '2026-10-04');
  field('calendarAnnotationDate', '2026-10-05');
  field('calendarAnnotationLabel', '休', 'input');
  for (const [value, name] of [['0', 'follow'], ['9', 'small'], ['12', 'large']]) {
    field('calendarAnnotationFontSize', value);
    assert.equal(elements.calendarAnnotationPreview.getAttribute('data-font'), name);
  }
  for (const [value, name] of [['0', 'black'], ['1', 'white'], ['2', 'red']]) {
    field('calendarAnnotationColor', value);
    assert.equal(elements.calendarAnnotationPreview.getAttribute('data-color'), name);
    assert.equal(elements.calendarAnnotationPreviewNote.textContent.includes('不易辨认'), value === '1');
  }
  field('calendarAnnotationDate', '');
  assert.equal(elements.calendarAnnotationPreviewDate.textContent, '—');
  assert.equal(elements.calendarAnnotationPreviewMonth.textContent, '请选择日期');
});

test('today red below labels warn without changing the selected color or saved record', () => {
  const { annotations, elements, values, field } = editorFixture([], '2026-10-05');
  field('calendarAnnotationLabel', '休', 'input');
  field('calendarAnnotationColor', '2');
  assert.equal(elements.calendarAnnotationPreview.getAttribute('data-today'), 'true');
  assert.equal(elements.calendarAnnotationColorHint.hidden, false);
  assert.match(elements.calendarAnnotationColorHint.textContent, /当日.*不要选择红色/);
  assert.match(elements.calendarAnnotationPreviewNote.textContent, /红字.*背景重合/);
  assert.equal(elements.calendarAnnotationColor.value, '2');
  annotations.addOrUpdate();
  assert.equal(annotations.annotations[0].color, 2);
  assert.equal(JSON.parse(values.get(annotations.STORAGE_KEY))[0].color, 2);
  field('calendarAnnotationColor', '0');
  assert.equal(elements.calendarAnnotationColorHint.hidden, true);
  assert.equal(elements.calendarAnnotationColorHint.textContent, '');
});

test('red warning excludes corner labels, dot-only annotations and other dates', () => {
  const { elements, field } = editorFixture([], '2026-10-05');
  field('calendarAnnotationLabel', '班', 'input');
  field('calendarAnnotationColor', '2');
  assert.equal(elements.calendarAnnotationColorHint.hidden, false);
  field('calendarAnnotationPosition', '1');
  assert.equal(elements.calendarAnnotationColorHint.hidden, true);
  assert.equal(elements.calendarAnnotationPreviewNote.textContent.includes('背景重合'), false);
  field('calendarAnnotationPosition', '0');
  field('calendarAnnotationLabel', '', 'input');
  assert.equal(elements.calendarAnnotationColorHint.hidden, true);
  assert.equal(elements.calendarAnnotationPreviewDot.hidden, false);
  field('calendarAnnotationLabel', '休', 'input');
  for (const date of ['2026-10-04', '2026-10-06', '2026-11-05', '2027-10-05', '']) {
    field('calendarAnnotationDate', date);
    assert.equal(elements.calendarAnnotationColorHint.hidden, true, date);
    assert.equal(elements.calendarAnnotationPreview.getAttribute('data-today'), 'false', date);
    assert.equal(elements.calendarAnnotationPreviewNote.textContent.includes('背景重合'), false, date);
  }
});

test('white below text on today red background is readable while white corner and other dates retain hints', () => {
  const { elements, field } = editorFixture([], '2026-10-05');
  field('calendarAnnotationLabel', '休', 'input');
  field('calendarAnnotationColor', '1');
  assert.equal(elements.calendarAnnotationColorHint.hidden, true);
  assert.equal(elements.calendarAnnotationPreviewNote.textContent.includes('不易辨认'), false);
  field('calendarAnnotationPosition', '1');
  assert.equal(elements.calendarAnnotationPreviewNote.textContent.includes('不易辨认'), true);
  field('calendarAnnotationPosition', '0');
  field('calendarAnnotationDate', '2026-10-06');
  assert.equal(elements.calendarAnnotationPreviewNote.textContent.includes('不易辨认'), true);
});

test('preview follows device small-font fallback for ASCII letters while large digits and Chinese remain large', () => {
  const { elements, field } = editorFixture();
  field('calendarAnnotationFontSize', '12');
  field('calendarAnnotationLabel', 'A休', 'input');
  assert.equal(elements.calendarAnnotationFontSize.value, '12');
  assert.equal(elements.calendarAnnotationPreview.getAttribute('data-font'), 'small');
  assert.match(elements.calendarAnnotationPreviewNote.textContent, /英文字母.*小字号/);
  field('calendarAnnotationLabel', '123456789', 'input');
  assert.equal(elements.calendarAnnotationPreview.getAttribute('data-font'), 'large');
  assert.equal(elements.calendarAnnotationPreviewNote.textContent.includes('英文字母'), false);
  field('calendarAnnotationLabel', '春节休', 'input');
  assert.equal(elements.calendarAnnotationPreview.getAttribute('data-font'), 'large');
  assert.equal(elements.calendarAnnotationPreviewNote.textContent.includes('英文字母'), false);
  field('calendarAnnotationFontSize', '0');
  field('calendarAnnotationLabel', 'x班', 'input');
  assert.equal(elements.calendarAnnotationPreview.getAttribute('data-font'), 'small');
});

test('IME composition is preserved until completion then clamped at a UTF-8 character boundary', () => {
  const { annotations, elements } = editorFixture();
  const label = elements.calendarAnnotationLabel;
  label.dispatch('compositionstart');
  label.value = '春节休班';
  label.dispatch('input', { isComposing: true });
  assert.equal(label.value, '春节休班');
  annotations.addOrUpdate();
  assert.equal(annotations.annotations.length, 0);
  assert.match(elements.calendarAnnotationStatus.textContent, /完成标签输入/);
  label.dispatch('compositionend');
  assert.equal(label.value, '春节休');
  assert.match(elements.calendarAnnotationLabelHint.textContent, /已用 9\/9 字节/);
  annotations.addOrUpdate();
  assert.equal(annotations.annotations[0].label, '春节休');
  label.value = '12休班AB';
  label.dispatch('input');
  assert.equal(label.value, '12休班A');
});

test('editing another date keeps date-based upsert semantics and cancellation preserves the list', () => {
  const { annotations, elements, values, field } = editorFixture([
    { date: '2026-10-01', label: '休', fontSize: 0, color: 0, position: 0 },
    { date: '2026-10-02', label: '班', fontSize: 9, color: 2, position: 1 },
  ]);
  annotations.editAt(0);
  assert.equal(annotations._editingDate, '2026-10-01');
  assert.equal(elements.calendarAnnotationSubmit.textContent, '更新标注');
  assert.ok(elements.calendarAnnotationsList.children[0].classList.contains('editing'));
  field('calendarAnnotationDate', '2026-10-03');
  field('calendarAnnotationLabel', '春节', 'input');
  assert.match(elements.calendarAnnotationEditorNote.textContent, /原日期 2026-10-01 保留/);
  annotations.addOrUpdate();
  assert.deepEqual(Array.from(annotations.annotations, item => item.date), ['2026-10-01', '2026-10-02', '2026-10-03']);
  annotations.editAt(0);
  field('calendarAnnotationDate', '2026-10-02');
  field('calendarAnnotationLabel', '休班', 'input');
  assert.match(elements.calendarAnnotationEditorNote.textContent, /将更新/);
  annotations.addOrUpdate();
  assert.equal(annotations.annotations.length, 3);
  assert.equal(annotations.annotations[0].label, '休');
  assert.equal(annotations.annotations[1].label, '休班');
  assert.equal(JSON.parse(values.get(annotations.STORAGE_KEY))[1].label, '休班');
  const before = JSON.stringify(annotations.annotations);
  field('calendarAnnotationLabel', '元旦', 'input');
  annotations.cancelEdit();
  assert.equal(JSON.stringify(annotations.annotations), before);
  assert.equal(annotations._editingDate, null);
  assert.equal(elements.calendarAnnotationLabel.value, '');
  assert.equal(elements.calendarAnnotationDate.value, annotations.today());
  assert.equal(elements.calendarAnnotationFontSize.value, '0');
  assert.equal(elements.calendarAnnotationColor.value, '0');
  assert.equal(elements.calendarAnnotationPosition.value, '0');
  assert.equal(elements.calendarAnnotationCancel.hidden, true);
});

test('removing a different row and rendering device data preserves an in-progress local draft', () => {
  const { annotations, elements, field } = editorFixture([
    { date: '2026-10-01', label: '休', fontSize: 0, color: 0, position: 0 },
    { date: '2026-10-02', label: '班', fontSize: 9, color: 2, position: 1 },
  ]);
  annotations.editAt(0);
  field('calendarAnnotationLabel', '元旦', 'input');
  annotations.removeAt(1);
  assert.equal(elements.calendarAnnotationLabel.value, '元旦');
  assert.equal(elements.calendarAnnotationDate.value, '2026-10-01');
  assert.equal(annotations._editingDate, '2026-10-01');
  annotations.render();
  assert.equal(elements.calendarAnnotationLabel.value, '元旦');
  annotations.removeAt(0);
  assert.equal(elements.calendarAnnotationLabel.value, '元旦');
  assert.equal(annotations._editingDate, null);
  assert.equal(elements.calendarAnnotationSubmit.textContent, '添加标注');
});

test('list entries use text nodes, four cells and named edit and delete buttons', () => {
  const malicious = '<img src=x onerror=alert(1)>';
  const { annotations, elements } = editorFixture([
    { date: '2026-10-05', label: malicious, fontSize: 12, color: 2, position: 1 },
  ]);
  const row = elements.calendarAnnotationsList.children[0];
  assert.equal(row.children.length, 4);
  assert.equal(row.children[0].textContent, '2026-10-05周一');
  assert.equal(row.children[1].children[0].textContent, malicious);
  assert.equal(row.children[1].children[0].children.length, 0);
  const chips = row.children[2].children;
  assert.deepEqual(chips.map(chip => chip.textContent), ['右上角', '大', '红字']);
  assert.equal(chips[2].getAttribute('data-color'), '2');
  const [edit, remove] = row.children[3].children;
  assert.equal(edit.type, 'button');
  assert.match(edit.getAttribute('aria-label'), /编辑 2026-10-05/);
  assert.equal(remove.type, 'button');
  assert.equal(remove.classList.contains('danger'), true);
  edit.onclick({ stopPropagation() {} });
  assert.equal(annotations._editingDate, '2026-10-05');
  remove.onclick({ stopPropagation() {} });
  assert.equal(annotations.annotations.length, 0);
});

test('label failures and record limits report locally without replacing existing records', () => {
  const { annotations, elements, field } = editorFixture();
  field('calendarAnnotationDate', '2026-10-05');
  field('calendarAnnotationLabel', '生', 'input');
  annotations.addOrUpdate();
  assert.equal(annotations.annotations.length, 0);
  assert.match(elements.calendarAnnotationStatus.textContent, /不在机内日历字库/);
  field('calendarAnnotationLabel', '休', 'input');
  annotations.annotations = Array.from({ length: 64 }, (_, index) => ({
    date: `2026-${String(1 + Math.floor(index / 28)).padStart(2, '0')}-${String(1 + index % 28).padStart(2, '0')}`,
    label: '班', fontSize: 0, color: 0, position: 0,
  }));
  annotations.addOrUpdate();
  assert.equal(annotations.annotations.length, 64);
  assert.match(elements.calendarAnnotationStatus.textContent, /最多保存 64 条/);
  field('calendarAnnotationDate', '2026-01-01');
  annotations.addOrUpdate();
  assert.equal(annotations.annotations.length, 64);
  assert.equal(annotations.annotations[0].label, '休');
  assert.equal(elements.calendarAnnotationStatus.classList.contains('error'), false);
});
