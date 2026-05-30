const UIEditor = {
  TYPE_IDS: {
    week_header: 2,
    month_grid: 3,
    time_7seg: 4,
    battery: 5,
    line: 6,
    low_battery_tip: 10,
    time_sync_tip: 11,
    text_field: 12,
  },
  MAGIC: 0x314C4955,
  VERSION: 1,
  HEADER_SIZE: 28,
  MODE_SIZE: 16,
  COMPONENT_SIZE: 24,

  layout: null,
  modeIndex: 0,
  componentIndex: 0,
  dragging: false,
  dragOffsetX: 0,
  dragOffsetY: 0,

  init() {
    const raw = window.EPD_RESOURCE_BUNDLE;
    if (!raw || !raw.uiLayout) return;
    this.layout = JSON.parse(JSON.stringify(raw.uiLayout));
    this.populateModes();
    this.bindCanvas();
    this.render();
  },

  populateModes() {
    const select = document.getElementById('uiModeSelect');
    if (!select || !this.layout) return;
    select.innerHTML = '';
    this.layout.modes.forEach((mode, index) => {
      const option = document.createElement('option');
      option.value = String(index);
      option.textContent = `${mode.name || 'mode'} (${mode.id})`;
      select.appendChild(option);
    });
    this.modeIndex = 0;
    this.populateComponents();
  },

  populateComponents() {
    const select = document.getElementById('uiComponentSelect');
    const mode = this.currentMode();
    if (!select || !mode) return;
    select.innerHTML = '';
    mode.components.forEach((component, index) => {
      const option = document.createElement('option');
      option.value = String(index);
      option.textContent = `${index}: ${component.type}`;
      select.appendChild(option);
    });
    this.componentIndex = Math.min(this.componentIndex, Math.max(0, mode.components.length - 1));
    select.value = String(this.componentIndex);
    this.syncInputs();
  },

  selectMode(value) {
    this.modeIndex = parseInt(value, 10) || 0;
    this.componentIndex = 0;
    this.populateComponents();
    this.render();
  },

  selectComponent(index) {
    this.componentIndex = Number.isFinite(index) ? index : 0;
    this.syncInputs();
    this.render();
  },

  currentMode() {
    return this.layout && this.layout.modes ? this.layout.modes[this.modeIndex] : null;
  },

  currentComponent() {
    const mode = this.currentMode();
    return mode && mode.components ? mode.components[this.componentIndex] : null;
  },

  syncInputs() {
    const c = this.currentComponent();
    if (!c) return;
    this.setInput('uiX', c.x || 0);
    this.setInput('uiY', c.y || 0);
    this.setInput('uiW', c.w || 0);
    this.setInput('uiH', c.h || 0);
  },

  setInput(id, value) {
    const el = document.getElementById(id);
    if (el) el.value = String(value);
  },

  updateSelectedFromInputs() {
    const c = this.currentComponent();
    if (!c) return;
    c.x = this.readNumber('uiX', c.x || 0);
    c.y = this.readNumber('uiY', c.y || 0);
    c.w = this.readNumber('uiW', c.w || 0);
    c.h = this.readNumber('uiH', c.h || 0);
    this.clampComponent(c);
    this.syncInputs();
    this.render();
  },

  readNumber(id, fallback) {
    const el = document.getElementById(id);
    const value = el ? parseInt(el.value, 10) : NaN;
    return Number.isFinite(value) ? value : fallback;
  },

  bindCanvas() {
    const canvas = document.getElementById('uiEditorCanvas');
    if (!canvas) return;
    canvas.addEventListener('mousedown', (e) => this.onPointerDown(e));
    canvas.addEventListener('mousemove', (e) => this.onPointerMove(e));
    window.addEventListener('mouseup', () => { this.dragging = false; });
  },

  canvasPoint(event) {
    const canvas = document.getElementById('uiEditorCanvas');
    const rect = canvas.getBoundingClientRect();
    return {
      x: Math.round((event.clientX - rect.left) * canvas.width / rect.width),
      y: Math.round((event.clientY - rect.top) * canvas.height / rect.height),
    };
  },

  onPointerDown(event) {
    const mode = this.currentMode();
    if (!mode) return;
    const point = this.canvasPoint(event);
    for (let i = mode.components.length - 1; i >= 0; i--) {
      const c = mode.components[i];
      if (this.hitTest(c, point.x, point.y)) {
        this.componentIndex = i;
        this.dragging = true;
        this.dragOffsetX = point.x - (c.x || 0);
        this.dragOffsetY = point.y - (c.y || 0);
        const select = document.getElementById('uiComponentSelect');
        if (select) select.value = String(i);
        this.syncInputs();
        this.render();
        return;
      }
    }
  },

  onPointerMove(event) {
    if (!this.dragging) return;
    const c = this.currentComponent();
    if (!c) return;
    const point = this.canvasPoint(event);
    c.x = point.x - this.dragOffsetX;
    c.y = point.y - this.dragOffsetY;
    this.clampComponent(c);
    this.syncInputs();
    this.render();
  },

  hitTest(c, x, y) {
    const w = Math.max(4, c.w || 0);
    const h = Math.max(4, c.h || 0);
    return x >= (c.x || 0) && y >= (c.y || 0) && x <= (c.x || 0) + w && y <= (c.y || 0) + h;
  },

  clampComponent(c) {
    const screen = this.layout.screen;
    const min = this.minSizeForType(c.type);
    c.w = Math.max(min.w, Math.min(screen.width, c.w || min.w));
    c.h = Math.max(min.h, Math.min(screen.height, c.h || min.h));
    if (c.type === 'battery') {
      c.x = Math.max(c.w, Math.min(screen.width - 2, c.x || c.w));
    } else {
      c.x = Math.max(0, Math.min(screen.width - c.w, c.x || 0));
    }
    c.y = Math.max(0, Math.min(screen.height - c.h, c.y || 0));
  },

  minSizeForType(type) {
    switch (type) {
      case 'week_header':
        return { w: 70, h: 16 };
      case 'month_grid':
        return { w: 70, h: 60 };
      case 'time_7seg':
        return { w: 32, h: 32 };
      case 'battery':
        return { w: 4, h: 12 };
      case 'line':
        return { w: 1, h: 1 };
      default:
        return { w: 1, h: 1 };
    }
  },

  normalizePartial(partial, screen) {
    const rect = Array.isArray(partial) ? partial : [0, 0, 0, 0];
    const x = rect[0] || 0;
    const y = rect[1] || 0;
    const w = rect[2] || 0;
    const h = rect[3] || 0;
    if (w === 0 && h === 0) return [0, 0, 0, 0];
    if (x < 0 || y < 0 || w < 8 || h <= 0 || x + w > screen.width || y + h > screen.height) {
      throw new Error('局刷区域超出屏幕范围或宽度小于8像素');
    }
    return [x, y, w, h];
  },

  render() {
    const canvas = document.getElementById('uiEditorCanvas');
    const mode = this.currentMode();
    if (!canvas || !mode) return;
    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.strokeStyle = '#d9e1ef';
    for (let x = 0; x <= canvas.width; x += 80) {
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, canvas.height);
      ctx.stroke();
    }
    for (let y = 0; y <= canvas.height; y += 48) {
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(canvas.width, y);
      ctx.stroke();
    }
    mode.components.forEach((c, index) => {
      const selected = index === this.componentIndex;
      ctx.strokeStyle = selected ? '#2563eb' : '#475569';
      ctx.lineWidth = selected ? 3 : 1;
      ctx.strokeRect(c.x || 0, c.y || 0, Math.max(4, c.w || 0), Math.max(4, c.h || 0));
      ctx.fillStyle = selected ? '#2563eb' : '#475569';
      ctx.font = '14px sans-serif';
      ctx.fillText(c.type, (c.x || 0) + 4, (c.y || 0) + 16);
    });
  },

  packLayout(layout) {
    const screen = layout.screen;
    const components = [];
    const modeRecords = [];
    layout.modes.forEach((mode) => {
      const start = components.length;
      mode.components.forEach((component) => {
        this.clampComponent(component);
        components.push(component);
      });
      modeRecords.push({
        id: mode.id,
        updatePolicy: mode.updatePolicy || 0,
        componentStart: start,
        componentCount: mode.components.length,
        partial: this.normalizePartial(mode.partial, screen),
        flags: mode.flags || 0,
      });
    });

    const modeOffset = this.HEADER_SIZE;
    const componentOffset = modeOffset + modeRecords.length * this.MODE_SIZE;
    const stringOffset = componentOffset + components.length * this.COMPONENT_SIZE;
    const out = new Uint8Array(stringOffset + 1);
    this.writeU32(out, 0, this.MAGIC);
    this.writeU16(out, 4, this.VERSION);
    this.writeU16(out, 6, layout.screen.width);
    this.writeU16(out, 8, layout.screen.height);
    out[10] = modeRecords.length;
    out[11] = components.length;
    this.writeU16(out, 12, modeOffset);
    this.writeU16(out, 14, componentOffset);
    this.writeU16(out, 16, stringOffset);
    this.writeU16(out, 18, 1);

    modeRecords.forEach((mode, i) => {
      const off = modeOffset + i * this.MODE_SIZE;
      out[off] = mode.id;
      out[off + 1] = mode.updatePolicy;
      out[off + 2] = mode.componentStart;
      out[off + 3] = mode.componentCount;
      this.writeU16(out, off + 4, mode.partial[0] || 0);
      this.writeU16(out, off + 6, mode.partial[1] || 0);
      this.writeU16(out, off + 8, mode.partial[2] || 0);
      this.writeU16(out, off + 10, mode.partial[3] || 0);
      this.writeU16(out, off + 12, mode.flags || 0);
    });

    components.forEach((c, i) => {
      const off = componentOffset + i * this.COMPONENT_SIZE;
      out[off] = this.TYPE_IDS[c.type] || 0;
      out[off + 1] = c.dataId || 0;
      out[off + 2] = c.fontId || 0;
      out[off + 3] = c.flags || 0;
      this.writeI16(out, off + 4, c.x || 0);
      this.writeI16(out, off + 6, c.y || 0);
      this.writeI16(out, off + 8, c.w || 0);
      this.writeI16(out, off + 10, c.h || 0);
      this.writeU16(out, off + 12, c.fg || 0);
      this.writeU16(out, off + 14, c.bg || 1);
      this.writeI16(out, off + 16, c.p0 || 0);
      this.writeI16(out, off + 18, c.p1 || 0);
      this.writeI16(out, off + 20, c.p2 || 0);
      this.writeU16(out, off + 22, c.textOffset || 0);
    });
    return out;
  },

  applyToBundle() {
    if (!this.layout || typeof ResourceManager === 'undefined') return;
    const bundle = ResourceManager.getBundle();
    const uiEntry = bundle.entries.find((e) => e.type === 4 && e.id === 1);
    if (!uiEntry) throw new Error('资源包缺少 UI bundle');
    const uiBytes = this.packLayout(this.layout);
    if (uiBytes.length !== uiEntry.length) {
      throw new Error('当前编辑器仅支持不改变组件数量的坐标修改');
    }

    const dataOffset = uiEntry.offset - bundle.dataStart;
    bundle.data.set(uiBytes, dataOffset);
    uiEntry.crc32 = ResourceManager.crc32(uiBytes);
    bundle.uiLayout = JSON.parse(JSON.stringify(this.layout));
    window.EPD_RESOURCE_BUNDLE.uiLayout = JSON.parse(JSON.stringify(this.layout));
    this.rebuildManifest(bundle);

    ResourceManager.latestCheck = { needsUpdate: true, reason: 'UI模板已修改，需要更新资源' };
    ResourceManager.updateUi();
    if (typeof addLog === 'function') addLog('UI模板已保存到网页资源包，请点击“更新资源”写入设备');
  },

  rebuildManifest(bundle) {
    const manifest = new Uint8Array(bundle.manifest.length);
    this.writeU32(manifest, 0, bundle.magic);
    this.writeU16(manifest, 4, bundle.version);
    this.writeU16(manifest, 6, bundle.entries.length);
    bundle.entries.forEach((entry, i) => {
      const off = 8 + i * 16;
      this.writeU16(manifest, off, entry.type);
      this.writeU16(manifest, off + 2, entry.id);
      this.writeU32(manifest, off + 4, entry.offset);
      this.writeU32(manifest, off + 8, entry.length);
      this.writeU32(manifest, off + 12, entry.crc32);
    });
    const manifestCrc = ResourceManager.crc32(manifest);
    this.writeU32(manifest, manifest.length - 4, manifestCrc);
    bundle.manifest = manifest;
    bundle.manifestCrc32 = manifestCrc;
    window.EPD_RESOURCE_BUNDLE.manifestCrc32 = manifestCrc;
  },

  writeU16(data, offset, value) {
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
  },

  writeI16(data, offset, value) {
    this.writeU16(data, offset, value & 0xFFFF);
  },

  writeU32(data, offset, value) {
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
    data[offset + 2] = (value >> 16) & 0xFF;
    data[offset + 3] = (value >> 24) & 0xFF;
  },
};

window.addEventListener('load', () => UIEditor.init());
window.UIEditor = UIEditor;
