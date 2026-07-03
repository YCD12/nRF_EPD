/**
 * External FM25Q04B resource manager.
 *
 * Handles resource status checks, manifest/sample verification, and reliable
 * resource package updates over the existing EPD BLE characteristic.
 */
const ResourceManager = {
  CMD_RES_ERASE: 0x40,
  CMD_RES_WRITE: 0x41,
  CMD_RES_COMMIT: 0x42,
  CMD_RES_STATUS: 0x43,
  CMD_RES_READ: 0x44,

  RSP_RESOURCE_STATUS: 0xA2,
  RSP_RESOURCE_OP: 0xA3,
  RSP_RESOURCE_READ: 0xA4,

  RESOURCE_PROTOCOL_VERSION: 0x22,
  LOW_BATTERY_THRESHOLD_MV: 2800,
  MAX_RETRIES: 3,
  SAMPLE_BYTES: 32,
  NRF_ERROR_BUSY: 17,
  NRF_ERROR_INVALID_DATA: 9,
  NRF_ERROR_NOT_SUPPORTED: 11,

  seq: 1,
  statusResolver: null,
  opResolvers: new Map(),
  readResolvers: new Map(),
  decodedBundle: null,
  latestStatus: null,
  latestCheck: null,
  isUpdating: false,
  isChecking: false,
  isClearing: false,
  busyOperation: null,
  crcTable: null,
  get isBusy() {
    return !!this.busyOperation || this.isUpdating || this.isChecking || this.isClearing;
  },

  init() {
    this.setUiStatus('等待检查');
    this.updateUi();
  },

  onDisconnect() {
    this.latestStatus = null;
    this.latestCheck = null;
    this.isUpdating = false;
    this.isChecking = false;
    this.isClearing = false;
    this.busyOperation = null;
    this.statusResolver = null;
    this.opResolvers.clear();
    this.readResolvers.clear();
    this.setUiStatus('未连接');
    this.updateUi();
  },

  isConnected() {
    return typeof epdCharacteristic !== 'undefined' &&
      epdCharacteristic !== null &&
      typeof bleDevice !== 'undefined' &&
      bleDevice !== null &&
      bleDevice.gatt &&
      bleDevice.gatt.connected;
  },

  isSupported(firmwareVersion) {
    return typeof firmwareVersion === 'number' && firmwareVersion >= this.RESOURCE_PROTOCOL_VERSION;
  },

  handleNotification(value) {
    let data;
    if (value instanceof DataView) {
      data = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    } else if (value instanceof Uint8Array) {
      data = value;
    } else {
      data = new Uint8Array(value);
    }

    if (data[0] === this.RSP_RESOURCE_STATUS) {
      const status = this.parseStatus(data);
      this.latestStatus = status;
      if (this.statusResolver) {
        const resolver = this.statusResolver;
        this.statusResolver = null;
        resolver(status);
      }
      this.updateUi();
      return true;
    }

    if (data[0] === this.RSP_RESOURCE_OP) {
      const op = {
        cmd: data[1],
        seq: this.readU16(data, 2),
        status: this.readU32(data, 4),
      };
      const key = this.opKey(op.cmd, op.seq);
      const resolver = this.opResolvers.get(key);
      if (resolver) {
        this.opResolvers.delete(key);
        resolver(op);
      }
      return true;
    }

    if (data[0] === this.RSP_RESOURCE_READ) {
      const seq = this.readU16(data, 1);
      const status = data[3];
      const address = data[4] | (data[5] << 8) | (data[6] << 16);
      const length = data[7] || 0;
      const payload = data.slice(8, 8 + length);
      const recvCrc = data.length >= 10 + length ? this.readU16(data, 8 + length) : -1;
      const calcCrc = this.crc16(payload);
      const resolver = this.readResolvers.get(seq);
      if (resolver) {
        this.readResolvers.delete(seq);
        resolver({ seq, status, address, payload, crcOk: recvCrc === calcCrc });
      }
      return true;
    }

    return false;
  },

  async checkOnConnect(firmwareVersion) {
    if (!this.isSupported(firmwareVersion)) {
      this.latestCheck = { needsUpdate: false, reason: '当前固件不支持外置资源协议' };
      this.setUiStatus('固件不支持');
      this.updateUi();
      return this.latestCheck;
    }

    try {
      return await this.checkResources();
    } catch (e) {
      console.error(e);
      this.latestCheck = { needsUpdate: true, reason: `资源检查失败: ${e.message}` };
      this.setUiStatus('检查失败');
      this.updateUi();
      if (typeof addLog === 'function') addLog(this.latestCheck.reason);
      return this.latestCheck;
    }
  },

  beginOperation(label) {
    if (this.busyOperation) {
      throw new Error(`${this.busyOperation}正在进行，请稍后再试`);
    }
    this.busyOperation = label;
    this.updateButtonLocks();
    this.updateUi();
  },

  endOperation() {
    this.busyOperation = null;
    this.updateButtonLocks();
    this.updateUi();
  },

  async checkResources(options = {}) {
    const ownLock = !options.skipLock;
    if (!this.isConnected()) throw new Error('BLE未连接');
    const bundle = this.getBundle();

    if (ownLock) {
      this.beginOperation('资源检查');
      this.isChecking = true;
    }

    try {
      this.setUiStatus('读取状态');
      const status = await this.queryStatus();
      this.latestStatus = status;
      if (status.statusCode === this.NRF_ERROR_BUSY) {
        throw new Error('设备正在刷新屏幕，请稍后再试');
      }
      if (!status.flashReady) {
        this.latestCheck = { needsUpdate: true, reason: '外置Flash未就绪' };
        this.updateUi();
        return this.latestCheck;
      }

      if (status.uiBundleError) {
        const reason = this.describeUiBundleError(status.uiBundleError);
        this.latestCheck = { needsUpdate: true, reason };
        this.updateUi();
        return this.latestCheck;
      }

      this.setUiStatus('读取manifest');
      const deviceManifestBytes = await this.readRaw(bundle.manifestAddress, bundle.manifest.length);
      const deviceManifest = this.parseManifest(deviceManifestBytes);
      if (!deviceManifest.valid) {
        this.latestCheck = { needsUpdate: true, reason: deviceManifest.reason || '设备manifest无效' };
        this.updateUi();
        return this.latestCheck;
      }

      if (deviceManifest.crc32 !== bundle.manifestCrc32) {
        this.latestCheck = { needsUpdate: true, reason: 'manifest CRC不一致' };
        this.updateUi();
        return this.latestCheck;
      }

      const entryMismatch = this.compareEntries(deviceManifest.entries, bundle.entries);
      if (entryMismatch) {
        this.latestCheck = { needsUpdate: true, reason: entryMismatch };
        this.updateUi();
        return this.latestCheck;
      }

      this.setUiStatus('抽样校验');
      const sampleMismatch = await this.verifySamples(bundle.entries, bundle);
      if (sampleMismatch) {
        this.latestCheck = { needsUpdate: true, reason: sampleMismatch };
        this.updateUi();
        return this.latestCheck;
      }

      this.latestCheck = { needsUpdate: false, reason: '资源已是最新' };
      this.setUiStatus('已是最新');
      this.updateUi();
      return this.latestCheck;
    } finally {
      if (ownLock) {
        this.isChecking = false;
        this.endOperation();
      }
    }
  },

  async updateResources() {
    if (!this.isConnected()) throw new Error('BLE未连接');
    if (this.isLowBattery()) throw new Error('电量过低，已阻止外置Flash擦写');

    const bundle = this.getBundle();
    this.beginOperation('资源更新');
    this.isUpdating = true;
    this.updateButtonLocks();
    this.updateUi();

    try {
      this.setUiStatus('擦除资源区');
      await this.eraseRange(bundle.dataStart, this.alignSector(bundle.data.length));
      await this.eraseRange(bundle.manifestAddress, 4096);
      if (bundle.stagingManifestAddress !== bundle.manifestAddress) {
        await this.eraseRange(bundle.stagingManifestAddress, 4096);
      }

      this.setUiStatus('写入资源数据');
      await this.writeSegment(bundle.dataStart, bundle.data, '资源数据');

      this.setUiStatus('写入staging manifest');
      await this.writeSegment(bundle.stagingManifestAddress, bundle.manifest, 'manifest');

      this.setUiStatus('写后完整校验');
      const resourceMismatch = await this.verifyResourceCrcs(bundle.entries, bundle);
      if (resourceMismatch) throw new Error(resourceMismatch);

      this.setUiStatus('提交manifest');
      await this.commitManifest();

      this.setUiStatus('提交后复核');
      const check = await this.checkResources({ skipLock: true });
      if (check.needsUpdate) throw new Error(check.reason);

      if (typeof addLog === 'function') addLog('外置Flash资源更新完成');
      if (typeof showToast === 'function') showToast('外置Flash资源已更新');
      return true;
    } finally {
      this.isUpdating = false;
      this.endOperation();
    }
  },

  async clearFlash() {
    if (!this.isConnected()) throw new Error('BLE未连接');
    if (this.isLowBattery()) throw new Error('电量过低，已阻止外置Flash擦写');

    const bundle = this.getBundle();
    this.beginOperation('清空外置Flash');
    this.isClearing = true;
    this.updateButtonLocks();
    this.updateUi();

    try {
      this.setUiStatus('清空全部Flash');
      const status = this.latestStatus || await this.queryStatus();
      const flashTotal = status && status.flashTotalBytes ? status.flashTotalBytes : (bundle.scratchAddress + 4096);
      await this.eraseRange(0, flashTotal, 90000);

      this.latestCheck = { needsUpdate: true, reason: '外置Flash已清空，需要更新资源' };
      this.latestStatus = await this.queryStatus();
      this.setUiStatus('已清空');
      this.updateUi();
      if (typeof addLog === 'function') addLog('外置Flash资源区已强制清空');
      if (typeof showToast === 'function') showToast('外置Flash已清空，请更新资源');
      return true;
    } finally {
      this.isClearing = false;
      this.endOperation();
    }
  },

  async queryStatus(timeout = 2500) {
    return new Promise(async (resolve, reject) => {
      let response = null;
      let writeDone = false;
      let settled = false;
      const finish = () => {
        if (!settled && writeDone && response) {
          settled = true;
          resolve(response);
        }
      };
      const timer = setTimeout(() => {
        this.statusResolver = null;
        settled = true;
        reject(new Error('资源状态查询超时'));
      }, timeout);

      this.statusResolver = (status) => {
        response = status;
        if (writeDone) clearTimeout(timer);
        finish();
      };

      const ok = await write(this.CMD_RES_STATUS);
      writeDone = true;
      if (!ok) {
        clearTimeout(timer);
        this.statusResolver = null;
        settled = true;
        reject(new Error('资源状态命令发送失败'));
        return;
      }
      if (response) clearTimeout(timer);
      finish();
    });
  },

  async eraseRange(address, length, timeout = 12000) {
    const payload = new Uint8Array(7);
    this.writeU24(payload, 0, address);
    this.writeU32(payload, 3, length);
    const op = await this.sendOp(this.CMD_RES_ERASE, 0, payload, timeout);
    if (op.status === this.NRF_ERROR_BUSY) throw new Error('设备正在刷新屏幕，请稍后再试');
    if (op.status !== 0) throw new Error(`资源区擦除失败: ${op.status}`);
  },

  async commitManifest() {
    const op = await this.sendOp(this.CMD_RES_COMMIT, 0, null, 12000);
    if (op.status === this.NRF_ERROR_BUSY) throw new Error('设备正在刷新屏幕，请稍后再试');
    if (op.status !== 0) throw new Error(`manifest提交失败: ${op.status}`);
  },

  async writeSegment(baseAddress, data, label) {
    const mtu = this.getMtu();
    const chunkSize = Math.max(8, mtu - 8);
    const total = Math.ceil(data.length / chunkSize);
    for (let offset = 0; offset < data.length; offset += chunkSize) {
      const chunk = data.slice(offset, Math.min(offset + chunkSize, data.length));
      const seq = this.nextSeq();
      const packet = new Uint8Array(5 + chunk.length + 2);
      packet[0] = seq & 0xFF;
      packet[1] = seq >> 8;
      this.writeU24(packet, 2, baseAddress + offset);
      packet.set(chunk, 5);
      const crc = this.crc16(chunk);
      packet[5 + chunk.length] = crc & 0xFF;
      packet[6 + chunk.length] = crc >> 8;

      let lastError = null;
      for (let attempt = 0; attempt < this.MAX_RETRIES; attempt++) {
        try {
          const op = await this.sendOp(this.CMD_RES_WRITE, seq, packet, 3000);
          if (op.status === 0) {
            lastError = null;
            break;
          }
          if (op.status === this.NRF_ERROR_BUSY) {
            lastError = new Error('设备正在刷新屏幕，请稍后再试');
            break;
          }
          lastError = new Error(`写入失败: ${op.status}`);
        } catch (e) {
          lastError = e;
        }
      }
      if (lastError) throw lastError;

      const done = Math.min(total, Math.floor(offset / chunkSize) + 1);
      this.setUiStatus(`${label} ${done}/${total}`);
    }
  },

  async readRaw(address, length) {
    const mtu = this.getMtu();
    const chunkSize = Math.max(8, mtu - 10);
    const out = new Uint8Array(length);
    let pos = 0;

    while (pos < length) {
      const readLen = Math.min(chunkSize, length - pos);
      const seq = this.nextSeq();
      const request = new Uint8Array(6);
      request[0] = seq & 0xFF;
      request[1] = seq >> 8;
      this.writeU24(request, 2, address + pos);
      request[5] = readLen;

      const response = await this.sendRead(seq, request);
      if (response.status === this.NRF_ERROR_BUSY) throw new Error('设备正在刷新屏幕，请稍后再试');
      if (response.status !== 0) throw new Error(`Flash读取失败: ${response.status}`);
      if (!response.crcOk) throw new Error('Flash读取响应CRC错误');
      if (response.address !== address + pos || response.payload.length !== readLen) {
        throw new Error('Flash读取响应地址或长度不匹配');
      }
      out.set(response.payload, pos);
      pos += readLen;
    }

    return out;
  },

  async sendOp(cmd, seq, payload, timeout = 3000) {
    const key = this.opKey(cmd, seq);
    return new Promise(async (resolve, reject) => {
      let response = null;
      let writeDone = false;
      let settled = false;
      const finish = () => {
        if (!settled && writeDone && response) {
          settled = true;
          resolve(response);
        }
      };
      const timer = setTimeout(() => {
        this.opResolvers.delete(key);
        settled = true;
        reject(new Error(`资源操作0x${cmd.toString(16)}超时`));
      }, timeout);

      this.opResolvers.set(key, (op) => {
        response = op;
        if (writeDone) clearTimeout(timer);
        finish();
      });

      const ok = await write(cmd, payload, true);
      writeDone = true;
      if (!ok) {
        clearTimeout(timer);
        this.opResolvers.delete(key);
        settled = true;
        reject(new Error(`资源操作0x${cmd.toString(16)}发送失败`));
        return;
      }
      if (response) clearTimeout(timer);
      finish();
    });
  },

  async sendRead(seq, payload, timeout = 3000) {
    return new Promise(async (resolve, reject) => {
      let response = null;
      let writeDone = false;
      let settled = false;
      const finish = () => {
        if (!settled && writeDone && response) {
          settled = true;
          resolve(response);
        }
      };
      const timer = setTimeout(() => {
        this.readResolvers.delete(seq);
        settled = true;
        reject(new Error('Flash读取超时'));
      }, timeout);

      this.readResolvers.set(seq, (readResponse) => {
        response = readResponse;
        if (writeDone) clearTimeout(timer);
        finish();
      });

      const ok = await write(this.CMD_RES_READ, payload, true);
      writeDone = true;
      if (!ok) {
        clearTimeout(timer);
        this.readResolvers.delete(seq);
        settled = true;
        reject(new Error('Flash读取命令发送失败'));
        return;
      }
      if (response) clearTimeout(timer);
      finish();
    });
  },

  async verifySamples(entries, bundle) {
    for (const entry of entries) {
      const ranges = this.sampleRanges(entry);
      for (const range of ranges) {
        const actual = await this.readRaw(range.address, range.length);
        const expected = bundle.data.slice(range.address - bundle.dataStart,
          range.address - bundle.dataStart + range.length);
        if (!this.bytesEqual(actual, expected)) {
          return `${entry.name || `${entry.type}/${entry.id}`} 抽样数据不一致`;
        }
      }
    }
    return null;
  },

  async verifyResourceCrcs(entries, bundle) {
    for (let i = 0; i < entries.length; i++) {
      const entry = entries[i];
      const actual = await this.readRaw(entry.offset, entry.length);
      const actualCrc = this.crc32(actual);
      const expected = bundle.data.slice(entry.offset - bundle.dataStart,
        entry.offset - bundle.dataStart + entry.length);
      if (actualCrc !== entry.crc32 || !this.bytesEqual(actual, expected)) {
        return `${entry.name || `${entry.type}/${entry.id}`} 完整校验不一致`;
      }
      this.setUiStatus(`写后完整校验 ${i + 1}/${entries.length}`);
    }
    return null;
  },

  sampleRanges(entry) {
    if (entry.length === 0) return [];
    const firstLen = Math.min(this.SAMPLE_BYTES, entry.length);
    const ranges = [{ address: entry.offset, length: firstLen }];
    if (entry.length > this.SAMPLE_BYTES) {
      const tailLen = Math.min(this.SAMPLE_BYTES, entry.length);
      const tailAddress = entry.offset + entry.length - tailLen;
      if (tailAddress !== entry.offset) ranges.push({ address: tailAddress, length: tailLen });
    }
    return ranges;
  },

  compareEntries(deviceEntries, bundleEntries) {
    if (deviceEntries.length !== bundleEntries.length) {
      return `资源数量不一致: 设备${deviceEntries.length}, 网页${bundleEntries.length}`;
    }

    const key = (entry) => `${entry.type}:${entry.id}`;
    const deviceMap = new Map(deviceEntries.map(entry => [key(entry), entry]));
    for (const expected of bundleEntries) {
      const actual = deviceMap.get(key(expected));
      if (!actual) return `缺少资源 ${expected.name || key(expected)}`;
      if (actual.offset !== expected.offset || actual.length !== expected.length || actual.crc32 !== expected.crc32) {
        return `资源 ${expected.name || key(expected)} 信息不一致`;
      }
    }
    return null;
  },

  parseStatus(data) {
    return {
      flashReady: data[1] === 1,
      manifestValid: data[2] === 1,
      resourceCount: this.readU16(data, 3),
      jedec: [data[5], data[6], data[7]],
      manifestCrc32: this.readU32(data, 8),
      statusCode: this.readU32(data, 12),
      flashTotalBytes: data.length >= 20 ? this.readU16(data, 16) * 1024 : null,
      resourceFreeBytes: data.length >= 20 ? this.readU16(data, 18) * 1024 : null,
      uiBundleError: data.length >= 24 ? this.readU32(data, 20) : 0,
    };
  },

  describeUiBundleError(code) {
    if (!code) return '';
    if (code === this.NRF_ERROR_NOT_SUPPORTED) {
      return 'UI 资源 schema 版本高于固件，请升级固件或下发兼容资源包';
    }
    if (code === this.NRF_ERROR_INVALID_DATA) {
      return 'UI 资源与屏幕规格不匹配（分辨率/色深），请重新打包资源';
    }
    return `UI bundle 校验失败 (0x${code.toString(16)})`;
  },

  parseManifest(bytes) {
    if (!bytes || bytes.length < 12) return { valid: false, reason: 'manifest长度不足' };
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const magic = view.getUint32(0, true);
    const version = view.getUint16(4, true);
    const count = view.getUint16(6, true);
    const entryCapacity = Math.floor((bytes.length - 12) / 16);
    const storedCrc = view.getUint32(bytes.length - 4, true);
    const crcBytes = new Uint8Array(bytes);
    crcBytes[bytes.length - 4] = 0;
    crcBytes[bytes.length - 3] = 0;
    crcBytes[bytes.length - 2] = 0;
    crcBytes[bytes.length - 1] = 0;
    const computedCrc = this.crc32(crcBytes);

    if (magic !== this.getBundle().magic) return { valid: false, reason: 'manifest magic不匹配' };
    if (version !== this.getBundle().version) return { valid: false, reason: 'manifest版本不匹配' };
    if (count > entryCapacity) return { valid: false, reason: 'manifest资源数量超限' };
    if (storedCrc !== computedCrc) return { valid: false, reason: 'manifest CRC错误' };

    const entries = [];
    for (let i = 0; i < count; i++) {
      const off = 8 + i * 16;
      entries.push({
        type: view.getUint16(off, true),
        id: view.getUint16(off + 2, true),
        offset: view.getUint32(off + 4, true),
        length: view.getUint32(off + 8, true),
        crc32: view.getUint32(off + 12, true),
      });
    }
    return { valid: true, crc32: storedCrc, entries };
  },

  getBundle() {
    if (this.decodedBundle) return this.decodedBundle;
    const raw = window.EPD_RESOURCE_BUNDLE;
    if (!raw) throw new Error('网页内置资源包不存在');
    this.decodedBundle = {
      ...raw,
      data: this.base64ToBytes(raw.dataBase64),
      manifest: this.base64ToBytes(raw.manifestBase64),
    };
    return this.decodedBundle;
  },

  base64ToBytes(text) {
    const binary = atob(text);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    return bytes;
  },

  crc16(data) {
    let crc = 0xFFFF;
    for (let i = 0; i < data.length; i++) {
      crc ^= data[i];
      for (let j = 0; j < 8; j++) {
        crc = (crc & 1) ? (crc >>> 1) ^ 0x8408 : crc >>> 1;
      }
    }
    return crc & 0xFFFF;
  },

  crc32(data) {
    if (!this.crcTable) {
      this.crcTable = new Uint32Array(256);
      for (let i = 0; i < 256; i++) {
        let c = i;
        for (let j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
        this.crcTable[i] = c >>> 0;
      }
    }
    let crc = 0xFFFFFFFF;
    for (let i = 0; i < data.length; i++) {
      crc = this.crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >>> 8);
    }
    return (~crc) >>> 0;
  },

  readU16(data, offset) {
    return data[offset] | (data[offset + 1] << 8);
  },

  readU32(data, offset) {
    return ((data[offset]) | (data[offset + 1] << 8) |
      (data[offset + 2] << 16) | (data[offset + 3] << 24)) >>> 0;
  },

  writeU24(data, offset, value) {
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
    data[offset + 2] = (value >> 16) & 0xFF;
  },

  writeU32(data, offset, value) {
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
    data[offset + 2] = (value >> 16) & 0xFF;
    data[offset + 3] = (value >> 24) & 0xFF;
  },

  nextSeq() {
    this.seq = (this.seq + 1) & 0xFFFF;
    if (this.seq === 0) this.seq = 1;
    return this.seq;
  },

  alignSector(length) {
    return (length + 4095) & ~4095;
  },

  opKey(cmd, seq) {
    return `${cmd}:${seq}`;
  },

  getMtu() {
    const el = document.getElementById('mtusize');
    const mtu = el ? parseInt(el.value, 10) : 20;
    return Number.isFinite(mtu) && mtu >= 20 ? mtu : 20;
  },

  isLowBattery() {
    return typeof window.latestBatteryVoltageMv === 'number' &&
      window.latestBatteryVoltageMv <= this.LOW_BATTERY_THRESHOLD_MV;
  },

  formatBytes(value) {
    if (typeof value !== 'number' || !Number.isFinite(value)) return '--';
    if (value >= 1024 * 1024) return `${(value / (1024 * 1024)).toFixed(2)} MB`;
    if (value >= 1024) return `${(value / 1024).toFixed(2)} KB`;
    return `${value} B`;
  },

  bytesEqual(a, b) {
    if (!a || !b || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
      if (a[i] !== b[i]) return false;
    }
    return true;
  },

  setUiStatus(text) {
    const el = document.getElementById('resourceCheckResult');
    if (el) el.innerText = text;
  },

  setProgress(text) {
    const el = document.getElementById('resourceProgress');
    if (el) el.innerText = text;
  },

  updateUi() {
    const bundle = window.EPD_RESOURCE_BUNDLE;
    const flashStatus = document.getElementById('resourceFlashStatus');
    const jedec = document.getElementById('resourceJedec');
    const totalSpace = document.getElementById('resourceTotalSpace');
    const freeSpace = document.getElementById('resourceFreeSpace');
    const deviceCrc = document.getElementById('resourceDeviceCrc');
    const bundleCrc = document.getElementById('resourceBundleCrc');
    const updateButton = document.getElementById('updateResourceButton');
    const checkButton = document.getElementById('checkResourceButton');
    const clearButton = document.getElementById('clearResourceButton');

    if (flashStatus) {
      if (!this.isConnected()) flashStatus.innerText = '未连接';
      else if (!this.latestStatus) flashStatus.innerText = '未查询';
      else if (this.latestStatus.statusCode === this.NRF_ERROR_BUSY) flashStatus.innerText = '刷新中';
      else flashStatus.innerText = this.latestStatus.flashReady ? '在线' : '离线';
    }
    if (jedec) {
      jedec.innerText = this.latestStatus && this.latestStatus.flashReady
        ? this.latestStatus.jedec.map(v => v.toString(16).padStart(2, '0')).join(' ')
        : '--';
    }
    if (totalSpace) {
      totalSpace.innerText = this.latestStatus && this.latestStatus.flashReady
        ? this.formatBytes(this.latestStatus.flashTotalBytes)
        : '--';
    }
    if (freeSpace) {
      if (this.latestStatus && this.latestStatus.flashReady &&
          typeof this.latestStatus.resourceFreeBytes === 'number') {
        freeSpace.innerText = this.formatBytes(this.latestStatus.resourceFreeBytes);
      } else {
        freeSpace.innerText = '--';
      }
    }
    if (deviceCrc) {
      deviceCrc.innerText = this.latestStatus && this.latestStatus.manifestCrc32
        ? `0x${this.latestStatus.manifestCrc32.toString(16).padStart(8, '0')}`
        : '--';
    }
    if (bundleCrc && bundle) {
      bundleCrc.innerText = `0x${bundle.manifestCrc32.toString(16).padStart(8, '0')}`;
    }
    const busy = this.isBusy;
    if (updateButton) {
      updateButton.disabled = !this.isConnected() || busy || this.isLowBattery() ||
        (this.latestStatus && !this.latestStatus.flashReady) ||
        !this.latestCheck || !this.latestCheck.needsUpdate;
    }
    if (checkButton) checkButton.disabled = !this.isConnected() || busy;
    if (clearButton) {
      clearButton.disabled = !this.isConnected() || busy || this.isLowBattery() ||
        (this.latestStatus && !this.latestStatus.flashReady);
    }
    if (this.latestCheck) this.setProgress(this.latestCheck.reason);
  },

  updateButtonLocks() {
    if (typeof updateButtonStatus === 'function') updateButtonStatus(this.isBusy);
  },
};

