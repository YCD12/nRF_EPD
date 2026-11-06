const { test } = require('node:test');
const assert = require('node:assert/strict');
const sources = require('../html/js/ui_tools/font_sources.js');
const drafts = require('../html/js/ui_tools/resource_draft.js');

function sfnt(format = 4) {
  const length = format === 4 ? 32 : 40, bytes = new Uint8Array(28 + 12 + length), v = new DataView(bytes.buffer);
  v.setUint32(0, 0x00010000); v.setUint16(4, 1); v.setUint32(12, 0x636d6170);
  v.setUint32(20, 28); v.setUint32(24, 12 + length); v.setUint16(30, 1);
  v.setUint16(32, 3); v.setUint16(34, format === 4 ? 1 : 10); v.setUint32(36, 12);
  const at = 40; v.setUint16(at, format);
  if (format === 4) {
    v.setUint16(at + 2, length); v.setUint16(at + 6, 4);
    v.setUint16(at + 14, 90); v.setUint16(at + 16, 65535);
    v.setUint16(at + 20, 65); v.setUint16(at + 22, 65535);
    v.setInt16(at + 24, -64); v.setInt16(at + 26, 1);
  } else {
    v.setUint32(at + 4, length); v.setUint32(at + 12, 2);
    v.setUint32(at + 16, 48); v.setUint32(at + 20, 57); v.setUint32(at + 24, 1);
    v.setUint32(at + 28, 0x65b0); v.setUint32(at + 32, 0x65b0); v.setUint32(at + 36, 11);
  }
  return bytes;
}

test('TTF/OTF Unicode coverage reads cmap4 and cmap12 and excludes unmapped or unsupported characters', () => {
  const ttf = sources.coverage(sfnt());
  assert.equal(ttf(65), true); assert.equal(ttf(90), true); assert.equal(ttf(64), false); assert.equal(ttf(0xffff), false);
  const otf = sfnt(12); new DataView(otf.buffer).setUint32(0, 0x4f54544f);
  const has = sources.coverage(otf);
  assert.equal(has(48), true); assert.equal(has(57), true); assert.equal(has(58), false);
  assert.equal(has('新'.codePointAt(0)), true); assert.equal(has(0x1f600), false);
  const range = sfnt(), v = new DataView(range.buffer);
  // The idRangeOffset path points to an appended glyph-ID array, including
  // a mapped glyph-zero hole that must not be mistaken for font coverage.
  const expanded = new Uint8Array(range.length + 52); expanded.set(range);
  const ev = new DataView(expanded.buffer); ev.setUint32(24, 96); ev.setUint16(42, 84); ev.setUint16(68, 4);
  ev.setUint16(72, 7); ev.setUint16(74, 0);
  const ranged = sources.coverage(expanded); assert.equal(ranged(65), true); assert.equal(ranged(66), false);
  v.setUint32(0, 0x74746366); assert.throws(() => sources.coverage(range), /TTF.*OTF/);
});

test('malformed font table lengths and character-map ranges fail before font loading', () => {
  for (const edit of [v => v.setUint32(20, 0xffffffff), v => v.setUint32(36, 0xffffffff),
    v => v.setUint16(42, 65535), v => v.setUint16(60, 100), v => v.setUint16(68, 65534)]) {
    const bytes = sfnt(); edit(new DataView(bytes.buffer)); assert.throws(() => sources.coverage(bytes), /越界|无效|截断/);
  }
  const bytes = sfnt(12); new DataView(bytes.buffer).setUint32(52, 0xffffffff);
  assert.throws(() => sources.coverage(bytes), /长度/);
});

test('FontFace imports use local bytes, refuse fallback for missing characters, and keep the prior face on failure', async () => {
  const faces = new Set(); let fail = false;
  class Face { constructor(family, bytes) { this.family = family; this.bytes = bytes; } async load() { if (fail) throw new Error('load failed'); return this; } }
  const source = sources.create({ fonts: faces }, Face);
  const first = await source.import({ name: 'first.ttf', bytes: sfnt() });
  assert.equal(faces.size, 1); assert.equal(source.options().fontFamily, first.family);
  assert.doesNotThrow(() => source.options().validateCharacter(65));
  assert.throws(() => source.options().validateCharacter('新'.codePointAt(0)), /不包含/);
  fail = true; await assert.rejects(source.import({ name: 'bad.ttf', bytes: sfnt(12) }), /load failed/);
  assert.equal(source.get(), first); assert.ok(faces.has(first.face));
  fail = false; await source.import({ name: 'next.otf', bytes: sfnt(12) });
  assert.equal(faces.size, 1); assert.equal(faces.has(first.face), false); source.clear(); assert.equal(faces.size, 0);
});

