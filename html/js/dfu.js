(function () {
  'use strict';

  const CONTROL_UUID = '8ec90001-f315-4f60-9fb8-838830daea50';
  const PACKET_UUID = '8ec90002-f315-4f60-9fb8-838830daea50';
  const BUTTON_UUID = '8ec90003-f315-4f60-9fb8-838830daea50';
  const EPD_APP_SERVICE_UUID = '62750001-d828-918d-fb46-b6c11c675aec';
  const DFU_SERVICE_UUID = 0xFE59;
  const LITTLE_ENDIAN = true;
  const DEFAULT_PACKET_SIZE = 20;
  const INITIAL_DFU_DISCOVERY_TIMEOUT_MS = 8000;
  const NEXT_STAGE_DISCOVERY_TIMEOUT_MS = 12000;
  const DFU_REBOOT_SETTLE_MS = 1600;
  const DFU_RESPONSE_TIMEOUT_MS = 10000;
  const CONNECT_TIMEOUT_MS = 6000;
  const SERVICE_TIMEOUT_MS = 6000;
  const CHARS_TIMEOUT_MS = 5000;
  const AUTO_FIND_RETRY_INTERVAL_MS = 2500;
  const MANUAL_PICK_REQUIRED_ERROR = 'DFU_MANUAL_PICK_REQUIRED';
  const MANUAL_DFU_MODE_REQUIRED_ERROR = 'DFU_MANUAL_MODE_REQUIRED';
  const DFU_BOOTLOADER_PICK_CANCELLED_ERROR = 'DFU_BOOTLOADER_PICK_CANCELLED';
  const DFU_BOOTLOADER_FILTERS = [
    { services: [DFU_SERVICE_UUID] },
    { namePrefix: 'DfuTarg' },
    { namePrefix: 'Dfu' },
    { namePrefix: 'DFU' }
  ];
  const DFU_APP_FILTERS = [
    { services: [EPD_APP_SERVICE_UUID] },
    { namePrefix: 'NRF_EPD' }
  ];

  const OP = {
    BUTTON_COMMAND: [0x01],
    SET_PRN: [0x02],
    CREATE_COMMAND: [0x01, 0x01],
    CREATE_DATA: [0x01, 0x02],
    CALCULATE_CHECKSUM: [0x03],
    EXECUTE: [0x04],
    SELECT_COMMAND: [0x06, 0x01],
    SELECT_DATA: [0x06, 0x02],
    GET_MTU: [0x07],
    RESPONSE: [0x60, 0x20]
  };

  const RESPONSE_MSG = {
    0x00: 'Invalid opcode',
    0x01: 'Operation successful',
    0x02: 'Opcode not supported',
    0x03: 'Missing or invalid parameter value',
    0x04: 'Not enough memory for the data object',
    0x05: 'Object does not match requirements or signature invalid',
    0x07: 'Not a valid object type for Create',
    0x08: 'Operation not permitted in current DFU state',
    0x0A: 'Operation failed',
    0x0B: 'Extended error'
  };

  const EXTENDED_ERROR_MSG = {
    0x00: 'No extended error code set',
    0x01: 'Invalid error code',
    0x02: 'Wrong command format',
    0x03: 'Unsupported or unknown command',
    0x04: 'Invalid init command',
    0x05: 'Firmware version too low',
    0x06: 'Hardware version mismatch',
    0x07: 'SoftDevice FWID mismatch',
    0x08: 'Init packet has no signature',
    0x09: 'Unsupported hash type',
    0x0A: 'Firmware hash calculation failed',
    0x0B: 'Unsupported signature type',
    0x0C: 'Firmware hash mismatch',
    0x0D: 'Insufficient device storage'
  };

  let dfuPackage = null;
  let dfuBusy = false;
  let pendingManualSession = null;
  let packageVersionHint = null;
  let manualBootloaderEntryRequired = false;

  function getDfuPageState() {
    return {
      busy: !!dfuBusy,
      hasPendingSession: !!pendingManualSession,
      manualBootloaderEntryRequired: !!manualBootloaderEntryRequired
    };
  }

  function notifyDfuPageStateChange() {
    if (typeof window.onDfuPageStateChange !== 'function') return;
    try {
      window.onDfuPageStateChange(getDfuPageState());
    } catch (_) { }
  }

  function setPendingManualSession(session) {
    pendingManualSession = session || null;
    notifyDfuPageStateChange();
  }

  function setManualBootloaderEntryRequired(required) {
    manualBootloaderEntryRequired = !!required;
    notifyDfuPageStateChange();
  }

  function getErrorMessage(err) {
    if (!err) return '未知错误';
    if (typeof err === 'string') return err;
    if (err.message) return err.message;
    return String(err);
  }

  function isUserGestureError(err) {
    const msg = getErrorMessage(err);
    return /gesture|user activation|must be handling a user gesture|NotAllowedError/i.test(msg);
  }

  function isPickerCancelled(err) {
    if (!err) return false;
    if (err.name === 'NotFoundError') return true;
    return /User cancelled|cancelled|canceled/i.test(getErrorMessage(err));
  }

  function emitLog(message) {
    const line = `[DFU] ${message}`;
    if (typeof addLog === 'function') addLog(line);
    else console.log(line);
  }

  function setDfuStatus(message, isError) {
    const statusEl = document.getElementById('dfuStatus');
    if (!statusEl) return;
    statusEl.textContent = `状态：${message}`;
    statusEl.classList.toggle('error', !!isError);
  }

  function setDfuRetryInfo(text, isError) {
    const el = document.getElementById('dfuRetryInfo');
    if (!el) return;
    if (!text) {
      el.style.display = 'none';
      el.textContent = '';
      el.classList.remove('error');
      return;
    }
    el.style.display = 'block';
    el.textContent = text;
    el.classList.toggle('error', !!isError);
  }

  function setDfuPrecheck(text, warn = false) {
    const el = document.getElementById('dfuPrecheck');
    if (!el) return;
    if (!text) {
      el.style.display = 'none';
      el.textContent = '';
      el.classList.remove('warn');
      return;
    }
    el.style.display = 'block';
    el.textContent = text;
    el.classList.toggle('warn', !!warn);
  }

  function formatVersionValue(v) {
    if (!Number.isFinite(v)) return '未知';
    return `0x${v.toString(16)} (${v})`;
  }

  function readCurrentDeviceVersion() {
    try {
      if (typeof appVersion !== 'undefined' && Number.isFinite(appVersion)) {
        return Number(appVersion);
      }
    } catch (_) { }
    return null;
  }

  function parseVarint(u8, start) {
    let value = 0;
    let shift = 0;
    let i = start;
    while (i < u8.length) {
      const b = u8[i++];
      value |= (b & 0x7f) << shift;
      if ((b & 0x80) === 0) {
        return { value: value >>> 0, next: i };
      }
      shift += 7;
      if (shift > 35) break;
    }
    return null;
  }

  function collectField3Varints(u8, depth = 0, out = []) {
    if (!u8 || !u8.length || depth > 5) return out;
    let i = 0;
    while (i < u8.length) {
      const keyRes = parseVarint(u8, i);
      if (!keyRes) break;
      i = keyRes.next;
      const key = keyRes.value;
      const fieldNum = key >>> 3;
      const wire = key & 0x07;
      if (wire === 0) {
        const v = parseVarint(u8, i);
        if (!v) break;
        if (fieldNum === 3) out.push(v.value >>> 0);
        i = v.next;
      } else if (wire === 1) {
        i += 8;
      } else if (wire === 2) {
        const lenRes = parseVarint(u8, i);
        if (!lenRes) break;
        i = lenRes.next;
        const len = lenRes.value >>> 0;
        if (i + len > u8.length) break;
        const chunk = u8.slice(i, i + len);
        if (len > 0 && len <= 96) {
          collectField3Varints(chunk, depth + 1, out);
        }
        i += len;
      } else if (wire === 5) {
        i += 4;
      } else {
        break;
      }
    }
    return out;
  }

  function estimatePackageVersion(initData) {
    if (!initData) return null;
    try {
      const u8 = new Uint8Array(initData);
      const vars = collectField3Varints(u8, 0, []);
      const candidates = vars.filter(v => v > 0 && v < 0x100000);
      if (!candidates.length) return null;
      return Math.max(...candidates);
    } catch (_) {
      return null;
    }
  }

  function updateVersionPrecheck(baseImage, appImage) {
    const current = readCurrentDeviceVersion();
    const source = appImage || baseImage;
    const pkg = source ? estimatePackageVersion(source.initData) : packageVersionHint;
    if (source) packageVersionHint = pkg;
    const currentText = current == null ? '设备版本: 当前页未读取（仅校验包版本）' : `设备版本: ${formatVersionValue(current)}`;
    const pkgText = pkg == null ? '包内应用版本: 未解析' : `包内应用版本(推测): ${formatVersionValue(pkg)}`;
    let warn = false;
    let extra = '';
    if (current != null && pkg != null && pkg < current) {
      warn = true;
      extra = ' | 预警: 包版本低于设备版本，可能被Bootloader拒绝(疑似降级)';
    }
    setDfuPrecheck(`${currentText} | ${pkgText}${extra}`, warn);
    return { current, pkg, warn };
  }

  function setManualPickVisible(visible) {
    const btn = document.getElementById('dfuManualPickButton');
    if (!btn) return;
    btn.style.display = visible ? 'inline-block' : 'none';
    if (visible) btn.disabled = false;
  }

  function ensureManualPickVisible(reason = '') {
    const applyVisible = () => {
      const btn = document.getElementById('dfuManualPickButton');
      if (!btn) return;
      btn.style.display = 'inline-block';
      btn.hidden = false;
      btn.disabled = false;
      btn.classList.remove('hide');
    };
    applyVisible();
    setTimeout(applyVisible, 0);
    setTimeout(applyVisible, 300);
    setTimeout(applyVisible, 1000);
    const btn = document.getElementById('dfuManualPickButton');
    if (btn) {
      emitLog(`手动接管按钮已强制显示${reason ? ` (${reason})` : ''}，display=${btn.style.display}`);
    }
  }

  function pulseManualPickButton() {
    const btn = document.getElementById('dfuManualPickButton');
    if (!btn) return;
    btn.classList.remove('manual-pick-attention');
    // 触发reflow，确保重复点击也能重新播放动画
    void btn.offsetWidth;
    btn.classList.add('manual-pick-attention');
    setTimeout(() => {
      btn.classList.remove('manual-pick-attention');
    }, 3200);
  }

  function makeManualPickError(message) {
    const err = new Error(message);
    err.code = MANUAL_PICK_REQUIRED_ERROR;
    return err;
  }

  function resetPendingDfuSession() {
    setPendingManualSession(null);
    setDfuProgress(null);
    setManualPickVisible(false);
    setDfuRetryInfo('');
  }

  function setDfuProgress(state) {
    const wrapEl = document.getElementById('dfuProgressWrap');
    const barEl = document.getElementById('dfuProgressBar');
    const textEl = document.getElementById('dfuProgressText');
    if (!wrapEl || !barEl || !textEl) return;

    if (!state) {
      wrapEl.style.display = 'none';
      barEl.style.width = '0%';
      textEl.textContent = '';
      return;
    }
    wrapEl.style.display = 'block';
    const totalBytes = Math.max(1, Number(state.totalBytes || 0));
    const currentBytes = Math.max(0, Number(state.currentBytes || 0));
    const percent = Math.min(100, (currentBytes / totalBytes) * 100);
    barEl.style.width = `${percent.toFixed(1)}%`;
    textEl.textContent = `${state.object || 'dfu'}: ${percent.toFixed(1)}% (${currentBytes}/${totalBytes} bytes)`;
  }

  async function withTimeout(promise, timeoutMs, message) {
    let timer = null;
    const timeoutPromise = new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(message)), timeoutMs);
    });
    try {
      return await Promise.race([promise, timeoutPromise]);
    } finally {
      if (timer) clearTimeout(timer);
    }
  }

  function makeCrc32() {
    const table = new Int32Array(256);
    for (let i = 0; i < 256; i++) {
      let c = i;
      for (let j = 0; j < 8; j++) {
        c = ((c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1));
      }
      table[i] = c | 0;
    }
    return function crc32Buf(u8) {
      let crc = -1;
      for (let i = 0; i < u8.length; i++) {
        crc = (crc >>> 8) ^ table[(crc ^ u8[i]) & 0xFF];
      }
      return (crc ^ -1) | 0;
    };
  }

  async function inflateRaw(compressedBytes) {
    if (typeof DecompressionStream === 'undefined') {
      throw new Error('浏览器不支持离线ZIP解压(DecompressionStream缺失)');
    }
    const ds = new DecompressionStream('deflate-raw');
    const stream = new Blob([compressedBytes]).stream().pipeThrough(ds);
    const out = await new Response(stream).arrayBuffer();
    return new Uint8Array(out);
  }

  function readLocalFileData(zipBytes, localOffset, compressedSize) {
    const sig = zipBytes[localOffset] |
      (zipBytes[localOffset + 1] << 8) |
      (zipBytes[localOffset + 2] << 16) |
      (zipBytes[localOffset + 3] << 24);
    if ((sig >>> 0) !== 0x04034b50) {
      throw new Error('ZIP本地头校验失败');
    }
    const fileNameLen = zipBytes[localOffset + 26] | (zipBytes[localOffset + 27] << 8);
    const extraLen = zipBytes[localOffset + 28] | (zipBytes[localOffset + 29] << 8);
    const dataStart = localOffset + 30 + fileNameLen + extraLen;
    return zipBytes.slice(dataStart, dataStart + compressedSize);
  }

  async function unzipEntries(arrayBuffer) {
    const zipBytes = new Uint8Array(arrayBuffer);
    let eocdOffset = -1;
    const minEOCD = 22;
    const maxSearch = Math.max(0, zipBytes.length - (0xFFFF + minEOCD));
    for (let i = zipBytes.length - minEOCD; i >= maxSearch; i--) {
      if (zipBytes[i] === 0x50 && zipBytes[i + 1] === 0x4b && zipBytes[i + 2] === 0x05 && zipBytes[i + 3] === 0x06) {
        eocdOffset = i;
        break;
      }
    }
    if (eocdOffset < 0) throw new Error('未找到ZIP中央目录');

    const dv = new DataView(arrayBuffer);
    const centralDirSize = dv.getUint32(eocdOffset + 12, true);
    const centralDirOffset = dv.getUint32(eocdOffset + 16, true);
    const centralEnd = centralDirOffset + centralDirSize;
    const textDecoder = new TextDecoder();
    const files = new Map();

    let ptr = centralDirOffset;
    while (ptr < centralEnd) {
      const sig = dv.getUint32(ptr, true);
      if (sig !== 0x02014b50) break;
      const compression = dv.getUint16(ptr + 10, true);
      const compressedSize = dv.getUint32(ptr + 20, true);
      const fileNameLen = dv.getUint16(ptr + 28, true);
      const extraLen = dv.getUint16(ptr + 30, true);
      const commentLen = dv.getUint16(ptr + 32, true);
      const localOffset = dv.getUint32(ptr + 42, true);

      const nameStart = ptr + 46;
      const fileName = textDecoder.decode(zipBytes.slice(nameStart, nameStart + fileNameLen));
      const compressedData = readLocalFileData(zipBytes, localOffset, compressedSize);
      let fileData;
      if (compression === 0) {
        fileData = compressedData;
      } else if (compression === 8) {
        fileData = await inflateRaw(compressedData);
      } else {
        throw new Error(`ZIP压缩方法不支持: ${compression} (${fileName})`);
      }
      files.set(fileName, fileData);
      ptr = nameStart + fileNameLen + extraLen + commentLen;
    }
    return files;
  }

  class LocalDfuPackage {
    constructor(file) {
      this.file = file;
      this.files = new Map();
      this.manifest = null;
    }

    async load() {
      const bytes = await this.file.arrayBuffer();
      this.files = await unzipEntries(bytes);
      const manifestBytes = this.files.get('manifest.json');
      if (!manifestBytes) throw new Error('未找到 manifest.json，固件包格式不正确');
      const manifestText = new TextDecoder().decode(manifestBytes);
      const parsed = JSON.parse(manifestText);
      if (!parsed.manifest) throw new Error('manifest.json 缺少 manifest 字段');
      this.manifest = parsed.manifest;
      return this;
    }

    getImage(types) {
      for (const type of types) {
        const entry = this.manifest[type];
        if (!entry) continue;
        const initBytes = this.files.get(entry.dat_file);
        const imageBytes = this.files.get(entry.bin_file);
        if (!initBytes || !imageBytes) {
          throw new Error(`固件包缺少文件: ${entry.dat_file || ''} ${entry.bin_file || ''}`);
        }
        return {
          type,
          initFile: entry.dat_file,
          imageFile: entry.bin_file,
          initData: initBytes.buffer.slice(initBytes.byteOffset, initBytes.byteOffset + initBytes.byteLength),
          imageData: imageBytes.buffer.slice(imageBytes.byteOffset, imageBytes.byteOffset + imageBytes.byteLength)
        };
      }
      return null;
    }

    getBaseImage() {
      return this.getImage(['softdevice', 'bootloader', 'softdevice_bootloader']);
    }

    getAppImage() {
      return this.getImage(['application']);
    }
  }

  class LocalSecureDfu {
    constructor(crc32Fn, bluetooth, delayMs = 0) {
      this.crc32 = crc32Fn;
      this.bluetooth = bluetooth || (navigator && navigator.bluetooth);
      this.delay = delayMs;
      this.packetSize = DEFAULT_PACKET_SIZE;
      this.notifyFns = {};
      this.controlChar = null;
      this.packetChar = null;
      this.listeners = { log: [], progress: [] };
      this.connectedDevice = null;
      this.disconnectHandler = null;
      this.connectedControlChar = null;
      this.connectedButtonChar = null;
    }

    addEventListener(type, fn) {
      if (!this.listeners[type]) this.listeners[type] = [];
      this.listeners[type].push(fn);
    }

    dispatchEvent(type, payload) {
      const list = this.listeners[type] || [];
      for (const fn of list) fn(payload);
    }

    log(message) {
      this.dispatchEvent('log', { message });
    }

    progress(object, totalBytes, currentBytes) {
      this.dispatchEvent('progress', { object, totalBytes, currentBytes });
    }

    async delayPromise(ms) {
      await new Promise(resolve => setTimeout(resolve, ms));
    }

    async gattConnect(device, serviceUUID = DFU_SERVICE_UUID) {
      const server = device.gatt.connected
        ? device.gatt
        : await withTimeout(device.gatt.connect(), CONNECT_TIMEOUT_MS, `GATT连接超时(${CONNECT_TIMEOUT_MS}ms)`);
      this.log('connected to gatt server');
      const service = await withTimeout(
        server.getPrimaryService(serviceUUID),
        SERVICE_TIMEOUT_MS,
        `获取DFU服务超时(${SERVICE_TIMEOUT_MS}ms)`
      ).catch(() => {
        throw new Error('Unable to find DFU service');
      });
      this.log('found DFU service');
      return withTimeout(
        service.getCharacteristics(),
        CHARS_TIMEOUT_MS,
        `获取DFU特征超时(${CHARS_TIMEOUT_MS}ms)`
      );
    }

    hasDfuCharacteristics(chars, uuids) {
      const controlChar = chars.find(c => c.uuid === uuids.control);
      const packetChar = chars.find(c => c.uuid === uuids.packet);
      return !!(controlChar && packetChar);
    }

    async tryConnectAsDfu(device, uuids) {
      if (!device || !device.gatt) return false;
      const safeDisconnect = () => {
        try {
          if (device.gatt && device.gatt.connected) {
            device.gatt.disconnect();
          }
        } catch (_) { }
      };
      try {
        const chars = await this.gattConnect(device, uuids.service);
        if (this.hasDfuCharacteristics(chars, uuids)) {
          this.log(`auto-found DFU target: ${device.name || 'Unknown Device'}`);
          return true;
        }
        safeDisconnect();
      } catch (_) {
        safeDisconnect();
      }
      return false;
    }

    async autoFindDfuTarget(preferredDevice, uuids, timeoutMs = 35000) {
      const start = Date.now();
      const lastTriedAt = new Map();
      this.log('正在自动查找 DFU 设备...');
      while (Date.now() - start < timeoutMs) {
        const candidates = [];
        if (preferredDevice) candidates.push(preferredDevice);
        if (this.bluetooth && typeof this.bluetooth.getDevices === 'function') {
          try {
            const knownDevices = await this.bluetooth.getDevices();
            for (const dev of knownDevices) candidates.push(dev);
          } catch (_) { }
        }

        for (const dev of candidates) {
          if (!dev || !dev.gatt) continue;
          const key = `${dev.id || ''}|${dev.name || ''}`;
          const now = Date.now();
          const prev = lastTriedAt.get(key) || 0;
          if (now - prev < AUTO_FIND_RETRY_INTERVAL_MS) continue;
          lastTriedAt.set(key, now);
          if (await this.tryConnectAsDfu(dev, uuids)) return dev;
        }
        await this.delayPromise(600);
      }
      return null;
    }

    handleNotification = (event) => {
      const view = event.target.value;
      if (OP.RESPONSE.indexOf(view.getUint8(0)) < 0) {
        throw new Error('Unrecognised control characteristic response notification');
      }
      const operation = view.getUint8(1);
      const target = this.notifyFns[operation];
      if (!target) return;
      const result = view.getUint8(2);
      if (result === 0x01) {
        target.resolve(new DataView(view.buffer, view.byteOffset + 3, Math.max(0, view.byteLength - 3)));
      } else {
        const err = (result === 0x0B)
          ? `Error: ${EXTENDED_ERROR_MSG[view.getUint8(3)] || 'Unknown extended error'}`
          : `Error: ${RESPONSE_MSG[result] || 'Unknown response error'}`;
        this.log(`notify: ${err}`);
        target.reject(new Error(err));
      }
      delete this.notifyFns[operation];
    };

    bindDisconnectHandler(device) {
      if (!device) return;
      if (this.connectedDevice && this.disconnectHandler) {
        try {
          this.connectedDevice.removeEventListener('gattserverdisconnected', this.disconnectHandler);
        } catch (_) { }
      }
      this.disconnectHandler = () => {
        const pending = this.notifyFns;
        this.notifyFns = {};
        for (const key of Object.keys(pending)) {
          const target = pending[key];
          if (!target || typeof target.reject !== 'function') continue;
          try {
            target.reject(new Error('Device disconnected'));
          } catch (_) { }
        }
        this.controlChar = null;
        this.packetChar = null;
        this.packetSize = DEFAULT_PACKET_SIZE;
        this.connectedDevice = null;
        this.connectedControlChar = null;
        this.connectedButtonChar = null;
      };
      device.addEventListener('gattserverdisconnected', this.disconnectHandler);
      this.connectedDevice = device;
    }

    bindNotificationHandler(characteristic, kind) {
      if (!characteristic) return;
      if (kind === 'control') {
        if (this.connectedControlChar && this.connectedControlChar !== characteristic) {
          try {
            this.connectedControlChar.removeEventListener('characteristicvaluechanged', this.handleNotification);
          } catch (_) { }
        }
        try {
          characteristic.removeEventListener('characteristicvaluechanged', this.handleNotification);
        } catch (_) { }
        characteristic.addEventListener('characteristicvaluechanged', this.handleNotification);
        this.connectedControlChar = characteristic;
        return;
      }
      if (kind === 'button') {
        if (this.connectedButtonChar && this.connectedButtonChar !== characteristic) {
          try {
            this.connectedButtonChar.removeEventListener('characteristicvaluechanged', this.handleNotification);
          } catch (_) { }
        }
        try {
          characteristic.removeEventListener('characteristicvaluechanged', this.handleNotification);
        } catch (_) { }
        characteristic.addEventListener('characteristicvaluechanged', this.handleNotification);
        this.connectedButtonChar = characteristic;
      }
    }

    async connect(device) {
      this.bindDisconnectHandler(device);
      const chars = await this.gattConnect(device);
      this.log(`found ${chars.length} characteristic(s)`);
      this.packetChar = chars.find(c => c.uuid === PACKET_UUID);
      if (!this.packetChar) throw new Error('Unable to find packet characteristic');
      this.log('found packet characteristic');
      this.controlChar = chars.find(c => c.uuid === CONTROL_UUID);
      if (!this.controlChar) throw new Error('Unable to find control characteristic');
      this.log('found control characteristic');
      if (!this.controlChar.properties.notify && !this.controlChar.properties.indicate) {
        throw new Error('Control characteristic does not allow notifications');
      }
      await this.controlChar.startNotifications();
      this.bindNotificationHandler(this.controlChar, 'control');
      this.log('enabled control notifications');
      await this.configureTransport();
      return device;
    }

    async configureTransport() {
      try {
        const prnView = new DataView(new ArrayBuffer(4));
        prnView.setUint32(0, 0, LITTLE_ENDIAN);
        await this.sendControl(OP.SET_PRN, prnView.buffer);
        this.log('configured PRN=0');
      } catch (err) {
        this.log(`配置PRN失败，继续使用默认行为: ${getErrorMessage(err)}`);
      }

      try {
        const resp = await this.sendControl(OP.GET_MTU);
        const mtu = resp.getUint16(0, LITTLE_ENDIAN);
        const negotiated = Math.max(DEFAULT_PACKET_SIZE, Math.min(244, mtu - 3));
        this.packetSize = negotiated;
        this.log(`transport mtu=${mtu}, packet size=${this.packetSize}`);
      } catch (err) {
        this.packetSize = DEFAULT_PACKET_SIZE;
        this.log(`读取MTU失败，回退到${DEFAULT_PACKET_SIZE}字节分片: ${getErrorMessage(err)}`);
      }
    }

    sendOperation(characteristic, operation, buffer) {
      return new Promise((resolve, reject) => {
        let settled = false;
        let timeoutId = null;
        const finish = (fn, value) => {
          if (settled) return;
          settled = true;
          if (timeoutId) clearTimeout(timeoutId);
          delete this.notifyFns[operation[0]];
          fn(value);
        };
        const payload = new Uint8Array(operation.length + (buffer ? buffer.byteLength : 0));
        payload.set(operation);
        if (buffer) payload.set(new Uint8Array(buffer), operation.length);
        this.notifyFns[operation[0]] = {
          resolve: value => finish(resolve, value),
          reject: err => finish(reject, err)
        };
        timeoutId = setTimeout(() => {
          finish(reject, new Error(`DFU response timeout (${DFU_RESPONSE_TIMEOUT_MS}ms)`));
        }, DFU_RESPONSE_TIMEOUT_MS);
        characteristic.writeValue(payload).catch(async (e) => {
          this.log(String(e));
          try {
            await this.delayPromise(500);
            await characteristic.writeValue(payload);
          } catch (retryErr) {
            finish(reject, retryErr);
          }
        });
      });
    }

    async sendControl(operation, buffer) {
      const resp = await this.sendOperation(this.controlChar, operation, buffer);
      if (this.delay > 0) await this.delayPromise(this.delay);
      return resp;
    }

    checkCrc(buffer, crcValue) {
      if (!this.crc32) return true;
      return this.crc32(new Uint8Array(buffer)) === crcValue;
    }

    async writePacket(packet) {
      if (this.packetChar.properties.writeWithoutResponse &&
        typeof this.packetChar.writeValueWithoutResponse === 'function') {
        await this.packetChar.writeValueWithoutResponse(packet);
        return;
      }
      await this.packetChar.writeValue(packet);
    }

    async transferData(dataBuffer, offset, start = 0, objectName, totalBytes) {
      let cursor = start;
      while (cursor < dataBuffer.byteLength) {
        const end = Math.min(cursor + this.packetSize, dataBuffer.byteLength);
        const packet = dataBuffer.slice(cursor, end);
        try {
          await this.writePacket(packet);
        } catch (err) {
          if (this.packetSize > DEFAULT_PACKET_SIZE) {
            this.log(`分片大小 ${this.packetSize} 写入失败，回退到 ${DEFAULT_PACKET_SIZE}: ${getErrorMessage(err)}`);
            this.packetSize = DEFAULT_PACKET_SIZE;
            continue;
          }
          throw err;
        }
        if (this.delay > 0) await this.delayPromise(this.delay);
        this.progress(objectName, totalBytes, offset + end);
        cursor = end;
      }
    }

    async transferObject(buffer, createType, maxSize, offset, objectName) {
      const start = offset - (offset % maxSize);
      const end = Math.min(start + maxSize, buffer.byteLength);
      const view = new DataView(new ArrayBuffer(4));
      view.setUint32(0, end - start, LITTLE_ENDIAN);
      await this.sendControl(createType, view.buffer);
      await this.transferData(buffer.slice(start, end), start, 0, objectName, buffer.byteLength);
      const response = await this.sendControl(OP.CALCULATE_CHECKSUM);
      const crc = response.getInt32(4, LITTLE_ENDIAN);
      const transferred = response.getUint32(0, LITTLE_ENDIAN);
      const data = buffer.slice(0, transferred);
      if (!this.checkCrc(data, crc)) {
        throw new Error('DFU object CRC校验失败');
      }
      this.log(`written ${transferred} bytes`);
      await this.sendControl(OP.EXECUTE);
      if (end < buffer.byteLength) {
        await this.transferObject(buffer, createType, maxSize, transferred, objectName);
      } else {
        this.log('transfer complete');
      }
    }

    async transfer(buffer, objectName, selectType, createType) {
      const resp = await this.sendControl(selectType);
      const maxSize = resp.getUint32(0, LITTLE_ENDIAN);
      const offset = resp.getUint32(4, LITTLE_ENDIAN);
      const crc = resp.getInt32(8, LITTLE_ENDIAN);
      if (objectName === 'init' && offset === buffer.byteLength && this.checkCrc(buffer, crc)) {
        this.log('init packet already available, skipping transfer');
        return;
      }
      this.progress(objectName, buffer.byteLength, 0);
      await this.transferObject(buffer, createType, maxSize, offset, objectName);
    }

    async requestDevice(buttonLess, filters, uuids) {
      const u = Object.assign({
        service: DFU_SERVICE_UUID,
        button: BUTTON_UUID,
        control: CONTROL_UUID,
        packet: PACKET_UUID
      }, uuids || {});
      let useFilters = filters;
      if (!buttonLess && !useFilters) {
        useFilters = [{ services: [u.service] }];
      }
      const optionalServices = [u.service];
      if (EPD_APP_SERVICE_UUID !== u.service) optionalServices.push(EPD_APP_SERVICE_UUID);
      const options = { optionalServices: optionalServices };
      if (useFilters) options.filters = useFilters;
      else options.acceptAllDevices = true;
      const device = await this.bluetooth.requestDevice(options);
      if (!buttonLess) return device;
      return this.setDfuMode(device, u);
    }

    async setDfuMode(device, uuids) {
      this.bindDisconnectHandler(device);
      const chars = await this.gattConnect(device, uuids.service);
      this.log(`found ${chars.length} characteristic(s)`);
      if (this.hasDfuCharacteristics(chars, uuids)) return device;
      const buttonChar = chars.find(c => c.uuid === uuids.button);
      if (!buttonChar) throw new Error('Unsupported device');
      this.log('found buttonless characteristic');
      if (!buttonChar.properties.notify && !buttonChar.properties.indicate) {
        throw new Error('Buttonless characteristic does not allow notifications');
      }
      await buttonChar.startNotifications();
      this.log('enabled buttonless notifications');
      this.bindNotificationHandler(buttonChar, 'button');
      await this.sendOperation(buttonChar, OP.BUTTON_COMMAND);
      this.log('sent DFU mode');

      // 等待应用模式断开。unbonded buttonless DFU 后续应重新发现/重新选择 DfuTarg。
      await new Promise(resolve => {
        let done = false;
        const complete = () => {
          if (done) return;
          done = true;
          this.notifyFns = {};
          resolve();
        };
        device.addEventListener('gattserverdisconnected', complete, { once: true });
        setTimeout(complete, 2500);
      });
      return null;
    }

    async switchToDfuModeOnly(device, uuids) {
      this.bindDisconnectHandler(device);
      let chars = null;
      let connectedServiceName = '';
      try {
        chars = await this.gattConnect(device, uuids.service);
        connectedServiceName = 'DFU';
      } catch (e) {
        // 某些固件在应用态不暴露FE59，回退到应用服务尝试查找Buttonless特征。
        this.log('应用态未发现DFU服务，尝试在应用服务中查找Buttonless特征');
        chars = await this.gattConnect(device, EPD_APP_SERVICE_UUID);
        connectedServiceName = 'APP';
      }

      this.log(`found ${chars.length} characteristic(s) on ${connectedServiceName} service`);
      if (this.hasDfuCharacteristics(chars, uuids)) {
        this.log('设备已处于 DFU 模式，跳过切换');
        return device;
      }
      const buttonChar = chars.find(c => c.uuid === uuids.button);
      if (!buttonChar) throw new Error('Unsupported device');
      this.log('found buttonless characteristic');
      if (!buttonChar.properties.notify && !buttonChar.properties.indicate) {
        throw new Error('Buttonless characteristic does not allow notifications');
      }
      await buttonChar.startNotifications();
      this.log('enabled buttonless notifications');
      this.bindNotificationHandler(buttonChar, 'button');
      await this.sendOperation(buttonChar, OP.BUTTON_COMMAND);
      this.log('sent DFU mode');
      // 等待应用模式断开完成。unbonded buttonless DFU 后续需要重新发现/重连 DfuTarg。
      await new Promise(resolve => {
        let done = false;
        const complete = () => {
          if (done) return;
          done = true;
          this.notifyFns = {};
          resolve();
        };
        device.addEventListener('gattserverdisconnected', complete, { once: true });
        setTimeout(complete, 2500);
      });
      return null;
    }

    async update(device, initData, firmwareData) {
      if (!device) throw new Error('Device not specified');
      if (!initData) throw new Error('Init not specified');
      if (!firmwareData) throw new Error('Firmware not specified');
      try {
        await this.connect(device);
        this.log('transferring init');
        await this.transfer(initData, 'init', OP.SELECT_COMMAND, OP.CREATE_COMMAND);
        this.log('transferring firmware');
        await this.transfer(firmwareData, 'firmware', OP.SELECT_DATA, OP.CREATE_DATA);
        this.log('complete, disconnecting...');
      } catch (err) {
        if (this.delay === 0) {
          this.log('DFU update failed, delay=0 -> retry with delay=10');
          this.delay = 10;
          await this.update(device, initData, firmwareData);
          return;
        }
        throw err;
      }
    }
  }

  async function loadPackageFromFile(file) {
    if (!file) return;
    setDfuProgress(null);
    setDfuStatus('正在解析固件包...');
    const pkg = new LocalDfuPackage(file);
    await pkg.load();
    dfuPackage = pkg;
    const appImage = pkg.getAppImage();
    const baseImage = pkg.getBaseImage();
    updateVersionPrecheck(baseImage, appImage);
    setDfuStatus(`固件包已就绪：${file.name}`);
    emitLog(`固件包已加载: ${file.name}`);
  }

  async function updateOneImage(dfu, device, image) {
    if (!image) return;
    setDfuStatus(`正在升级 ${image.type}: ${image.imageFile}`);
    emitLog(`开始升级 ${image.type}: ${image.imageFile}`);
    await dfu.update(device, image.initData, image.imageData);
  }

  function finalizeDfuSession() {
    setDfuStatus('DFU 升级完成');
    emitLog('DFU 升级完成');
    setDfuProgress(null);
    setDfuRetryInfo('');
    setManualPickVisible(false);
    setPendingManualSession(null);
    setManualBootloaderEntryRequired(false);
    if (typeof showToast === 'function') showToast('DFU升级完成');
    const fileEl = document.getElementById('dfuPackageFile');
    if (fileEl) fileEl.value = '';
    dfuPackage = null;
  }

  function getQueuedImages(baseImage, appImage) {
    return [baseImage, appImage].filter(Boolean);
  }

  async function requestApplicationDevice(dfu) {
    const connected = getConnectedMainDevice();
    if (connected) {
      emitLog(`复用当前已连接应用设备: ${connected.name || 'Unknown Device'}`);
      return connected;
    }
    setDfuStatus('请在弹窗中选择应用设备...');
    emitLog('未检测到现有连接，按官方 unbonded buttonless DFU 流程选择应用设备');
    return dfu.requestDevice(false, DFU_APP_FILTERS);
  }

  async function requestBootloaderDevice(dfu) {
    setDfuStatus('请在弹窗中选择 DFU 设备（如 DfuTarg）...');
    emitLog('等待用户选择 DfuTarg/DFU Bootloader 设备');
    return dfu.requestDevice(false, DFU_BOOTLOADER_FILTERS);
  }

  async function requestInitialDfuTarget(dfu) {
    if (!manualBootloaderEntryRequired) {
      return {
        mode: 'application',
        device: await requestApplicationDevice(dfu)
      };
    }

    setDfuStatus('请先让设备进入 DfuTarg，再在弹窗中选择 DFU 设备...');
    emitLog('当前会话已切换为“手动进入 DFU 模式”流程，直接选择 DfuTarg');
    return {
      mode: 'bootloader',
      device: await requestBootloaderDevice(dfu)
    };
  }

  async function tryResolveAuthorizedBootloaderDevice(dfu, preferredDevice, timeoutMs, phaseLabel) {
    setDfuStatus(`正在查找已授权 DFU 设备（${phaseLabel}）...`);
    emitLog(`尝试在已授权设备中查找 DFU 目标（${phaseLabel}）`);
    setDfuRetryInfo(`正在等待 DfuTarg 出现（${Math.ceil(timeoutMs / 1000)} 秒）...`, false);
    const found = await dfu.autoFindDfuTarget(preferredDevice, {
      service: DFU_SERVICE_UUID,
      control: CONTROL_UUID,
      packet: PACKET_UUID
    }, timeoutMs);
    setDfuRetryInfo('');
    return found;
  }

  async function resolveBootloaderAfterSwitch(dfu, preferredDevice) {
    await dfu.delayPromise(DFU_REBOOT_SETTLE_MS);
    const authorized = await tryResolveAuthorizedBootloaderDevice(
      dfu,
      preferredDevice,
      INITIAL_DFU_DISCOVERY_TIMEOUT_MS,
      '切换到 DFU'
    );
    if (authorized) return authorized;
    try {
      return await requestBootloaderDevice(dfu);
    } catch (err) {
      if (isUserGestureError(err)) {
        throw makeManualPickError('浏览器要求再次确认 DfuTarg，请点击“手动选择DFU设备并继续”');
      }
      if (isPickerCancelled(err)) {
        const cancelled = new Error('已取消选择DFU设备，可点击“手动选择DFU设备并继续”重试');
        cancelled.code = DFU_BOOTLOADER_PICK_CANCELLED_ERROR;
        throw cancelled;
      }
      throw err;
    }
  }

  async function resolveBootloaderForRemainingImages(dfu, preferredDevice, nextImage) {
    await dfu.delayPromise(DFU_REBOOT_SETTLE_MS);
    const authorized = await tryResolveAuthorizedBootloaderDevice(
      dfu,
      preferredDevice,
      NEXT_STAGE_DISCOVERY_TIMEOUT_MS,
      `继续 ${nextImage.type}`
    );
    if (authorized) return authorized;
    ensureManualPickVisible(`继续${nextImage.type}`);
    pulseManualPickButton();
    throw makeManualPickError(`设备已重启，请点击“手动选择DFU设备并继续”继续升级 ${nextImage.type}`);
  }

  async function performDfuTransfer(dfu, device, images) {
    let activeDevice = device;
    for (let index = 0; index < images.length; index++) {
      const image = images[index];
      setPendingManualSession({
        dfu: dfu,
        remainingImages: images.slice(index)
      });
      if (!activeDevice) {
        ensureManualPickVisible(`等待${image.type}`);
        throw makeManualPickError(`请点击“手动选择DFU设备并继续”继续升级 ${image.type}`);
      }
      await updateOneImage(dfu, activeDevice, image);
      if (index < images.length - 1) {
        const nextImage = images[index + 1];
        setPendingManualSession({
          dfu: dfu,
          remainingImages: images.slice(index + 1)
        });
        setDfuStatus(`正在等待设备重启，准备继续升级 ${nextImage.type}...`);
        emitLog(`当前镜像已完成，准备重新连接 DFU 设备继续 ${nextImage.type}`);
        activeDevice = await resolveBootloaderForRemainingImages(dfu, activeDevice, nextImage);
      }
    }
    finalizeDfuSession();
  }

  function getConnectedMainDevice() {
    try {
      if (typeof bleDevice === 'undefined' || !bleDevice || !bleDevice.gatt) return null;
      if (typeof gattServer !== 'undefined' && gattServer && gattServer.connected) return bleDevice;
      if (bleDevice.gatt.connected) return bleDevice;
      return null;
    } catch (_) {
      return null;
    }
  }

  async function startDfuUpgrade() {
    if (dfuBusy) {
      setDfuStatus('DFU 正在执行，请稍候');
      return;
    }
    if (!dfuPackage) {
      setDfuStatus('请先选择 DFU 固件包(.zip)', true);
      if (typeof showToast === 'function') showToast('请先选择 DFU 固件包');
      return;
    }
    if (!navigator.bluetooth) {
      setDfuStatus('当前浏览器不支持 Web Bluetooth', true);
      return;
    }

    const startBtn = document.getElementById('dfuStartButton');
    const manualBtn = document.getElementById('dfuManualPickButton');
    const dfu = new LocalSecureDfu(makeCrc32());
    dfuBusy = true;
    notifyDfuPageStateChange();
    if (startBtn) startBtn.disabled = true;
    if (manualBtn) manualBtn.disabled = true;
    setDfuProgress(null);
    setDfuRetryInfo('');
    setManualPickVisible(false);
    setPendingManualSession(null);

    dfu.addEventListener('log', event => {
      if (event && event.message) emitLog(event.message);
    });
    dfu.addEventListener('progress', event => setDfuProgress(event || null));

    try {
      const baseImage = dfuPackage.getBaseImage();
      const appImage = dfuPackage.getAppImage();
      if (!baseImage && !appImage) {
        throw new Error('固件包中未找到可升级镜像');
      }
      const precheck = updateVersionPrecheck(baseImage, appImage);
      if (precheck.warn) {
        emitLog('预检查提示：检测到疑似降级，设备可能拒绝升级');
        if (typeof showToast === 'function') {
          showToast('预警: 包版本低于设备版本，可能被拒绝', 3000);
        }
      }

      const images = getQueuedImages(baseImage, appImage);
      setPendingManualSession({
        dfu: dfu,
        remainingImages: images.slice()
      });

      const target = await requestInitialDfuTarget(dfu);

      if (target.mode === 'bootloader') {
        setManualPickVisible(false);
        await performDfuTransfer(dfu, target.device, images);
      } else {
        setDfuStatus('正在切换到 DFU 模式...');
        emitLog('正在按 unbonded buttonless DFU 流程切换到 Bootloader...');
        try {
          const maybeDfuDevice = await dfu.switchToDfuModeOnly(target.device, {
            service: DFU_SERVICE_UUID,
            button: BUTTON_UUID,
            control: CONTROL_UUID,
            packet: PACKET_UUID
          });
          setManualPickVisible(false);
          const dfuTargetDevice = maybeDfuDevice || await resolveBootloaderAfterSwitch(dfu, target.device);
          await performDfuTransfer(dfu, dfuTargetDevice, images);
        } catch (switchErr) {
          const switchMsg = getErrorMessage(switchErr);
          if (switchMsg.includes('Unsupported device')) {
            const manualModeErr = new Error(
              '当前固件未暴露Buttonless DFU切换能力，请先手动让设备进入DFU模式(DfuTarg)后，再重新点击“选择设备并升级”。'
            );
            manualModeErr.code = MANUAL_DFU_MODE_REQUIRED_ERROR;
            throw manualModeErr;
          }
          throw switchErr;
        }
      }
    } catch (err) {
      const errMsg = getErrorMessage(err);
      if (err && err.code === MANUAL_DFU_MODE_REQUIRED_ERROR) {
        resetPendingDfuSession();
        setManualBootloaderEntryRequired(true);
        setDfuStatus(getErrorMessage(err), true);
        emitLog('自动切DFU失败：固件不支持Buttonless切换，请手动让设备进入DfuTarg后再升级');
        if (typeof showToast === 'function') {
          showToast('请先手动让设备进入DfuTarg，再重新点击升级', 3600);
        }
      } else if (err && err.code === MANUAL_PICK_REQUIRED_ERROR) {
        ensureManualPickVisible('等待DfuTarg');
        pulseManualPickButton();
        setDfuStatus(errMsg, false);
        emitLog(`等待用户手动选择 DFU 设备继续: ${errMsg}`);
        if (typeof showToast === 'function') {
          showToast(isUserGestureError(err) ? '浏览器要求再次确认 DfuTarg，请点击手动继续' : '请手动选择 DfuTarg 继续');
        }
      } else if (err && err.code === DFU_BOOTLOADER_PICK_CANCELLED_ERROR) {
        ensureManualPickVisible('取消选择DfuTarg');
        setDfuStatus(errMsg, false);
        emitLog(`DFU设备选择已取消，但会话仍可继续: ${errMsg}`);
        if (typeof showToast === 'function') {
          showToast('已取消DfuTarg选择，可点击“手动选择DFU设备并继续”重试');
        }
      } else if (isPickerCancelled(err)) {
        resetPendingDfuSession();
        setDfuStatus('已取消设备选择');
        emitLog('DFU 已取消：用户关闭了设备选择窗口');
        if (typeof showToast === 'function') showToast('已取消设备选择');
      } else {
        resetPendingDfuSession();
        setDfuStatus(`DFU 失败: ${errMsg}`, true);
        emitLog(`DFU 失败: ${errMsg}`);
        if (typeof showToast === 'function') showToast(`DFU失败: ${errMsg}`, 3600);
      }
    } finally {
      dfuBusy = false;
      notifyDfuPageStateChange();
      if (startBtn) startBtn.disabled = false;
      if (manualBtn) manualBtn.disabled = !pendingManualSession;
    }
  }

  async function continueWithManualPick() {
    emitLog('收到手动接管按钮点击');
    if (dfuBusy) {
      setDfuStatus('DFU 正在执行，请稍候后再试');
      return;
    }
    if (!pendingManualSession || !pendingManualSession.dfu) {
      setDfuStatus('没有待继续的DFU会话，请先点击“选择设备并升级”');
      return;
    }

    const startBtn = document.getElementById('dfuStartButton');
    const manualBtn = document.getElementById('dfuManualPickButton');
    const { dfu, remainingImages } = pendingManualSession;
    dfuBusy = true;
    notifyDfuPageStateChange();
    if (startBtn) startBtn.disabled = true;
    if (manualBtn) manualBtn.disabled = true;

    try {
      const device = await requestBootloaderDevice(dfu);
      await performDfuTransfer(dfu, device, remainingImages || []);
    } catch (err) {
      const errMsg = getErrorMessage(err);
      if (isPickerCancelled(err)) {
        ensureManualPickVisible('手动选择取消');
        setDfuStatus('已取消手动选择，可再次点击继续');
        emitLog('DFU 手动选择已取消，当前会话保留，可再次点击继续');
      } else {
        resetPendingDfuSession();
        setDfuStatus(`DFU 失败: ${errMsg}`, true);
        emitLog(`DFU 失败: ${errMsg}`);
        if (typeof showToast === 'function') showToast(`DFU失败: ${errMsg}`, 3600);
      }
    } finally {
      dfuBusy = false;
      notifyDfuPageStateChange();
      if (startBtn) startBtn.disabled = false;
      if (manualBtn) manualBtn.disabled = !pendingManualSession;
    }
  }

  async function runDfuSelfCheck() {
    const checks = [];
    const secureContextOk = !!window.isSecureContext ||
      window.location.protocol === 'https:' ||
      window.location.hostname === 'localhost' ||
      window.location.hostname === '127.0.0.1';
    checks.push({
      name: '安全上下文(HTTPS/localhost)',
      ok: secureContextOk,
      detail: `${window.location.protocol}//${window.location.host}`
    });

    const webBluetoothOk = !!navigator.bluetooth;
    checks.push({
      name: 'Web Bluetooth API',
      ok: webBluetoothOk,
      detail: webBluetoothOk ? 'navigator.bluetooth 可用' : 'navigator.bluetooth 不可用'
    });

    const decompressOk = typeof DecompressionStream !== 'undefined';
    checks.push({
      name: '离线ZIP解压(DecompressionStream)',
      ok: decompressOk,
      detail: decompressOk ? '支持' : '不支持'
    });

    const requestDeviceOk = !!(navigator.bluetooth && navigator.bluetooth.requestDevice);
    checks.push({
      name: 'requestDevice 接口',
      ok: requestDeviceOk,
      detail: requestDeviceOk ? '可用' : '不可用'
    });

    if (navigator.bluetooth && typeof navigator.bluetooth.getAvailability === 'function') {
      try {
        const available = await navigator.bluetooth.getAvailability();
        checks.push({
          name: '系统蓝牙可用性',
          ok: !!available,
          detail: available ? '可用' : '不可用/已关闭'
        });
      } catch (err) {
        checks.push({
          name: '系统蓝牙可用性',
          ok: false,
          detail: `检测失败: ${getErrorMessage(err)}`
        });
      }
    } else {
      checks.push({
        name: '系统蓝牙可用性',
        ok: true,
        detail: '浏览器不支持 getAvailability，跳过'
      });
    }

    const failed = checks.filter(item => !item.ok);
    for (const item of checks) {
      emitLog(`自检 ${item.ok ? '通过' : '失败'}: ${item.name} - ${item.detail}`);
    }

    if (failed.length === 0) {
      setDfuStatus('环境自检通过，可执行离线 DFU');
      if (typeof showToast === 'function') showToast('DFU 环境自检通过');
    } else {
      const failedNames = failed.map(item => item.name).join('、');
      setDfuStatus(`环境自检失败: ${failedNames}`, true);
      if (typeof showToast === 'function') showToast(`DFU 环境自检失败: ${failedNames}`, 3600);
    }
  }

  window.initDfuPanel = function () {
    const fileEl = document.getElementById('dfuPackageFile');
    const startBtn = document.getElementById('dfuStartButton');
    const manualBtn = document.getElementById('dfuManualPickButton');
    const selfCheckBtn = document.getElementById('dfuSelfCheckButton');
    if (!fileEl || !startBtn || !manualBtn || !selfCheckBtn) return;
    if (startBtn.dataset.bound === '1') return;

    fileEl.addEventListener('change', async event => {
      const file = event.target && event.target.files ? event.target.files[0] : null;
      try {
        await loadPackageFromFile(file);
      } catch (err) {
        const errMsg = getErrorMessage(err);
        setDfuStatus(`固件包加载失败: ${errMsg}`, true);
        emitLog(`固件包加载失败: ${errMsg}`);
      }
    });

    startBtn.addEventListener('click', startDfuUpgrade);
    manualBtn.addEventListener('click', continueWithManualPick);
    selfCheckBtn.addEventListener('click', runDfuSelfCheck);
    startBtn.dataset.bound = '1';
    setManualPickVisible(false);
    setDfuPrecheck('');
    setDfuStatus('未选择 DFU 固件包');
  };

  window.isDfuBusy = function () {
    return !!dfuBusy;
  };

  window.getDfuPageState = function () {
    return getDfuPageState();
  };

  window.resetDfuMode = function () {
    if (dfuBusy || pendingManualSession) return false;
    setManualBootloaderEntryRequired(false);
    setDfuStatus('已恢复默认升级流程，请从应用设备重新开始');
    emitLog('已恢复默认 DFU 流程：后续将先选择应用设备再切换到 DfuTarg');
    return true;
  };

  window.abandonPendingDfuSession = function () {
    if (dfuBusy || !pendingManualSession) return false;
    resetPendingDfuSession();
    setDfuStatus('已放弃当前可恢复会话');
    emitLog('已放弃当前可恢复的 DFU 会话');
    return true;
  };
})();