async function checkExternalResources() {
  try {
    const result = await ResourceManager.checkResources();
    addLog(result.reason);
  } catch (e) {
    console.error(e);
    addLog(`资源检查失败: ${e.message}`);
  }
}

async function updateExternalResources() {
  if (!confirm('确认更新外置Flash资源？更新过程中请保持设备供电和蓝牙连接。')) return;
  try {
    await ResourceManager.updateResources();
  } catch (e) {
    console.error(e);
    addLog(`资源更新失败: ${e.message}`);
    if (typeof showToast === 'function') showToast(`资源更新失败: ${e.message}`, 4200);
  }
}

async function clearExternalFlash() {
  if (!confirm('确认强制清空整颗外置Flash？清空后屏幕资源会失效，需要重新更新资源。')) return;
  try {
    await ResourceManager.clearFlash();
  } catch (e) {
    console.error(e);
    addLog(`外置Flash清空失败: ${e.message}`);
    if (typeof showToast === 'function') showToast(`外置Flash清空失败: ${e.message}`, 4200);
  }
}

if (typeof window !== 'undefined') {
  window.ResourceManager = ResourceManager;
  window.checkExternalResources = checkExternalResources;
  window.updateExternalResources = updateExternalResources;
  window.clearExternalFlash = clearExternalFlash;
}
