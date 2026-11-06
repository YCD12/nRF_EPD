(function (root) {
  'use strict';
  const builder = typeof module !== 'undefined' && module.exports ? require('./resource_builder.js') : root.EPDResourceBuilder;
  const fail = message => { throw new Error(message); };
  const word = (bytes, at) => bytes[at] * 256 + bytes[at + 1];
  const label = code => `“${String.fromCodePoint(code)}” (U+${code.toString(16).toUpperCase().padStart(4, '0')})`;
  const sizes = [12, 16, 22];
  const defaultFamily = '"Microsoft YaHei", "PingFang SC", "Noto Sans CJK SC", "Noto Sans SC", sans-serif';
  const glyphFailure = message => { const error = new Error(message); error.fontIssue = true; throw error; };

  function integer(value, min, max, name) {
    if (!Number.isInteger(value) || value < min || value > max) fail(`${name} 必须为 ${min}..${max} 的整数`);
    return value;
  }
  function codePoint(value) {
    integer(value, 0, 0xffff, '字符编码');
    const char = String.fromCodePoint(value);
    if ((value >= 0xd800 && value <= 0xdfff) || value >= 0xfffe || /[\p{Control}\p{Format}]/u.test(char))
      fail(`${label(value)} 不支持；仅支持 BMP 中可显示的字符，不能使用控制字符或代理项`);
    return value;
  }
  function headerBits(header) {
    if (!(header instanceof Uint8Array) || header.length < 23 || Array.from(header.subarray(2, 9)).some(n => n < 1 || n > 8))
      fail('字体头的编码位宽无效');
    return Array.from(header.subarray(2, 9));
  }
  function fontList(fonts) {
    if (!Array.isArray(fonts) || ![3, 4].includes(fonts.length)) fail('必须提供三个基础字体槽位和可选待办槽位');
    fonts.forEach((font, slot) => builder.validateFont(font, slot));
  }
  function readFont(bytes, slot) {
    builder.validateFont(bytes, slot);
    const records = new Map();
    function walk(at, unicode) {
      for (;;) {
        const code = unicode ? word(bytes, at) : bytes[at];
        const size = bytes[at + (unicode ? 2 : 1)];
        if ((unicode && !code) || (!unicode && !size)) return;
        const record = bytes.slice(at, at + size), old = records.get(code);
        if (old && (old.length !== record.length || !old.every((n, i) => n === record[i])))
          fail(`字体槽位 ${slot} 中 ${label(code)} 有冲突的字形记录`);
        records.set(code, record); at += size;
      }
    }
    [23, 23 + word(bytes, 17), 23 + word(bytes, 19)].forEach(at => walk(at, false));
    const unicode = 23 + word(bytes, 21);
    if (word(bytes, unicode + 2)) walk(unicode + word(bytes, unicode), true);
    return records;
  }

  function prepareRequests(inputFonts, requests) {
    fontList(inputFonts);
    if (!Array.isArray(requests)) fail('字体需求必须为数组');
    const fonts = inputFonts.slice();
    if (inputFonts.length === 4) fonts.customKey = inputFonts.customKey;
    const custom = requests.filter(request => request?.slot === 3 && request.sourceSlot !== undefined);
    let rebuild = false;
    if (custom.length) {
      const { sourceSlot, fontFamily } = custom[0];
      integer(sourceSlot, 0, 1, '待办基础字体槽位');
      const extended = request => ['fontSize', 'fontBold', 'fontItalic'].some(name => Object.hasOwn(request, name));
      const v3 = extended(custom[0]);
      if (typeof fontFamily !== 'string' || (!v3 && !fontFamily.trim())) fail('待办字体名称无效');
      const keyFor = request => extended(request)
        ? builder.todoFontKey(request.fontFamily, request.fontSize, request.fontBold ?? false, request.fontItalic ?? false)
        : builder.fontKey(request.fontFamily, request.sourceSlot);
      const key = keyFor(custom[0]);
      if (custom.some(request => request.sourceSlot !== sourceSlot || extended(request) !== v3 || keyFor(request) !== key))
        fail('一次保存只能使用一种待办字体');
      const geometry = v3 ? builder.todoFontGeometry(custom[0].fontSize).map(n => n & 255) : fonts[sourceSlot].subarray(9, 17);
      rebuild = fonts.length !== 4 || fonts.customKey !== key ||
        !fonts[3].subarray(9, 17).every((n, i) => n === geometry[i]);
      if (rebuild) {
        if (v3) {
          // A separate header leaves the three device fonts byte-identical.
          // Wide signed fields retain italic bearings and large advances.
          const font = new Uint8Array(31);
          font.set([1, 0, 4, 4, 7, 7, 8, 8, 8]); font.set(geometry, 9);
          font[22] = 2; font.set([0, 4, 255, 255], 25); fonts[3] = font;
        } else fonts[3] = fonts[sourceSlot].slice();
      }
      fonts.customKey = key;
    }
    if(requests.some(r=>r?.slot===3)&&fonts.length!==4) fail('布局引用了尚未安装的字体槽位 3');
    return { fonts, rebuild, custom: custom[0] || {sourceSlot:1,pixelSize:fonts[3]?.[13]} };
  }
  function gatherMissing(inputFonts, requests) {
    const { fonts, rebuild } = prepareRequests(inputFonts, requests);
    fontList(fonts);
    if (!Array.isArray(requests)) fail('字体需求必须为数组');
    const present = fonts.map((font, slot) => builder.validateFont(font, slot)), missing = new Map();
    for (const request of requests) {
      if (!request || typeof request.text !== 'string') fail('字体需求必须包含文本 text');
      const slot = integer(request.slot, 0, 3, '字体槽位');
      for (const char of request.text) {
        const code = char.codePointAt(0);
        if (code > 0xffff || (code >= 0xd800 && code <= 0xdfff))
          fail(`${label(code)} 不支持；设备仅支持 BMP 字符，不能使用 emoji 或代理项`);
        codePoint(code);
        if ((slot === 3 && rebuild) || !present[slot].has(code)) missing.set(`${slot}:${code}`, { slot, codePoint: code });
      }
    }
    return Array.from(missing.values()).sort((a, b) => a.slot - b.slot || a.codePoint - b.codePoint);
  }

  // The firmware reads bits LSB first. Signed fields use a half-range bias,
  // rather than two's complement (e.g. three-bit -4..3 is stored as 0..7).
  function encodeGlyph(header, code, glyph) {
    codePoint(code);
    return encodeRecord(header, code, glyph);
  }
  function glyphHeader(header, glyph) {
    headerBits(header);
    let next = header;
    [glyph.width, glyph.height, glyph.x, glyph.y, glyph.advance].forEach((value, i) => {
      let count = header[i + 4];
      const fits = () => i < 2 ? value >= 0 && value < 2 ** count : value >= -(2 ** (count - 1)) && value < 2 ** (count - 1);
      while (count < 8 && !fits()) count++;
      if (count !== header[i + 4]) {
        if (next === header) next = header.slice();
        next[i + 4] = count;
      }
    });
    return next;
  }
  function transcodeRecords(original, header, records) {
    if (original === header) return;
    for (const [code, record] of records)
      records.set(code, encodeRecord(header, code, decodeGlyph(original, code, record)));
  }
  function checkGlyphBounds(header, code, glyph) {
    if (!glyph.width || !glyph.height) return;
    const signed = n => n > 127 ? n - 256 : n;
    const left = signed(header[11]), bottom = signed(header[12]);
    if (glyph.x < left || glyph.x + glyph.width > left + header[9] ||
        glyph.y < bottom || glyph.y + glyph.height > bottom + header[10])
      fail(`${label(code)} 的字形偏移超出槽位边界；请重新生成或更换兼容字模`);
  }
  function fitGlyph(header, code, glyph) {
    if (glyph.width > header[9] || glyph.height > header[10])
      glyphFailure(`${label(code)} 的字形 ${glyph.width}×${glyph.height} 超出字体槽位允许的 ${header[9]}×${header[10]}；请更换尺寸兼容的字体，或减小待办字号`);
    const signed = n => n > 127 ? n - 256 : n;
    const left = signed(header[11]), bottom = signed(header[12]);
    // Move the complete bitmap into the fixed device box, retaining every
    // pixel and its advance. The header/protocol geometry never changes.
    return { ...glyph,
      x: glyph.width ? Math.max(left, Math.min(glyph.x, left + header[9] - glyph.width)) : glyph.x,
      y: glyph.height ? Math.max(bottom, Math.min(glyph.y, bottom + header[10] - glyph.height)) : glyph.y };
  }
  function encodeRecord(header, code, glyph) {
    integer(code, 0, 0xffff, '字符编码');
    if (!glyph || typeof glyph !== 'object') fail(`${label(code)} 缺少可编码的字形点阵`);
    const bits = headerBits(header), values = [glyph.width, glyph.height, glyph.x, glyph.y, glyph.advance];
    values.forEach((value, i) => {
      const count = bits[i + 2], half = 2 ** (count - 1);
      integer(value, i < 2 ? 0 : -half, i < 2 ? 2 ** count - 1 : half - 1, `${label(code)} 的${['宽度', '高度', 'X 偏移', 'Y 偏移', '前进宽度'][i]}`);
    });
    if (glyph.width > header[9] || glyph.height > header[10])
      fail(`${label(code)} 的字形 ${glyph.width}×${glyph.height} 超出字体槽位允许的 ${header[9]}×${header[10]}；请使用较小字体槽位`);
    checkGlyphBounds(header, code, glyph);
    if ((!glyph.width) !== (!glyph.height)) fail(`${label(code)} 的空白字形宽高必须同时为 0`);
    if (!glyph.pixels || glyph.pixels.length !== glyph.width * glyph.height || Array.from(glyph.pixels).some(n => n !== 0 && n !== 1))
      fail(`${label(code)} 的点阵数据必须为 width×height 个 0 或 1`);
    const out = [], prefix = code <= 255 ? 2 : 3;
    let cursor = 0;
    function put(value, count) {
      for (let i = 0; i < count; i++, cursor++) {
        const at = cursor >> 3;
        if (at === out.length) out.push(0);
        out[at] |= ((value >> i) & 1) << (cursor & 7);
      }
    }
    values.forEach((value, i) => put(value + (i < 2 ? 0 : 2 ** (bits[i + 2] - 1)), bits[i + 2]));
    // A pair is a background run followed by a foreground run. Zero-length
    // runs split long areas without flipping their color. The final bit says
    // whether to repeat that same pair, which keeps alternating pixels small.
    const pairs = [], max0 = 2 ** bits[0] - 1, max1 = 2 ** bits[1] - 1;
    let at = 0;
    while (at < glyph.pixels.length) {
      let zero = 0, one = 0;
      while (at < glyph.pixels.length && glyph.pixels[at] === 0 && zero < max0) { zero++; at++; }
      while (at < glyph.pixels.length && glyph.pixels[at] === 1 && one < max1) { one++; at++; }
      pairs.push([zero, one]);
    }
    for (let i = 0; i < pairs.length; ) {
      const pair = pairs[i++]; put(pair[0], bits[0]); put(pair[1], bits[1]);
      while (i < pairs.length && pairs[i][0] === pair[0] && pairs[i][1] === pair[1]) { put(1, 1); i++; }
      put(0, 1);
    }
    if (prefix + out.length > 255) fail(`${label(code)} 的压缩字形超过设备允许的 255 字节`);
    return Uint8Array.from(code <= 255 ? [code, prefix + out.length, ...out] : [code >> 8, code & 255, prefix + out.length, ...out]);
  }

  function decodeGlyph(header, code, record, checkBounds = true) {
    const bits = headerBits(header), prefix = code <= 255 ? 2 : 3;
    if (!(record instanceof Uint8Array) || record.length < prefix || record[prefix - 1] !== record.length)
      fail(`${label(code)} 的字形记录长度无效`);
    let cursor = prefix * 8;
    function get(count) {
      if (cursor + count > record.length * 8) fail(`${label(code)} 的压缩字形提前结束`);
      let value = 0;
      for (let i = 0; i < count; i++, cursor++) value |= ((record[cursor >> 3] >> (cursor & 7)) & 1) << i;
      return value;
    }
    const values = bits.slice(2).map((count, i) => get(count) - (i < 2 ? 0 : 2 ** (count - 1)));
    const [width, height, x, y, advance] = values, pixels = new Uint8Array(width * height);
    if (width > header[9] || height > header[10]) fail(`${label(code)} 的字形超出槽位尺寸`);
    if (checkBounds) checkGlyphBounds(header, code, { width, height, x, y });
    let at = 0, guard = record.length * 8 + 1;
    while (at < pixels.length) {
      const zero = get(bits[0]), one = get(bits[1]);
      do {
        if (--guard < 0 || !zero && !one || at + zero + one > pixels.length) fail(`${label(code)} 的压缩游程无效`);
        at += zero; pixels.fill(1, at, at + one); at += one;
      } while (get(1));
    }
    return { width, height, x, y, advance, pixels };
  }

  function writeFont(original, records, slot) {
    const entries = Array.from(records).sort((a, b) => a[0] - b[0]), ascii = entries.filter(([code]) => code <= 255), unicode = entries.filter(([code]) => code > 255);
    const asciiLength = ascii.reduce((n, [, bytes]) => n + bytes.length, 0) + 2;
    if (asciiLength > 65535) fail(`字体槽位 ${slot} 的 ASCII 表超出 16 位索引范围`);
    const bytes = new Uint8Array(23 + asciiLength + 4 + unicode.reduce((n, [, record]) => n + record.length, 0) + 2);
    bytes.set(original.subarray(0, 23));
    // 255 is reserved by the existing slot validator. The count is metadata;
    // glyph lookup walks the records, so fonts with >254 glyphs stay usable.
    bytes[0] = Math.min(254, records.size);
    let at = 23, upper = null, lower = null;
    for (const [code, record] of ascii) {
      if (upper === null && code >= 65) upper = at - 23;
      if (lower === null && code >= 97) lower = at - 23;
      bytes.set(record, at); at += record.length;
    }
    const end = at - 23;
    [[17, upper === null ? end : upper], [19, lower === null ? end : lower], [21, asciiLength]].forEach(([offset, value]) => {
      bytes[offset] = value >> 8; bytes[offset + 1] = value & 255;
    });
    at += 2; bytes.set([0, 4, 255, 255], at); at += 4;
    for (const [, record] of unicode) { bytes.set(record, at); at += record.length; }
    builder.validateFont(bytes, slot); return bytes;
  }

  function mergeFonts(incomingFonts, installedFonts, options = {}) {
    fontList(incomingFonts); fontList(installedFonts);
    installedFonts = normalizeFonts(installedFonts).fonts;
    const fonts = incomingFonts.slice(), added = [];
    let customPreserved = false;
    if (incomingFonts.length === 4) fonts.customKey = incomingFonts.customKey;
    if (installedFonts.length === 4 && (fonts.length === 3 || options.preserveCustom)) {
      customPreserved = fonts.length !== 4 || fonts.customKey !== installedFonts.customKey ||
        fonts[3].length !== installedFonts[3].length || !fonts[3].every((n, i) => n === installedFonts[3][i]);
      fonts[3] = installedFonts[3].slice(); fonts.customKey = installedFonts.customKey;
      if (customPreserved)
        for (const code of readFont(fonts[3], 3).keys()) added.push({ slot: 3, codePoint: code });
    }
    for (let slot = 0; slot < incomingFonts.length; slot++) {
      if (slot === 3 && options.preserveCustom && installedFonts.length === 4) continue;
      if (slot === 3 && (installedFonts.length !== 4 || incomingFonts.customKey !== installedFonts.customKey)) continue;
      const records = readFont(incomingFonts[slot], slot), installed = readFont(installedFonts[slot], slot), count = records.size;
      for (const [code, record] of installed) {
        if (records.has(code)) continue;
        // Some source fonts already contain a control-code glyph (WQY9 has
        // U+0080). Retain it during migration, while generation still rejects
        // newly requested control characters through encodeGlyph.
        const glyph = decodeGlyph(installedFonts[slot], code, record), next = glyphHeader(fonts[slot], glyph);
        transcodeRecords(fonts[slot], next, records); fonts[slot] = next;
        checkGlyphBounds(next, code, glyph);
        const sameBits = next.subarray(2, 9).every((n, i) => n === installedFonts[slot][i + 2]);
        records.set(code, sameBits ? record : encodeRecord(next, code, glyph));
        added.push({ slot, codePoint: code });
      }
      if (records.size !== count) fonts[slot] = writeFont(fonts[slot], records, slot);
    }
    return { fonts, added, customPreserved };
  }

  async function rasterizeGlyph(code, slot, header, options = {}) {
    codePoint(code); integer(slot, 0, 2, '字体槽位'); headerBits(header);
    const document = options.document || root.document;
    if (!document || !document.fonts || typeof document.fonts.load !== 'function') fail('浏览器不支持字体加载和 Canvas 字形生成');
    const v3 = Object.hasOwn(options, 'fontSize');
    if (v3) builder.todoFontKey(options.fontFamily ?? '', options.fontSize, options.fontBold ?? false, options.fontItalic ?? false);
    const family = options.fontFamily || defaultFamily, size = v3 ? Math.ceil(options.fontSize * 4 / 3) : options.pixelSize || sizes[slot];
    const css = `${v3 && options.fontItalic ? 'italic ' : ''}${v3 ? options.fontBold ? 'bold ' : '' : slot === 2 ? 'bold ' : ''}${size}px ${family}`, char = String.fromCodePoint(code);
    let loadedFaces;
    try { loadedFaces = await document.fonts.load(css, char) || []; await document.fonts.ready; }
    catch (error) { fail(`加载浏览器字体失败：${error.message || error}`); }
    const side = size * 6, origin = size * 2, canvasKey = `${side}:${css}`;
    const canvas = options.canvasCache?.get(canvasKey) || document.createElement('canvas');
    if (canvas.width !== side || canvas.height !== side) canvas.width = canvas.height = side;
    options.canvasCache?.set(canvasKey, canvas);
    const context = canvas.getContext('2d', { willReadFrequently: true });
    if (!context) fail('浏览器无法创建 Canvas 字形画布');
    context.font = css; context.textBaseline = 'alphabetic'; context.textAlign = 'left'; context.fillStyle = '#000';
    function render(text) {
      context.clearRect(0, 0, side, side); context.fillText(text, origin, origin);
      const alpha = context.getImageData(0, 0, side, side).data;
      let left = side, top = side, right = -1, bottom = -1;
      for (let row = 0; row < side; row++) for (let col = 0; col < side; col++) {
        if (alpha[(row * side + col) * 4 + 3] < 128) continue;
        left = Math.min(left, col); right = Math.max(right, col); top = Math.min(top, row); bottom = Math.max(bottom, row);
      }
      const width = right < left ? 0 : right - left + 1, height = bottom < top ? 0 : bottom - top + 1, pixels = new Uint8Array(width * height);
      for (let row = 0; row < height; row++) for (let col = 0; col < width; col++)
        pixels[row * width + col] = alpha[((top + row) * side + left + col) * 4 + 3] >= 128 ? 1 : 0;
      if (width && (left === 0 || top === 0 || right === side - 1 || bottom === side - 1))
        glyphFailure(`${label(code)} 超出字形画布，请更换字体`);
      return { width, height, x: width ? left - origin : 0, y: height ? origin - bottom - 1 : 0, advance: Math.round(context.measureText(text).width), pixels };
    }
    const glyph = render(char);
    if (!glyph.width && !/\p{Space_Separator}/u.test(char)) glyphFailure(`浏览器字体不支持 ${label(code)}，未生成可显示的点阵；请更换系统字体`);
    if (glyph.width) {
      // Missing glyphs generally share the font's .notdef bitmap. Compare with
      // several known absent code points and the replacement character instead
      // of installing a browser's placeholder as if it were a real glyph.
      // This cache belongs to one generation job, so font imports or system
      // font changes cannot leave stale reference bitmaps in the next job.
      let references = options.missingGlyphCache?.get(css);
      if (!references || references.document !== document || references.faces.length !== loadedFaces.length ||
          loadedFaces.some((face, index) => references.faces[index] !== face)) {
        references = { document, faces: loadedFaces.slice(), glyphs: new Map() };
        options.missingGlyphCache?.set(css, references);
      }
      for (const sentinel of ['\u0378', '\u0380', '\uffff', '\ufffe', '\ufffd', '\u{10ffff}']) {
        if (sentinel === char) continue;
        let absent = references.glyphs.get(sentinel);
        if (!absent) { absent = render(sentinel); references.glyphs.set(sentinel, absent); }
        if (glyph.width === absent.width && glyph.height === absent.height && glyph.advance === absent.advance &&
            glyph.pixels.every((pixel, i) => pixel === absent.pixels[i]))
          glyphFailure(`浏览器字体不支持 ${label(code)}，检测到缺字占位方框；请安装支持该字符的系统字体`);
      }
    }
    // Validate without mutating the font, and report field/geometry failures
    // before any BLE transfer. The compact bounding box is never cropped.
    const fitted = fitGlyph(header, code, glyph);
    try { encodeGlyph(glyphHeader(header, fitted), code, fitted); }
    catch (error) { error.fontIssue = true; throw error; }
    return fitted;
  }

  async function extendFonts(inputFonts, requests, options = {}) {
    const prepared = prepareRequests(inputFonts, requests);
    const fonts = prepared.fonts, added = [], records = new Map(), canvasCache = new Map(), missingGlyphCache = new Map(), issues = [];
    const pending = new Map(gatherMissing(inputFonts, requests).map(item => [`${item.slot}:${item.codePoint}`, item]));
    const rebuildSlots = new Set(options.rebuildSlots || []);
    for (const slot of rebuildSlots) {
      integer(slot, 0, 2, '重绘字体槽位');
      const old = readFont(fonts[slot], slot), original = fonts[slot];
      fonts[slot] = original.slice(); fonts[slot].set([4, 4, 7, 7, 8, 8, 8], 2);
      const pixelSize=options.slotOptions?.(slot)?.pixelSize;
      if(pixelSize) {
        integer(pixelSize,8,43,'界面字体像素字号');
        const descent=Math.ceil(pixelSize/3),overhang=Math.ceil(pixelSize/4);
        fonts[slot].set([pixelSize+overhang,pixelSize+descent,-overhang&255,-descent&255,
          pixelSize,-descent&255,pixelSize,-descent&255],9);
      }
      const preserved = new Map();
      for (const [code, record] of old) {
        if (/[\p{Control}\p{Format}]/u.test(String.fromCodePoint(code)))
          preserved.set(code, encodeRecord(fonts[slot], code, decodeGlyph(original, code, record)));
        else pending.set(`${slot}:${code}`, { slot, codePoint: code });
      }
      records.set(slot, preserved);
    }
    // Resizing a base slot also changes the geometry of a family-only TODO
    // font derived from that slot. Rebuild it in the same atomic generation.
    const refreshed=prepareRequests(fonts,requests);
    if(refreshed.rebuild) {
      fonts[3]=refreshed.fonts[3];fonts.customKey=refreshed.fonts.customKey;
      prepared.rebuild=true;prepared.custom=refreshed.custom;
      for(const request of requests.filter(r=>r.slot===3))for(const char of request.text)
        pending.set(`3:${char.codePointAt(0)}`,{slot:3,codePoint:char.codePointAt(0)});
    }
    const missing = Array.from(pending.values()).sort((a, b) => a.slot - b.slot || a.codePoint - b.codePoint);
    const estimated = fonts.map((font, slot) => records.has(slot) ? 31 + [...records.get(slot).values()].reduce((sum, record) => sum + record.length, 0)
      : slot === 3 && prepared.rebuild ? 31 : font.length);
    const rasterize = options.rasterize || rasterizeGlyph;
    const checkCancelled = () => {
      if (options.signal?.aborted) { const error = new Error('已取消生成，保留之前的资源'); error.name = 'AbortError'; throw error; }
    };
    checkCancelled();
    options.onProgress?.({ completed: 0, total: missing.length });
    let completed = 0;
    for (const request of missing) {
      checkCancelled();
      const { slot, codePoint: code } = request;
      if (!records.has(slot)) records.set(slot, slot === 3 && prepared.rebuild ? new Map() : readFont(fonts[slot], slot));
      const sourceSlot = slot === 3 ? prepared.custom.sourceSlot : slot;
      const slotOptions = options.slotOptions?.(slot) || {};
      const identity = slot === 3 ? fonts.customKey : slotOptions.fontFamily || options.fontFamily || '';
      const cacheKey = `${slot}:${identity}:${code}:${Array.from(fonts[slot].subarray(2, 17)).join(',')}`;
      let glyph, next, encoded, phase = 'coverage';
      try {
        slotOptions.validateCharacter?.(code);
        phase = 'rasterize';
        const bitmap = slotOptions.glyphCache?.get(cacheKey) || await rasterize(code, sourceSlot, fonts[slot].slice(0, 23),
          slot === 3 ? { ...options, ...Object.fromEntries(['fontFamily', 'fontSize', 'fontBold', 'fontItalic', 'pixelSize']
            .filter(name => Object.hasOwn(prepared.custom, name)).map(name => [name, prepared.custom[name]])), canvasCache, missingGlyphCache }
            : { ...options, ...slotOptions, canvasCache, missingGlyphCache });
        phase = 'encoding';
        glyph = fitGlyph(fonts[slot], code, bitmap);
        next = glyphHeader(fonts[slot], glyph);
        encoded = encodeGlyph(next, code, glyph);
      } catch (error) {
        checkCancelled();
        if (!options.collectIssues || error.name === 'AbortError' || phase === 'rasterize' && !error.fontIssue) throw error;
        issues.push({ slot, codePoint: code, message: error.message || String(error) });
      }
      checkCancelled();
      if (encoded) {
        if (next !== fonts[slot]) {
          transcodeRecords(fonts[slot], next, records.get(slot)); fonts[slot] = next;
          estimated[slot] = 31 + [...records.get(slot).values()].reduce((sum, record) => sum + record.length, 0);
        }
        estimated[slot] += encoded.length - (records.get(slot).get(code)?.length || 0);
        if (options.maxPayloadBytes && 8 + fonts.length * 16 + estimated.reduce((sum, size) => sum + size, 0) > options.maxPayloadBytes)
          fail('生成的字模超过字体分区容量，请减少额外文字');
        slotOptions.glyphCache?.set(cacheKey, glyph);
        records.get(slot).set(code, encoded); added.push(request);
      }
      options.onProgress?.({ completed: ++completed, total: missing.length, slot, codePoint: code, issues: issues.length });
      if (completed % (options.batchSize || 8) === 0)
        await (options.yield || (() => new Promise(resolve => setTimeout(resolve, 0))))();
    }
    checkCancelled();
    if (issues.length) {
      const samples = issues.slice(0, 8).map(issue => `槽位 ${issue.slot} ${issue.message}`).join('；');
      const error = new Error(`字体兼容性检查发现 ${issues.length} 个字符问题：${samples}${issues.length > 8 ? '；其余问题请查看明细' : ''}。请更换支持这些字符且尺寸兼容的字体后重新生成。`);
      error.fontIssues = issues; throw error;
    }
    for (const [slot, glyphs] of records) fonts[slot] = writeFont(fonts[slot], glyphs, slot);
    return { fonts, added, missing };
  }
  function validateGlyphs(font, slot) { for (const [code, record] of readFont(font, slot)) decodeGlyph(font, code, record); }
  function glyph(font, code, slot) {
    const record = readFont(font, slot).get(code);
    return record ? decodeGlyph(font, code, record) : null;
  }
  function glyphs(font, slot) {
    return new Map([...readFont(font, slot)].map(([code, record]) => [code, decodeGlyph(font, code, record)]));
  }
  function normalizeFonts(inputFonts) {
    fontList(inputFonts);
    const fonts = inputFonts.slice(), changedSlots = [];
    if (inputFonts.length === 4) fonts.customKey = inputFonts.customKey;
    for (let slot = 0; slot < fonts.length; slot++) {
      const original = fonts[slot], records = readFont(original, slot), adjusted = new Map();
      for (const [code, record] of records) {
        const decoded = decodeGlyph(original, code, record, false), fitted = fitGlyph(original, code, decoded);
        if (fitted.x !== decoded.x || fitted.y !== decoded.y) {
          fonts[slot] = glyphHeader(fonts[slot], fitted); adjusted.set(code, fitted);
        }
      }
      if (adjusted.size) {
        for (const [code, record] of records) {
          if (adjusted.has(code) || fonts[slot] !== original)
            records.set(code, encodeRecord(fonts[slot], code, adjusted.get(code) || decodeGlyph(original, code, record, false)));
        }
        fonts[slot] = writeFont(fonts[slot], records, slot); changedSlots.push(slot);
      }
    }
    return { fonts, changedSlots };
  }
  function recoverFonts(inputFonts, fallbackFonts) {
    fontList(fallbackFonts);
    if (!Array.isArray(inputFonts) || ![3, 4].includes(inputFonts.length)) fail('设备字体槽位数量无效');
    const fonts = inputFonts.slice(), damaged = [];
    let recovered = 0;
    if (fonts.length === 4) fonts.customKey = inputFonts.customKey;
    function replace(slot) {
      if (slot < 3) fonts[slot] = fallbackFonts[slot];
      else { fonts.length = 3; delete fonts.customKey; }
    }
    for (let slot = 0; slot < inputFonts.length; slot++) {
      const original = inputFonts[slot];
      let records;
      try { records = readFont(original, slot); }
      catch (error) { damaged.push({ slot, message: error.message }); replace(slot); continue; }
      let header = original, changed = false;
      const retained = new Map();
      for (const [code, record] of records) {
        try {
          const decoded = decodeGlyph(original, code, record, false), fitted = fitGlyph(original, code, decoded);
          header = glyphHeader(header, fitted);
          changed ||= fitted.x !== decoded.x || fitted.y !== decoded.y;
          retained.set(code, fitted);
        } catch (error) {
          damaged.push({ slot, codePoint: code, message: error.message }); changed = true;
        }
      }
      recovered += retained.size;
      if (!changed) continue;
      if (!retained.size) { replace(slot); continue; }
      // encodeRecord also preserves legacy control-code records. Only a
      // corrupt record is discarded; healthy device-only glyphs survive.
      const repaired = new Map([...retained].map(([code, shape]) => [code, encodeRecord(header, code, shape)]));
      fonts[slot] = writeFont(header, repaired, slot);
    }
    return { fonts, damaged, recovered };
  }
  const api = { gatherMissing, encodeGlyph, decodeGlyph, fitGlyph, rasterizeGlyph, extendFonts, mergeFonts, validateGlyphs, glyph, glyphs, normalizeFonts, recoverFonts };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.EPDWebFonts = api;
})(globalThis);
