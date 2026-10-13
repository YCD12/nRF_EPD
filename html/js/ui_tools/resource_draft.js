(function (root) {
  'use strict';
  const node = typeof module !== 'undefined' && module.exports;
  const project = node ? require('./resource_project.js') : root.EPDResourceProject;
  const store = node ? require('../resource_store.js') : root.EPDResourceStore;
  const KEY = 'epd-resource-draft-v1', BACKEND = KEY + '-backend', FONT_PREFIX = KEY + '-font-', GENERATION = KEY + '-assets-generation';
  const REVISION = '_draftRevision', DELETED = '_draftDeleted', LOCK = KEY + '-commit';
  function create(environment = root) {
    let database, fallbackReason = '', backend = '', lastRevision = 0, queue = Promise.resolve();
    let knownGeneration = null;
    const fingerprints = new WeakMap(), persisted = new Set();
    function storage() { return environment.localStorage; }
    const revision = record => Number.isSafeInteger(record?.[REVISION]) ? record[REVISION] : 0;
    function localRecord() {
      const saved = storage()?.getItem(KEY);
      return saved ? decode(saved) : undefined;
    }
    function nextRevision(...records) {
      lastRevision = Math.max(Date.now(), lastRevision, ...records.map(revision)) + 1;
      return lastRevision;
    }
    function publicSnapshot(record) {
      if (!record || record[DELETED]) return null;
      const snapshot = { ...record }; delete snapshot[REVISION]; delete snapshot[DELETED];
      return snapshot;
    }
    function schedule(operation) {
      const task = queue.then(() => environment.navigator?.locks?.request
        ? environment.navigator.locks.request(LOCK, operation) : operation());
      queue = task.catch(() => {}); return task;
    }
    function db() {
      if (!database) database = new Promise((resolve, reject) => {
        let finished = false;
        const request = environment.indexedDB.open(KEY, 2);
        request.onupgradeneeded = () => {
          for (const name of ['drafts', 'fonts']) if (!request.result.objectStoreNames.contains(name)) request.result.createObjectStore(name);
        };
        request.onsuccess = () => {
          const opened = request.result;
          if (finished) { opened.close(); return; }
          finished = true;
          opened.onversionchange = () => { opened.close(); database = null; persisted.clear(); };
          resolve(opened);
        };
        request.onerror = request.onblocked = () => {
          finished = true; reject(request.error || new Error('草稿存储暂时不可用'));
        };
      }).catch(error => { database = null; throw error; });
      return database;
    }
    async function fontKey(font) {
      if (!font) return null;
      if (!(font.bytes instanceof Uint8Array) || font.bytes.length > 32 * 1024 * 1024) throw new Error('草稿字体超过 32 MiB 或格式无效');
      if (!fingerprints.has(font.bytes)) fingerprints.set(font.bytes, (async () => {
        const crypto = environment.crypto || root.crypto;
        if (crypto?.subtle) {
          const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', font.bytes));
          return Array.from(digest, n => n.toString(16).padStart(2, '0')).join('');
        }
        return `${font.bytes.length}-${store.crc32(font.bytes).toString(16)}`;
      })());
      return fingerprints.get(font.bytes);
    }
    const encode = value => JSON.stringify(value, (_, item) => item instanceof Uint8Array ? { base64: project.encode64(item) } : item);
    const decode = value => JSON.parse(value, (_, item) => {
      if (item?.binary && Array.isArray(item.binary)) return Uint8Array.from(item.binary);
      if (item && typeof item.base64 === 'string' && Object.keys(item).length === 1) return project.decode64(item.base64, 32 * 1024 * 1024);
      return item;
    });
    function localRead(record) {
      const snapshot = publicSnapshot(record);
      if (snapshot?.font?.ref) {
        const bytes = storage().getItem(FONT_PREFIX + snapshot.font.ref);
        if (!bytes) throw new Error('草稿引用的字体丢失，请导入完整工程或恢复原字体');
        snapshot.font = { name: snapshot.font.name, bytes: project.decode64(bytes, 32 * 1024 * 1024) };
      }
      backend = 'localStorage'; return snapshot;
    }
    function localSave(snapshot, key, deleted = false) {
      const target = storage();
      if (!target) throw new Error('浏览器不支持草稿存储，请导出完整工程');
      const record = deleted ? { [DELETED]: true } : { ...snapshot };
      let previous, acknowledged;
      try { previous = localRecord(); } catch (error) { if (!deleted) throw error; }
      try { acknowledged = /^IndexedDB:(\d+)$/.exec(target.getItem(BACKEND) || ''); } catch (_) {}
      record[REVISION] = nextRevision(previous, { [REVISION]: Number(acknowledged?.[1] || 0) });
      let added = false;
      if (key) {
        if (!target.getItem(FONT_PREFIX + key)) {
          target.setItem(FONT_PREFIX + key, project.encode64(snapshot.font.bytes)); added = true;
        }
        record.font = { name: snapshot.font.name, ref: key };
      }
      try { target.setItem(KEY, encode(record)); }
      catch (error) { if (added) target.removeItem(FONT_PREFIX + key); throw error; }
      // New records select the latest committed revision, independent of this
      // legacy hint. A marker quota error can no longer hide a successful save.
      try { target.setItem(BACKEND, 'localStorage'); } catch (_) {}
      // localStorage has no transactions. Only collect while all participating
      // tabs hold the same Web Lock, including readers hydrating a font ref.
      if (environment.navigator?.locks?.request) {
        try {
          const keys = [];
          for (let i = 0; i < target.length; i++) {
            const name = target.key(i);
            if (name?.startsWith(FONT_PREFIX) && name !== FONT_PREFIX + key) keys.push(name);
          }
          for (const name of keys) target.removeItem(name);
        } catch (_) {} // A committed draft remains usable if optional cleanup fails.
      }
      backend = 'localStorage';
    }
    async function save(snapshot) {
      const key = await fontKey(snapshot.font);
      if (environment.indexedDB) {
        try {
          const opened = await db(), record = { ...snapshot };
          let local;
          try { local = localRecord(); } catch (_) {}
          if (key) record.font = { name: snapshot.font.name, ref: key };
          let generation;
          await new Promise((resolve, reject) => {
            const tx = opened.transaction(['drafts', 'fonts'], 'readwrite');
            const drafts = tx.objectStore('drafts'), assets = tx.objectStore('fonts');
            const previous = drafts.get('current'), token = drafts.get('assets-generation');
            token.onsuccess = () => {
              generation = token.result || 0;
              if (generation !== knownGeneration) persisted.clear();
              if (key && !persisted.has(key)) assets.put(snapshot.font.bytes, key);
              record[REVISION] = nextRevision(previous.result, local);
              drafts.put(record, 'current');
              // Cursor keys avoid cloning large font buffers just to collect
              // them. GC shares the draft transaction, so another tab cannot
              // observe a committed ref whose asset has already been removed.
              if (assets.openKeyCursor) {
                const cursor = assets.openKeyCursor();
                cursor.onsuccess = () => {
                  const item = cursor.result;
                  if (!item) return;
                  if (item.key !== key) {
                    assets.delete(item.key);
                    generation = record[REVISION]; drafts.put(generation, 'assets-generation');
                  }
                  item.continue();
                };
              }
            };
            tx.oncomplete = resolve;
            tx.onerror = tx.onabort = () => reject(tx.error || new Error('草稿存储失败'));
          });
          knownGeneration = generation;
          persisted.clear();
          if (key) persisted.add(key); backend = 'IndexedDB'; fallbackReason = '';
          // This hint also prevents restoring a known older fallback while
          // the database is temporarily offline. It never decides which of
          // two available records is newer.
          try { storage()?.setItem(BACKEND, 'IndexedDB:' + record[REVISION]); } catch (_) {}
          // Removing the fallback and its fonts is safe only while other tabs
          // cannot commit or hydrate a local snapshot concurrently.
          if (environment.navigator?.locks?.request) {
            try {
              const target = storage();
              if (target && revision(localRecord()) <= record[REVISION]) {
                target.removeItem(KEY);
                const keys = [];
                for (let i = 0; i < target.length; i++) if (target.key(i)?.startsWith(FONT_PREFIX)) keys.push(target.key(i));
                for (const name of keys) target.removeItem(name);
              }
            } catch (_) {}
          }
          return;
        } catch (error) { fallbackReason = error.message; }
      }
      await localSave(snapshot, key);
    }
    async function read() {
      let marker, local, saved, localAvailable = true, databaseRecord;
      try { marker = storage()?.getItem(BACKEND); saved = storage()?.getItem(KEY); }
      catch (error) { localAvailable = false; if (!environment.indexedDB) throw error; }
      if (saved) local = decode(saved);
      // A corrupt local record cannot safely be ranked against the database.
      // Keep the restore failure visible instead of silently loading old data.
      if (!localAvailable && marker === 'localStorage') throw new Error('备用草稿读取失败，请导出或恢复原草稿');
      if (environment.indexedDB) {
        try {
          const opened = await db();
          const record = await new Promise((resolve, reject) => {
            const tx = opened.transaction(['drafts', 'fonts'], 'readonly');
            const request = tx.objectStore('drafts').get('current');
            const token = tx.objectStore('drafts').get('assets-generation');
            let result;
            request.onsuccess = () => {
              result = request.result;
              databaseRecord = result;
              if (result?.font?.ref && !(revision(local) > revision(result) || (!revision(result) && marker === 'localStorage'))) {
                const ref = result.font.ref, asset = tx.objectStore('fonts').get(ref);
                asset.onsuccess = () => {
                  if (!(asset.result instanceof Uint8Array)) { tx.abort(); return; }
                  result.font = { name: result.font.name, bytes: asset.result };
                };
              }
            };
            tx.oncomplete = () => {
              const generation = token.result || 0;
              if (generation !== knownGeneration) persisted.clear();
              knownGeneration = generation;
              resolve(result);
            };
            tx.onerror = tx.onabort = () => reject(tx.error || new Error('草稿读取失败或引用的字体丢失'));
          });
          lastRevision = Math.max(lastRevision, revision(record), revision(local));
          if (localAvailable && (revision(local) > revision(record) ||
            (!revision(record) && marker === 'localStorage') || record === undefined)) {
            if (local === undefined && marker === 'localStorage') throw new Error('备用草稿丢失，未覆盖原数据库草稿');
            return localRead(local);
          }
          backend = 'IndexedDB'; return publicSnapshot(record);
        } catch (error) {
          fallbackReason = error.message;
          if (!localAvailable) throw error;
          const acknowledged = /^IndexedDB:(\d+)$/.exec(marker || '');
          if (revision(databaseRecord) > revision(local) || (acknowledged && Number(acknowledged[1]) > revision(local))) throw error;
          if (local === undefined) throw error; // Failed reads are never treated as an empty draft.
          lastRevision = Math.max(lastRevision, revision(local));
          return localRead(local);
        }
      }
      lastRevision = Math.max(lastRevision, revision(local));
      return localRead(local);
    }
    async function clear() {
      let failedDatabase;
      if (environment.indexedDB) {
        try {
          const opened = await db();
          let local;
          try { local = localRecord(); } catch (_) {}
          await new Promise((resolve, reject) => {
            const tx = opened.transaction(['drafts', 'fonts'], 'readwrite');
            const drafts = tx.objectStore('drafts'), previous = drafts.get('current');
            previous.onsuccess = () => {
              const value = nextRevision(previous.result, local);
              drafts.clear(); tx.objectStore('fonts').clear();
              drafts.put({ [REVISION]: value, [DELETED]: true }, 'current');
              drafts.put(value, 'assets-generation');
            };
            tx.oncomplete = resolve; tx.onerror = tx.onabort = () => reject(tx.error || new Error('草稿清除失败'));
          });
        } catch (error) { failedDatabase = error; }
      }
      let target;
      try { target = storage(); } catch (error) { if (failedDatabase || !environment.indexedDB) throw failedDatabase || error; }
      if (failedDatabase && !target) throw failedDatabase;
      if (target) {
        if (failedDatabase) {
          // Remember an explicit clear even while the database is offline,
          // so an older database draft cannot reappear on the next visit.
          localSave(null, null, true);
        }
        const keys = [];
        for (let i = 0; i < target.length; i++) if (target.key(i)?.startsWith(FONT_PREFIX)) keys.push(target.key(i));
        for (const key of keys) target.removeItem(key);
        target.removeItem(GENERATION);
        if (!failedDatabase) { target.removeItem(KEY); target.removeItem(BACKEND); }
      }
      persisted.clear();
    }
    return { save: snapshot => schedule(() => save(snapshot)), read: () => schedule(read),
      clear: () => schedule(clear), status: () => ({ backend, fallbackReason }) };
  }
  const api = { create };
  if (typeof module !== 'undefined' && module.exports) module.exports = api; else root.EPDResourceDraft = api;
})(globalThis);
