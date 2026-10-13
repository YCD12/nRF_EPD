(function (root) {
  'use strict';
  const node = typeof module !== 'undefined' && module.exports;
  const defaults = node ? require('./resource_defaults.js') : root.EPDResourceDefaults;
  const fonts = node ? require('./font_store_tool.js') : root.EPDFontStoreTool;
  const builder = node ? require('../resource_builder.js') : root.EPDResourceBuilder;
  const store = node ? require('../resource_store.js') : root.EPDResourceStore;
  const todo = () => node ? require('../todo_calendar.js') : root.TodoCalendar;
  const order = [1, 5, 6, 7, 2, 3, 9];
  const names = { 1: 'partition-table.ers', 5: 'calendar.ers', 6: 'ui.ers', 7: 'fonts.ers',
    2: 'epd-config.ers', 3: 'calendar-annotations.ers', 9: 'todo.ers' };
  const types = { partition_table: 1, config_record: 2, annotation_record: 3, update_status: 4,
    calendar_package: 5, ui_package: 6, font_package: 7, reserved: 8, todo_items: 9 };
  const sections = ['month_names', 'leap_names', 'date_names', 'weekday_names', 'zodiac_names',
    'stem_names', 'branch_names', 'jieqi_names', 'lunar_month_days', 'solar_new_year',
    'jieqi_year_bits', 'jieqi_base_days', 'solar_festivals', 'lunar_festivals', 'holidays', 'special_festival_names'];
  const view = bytes => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const hex = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
  const crc = bytes => '0x' + store.crc32(bytes).toString(16).toUpperCase().padStart(8, '0');
  function integer(value, min, max, label) {
    if (!Number.isInteger(value) || value < min || value > max) throw new Error(`${label}无效`);
    return value;
  }
  function validateLayout(layout) {
    if (!layout || layout.layout_version !== 1 || layout.capacity_bytes !== 524288 ||
        layout.erase_sector_bytes !== 4096 || layout.external_data_copies !== 1 || layout.external_firmware_slots !== false)
      throw new Error('分区布局必须为 ABI 1、512 KiB、4096 字节扇区及单份数据');
    if (!Array.isArray(layout.partitions) || !layout.partitions.length || layout.partitions.length > 32)
      throw new Error('分区数量必须为 1–32');
    const seen = new Set(); let end = 0;
    const partitions = layout.partitions.map(part => {
      const id = integer(part.id, 1, 65535, '分区 ID');
      const type = typeof part.type === 'string' ? types[part.type] : part.type;
      integer(type, 1, 9, '分区类型');
      const offset = typeof part.offset === 'string' ? Number(part.offset) : part.offset;
      const size = part.size_bytes ?? part.size;
      integer(offset, 0, 524288, '分区偏移'); integer(size, 4096, 524288, '分区容量');
      if (seen.has(id) || offset !== end || offset % 4096 || size % 4096 || offset + size > 524288 ||
          (part.schema !== undefined && part.schema !== 1) || (part.flags !== undefined && part.flags !== 0))
        throw new Error('分区存在重复、间隙、重叠、未对齐或越界');
      seen.add(id); end = offset + size;
      return { id, type, offset, size, schema: 1, flags: 0 };
    });
    const first = partitions[0];
    if (first.id !== 1 || first.type !== 1 || first.offset !== 0 || first.size !== 4096 || end !== 524288)
      throw new Error('分区布局必须包含固定表定位区并覆盖整个 Flash');
    return partitions;
  }
  function encodeTable(partitions) {
    const bytes = new Uint8Array(partitions.length * 16), v = view(bytes);
    partitions.forEach((part, index) => {
      const at = index * 16;
      v.setUint16(at, part.id, true); v.setUint16(at + 2, part.type, true);
      v.setUint32(at + 4, part.offset, true); v.setUint32(at + 8, part.size, true);
      v.setUint16(at + 12, part.schema ?? 1, true); v.setUint16(at + 14, part.flags ?? 0, true);
    });
    store.parseTable({ id: 1, payload: bytes });
    return bytes;
  }
  const tablePayload = (layout = defaults.layout()) => encodeTable(validateLayout(layout));
  function recordPayload(magic, data, tombstone = false) {
    if (!(data instanceof Uint8Array) || data.length > 65535 || (tombstone && data.length))
      throw new Error('记录数据长度或删除标志无效');
    const bytes = new Uint8Array(16 + data.length), v = view(bytes);
    v.setUint32(0, magic, true); v.setUint16(4, 1, true); v.setUint16(6, 16, true);
    v.setUint16(8, data.length, true); v.setUint16(10, tombstone ? 1 : 0, true);
    v.setUint32(12, store.crc32(data), true); bytes.set(data, 16);
    return bytes;
  }
  function readRecord(payload, magic) {
    if (payload.length < 16) throw new Error('REC1 记录头长度无效');
    const v = view(payload), data = payload.subarray(16), flags = v.getUint16(10, true);
    if (v.getUint32(0, true) !== magic || v.getUint16(4, true) !== 1 || v.getUint16(6, true) !== 16 ||
        v.getUint16(8, true) !== data.length || flags > 1 || (flags && data.length) ||
        v.getUint32(12, true) !== store.crc32(data)) throw new Error('REC1 格式、长度或 CRC 错误');
    return { data, tombstone: !!flags };
  }
  function emptyAnnotations() {
    const bytes = new Uint8Array(16); view(bytes).setUint32(0, 0x4f4e4e41, true);
    bytes[4] = 1; bytes[5] = 16; return bytes;
  }
  function validateRecords(config, annotations) {
    return builder.decodeMigration({ format: 'EPD-MIG1', epd_config_hex: hex(config), calendar_annotations_hex: hex(annotations) });
  }
  function todoPayload(config) {
    if (config === undefined) {
      const bytes = new Uint8Array(16); view(bytes).setUint32(0, 0x4f444f54, true);
      bytes.set([1, 80, 0, 0, 5, 12], 4); return bytes;
    }
    const api = todo();
    const bytes = config instanceof Uint8Array ? config.slice() : api.pack(config);
    api.unpack(bytes); return bytes;
  }
  function optionsFor(options) {
    if (options.revision !== undefined) integer(options.revision, 1, 0xffffffff, '资源版本');
    const builtAt = options.builtAt ?? Math.floor(Date.now() / 1000);
    integer(builtAt, 0, 0xffffffff, '生成时间');
    return { ...options, builtAt };
  }
  function assemble(payloads, options, previous) {
    options = optionsFor(options);
    const versions = defaults.versions();
    return store.prepareBundle(order.filter(id => payloads.has(id)).map(id => {
      const payload = payloads.get(id), old = previous?.packages.find(item => item.pack.id === id);
      const changed = old && (payload.length !== old.pack.payload.length || payload.some((byte, i) => byte !== old.pack.payload[i]));
      const revision = options.revision ?? (old ? old.pack.revision + (changed ? 1 : 0) : versions[id]);
      return { name: names[id], bytes: builder.wrap(id, payload, revision, options.builtAt) };
    }));
  }
  // Construct every package from source data. No existing ERS or Python output is required.
  function create(source = {}, options = {}) {
    let config = source.config ?? defaults.config(), annotations = source.annotations ?? emptyAnnotations();
    if (options.migration) ({ config, annotations } = builder.decodeMigration(options.migration));
    validateRecords(config, annotations);
    const payloads = new Map([
      [1, tablePayload(source.layout)], [5, builder.encodeCalendar(source.calendar ?? defaults.calendar())],
      [6, builder.encodeUI(source.ui ?? defaults.ui())], [7, fonts.pack(source.fonts)],
      [2, recordPayload(0x31435045, config)], [3, recordPayload(0x31434e41, annotations)], [9, todoPayload(source.todo)]
    ]);
    const result = assemble(payloads, options);
    if (result.partitions.some(part => order.includes(part.id) && part.type !== part.id))
      throw new Error('资源分区 ID 与类型不匹配');
    return validateBundle(result);
  }
  async function generateDefault(options = {}) {
    const loaded = await fonts.loadDefault(options.fetch);
    return create({ fonts: loaded }, options);
  }
  function dependencies(bundle, model = builder.open(bundle), migration) {
    const packs = new Map(bundle.packages.map(item => [item.pack.id, item.pack]));
    const config = readRecord(packs.get(2).payload, 0x31435045);
    const annotations = readRecord(packs.get(3).payload, 0x31434e41);
    const records = migration ? builder.decodeMigration(migration) : validateRecords(
      config.tombstone ? defaults.config() : config.data, annotations.tombstone ? emptyAnnotations() : annotations.data);
    const requests = fonts.requests(model.calendar, '', model.ui);
    const av = view(records.annotations), decoder = new TextDecoder('utf-8', { fatal: true });
    for (let i = 0; i < av.getUint16(6, true); i++) {
      const at = 16 + i * 16, flags = records.annotations[at + 4];
      const text = decoder.decode(records.annotations.subarray(at + 6, at + 6 + records.annotations[at + 5]));
      const size = flags >> 1 & 3;
      const slots = /[a-z]/i.test(text) || size === 1 ? [0] : size === 2 ? [1] : [0, 1];
      slots.forEach(slot => requests.push({ slot, text }));
    }
    let todoConfig = null, todoBytes = packs.get(9)?.payload;
    if (model.todo !== undefined) todoBytes = todoPayload(model.todo);
    if (todoBytes) {
      todoConfig = todo().unpack(todoBytes);
      requests.push(...todo().fontRequests.call({ ...todo(), ...todoConfig }));
    }
    return { requests, todoConfig, todoBytes };
  }
  function validateBundle(input, options = {}) {
    let bundle = store.prepareBundle(input.packages || input);
    const model = builder.open(bundle), glyphs = node ? require('../font_builder.js') : root.EPDWebFonts;
    if (options.repairFontBounds) {
      const repaired = glyphs.normalizeFonts(model.fonts);
      if (repaired.changedSlots.length) {
        const previous = bundle.packages.find(item => item.pack.id === 7);
        const bytes = builder.wrap(7, builder.encodeFonts(repaired.fonts), previous.pack.revision + 1);
        bundle = store.prepareBundle(bundle.packages.map(item => item === previous ? { name: item.name, bytes } : item));
        model.fonts = repaired.fonts; options.onRepair?.(repaired.changedSlots);
      }
    }
    if (bundle.partitions.some(part => order.includes(part.id) && part.type !== part.id))
      throw new Error('资源分区 ID 与类型不匹配');
    model.fonts.forEach((font, slot) => fonts.checkSlot(slot, font));
    const required = dependencies(bundle, model);
    const custom = required.requests.find(request => request.slot === 3);
    if (custom) {
      const typography = todo().usesTypography(required.todoConfig);
      const key = typography ? builder.todoFontKey(custom.fontFamily, custom.fontSize, custom.fontBold, custom.fontItalic)
        : builder.fontKey(custom.fontFamily, custom.sourceSlot);
      const geometry = typography ? builder.todoFontGeometry(custom.fontSize).map(n => n & 255)
        : model.fonts[custom.sourceSlot].subarray(9, 17);
      if (model.fonts.length !== 4 || model.fonts.customKey !== key ||
          !model.fonts[3].subarray(9, 17).every((n, i) => n === geometry[i]))
        throw new Error('待办自定义字体缺失，或字体标识、字号与 TODO 不匹配');
    }
    if (options.checkGlyphs !== false) {
      const present = model.fonts.map((font, slot) => builder.validateFont(font, slot));
      for (const request of required.requests) {
        const missing = [...new Set(request.text)].filter(char => !present[request.slot]?.has(char.codePointAt(0)));
        if (missing.length) throw new Error(`字体槽位 ${request.slot} 缺少资源所需字符：${missing.join('')}`);
      }
    }
    if (!options.repairFontBounds && glyphs?.validateGlyphs) model.fonts.forEach((font, slot) => glyphs.validateGlyphs(font, slot));
    options.onModel?.(model);
    return bundle;
  }
  function repairFontBounds(input) {
    return validateBundle(input, { repairFontBounds: true });
  }
  function inspectBundle(input, options = {}) {
    let model, repairedSlots = [];
    const bundle = validateBundle(input, { ...options, onModel: value => { model = value; }, onRepair: value => { repairedSlots = value; } });
    return { bundle, model, repairedSlots };
  }
  function build(bundle, model, options = {}) {
    const previous = store.prepareBundle(bundle.packages);
    const config = readRecord(previous.packages.find(item => item.pack.id === 2).pack.payload, 0x31435045);
    const annotations = readRecord(previous.packages.find(item => item.pack.id === 3).pack.payload, 0x31434e41);
    validateRecords(config.tombstone ? defaults.config() : config.data, annotations.tombstone ? emptyAnnotations() : annotations.data);
    const payloads = new Map([
      [1, encodeTable(previous.partitions)], [5, builder.encodeCalendar(model.calendar)],
      [6, builder.encodeUI(model.ui)], [7, fonts.pack(model.fonts)],
      [2, recordPayload(0x31435045, config.data, config.tombstone)],
      [3, recordPayload(0x31434e41, annotations.data, annotations.tombstone)]
    ]);
    const oldTodo = previous.packages.find(item => item.pack.id === 9);
    if (oldTodo || model.todo !== undefined) payloads.set(9, todoPayload(model.todo ?? oldTodo.pack.payload));
    if (options.migration) {
      const records = builder.decodeMigration(options.migration);
      payloads.set(2, recordPayload(0x31435045, records.config));
      payloads.set(3, recordPayload(0x31434e41, records.annotations));
    }
    return validateBundle(assemble(payloads, options, previous));
  }
  function exportFiles(bundle) {
    const validated = validateBundle(bundle);
    const packs = new Map(validated.packages.map(item => [item.pack.id, item.pack]));
    const cal = packs.get(5), ui = packs.get(6), font = packs.get(7), cv = view(cal.payload), fv = view(font.payload);
    const decoded = builder.open(validated);
    const calendar = { format: 'CAL1', revision: cal.revision, year_min: decoded.calendar.year_min,
      year_max: decoded.calendar.year_max, holiday_year: decoded.calendar.holiday_year, payload_bytes: cal.payload.length,
      sections: sections.map((name, index) => {
        const at = 16 + index * 16, offset = cv.getUint32(at + 8, true), length = cv.getUint32(at + 12, true);
        return { id: index + 1, name, item_size: cv.getUint16(at + 2, true), count: cv.getUint16(at + 4, true),
          offset, length, crc32: crc(cal.payload.subarray(offset, offset + length)) };
      }) };
    const fontManifest = { format: 'FPK1', revision: font.revision, payload_bytes: font.payload.length,
      slots: decoded.fonts.map((bytes, slot) => ({ ...fonts.checkSlot(slot, bytes), offset: fv.getUint32(8 + slot * 16, true),
        ...(slot === 3 ? { key: decoded.fonts.customKey } : {}) })) };
    const config = readRecord(packs.get(2).payload, 0x31435045), annotations = readRecord(packs.get(3).payload, 0x31434e41);
    const records = { format: 'REC1', revision: packs.get(2).revision,
      epd_config: { bytes: config.data.length, crc32: crc(config.data), revision: packs.get(2).revision, tombstone: config.tombstone },
      calendar_annotations: { bytes: annotations.data.length, records: annotations.tombstone ? 0 : view(annotations.data).getUint16(6, true),
        crc32: crc(annotations.data), revision: packs.get(3).revision, tombstone: annotations.tombstone } };
    const files = validated.packages.map(item => ({ name: item.name, bytes: item.bytes.slice() }));
    const encoder = new TextEncoder();
    const json = (name, data) => files.push({ name, bytes: encoder.encode(JSON.stringify(data, null, 2) + '\n') });
    files.push({ name: 'partition-table.payload.bin', bytes: packs.get(1).payload.slice() });
    json('fonts.json', fontManifest); json('calendar.json', calendar);
    json('ui.json', { format: 'UIR1', revision: ui.revision, screen_width: 800, screen_height: 480,
      payload_bytes: ui.payload.length, components: decoded.ui.components });
    json('records.json', records);
    if (packs.has(9)) {
      const pack = packs.get(9), bytes = todoPayload(pack.payload);
      json('todo.json', { format: 'TODO', revision: pack.revision, payload_bytes: bytes.length, version: bytes[4],
        record_size: bytes[5], count: view(bytes).getUint16(6, true), display_limit: bytes[8], font_size: bytes[9] });
    }
    return files;
  }
  const api = { validateLayout, tablePayload, recordPayload, readRecord, emptyAnnotations, todoPayload,
    dependencies, validateBundle, inspectBundle, repairFontBounds, create, generateDefault, build, exportFiles };
  if (node) module.exports = api;
  else root.EPDResourcePack = api;
})(globalThis);
