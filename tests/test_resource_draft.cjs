const { test } = require('node:test');
const assert = require('node:assert/strict');
const drafts = require('../html/js/ui_tools/resource_draft.js');
const KEY = 'epd-resource-draft-v1', BACKEND = KEY + '-backend', FONT_PREFIX = KEY + '-font-';

function localFixture() {
  const values = new Map();
  return { values, get length() { return values.size; }, key: index => [...values.keys()][index],
    getItem: key => values.get(key) || null, setItem: (key, value) => values.set(key, value),
    removeItem: key => values.delete(key) };
}
function databaseFixture() {
  const stores = new Map(), opened = { close() {}, objectStoreNames: { contains: name => stores.has(name) },
    createObjectStore: name => stores.set(name, new Map()) };
  let offline = false;
  opened.transaction = () => {
    if (offline) throw new Error('database offline');
    let pending = 0, aborted = false;
    const tx = { abort() { aborted = true; queueMicrotask(() => tx.onabort?.()); } };
    const request = action => {
      const item = {}; pending++;
      queueMicrotask(() => {
        if (aborted) return;
        item.result = action(); item.onsuccess?.();
        if (!--pending) queueMicrotask(() => { if (!aborted) tx.oncomplete?.(); });
      });
      return item;
    };
    tx.objectStore = name => {
      const values = stores.get(name);
      return { get: key => request(() => structuredClone(values.get(key))),
        put: (value, key) => request(() => values.set(key, structuredClone(value))),
        delete: key => request(() => values.delete(key)), clear: () => request(() => values.clear()),
        openKeyCursor() {
          const item = {}; pending++;
          // Read keys after earlier queued put requests, as an IDB cursor does.
          queueMicrotask(() => {
            const keys = [...values.keys()]; let position = 0;
            const emit = () => {
              if (aborted) return;
              item.result = position < keys.length ? { key: keys[position++], continue: () => queueMicrotask(emit) } : null;
              item.onsuccess?.();
              if (!item.result && !--pending) queueMicrotask(() => { if (!aborted) tx.oncomplete?.(); });
            };
            emit();
          });
          return item;
        } };
    };
    return tx;
  };
  return { stores, fail: value => { offline = value; }, indexedDB: { open() {
    const request = { result: opened }; queueMicrotask(() => { request.onupgradeneeded?.(); request.onsuccess?.(); }); return request;
  } } };
}
function lockFixture() {
  let queue = Promise.resolve();
  return { request(name, callback) { assert.equal(name, KEY + '-commit');
    const operation = queue.then(callback); queue = operation.catch(() => {}); return operation; } };
}
const snapshot = (text, font) => ({ version: 1, values: { text }, ...(font ? { font: { name: text + '.ttf', bytes: font } } : {}) });

test('latest fallback survives a failed legacy marker write after IndexedDB recovers', async () => {
  const db = databaseFixture(), localStorage = localFixture(), environment = { indexedDB: db.indexedDB, localStorage };
  const draft = drafts.create(environment); await draft.save(snapshot('old DB'));
  db.fail(true); const set = localStorage.setItem;
  localStorage.setItem = (key, value) => { if (key === BACKEND) throw new Error('marker quota'); set(key, value); };
  await draft.save(snapshot('new local')); assert.equal(draft.status().backend, 'localStorage');
  db.fail(false); assert.deepEqual(await drafts.create(environment).read(), snapshot('new local'));
});

test('a failed backend hint and cleanup cannot hide a newer database draft', async () => {
  const db = databaseFixture(), localStorage = localFixture(), environment = { indexedDB: db.indexedDB, localStorage,
    navigator: { locks: lockFixture() } };
  db.fail(true); const draft = drafts.create(environment); await draft.save(snapshot('old local'));
  const set = localStorage.setItem;
  localStorage.setItem = (key, value) => { if (key === BACKEND) throw new Error('hint denied'); set(key, value); };
  localStorage.removeItem = () => { throw new Error('cleanup denied'); };
  db.fail(false); await draft.save(snapshot('new DB'));
  assert.equal(localStorage.getItem(BACKEND), 'localStorage');
  assert.deepEqual(await drafts.create(environment).read(), snapshot('new DB'));
});

test('same-millisecond saves rank by sequence and preserve legacy marker-based drafts', async () => {
  const db = databaseFixture(), localStorage = localFixture(), environment = { indexedDB: db.indexedDB, localStorage };
  const draft = drafts.create(environment); await draft.read();
  db.stores.get('drafts').set('current', snapshot('legacy DB'));
  localStorage.setItem(KEY, JSON.stringify(snapshot('legacy local'))); localStorage.setItem(BACKEND, 'localStorage');
  assert.deepEqual(await draft.read(), snapshot('legacy local'));
  const now = Date.now; Date.now = () => 1;
  try {
    await draft.save(snapshot('first')); const first = db.stores.get('drafts').get('current')._draftRevision;
    db.fail(true); await draft.save(snapshot('second'));
    const second = JSON.parse(localStorage.getItem(KEY))._draftRevision; assert.ok(second > first);
    db.fail(false); await drafts.create(environment).save(snapshot('third'));
    assert.ok(db.stores.get('drafts').get('current')._draftRevision > second);
    assert.deepEqual(await drafts.create(environment).read(), snapshot('third'));
  } finally { Date.now = now; }
});

