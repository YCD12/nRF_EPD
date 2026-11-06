const CalendarAnnotations = {
  MAGIC: 0x4F4E4E41,
  VERSION: 1,
  HEADER_SIZE: 16,
  RECORD_SIZE: 16,
  MAX_RECORDS: 64,
  MAX_LABEL_BYTES: 9,
  NOTIFY: 0xA2,
  NOTIFY_HDR: 5,
  COMMIT_NOTIFY: 0xA8,
  PARTITION_ID: 3,
  STORAGE_KEY: 'epd_calendar_annotations_v1',
  FONT_FOLLOW: 0,
  FONT_SMALL: 9,
  FONT_LARGE: 12,
  COLOR_BLACK: 0,
  COLOR_WHITE: 1,
  COLOR_RED: 2,
  POS_BELOW: 0,
  POS_CORNER: 1,

  annotations: [],
  _commitWaiter: null,
  _editingDate: null,
  _labelComposing: false,

  crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
      crc ^= byte;
      for (let i = 0; i < 8; ++i) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
  },

  waitForCommit(bytes) {
    this.cancelCommitWait();
    const expectedCrc = this.crc32(bytes);
    return new Promise(resolve => {
      const timer = setTimeout(() => {
        if (this._commitWaiter && this._commitWaiter.expectedCrc === expectedCrc) {
          this._commitWaiter = null;
          resolve(false);
        }
      }, 30000);
      this._commitWaiter = { expectedCrc, resolve, timer };
    });
  },

  cancelCommitWait() {
    if (!this._commitWaiter) return;
    clearTimeout(this._commitWaiter.timer);
    this._commitWaiter.resolve(false);
    this._commitWaiter = null;
  },

  labelByteLength(text) {
    return new TextEncoder().encode(text || '').length;
  },

  selectedPosition() {
    const select = document.getElementById('calendarAnnotationPosition');
    return Number(select ? select.value : 0) === this.POS_CORNER ? this.POS_CORNER : this.POS_BELOW;
  },

  clampLabelToMaxBytes(text) {
    let next = text || '';
    while (this.labelByteLength(next) > this.MAX_LABEL_BYTES) {
      const chars = Array.from(next);
      if (chars.length === 0) return '';
      next = chars.slice(0, -1).join('');
    }
    return next;
  },

  assertLabel(label) {
    const normalized = (label || '').trim();
    if (this.labelByteLength(normalized) > this.MAX_LABEL_BYTES) {
      throw new Error(`短标签最多 ${this.MAX_LABEL_BYTES} 个 UTF-8 字节（中文最多 3 个）`);
    }
    for (const ch of normalized) {
      const code = ch.codePointAt(0);
      if (code < 0x20 || (code >= 0x7f && code <= 0x9f) ||
          code > 0xffff || (code >= 0xd800 && code <= 0xdfff))
        throw new Error(`“${ch}”暂不支持，请使用基本平面的文字，避免控制字符或表情`);
    }
    return normalized;
  },

  fontRequests() {
    const texts = ['', ''];
    for (const item of this.annotations) {
      const text = this.assertLabel(item.label || '');
      if (/[a-z]/i.test(text) || item.fontSize === this.FONT_SMALL) texts[0] += text;
      else if (item.fontSize === this.FONT_LARGE) texts[1] += text;
      else { texts[0] += text; texts[1] += text; }
    }
    return texts.map((text, slot) => ({ slot, text })).filter(item => item.text);
  },

  packFlags(item) {
    let flags = 0x01;
    if (item.fontSize === this.FONT_SMALL) flags |= (1 << 1);
    else if (item.fontSize === this.FONT_LARGE) flags |= (2 << 1);
    if (item.color === this.COLOR_WHITE) flags |= (1 << 3);
    else if (item.color === this.COLOR_RED) flags |= (2 << 3);
    if (item.position === this.POS_CORNER) flags |= (1 << 5);
    return flags;
  },

  unpackFlags(flags) {
    const fontMode = (flags >> 1) & 3;
    const colorMode = (flags >> 3) & 3;
    return {
      fontSize: fontMode === 1 ? this.FONT_SMALL : fontMode === 2 ? this.FONT_LARGE : this.FONT_FOLLOW,
      color: colorMode === 1 ? this.COLOR_WHITE : colorMode === 2 ? this.COLOR_RED : this.COLOR_BLACK,
      position: (flags & 0x20) ? this.POS_CORNER : this.POS_BELOW,
    };
  },

  requireDevice() {
    if (typeof write !== 'function' || !epdCharacteristic) {
      this.report('请先连接设备，再读取或保存日期标注。', true);
      return false;
    }
    const policy = typeof window !== 'undefined' ? window.EpdFirmwarePolicy : undefined;
    const version = typeof appVersion !== 'undefined' ? appVersion : undefined;
    if (!policy?.isNewFirmware?.(version)) {
      this.report('设备固件不支持新版网页，或版本尚未确认，请重新连接或使用旧版网页。', true);
      return false;
    }
    return true;
  },

  report(message, error = false) {
    const status = typeof document === 'undefined' ? null : document.getElementById('calendarAnnotationStatus');
    if (status) {
      status.textContent = message;
      status.classList.toggle('error', error);
    }
    if (typeof addLog === 'function') addLog(message);
  },

  updateHint(composing = this._labelComposing) {
    const input = document.getElementById('calendarAnnotationLabel');
    const hint = document.getElementById('calendarAnnotationLabelHint');
    if (input) {
      if (!composing) {
        const clamped = this.clampLabelToMaxBytes(input.value);
        if (clamped !== input.value) input.value = clamped;
      }
      if (hint) hint.textContent = `中文最多 3 字 · 已用 ${this.labelByteLength(input.value)}/${this.MAX_LABEL_BYTES} 字节`;
    }
    this.updatePreview();
  },

  dateParts(value) {
    const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(value || '');
    if (!match) return null;
    const [year, month, day] = match.slice(1).map(Number);
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) return null;
    const date = new Date(Date.UTC(year, month - 1, day));
    if (date.getUTCFullYear() !== year || date.getUTCMonth() !== month - 1 || date.getUTCDate() !== day) return null;
    return { year, month, day, weekday: ['周日', '周一', '周二', '周三', '周四', '周五', '周六'][date.getUTCDay()] };
  },

  today() {
    const date = new Date();
    return this.formatDate(date.getFullYear(), date.getMonth() + 1, date.getDate());
  },

  updatePreview() {
    const preview = document.getElementById('calendarAnnotationPreview');
    if (!preview) return;
    const selectedDate = (document.getElementById('calendarAnnotationDate') || {}).value;
    const date = this.dateParts(selectedDate);
    const isToday = Boolean(date) && selectedDate === this.today();
    const text = ((document.getElementById('calendarAnnotationLabel') || {}).value || '').trim();
    const position = this.selectedPosition();
    const color = Number((document.getElementById('calendarAnnotationColor') || {}).value) || this.COLOR_BLACK;
    const font = Number((document.getElementById('calendarAnnotationFontSize') || {}).value) || this.FONT_FOLLOW;
    const hasLetters = /[a-z]/i.test(text);
    const previewFont = hasLetters ? this.FONT_SMALL : font;
    const onRedBackground = isToday && position === this.POS_BELOW;
    const colorConflict = Boolean(text) && onRedBackground && color === this.COLOR_RED;
    const setText = (id, value) => {
      const element = document.getElementById(id);
      if (element) element.textContent = value;
    };
    preview.setAttribute('data-position', position === this.POS_CORNER ? 'corner' : 'below');
    preview.setAttribute('data-color', color === this.COLOR_WHITE ? 'white' : color === this.COLOR_RED ? 'red' : 'black');
    preview.setAttribute('data-font', previewFont === this.FONT_SMALL ? 'small' : previewFont === this.FONT_LARGE ? 'large' : 'follow');
    preview.setAttribute('data-today', String(isToday));
    const colorHint = document.getElementById('calendarAnnotationColorHint');
    if (colorHint) {
      colorHint.hidden = !colorConflict;
      colorHint.textContent = colorConflict ? '当日日期下方是红色背景，请不要选择红色；建议改用白色或黑色。' : '';
    }
    setText('calendarAnnotationPreviewDate', date ? String(date.day).padStart(2, '0') : '—');
    setText('calendarAnnotationPreviewMonth', date ? `${date.year} 年 ${date.month} 月` : '请选择日期');
    setText('calendarAnnotationPreviewWeekday', date ? date.weekday.replace('周', '星期') : '');
    setText('calendarAnnotationPreviewLabel', text);
    const label = document.getElementById('calendarAnnotationPreviewLabel');
    const dot = document.getElementById('calendarAnnotationPreviewDot');
    const lunar = document.getElementById('calendarAnnotationPreviewLunar');
    if (label) label.hidden = !text;
    if (dot) dot.hidden = position === this.POS_CORNER && Boolean(text);
    if (lunar) lunar.hidden = position === this.POS_BELOW && Boolean(text);
    let note = !text ? '留空只显示红点，节日 / 农历仍保留。'
      : position === this.POS_CORNER ? '标签显示在右上角，节日 / 农历仍保留。'
        : '标签替换日期下方的节日 / 农历，并显示红点。';
    if (hasLetters) note += ' 含英文字母时，设备使用小字号显示。';
    if (colorConflict) note += ' 当日红字会与红色背景重合，请改用白色或黑色。';
    if (text && color === this.COLOR_WHITE && !onRedBackground) note += ' 白字在浅色背景上可能不易辨认。';
    setText('calendarAnnotationPreviewNote', note);
    this.renderResourcePreview();
  },

  async renderResourcePreview() {
    const canvas=document.getElementById('calendarAnnotationResourceCanvas'),editor=globalThis.ExternalResourceEditor;
    if(!canvas?.getContext || !editor || !globalThis.EPDUIRenderer) return;
    const date=(document.getElementById('calendarAnnotationDate')||{}).value;
    const parts=this.dateParts(date);if(!parts)return;
    const token=this._resourcePreviewToken=(this._resourcePreviewToken||0)+1;
    try {
      const label=this.assertLabel((document.getElementById('calendarAnnotationLabel')||{}).value||'');
      const annotation={date,label,position:this.selectedPosition(),
        color:Number(document.getElementById('calendarAnnotationColor')?.value)||0,
        fontSize:Number(document.getElementById('calendarAnnotationFontSize')?.value)||0};
      if(!editor.previewModel())await editor.ensureReady();
      const model=editor.previewModel();if(!model||token!==this._resourcePreviewToken)return;
      const requests=[];
      for(const c of model.ui.components.filter(c=>c.kind===3)) {
        const small=annotation.fontSize===9||c.style.ascii_small&&/[a-z]/i.test(label);
        requests.push({slot:small?c.style.annotation_small_font:c.style.annotation_large_font,text:label});
      }
      const fonts=(await globalThis.EPDWebFonts.extendFonts(model.fonts,requests)).fonts;
      if(token!==this._resourcePreviewToken)return;
      const annotations=this.annotations.filter(a=>a.date!==date).concat(annotation);
      globalThis.EPDUIRenderer.draw(canvas,model.ui,model.calendar,fonts,
        {...editor.previewRuntime(1),timestamp:Math.floor(Date.UTC(parts.year,parts.month-1,parts.day,12)/1000)},
        globalThis.TodoCalendar||{},annotations);
    } catch(_) { /* Keep the last valid frame while the label is being composed. */ }
  },

  updateEditorState() {
    const date = (document.getElementById('calendarAnnotationDate') || {}).value || '';
    const existing = this.annotations.some(item => item.date === date);
    const editing = Boolean(this._editingDate);
    const title = document.getElementById('calendarAnnotationEditorTitle');
    const note = document.getElementById('calendarAnnotationEditorNote');
    const submit = document.getElementById('calendarAnnotationSubmit');
    const cancel = document.getElementById('calendarAnnotationCancel');
    if (title) title.textContent = existing ? '编辑日期标注' : '新增日期标注';
    if (submit) submit.textContent = existing ? '更新标注' : '添加标注';
    if (cancel) cancel.hidden = !editing;
    if (note) {
      if (editing && this._editingDate !== date) {
        note.textContent = existing
          ? `该日期已有标注，将更新其内容；原日期 ${this._editingDate} 保留。`
          : `修改日期会新增标注，原日期 ${this._editingDate} 保留。`;
      } else note.textContent = existing
        ? '更新当前日期的本地标注，保存到设备后生效。'
        : '选择日期并设置标注，添加到下方列表。';
    }
    const body = document.getElementById('calendarAnnotationsList');
    if (body) Array.from(body.children).forEach(row => {
      row.classList.toggle('editing', editing && row.getAttribute('data-date') === date);
    });
  },

  persist() {
    try {
      if (typeof localStorage === 'undefined') return;
      localStorage.setItem(this.STORAGE_KEY, JSON.stringify(this.annotations));
    } catch (e) { /* ignore quota / private mode */ }
  },

  formatDate(year, month, day) {
    return `${year}-${String(month).padStart(2, '0')}-${String(day).padStart(2, '0')}`;
  },

  loadLocal() {
    try {
      const raw = JSON.parse(localStorage.getItem(this.STORAGE_KEY) || '[]');
      this.annotations = Array.isArray(raw) ? raw : [];
    } catch (e) {
      this.annotations = [];
    }
    this.render();
  },

  fontLabel(size) {
    if (size === this.FONT_SMALL) return '小';
    if (size === this.FONT_LARGE) return '大';
    return '跟随日历';
  },

  colorLabel(color) {
    if (color === this.COLOR_WHITE) return '白';
    if (color === this.COLOR_RED) return '红';
    return '黑';
  },

  positionLabel(position) {
    return position === this.POS_CORNER ? '右上角' : '日期下方';
  },

  render() {
    const body = document.getElementById('calendarAnnotationsList');
    const count = document.getElementById('calendarAnnotationCount');
    const summary = document.getElementById('calendarAnnotationsSummary');
    if (count) count.textContent = `${this.annotations.length} / ${this.MAX_RECORDS}`;
    if (summary) summary.textContent = `共 ${this.annotations.length} 条`;
    this.updateEditorState();
    if (!body) return;
    body.innerHTML = '';
    if (!this.annotations.length) {
      const tr = document.createElement('tr');
      const td = document.createElement('td');
      td.colSpan = 4;
      td.className = 'empty-state';
      const icon = document.createElement('span');
      icon.className = 'annotation-empty-icon';
      icon.textContent = '＋';
      icon.setAttribute('aria-hidden', 'true');
      const title = document.createElement('strong');
      title.textContent = '还没有日期标注';
      const help = document.createElement('p');
      help.textContent = '从上方添加一个重要日期，也可以连接设备读取已有标注。';
      td.appendChild(icon);
      td.appendChild(title);
      td.appendChild(help);
      tr.appendChild(td);
      body.appendChild(tr);
      return;
    }
    this.annotations.forEach((item, index) => {
      const tr = document.createElement('tr');
      tr.setAttribute('data-date', item.date);
      tr.classList.toggle('editing', Boolean(this._editingDate) &&
        item.date === (document.getElementById('calendarAnnotationDate') || {}).value);
      const dateCell = document.createElement('td');
      dateCell.className = 'annotation-date-cell';
      const dateText = document.createElement('strong');
      dateText.textContent = item.date;
      dateCell.appendChild(dateText);
      const date = this.dateParts(item.date);
      if (date) {
        const weekday = document.createElement('span');
        weekday.className = 'date-subline';
        weekday.textContent = date.weekday;
        dateCell.appendChild(weekday);
      }
      tr.appendChild(dateCell);
      const labelCell = document.createElement('td');
      labelCell.className = 'annotation-label-cell';
      const label = document.createElement('span');
      label.textContent = item.label || '仅红点';
      label.className = item.label ? 'annotation-list-label' : 'annotation-dot-label';
      labelCell.appendChild(label);
      tr.appendChild(labelCell);
      const displayCell = document.createElement('td');
      displayCell.className = 'annotation-display-cell';
      [this.positionLabel(item.position), this.fontLabel(item.fontSize), `${this.colorLabel(item.color)}字`].forEach((text, i) => {
        const chip = document.createElement('span');
        chip.className = i === 2 ? 'annotation-chip annotation-color-chip' : 'annotation-chip';
        chip.textContent = text;
        if (i === 2) chip.setAttribute('data-color', String(item.color || 0));
        displayCell.appendChild(chip);
      });
      tr.appendChild(displayCell);
      const td = document.createElement('td');
      td.className = 'annotation-actions-cell';
      const editBtn = document.createElement('button');
      editBtn.type = 'button';
      editBtn.className = 'secondary';
      editBtn.textContent = '编辑';
      editBtn.setAttribute('aria-label', `编辑 ${item.date} 的标注`);
      editBtn.onclick = (event) => {
        event.stopPropagation();
        this.editAt(index);
      };
      td.appendChild(editBtn);
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'secondary danger';
      btn.textContent = '删除';
      btn.setAttribute('aria-label', `删除 ${item.date} 的标注`);
      btn.onclick = (event) => {
        event.stopPropagation();
        this.removeAt(index);
      };
      td.appendChild(btn);
      tr.appendChild(td);
      body.appendChild(tr);
    });
  },

  addOrUpdate() {
    const dateInput = document.getElementById('calendarAnnotationDate');
    const labelInput = document.getElementById('calendarAnnotationLabel');
    const fontSelect = document.getElementById('calendarAnnotationFontSize');
    const colorSelect = document.getElementById('calendarAnnotationColor');
    const position = this.selectedPosition();
    if (!dateInput || !this.dateParts(dateInput.value)) {
      this.report('请选择 2000–2099 年内的有效日期。', true);
      if (dateInput) dateInput.focus();
      return;
    }
    if (this._labelComposing) {
      this.report('请完成标签输入后再添加标注。', true);
      return;
    }
    let label;
    try {
      label = this.assertLabel(labelInput ? labelInput.value.trim() : '');
    } catch (e) {
      this.report(e.message, true);
      if (labelInput) labelInput.focus();
      return;
    }
    const item = {
      date: dateInput.value,
      label,
      fontSize: Number(fontSelect ? fontSelect.value : 0),
      color: Number(colorSelect ? colorSelect.value : 0),
      position,
    };
    const existing = this.annotations.findIndex((row) => row.date === item.date);
    if (existing >= 0) this.annotations[existing] = item;
    else {
      if (this.annotations.length >= this.MAX_RECORDS) {
        this.report(`最多保存 ${this.MAX_RECORDS} 条日期标注，可编辑或移除已有日期。`, true);
        return;
      }
      this.annotations.push(item);
    }
    this.annotations.sort((a, b) => a.date.localeCompare(b.date));
    this._editingDate = item.date;
    this.persist();
    this.render();
    this.updateHint();
    this.report(`已${existing >= 0 ? '更新' : '添加'} ${item.date} 的本地标注，保存到设备后生效。`);
  },

  removeAt(index) {
    const item = this.annotations[index];
    if (!item) return;
    this.annotations.splice(index, 1);
    if (this._editingDate === item.date) this._editingDate = null;
    this.persist();
    this.render();
    this.report(`已移除 ${item.date} 的本地标注，保存到设备后生效。`);
  },

  editAt(index) {
    const item = this.annotations[index];
    if (!item) return;
    const dateInput = document.getElementById('calendarAnnotationDate');
    const labelInput = document.getElementById('calendarAnnotationLabel');
    const fontSelect = document.getElementById('calendarAnnotationFontSize');
    const colorSelect = document.getElementById('calendarAnnotationColor');
    const positionSelect = document.getElementById('calendarAnnotationPosition');
    if (dateInput) dateInput.value = item.date;
    if (labelInput) labelInput.value = item.label || '';
    if (fontSelect) fontSelect.value = String(item.fontSize || 0);
    if (colorSelect) colorSelect.value = String(item.color || 0);
    if (positionSelect) positionSelect.value = String(item.position || 0);
    this._editingDate = item.date;
    this.updateHint();
    this.updateEditorState();
    if (labelInput) labelInput.focus();
  },

  cancelEdit() {
    const defaults = {
      calendarAnnotationDate: this.today(),
      calendarAnnotationLabel: '',
      calendarAnnotationFontSize: '0',
      calendarAnnotationColor: '0',
      calendarAnnotationPosition: '0',
    };
    Object.entries(defaults).forEach(([id, value]) => {
      const element = document.getElementById(id);
      if (element) element.value = value;
    });
    this._editingDate = null;
    this._labelComposing = false;
    this.updateHint();
    this.updateEditorState();
    this.report('已取消本次编辑，列表内容保持不变。');
  },

  pack() {
    if (this.annotations.length > this.MAX_RECORDS) {
      throw new Error(`最多保存 ${this.MAX_RECORDS} 条日期标注`);
    }
    const count = this.annotations.length;
    const bytes = new Uint8Array(this.HEADER_SIZE + count * this.RECORD_SIZE);
    const view = new DataView(bytes.buffer);
    view.setUint32(0, this.MAGIC, true);
    bytes[4] = this.VERSION;
    bytes[5] = this.RECORD_SIZE;
    view.setUint16(6, count, true);
    this.annotations.forEach((item, index) => {
      const parts = item.date.split('-').map((n) => Number(n));
      const labelBytes = new TextEncoder().encode(this.assertLabel(item.label || ''));
      const offset = this.HEADER_SIZE + index * this.RECORD_SIZE;
      const flags = this.packFlags(item);
      view.setUint16(offset, parts[0], true);
      bytes[offset + 2] = parts[1];
      bytes[offset + 3] = parts[2];
      bytes[offset + 4] = flags;
      bytes[offset + 5] = labelBytes.length;
      bytes.set(labelBytes, offset + 6);
    });
    return bytes;
  },

  unpack(bytes) {
    if (!bytes || bytes.length < this.HEADER_SIZE) {
      throw new Error('标注数据太短');
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (view.getUint32(0, true) !== this.MAGIC || bytes[4] !== this.VERSION ||
        bytes[5] !== this.RECORD_SIZE) {
      throw new Error('标注数据无效');
    }
    const count = view.getUint16(6, true);
    const expected = this.HEADER_SIZE + count * this.RECORD_SIZE;
    if (count > this.MAX_RECORDS || bytes.length < expected) {
      throw new Error('标注条数无效');
    }
    const annotations = [];
    for (let i = 0; i < count; i++) {
      const offset = this.HEADER_SIZE + i * this.RECORD_SIZE;
      const flags = bytes[offset + 4];
      const labelLen = bytes[offset + 5];
      if (labelLen > this.MAX_LABEL_BYTES) {
        throw new Error('标注标签过长');
      }
      annotations.push({
        date: this.formatDate(view.getUint16(offset, true), bytes[offset + 2], bytes[offset + 3]),
        label: new TextDecoder().decode(bytes.subarray(offset + 6, offset + 6 + labelLen)),
        ...this.unpackFlags(flags),
      });
    }
    return annotations;
  },

  finishLoad(ok) {
    if (typeof this._resolveLoad === 'function') {
      const resolve = this._resolveLoad;
      this._resolveLoad = null;
      resolve(ok);
    }
  },

  handleNotification(value) {
    const data = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    if (data.length === 12 && data[0] === this.COMMIT_NOTIFY && data[1] === this.PARTITION_ID) {
      const crc = new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(8, true);
      if (this._commitWaiter && crc === this._commitWaiter.expectedCrc) {
        const waiter = this._commitWaiter;
        this._commitWaiter = null;
        clearTimeout(waiter.timer);
        waiter.resolve(data[2] === 0);
      }
      return true;
    }
    if (data.length < this.NOTIFY_HDR || data[0] !== this.NOTIFY) return false;
    if (!this._resolveLoad) return true;
    const offset = data[1] | (data[2] << 8);
    const total = data[3] | (data[4] << 8);
    const slice = data.subarray(this.NOTIFY_HDR);
    if (total < this.HEADER_SIZE || offset + slice.length > total) {
      this.finishLoad(false);
      return true;
    }
    if (!this._rx || this._rx.total !== total) {
      if (offset !== 0) {
        this.finishLoad(false);
        return true;
      }
      this._rx = { total, buf: new Uint8Array(total), next: 0 };
    }
    if (offset !== this._rx.next) {
      this._rx = null;
      this.finishLoad(false);
      return true;
    }
    this._rx.buf.set(slice, offset);
    this._rx.next = offset + slice.length;
    if (this._rx.next >= total) {
      try {
        this.annotations = this.unpack(this._rx.buf);
        this.persist();
        this.render();
        this.finishLoad(true);
      } catch (e) {
        this.finishLoad(false);
      }
      this._rx = null;
    }
    return true;
  },

  deviceTask(action, parent = null) {
    return typeof runDeviceOperation === 'function' ? runDeviceOperation('annotations', action, parent) : action(null);
  },

  cancelLoad() {
    clearTimeout(this._loadTimer);
    this.finishLoad(false);
    this._rx = null;
    this._loadPromise = null;
  },

  async loadFromDevice(parent = null) {
    if (!this.requireDevice()) return false;
    if (this._loadPromise) return this._loadPromise;
    const pending = this.deviceTask(owner => this._loadFromDeviceOnce(owner), parent).finally(() => {
      if (this._loadPromise === pending) this._loadPromise = null;
    });
    this._loadPromise = pending;
    return pending;
  },

  async _loadFromDeviceOnce(owner) {
    if (!this.requireDevice()) return false;
    this._rx = null;
    let resolveLoad, timer;
    const done = new Promise((resolve) => {
      let settled = false;
      resolveLoad = this._resolveLoad = (ok) => {
        if (settled) return;
        settled = true;
        resolve(ok);
      };
      timer = this._loadTimer = setTimeout(() => {
        if (this._resolveLoad === resolveLoad) this.finishLoad(false);
      }, 4000);
    });
    try {
      const okWrite = await write(EpdCmd.SET_ANNOTATIONS, new Uint8Array([2]), true, true, true, owner);
      if (owner) DeviceOperations.assert(owner);
      if (!okWrite) {
        if (this._resolveLoad === resolveLoad) this.finishLoad(false);
        addLog('请求回读失败');
        return false;
      }
      const ok = await done;
      if (owner) DeviceOperations.assert(owner);
      if (ok) addLog(`已从设备读回 ${this.annotations.length} 条日期标注`);
      else addLog('回读超时或数据无效');
      return ok;
    } finally {
      clearTimeout(timer);
      if (this._loadTimer === timer) this._loadTimer = null;
      if (this._resolveLoad === resolveLoad) this.finishLoad(false);
    }
  },

  async save() {
    return this.deviceTask(owner => this.saveOperation(owner));
  },

  async saveOperation(owner) {
    if (!this.requireDevice()) return;
    let bytes;
    try {
      bytes = this.pack();
      const requests = this.fontRequests();
      if (requests.length) {
        if (typeof ExternalResourceInstaller === 'undefined' || typeof ExternalResourceInstaller.ensureFonts !== 'function')
          throw new Error('网页字体模块未加载，请刷新页面后重试');
        const result = await ExternalResourceInstaller.ensureFonts(requests, owner, message => this.report(message));
        if (owner) DeviceOperations.assert(owner);
        if (result.changed) addLog(`已补充 ${result.added.length} 个字模并加载到设备`);
      }
    } catch (e) {
      if (owner && DeviceOperations.current !== owner) return;
      this.report(e.message, true);
      return;
    }
    const committed = this.waitForCommit(bytes);
    const mtu = Number((document.getElementById('mtusize') || {}).value) || 20;
    const chunk = Math.max(1, mtu - 6);
    for (let offset = 0; offset < bytes.length; offset += chunk) {
      const slice = bytes.subarray(offset, Math.min(bytes.length, offset + chunk));
      const payload = new Uint8Array(5 + slice.length);
      payload[0] = 0;
      payload[1] = offset & 0xff;
      payload[2] = (offset >> 8) & 0xff;
      payload[3] = bytes.length & 0xff;
      payload[4] = (bytes.length >> 8) & 0xff;
      payload.set(slice, 5);
      const ok = await write(EpdCmd.SET_ANNOTATIONS, payload, true, true, true, owner);
      if (owner) DeviceOperations.assert(owner);
      if (!ok) {
        this.cancelCommitWait();
        addLog('标注分片发送失败');
        return;
      }
    }
    if (!await committed) {
      addLog('设备未确认标注已写入外置 Flash，请重试');
      return;
    }
    if (owner) DeviceOperations.assert(owner);
    this.report(`已保存 ${new DataView(bytes.buffer).getUint16(6, true)} 条日期标注到外置 Flash`);
  },

  async clearDevice() {
    return this.deviceTask(owner => this.clearOperation(owner));
  },

  async clearOperation(owner) {
    if (!this.requireDevice()) return;
    const empty = new Uint8Array(this.HEADER_SIZE);
    const view = new DataView(empty.buffer);
    view.setUint32(0, this.MAGIC, true);
    empty[4] = this.VERSION;
    empty[5] = this.RECORD_SIZE;
    const committed = this.waitForCommit(empty);
    const ok = await write(EpdCmd.SET_ANNOTATIONS, new Uint8Array([1]), true, true, true, owner);
    if (owner) DeviceOperations.assert(owner);
    if (ok) {
      if (!await committed) {
        addLog('设备未确认清除结果已写入外置 Flash，请重试');
        return;
      }
      if (owner) DeviceOperations.assert(owner);
      this.annotations = [];
      this.persist();
      this.render();
      addLog('已清除设备标注并写入外置 Flash');
    } else this.cancelCommitWait();
  },

  init() {
    const label = document.getElementById('calendarAnnotationLabel');
    const date = document.getElementById('calendarAnnotationDate');
    if (date) {
      if (!date.value) date.value = this.today();
      const changed = () => { this.updatePreview(); this.updateEditorState(); };
      date.addEventListener('input', changed);
      date.addEventListener('change', changed);
    }
    if (label) {
      label.addEventListener('compositionstart', () => { this._labelComposing = true; });
      label.addEventListener('compositionend', () => { this._labelComposing = false; this.updateHint(); });
      label.addEventListener('input', event => this.updateHint(this._labelComposing || Boolean(event.isComposing)));
    }
    ['calendarAnnotationPosition', 'calendarAnnotationFontSize', 'calendarAnnotationColor'].forEach(id => {
      const select = document.getElementById(id);
      if (select) select.addEventListener('change', () => this.updateHint());
    });
    this.loadLocal();
    this.updateHint();
  },
};

if (typeof window !== 'undefined') window.CalendarAnnotations = CalendarAnnotations;
