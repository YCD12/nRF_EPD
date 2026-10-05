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
  /* Characters compiled into u8g2_font_wqy9/12_t_lunar. */
  FONT_CHARS:
    '正月二月三月四月五月六月七月八月九月十月冬月腊月闰' +
    '初一廿二三十' +
    '星期一二三四五六日周' +
    '猴鸡狗猪鼠牛虎兔龙蛇马羊' +
    '庚辛壬癸甲乙丙丁戊己' +
    '申酉戌亥子丑寅卯辰巳午未' +
    '小寒大寒立春雨水惊蛰春分清明谷雨立夏小满芒种夏至' +
    '小暑大暑立秋处暑白露秋分寒露霜降立冬小雪大雪冬至' +
    '年月日离还有天℃' +
    '元旦节情人节妇女节植树节愚人节劳动节青年节儿童节建党节建军节教师节国庆节' +
    '母亲节父亲节万圣节感恩节平安夜圣诞节' +
    '春节元宵节龙抬头端午节七夕节中元节中秋节重阳节寒衣节腊八节除夕' +
    '休班',

  annotations: [],
  _commitWaiter: null,

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

  fontHasChar(ch) {
    const code = ch.codePointAt(0);
    if (code >= 0x20 && code <= 0x7e) return true;
    return this.FONT_CHARS.indexOf(ch) >= 0;
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
      if (!this.fontHasChar(ch)) {
        throw new Error(`“${ch}”不在机内日历字库中，请改用节日/农历/休班等已有汉字，或英文字母`);
      }
    }
    return normalized;
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
      addLog('请先连接设备');
      return false;
    }
    return true;
  },

  updateHint() {
    const input = document.getElementById('calendarAnnotationLabel');
    const hint = document.getElementById('calendarAnnotationLabelHint');
    if (!input || !hint) return;
    const clamped = this.clampLabelToMaxBytes(input.value);
    if (clamped !== input.value) input.value = clamped;
    hint.textContent = `${this.labelByteLength(input.value)}/${this.MAX_LABEL_BYTES} UTF-8 字节`;
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
    if (count) count.textContent = `${this.annotations.length}/${this.MAX_RECORDS}`;
    if (!body) return;
    body.innerHTML = '';
    if (!this.annotations.length) {
      const tr = document.createElement('tr');
      const td = document.createElement('td');
      td.colSpan = 6;
      td.className = 'empty-state';
      td.textContent = '还没有日期标注。选择日期并添加短标签，即可开始。';
      tr.appendChild(td);
      body.appendChild(tr);
      return;
    }
    this.annotations.forEach((item, index) => {
      const tr = document.createElement('tr');
      for (const value of [item.date, item.label || '（仅红点）',
        this.positionLabel(item.position), this.fontLabel(item.fontSize), this.colorLabel(item.color)]) {
        const cell = document.createElement('td');
        cell.textContent = value;
        tr.appendChild(cell);
      }
      const td = document.createElement('td');
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
      btn.className = 'secondary';
      btn.textContent = '删除';
      btn.setAttribute('aria-label', `删除 ${item.date} 的标注`);
      btn.onclick = (event) => {
        event.stopPropagation();
        this.removeAt(index);
      };
      td.appendChild(btn);
      tr.style.cursor = 'pointer';
      tr.title = '点击填入编辑区';
      tr.onclick = () => this.editAt(index);
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
    if (!dateInput || !dateInput.value) {
      addLog('请选择日期');
      return;
    }
    let label;
    try {
      label = this.assertLabel(labelInput ? labelInput.value.trim() : '');
    } catch (e) {
      addLog(e.message);
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
        addLog(`最多保存 ${this.MAX_RECORDS} 条日期标注`);
        return;
      }
      this.annotations.push(item);
    }
    this.annotations.sort((a, b) => a.date.localeCompare(b.date));
    this.persist();
    this.render();
  },

  removeAt(index) {
    this.annotations.splice(index, 1);
    this.persist();
    this.render();
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
    this.updateHint();
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
    if (typeof appVersion === 'number' && appVersion < 0x29) {
      addLog('当前固件不支持回读标注，请升级到 0x29 或更高');
      return false;
    }
    if (this._loadPromise) return this._loadPromise;
    const pending = this.deviceTask(owner => this._loadFromDeviceOnce(owner), parent).finally(() => {
      if (this._loadPromise === pending) this._loadPromise = null;
    });
    this._loadPromise = pending;
    return pending;
  },

  async _loadFromDeviceOnce(owner) {
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
    if (typeof appVersion === 'number' && appVersion < 0x28) {
      addLog('当前固件不支持日期标注，请升级到 0x28 或更高');
      return;
    }
    if (typeof appVersion === 'number' && appVersion < 0x2A &&
        this.annotations.some((item) => item.position === this.POS_CORNER)) {
      addLog('当前固件不支持右上角标注，请升级到 0x2A 或更高，或把位置改回日期下方');
      return;
    }
    let bytes;
    try {
      bytes = this.pack();
    } catch (e) {
      addLog(e.message);
      return;
    }
    const durable = typeof appVersion === 'number' && appVersion >= 0x33;
    const committed = durable ? this.waitForCommit(bytes) : null;
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
    if (durable) {
      if (!await committed) {
        addLog('设备未确认标注已写入外置 Flash，请重试');
        return;
      }
      if (owner) DeviceOperations.assert(owner);
      addLog(`已保存 ${this.annotations.length} 条日期标注到外置 Flash`);
    } else {
      addLog(`已下发 ${this.annotations.length} 条日期标注`);
    }
    if (!durable && typeof appVersion === 'number' && appVersion >= 0x29) {
      const verified = await this.loadFromDevice(owner);
      if (!verified) addLog('保存后回读失败，请再读一次核对');
    }
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
    const durable = typeof appVersion === 'number' && appVersion >= 0x33;
    const committed = durable ? this.waitForCommit(empty) : null;
    const ok = await write(EpdCmd.SET_ANNOTATIONS, new Uint8Array([1]), true, true, true, owner);
    if (owner) DeviceOperations.assert(owner);
    if (ok) {
      if (durable && !await committed) {
        addLog('设备未确认清除结果已写入外置 Flash，请重试');
        return;
      }
      if (owner) DeviceOperations.assert(owner);
      this.annotations = [];
      this.persist();
      this.render();
      addLog(durable ? '已清除设备标注并写入外置 Flash' : '已清除设备标注');
    } else this.cancelCommitWait();
  },

  init() {
    const label = document.getElementById('calendarAnnotationLabel');
    const position = document.getElementById('calendarAnnotationPosition');
    if (label) label.addEventListener('input', () => this.updateHint());
    if (position) position.addEventListener('change', () => this.updateHint());
    this.loadLocal();
    this.updateHint();
  },
};

if (typeof window !== 'undefined') window.CalendarAnnotations = CalendarAnnotations;
