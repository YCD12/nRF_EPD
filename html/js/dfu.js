(function () {
  'use strict';

  const CONTROL_UUID = '8ec90001-f315-4f60-9fb8-838830daea50';
  const PACKET_UUID = '8ec90002-f315-4f60-9fb8-838830daea50';
  const BUTTON_UUID = '8ec90003-f315-4f60-9fb8-838830daea50';
  const EPD_APP_SERVICE_UUID = '62750001-d828-918d-fb46-b6c11c675aec';
  const DFU_SERVICE_UUID = 0xFE59;
  const LITTLE_ENDIAN = true;
  const PACKET_SIZE = 20;
  const CONNECT_TIMEOUT_MS = 6000;
  const SERVICE_TIMEOUT_MS = 6000;
  const CHARS_TIMEOUT_MS = 5000;
  const AUTO_FIND_RETRY_INTERVAL_MS = 2500;
  const AUTO_FIND_COUNTDOWN_TICK_MS = 250;
  const MANUAL_PICK_REQUIRED_ERROR = 'DFU_MANUAL_PICK_REQUIRED';
  const MANUAL_DFU_MODE_REQUIRED_ERROR = 'DFU_MANUAL_MODE_REQUIRED';
  const DFU_MANUAL_NAME_FILTERS = [
    { namePrefix: 'DfuTarg' },
    { namePrefix: 'Dfu' },
    { namePrefix: 'DFU' },
    { namePrefix: 'NRF_EPD' }
  ];

  const OP = {
    BUTTON_COMMAND: [0x01],
    CREATE_COMMAND: [0x01, 0x01],
    CREATE_DATA: [0x01, 0x02],
    CALCULATE_CHECKSUM: [0x03],
    EXECUTE: [0x04],
    SELECT_COMMAND: [0x06, 0x01],
    SELECT_DATA: [0x06, 0x02],
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
  let autoFindAbortRequested = false;
  let manualTakeoverRequestedWhileBusy = false;
  let queuedManualPick = false;
  let packageVersionHint = null;
  const AUTO_FIND_ATTEMPTS = 3;
  const AUTO_FIND_TIMEOUT_MS = 20000;

  function getErrorMessage(err) {
    if (!err) return '未知错误';
    if (typeof err === 'string') return err;
    if (err.message) return err.message;
    return String(err);
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

  function formatDuration(totalSeconds) {
    const sec = Math.max(0, Math.floor(totalSeconds));
    const mm = String(Math.floor(sec / 60)).padStart(2, '0');
    const ss = String(sec % 60).padStart(2, '0');
    return `${mm}:${ss}`;
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
    const currentText = current == null ? '设备版本: 未读取（请先连接设备）' : `设备版本: ${formatVersionValue(current)}`;
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
      this.notifyFns = {};
      this.controlChar = null;
      this.packetChar = null;
      this.listeners = { log: [], progress: [] };
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
        if (autoFindAbortRequested) {
          this.log('检测到手动接管请求，停止自动查找');
          return null;
        }
        const candidates = [];
        if (preferredDevice) candidates.push(preferredDevice);
        if (this.bluetooth && typeof this.bluetooth.getDevices === 'function') {
          try {
            const knownDevices = await this.bluetooth.getDevices();
            for (const dev of knownDevices) candidates.push(dev);
          } catch (_) { }
        }

        for (const dev of candidates) {
          if (autoFindAbortRequested) return null;
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
        target.resolve(new DataView(view.buffer, 3));
      } else {
        const err = (result === 0x0B)
          ? `Error: ${EXTENDED_ERROR_MSG[view.getUint8(3)] || 'Unknown extended error'}`
          : `Error: ${RESPONSE_MSG[result] || 'Unknown response error'}`;
        this.log(`notify: ${err}`);
        target.reject(new Error(err));
      }
      delete this.notifyFns[operation];
    };

    async connect(device) {
      device.addEventListener('gattserverdisconnected', () => {
        this.notifyFns = {};
        this.controlChar = null;
        this.packetChar = null;
      });
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
      this.controlChar.addEventListener('characteristicvaluechanged', this.handleNotification);
      this.log('enabled control notifications');
      return device;
    }

    sendOperation(characteristic, operation, buffer) {
      return new Promise((resolve, reject) => {
        const payload = new Uint8Array(operation.length + (buffer ? buffer.byteLength : 0));
        payload.set(operation);
        if (buffer) payload.set(new Uint8Array(buffer), operation.length);
        this.notifyFns[operation[0]] = { resolve, reject };
        characteristic.writeValue(payload).catch(async (e) => {
          this.log(String(e));
          await this.delayPromise(500);
          await characteristic.writeValue(payload);
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

    async transferData(dataBuffer, offset, start = 0, objectName, totalBytes) {
      const end = Math.min(start + PACKET_SIZE, dataBuffer.byteLength);
      const packet = dataBuffer.slice(start, end);
      await this.packetChar.writeValue(packet);
      if (this.delay > 0) await this.delayPromise(this.delay);
      this.progress(objectName, totalBytes, offset + end);
      if (end < dataBuffer.byteLength) {
        await this.transferData(dataBuffer, offset, end, objectName, totalBytes);
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
      const options = { optionalServices: [u.service] };
      if (useFilters) options.filters = useFilters;
      else options.acceptAllDevices = true;
      const device = await this.bluetooth.requestDevice(options);
      if (!buttonLess) return device;
      return this.setDfuMode(device, u);
    }

    async setDfuMode(device, uuids) {
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
      buttonChar.addEventListener('characteristicvaluechanged', this.handleNotification);
      await this.sendOperation(buttonChar, OP.BUTTON_COMMAND);
      this.log('sent DFU mode');

      // 等待应用模式断开，再自动定位切换后的DFU设备，尽量避免二次手动选择。
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

      for (let attempt = 1; attempt <= AUTO_FIND_ATTEMPTS; attempt++) {
        if (autoFindAbortRequested) break;
        this.log(`自动查找DFU设备，第 ${attempt}/${AUTO_FIND_ATTEMPTS} 轮`);
        if (attempt >= 2) {
          // 从第2轮开始强制确保手动接管按钮可见，避免偶发未刷新显示。
          ensureManualPickVisible(`第${attempt}轮开始`);
        }
        setDfuStatus(`正在自动查找 DFU 设备（第 ${attempt}/${AUTO_FIND_ATTEMPTS} 轮）...`);
        const roundStartAt = Date.now();
        const timer = setInterval(() => {
          const elapsedMs = Date.now() - roundStartAt;
          const leftSec = Math.max(0, Math.ceil((AUTO_FIND_TIMEOUT_MS - elapsedMs) / 1000));
          setDfuRetryInfo(`自动查找倒计时（第 ${attempt}/${AUTO_FIND_ATTEMPTS} 轮）：${formatDuration(leftSec)}`);
        }, AUTO_FIND_COUNTDOWN_TICK_MS);
        let autoFound = null;
        try {
          autoFound = await this.autoFindDfuTarget(device, uuids, AUTO_FIND_TIMEOUT_MS);
        } finally {
          clearInterval(timer);
        }
        if (autoFound) {
          setDfuRetryInfo('');
          return autoFound;
        }
        this.log(`第 ${attempt}/${AUTO_FIND_ATTEMPTS} 轮自动查找超时`);
        if (attempt === 1) {
          ensureManualPickVisible('第1轮超时');
          setDfuRetryInfo('已完成第1轮自动查找。可点击“手动选择DFU设备并继续”随时接管。', false);
          this.log('已提前开放手动接管按钮');
          pulseManualPickButton();
          if (typeof showToast === 'function') {
            showToast('已开放手动接管：可点击“手动选择DFU设备并继续”');
          }
        }
      }

      if (autoFindAbortRequested) {
        const manualAbortErr = new Error('用户请求手动接管DFU设备选择');
        manualAbortErr.code = MANUAL_PICK_REQUIRED_ERROR;
        throw manualAbortErr;
      }

      setDfuRetryInfo('自动查找失败：已完成 3 轮自动查找。', true);
      this.log('自动查找失败：已完成 3 轮自动查找');
      const manualErr = new Error('自动查找失败，需要用户手动选择DFU设备继续');
      manualErr.code = MANUAL_PICK_REQUIRED_ERROR;
      throw manualErr;
    }

    async switchToDfuModeOnly(device, uuids) {
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
      buttonChar.addEventListener('characteristicvaluechanged', this.handleNotification);
      await this.sendOperation(buttonChar, OP.BUTTON_COMMAND);
      this.log('sent DFU mode');
      // 等待应用模式断开完成
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

  async function performDfuTransfer(dfu, device, baseImage, appImage) {
    await updateOneImage(dfu, device, baseImage);
    await updateOneImage(dfu, device, appImage);
    setDfuStatus('DFU 升级完成');
    emitLog('DFU 升级完成');
    setDfuProgress(null);
    setDfuRetryInfo('');
    setManualPickVisible(false);
    pendingManualSession = null;
    queuedManualPick = false;
    if (typeof showToast === 'function') showToast('DFU升级完成');
    const fileEl = document.getElementById('dfuPackageFile');
    if (fileEl) fileEl.value = '';
    dfuPackage = null;
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

  async function pickUpgradeTargetDevice(dfu) {
    // 按约定流程：先用上方“连接”按钮连上设备，再点DFU。
    const connected = getConnectedMainDevice();
    if (connected) {
      emitLog(`复用当前已连接设备: ${connected.name || 'Unknown Device'}`);
      return connected;
    }
    setDfuStatus('请先使用上方“连接”按钮连接设备后，再执行DFU升级', true);
    emitLog('未检测到上方已连接设备，已终止DFU流程');
    if (typeof showToast === 'function') {
      showToast('请先连接设备，再执行DFU升级', 3000);
    }
    return null;
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
    autoFindAbortRequested = false;
    manualTakeoverRequestedWhileBusy = false;
    queuedManualPick = false;
    if (startBtn) startBtn.disabled = true;
    // 自动查找阶段需要允许用户点击“手动接管”按钮，所以这里不禁用 manualBtn
    setDfuProgress(null);
    setDfuRetryInfo('');
    setManualPickVisible(false);
    pendingManualSession = null;

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

      setDfuStatus('正在选择设备...');
      emitLog('开始选择升级目标设备');
      const selectedDevice = await pickUpgradeTargetDevice(dfu);
      if (!selectedDevice) {
        return;
      }

      setDfuStatus('正在切换 DFU 模式...');
      emitLog('正在切换到 DFU 模式...');
      pendingManualSession = {
        dfu: dfu,
        baseImage: baseImage,
        appImage: appImage
      };
      let maybeDfuDevice = null;
      try {
        maybeDfuDevice = await dfu.switchToDfuModeOnly(selectedDevice, {
          service: DFU_SERVICE_UUID,
          button: BUTTON_UUID,
          control: CONTROL_UUID,
          packet: PACKET_UUID
        });
      } catch (switchErr) {
        const switchMsg = getErrorMessage(switchErr);
        if (switchMsg.includes('Unsupported device')) {
          const manualModeErr = new Error(
            '当前固件未暴露Buttonless DFU切换能力，请先手动让设备进入DFU模式(DfuTarg)后再继续。'
          );
          manualModeErr.code = MANUAL_DFU_MODE_REQUIRED_ERROR;
          throw manualModeErr;
        }
        throw switchErr;
      }
      setDfuRetryInfo('');
      setManualPickVisible(false);

      let dfuTargetDevice = maybeDfuDevice;
      if (!dfuTargetDevice) {
        setDfuStatus('请在弹窗中选择 DFU 设备（如 DfuTarg）...');
        emitLog('等待用户手动选择 DFU 设备');
        dfuTargetDevice = await dfu.requestDevice(false, DFU_MANUAL_NAME_FILTERS);
      }

      await performDfuTransfer(dfu, dfuTargetDevice, baseImage, appImage);
    } catch (err) {
      const errMsg = getErrorMessage(err);
      if (err && err.code === MANUAL_DFU_MODE_REQUIRED_ERROR) {
        setDfuStatus(getErrorMessage(err), true);
        emitLog('自动切DFU失败：固件不支持Buttonless切换，请手动让设备进入DfuTarg后再升级');
        if (typeof showToast === 'function') {
          showToast('请先手动让设备进入DfuTarg，再点击升级', 3600);
        }
      } else if (err && err.code === MANUAL_PICK_REQUIRED_ERROR) {
        if (!pendingManualSession) {
          pendingManualSession = {
            dfu: dfu,
            baseImage: dfuPackage ? dfuPackage.getBaseImage() : null,
            appImage: dfuPackage ? dfuPackage.getAppImage() : null
          };
        }
        ensureManualPickVisible('自动查找失败');
        setDfuStatus('自动查找失败（可能是 DfuTarg 未授权），请点击“手动选择DFU设备并继续”');
        emitLog('自动查找失败：可能无法访问未授权的 DfuTarg，等待用户手动选择继续');
        if (manualTakeoverRequestedWhileBusy) {
          pulseManualPickButton();
          if (queuedManualPick) {
            emitLog('检测到手动接管已排队，自动进入手动选择流程');
            setDfuStatus('自动查找已停止，正在打开手动选择...');
            setTimeout(() => { continueWithManualPick(true); }, 0);
          } else if (typeof showToast === 'function') {
            showToast('自动查找已停止，请再次点击“手动选择DFU设备并继续”');
          }
          manualTakeoverRequestedWhileBusy = false;
        } else if (typeof showToast === 'function') {
          showToast('自动查找失败，请手动选择 DfuTarg 继续');
        }
      } else if (errMsg.includes('User cancelled')) {
        setDfuStatus('已取消设备选择');
        emitLog('DFU 已取消：用户关闭了设备选择窗口');
        if (typeof showToast === 'function') showToast('已取消设备选择');
      } else {
        setDfuStatus(`DFU 失败: ${errMsg}`, true);
        emitLog(`DFU 失败: ${errMsg}`);
        if (typeof showToast === 'function') showToast(`DFU失败: ${errMsg}`, 3600);
      }
    } finally {
      dfuBusy = false;
      if (startBtn) startBtn.disabled = false;
      if (manualBtn) manualBtn.disabled = false;
    }
  }

  async function continueWithManualPick(fromQueuedAuto = false) {
    emitLog('收到手动接管按钮点击');
    if (dfuBusy) {
      autoFindAbortRequested = true;
      manualTakeoverRequestedWhileBusy = true;
      queuedManualPick = true;
      setDfuStatus('正在停止自动查找，已为你排队手动接管...');
      emitLog('已请求停止自动查找，手动接管已排队');
      return;
    }
    if (!pendingManualSession || !pendingManualSession.dfu) {
      setDfuStatus('没有待继续的DFU会话，请先点击“选择设备并升级”');
      queuedManualPick = false;
      return;
    }

    const startBtn = document.getElementById('dfuStartButton');
    const manualBtn = document.getElementById('dfuManualPickButton');
    const { dfu, baseImage, appImage } = pendingManualSession;
    dfuBusy = true;
    if (startBtn) startBtn.disabled = true;
    if (manualBtn) manualBtn.disabled = true;

    try {
      setDfuStatus('请手动选择 DFU 设备...');
      emitLog('进入手动选择 DFU 设备流程');
      // 手动兜底场景下优先按常见DFU名称过滤，避免只按服务UUID时看不到DfuTarg。
      const device = await dfu.requestDevice(false, DFU_MANUAL_NAME_FILTERS);
      queuedManualPick = false;
      await performDfuTransfer(dfu, device, baseImage, appImage);
    } catch (err) {
      const errMsg = getErrorMessage(err);
      if (errMsg.includes('User cancelled')) {
        if (fromQueuedAuto && /gesture|user activation|must be handling a user gesture|NotAllowedError/i.test(errMsg)) {
          setDfuStatus('浏览器限制：请再点击一次“手动选择DFU设备并继续”');
          emitLog('自动排队进入手动选择被浏览器手势策略拦截，请再点击一次按钮');
          if (typeof showToast === 'function') showToast('请再点一次“手动选择DFU设备并继续”');
        } else {
          setDfuStatus('已取消手动选择');
          emitLog('DFU 手动选择已取消');
        }
      } else {
        setDfuStatus(`DFU 失败: ${errMsg}`, true);
        emitLog(`DFU 失败: ${errMsg}`);
        if (typeof showToast === 'function') showToast(`DFU失败: ${errMsg}`, 3600);
      }
    } finally {
      dfuBusy = false;
      if (startBtn) startBtn.disabled = false;
      if (manualBtn) manualBtn.disabled = false;
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
})();