test('draft fallback round-trips binary resources and font files and reports storage failures', async () => {
  const values = new Map(); let full = false;
  const localStorage = { getItem: key => values.get(key) || null, removeItem: key => values.delete(key),
    setItem(key, value) { if (full) throw new Error('quota'); values.set(key, value); } };
  const draft = drafts.create({ localStorage }), snapshot = { version: 1, files: [{ name: 'font.bin', bytes: Uint8Array.of(1, 2) }],
    font: { name: 'font.ttf', bytes: sfnt() }, values: { calendar: '{unfinished' } };
  assert.equal(await draft.read(), null); await draft.save(snapshot); assert.deepEqual(await draft.read(), snapshot);
  full = true; await assert.rejects(draft.save({ ...snapshot, version: 2 }), /quota/);
  assert.deepEqual(await draft.read(), snapshot); await draft.clear(); assert.equal(await draft.read(), null);
  await assert.rejects(drafts.create({}).save(snapshot), /草稿存储/);
});

function localFixture() {
  const data = new Map(), writes = [];
  return { data, writes, get length() { return data.size; }, key: index => [...data.keys()][index],
    getItem: key => data.get(key) || null, removeItem: key => data.delete(key),
    setItem(key, value) { writes.push(key); data.set(key, value); } };
}
function databaseFixture() {
  const stores = new Map(), writes = [], reads = []; let fail = false;
  const opened = { objectStoreNames: { contains: name => stores.has(name) },
    createObjectStore: name => stores.set(name, new Map()), close() {},
    transaction() {
      if (fail) throw new Error('database offline');
      let pending = 0; const tx = {};
      tx.objectStore = name => {
        const map = stores.get(name);
        const request = action => {
          const item = {}; pending++;
          queueMicrotask(() => { item.result = action(); item.onsuccess?.(); if (!--pending) queueMicrotask(() => tx.oncomplete?.()); });
          return item;
        };
        return { put: (value, key) => request(() => { writes.push(name); map.set(key, structuredClone(value)); }),
          get: key => request(() => { reads.push(name); return structuredClone(map.get(key)); }), clear: () => request(() => map.clear()) };
      };
      return tx;
    } };
  return { writes, reads, stores, fail: value => { fail = value; },
    indexedDB: { open() { const request = { result: opened }; queueMicrotask(() => { request.onupgradeneeded?.(); request.onsuccess?.(); }); return request; } } };
}

test('unavailable IndexedDB falls back to local storage and failed reads never pretend the database was empty', async () => {
  const localStorage = localFixture();
  const indexedDB = { open() { const request = { error: new Error('database denied') };
    queueMicrotask(() => request.onerror()); return request; } };
  const draft = drafts.create({ indexedDB, localStorage });
  await assert.rejects(draft.read(), /database denied/);
  const snapshot = { version: 1, values: { calendar: '{draft' }, font: { name: 'font.ttf', bytes: sfnt() } };
  await draft.save(snapshot); assert.deepEqual(await drafts.create({ indexedDB, localStorage }).read(), snapshot);
  assert.equal(draft.status().backend, 'localStorage');
  await draft.clear(); assert.equal(await drafts.create({ indexedDB, localStorage }).read(), null);
});

test('draft font assets are stored once while later saves only update editor state, and reload hydrates references', async () => {
  const db = databaseFixture(), environment = { indexedDB: db.indexedDB, localStorage: localFixture() }, draft = drafts.create(environment);
  const snapshot = { version: 1, font: { name: 'font.ttf', bytes: sfnt() }, values: { text: 'first' } };
  await draft.save(snapshot); await draft.save({ ...snapshot, values: { text: 'second' } });
  assert.equal(db.writes.filter(name => name === 'fonts').length, 1);
  const loaded = await drafts.create(environment).read(); assert.deepEqual(loaded.font, snapshot.font); assert.equal(loaded.values.text, 'second');
  assert.equal(db.stores.get('drafts').get('current').font.bytes, undefined);
  db.fail(true); await draft.save({ ...snapshot, values: { text: 'fallback latest' } });
  db.fail(false); assert.equal((await drafts.create(environment).read()).values.text, 'fallback latest');
  await draft.save({ ...snapshot, values: { text: 'database latest' } });
  assert.equal((await drafts.create(environment).read()).values.text, 'database latest');
  await draft.clear(); assert.equal(db.stores.get('fonts').size, 0);
  // Another open tab may still have the old font in its cache after clear.
  await draft.save(snapshot); const other = drafts.create(environment); await other.read();
  await draft.clear(); await other.save(snapshot);
  assert.deepEqual((await drafts.create(environment).read()).font, snapshot.font);
});

