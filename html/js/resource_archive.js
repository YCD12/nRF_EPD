(function (root, factory) {
  'use strict';
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = factory(require('./resource_store.js'));
  } else {
    root.EPDResourceArchive = factory(root.EPDResourceStore);
  }
})(typeof globalThis !== 'undefined' ? globalThis : this, function (store) {
  'use strict';

  const MAX_UINT32 = 0xffffffff;
  const DOS_DATE = 0x0021; // 1980-01-01; backup time belongs in the manifest.

  function createZip(entries) {
    if (!store || typeof store.crc32 !== 'function') throw new Error('资源校验模块未加载');
    if (!Array.isArray(entries) || entries.length > 0xffff) throw new Error('ZIP 文件列表无效或过大');
    const names = new Set();
    let localSize = 0, directorySize = 0;
    const files = Array.from(entries, entry => {
      const name = entry && entry.name;
      if (typeof name !== 'string' || !name.trim() || name === '.' || name === '..' ||
          !/^[\x20-\x7e]+$/.test(name) || /[\\/:]/.test(name) || name.length > 0xffff) {
        throw new Error('ZIP 文件名必须是单个 ASCII 文件名');
      }
      if (names.has(name)) throw new Error(`ZIP 文件名重复：${name}`);
      names.add(name);
      if (!(entry.bytes instanceof Uint8Array) || entry.bytes.length > MAX_UINT32) {
        throw new Error(`ZIP 文件数据无效：${name}`);
      }
      const filename = Uint8Array.from(name, char => char.charCodeAt(0));
      const file = { filename, bytes: entry.bytes, crc: store.crc32(entry.bytes), offset: localSize };
      localSize += 30 + filename.length + entry.bytes.length;
      directorySize += 46 + filename.length;
      if (localSize + directorySize + 22 > MAX_UINT32) throw new Error('ZIP 过大，超出 ZIP32 容量');
      return file;
    });

    const output = new Uint8Array(localSize + directorySize + 22);
    const view = new DataView(output.buffer);
    for (const file of files) {
      const at = file.offset;
      view.setUint32(at, 0x04034b50, true);
      view.setUint16(at + 4, 20, true);
      view.setUint16(at + 12, DOS_DATE, true);
      view.setUint32(at + 14, file.crc, true);
      view.setUint32(at + 18, file.bytes.length, true);
      view.setUint32(at + 22, file.bytes.length, true);
      view.setUint16(at + 26, file.filename.length, true);
      output.set(file.filename, at + 30);
      output.set(file.bytes, at + 30 + file.filename.length);
    }

    let at = localSize;
    for (const file of files) {
      view.setUint32(at, 0x02014b50, true);
      view.setUint16(at + 4, 20, true);
      view.setUint16(at + 6, 20, true);
      view.setUint16(at + 14, DOS_DATE, true);
      view.setUint32(at + 16, file.crc, true);
      view.setUint32(at + 20, file.bytes.length, true);
      view.setUint32(at + 24, file.bytes.length, true);
      view.setUint16(at + 28, file.filename.length, true);
      view.setUint32(at + 42, file.offset, true);
      output.set(file.filename, at + 46);
      at += 46 + file.filename.length;
    }

    view.setUint32(at, 0x06054b50, true);
    view.setUint16(at + 8, files.length, true);
    view.setUint16(at + 10, files.length, true);
    view.setUint32(at + 12, directorySize, true);
    view.setUint32(at + 16, localSize, true);
    return output;
  }

  return { createZip };
});
