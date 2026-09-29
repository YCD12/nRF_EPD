(function (root) {
  'use strict';
  const store = typeof module !== 'undefined' && module.exports ? require('./resource_store.js') : root.EPDResourceStore;
  const encoder = new TextEncoder(), decoder = new TextDecoder('utf-8', { fatal: true });
  const view = b => new DataView(b.buffer, b.byteOffset, b.byteLength);
  const fail = message => { throw new Error(message); };
  function integer(n, min, max, name) {
    if (!Number.isInteger(n) || n < min || n > max) fail(`${name} 必须为 ${min}..${max} 的整数`);
    return n;
  }
  function list(a, count, name) {
    if (!Array.isArray(a) || (count !== null && a.length !== count) || a.length > 65535)
      fail(`${name} 的项目数量不正确`);
    return a;
  }
  function date(month, day, name) {
    integer(month, 1, 12, name); integer(day, 1, 31, name);
  }
  function textBytes(text, size, name) {
    if (typeof text !== 'string' || text.includes('\0')) fail(`${name} 必须为文本且不能包含 NUL`);
    const bytes = encoder.encode(text);
    if (bytes.length >= size) fail(`${name} 的 UTF-8 文本最多 ${size - 1} 字节`);
    const result = new Uint8Array(size); result.set(bytes); return result;
  }
  function readText(bytes) {
    const end = bytes.indexOf(0);
    if (end < 0) fail('文本缺少结束符');
    return decoder.decode(bytes.subarray(0, end));
  }
  const sections = [
    ['month_names', 7, 13, 'text'], ['leap_names', 4, 2, 'text'],
    ['date_names', 7, 31, 'text'], ['weekday_names', 4, 7, 'text'],
    ['zodiac_names', 4, 12, 'text'], ['stem_names', 4, 10, 'text'],
    ['branch_names', 4, 12, 'text'], ['jieqi_names', 7, 24, 'text'],
    ['lunar_month_days', 4, null, 'number'], ['solar_new_year', 4, null, 'number'],
    ['jieqi_year_bits', 1, null, 'number'], ['jieqi_base_days', 1, 24, 'number'],
    ['solar_festivals', 12, null, 'festival'], ['lunar_festivals', 12, null, 'festival'],
    ['holidays', 2, null, 'holiday'], ['special_festival_names', 10, 5, 'text'],
  ];

  function encodeCalendar(model) {
    const min = integer(model.year_min, 1584, 4095, 'year_min');
    const max = integer(model.year_max, min, 4095, 'year_max');
    integer(model.holiday_year, 1583, 4095, 'holiday_year');
    const years = max - min + 1;
    list(model.lunar_month_days, years + 2, 'lunar_month_days');
    list(model.solar_new_year, years + 2, 'solar_new_year');
    list(model.jieqi_year_bits, (years + 1) * 3, 'jieqi_year_bits');
    if (model.lunar_month_days[0] !== min - 1 || model.solar_new_year[0] !== min - 1)
      fail('两个农历计算表的基准年必须等于 year_min - 1');
    const parts = sections.map(([name, size, count, type]) => {
      const rows = list(model[name], count, name), bytes = new Uint8Array(rows.length * size), v = view(bytes);
      const dates = new Set();
      rows.forEach((row, i) => {
        const at = i * size;
        if (type === 'text') bytes.set(textBytes(row, size, name), at);
        else if (type === 'number') {
          integer(row, 0, size === 4 ? 0xffffffff : 255, name);
          if (size === 4) v.setUint32(at, row, true); else bytes[at] = row;
        } else {
          date(row.month, row.day, name);
          const key = row.month * 32 + row.day;
          if (dates.has(key)) fail(`${name} 包含重复日期`);
          dates.add(key);
          if (type === 'festival') {
            bytes[at] = row.month; bytes[at + 1] = row.day;
            bytes.set(textBytes(row.name, 10, name), at + 2);
          } else {
            if (typeof row.work !== 'boolean') fail('holidays.work 必须为 true 或 false');
            v.setUint16(at, (row.work ? 0x1000 : 0) | row.month << 8 | row.day, true);
          }
        }
      });
      return bytes;
    });
    const result = new Uint8Array(272 + parts.reduce((n, b) => n + b.length, 0)), v = view(result);
    v.setUint32(0, 0x314c4143, true);
    [1, 16, min, max, model.holiday_year, 0].forEach((n, i) => v.setUint16(4 + i * 2, n, true));
    let offset = 272;
    parts.forEach((bytes, i) => {
      const at = 16 + i * 16, size = sections[i][1];
      [i + 1, size, bytes.length / size, 0].forEach((n, j) => v.setUint16(at + j * 2, n, true));
      v.setUint32(at + 8, offset, true); v.setUint32(at + 12, bytes.length, true);
      result.set(bytes, offset); offset += bytes.length;
    });
    return result;
  }

  function decodeCalendar(bytes) {
    const v = view(bytes);
    if (bytes.length < 272 || v.getUint32(0, true) !== 0x314c4143 || v.getUint16(4, true) !== 1 ||
        v.getUint16(6, true) !== 16 || v.getUint16(14, true)) fail('不支持的 CAL1 资源');
    const model = { year_min: v.getUint16(8, true), year_max: v.getUint16(10, true), holiday_year: v.getUint16(12, true) };
    let end = 272;
    sections.forEach(([name, size, , type], i) => {
      const at = 16 + i * 16, count = v.getUint16(at + 4, true), offset = v.getUint32(at + 8, true);
      if (v.getUint16(at, true) !== i + 1 || v.getUint16(at + 2, true) !== size || v.getUint16(at + 6, true) ||
          v.getUint32(at + 12, true) !== size * count || offset < end || offset + size * count > bytes.length)
        fail(`${name} 索引越界或格式不兼容`);
      end = offset + size * count;
      model[name] = Array.from({ length: count }, (_, n) => {
        const p = offset + n * size;
        if (type === 'text') return readText(bytes.subarray(p, p + size));
        if (type === 'number') return size === 4 ? v.getUint32(p, true) : bytes[p];
        if (type === 'festival') return { month: bytes[p], day: bytes[p + 1], name: readText(bytes.subarray(p + 2, p + 12)) };
        const value = v.getUint16(p, true);
        if (value >> 12 > 1) fail('节假日标志不兼容');
        return { month: value >> 8 & 15, day: value & 255, work: !!(value >> 12) };
      });
    });
    encodeCalendar(model); // Apply the same semantic checks to imported data.
    return model;
  }

  const modes = [1,1,1,2,2,2,2,2,2,2,2,2,2,2,3,3,3,3,3];
  const kinds = [1,1,3,2,2,2,1,1,3,1,2,4,2,1,1,2,4,2,1];
  const fields = ['x', 'y', 'width', 'height', 'value0', 'value1'];
  function validateComponent(c, i) {
    if (c.id !== i + 1 || c.mode !== modes[i] || c.kind !== kinds[i] || c.flags !== 0)
      fail(`组件 ${i + 1} 的 ID、模式、类型和 flags 不能改变`);
    list(c.fonts, 3, 'fonts').forEach(n => { if (n !== 255) integer(n, 0, 2, '字体槽位'); });
    fields.forEach(key => integer(c[key], -32768, 32767, key));
    if (c.x < 0 || c.y < 0 || c.width <= 0 || c.height <= 0 || c.x + c.width > 800 || c.y + c.height > 480)
      fail(`组件 ${c.id} 超出 800×480 画布`);
    if ([1, 3].includes(c.kind) && c.fonts[0] === 255) fail(`组件 ${c.id} 缺少主字体`);
    if ([1, 3, 9, 15].includes(c.id) && (c.fonts[1] === 255 || c.fonts[2] === 255)) fail(`组件 ${c.id} 缺少字体`);
    if (c.id === 10 && c.fonts[1] === 255) fail('组件 10 缺少第二字体');
    if (c.id === 1 && (c.value0 < 0 || c.value0 >= 480 || c.value1 < 2 || c.value1 >= 800)) fail('日历标题参数越界');
    if ([7, 10, 14, 19].includes(c.id) && (c.value0 < ([14, 19].includes(c.id) ? c.y : 0) || c.value0 >= 480 || c.value1 < 0 || c.value1 > 800)) fail('文字组件参数越界');
    if (c.id === 15 && (c.value0 < c.y || c.value0 >= 480 || c.value1 < c.x || c.value1 > c.x + c.width)) fail('时钟标题参数越界');
    if (c.kind === 4) {
      const scale = c.value0, digits = c.value1;
      const block = digits * (11 * scale + 2) - 2 * scale;
      const span = 2 * block + (c.mode === 2 ? 4 : 6) * scale;
      if (scale < 2 || scale > 10 || scale % 2 || digits !== 2 || c.x % 8 || c.width % 8 ||
          c.height > 208 || span > c.width || 20 * scale + 4 > c.height) fail(`组件 ${c.id} 的时钟几何不兼容`);
    }
  }
  function encodeUI(model) {
    const components = list(model.components, 19, 'components'), bytes = new Uint8Array(396), v = view(bytes);
    v.setUint32(0, 0x31524955, true);
    [1, 19, 800, 480, 20, 0].forEach((n, i) => v.setUint16(4 + i * 2, n, true));
    components.forEach((c, i) => {
      validateComponent(c, i);
      const at = 16 + 20 * i;
      bytes.set([c.mode, c.id, c.kind, c.flags, ...c.fonts, 0], at);
      fields.forEach((key, j) => v.setInt16(at + 8 + j * 2, c[key], true));
    });
    return bytes;
  }
  function decodeUI(bytes) {
    const v = view(bytes);
    if (bytes.length !== 396 || v.getUint32(0, true) !== 0x31524955 ||
        [1,19,800,480,20,0].some((n, i) => v.getUint16(4 + 2 * i, true) !== n)) fail('不支持的 UIR1 资源');
    const model = { components: Array.from({ length: 19 }, (_, i) => {
      const at = 16 + 20 * i;
      if (bytes[at + 7]) fail('组件 reserved 必须为 0');
      const c = { mode: bytes[at], id: bytes[at + 1], kind: bytes[at + 2], flags: bytes[at + 3], fonts: Array.from(bytes.subarray(at + 4, at + 7)) };
      fields.forEach((key, j) => { c[key] = v.getInt16(at + 8 + j * 2, true); }); return c;
    }) };
    encodeUI(model); return model;
  }

  // Same slot geometry and bounded record walks as the firmware/font_store_tool.
  const metrics = [[12,12,0,254,8,254,10,255], [16,16,0,254,11,253,12,252], [13,22,0,253,19,251,19,251]];
  function validateFont(bytes, slot) {
    integer(slot, 0, 2, '字体槽位');
    if (!(bytes instanceof Uint8Array) || bytes.length < 23 || !bytes[0] || bytes[0] === 255 ||
        metrics[slot].some((n, i) => bytes[9 + i] !== n) ||
        Array.from(bytes.subarray(2, 9)).some(n => n < 1 || n > 8)) fail(`字体槽位 ${slot} 的头或几何不兼容`);
    const v = view(bytes), glyphs = new Set();
    const offsets = [17, 19, 21].map(at => 23 + v.getUint16(at));
    if (offsets.some(at => at >= bytes.length)) fail('字体入口越界');
    function walk(start, unicode) {
      const prefix = unicode ? 3 : 2;
      for (let at = start; ; ) {
        if (at + 2 > bytes.length) fail('字体表缺少结束符');
        const code = unicode ? v.getUint16(at) : bytes[at];
        if (unicode && code === 0) return;
        if (at + prefix > bytes.length) fail('字体记录越界');
        const size = bytes[at + prefix - 1];
        if (!unicode && !size) return;
        if (size < prefix || at + size > bytes.length) fail('字体记录长度错误');
        glyphs.add(code); at += size;
      }
    }
    walk(23, false); walk(offsets[0], false); walk(offsets[1], false);
    const table = offsets[2];
    if (table + 4 > bytes.length) fail('Unicode 索引越界');
    const jump = v.getUint16(table), first = v.getUint16(table + 2), start = table + jump;
    if (start >= bytes.length || (first && (first !== 0xffff || jump !== 4))) fail('字体必须使用单个 Unicode 块');
    if (first) walk(start, true);
    return glyphs;
  }
  function decodeFonts(bytes) {
    const v = view(bytes);
    if (bytes.length < 56 || v.getUint32(0, true) !== 0x314b5046 || v.getUint16(4, true) !== 1 || v.getUint16(6, true) !== 3)
      fail('不支持的 FPK1 资源');
    let end = 56;
    return Array.from({ length: 3 }, (_, slot) => {
      const at = 8 + slot * 16, offset = v.getUint32(at, true), length = v.getUint32(at + 4, true);
      if (offset < end || !length || offset + length > bytes.length || v.getUint32(at + 12, true)) fail('字体目录越界或重叠');
      end = offset + length;
      const font = bytes.slice(offset, end);
      if (store.crc32(font) !== v.getUint32(at + 8, true)) fail('字体 CRC 错误');
      validateFont(font, slot); return font;
    });
  }
  function encodeFonts(fonts) {
    list(fonts, 3, 'fonts');
    fonts.forEach(validateFont);
    const bytes = new Uint8Array(56 + fonts.reduce((n, b) => n + b.length, 0)), v = view(bytes);
    v.setUint32(0, 0x314b5046, true); v.setUint16(4, 1, true); v.setUint16(6, 3, true);
    let offset = 56;
    fonts.forEach((b, i) => {
      [offset, b.length, store.crc32(b), 0].forEach((n, j) => v.setUint32(8 + i * 16 + j * 4, n, true));
      bytes.set(b, offset); offset += b.length;
    }); return bytes;
  }
  function wrap(id, payload, revision, builtAt = Math.floor(Date.now() / 1000)) {
    integer(id, 1, 255, '分区 ID');
    if (!(payload instanceof Uint8Array) || !payload.length || payload.length > 524256) fail('资源载荷长度无效');
    integer(revision, 0, 0xffffffff, '资源版本'); integer(builtAt, 0, 0xffffffff, '生成时间');
    const bytes = new Uint8Array(44 + payload.length), v = view(bytes), end = 32 + payload.length;
    [0x31535245, 0x10000 | id, revision, payload.length, store.crc32(payload), 524288].forEach((n, i) => v.setUint32(i * 4, n, true));
    v.setUint32(24, store.crc32(bytes.subarray(0, 24)), true); v.setUint32(28, 0xa55ac33c, true);
    bytes.set(payload, 32);
    v.setUint32(end, 0x54535245, true); v.setUint32(end + 4, builtAt, true);
    v.setUint32(end + 8, store.crc32(bytes.subarray(end, end + 8)), true);
    return bytes;
  }
  function open(bundle) {
    const payload = id => bundle.packages.find(item => item.pack.id === id).pack.payload;
    return { calendar: decodeCalendar(payload(5)), ui: decodeUI(payload(6)), fonts: decodeFonts(payload(7)) };
  }
  function build(bundle, model, builtAt) {
    const payloads = new Map([[5, encodeCalendar(model.calendar)], [6, encodeUI(model.ui)], [7, encodeFonts(model.fonts)]]);
    const entries = bundle.packages.map(item => {
      const next = payloads.get(item.pack.id), old = item.pack.payload;
      if (!next || (next.length === old.length && next.every((n, i) => n === old[i])))
        return { name: item.name, bytes: item.bytes.slice() };
      const bytes = wrap(item.pack.id, next, item.pack.revision + 1, builtAt);
      store.validateTarget(store.parsePackage(bytes), bundle.partitions);
      return { name: item.name, bytes };
    });
    return store.prepareBundle(entries);
  }
  const api = { encodeCalendar, decodeCalendar, encodeUI, decodeUI, validateFont, encodeFonts, decodeFonts, wrap, open, build };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.EPDResourceBuilder = api;
})(globalThis);
