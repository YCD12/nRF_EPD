(function (root) {
  'use strict';
  const builder = typeof module !== 'undefined' && module.exports ? require('./resource_builder.js') : root.EPDResourceBuilder;
  const uiModel = typeof module !== 'undefined' && module.exports ? require('./ui_tools/ui_model.js') : root.EPDUIModel;
  const firmwarePolicy = typeof module !== 'undefined' && module.exports ? require('./main.js') : root.EpdFirmwarePolicy;

  // Times are seconds on the device's local wall clock, as used by SYNC_TIME.
  // They are intentionally independent of the browser's UTC offset.
  const TodoCalendar = {
    MAGIC: 0x4f444f54,
    VERSION: 1,
    get MIN_VERSION() { return firmwarePolicy.newWebMinVersion; },
    get FULL_MIN_VERSION() { return firmwarePolicy.newWebMinVersion; },
    get TYPOGRAPHY_MIN_VERSION() { return firmwarePolicy.newWebMinVersion; },
    get TIMING_MIN_VERSION() { return firmwarePolicy.newWebMinVersion; },
    FONT_SIZES: [9, 12, 16, 20, 24, 32],
    HEADER_SIZE: 16,
    RECORD_SIZE: 80,
    MAX_RECORDS: 32,
    MAX_TEXT_BYTES: 64,
    STORAGE_KEY: 'epd_todo_calendar_v1',
    items: [],
    limit: 5,
    fontSize: 12,
    fontFamily: '',
    fontBold: false,
    fontItalic: false,
    leadMinutes: 10,
    retainMinutes: 5,
    previewMode: 4,
    _editingId: null,
    _revision: 0,
    _initialized: false,
    _composing: false,
    _importToken: 0,
    _temperature: null,

    element(id) { return typeof document === 'undefined' ? null : document.getElementById(id); },
    minimumFirmwareLabel() { return `0x${firmwarePolicy.newWebMinVersion.toString(16).toUpperCase()}`; },
    value(id, fallback = '') { const input = this.element(id); return input ? input.value : fallback; },
    checked(id) { return !!this.element(id)?.checked; },
    report(message, error = false) {
      const status = this.element('todoCalendarStatus');
      if (status) { status.textContent = message; status.classList.toggle('error', error); }
      if (typeof addLog === 'function') addLog(message);
    },
    textByteLength(text) { return new TextEncoder().encode(text).length; },
    normalizeFontFamily(value = '') {
      if (typeof value !== 'string') throw new Error('待办字体类型必须是字体名称');
      const family = value.trim();
      if (family && (this.textByteLength(family) > 64 || !/^[\p{L}\p{N}\p{Zs}_.-]+$/u.test(family)))
        throw new Error('待办字体名称无效，请选择列表中的字体');
      return family;
    },
    assertText(text) {
      if (typeof text !== 'string') throw new Error('待办内容必须是文字');
      const normalized = text.trim();
      if (!normalized) throw new Error('请填写待办事项');
      if (/\p{Cf}/u.test(text)) throw new Error('暂不支持不可见格式字符，请使用基本平面文字');
      if (this.textByteLength(normalized) > this.MAX_TEXT_BYTES)
        throw new Error('每项最多 64 个 UTF-8 字节（中文通常最多 21 字）');
      for (const ch of text) {
        const code = ch.codePointAt(0);
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f) || code > 0xffff ||
            (code >= 0xd800 && code <= 0xdfff) || code === 0xfffe || code === 0xffff)
          throw new Error('暂不支持表情、扩展平面文字或控制字符，请使用基本平面文字');
      }
      return normalized;
    },
    localNow(date = new Date()) {
      return Math.floor(Date.UTC(date.getFullYear(), date.getMonth(), date.getDate(),
        date.getHours(), date.getMinutes(), date.getSeconds()) / 1000);
    },
    parseTime(value) {
      if (typeof value === 'number') {
        if (!Number.isInteger(value) || value < 0 || value > 0xffffffff)
          throw new Error('时间超出设备支持范围');
        return value;
      }
      const match = typeof value === 'string' && /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?$/.exec(value);
      if (!match) throw new Error('请填写完整的触发时间和截止时间');
      const [year, month, day, hour, minute, second] = match.slice(1).map(part => Number(part || 0));
      if (year < 2000 || year > 2099) throw new Error('日期范围为 2000 至 2099 年');
      const millis = Date.UTC(year, month - 1, day, hour, minute, second);
      const date = new Date(millis);
      if (date.getUTCFullYear() !== year || date.getUTCMonth() !== month - 1 ||
          date.getUTCDate() !== day || date.getUTCHours() !== hour ||
          date.getUTCMinutes() !== minute || date.getUTCSeconds() !== second)
        throw new Error('时间或日期无效');
      return Math.floor(millis / 1000);
    },
    formatTime(seconds) {
      if (seconds === null || seconds === undefined || seconds === '') return '';
      const date = new Date(seconds * 1000), pad = value => String(value).padStart(2, '0');
      return `${date.getUTCFullYear()}-${pad(date.getUTCMonth() + 1)}-${pad(date.getUTCDate())}T${pad(date.getUTCHours())}:${pad(date.getUTCMinutes())}:${pad(date.getUTCSeconds())}`;
    },
    displayTime(seconds) { return this.formatTime(seconds).replace('T', ' ').replace(/:00$/, ''); },
    normalizeItem(item) {
      if (!item || typeof item !== 'object' || Array.isArray(item)) throw new Error('待办记录格式无效');
      if (!Number.isInteger(item.id) || item.id < 1 || item.id > 0xffffffff) throw new Error('待办 ID 无效');
      if (typeof item.resident !== 'boolean' || typeof item.completed !== 'boolean') throw new Error('待办状态无效');
      const text = this.assertText(item.text);
      const start = item.resident ? 0 : this.parseTime(item.start);
      const end = item.resident ? 0 : this.parseTime(item.end);
      if (!item.resident && end <= start) throw new Error('截止时间必须晚于触发时间');
      return { id: item.id, start, end, resident: item.resident, completed: item.completed, text };
    },
    normalizeConfig(config) {
      if (!config || typeof config !== 'object' || !Array.isArray(config.items)) throw new Error('待办配置格式无效');
      if (!Number.isInteger(config.limit) || config.limit < 1 || config.limit > 8) throw new Error('屏幕显示数量应为 1 至 8');
      if (!this.FONT_SIZES.includes(config.fontSize)) throw new Error('待办字号应为 9、12、16、20、24 或 32');
      for (const field of ['fontBold', 'fontItalic'])
        if (config[field] !== undefined && typeof config[field] !== 'boolean') throw new Error('待办字形设置应为开关');
      const leadMinutes = config.leadMinutes === undefined ? 10 : config.leadMinutes;
      const retainMinutes = config.retainMinutes === undefined ? 5 : config.retainMinutes;
      for (const value of [leadMinutes, retainMinutes])
        if (!Number.isInteger(value) || value < 0 || value > 1440) throw new Error('显示提前时间和过期保留时间应为 0 至 1440 分钟');
      const maximum = this.maxVisible(config);
      if (config.limit > maximum) throw new Error(`当前字号最多显示 ${maximum} 项，请减少屏幕显示数量`);
      if (config.items.length > this.MAX_RECORDS) throw new Error('最多保存 32 条待办事项');
      const items = config.items.map(item => this.normalizeItem(item));
      if (new Set(items.map(item => item.id)).size !== items.length) throw new Error('待办 ID 重复');
      const normalized = { items, limit: config.limit, fontSize: config.fontSize, leadMinutes, retainMinutes };
      for (const field of ['fontBold', 'fontItalic']) if (config[field] !== undefined) normalized[field] = config[field];
      return normalized;
    },
    usesTypography(config = this) { return config.fontSize > 12 || config.fontBold === true || config.fontItalic === true; },
    maxVisible(config = this) {
      const selected=root.ExternalResourceEditor?.previewModel?.();
      const slot=this.usesTypography(config)||config.fontFamily?3:config.fontSize===9?0:1;
      const pixels=Math.ceil(config.fontSize*4/3);
      const height=this.usesTypography(config)?pixels+Math.ceil(pixels/3):selected?.fonts[slot]?.[10]||(config.fontSize===9?12:16);
      return uiModel.todoCapacity(config.ui||selected?.ui||uiModel.defaults(),height,this.usesTypography(config)||height>16);
    },
    compare(a, b) {
      return Number(b.resident) - Number(a.resident) || a.start - b.start || a.end - b.end || a.id - b.id;
    },
    phase(item, now = this.localNow()) {
      if (item.completed) return 'hidden';
      if (item.resident) return 'resident';
      if (now < Math.max(0, item.start - this.leadMinutes * 60)) return 'hidden';
      if (now < item.start) return 'upcoming';
      if (now < item.end) return 'active';
      return now - item.end < this.retainMinutes * 60 ? 'expired' : 'hidden';
    },
    activeItems(now = this.localNow()) {
      return this.items.filter(item => this.phase(item, now) !== 'hidden')
        .slice().sort((a, b) => this.compare(a, b));
    },
    visibleItems(now = this.localNow()) { return this.activeItems(now).slice(0, this.limit); },
    dailyCount(now = this.localNow()) {
      const dayStart = Math.floor(now / 86400) * 86400, dayEnd = dayStart + 86400;
      return this.items.filter(item => !item.completed &&
        (item.resident || (Math.max(0, item.start - this.leadMinutes * 60) < dayEnd && item.end + this.retainMinutes * 60 > dayStart))).length;
    },
    setTemperature(value) {
      this._temperature = Number.isInteger(value) && value >= -128 && value <= 127 ? value : null;
      this.renderHeader(this.previewNow()); this.renderResourcePreview(this.previewNow());
    },
    renderHeader(now) {
      const title = this.element('todoCalendarPreviewTitle'), total = this.dailyCount(now), current = this.visibleItems(now).length;
      if (title) {
        title.textContent = `TODO ${current}/${total}`;
        title.setAttribute('aria-label', `当前显示 ${current} 项，当天未完成待办总数 ${total}`);
      }
      const temperature = this.element('todoCalendarPreviewTemperature');
      if (temperature) {
        const number = temperature.querySelector?.('.todo-calendar-temperature-value');
        if (number) number.textContent = this._temperature === null ? '—' : String(this._temperature);
        else temperature.textContent = this._temperature === null ? '—℃' : `${this._temperature}℃`;
        temperature.setAttribute('aria-label', this._temperature === null ? '设备温度未知' : `设备温度 ${this._temperature} 摄氏度`);
      }
    },
    nextId() {
      const used = new Set(this.items.map(item => item.id));
      let candidate = Math.max(0, ...used) + 1;
      if (candidate <= 0xffffffff) return candidate;
      candidate = 1;
      while (used.has(candidate)) ++candidate;
      return candidate;
    },
    pack(config = this) {
      const normalized = this.normalizeConfig(config);
      const items = normalized.items.slice().sort((a, b) => this.compare(a, b));
      const family = this.normalizeFontFamily(config.fontFamily), familyBytes = new TextEncoder().encode(family);
      const typography = this.usesTypography(normalized), trailer = this.HEADER_SIZE + items.length * this.RECORD_SIZE;
      const bytes = new Uint8Array(trailer + 5 + familyBytes.length);
      const view = new DataView(bytes.buffer);
      view.setUint32(0, this.MAGIC, true);
      bytes[4] = 4; bytes[5] = this.RECORD_SIZE;
      view.setUint16(6, items.length, true);
      bytes[8] = normalized.limit; bytes[9] = normalized.fontSize;
      if (family || typography) {
        bytes[10] = 3; bytes[11] = familyBytes.length;
        view.setUint32(12, typography ? builder.todoFontKey(family, normalized.fontSize, !!normalized.fontBold, !!normalized.fontItalic)
          : builder.fontKey(family, normalized.fontSize === 9 ? 0 : 1), true);
      }
      bytes[trailer] = Number(!!normalized.fontBold) | (Number(!!normalized.fontItalic) << 1);
      view.setUint16(trailer + 1, normalized.leadMinutes, true);
      view.setUint16(trailer + 3, normalized.retainMinutes, true);
      bytes.set(familyBytes, trailer + 5);
      items.forEach((item, index) => {
        const offset = this.HEADER_SIZE + index * this.RECORD_SIZE;
        const text = new TextEncoder().encode(item.text);
        view.setUint32(offset, item.id, true);
        view.setUint32(offset + 4, item.start, true);
        view.setUint32(offset + 8, item.end, true);
        bytes[offset + 12] = Number(item.resident) | (Number(item.completed) << 1);
        bytes[offset + 13] = text.length;
        bytes.set(text, offset + 14);
      });
      return bytes;
    },
    unpack(input) {
      const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
      if (bytes.length < this.HEADER_SIZE) throw new Error('待办数据不完整');
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      const timed = bytes[4] === 4, styled = bytes[4] === 3 || timed;
      const custom = timed ? bytes[10] === 3 : bytes[4] === 2 || bytes[4] === 3;
      if (view.getUint32(0, true) !== this.MAGIC || ![1, 2, 3, 4].includes(bytes[4]) || bytes[5] !== this.RECORD_SIZE)
        throw new Error('待办数据版本或格式无效');
      const count = view.getUint16(6, true);
      const trailer = this.HEADER_SIZE + count * this.RECORD_SIZE;
      const tailSize = timed ? 5 : styled ? 1 : 0;
      if (count > this.MAX_RECORDS || bytes.length !== trailer + (custom ? bytes[11] : 0) + tailSize ||
          (custom ? bytes[10] !== 3 || (bytes[4] === 2 && !bytes[11]) || bytes[11] > 64 || !view.getUint32(12, true) : bytes.subarray(10, 16).some(value => value !== 0)) ||
          (!styled && bytes[9] !== 9 && bytes[9] !== 12) || (styled && (bytes[trailer] & ~3)))
        throw new Error('待办数据长度或保留字段无效');
      const items = [];
      const decoder = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
      for (let index = 0; index < count; ++index) {
        const offset = this.HEADER_SIZE + index * this.RECORD_SIZE, flags = bytes[offset + 12], length = bytes[offset + 13];
        if (flags & ~3 || !length || length > this.MAX_TEXT_BYTES ||
            bytes.subarray(offset + 14 + length, offset + this.RECORD_SIZE).some(value => value !== 0))
          throw new Error('待办记录状态或文字长度无效');
        const start = view.getUint32(offset + 4, true), end = view.getUint32(offset + 8, true);
        if ((flags & 1) && (start || end)) throw new Error('常驻待办时间应为零');
        const text = decoder.decode(bytes.subarray(offset + 14, offset + 14 + length));
        if (text !== this.assertText(text)) throw new Error('待办文字格式无效');
        items.push({ id: view.getUint32(offset, true), start, end, resident: !!(flags & 1), completed: !!(flags & 2), text });
      }
      const style = styled ? { fontBold: !!(bytes[trailer] & 1), fontItalic: !!(bytes[trailer] & 2) } : {};
      const config = this.normalizeConfig({ items, limit: bytes[8], fontSize: bytes[9], ...style,
        leadMinutes: timed ? view.getUint16(trailer + 1, true) : 0, retainMinutes: timed ? view.getUint16(trailer + 3, true) : 0 });
      const typography = this.usesTypography(config);
      if (bytes[4] === 3 && !typography) throw new Error('待办字形版本与设置不符');
      if (timed && typography && !custom) throw new Error('待办字形缺少专用字库');
      if (custom) {
        const family = decoder.decode(bytes.subarray(trailer + tailSize));
        config.fontFamily = this.normalizeFontFamily(family);
        if (timed && !typography && !family) throw new Error('待办字体类型与字库设置不符');
        const key = typography ? builder.todoFontKey(family, config.fontSize, config.fontBold, config.fontItalic)
          : builder.fontKey(family, config.fontSize === 9 ? 0 : 1);
        if (family !== config.fontFamily || key !== view.getUint32(12, true))
          throw new Error('待办字体名称或标识无效');
      }
      return config;
    },
    fontRequests(source = this) {
      const config = this.normalizeConfig(source);
      const ui=source.ui||root.ExternalResourceEditor?.previewModel?.()?.ui||uiModel.defaults();
      const ellipsis=[...new Set(ui.components.filter(c=>c.kind===6).map(c=>c.text[2]))].join('');
      const text = config.items.map(item => item.text).join('');
      const fontFamily = this.normalizeFontFamily(source.fontFamily);
      // Match the firmware's three-dot truncation marker, including its glyph.
      if (this.usesTypography(config)) return [{ slot: 3, sourceSlot: config.fontSize === 9 ? 0 : 1, text: ' ' + ellipsis + text,
        fontFamily, fontSize: config.fontSize, fontBold: !!config.fontBold, fontItalic: !!config.fontItalic }];
      if (fontFamily) return [{ slot: 3, sourceSlot: config.fontSize === 9 ? 0 : 1, text: ' ' + ellipsis + text, fontFamily }];
      return text ? [{ slot: config.fontSize === 9 ? 0 : 1, text: ellipsis + text }] : [];
    },
    changed() {
      ++this._revision;
      this.persist(); this.render();
    },
    persist() {
      try {
        if (typeof localStorage !== 'undefined') localStorage.setItem(this.STORAGE_KEY, JSON.stringify({
          version: this.VERSION, items: this.items, limit: this.limit, fontSize: this.fontSize, fontFamily: this.fontFamily,
          fontBold: this.fontBold, fontItalic: this.fontItalic, leadMinutes: this.leadMinutes, retainMinutes: this.retainMinutes, previewMode: this.previewMode,
          editor: { id: this._editingId, text: this.value('todoCalendarText'), start: this.value('todoCalendarStart'),
            end: this.value('todoCalendarEnd'), resident: this.checked('todoCalendarResident'), completed: this.checked('todoCalendarCompleted') },
        }));
      } catch (_) { this.report('浏览器无法保存本地草稿，请导出备份；设备保存仍可使用。', true); }
    },
    loadLocal() {
      try {
        if (typeof localStorage === 'undefined') return;
        const raw = localStorage.getItem(this.STORAGE_KEY);
        if (!raw) return;
        const parsed = JSON.parse(raw);
        if (parsed.version !== this.VERSION) throw new Error('草稿版本无效');
        const config = this.normalizeConfig(parsed), fontFamily = this.normalizeFontFamily(parsed.fontFamily);
        Object.assign(this, config, { fontFamily, fontBold: config.fontBold || false, fontItalic: config.fontItalic || false });
        this.previewMode = parsed.previewMode === 5 ? 5 : 4;
        const editor = parsed.editor;
        if (editor && typeof editor === 'object') {
          for (const [field, key] of [['todoCalendarText', 'text'], ['todoCalendarStart', 'start'], ['todoCalendarEnd', 'end']]) {
            const input = this.element(field);
            if (input && typeof editor[key] === 'string') input.value = editor[key];
          }
          const resident = this.element('todoCalendarResident'), completed = this.element('todoCalendarCompleted');
          if (resident) resident.checked = editor.resident === true;
          if (completed) completed.checked = editor.completed === true;
          if (this.items.some(item => item.id === editor.id)) this._editingId = editor.id;
        }
      } catch (error) { this.report(`本地草稿未加载：${error.message}`, true); }
    },
    updateEditor() {
      const resident = this.checked('todoCalendarResident');
      for (const id of ['todoCalendarStart', 'todoCalendarEnd']) { const input = this.element(id); if (input) input.disabled = resident; }
      const submit = this.element('todoCalendarSubmit'), cancel = this.element('todoCalendarCancel');
      if (submit) submit.textContent = this._editingId === null ? '添加事项' : '更新事项';
      if (cancel) cancel.hidden = this._editingId === null;
      const hint = this.element('todoCalendarTextHint');
      if (hint) {
        const bytes = this.textByteLength(this.value('todoCalendarText'));
        hint.textContent = `已用 ${bytes} / 64 字节 · 中文通常最多 21 字`;
        hint.classList.toggle('error', bytes > this.MAX_TEXT_BYTES);
      }
    },
    resetEditor(persist = true) {
      if (persist) ++this._revision;
      this._editingId = null;
      const now = Math.floor(this.localNow() / 60) * 60;
      for (const [id, value] of [['todoCalendarText', ''], ['todoCalendarStart', this.formatTime(now)], ['todoCalendarEnd', this.formatTime(now + 3600)]]) {
        const input = this.element(id); if (input) input.value = value;
      }
      for (const id of ['todoCalendarResident', 'todoCalendarCompleted']) { const input = this.element(id); if (input) input.checked = false; }
      this.updateEditor();
      if (persist) this.persist();
    },
    addOrUpdate() {
      try {
        if (this._composing) throw new Error('请先完成事项文字输入');
        const item = this.normalizeItem({ id: this._editingId ?? this.nextId(), text: this.value('todoCalendarText'),
          start: this.value('todoCalendarStart'), end: this.value('todoCalendarEnd'),
          resident: this.checked('todoCalendarResident'), completed: this.checked('todoCalendarCompleted') });
        const index = this.items.findIndex(old => old.id === item.id);
        if (index < 0) {
          if (this.items.length >= this.MAX_RECORDS) throw new Error('最多保存 32 条待办事项，请删除不再需要的事项');
          this.items.push(item);
        } else this.items[index] = item;
        this.resetEditor(false); this.changed();
        this.report('事项已更新到本地草稿，保存到设备后生效。');
        return true;
      } catch (error) { this.report(error.message, true); return false; }
    },
    edit(id) {
      const item = this.items.find(entry => entry.id === id); if (!item) return;
      ++this._revision;
      this._editingId = id;
      const now = Math.floor(this.localNow() / 60) * 60;
      for (const [field, value] of [['todoCalendarText', item.text], ['todoCalendarStart', this.formatTime(item.resident ? now : item.start)], ['todoCalendarEnd', this.formatTime(item.resident ? now + 3600 : item.end)]]) {
        const input = this.element(field); if (input) input.value = value;
      }
      for (const [field, value] of [['todoCalendarResident', item.resident], ['todoCalendarCompleted', item.completed]]) {
        const input = this.element(field); if (input) input.checked = value;
      }
      this.updateEditor(); this.persist(); this.render(); this.element('todoCalendarText')?.focus();
    },
    remove(id) {
      this.items = this.items.filter(item => item.id !== id);
      if (this._editingId === id) this.resetEditor(false);
      this.changed();
    },
    toggleComplete(id) {
      const item = this.items.find(entry => entry.id === id);
      if (!item) return;
      item.completed = !item.completed;
      if (this._editingId === id) { const input = this.element('todoCalendarCompleted'); if (input) input.checked = item.completed; }
      this.changed();
    },
    setOptions() {
      try {
        const options = { items: this.items, limit: Number(this.value('todoCalendarLimit', this.limit)),
          fontSize: Number(this.value('todoCalendarFontSize', this.fontSize)),
          fontBold: this.element('todoCalendarFontBold') ? this.checked('todoCalendarFontBold') : this.fontBold,
          fontItalic: this.element('todoCalendarFontItalic') ? this.checked('todoCalendarFontItalic') : this.fontItalic,
          leadMinutes: Number(this.value('todoCalendarLeadMinutes', this.leadMinutes)),
          retainMinutes: Number(this.value('todoCalendarRetainMinutes', this.retainMinutes)) };
        if (!Number.isInteger(options.limit) || options.limit < 1 || options.limit > 8) throw new Error('屏幕显示数量应为 1 至 8');
        const maximum = this.maxVisible(options), adjusted = options.limit > maximum;
        if (adjusted) options.limit = maximum;
        const next = this.normalizeConfig(options);
        const fontFamily = this.normalizeFontFamily(this.value('todoCalendarFontFamily', this.fontFamily));
        Object.assign(this, next, { fontFamily }); this.syncOptions(true); this.changed();
        if (adjusted) this.report(`字号 ${this.fontSize} 最多显示 ${maximum} 项，屏幕显示数量已调整为 ${maximum}。`);
      } catch (error) { this.syncOptions(true); this.report(error.message, true); }
    },
    syncOptions(force = false) {
      const signature = `${this.limit}/${this.fontSize}/${this.fontFamily}/${this.fontBold}/${this.fontItalic}/${this.leadMinutes}/${this.retainMinutes}`;
      if (!force && this._optionsSignature === signature) return;
      this._optionsSignature = signature;
      this.ensureFontOption(this.fontFamily);
      for (const [id, value] of [['todoCalendarLimit', this.limit], ['todoCalendarFontSize', this.fontSize], ['todoCalendarFontFamily', this.fontFamily],
        ['todoCalendarLeadMinutes', this.leadMinutes], ['todoCalendarRetainMinutes', this.retainMinutes]]) {
        const input = this.element(id); if (input) input.value = String(value);
      }
      for (const [id, value] of [['todoCalendarFontBold', this.fontBold], ['todoCalendarFontItalic', this.fontItalic]]) {
        const input = this.element(id); if (input) input.checked = !!value;
      }
      const limit = this.element('todoCalendarLimit'); if (limit) limit.max = String(this.maxVisible());
      const hint = this.element('todoCalendarFontSizeHint');
      if (hint) hint.textContent = `当前字号最多显示 ${this.maxVisible()} 项。大字号、加粗及斜体需要固件 ${this.minimumFirmwareLabel()}。`;
      const timingHint = this.element('todoCalendarTimingHint');
      if (timingHint) timingHint.textContent = `事项会提前 ${this.leadMinutes} 分钟出现，触发后方框变红；截止后保留 ${this.retainMinutes} 分钟并加删除线，再自动隐藏。保存需要固件 ${this.minimumFirmwareLabel()}。`;
      const preview = this.element('todoCalendarPreviewItems');
      if (preview?.style) {
        preview.style.fontFamily = this.fontFamily || '';
        preview.style.fontWeight = this.fontBold ? '700' : '400';
        preview.style.fontStyle = this.fontItalic ? 'italic' : 'normal';
      }
    },
    ensureFontOption(family, label = family || '设备默认') {
      const select = this.element('todoCalendarFontFamily'); if (!select) return;
      const options = Array.from(select.options || select.children || []);
      if (!options.some(option => option.value === family)) {
        const option = this.append(select, 'option', label); option.value = family;
      }
    },
    initFontOptions() {
      const source = this.element('font-family');
      this.ensureFontOption('');
      for (const option of Array.from(source?.options || [])) {
        const family = this.normalizeFontFamily(option.value);
        this.ensureFontOption(family, option.textContent || family);
      }
    },
    assertEditorApplied() {
      let pending = this._composing;
      if (this._editingId === null) pending ||= this.value('todoCalendarText').trim().length > 0;
      else {
        try {
          const item = this.normalizeItem({ id: this._editingId, text: this.value('todoCalendarText'),
            start: this.value('todoCalendarStart'), end: this.value('todoCalendarEnd'),
            resident: this.checked('todoCalendarResident'), completed: this.checked('todoCalendarCompleted') });
          const existing = this.items.find(entry => entry.id === this._editingId);
          pending ||= !existing || JSON.stringify(item) !== JSON.stringify(this.normalizeItem(existing));
        } catch (_) { pending = true; }
      }
      if (pending) throw new Error('表单中的事项尚未加入清单，请先点击“添加事项”或“更新事项”，再保存或导出。');
    },
    setPreviewMode() {
      const value = Number(this.value('todoCalendarLayout', this.previewMode));
      if (value !== 4 && value !== 5) {
        this.report('请选择待办+日历或全屏待办布局。', true); this.syncLayout(true); return false;
      }
      this.previewMode = value; this.syncLayout(true); this.persist(); this.render(); return true;
    },
    syncLayout(force = false) {
      if (!force && this._layoutMode === this.previewMode) return;
      this._layoutMode = this.previewMode;
      const select = this.element('todoCalendarLayout'); if (select) select.value = String(this.previewMode);
      const full = this.previewMode === 5, screen = this.element('todoCalendarPreviewScreen');
      if (screen) screen.dataset.layout = full ? 'full' : 'split';
      const calendar = this.element('todoCalendarPreviewCalendar'); if (calendar) calendar.hidden = full;
      const caption = this.element('todoCalendarPreviewLayout'); if (caption) caption.textContent = full ? '全屏待办' : '左待办 · 右日历';
      const hint = this.element('todoCalendarLayoutHint');
      if (hint) hint.textContent = `${full ? '全屏待办' : '待办+日历'}需要固件 ${this.minimumFirmwareLabel()} 或更新版本。`;
      const activate = this.element('todoCalendarActivate'); if (activate) activate.textContent = full ? '启用全屏待办' : '启用待办+日历';
    },
    async activate() {
      if (!this.requireDevice()) return false;
      try {
        this.assertEditorApplied();
        if (typeof syncTime !== 'function') throw new Error('设备模式模块未加载，请刷新页面。');
        return await syncTime(this.previewMode) === true;
      } catch (error) { this.report(error.message, true); return false; }
    },
    splitLegacyText(text) {
      if (typeof text !== 'string') throw new Error('旧版待办内容必须是文字');
      let chars = Array.from(text.trim());
      if (!chars.length) throw new Error('旧版待办内容为空');
      const parts = [];
      while (chars.length) {
        let used = 0, count = 0;
        while (count < chars.length && used + this.textByteLength(chars[count]) <= this.MAX_TEXT_BYTES) {
          used += this.textByteLength(chars[count]); ++count;
        }
        if (count < chars.length) {
          // Each record is trimmed by the protocol encoder. Keep spaces inside
          // a segment so joining segments preserves the original text.
          while (count > 0 && (/\s/u.test(chars[count - 1]) || /\s/u.test(chars[count]))) --count;
          if (!count) throw new Error('旧版待办含无法无损拆分的长空白内容，旧草稿仍保留，请精简后重试');
        }
        parts.push(this.assertText(chars.slice(0, count).join('')));
        chars = chars.slice(count);
        if (parts.length > this.MAX_RECORDS) throw new Error('单条旧版待办拆分后超过 32 项，旧草稿仍保留，请精简后重试');
      }
      return parts;
    },
    importPaintTodos(manager = typeof paintManager === 'undefined' ? null : paintManager) {
      try {
        const todos = typeof manager?.getLegacyTodoItems === 'function' ? manager.getLegacyTodoItems() : manager?.todoItems;
        if (!Array.isArray(todos) || !todos.length) throw new Error('此浏览器没有可导入的旧版待办草稿。');
        const segments = todos.flatMap(todo => this.splitLegacyText(todo.text).map(text => ({ text, completed: !!todo.completed })));
        if (this.items.length + segments.length > this.MAX_RECORDS)
          throw new Error(`旧版待办需 ${segments.length} 个事项，导入后超过 32 条；当前清单和旧版草稿均已保留，请先减少事项后重试。`);
        // Validate the entire batch before changing the existing local draft.
        const existing = this.items, added = [];
        let id = this.nextId();
        for (const todo of segments) {
          while (existing.some(item => item.id === id) || added.some(item => item.id === id)) id = id === 0xffffffff ? 1 : id + 1;
          added.push(this.normalizeItem({ id, text: todo.text, resident: true, completed: !!todo.completed, start: 0, end: 0 }));
          id = id === 0xffffffff ? 1 : id + 1;
        }
        this.items = [...existing, ...added]; this.changed();
        this.report(`已导入 ${todos.length} 条旧版待办，共 ${added.length} 项常驻事项${added.length > todos.length ? '（长内容已拆分）' : ''}；取消常驻显示后可设置触发和截止时间，再保存到设备。`);
        return true;
      } catch (error) { this.report(error.message, true); return false; }
    },
    importJSON(text) {
      const parsed = JSON.parse(text);
      if (parsed.version !== this.VERSION) throw new Error('待办备份版本无效');
      const next = this.normalizeConfig(parsed);
      const fontFamily = this.normalizeFontFamily(parsed.fontFamily);
      Object.assign(this, next, { fontFamily, fontBold: next.fontBold || false, fontItalic: next.fontItalic || false }); this.resetEditor(false); this.changed();
      this.report(`已导入 ${this.items.length} 条待办到本地草稿，请保存到设备。`);
      return true;
    },
    exportJSON() {
      this.assertEditorApplied();
      const config = this.normalizeConfig(this);
      const exportTime = value => value >= 946684800 && value <= 4102444799 ? this.formatTime(value) : value;
      return JSON.stringify({ version: this.VERSION, limit: config.limit, fontSize: config.fontSize, fontFamily: this.normalizeFontFamily(this.fontFamily),
        fontBold: !!config.fontBold, fontItalic: !!config.fontItalic,
        leadMinutes: config.leadMinutes, retainMinutes: config.retainMinutes,
        items: config.items.map(item => ({ ...item, start: item.resident ? 0 : exportTime(item.start), end: item.resident ? 0 : exportTime(item.end) })) }, null, 2);
    },
    downloadJSON() {
      try {
        const url = URL.createObjectURL(new Blob([this.exportJSON()], { type: 'application/json' }));
        const link = document.createElement('a'); link.href = url; link.download = 'epd-todo-calendar.json'; link.click();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
      } catch (error) { this.report(error.message, true); }
    },
    requireDevice() {
      if (typeof epdCharacteristic === 'undefined' || !epdCharacteristic) {
        this.report('请先连接设备，再读取或保存待办事项。', true); return false;
      }
      if (!firmwarePolicy.isNewFirmware(typeof appVersion === 'undefined' ? undefined : appVersion)) {
        this.report(`待办事项需要固件 ${this.minimumFirmwareLabel()} 或更新版本。`, true); return false;
      }
      if (!root.ExternalResourceInstaller?.saveTodo || !root.ExternalResourceInstaller?.readTodo) {
        this.report('待办资源模块未加载，请刷新页面。', true); return false;
      }
      return true;
    },
    deviceTask(action, parent = null) {
      return typeof runDeviceOperation === 'function' ? runDeviceOperation('todo', action, parent) : action(parent);
    },
    assertOwner(owner) { if (owner && root.DeviceOperations) root.DeviceOperations.assert(owner); },
    ownerCurrent(owner) {
      try { this.assertOwner(owner); return true; }
      catch (_) { return false; }
    },
    async save(parent = null) {
      if (!this.requireDevice()) return false;
      try { this.assertEditorApplied(); }
      catch (error) { this.report(error.message, true); return false; }
      return this.deviceTask(async owner => {
        try {
          const bytes = this.pack(), requests = this.fontRequests(), revision = this._revision;
          this.report('正在保存待办事项并检查设备字库…');
          const ok = await root.ExternalResourceInstaller.saveTodo(bytes, requests, owner, message => {
            if (this.ownerCurrent(owner)) this.report(message);
          });
          this.assertOwner(owner);
          if (ok !== true) throw new Error('设备未确认待办保存成功，请重试');
          this.report(revision === this._revision ? `已保存 ${new DataView(bytes.buffer).getUint16(6, true)} 条待办到设备，触发与截止时间将在离线状态下生效。`
            : '本次待办已保存到设备；本地还有新修改，请再次保存。');
          return true;
        } catch (error) { if (this.ownerCurrent(owner)) this.report(error.message, true); return false; }
      }, parent);
    },
    async loadFromDevice(parent = null) {
      if (!this.requireDevice()) return false;
      return this.deviceTask(async owner => {
        try {
          const revision = this._revision;
          this.report('正在读取设备待办…');
          const bytes = await root.ExternalResourceInstaller.readTodo(owner);
          this.assertOwner(owner);
          if (revision !== this._revision) throw new Error('读取期间本地草稿已修改，请再次读取以替换草稿');
          const config = bytes === null ? { items: [], limit: 5, fontSize: 12, leadMinutes: 10, retainMinutes: 5 } : this.unpack(bytes);
          Object.assign(this, config, { fontFamily: config.fontFamily || '', fontBold: config.fontBold || false, fontItalic: config.fontItalic || false });
          this.resetEditor(false); this.changed();
          this.report(`已从设备读取 ${this.items.length} 条待办。`);
          return true;
        } catch (error) { if (this.ownerCurrent(owner)) this.report(error.message, true); return false; }
      }, parent);
    },
    controls(forceDisabled = false) {
      const ready = typeof epdCharacteristic !== 'undefined' && !!epdCharacteristic &&
        firmwarePolicy.isNewFirmware(typeof appVersion === 'undefined' ? undefined : appVersion);
      const busy = forceDisabled || !!root.DeviceOperations?.current || !!root.ExternalResourceInstaller?.busy?.();
      for (const id of ['todoCalendarSave', 'todoCalendarRead', 'todocalendarmodebutton', 'todofullmodebutton', 'todoCalendarActivate']) {
        const button = this.element(id); if (button) button.disabled = !ready || busy;
      }
    },
    previewNow() {
      const value = this.value('todoCalendarPreviewTime');
      try { return value ? this.parseTime(value) : this.localNow(); }
      catch (_) { return this.localNow(); }
    },
    state(item, now, visible) {
      if (item.completed) return '已完成';
      const phase = this.phase(item, now);
      if (phase === 'hidden') return now < item.start ? '未触发 · 未到提前显示时间' : '已过期 · 已隐藏';
      if (phase === 'upcoming') return visible.has(item.id) ? '提前显示 · 未触发' : '未触发 · 等待空位';
      if (phase === 'expired') return visible.has(item.id) ? '已过期 · 保留显示' : '已过期 · 等待空位';
      if (visible.has(item.id)) return phase === 'resident' ? '常驻显示' : '已触发 · 正在显示';
      return '等待空位';
    },
    append(parent, tag, text, className = '') {
      const node = document.createElement(tag); node.textContent = text;
      if (className) node.className = className;
      parent.appendChild(node); return node;
    },
    previewGeometry() {
      if (this.element('todoCalendarResourceCanvas')?.getContext) return;
      const screen = this.element('todoCalendarPreviewScreen'), preview = this.element('todoCalendarPreviewItems');
      if (!preview?.style) return;
      const scale = screen?.clientWidth ? screen.clientWidth / 800 : 0.6;
      const pixels = Math.ceil(this.fontSize * 4 / 3), height = pixels + Math.ceil(pixels / 3);
      const ui=this.ui||root.ExternalResourceEditor?.previewModel?.()?.ui||uiModel.defaults();
      const pane=ui.components.find(c=>c.kind===6&&(c.modes&(1<<(this.previewMode-1)))), style=pane.style;
      const rowHeight=Math.floor(pane.height/this.limit),lines=rowHeight>=2*height+style.two_line_padding?2:1;
      preview.dataset.typography = String(this.usesTypography());
      preview.dataset.lines = String(lines);
      preview.style.fontSize = `${pixels * scale}px`;
      if (screen?.style) {
        const headerScale = Math.min(1, (screen.clientWidth || 505) / 505);
        for (const [name, value] of [['--todo-title-size', Math.max(9, 14 * headerScale)],
          ['--todo-temperature-number-size', Math.max(11, 22 * headerScale)], ['--todo-temperature-unit-size', Math.max(8, 16 * headerScale)]]) {
          if (typeof screen.style.setProperty === 'function') screen.style.setProperty(name, `${value}px`);
          else screen.style[name] = `${value}px`;
        }
      }
      // Keep text spacing and the available body height proportional to 800×480.
      for (const [name, value] of [['--todo-row-height', rowHeight * scale], ['--todo-line-height', (height + 4) * scale],
        ['--todo-body-height', pane.height * scale], ['--todo-marker-size', style.marker_size * scale],
        ['--todo-marker-top', (style.text_top + Math.floor((height - style.marker_size) / 2)) * scale], ['--todo-copy-inset', (style.text_inset + Math.ceil(pixels / 4)) * scale]]) {
        if (typeof preview.style.setProperty === 'function') preview.style.setProperty(name, `${value}px`);
        else preview.style[name] = `${value}px`;
      }
    },
    render() {
      this.syncOptions(); this.syncLayout();
      this.previewGeometry();
      const count = this.element('todoCalendarCount'); if (count) count.textContent = `${this.items.length} / ${this.MAX_RECORDS}`;
      const now = this.previewNow(), active = this.activeItems(now), shown = active.slice(0, this.limit), visible = new Set(shown.map(item => item.id));
      this.renderHeader(now);
      const list = this.element('todoCalendarList');
      const listSignature = JSON.stringify([this._editingId, this.items.map(item => [item, this.state(item, now, visible)])]);
      if (list && this._listSignature !== listSignature) {
        this._listSignature = listSignature;
        list.replaceChildren();
        if (!this.items.length) this.append(list, 'p', '还没有事项，添加一个定时待办，或导入旧版待办草稿。', 'todo-calendar-empty');
        this.items.slice().sort((a, b) => this.compare(a, b)).forEach(item => {
          const row = this.append(list, 'div', '', `todo-calendar-row${item.completed ? ' completed' : ''}${this._editingId === item.id ? ' editing' : ''}`);
          const content = this.append(row, 'div', '', 'todo-calendar-item-content');
          this.append(content, 'strong', item.text);
          this.append(content, 'span', item.resident ? '常驻 · 完成后隐藏' : `${this.displayTime(item.start)} → ${this.displayTime(item.end)}`, 'todo-calendar-time');
          const state = this.append(row, 'span', this.state(item, now, visible), 'todo-calendar-state');
          state.dataset.state = item.completed ? 'completed' : visible.has(item.id) ? 'visible' : 'pending';
          const actions = this.append(row, 'div', '', 'todo-calendar-actions');
          for (const [label, action, danger] of [[item.completed ? '恢复' : '完成', () => this.toggleComplete(item.id), false],
            ['编辑', () => this.edit(item.id), false], ['删除', () => this.remove(item.id), true]]) {
            const button = this.append(actions, 'button', label, danger ? 'danger' : 'secondary');
            button.type = 'button'; button.setAttribute('aria-label', `${label} ${item.text}`); button.onclick = action;
          }
        });
      }
      const preview = this.element('todoCalendarPreviewItems');
      const previewSignature = JSON.stringify([this.fontSize, this.fontFamily, this.fontBold, this.fontItalic, this.limit, this.previewMode,
        shown.map(item => [item, this.phase(item, now)])]);
      if (preview && this._previewSignature !== previewSignature) {
        this._previewSignature = previewSignature;
        preview.replaceChildren(); preview.dataset.font = String(this.fontSize); preview.dataset.compact = String(this.previewMode === 4 && this.limit >= 6);
        if (!shown.length) this.append(preview, 'p', '暂无待办', 'todo-calendar-preview-empty');
        shown.forEach(item => {
          const row = this.append(preview, 'div', '', 'todo-calendar-preview-item'), phase = this.phase(item, now);
          row.dataset.phase = phase;
          const marker = this.append(row, 'span', '', 'todo-calendar-preview-box');
          marker.dataset.resident = String(item.resident);
          marker.setAttribute('aria-label', phase === 'upcoming' ? '未触发' : phase === 'expired' ? '已过期，保留显示' : phase === 'active' ? '已触发' : '常驻');
          const copy = this.append(row, 'div', '', 'todo-calendar-preview-copy');
          this.append(copy, 'span', item.text);
        });
      }
      const summary = this.element('todoCalendarPreviewSummary');
      if (summary) summary.textContent = `${this.displayTime(now)} · 显示 ${shown.length} / ${this.limit} 项${active.length > shown.length ? ` · ${active.length - shown.length} 项等待空位` : ''}`;
      this.renderCalendar(now); this.updateEditor(); this.controls();
      this.renderResourcePreview(now);
    },
    async renderResourcePreview(now = this.previewNow()) {
      const canvas=this.element('todoCalendarResourceCanvas');
      if (!canvas?.getContext || !root.ExternalResourceEditor || !root.EPDUIRenderer) return;
      const token=this._resourcePreviewToken=(this._resourcePreviewToken||0)+1;
      try {
        if (!root.ExternalResourceEditor.previewModel()) await root.ExternalResourceEditor.ensureReady();
        const model=root.ExternalResourceEditor.previewModel();
        if (!model || token!==this._resourcePreviewToken) return;
        const requests=this.fontRequests();
        const fonts=(await root.EPDWebFonts.extendFonts(model.fonts,requests,{fontFamily:this.fontFamily||undefined})).fonts;
        if (token!==this._resourcePreviewToken) return;
        root.EPDUIRenderer.draw(canvas,model.ui,model.calendar,fonts,
          {...root.ExternalResourceEditor.previewRuntime(this.previewMode),timestamp:now},this,root.CalendarAnnotations?.annotations||[]);
        canvas.hidden=false;
      } catch(error) { if(token===this._resourcePreviewToken) this.report(`预览失败：${error.message}`,true); }
    },
    renderCalendar(now) {
      const calendar = this.element('todoCalendarPreviewCalendar'); if (!calendar) return;
      const date = new Date(now * 1000), year = date.getUTCFullYear(), month = date.getUTCMonth(), today = date.getUTCDate();
      const week = Math.min(6, Math.max(0, Number(this.value('weekStart', 0)) || 0));
      const signature = JSON.stringify([year, month, today, week, root.CalendarAnnotations?.annotations]);
      if (this._calendarSignature === signature) return;
      this._calendarSignature = signature;
      const weekdays = ['日', '一', '二', '三', '四', '五', '六'];
      calendar.replaceChildren();
      this.append(calendar, 'strong', `${year} 年 ${month + 1} 月`, 'todo-calendar-month');
      const grid = this.append(calendar, 'div', '', 'todo-calendar-grid');
      for (let i = 0; i < 7; ++i) this.append(grid, 'span', weekdays[(week + i) % 7], 'todo-calendar-weekday');
      const offset = (new Date(Date.UTC(year, month, 1)).getUTCDay() + 7 - week) % 7;
      const last = new Date(Date.UTC(year, month + 1, 0)).getUTCDate();
      for (let i = 0; i < offset; ++i) this.append(grid, 'span', '');
      for (let day = 1; day <= last; ++day) {
        const cell = this.append(grid, 'span', String(day), day === today ? 'today' : '');
        const key = `${year}-${String(month + 1).padStart(2, '0')}-${String(day).padStart(2, '0')}`;
        if (root.CalendarAnnotations?.annotations?.some(item => item.date === key)) cell.classList.add('annotated');
      }
    },
    init() {
      if (this._initialized || !this.element('todoWorkspace')) return;
      this._initialized = true;
      this.initFontOptions(); this.resetEditor(false); this.loadLocal();
      const on = (id, event, listener) => this.element(id)?.addEventListener(event, listener);
      on('todoCalendarForm', 'submit', event => { event.preventDefault(); this.addOrUpdate(); });
      on('todoCalendarCancel', 'click', () => { this.resetEditor(); this.render(); });
      on('todoCalendarText', 'compositionstart', () => { ++this._revision; this._composing = true; });
      on('todoCalendarText', 'compositionend', () => { ++this._revision; this._composing = false; this.updateEditor(); this.persist(); });
      for (const id of ['todoCalendarText', 'todoCalendarStart', 'todoCalendarEnd'])
        on(id, 'input', () => { ++this._revision; this.updateEditor(); this.persist(); });
      for (const id of ['todoCalendarResident', 'todoCalendarCompleted'])
        on(id, 'change', () => { ++this._revision; this.updateEditor(); this.persist(); });
      for (const id of ['todoCalendarLimit', 'todoCalendarFontSize', 'todoCalendarFontFamily', 'todoCalendarFontBold', 'todoCalendarFontItalic',
        'todoCalendarLeadMinutes', 'todoCalendarRetainMinutes'])
        on(id, 'change', () => this.setOptions());
      on('todoCalendarLayout', 'change', () => this.setPreviewMode());
      on('todoCalendarSave', 'click', () => this.save());
      on('todoCalendarRead', 'click', () => this.loadFromDevice());
      on('todoCalendarActivate', 'click', () => this.activate());
      on('todoCalendarImportPaint', 'click', () => this.importPaintTodos());
      on('todoCalendarExport', 'click', () => this.downloadJSON());
      on('todoCalendarImport', 'change', async event => {
        const input = event.target, file = input.files?.[0], token = ++this._importToken, revision = this._revision;
        if (!file) return;
        try {
          if (file.size > 65536) throw new Error('待办备份文件过大');
          const text = await file.text();
          if (token !== this._importToken) return;
          if (revision !== this._revision) throw new Error('读取备份期间草稿已修改，请重新选择备份以替换草稿');
          this.importJSON(text);
        } catch (error) { if (token === this._importToken) this.report(`导入失败：${error.message}`, true); }
        finally { if (token === this._importToken) input.value = ''; }
      });
      on('todoCalendarPreviewTime', 'input', () => this.render());
      on('todoCalendarPreviewNow', 'click', () => { this.element('todoCalendarPreviewTime').value = ''; this.render(); });
      on('weekStart', 'change', () => this.render());
      this.render();
      if (typeof ResizeObserver === 'function') {
        const screen = this.element('todoCalendarPreviewScreen');
        if (screen) { this._previewObserver = new ResizeObserver(() => this.previewGeometry()); this._previewObserver.observe(screen); }
      }
      // Refresh the preview periodically and recompute immediately after sleep.
      if (typeof setInterval === 'function') this._timer = setInterval(() => this.render(), 15000);
      if (typeof document.addEventListener === 'function') document.addEventListener('visibilitychange', () => {
        if (!document.hidden) this.render();
      });
    },
  };

  root.TodoCalendar = TodoCalendar;
  if (typeof module !== 'undefined' && module.exports) module.exports = TodoCalendar;
  if (typeof document !== 'undefined' && typeof document.addEventListener === 'function') {
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => TodoCalendar.init(), { once: true });
    else TodoCalendar.init();
  }
})(globalThis);
