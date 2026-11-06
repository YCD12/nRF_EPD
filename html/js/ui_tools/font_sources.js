(function (root) {
  'use strict';
  const node = typeof module !== 'undefined' && module.exports;
  const store = node ? require('../resource_store.js') : root.EPDResourceStore;
  // Read the Unicode cmap before Canvas: browser font fallback would otherwise
  // turn an unsupported character into a glyph from a different font.
  function coverage(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length < 12 || bytes.length > 32 * 1024 * 1024)
      throw new Error('字体文件无效或超过 32 MiB');
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const tag = v.getUint32(0), count = v.getUint16(4);
    if (![0x00010000, 0x4f54544f].includes(tag) || !count || count > 128 || 12 + count * 16 > bytes.length)
      throw new Error('请选择独立的 TTF 或 OTF 字体文件（不支持 TTC 字体集合）');
    let cmap;
    for (let i = 0; i < count; i++) {
      const at = 12 + i * 16, offset = v.getUint32(at + 8), length = v.getUint32(at + 12);
      if (offset + length > bytes.length) throw new Error('字体表越界');
      if (v.getUint32(at) === 0x636d6170) cmap = bytes.subarray(offset, offset + length);
    }
    if (!cmap || cmap.length < 4) throw new Error('字体缺少 Unicode cmap 字符表');
    const cv = new DataView(cmap.buffer, cmap.byteOffset, cmap.byteLength), maps = [], seen = new Set();
    const n = cv.getUint16(2);
    if (4 + n * 8 > cmap.length) throw new Error('字体字符表目录越界');
    for (let i = 0; i < n; i++) {
      const at = 4 + i * 8, platform = cv.getUint16(at), encoding = cv.getUint16(at + 2), offset = cv.getUint32(at + 4);
      if (platform !== 0 && !(platform === 3 && [1, 10].includes(encoding))) continue;
      if (offset + 2 > cmap.length) throw new Error('字体字符映射越界');
      if (seen.has(offset)) continue; seen.add(offset);
      const format = cv.getUint16(offset);
      if (format === 4) {
        if (offset + 14 > cmap.length) throw new Error('字体 cmap4 截断');
        const length = cv.getUint16(offset + 2), segments = cv.getUint16(offset + 6) / 2;
        if (!Number.isInteger(segments) || !segments || length < 16 + segments * 8 || offset + length > cmap.length)
          throw new Error('字体 cmap4 长度无效');
        const end = offset + 14, start = end + segments * 2 + 2, delta = start + segments * 2, ranges = delta + segments * 2;
        let previous = -1;
        for (let j = 0; j < segments; j++) {
          const first = cv.getUint16(start + j * 2), last = cv.getUint16(end + j * 2), range = cv.getUint16(ranges + j * 2);
          if (first > last || first <= previous || range % 2 || (range && ranges + j * 2 + range + (last - first) * 2 + 2 > offset + length))
            throw new Error('字体 cmap4 区间无效');
          previous = last;
        }
        maps.push(code => {
          for (let j = 0; j < segments; j++) {
            if (code > cv.getUint16(end + j * 2)) continue;
            const first = cv.getUint16(start + j * 2); if (code < first) return false;
            const range = cv.getUint16(ranges + j * 2), change = cv.getInt16(delta + j * 2);
            const glyph = range ? cv.getUint16(ranges + j * 2 + range + (code - first) * 2) : code;
            return !!(range && !glyph ? 0 : (glyph + change) & 65535);
          }
          return false;
        });
      } else if (format === 12) {
        if (offset + 16 > cmap.length) throw new Error('字体 cmap12 截断');
        const length = cv.getUint32(offset + 4), groups = cv.getUint32(offset + 12);
        if (groups > 65536 || length < 16 + groups * 12 || offset + length > cmap.length) throw new Error('字体 cmap12 长度无效');
        let previous = -1;
        for (let j = 0; j < groups; j++) {
          const pos = offset + 16 + j * 12, first = cv.getUint32(pos), last = cv.getUint32(pos + 4);
          if (first > last || first <= previous || last > 0x10ffff) throw new Error('字体 cmap12 区间无效');
          previous = last;
        }
        maps.push(code => {
          let lo = 0, hi = groups - 1;
          while (lo <= hi) {
            const mid = lo + hi >> 1, pos = offset + 16 + mid * 12, first = cv.getUint32(pos), last = cv.getUint32(pos + 4);
            if (code < first) hi = mid - 1; else if (code > last) lo = mid + 1;
            else return cv.getUint32(pos + 8) + code - first !== 0;
          }
          return false;
        });
      }
    }
    if (!maps.length) throw new Error('字体没有支持的 Unicode cmap4/cmap12 字符表');
    return code => Number.isInteger(code) && code >= 0 && code < 0xfffe && maps.some(has => has(code));
  }
  function create(document = root.document, FontFace = root.FontFace) {
    let current = null;
    const api = {
      async prepare(file) {
        if (file.size > 32 * 1024 * 1024) throw new Error('字体文件超过 32 MiB');
        const bytes = file.bytes instanceof Uint8Array ? file.bytes.slice() : new Uint8Array(await file.arrayBuffer());
        const has = coverage(bytes), family = 'EPDImported_' + store.crc32(bytes).toString(16) + '_' + bytes.length;
        if (!FontFace || !document?.fonts) throw new Error('浏览器不支持本地字体导入');
        const face = new FontFace(family, bytes);
        await face.load();
        return { name: file.name, bytes, family: `"${family}"`, has, face, glyphCache: new Map() };
      },
      commit(prepared) {
        document.fonts.add(prepared.face);
        const previous = current;
        current = prepared;
        if (previous?.face) document.fonts.delete(previous.face);
        return current;
      },
      async import(file) { return api.commit(await api.prepare(file)); },
      get: () => current,
      clear() { if (current?.face) document.fonts.delete(current.face); current = null; },
      options() {
        if (!current) throw new Error('请先导入 TTF / OTF 字体');
        const selected = current;
        return { fontFamily: selected.family, glyphCache: selected.glyphCache, validateCharacter(code) {
          if (!selected.has(code)) throw new Error(`导入字体不包含“${String.fromCodePoint(code)}”（U+${code.toString(16).toUpperCase()}），请更换字体`);
        } };
      }
    };
    return api;
  }
  const api = { coverage, create };
  if (node) module.exports = api; else root.EPDFontSources = api;
})(globalThis);
