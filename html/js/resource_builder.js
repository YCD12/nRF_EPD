(function (root) {
  'use strict';
  const store = typeof module !== 'undefined' && module.exports ? require('./resource_store.js') : root.EPDResourceStore;
  const uiModel = typeof module !== 'undefined' && module.exports ? require('./ui_tools/ui_model.js') : root.EPDUIModel;
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
  function encodeUI(model) { return uiModel.encode(model); }

  function decodeLegacyUI(bytes) {
    const v = view(bytes);
    if (bytes.length !== 396 || v.getUint32(0, true) !== 0x31524955 ||
        [1,19,800,480,20,0].some((n, i) => v.getUint16(4 + 2 * i, true) !== n)) fail('不支持的 UIR1 资源');
    const model = { components: Array.from({ length: 19 }, (_, i) => {
      const at = 16 + 20 * i;
      if (bytes[at + 7]) fail('组件 reserved 必须为 0');
      const c = { mode: bytes[at], id: bytes[at + 1], kind: bytes[at + 2], flags: bytes[at + 3], fonts: Array.from(bytes.subarray(at + 4, at + 7)) };
      fields.forEach((key, j) => { c[key] = v.getInt16(at + 8 + j * 2, true); }); return c;
    }) };
    model.components.forEach(validateComponent); return uiModel.migrate(model);
  }

  function decodeUI(bytes) {
    return bytes.length >= 4 && view(bytes).getUint32(0,true) === 0x31524955 ? decodeLegacyUI(bytes) : uiModel.decode(bytes);
  }

  // Same slot geometry and bounded record walks as the firmware/font_store_tool.
  const metrics = [[12,12,0,254,8,254,10,255], [16,16,0,254,11,253,12,252], [13,22,0,253,19,251,19,251]];
  const todoSizes = [9, 12, 16, 20, 24, 32];
  function todoFontGeometry(size) {
    if (!todoSizes.includes(size)) fail('待办字号必须为 9、12、16、20、24 或 32');
    const pixels = Math.ceil(size * 4 / 3), descent = Math.ceil(pixels / 3), overhang = Math.ceil(pixels / 4);
    return [pixels + overhang, pixels + descent, -overhang, -descent, pixels, -descent, pixels, -descent];
  }
  const todoMetrics = todoSizes.map(size => todoFontGeometry(size).map(n => n & 255));
  function fontGeometryValid(bytes) {
    const signed=n=>n>127?n-256:n;
    return bytes[9]>=1&&bytes[9]<=64&&bytes[10]>=1&&bytes[10]<=80&&
      signed(bytes[11])>=-32&&signed(bytes[11])<=32&&signed(bytes[12])>=-64&&signed(bytes[12])<=0&&
      signed(bytes[13])>=0&&bytes[13]<=64&&signed(bytes[14])>=-64&&signed(bytes[14])<=0&&
      signed(bytes[15])>=0&&bytes[15]<=80&&signed(bytes[16])>=-64&&signed(bytes[16])<=0&&signed(bytes[13])-signed(bytes[14])<=100&&signed(bytes[15])-signed(bytes[16])<=127;
  }
  function validateFont(bytes, slot) {
    integer(slot, 0, 3, '字体槽位');
    if (!(bytes instanceof Uint8Array) || bytes.length < 23 || !bytes[0] || bytes[0] === 255 ||
        !fontGeometryValid(bytes) ||
        Array.from(bytes.subarray(2, 9)).some(n => n < 1 || n > 8)) fail(`字体槽位 ${slot} 的头或几何不兼容`);
    const v = view(bytes), glyphs = new Set(), records = new Map(), positions = new Map();
    const offsets = [17, 19, 21].map(at => 23 + v.getUint16(at));
    if (offsets.some(at => at >= bytes.length)) fail('字体入口越界');
    function walk(start, unicode) {
      const prefix = unicode ? 3 : 2, found = new Set();
      for (let at = start; ; ) {
        if (at + 2 > bytes.length) fail('字体表缺少结束符');
        const code = unicode ? v.getUint16(at) : bytes[at];
        if (unicode && code === 0) return found;
        if (at + prefix > bytes.length) fail('字体记录越界');
        const size = bytes[at + prefix - 1];
        if (!unicode && !size) return found;
        if (size < prefix || at + size > bytes.length) fail('字体记录长度错误');
        if (unicode && code <= 255) fail('Unicode 表不能包含 ASCII 编码');
        if (records.has(at) && records.get(at) !== size) fail('字体入口指向重叠的记录');
        if (positions.has(code) && positions.get(code) !== at)
          fail(`字体包含重复编码 U+${code.toString(16).toUpperCase().padStart(4, '0')}；请重新生成字库`);
        positions.set(code, at);
        records.set(at, size); glyphs.add(code); found.add(code); at += size;
      }
    }
    const ascii = [23, offsets[0], offsets[1]].map(at => walk(at, false));
    // The device selects exactly one entry for each ASCII range. A union of
    // the three walks can claim glyphs that its selected entry cannot reach.
    for (const code of glyphs) {
      if (!ascii[code >= 97 ? 2 : code >= 65 ? 1 : 0].has(code))
        fail(`字体 ASCII 入口无法在设备端查找 U+${code.toString(16).toUpperCase().padStart(4, '0')}；请重新生成字库索引`);
    }
    const table = offsets[2];
    if (table + 4 > bytes.length) fail('Unicode 索引越界');
    const jump = v.getUint16(table), first = v.getUint16(table + 2), start = table + jump;
    if (start >= bytes.length || (first && (first !== 0xffff || jump !== 4))) fail('字体必须使用单个 Unicode 块');
    if (first) walk(start, true);
    let previousEnd = 23;
    for (const [at, length] of [...records].sort((a, b) => a[0] - b[0])) {
      if (at < previousEnd) fail('字体入口指向记录内部或重叠区域');
      previousEnd = at + length;
    }
    return glyphs;
  }
  function decodeFonts(bytes) {
    const v = view(bytes);
    if (bytes.length < 56 || v.getUint32(0, true) !== 0x314b5046 || v.getUint16(4, true) !== 1 || ![3, 4].includes(v.getUint16(6, true)))
      fail('不支持的 FPK1 资源');
    const count = v.getUint16(6, true);
    let end = 8 + count * 16;
    if (bytes.length < end) fail('字体目录不完整');
    const fonts = Array.from({ length: count }, (_, slot) => {
      const at = 8 + slot * 16, offset = v.getUint32(at, true), length = v.getUint32(at + 4, true);
      const key = v.getUint32(at + 12, true);
      if (offset < end || !length || offset + length > bytes.length || (slot === 3 ? !key : key)) fail('字体目录越界或重叠');
      end = offset + length;
      const font = bytes.slice(offset, end);
      if (store.crc32(font) !== v.getUint32(at + 8, true)) fail('字体 CRC 错误');
      validateFont(font, slot); return font;
    });
    if (count === 4) fonts.customKey = v.getUint32(68, true);
    return fonts;
  }
  function encodeFonts(fonts) {
    list(fonts, null, 'fonts');
    if (![3, 4].includes(fonts.length)) fail('字体需要三个基础槽位和可选待办槽位');
    if (fonts.length === 4) integer(fonts.customKey, 1, 0xffffffff, '待办字体标识');
    fonts.forEach(validateFont);
    const directorySize = 8 + fonts.length * 16;
    const bytes = new Uint8Array(directorySize + fonts.reduce((n, b) => n + b.length, 0)), v = view(bytes);
    v.setUint32(0, 0x314b5046, true); v.setUint16(4, 1, true); v.setUint16(6, fonts.length, true);
    let offset = directorySize;
    fonts.forEach((b, i) => {
      [offset, b.length, store.crc32(b), i === 3 ? fonts.customKey : 0].forEach((n, j) => v.setUint32(8 + i * 16 + j * 4, n, true));
      bytes.set(b, offset); offset += b.length;
    }); return bytes;
  }
  function fontKey(family, sourceSlot) {
    integer(sourceSlot, 0, 1, '待办基础字号');
    return store.crc32(encoder.encode(`${sourceSlot}:${family.trim()}`)) || 1;
  }
  function todoFontKey(family, size, bold = false, italic = false) {
    todoFontGeometry(size);
    if (typeof family !== 'string' || typeof bold !== 'boolean' || typeof italic !== 'boolean')
      fail('待办字体名称或字形设置无效');
    return store.crc32(encoder.encode(`todo-v3:${size}:${bold ? 1 : 0}:${italic ? 1 : 0}:${family.trim()}`)) || 1;
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
  function importCalendarData(calendar, data) {
    if (!data || typeof data !== 'object' || Array.isArray(data)) fail('日历数据必须为 JSON 对象');
    const next = { ...calendar, ...data };
    if ((next.year_min !== calendar.year_min || next.year_max !== calendar.year_max) &&
        !['lunar_month_days', 'solar_new_year', 'jieqi_year_bits'].every(key => Object.hasOwn(data, key)))
      fail('修改年份范围时必须提供三个农历和节气计算表');
    if (next.holiday_year !== calendar.holiday_year && !Object.hasOwn(data, 'holidays'))
      fail('修改休班年份时必须提供 holidays 表');
    // Accept the packed holiday integers used by resource_pack.py as well as editor rows.
    if (Array.isArray(data.holidays)) next.holidays = data.holidays.map(row => {
      if (typeof row !== 'number') return row;
      integer(row, 0, 0x1fff, 'holidays');
      return { month: row >> 8 & 15, day: row & 255, work: !!(row & 0x1000) };
    });
    encodeCalendar(next);
    return next;
  }
  function recordPayload(magic, data) {
    const bytes = new Uint8Array(16 + data.length), v = view(bytes);
    v.setUint32(0, magic, true); v.setUint16(4, 1, true); v.setUint16(6, 16, true);
    v.setUint16(8, data.length, true); v.setUint32(12, store.crc32(data), true);
    bytes.set(data, 16); return bytes;
  }
  function decodeMigration(data) {
    if (!data || data.format !== 'EPD-MIG1') fail('迁移备份格式必须为 EPD-MIG1');
    function hex(key) {
      const value = data[key];
      if (typeof value !== 'string' || !/^(?:[0-9a-f]{2})+$/i.test(value) || value.length > 2080)
        fail(`迁移备份 ${key} 不是有效的十六进制数据`);
      return Uint8Array.from(value.match(/../g), byte => parseInt(byte, 16));
    }
    const config = hex('epd_config_hex'), annotations = hex('calendar_annotations_hex'), v = view(annotations);
    if (config.length !== 14) fail('迁移设备配置必须为 14 字节');
    if (annotations.length < 16 || v.getUint32(0, true) !== 0x4f4e4e41 || annotations[4] !== 1 ||
        annotations[5] !== 16 || v.getUint16(6, true) > 64 ||
        annotations.length !== 16 + v.getUint16(6, true) * 16 || annotations.subarray(8, 16).some(Boolean))
      fail('迁移日期标注头或长度无效');
    let previous = -1, text = '';
    for (let at = 16; at < annotations.length; at += 16) {
      const year = v.getUint16(at, true), month = annotations[at + 2], day = annotations[at + 3];
      const flags = annotations[at + 4], length = annotations[at + 5], key = year * 65536 + month * 256 + day;
      date(month, day, '迁移日期标注');
      if (!(flags & 1) || flags & 0xc0 || (flags >> 1 & 3) > 2 || (flags >> 3 & 3) > 2 ||
          length > 9 || annotations[at + 15] || key <= previous) fail('迁移日期标注无效或未按日期排序');
      text += decoder.decode(annotations.subarray(at + 6, at + 6 + length)); previous = key;
    }
    return { config, annotations, text };
  }
  function build(bundle, model, builtAt, options = {}) {
    if (options.revision !== undefined) integer(options.revision, 1, 0xffffffff, '资源版本');
    const payloads = new Map([[5, encodeCalendar(model.calendar)], [6, encodeUI(model.ui)], [7, encodeFonts(model.fonts)]]);
    if (options.migration) {
      const records = decodeMigration(options.migration);
      payloads.set(2, recordPayload(0x31435045, records.config));
      payloads.set(3, recordPayload(0x31434e41, records.annotations));
    }
    const entries = bundle.packages.map(item => {
      const next = payloads.get(item.pack.id), old = item.pack.payload;
      const changed = next && (next.length !== old.length || next.some((n, i) => n !== old[i]));
      if (!changed && !options.refreshStamp && options.revision === undefined)
        return { name: item.name, bytes: item.bytes.slice() };
      const bytes = wrap(item.pack.id, next || old,
        options.revision ?? item.pack.revision + (changed ? 1 : 0), builtAt);
      store.validateTarget(store.parsePackage(bytes), bundle.partitions);
      return { name: item.name, bytes };
    });
    return store.prepareBundle(entries);
  }
  const api = { encodeCalendar, decodeCalendar, importCalendarData, decodeMigration, encodeUI, decodeUI, validateFont, encodeFonts, decodeFonts, fontKey, todoFontGeometry, todoFontKey, wrap, open, build };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.EPDResourceBuilder = api;
})(globalThis);