test('offline clear commits a tombstone even when writing the backend hint is denied', async () => {
  const db = databaseFixture(), localStorage = localFixture(), environment = { indexedDB: db.indexedDB, localStorage };
  const draft = drafts.create(environment); await draft.save(snapshot('old DB'));
  db.fail(true); const set = localStorage.setItem;
  localStorage.setItem = (key, value) => { if (key === BACKEND) throw new Error('marker denied'); set(key, value); };
  await draft.clear(); db.fail(false);
  assert.equal(await drafts.create(environment).read(), null);
});

test('a fresh offline instance advances the acknowledged database revision after the clock moves backward', async () => {
  const db = databaseFixture(), localStorage = localFixture(), environment = { indexedDB: db.indexedDB, localStorage };
  const draft = drafts.create(environment); await draft.save(snapshot('old DB'));
  const revision = db.stores.get('drafts').get('current')._draftRevision, now = Date.now;
  db.fail(true); Date.now = () => 1;
  try { await drafts.create(environment).save(snapshot('new local')); } finally { Date.now = now; }
  assert.ok(JSON.parse(localStorage.getItem(KEY))._draftRevision > revision);
  db.fail(false); assert.deepEqual(await drafts.create(environment).read(), snapshot('new local'));
});

test('explicit clear can remove a corrupt fallback while the database is offline', async () => {
  const db = databaseFixture(), localStorage = localFixture(), environment = { indexedDB: db.indexedDB, localStorage };
  const draft = drafts.create(environment); await draft.save(snapshot('old DB'));
  db.fail(true); localStorage.setItem(KEY, '{broken');
  await draft.clear(); db.fail(false);
  assert.equal(await drafts.create(environment).read(), null);
});

test('clear waits for an earlier asynchronous font fingerprint and cannot be undone by its late save', async () => {
  let release, started; const entered = new Promise(resolve => { started = resolve; });
  const localStorage = localFixture(), draft = drafts.create({ localStorage,
    crypto: { subtle: { digest: () => { started(); return new Promise(resolve => { release = resolve; }); } } } });
  const pendingSave = draft.save(snapshot('first', Uint8Array.of(1))); await entered;
  const pendingClear = draft.clear(); release(Uint8Array.of(1).buffer);
  await Promise.all([pendingSave, pendingClear]); assert.equal(await draft.read(), null);
});

test('atomic database font collection invalidates another tab cached asset reference', async () => {
  const db = databaseFixture(), environment = { indexedDB: db.indexedDB, localStorage: localFixture() };
  const first = drafts.create(environment), other = drafts.create(environment), font = Uint8Array.of(1, 2, 3);
  await first.save(snapshot('first', font)); await other.read();
  await first.save(snapshot('second', Uint8Array.of(4, 5, 6)));
  assert.equal(db.stores.get('fonts').size, 1);
  await other.save(snapshot('third', font));
  assert.equal(db.stores.get('fonts').size, 1);
  assert.deepEqual(await drafts.create(environment).read(), snapshot('third', font));
  await first.save(snapshot('no font')); assert.equal(db.stores.get('fonts').size, 0);
});

test('local font collection is guarded by cross-tab locks and stale tabs rewrite collected assets', async () => {
  const localStorage = localFixture(), environment = { localStorage, navigator: { locks: lockFixture() } };
  const first = drafts.create(environment), other = drafts.create(environment), font = Uint8Array.of(1, 2);
  await first.save(snapshot('first', font)); await other.read();
  await first.save(snapshot('second', Uint8Array.of(3, 4)));
  assert.equal([...localStorage.values.keys()].filter(key => key.startsWith(FONT_PREFIX)).length, 1);
  await other.save(snapshot('third', font));
  assert.deepEqual(await drafts.create(environment).read(), snapshot('third', font));
  assert.equal([...localStorage.values.keys()].filter(key => key.startsWith(FONT_PREFIX)).length, 1);
  const unlocked = drafts.create({ localStorage });
  await unlocked.save(snapshot('fourth', Uint8Array.of(5, 6)));
  assert.equal([...localStorage.values.keys()].filter(key => key.startsWith(FONT_PREFIX)).length, 2);
});

test('newer font corruption or an offline newer database never silently restores the older fallback', async () => {
  const db = databaseFixture(), localStorage = localFixture(), environment = { indexedDB: db.indexedDB, localStorage };
  db.fail(true); const draft = drafts.create(environment); await draft.save(snapshot('old local'));
  db.fail(false); await draft.save(snapshot('new DB', Uint8Array.of(1, 2)));
  db.fail(true); await assert.rejects(drafts.create(environment).read(), /database offline/);
  db.fail(false); db.stores.get('fonts').clear();
  await assert.rejects(drafts.create(environment).read(), /字体丢失/);
  localStorage.setItem(KEY, '{invalid JSON');
  await assert.rejects(drafts.create(environment).read(), SyntaxError);
});
