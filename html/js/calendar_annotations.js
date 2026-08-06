/* Device-owned date annotations (external Flash resource type 7 / entry 1). */
const CalendarAnnotations = {
  TYPE: 7,
  ID: 1,
  MAGIC: 0x4F4E4E41, // "ANNO" in little-endian resource bytes
  VERSION: 1,
  HEADER_SIZE: 16,
  RECORD_SIZE: 16,
  MAX_RECORDS: 128,
  MAX_LABEL_BYTES: 9,
  GLYPH_FORMAT: 1,
  GLYPH_RECORD_SIZE: 12,
  MAX_GLYPHS: 768,
  MAX_GLYPH_DIM: 16,
  FONT_SIZE_FOLLOW: 0,
  FONT_SIZE_SMALL: 9,
  FONT_SIZE_LARGE: 12,
  FONT_MODE_SMALL: 1,
  FONT_MODE_LARGE: 2,
  COLOR_BLACK: 0,
  COLOR_WHITE: 1,
  COLOR_RED: 2,
  FONT_SPECS: [
    /* pixelSize is the resource lookup key.  The WQY font names are nominal:
     * their actual CJK cells, decoded from the bundled U8g2 fonts, are larger. */
    { id: 1, pixelSize: 9, name: '农历9号', cellWidth: 12, cellHeight: 11, advance: 13, yOffset: -10 },
    { id: 2, pixelSize: 12, name: '农历12号', cellWidth: 16, cellHeight: 16, advance: 17, yOffset: -14 }
  ],
  annotations: [],
  fontCoverage: null,
  tofuSignatures: new Map(),

  init() {
    const input = document.getElementById('calendarAnnotationDate');
    const labelInput = document.getElementById('calendarAnnotationLabel');
    const fontSizeInput = document.getElementById('calendarAnnotationFontSize');
    const colorInput = document.getElementById('calendarAnnotationColor');
    if (input && !input.value) input.value = this.localDateText();
    if (labelInput) labelInput.addEventListener('input', () => this.updateLabelHint());
    if (fontSizeInput) fontSizeInput.addEventListener('change', () => this.updateLabelHint());
    if (colorInput) colorInput.addEventListener('change', () => this.updateLabelHint());
    this.updateLabelHint();
    this.render();
  },

  localDateText() {
    const now = new Date();
    const pad = (value) => String(value).padStart(2, '0');
    return `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}`;
  },

  parseDate(value) {
    const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(value || '');
    if (!match) throw new Error('请选择有效日期');
    const year = Number(match[1]);
    const month = Number(match[2]);
    const day = Number(match[3]);
    if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > new Date(year, month, 0).getDate()) {
      throw new Error('日期超出支持范围或不存在');
    }
    return { year, month, day, key: value };
  },

  encoder() {
    if (typeof TextEncoder === 'undefined') throw new Error('当前浏览器不支持 UTF-8 日期标注');
    return new TextEncoder();
  },

  readU16BE(bytes, offset) {
    if (offset < 0 || offset + 1 >= bytes.length) return 0;
    return (bytes[offset] << 8) | bytes[offset + 1];
  },

  parseU8g2Coverage(fontBytes) {
    if (!fontBytes || fontBytes.length < 23) throw new Error('网页字库数据不完整，无法检测缺字');
    const supported = new Set();
    let index = 23;
    let guard = 0;
    while (index + 1 < fontBytes.length && guard++ < 2048) {
      const glyphSize = fontBytes[index + 1];
      if (glyphSize === 0) break;
      supported.add(fontBytes[index]);
      index += glyphSize;
    }

    const lookupStart = 23 + this.readU16BE(fontBytes, 21);
    if (lookupStart < fontBytes.length) {
      const entryCount = Math.floor(this.readU16BE(fontBytes, lookupStart) / 4);
      let subblockOffset = lookupStart;
      for (let i = 0; i < entryCount && i < 1024; i++) {
        const entryOffset = lookupStart + i * 4;
        if (entryOffset + 3 >= fontBytes.length) break;
        subblockOffset += this.readU16BE(fontBytes, entryOffset);
        index = subblockOffset;
        guard = 0;
        while (index + 2 < fontBytes.length && guard++ < 2048) {
          const codepoint = this.readU16BE(fontBytes, index);
          if (codepoint === 0) break;
          const glyphSize = fontBytes[index + 2];
          if (glyphSize === 0) break;
          supported.add(codepoint);
          index += glyphSize;
        }
      }
    }
    return supported;
  },

  getFontCoverage() {
    if (this.fontCoverage) return this.fontCoverage;
    const bundle = ResourceManager.getBundle();
    this.fontCoverage = new Map();
    for (const spec of this.FONT_SPECS) {
      const entry = bundle.entries.find(item => item.type === 1 && item.id === spec.id);
      if (!entry) throw new Error(`网页资源包缺少 ${spec.name} 日历字库`);
      const start = entry.offset - bundle.dataStart;
      const fontBytes = bundle.data.slice(start, start + entry.length);
      this.fontCoverage.set(spec.pixelSize, this.parseU8g2Coverage(fontBytes));
    }
    return this.fontCoverage;
  },

  normalizeFontSize(value) {
    const size = Number(value);
    return size === this.FONT_SIZE_SMALL || size === this.FONT_SIZE_LARGE ?
      size : this.FONT_SIZE_FOLLOW;
  },

  fontSpecsForSize(fontSize) {
    const normalized = this.normalizeFontSize(fontSize);
    return normalized === this.FONT_SIZE_FOLLOW ? this.FONT_SPECS :
      this.FONT_SPECS.filter(spec => spec.pixelSize === normalized);
  },

  fontSizeToMode(fontSize) {
    const normalized = this.normalizeFontSize(fontSize);
    if (normalized === this.FONT_SIZE_SMALL) return this.FONT_MODE_SMALL;
    if (normalized === this.FONT_SIZE_LARGE) return this.FONT_MODE_LARGE;
    return 0;
  },

  fontModeToSize(mode) {
    if (mode === this.FONT_MODE_SMALL) return this.FONT_SIZE_SMALL;
    if (mode === this.FONT_MODE_LARGE) return this.FONT_SIZE_LARGE;
    return this.FONT_SIZE_FOLLOW;
  },

  fontSizeName(fontSize) {
    const normalized = this.normalizeFontSize(fontSize);
    if (normalized === this.FONT_SIZE_SMALL) return '小号（农历9号）';
    if (normalized === this.FONT_SIZE_LARGE) return '大号（农历12号）';
    return '跟随农历';
  },

  normalizeColor(value) {
    const color = Number(value);
    return color === this.COLOR_WHITE || color === this.COLOR_RED ? color : this.COLOR_BLACK;
  },

  colorName(color) {
    const normalized = this.normalizeColor(color);
    if (normalized === this.COLOR_WHITE) return '白色';
    if (normalized === this.COLOR_RED) return '红色';
    return '黑色';
  },

  analyzeLabel(label, fontSize = this.FONT_SIZE_FOLLOW) {
    const coverage = this.getFontCoverage();
    const missing = new Map();
    const unsupported = [];
    const normalized = (label || '').normalize('NFC');

    for (const character of Array.from(normalized)) {
      const codepoint = character.codePointAt(0);
      if (codepoint > 0xFFFF || codepoint < 0x20 ||
          (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
          codepoint === 0x200D || (codepoint >= 0xFE00 && codepoint <= 0xFE0F) ||
          (codepoint >= 0x0300 && codepoint <= 0x036F) ||
          (codepoint >= 0x1AB0 && codepoint <= 0x1AFF) ||
          (codepoint >= 0x1DC0 && codepoint <= 0x1DFF) ||
          (codepoint >= 0x20D0 && codepoint <= 0x20FF) ||
          (codepoint >= 0xFE20 && codepoint <= 0xFE2F)) {
        unsupported.push(character);
        continue;
      }
      for (const spec of this.fontSpecsForSize(fontSize)) {
        if (!coverage.get(spec.pixelSize).has(codepoint)) {
          const key = `${codepoint}:${spec.pixelSize}`;
          missing.set(key, { character, codepoint, pixelSize: spec.pixelSize });
        }
      }
    }
    return { normalized, missing: [...missing.values()], unsupported };
  },

  labelByteLength(label) {
    return this.encoder().encode(label || '').length;
  },

  updateLabelHint() {
    const input = document.getElementById('calendarAnnotationLabel');
    const fontSizeInput = document.getElementById('calendarAnnotationFontSize');
    const hint = document.getElementById('calendarAnnotationLabelHint');
    if (!input || !hint) return;
    let bytes = 0;
    try {
      bytes = this.labelByteLength(input.value);
    } catch (_) {
      return;
    }
    let analysis = { missing: [], unsupported: [] };
    try {
      analysis = this.analyzeLabel(input.value, fontSizeInput ? fontSizeInput.value : 0);
    } catch (error) {
      hint.textContent = error.message;
      hint.style.color = '#b42318';
      hint.style.borderColor = '#b42318';
      return;
    }
    const overLimit = bytes > this.MAX_LABEL_BYTES;
    const missingCharacters = [...new Set(analysis.missing.map(item => item.character))].join('');
    const invalid = overLimit || analysis.unsupported.length > 0;
    let suffix = '';
    if (overLimit) suffix = '（超出，无法保存）';
    else if (analysis.unsupported.length > 0) suffix = `（不支持：${analysis.unsupported.join('')}）`;
    else if (missingCharacters) suffix = `（缺字：${missingCharacters}，保存时同步点阵）`;
    else if (input.value) suffix = '（设备字库已包含）';
    hint.textContent = `${bytes}/${this.MAX_LABEL_BYTES} UTF-8 字节${suffix}`;
    hint.style.color = invalid ? '#b42318' : (missingCharacters ? '#b54708' : '');
    hint.style.borderColor = invalid ? '#b42318' : (missingCharacters ? '#b54708' : '');
  },

  addOrUpdate() {
    try {
      const dateInput = document.getElementById('calendarAnnotationDate');
      const labelInput = document.getElementById('calendarAnnotationLabel');
      const fontSizeInput = document.getElementById('calendarAnnotationFontSize');
      const colorInput = document.getElementById('calendarAnnotationColor');
      const date = this.parseDate(dateInput.value);
      const fontSize = this.normalizeFontSize(fontSizeInput ? fontSizeInput.value : 0);
      const color = this.normalizeColor(colorInput ? colorInput.value : 0);
      const labelAnalysis = this.analyzeLabel((labelInput.value || '').trim(), fontSize);
      const label = labelAnalysis.normalized;
      const labelBytes = this.labelByteLength(label);
      if (labelBytes > this.MAX_LABEL_BYTES) {
        throw new Error(`短标签最多 ${this.MAX_LABEL_BYTES} 个 UTF-8 字节（可留空，仅显示红点）`);
      }
      if (labelAnalysis.unsupported.length > 0) {
        throw new Error(`短标签包含设备不支持的字符：${labelAnalysis.unsupported.join('')}`);
      }
      const item = { ...date, label, fontSize, color };
      const index = this.annotations.findIndex(existing => existing.key === item.key);
      if (index === -1) this.annotations.push(item);
      else this.annotations[index] = item;
      this.sort();
      this.render();
      this.log(`已在本页暂存 ${item.key} 的日期标注，请点击“保存到设备”生效`);
    } catch (error) {
      this.report(error);
    }
  },

  remove(key) {
    this.annotations = this.annotations.filter(item => item.key !== key);
    this.render();
    this.log(`已移除 ${key} 的暂存标注，请点击“保存到设备”生效`);
  },

  sort() {
    this.annotations.sort((a, b) => a.key.localeCompare(b.key));
  },

  decode(bytes) {
    if (typeof TextDecoder === 'undefined') throw new Error('当前浏览器不支持 UTF-8 日期标注');
    if (!bytes || bytes.length < this.HEADER_SIZE) throw new Error('日期标注资源长度不足');
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (view.getUint32(0, true) !== this.MAGIC || bytes[4] !== this.VERSION || bytes[5] !== this.RECORD_SIZE) {
      throw new Error('日期标注资源格式不兼容');
    }
    const count = view.getUint16(6, true);
    if (count > this.MAX_RECORDS || this.HEADER_SIZE + count * this.RECORD_SIZE > bytes.length) {
      throw new Error('日期标注资源已损坏');
    }
    if (bytes[8] !== 0) {
      const glyphCount = view.getUint16(10, true);
      const glyphTableOffset = this.HEADER_SIZE + count * this.RECORD_SIZE;
      const bitmapOffset = view.getUint32(12, true);
      if (bytes[8] !== this.GLYPH_FORMAT || bytes[9] !== this.GLYPH_RECORD_SIZE ||
          glyphCount > this.MAX_GLYPHS ||
          glyphTableOffset + glyphCount * this.GLYPH_RECORD_SIZE > bitmapOffset ||
          bitmapOffset > bytes.length) {
        throw new Error('日期标注的动态字形数据已损坏');
      }
      let previousKey = -1;
      for (let i = 0; i < glyphCount; i++) {
        const offset = glyphTableOffset + i * this.GLYPH_RECORD_SIZE;
        const codepoint = view.getUint16(offset, true);
        const pixelSize = bytes[offset + 2];
        const width = bytes[offset + 3];
        const height = bytes[offset + 4];
        const bitmapLength = bytes[offset + 8];
        const glyphBitmapOffset = view.getUint16(offset + 10, true);
        const expectedLength = Math.ceil(width / 8) * height;
        const key = codepoint * 256 + pixelSize;
        if (key <= previousKey || width < 1 || width > this.MAX_GLYPH_DIM ||
            height < 1 || height > this.MAX_GLYPH_DIM || bytes[offset + 5] === 0 ||
            bitmapLength !== expectedLength || glyphBitmapOffset < bitmapOffset ||
            glyphBitmapOffset + bitmapLength > bytes.length) {
          throw new Error('日期标注包含无效的动态字形记录');
        }
        previousKey = key;
      }
    }
    const decoder = new TextDecoder('utf-8', { fatal: true });
    const result = [];
    for (let i = 0; i < count; i++) {
      const offset = this.HEADER_SIZE + i * this.RECORD_SIZE;
      const year = view.getUint16(offset, true);
      const month = bytes[offset + 2];
      const day = bytes[offset + 3];
      const flags = bytes[offset + 4];
      const labelLength = bytes[offset + 5];
      const fontSize = this.fontModeToSize((flags >> 1) & 0x03);
      const color = this.normalizeColor((flags >> 3) & 0x03);
      const key = `${year}-${String(month).padStart(2, '0')}-${String(day).padStart(2, '0')}`;
      if ((flags & 1) === 0 || labelLength > this.MAX_LABEL_BYTES) continue;
      try {
        this.parseDate(key);
        result.push({
          year, month, day, key, fontSize, color,
          label: decoder.decode(bytes.slice(offset + 6, offset + 6 + labelLength))
        });
      } catch (_) {
        /* Skip an invalid individual record but keep the rest recoverable. */
      }
    }
    this.annotations = result;
    this.sort();
  },

  isCjkCharacter(character) {
    const codepoint = character.codePointAt(0);
    return (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
      (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
      (codepoint >= 0xF900 && codepoint <= 0xFAFF);
  },

  rasterizeCanvasGlyph(character, pixelSize, forceCjkCell = false) {
    /* Browser CSS pixels and the U8g2 font's nominal size are not equivalent.
     * Decode-derived target cells are 12x11 for WQY9 and 16x16 for WQY12.
     * Render at 4x, crop the real ink, then fit it to that exact lunar cell. */
    const spec = this.FONT_SPECS.find(item => item.pixelSize === pixelSize);
    if (!spec) throw new Error(`不支持的日期标注字号：${pixelSize}`);
    const sourceScale = 4;
    const sourcePixelSize = spec.cellHeight * sourceScale;
    const scratch = document.createElement('canvas');
    scratch.width = sourcePixelSize * 2;
    scratch.height = sourcePixelSize * 2;
    const scratchContext = scratch.getContext('2d', { willReadFrequently: true });
    if (!scratchContext) throw new Error('当前浏览器无法生成缺字点阵');
    const font = `${sourcePixelSize}px "Microsoft YaHei", "Noto Sans CJK SC", "PingFang SC", SimSun, sans-serif`;
    scratchContext.font = font;
    scratchContext.textBaseline = 'alphabetic';
    const metrics = scratchContext.measureText(character);
    const left = Math.max(0, Math.ceil(metrics.actualBoundingBoxLeft || 0));
    const right = Math.max(1, Math.ceil(metrics.actualBoundingBoxRight || metrics.width || sourcePixelSize));
    const ascent = Math.max(1, Math.ceil(metrics.actualBoundingBoxAscent || sourcePixelSize));
    const descent = Math.max(0, Math.ceil(metrics.actualBoundingBoxDescent || 0));
    const measuredWidth = Math.min(scratch.width, left + right + sourceScale * 2);
    const measuredHeight = Math.min(scratch.height, ascent + descent + sourceScale * 2);
    scratchContext.clearRect(0, 0, scratch.width, scratch.height);
    scratchContext.fillStyle = '#000';
    scratchContext.fillText(character, sourceScale + left, sourceScale + ascent);

    const sourcePixels = scratchContext.getImageData(0, 0, measuredWidth, measuredHeight).data;
    let minX = measuredWidth;
    let minY = measuredHeight;
    let maxX = -1;
    let maxY = -1;
    for (let y = 0; y < measuredHeight; y++) {
      for (let x = 0; x < measuredWidth; x++) {
        if (sourcePixels[(y * measuredWidth + x) * 4 + 3] >= 24) {
          minX = Math.min(minX, x);
          minY = Math.min(minY, y);
          maxX = Math.max(maxX, x);
          maxY = Math.max(maxY, y);
        }
      }
    }
    if (maxX < minX || maxY < minY) throw new Error(`浏览器字库无法生成字符“${character}”`);

    const normalizedCjk = forceCjkCell || this.isCjkCharacter(character);
    const sourceWidth = maxX - minX + 1;
    const sourceHeight = maxY - minY + 1;
    const fitScale = Math.min(1 / sourceScale, this.MAX_GLYPH_DIM / sourceWidth,
      this.MAX_GLYPH_DIM / sourceHeight);
    const inkWidth = normalizedCjk ? spec.cellWidth :
      Math.max(1, Math.min(this.MAX_GLYPH_DIM, Math.round(sourceWidth * fitScale)));
    const inkHeight = normalizedCjk ? spec.cellHeight :
      Math.max(1, Math.min(this.MAX_GLYPH_DIM, Math.round(sourceHeight * fitScale)));
    const width = inkWidth;
    const height = inkHeight;

    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext('2d', { willReadFrequently: true });
    if (!context) throw new Error('当前浏览器无法生成缺字点阵');
    context.clearRect(0, 0, width, height);
    context.imageSmoothingEnabled = true;
    context.imageSmoothingQuality = 'high';
    const destX = 0;
    const destY = 0;
    context.drawImage(scratch, minX, minY, sourceWidth, sourceHeight,
      destX, destY, inkWidth, inkHeight);

    const pixels = context.getImageData(0, 0, width, height).data;
    const rowBytes = Math.ceil(width / 8);
    const bitmap = new Uint8Array(rowBytes * height);
    let setPixels = 0;
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const alpha = pixels[(y * width + x) * 4 + 3];
        if (alpha >= 64) {
          bitmap[y * rowBytes + (x >> 3)] |= 0x80 >> (x & 7);
          setPixels++;
        }
      }
    }
    if (setPixels === 0) throw new Error(`浏览器字库无法生成字符“${character}”`);
    return {
      character,
      codepoint: character.codePointAt(0),
      pixelSize,
      width,
      height,
      advance: normalizedCjk ? spec.advance :
        Math.max(1, Math.min(this.MAX_GLYPH_DIM, Math.round((metrics.width || sourceWidth) / sourceScale))),
      xOffset: normalizedCjk ? 0 : -Math.round(left / sourceScale),
      yOffset: normalizedCjk ? spec.yOffset : -Math.round(ascent / sourceScale),
      bitmap
    };
  },

  bitmapSignature(glyph) {
    return `${glyph.width}:${glyph.height}:${glyph.advance}:${glyph.xOffset}:${glyph.yOffset}:` +
      Array.from(glyph.bitmap).join(',');
  },

  rasterizeGlyph(character, pixelSize) {
    const glyph = this.rasterizeCanvasGlyph(character, pixelSize);
    if (!this.tofuSignatures.has(pixelSize)) {
      const signatures = [];
      for (const sentinel of ['\uFFFF', '\uFFFD', '\uE000']) {
        try {
          signatures.push(this.bitmapSignature(this.rasterizeCanvasGlyph(sentinel, pixelSize, true)));
        } catch (_) {
          /* Some sentinels are intentionally non-renderable. */
        }
      }
      this.tofuSignatures.set(pixelSize, signatures);
    }
    if (this.tofuSignatures.get(pixelSize).includes(this.bitmapSignature(glyph))) {
      throw new Error(`浏览器自身也缺少字符“${character}”，无法同步点阵`);
    }
    return glyph;
  },

  collectFallbackGlyphs() {
    const required = new Map();
    for (const item of this.annotations) {
      item.fontSize = this.normalizeFontSize(item.fontSize);
      item.color = this.normalizeColor(item.color);
      const analysis = this.analyzeLabel(item.label || '', item.fontSize);
      if (analysis.unsupported.length > 0) {
        throw new Error(`${item.key} 包含设备不支持的字符：${analysis.unsupported.join('')}`);
      }
      if (this.labelByteLength(analysis.normalized) > this.MAX_LABEL_BYTES) {
        throw new Error(`${item.key} 的短标签超过 ${this.MAX_LABEL_BYTES} 个 UTF-8 字节`);
      }
      item.label = analysis.normalized;
      for (const missing of analysis.missing) {
        required.set(`${missing.codepoint}:${missing.pixelSize}`, missing);
      }
    }
    if (required.size > this.MAX_GLYPHS) {
      throw new Error(`缺字点阵共 ${required.size} 个，超过设备上限 ${this.MAX_GLYPHS}`);
    }
    return [...required.values()]
      .sort((a, b) => a.codepoint - b.codepoint || a.pixelSize - b.pixelSize)
      .map(item => this.rasterizeGlyph(item.character, item.pixelSize));
  },

  encode() {
    if (this.annotations.length > this.MAX_RECORDS) throw new Error(`最多保存 ${this.MAX_RECORDS} 条日期标注`);
    const glyphs = this.collectFallbackGlyphs();
    const glyphTableOffset = this.HEADER_SIZE + this.annotations.length * this.RECORD_SIZE;
    const bitmapOffset = glyphTableOffset + glyphs.length * this.GLYPH_RECORD_SIZE;
    const bitmapLength = glyphs.reduce((sum, glyph) => sum + glyph.bitmap.length, 0);
    const bytes = new Uint8Array(bitmapOffset + bitmapLength);
    const view = new DataView(bytes.buffer);
    view.setUint32(0, this.MAGIC, true);
    bytes[4] = this.VERSION;
    bytes[5] = this.RECORD_SIZE;
    view.setUint16(6, this.annotations.length, true);
    bytes[8] = this.GLYPH_FORMAT;
    bytes[9] = this.GLYPH_RECORD_SIZE;
    view.setUint16(10, glyphs.length, true);
    view.setUint32(12, bitmapOffset, true);
    const encoder = this.encoder();
    this.annotations.forEach((item, index) => {
      const offset = this.HEADER_SIZE + index * this.RECORD_SIZE;
      const labelBytes = encoder.encode(item.label || '');
      if (labelBytes.length > this.MAX_LABEL_BYTES) throw new Error(`${item.key} 的短标签过长`);
      view.setUint16(offset, item.year, true);
      bytes[offset + 2] = item.month;
      bytes[offset + 3] = item.day;
      bytes[offset + 4] = 1 | (this.fontSizeToMode(item.fontSize) << 1) |
        (this.normalizeColor(item.color) << 3);
      bytes[offset + 5] = labelBytes.length;
      bytes.set(labelBytes, offset + 6);
    });

    let nextBitmapOffset = bitmapOffset;
    glyphs.forEach((glyph, index) => {
      const offset = glyphTableOffset + index * this.GLYPH_RECORD_SIZE;
      if (nextBitmapOffset > 0xFFFF) throw new Error('日期标注字形资源偏移超出固件格式限制');
      view.setUint16(offset, glyph.codepoint, true);
      bytes[offset + 2] = glyph.pixelSize;
      bytes[offset + 3] = glyph.width;
      bytes[offset + 4] = glyph.height;
      bytes[offset + 5] = glyph.advance;
      view.setInt8(offset + 6, glyph.xOffset);
      view.setInt8(offset + 7, glyph.yOffset);
      bytes[offset + 8] = glyph.bitmap.length;
      view.setUint16(offset + 10, nextBitmapOffset, true);
      bytes.set(glyph.bitmap, nextBitmapOffset);
      nextBitmapOffset += glyph.bitmap.length;
    });
    return { bytes, glyphCount: glyphs.length };
  },

  async load() {
    try {
      if (ResourceManager.isBusy) throw new Error('资源操作正在进行，请稍后读取日期标注');
      if (!ResourceManager.isConnected()) throw new Error('请先连接设备');
      this.log('正在读取日期标注…');
      this.log('正在读取资源目录（manifest）…');
      const entries = await this.ensureDeviceEntries(true);
      const entry = entries.find(item => item.type === this.TYPE && item.id === this.ID);
      if (!entry) {
        this.annotations = [];
        this.render();
        this.log('设备还没有日期标注资源，可直接新增并保存');
        return;
      }
      this.decode(await ResourceManager.readRaw(entry.offset, entry.length));
      this.render();
      this.log(`已读取 ${this.annotations.length} 条日期标注`);
    } catch (error) {
      this.report(error);
    }
  },

  async ensureDeviceEntries(forceRead = false) {
    if (!forceRead && ResourceManager.deviceEntries) return ResourceManager.deviceEntries;
    const bundle = ResourceManager.getBundle();
    const manifestBytes = await ResourceManager.readRaw(bundle.manifestAddress, bundle.manifest.length);
    const manifest = ResourceManager.parseManifest(manifestBytes);
    if (!manifest.valid) {
      ResourceManager.logReadTrace('日期标注读取时资源目录无效');
      throw new Error(manifest.reason || '设备资源列表无效');
    }
    ResourceManager.deviceEntries = manifest.entries;
    return manifest.entries;
  },

  ensureDeviceFontsMatchBundle() {
    const bundle = ResourceManager.getBundle();
    for (const spec of this.FONT_SPECS) {
      const expected = bundle.entries.find(item => item.type === 1 && item.id === spec.id);
      const actual = ResourceManager.deviceEntries &&
        ResourceManager.deviceEntries.find(item => item.type === 1 && item.id === spec.id);
      if (!expected || !actual || expected.length !== actual.length || expected.crc32 !== actual.crc32) {
        throw new Error(`设备 ${spec.name} 字库与网页资源包不一致，请先点击“更新资源”，再保存日期标注`);
      }
    }
  },

  async save() {
    try {
      if (ResourceManager.isBusy) throw new Error('资源操作正在进行，请稍后保存日期标注');
      if (!ResourceManager.isConnected()) throw new Error('请先连接设备');
      await this.ensureDeviceEntries();
      this.sort();
      this.ensureDeviceFontsMatchBundle();
      const encoded = this.encode();
      const data = encoded.bytes;
      const bundle = ResourceManager.getBundle();
      const offset = await ResourceManager.determineSafeOffset(this.TYPE, this.ID, data.length,
        ResourceManager.deviceEntries, bundle);
      const target = {
        type: this.TYPE,
        id: this.ID,
        offset,
        length: data.length,
        crc32: ResourceManager.crc32(data),
        name: 'calendar_annotations'
      };
      await ResourceManager.updateResources(target, data);
      await this.refreshCalendar();
      this.log(`已保存 ${this.annotations.length} 条日期标注，并同步 ${encoded.glyphCount} 个缺字点阵`);
    } catch (error) {
      this.report(error);
    }
  },

  async refreshCalendar() {
    if (typeof write !== 'function' || typeof EpdCmd === 'undefined' || typeof buildTimeData !== 'function') return;
    const compensation = typeof getEstimatedRefreshSeconds === 'function' ? getEstimatedRefreshSeconds() : 0;
    /* Omit the optional mode byte.  Firmware then redraws annotations while
     * retaining its configured display mode instead of trusting a stale value
     * cached by the webpage. */
    const ok = await write(EpdCmd.SET_TIME, buildTimeData(null, compensation), true);
    if (!ok) this.log('标注已保存；自动刷新未触发，请稍后等待设备下一次刷新');
  },

  escape(text) {
    const node = document.createElement('span');
    node.textContent = text;
    return node.innerHTML;
  },

  render() {
    const list = document.getElementById('calendarAnnotationsList');
    const count = document.getElementById('calendarAnnotationCount');
    if (count) count.textContent = `${this.annotations.length}/${this.MAX_RECORDS}`;
    if (!list) return;
    if (this.annotations.length === 0) {
      list.innerHTML = '<tr><td colspan="5">暂无标注</td></tr>';
      return;
    }
    list.innerHTML = this.annotations.map(item => `<tr><td>${this.escape(item.key)}</td><td>${this.escape(item.label || '仅红点')}</td><td>${this.escape(this.fontSizeName(item.fontSize))}</td><td>${this.escape(this.colorName(item.color))}</td><td><button type="button" class="secondary" data-date="${item.key}">删除</button></td></tr>`).join('');
    list.querySelectorAll('button[data-date]').forEach(button => {
      button.addEventListener('click', () => this.remove(button.dataset.date));
    });
  },

  log(message) {
    if (typeof addLog === 'function') addLog(message);
    if (typeof showToast === 'function') showToast(message);
  },

  report(error) {
    console.error(error);
    this.log(error.message || String(error));
  }
};

document.addEventListener('DOMContentLoaded', () => CalendarAnnotations.init());