test('local draft saves reuse one font asset, preserve the old snapshot on quota failure and clean all assets', async () => {
  const localStorage = localFixture(), draft = drafts.create({ localStorage });
  const snapshot = { version: 1, font: { name: 'font.ttf', bytes: sfnt() }, values: { text: 'first' } };
  await draft.save(snapshot); await draft.save({ ...snapshot, values: { text: 'second' } });
  assert.equal(localStorage.writes.filter(key => key.includes('-font-')).length, 1);
  const saved = localStorage.setItem; localStorage.setItem = (key, value) => { if (key === 'epd-resource-draft-v1') throw new Error('quota'); saved(key, value); };
  await assert.rejects(draft.save({ ...snapshot, font: { name: 'other.ttf', bytes: sfnt(12) } }), /quota/);
  assert.deepEqual((await draft.read()).font, snapshot.font);
  localStorage.setItem = saved; await draft.clear(); assert.equal(localStorage.data.size, 0);
  await draft.save(snapshot); const other = drafts.create({ localStorage }); await other.read();
  await draft.clear(); await other.save(snapshot);
  assert.deepEqual((await drafts.create({ localStorage }).read()).font, snapshot.font);
});

test('project v2 carries source fonts, unfinished JSON and pending binaries while remaining compatible with v1', () => {
  const project = require('../html/js/ui_tools/resource_project.js'), template = require('../html/resources/default-project.json');
  const snapshot = { version: 1, files: template.files.map(f => ({ name: f.name, bytes: new Uint8Array(Buffer.from(f.base64, 'base64')) })),
    values: Object.fromEntries(project.fields.map(id => [id, ''])), bins: [Uint8Array.of(1, 2), null, null], font: { name: 'source.ttf', bytes: sfnt() } };
  snapshot.values.resourceCalendarSource = '{unfinished'; snapshot.values.resourceFontFamily = 'imported'; snapshot.values.resourceFontTarget = 'both';
  const saved = project.encode(snapshot), restored = project.decode(saved);
  assert.equal(restored.snapshot.values.resourceCalendarSource, '{unfinished'); assert.deepEqual(restored.snapshot.font, snapshot.font);
  assert.deepEqual(restored.snapshot.bins, snapshot.bins);
  assert.equal(project.decode(template).bundle.packages.length, 7);
  const noFont = { ...saved }; delete noFont.font; assert.throws(() => project.decode(noFont), /缺少.*字体/);
  assert.throws(() => project.decode({ ...saved, font: { name: 'bad.ttf', base64: 'AAAA' } }), /TTF|字体/);
  for (const value of ['A===', '=AAA', '!!!!', 'AA=A']) assert.throws(() => project.decode64(value, 8));
  const large = new Uint8Array(1024 * 1024).fill(65);
  assert.deepEqual(project.decode64(project.encode64(large), large.length), large);
});

test('preparing a replacement face does not change the active source until the caller commits it', async () => {
  const faces = new Set(); class Face { async load() { return this; } }
  const source = sources.create({ fonts: faces }, Face), first = await source.import({ name: 'first.ttf', bytes: sfnt() });
  const next = await source.prepare({ name: 'next.ttf', bytes: sfnt(12) });
  assert.equal(source.get(), first); assert.equal(faces.size, 1);
  source.commit(next); assert.equal(source.get(), next); assert.equal(faces.size, 1);
});

test('blocked local storage does not prevent an available database from creating a draft', async () => {
  const db = databaseFixture(), environment = { indexedDB: db.indexedDB,
    get localStorage() { throw new Error('local storage denied'); } };
  const draft = drafts.create(environment); assert.equal(await draft.read(), null);
  await draft.save({ version: 1, values: { text: 'database' } });
  assert.equal((await draft.read()).values.text, 'database');
  await draft.clear(); assert.equal(await draft.read(), null);
});

test('a corrupt newer fallback is reported instead of loading and overwriting an older database draft', async () => {
  const db = databaseFixture(), localStorage = localFixture(), environment = { indexedDB: db.indexedDB, localStorage };
  const draft = drafts.create(environment); await draft.save({ version: 1, values: { text: 'old database' } });
  localStorage.setItem('epd-resource-draft-v1-backend', 'localStorage'); localStorage.setItem('epd-resource-draft-v1', '{broken');
  await assert.rejects(drafts.create(environment).read());
  assert.equal(db.stores.get('drafts').get('current').values.text, 'old database');
});
