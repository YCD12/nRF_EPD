/**
 * External FM25Q04B resource manager.
 *
 * Handles resource status checks, manifest/sample verification, and reliable
 * resource package updates over the existing EPD BLE characteristic.
 */
const ResourceManager = {
  RESOURCE_NAME_MAP_CN: {
    'u8g2_font_wqy9_t_lunar': '文泉驿 9px 农历字体',
    'u8g2_font_wqy12_t_lunar': '文泉驿 12px 农历字体',
    'u8g2_font_helvB14_tn': 'Helvetica 14px 粗体数字',
    'u8g2_font_helvB18_tn': 'Helvetica 18px 粗体数字',
    'lunar_month_names': '农历月份名称表',
    'lunar_leap_names': '农历闰月名称表',
    'lunar_date_names': '农历日期名称表',
    'lunar_day_names': '星期/日名称表',
    'lunar_zodiac_names': '生肖十二兽表',
    'lunar_stem_names': '十天干表',
    'lunar_branch_names': '十二地支表',
    'lunar_jieqi_names': '二十四节气表',
    'lunar_month_days': '农历月份天数',
    'lunar_solar_1_1': '岁首朔日对照表',
    'lunar_jieqi_year_bits': '节气时间偏移表',
    'lunar_jieqi_base_days': '节气首交时刻表',
    'solar_festivals': '公历节日数据',
    'lunar_festivals': '农历节日数据',
    'holiday_adjustments': '节假日放假/调休安排',
    'lunar_db': '农历大数据库',
    'ui_bundle': '界面布局模板包'
  },

  getFriendlyName(name) {
    return this.RESOURCE_NAME_MAP_CN[name] || name;
  },

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
  NRF_ERROR_INVALID_DATA: 11,
  NRF_ERROR_NOT_SUPPORTED: 6,

  seq: 1,
  statusResolver: null,
  opResolvers: new Map(),
  readResolvers: new Map(),
  decodedBundle: null,
  latestStatus: null,
  latestCheck: null,
  deviceEntries: null,
  isUpdating: false,
  isChecking: false,
  isClearing: false,
  lastWriteTimes: {},
  BUSY_RETRY_INTERVAL_MS: 1000,
  BUSY_RETRY_MAX: 40,
  busyOperation: null,
  crcTable: null,
  get isBusy() {
    return !!this.busyOperation || this.isUpdating || this.isChecking || this.isClearing;
  },

  init() {
    this.lastWriteTimes = {};
    this.setUiStatus('等待检查');
    this.updateUi();
  },

  onDisconnect() {
    this.latestStatus = null;
    this.latestCheck = null;
    this.deviceEntries = null;
    this.isUpdating = false;
    this.isChecking = false;
    this.isClearing = false;
    this.busyOperation = null;
    this.statusResolver = null;
    this.opResolvers.clear();
    this.readResolvers.clear();
    this.lastWriteTimes = {};
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
      this.deviceEntries = null;
      this.setUiStatus('读取状态');
      const status = await this.queryStatusWithRetry();
      this.latestStatus = status;
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

      /* PRD §US-1.2: Manifest CRC fast-path.
       * If device manifestCrc32 matches the bundle's AND manifestValid is true,
       * skip readRaw(manifest) + verifySamples() entirely. */
      if (status.manifestValid &&
          status.manifestCrc32 !== 0 &&
          status.manifestCrc32 === bundle.manifestCrc32) {
        this.deviceEntries = bundle.entries;
        this.latestCheck = { needsUpdate: false, reason: '资源已是最新' };
        this.setUiStatus('已是最新');
        this.updateUi();
        if (typeof addLog === 'function') addLog('资源CRC一致，跳过详细校验（闪电校验）');
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
      this.deviceEntries = deviceManifest.entries;

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

  async updateResources(targetEntry = null, customData = null) {
    if (!this.isConnected()) throw new Error('BLE未连接');
    if (this.isLowBattery()) throw new Error('电量过低，已阻止外置Flash擦写');

    const bundle = this.getBundle();
    const SECTOR_SIZE = 4096;
    this.beginOperation('资源更新');
    this.isUpdating = true;
    this.updateButtonLocks();
    this.updateUi();

    try {
      /* US-2.1 AC2/AC3: Modular differential update / Single module update.
       * Merges targetEntry into the device manifest for single-module updates. */
      let manifestBytesToWrite = bundle.manifest;
      let changedEntries = bundle.entries;  // default: full update
      if (targetEntry) {
        changedEntries = [targetEntry];
        this.setUiStatus('读取当前manifest');
        let deviceManifestBytes;
        try {
          deviceManifestBytes = await this.readRaw(bundle.manifestAddress, bundle.manifest.length);
        } catch (e) {
          throw new Error('读取设备资源列表失败，请先执行“更新资源”（全量/差分更新）以初始化资源区。');
        }
        let deviceManifest = this.parseManifest(deviceManifestBytes);
        if (!deviceManifest.valid) {
          throw new Error('设备无有效资源列表（Manifest校验不通过），无法进行单模块更新，请先执行“更新资源”（全量/差分更新）。');
        }

        const entries = [...deviceManifest.entries];
        const existingIdx = entries.findIndex(e => e.type === targetEntry.type && e.id === targetEntry.id);
        if (existingIdx !== -1) {
          entries[existingIdx] = targetEntry;
        } else {
          if (entries.length >= 24) {
            throw new Error('设备资源列表容量已满，无法添加新模块');
          }
          entries.push(targetEntry);
        }

        manifestBytesToWrite = this.serializeManifest(entries, bundle);
      } else {
        this.setUiStatus('读取设备manifest');
        try {
          const status = this.latestStatus || await this.queryStatusWithRetry();
          if (status.manifestValid) {
            const deviceManifestBytes = await this.readRaw(bundle.manifestAddress, bundle.manifest.length);
            const deviceManifest = this.parseManifest(deviceManifestBytes);
            if (deviceManifest.valid) {
              const deviceMap = new Map(deviceManifest.entries.map(e => [`${e.type}:${e.id}`, e]));
              changedEntries = bundle.entries.filter(entry => {
                const dev = deviceMap.get(`${entry.type}:${entry.id}`);
                return !dev || dev.crc32 !== entry.crc32;
              });
            }
          }
        } catch (e) {
          changedEntries = bundle.entries;
        }
      }

      /* Throttling: prevent rewriting the same module within 3 minutes to protect Flash life */
      const now = Date.now();
      const THROTTLE_LIMIT_MS = 3 * 60 * 1000;
      for (const entry of changedEntries) {
        const key = `${entry.type}:${entry.id}`;
        const lastWrite = this.lastWriteTimes[key];
        if (lastWrite && (now - lastWrite) < THROTTLE_LIMIT_MS) {
          const remainingSec = Math.ceil((THROTTLE_LIMIT_MS - (now - lastWrite)) / 1000);
          throw new Error(`为保护Flash寿命，3分钟内禁止重复更新模块 "${this.getFriendlyName(entry.name || key)}"（请等待 ${remainingSec} 秒）`);
        }
      }

      const isFullUpdate = changedEntries.length === bundle.entries.length;
      if (typeof addLog === 'function') {
        if (changedEntries.length === 0) {
          addLog('所有资源数据无变化，仅更新manifest');
        } else if (isFullUpdate) {
          addLog(`完整更新全部 ${bundle.entries.length} 个资源模块`);
        } else {
          addLog(`差分更新 ${changedEntries.length}/${bundle.entries.length} 个模块: ${changedEntries.map(e => this.getFriendlyName(e.name || `${e.type}/${e.id}`)).join(', ')}`);
        }
      }

      /* Erase only the sectors occupied by changed entries. */
      if (changedEntries.length > 0) {
        this.setUiStatus('擦除变更扇区');
        const sectorSet = new Set();
        for (const entry of changedEntries) {
          const end = entry.offset + entry.length;
          for (let s = entry.offset & ~(SECTOR_SIZE - 1); s < end; s += SECTOR_SIZE) {
            sectorSet.add(s);
          }
        }
        const sorted = [...sectorSet].sort((a, b) => a - b);
        let rangeStart = sorted[0], rangeEnd = sorted[0] + SECTOR_SIZE;
        for (let i = 1; i <= sorted.length; i++) {
          if (i < sorted.length && sorted[i] === rangeEnd) {
            rangeEnd += SECTOR_SIZE;
          } else {
            await this.eraseRange(rangeStart, rangeEnd - rangeStart);
            if (i < sorted.length) { rangeStart = sorted[i]; rangeEnd = sorted[i] + SECTOR_SIZE; }
          }
        }
      }

      /* Write only changed entry data. */
      if (changedEntries.length > 0) {
        this.setUiStatus('写入变更资源');
        for (const entry of changedEntries) {
          const slice = (targetEntry && customData) ? customData : bundle.data.slice(
            entry.offset - bundle.dataStart,
            entry.offset - bundle.dataStart + entry.length
          );
          await this.writeSegment(entry.offset, slice, this.getFriendlyName(entry.name || `${entry.type}/${entry.id}`));
        }
      }

      /* Always refresh the manifest sector and write staging manifest. */
      this.setUiStatus('擦除manifest扇区');
      await this.eraseRange(bundle.manifestAddress, SECTOR_SIZE);
      if (bundle.stagingManifestAddress !== bundle.manifestAddress) {
        await this.eraseRange(bundle.stagingManifestAddress, SECTOR_SIZE);
      }
      this.setUiStatus('写入staging manifest');
      await this.writeSegment(bundle.stagingManifestAddress, manifestBytesToWrite, 'manifest');

      /* Post-write verification of the manifest itself */
      this.setUiStatus('校验staging manifest');
      const readManifest = await this.readRaw(bundle.stagingManifestAddress, manifestBytesToWrite.length);
      if (!this.bytesEqual(readManifest, manifestBytesToWrite)) {
        throw new Error('staging manifest 写入校验不一致');
      }

      /* Post-write verification (only targetEntry / changedEntries for single-module update, otherwise all). */
      this.setUiStatus('写后完整校验');
      const verifyEntries = targetEntry ? changedEntries : bundle.entries;
      let resourceMismatch = null;
      if (targetEntry && customData) {
        const actual = await this.readRaw(targetEntry.offset, targetEntry.length);
        const actualCrc = this.crc32(actual);
        if (actualCrc !== targetEntry.crc32 || !this.bytesEqual(actual, customData)) {
          resourceMismatch = `${this.getFriendlyName(targetEntry.name || `${targetEntry.type}/${targetEntry.id}`)} 完整校验不一致`;
        }
      } else {
        resourceMismatch = await this.verifyResourceCrcs(verifyEntries, bundle);
      }
      if (resourceMismatch) throw new Error(resourceMismatch);

      this.setUiStatus('提交manifest');
      await this.commitManifest();

      this.setUiStatus('提交后复核');
      const check = await this.checkResources({ skipLock: true });
      if (targetEntry) {
        const devEntry = this.deviceEntries && this.deviceEntries.find(e => e.type === targetEntry.type && e.id === targetEntry.id);
        if (!devEntry || devEntry.crc32 !== targetEntry.crc32) {
          throw new Error('单模块更新后校验失败，物理CRC不匹配');
        }
      } else {
        if (check.needsUpdate) throw new Error(check.reason);
      }

      // Record update timestamps for successfully written entries
      const nowSuccess = Date.now();
      for (const entry of changedEntries) {
        const key = `${entry.type}:${entry.id}`;
        this.lastWriteTimes[key] = nowSuccess;
      }

      // Record update duration metric
      try {
        if (typeof recordConnectionMetrics === 'function') {
          recordConnectionMetrics({
            handshakeMs: null,
            writeErrors: window.writeErrorCount || 0,
            updateMs: nowSuccess - now
          });
        }
      } catch (e) {
        console.error('Failed to record update metrics:', e);
      }

      if (typeof addLog === 'function') addLog('外置Flash资源更新完成');
      if (typeof showToast === 'function') showToast('外置Flash资源已更新');

      // Now that external resource transfer and verification is completely finished,
      // and the device has committed the manifest, we sync time!
      if (typeof syncTimeOnConnect === 'function') {
        try {
          if (typeof addLog === 'function') addLog('资源更新完成，开始同步时间...');
          await syncTimeOnConnect();
        } catch (syncErr) {
          console.error('Time sync failed after resource update:', syncErr);
        }
      }

      return true;
    } finally {
      this.isUpdating = false;
      this.endOperation();
    }
  },

  async determineSafeOffset(targetType, targetId, requiredLength, deviceEntries, bundle) {
    const FLASH_LIMIT = 0x7D000;
    
    if (!deviceEntries || deviceEntries.length === 0) {
      const defaultEntry = bundle.entries.find(e => e.type === targetType && e.id === targetId);
      const offset = defaultEntry ? defaultEntry.offset : 0x01000;
      if (offset + requiredLength > FLASH_LIMIT) {
        throw new Error(`文件大小超出限制！最大允许大小: ${this.formatBytes(FLASH_LIMIT - offset)}，当前大小: ${this.formatBytes(requiredLength)}`);
      }
      return offset;
    }
    
    const devEntry = deviceEntries.find(e => e.type === targetType && e.id === targetId);
    if (!devEntry) {
      let maxEnd = 0x01000;
      for (const e of deviceEntries) {
        const end = e.offset + e.length;
        if (end > maxEnd) maxEnd = end;
      }
      const offset = (maxEnd + 3) & ~3;
      if (offset + requiredLength > FLASH_LIMIT) {
        throw new Error(`文件大小超出限制！Flash 剩余空间不足（最大可用: ${this.formatBytes(FLASH_LIMIT - offset)}，当前大小: ${this.formatBytes(requiredLength)}）。您可以尝试先点击页面上方的“更新资源”进行全量整理，消除 Relocation 产生的空间碎片。`);
      }
      return offset;
    }
    
    const sorted = [...deviceEntries].sort((a, b) => a.offset - b.offset);
    const idx = sorted.findIndex(e => e.type === targetType && e.id === targetId);
    let allowedLen = 0;
    if (idx === sorted.length - 1) {
      allowedLen = FLASH_LIMIT - sorted[idx].offset;
    } else {
      allowedLen = sorted[idx + 1].offset - sorted[idx].offset;
    }
    
    if (requiredLength <= allowedLen) {
      return devEntry.offset;
    } else {
      let maxEnd = 0x01000;
      for (const e of deviceEntries) {
        if (e.type === targetType && e.id === targetId) continue;
        const end = e.offset + e.length;
        if (end > maxEnd) maxEnd = end;
      }
      const offset = (maxEnd + 3) & ~3;
      if (offset + requiredLength > FLASH_LIMIT) {
        throw new Error(`文件大小超出限制！Flash 剩余空间不足以容纳重定位后的该模块（最大可用: ${this.formatBytes(FLASH_LIMIT - offset)}，当前大小: ${this.formatBytes(requiredLength)}）。您可以尝试先点击页面上方的“更新资源”进行全量整理，消除 Relocation 产生的空间碎片。`);
      }
      return offset;
    }
  },

  async deleteResource(type, id) {
    if (!this.isConnected()) throw new Error('BLE未连接');
    if (this.isLowBattery()) throw new Error('电量过低，已阻止外置Flash擦写');

    const bundle = this.getBundle();
    const SECTOR_SIZE = 4096;
    this.beginOperation('删除资源');
    this.isUpdating = true;
    this.updateButtonLocks();
    this.updateUi();

    try {
      this.setUiStatus('读取当前manifest');
      const deviceManifestBytes = await this.readRaw(bundle.manifestAddress, bundle.manifest.length);
      const deviceManifest = this.parseManifest(deviceManifestBytes);
      if (!deviceManifest.valid) {
        throw new Error('设备无有效资源列表，无法删除模块');
      }

      const entries = deviceManifest.entries.filter(e => !(e.type === type && e.id === id));
      if (entries.length === deviceManifest.entries.length) {
        throw new Error('设备上未找到该模块，无需删除');
      }

      const newManifestBytes = this.serializeManifest(entries, bundle);

      // Erase manifest sector and write new manifest
      this.setUiStatus('擦除manifest扇区');
      await this.eraseRange(bundle.manifestAddress, SECTOR_SIZE);
      if (bundle.stagingManifestAddress !== bundle.manifestAddress) {
        await this.eraseRange(bundle.stagingManifestAddress, SECTOR_SIZE);
      }
      this.setUiStatus('写入新manifest');
      await this.writeSegment(bundle.stagingManifestAddress, newManifestBytes, 'manifest');

      this.setUiStatus('提交manifest');
      await this.commitManifest();

      this.setUiStatus('提交后复核');
      await this.checkResources({ skipLock: true });

      if (typeof addLog === 'function') {
        addLog(`已成功删除资源模块: type=${type}, id=${id}`);
      }
    } finally {
      this.isUpdating = false;
      this.endOperation();
      this.updateButtonLocks();
      this.updateUi();
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

  /**
   * PRD §US-1.1: queryStatus with automatic busy-retry.
   * If the device returns NRF_ERROR_BUSY (screen refreshing), retries at 1s
   * intervals up to BUSY_RETRY_MAX times with a friendly UI message.
   */
  async queryStatusWithRetry() {
    for (let attempt = 0; attempt <= this.BUSY_RETRY_MAX; attempt++) {
      const status = await this.queryStatus();
      if (status.statusCode !== this.NRF_ERROR_BUSY) {
        return status;
      }
      if (attempt < this.BUSY_RETRY_MAX) {
        this.setUiStatus(`设备正在刷新画面，等待中 (${attempt + 1}/${this.BUSY_RETRY_MAX})...`);
        if (typeof addLog === 'function' && attempt === 0) {
          addLog('设备正在刷新屏幕，自动等待重试中...');
        }
        await new Promise(r => setTimeout(r, this.BUSY_RETRY_INTERVAL_MS));
      }
    }
    throw new Error('设备持续刷新中，等待超时，请稍后重试');
  },

  async eraseRange(address, length, timeout = 12000) {
    const payload = new Uint8Array(7);
    this.writeU24(payload, 0, address);
    this.writeU32(payload, 3, length);
    const op = await this.sendOpWithBusyRetry(this.CMD_RES_ERASE, 0, payload, timeout);
    if (op.status !== 0) throw new Error(`资源区擦除失败: ${op.status}`);
  },

  async commitManifest() {
    const op = await this.sendOpWithBusyRetry(this.CMD_RES_COMMIT, 0, null, 12000);
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
      let busyCount = 0;
      for (let attempt = 0; attempt < this.MAX_RETRIES; attempt++) {
        try {
          const op = await this.sendOp(this.CMD_RES_WRITE, seq, packet, 3000);
          if (op.status === 0) {
            lastError = null;
            break;
          }
          if (op.status === this.NRF_ERROR_BUSY) {
            /* PRD §US-1.1: Auto-retry on busy during write, bounded by BUSY_RETRY_MAX. */
            if (++busyCount > this.BUSY_RETRY_MAX) {
              lastError = new Error('设备持续刷新中，写入超时');
              break;
            }
            this.setUiStatus(`设备正在刷新画面，等待中 (${busyCount}/${this.BUSY_RETRY_MAX})...`);
            await new Promise(r => setTimeout(r, this.BUSY_RETRY_INTERVAL_MS));
            attempt--;  // Busy wait does not consume a normal retry slot.
            continue;
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
    let busyRetries = 0;

    while (pos < length) {
      const readLen = Math.min(chunkSize, length - pos);
      const seq = this.nextSeq();
      const request = new Uint8Array(6);
      request[0] = seq & 0xFF;
      request[1] = seq >> 8;
      this.writeU24(request, 2, address + pos);
      request[5] = readLen;

      const response = await this.sendRead(seq, request);
      if (response.status === this.NRF_ERROR_BUSY) {
        /* PRD §US-1.1: Retry read on busy, with a bounded retry count. */
        if (++busyRetries > this.BUSY_RETRY_MAX) {
          throw new Error('Flash读取持续繁忙，等待超时，请稍后重试');
        }
        await new Promise(r => setTimeout(r, this.BUSY_RETRY_INTERVAL_MS));
        continue;
      }
      busyRetries = 0;
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

  /**
   * PRD §US-1.1: sendOp wrapper with automatic busy-retry.
   * Retries the operation when the device returns NRF_ERROR_BUSY.
   */
  async sendOpWithBusyRetry(cmd, seq, payload, timeout = 3000) {
    for (let attempt = 0; attempt <= this.BUSY_RETRY_MAX; attempt++) {
      const op = await this.sendOp(cmd, seq, payload, timeout);
      if (op.status !== this.NRF_ERROR_BUSY) {
        return op;
      }
      if (attempt < this.BUSY_RETRY_MAX) {
        this.setUiStatus(`设备正在刷新画面，等待中 (${attempt + 1}/${this.BUSY_RETRY_MAX})...`);
        await new Promise(r => setTimeout(r, this.BUSY_RETRY_INTERVAL_MS));
      }
    }
    throw new Error('设备持续刷新中，操作超时');
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
          return `${this.getFriendlyName(entry.name || `${entry.type}/${entry.id}`)} 抽样数据不一致`;
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
        return `${this.getFriendlyName(entry.name || `${entry.type}/${entry.id}`)} 完整校验不一致`;
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

  serializeManifest(entries, bundle) {
    const bytes = new Uint8Array(bundle.manifest.length); // 396 bytes
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    
    // Sort entries by offset (ascending) to match the bundle's original layout order
    const sorted = [...entries].sort((a, b) => a.offset - b.offset);
    
    view.setUint32(0, bundle.magic, true);
    view.setUint16(4, bundle.version, true);
    view.setUint16(6, sorted.length, true);
    
    for (let i = 0; i < sorted.length; i++) {
      const off = 8 + i * 16;
      const entry = sorted[i];
      view.setUint16(off, entry.type, true);
      view.setUint16(off + 2, entry.id, true);
      view.setUint32(off + 4, entry.offset, true);
      view.setUint32(off + 8, entry.length, true);
      view.setUint32(off + 12, entry.crc32, true);
    }
    
    // Compute CRC32
    bytes[bytes.length - 4] = 0;
    bytes[bytes.length - 3] = 0;
    bytes[bytes.length - 2] = 0;
    bytes[bytes.length - 1] = 0;
    const computedCrc = this.crc32(bytes);
    view.setUint32(bytes.length - 4, computedCrc, true);
    
    return bytes;
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

    // Toggle resource update notice banner
    const noticeEl = document.getElementById('resourceUpdateNotice');
    if (noticeEl) {
      noticeEl.style.display = (this.isConnected() && this.latestCheck && this.latestCheck.needsUpdate && !busy && this.latestStatus && this.latestStatus.flashReady) ? 'flex' : 'none';
    }

    // Render submodules list
    const modulesContainer = document.getElementById('resourceModulesContainer');
    const modulesList = document.getElementById('resourceModulesList');
    if (modulesContainer && modulesList) {
      if (!this.isConnected() || !bundle) {
        modulesContainer.style.display = 'none';
        modulesList.innerHTML = '';
      } else {
        modulesContainer.style.display = 'block';
        
        let html = '';
        const deviceMap = new Map();
        if (this.deviceEntries) {
          for (const e of this.deviceEntries) {
            deviceMap.set(`${e.type}:${e.id}`, e);
          }
        }
        
        for (const entry of bundle.entries) {
          const key = `${entry.type}:${entry.id}`;
          const devEntry = deviceMap.get(key);
          
          let devCrcText = '--';
          let statusText = '未知';
          let statusClass = 'status-unknown';
          
          if (this.deviceEntries) {
            if (devEntry) {
              devCrcText = `0x${devEntry.crc32.toString(16).toUpperCase().padStart(8, '0')}`;
              if (devEntry.crc32 === entry.crc32) {
                statusText = '已是最新';
                statusClass = 'status-match';
              } else {
                statusText = '待更新';
                statusClass = 'status-mismatch';
              }
            } else {
              statusText = '设备缺失';
              statusClass = 'status-mismatch';
            }
          } else if (this.latestStatus && this.latestStatus.manifestValid && this.latestStatus.manifestCrc32 === bundle.manifestCrc32) {
            // Flash is identical (Fast-path)
            devCrcText = `0x${entry.crc32.toString(16).toUpperCase().padStart(8, '0')}`;
            statusText = '已是最新';
            statusClass = 'status-match';
          }
          
          const webCrcText = `0x${entry.crc32.toString(16).toUpperCase().padStart(8, '0')}`;
          const sizeText = this.formatBytes(entry.length);
          
          const btnDisabled = !this.isConnected() || busy || this.isLowBattery() || (this.latestStatus && !this.latestStatus.flashReady);
          const btnClass = statusText === '已是最新' ? 'secondary btn-mini' : 'primary btn-mini';
          
          html += `
            <tr>
              <td><b>${this.getFriendlyName(entry.name || key)}</b></td>
              <td>${sizeText}</td>
              <td><code>${devCrcText}</code></td>
              <td><code>${webCrcText}</code></td>
              <td><span class="${statusClass}">${statusText}</span></td>
              <td style="white-space: nowrap;">
                <button class="${btnClass}" onclick="updateSingleResource(${entry.type}, ${entry.id})" ${btnDisabled ? 'disabled' : ''} style="margin-right: 4px;">更新</button>
                <button class="success btn-mini" onclick="uploadSingleResource(${entry.type}, ${entry.id})" ${btnDisabled ? 'disabled' : ''} style="margin-right: 4px;">上传</button>
                <button class="danger btn-mini" onclick="deleteSingleResource(${entry.type}, ${entry.id})" ${btnDisabled || !devEntry ? 'disabled' : ''}>删除</button>
              </td>
            </tr>
          `;
        }
        modulesList.innerHTML = html;
      }
    }
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

async function updateSingleResource(type, id) {
  let entry = null;
  try {
    const bundle = ResourceManager.getBundle();
    entry = bundle.entries.find(e => e.type === type && e.id === id);
  } catch (e) {
    console.error(e);
  }
  if (!entry) {
    addLog('找不到指定的资源模块');
    return;
  }
  if (!confirm(`确认独立更新模块 "${ResourceManager.getFriendlyName(entry.name || (type + ':' + id))}"？更新过程中请保持设备供电和蓝牙连接。`)) return;
  try {
    await ResourceManager.updateResources(entry);
  } catch (e) {
    console.error(e);
    addLog(`模块更新失败: ${e.message}`);
    if (typeof showToast === 'function') showToast(`模块更新失败: ${e.message}`, 4200);
  }
}

async function uploadSingleResource(type, id) {
  let entry = null;
  let bundle = null;
  try {
    bundle = ResourceManager.getBundle();
    entry = bundle.entries.find(e => e.type === type && e.id === id);
  } catch (e) {
    console.error(e);
  }
  const friendlyName = ResourceManager.getFriendlyName(entry ? entry.name : (type + ':' + id));

  // Create file input
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '*/*';
  input.onchange = async (event) => {
    const file = event.target.files[0];
    if (!file) return;

    const reader = new FileReader();
    reader.onload = async () => {
      try {
        const fileBytes = new Uint8Array(reader.result);
        const safeOffset = await ResourceManager.determineSafeOffset(
          type,
          id,
          fileBytes.length,
          ResourceManager.deviceEntries || [],
          bundle
        );

        const targetEntry = {
          type,
          id,
          offset: safeOffset,
          length: fileBytes.length,
          crc32: ResourceManager.crc32(fileBytes),
          name: entry ? entry.name : `${type}:${id}`
        };

        if (!confirm(`确认上传本地资源文件并独立更新模块 "${friendlyName}" 吗？\n文件大小: ${file.size} 字节\n写入地址: 0x${safeOffset.toString(16).toUpperCase()}`)) return;

        await ResourceManager.updateResources(targetEntry, fileBytes);
      } catch (e) {
        console.error(e);
        addLog(`上传更新模块失败: ${e.message}`);
        if (typeof showToast === 'function') showToast(`上传更新模块失败: ${e.message}`, 4200);
      }
    };
    reader.onerror = () => {
      addLog('读取本地文件失败');
    };
    reader.readAsArrayBuffer(file);
  };
  input.click();
}

async function deleteSingleResource(type, id) {
  let entry = null;
  try {
    const bundle = ResourceManager.getBundle();
    entry = bundle.entries.find(e => e.type === type && e.id === id);
  } catch (e) {
    console.error(e);
  }
  const friendlyName = ResourceManager.getFriendlyName(entry ? entry.name : (type + ':' + id));

  if (!confirm(`确认从设备中删除模块 "${friendlyName}" 吗？删除后该模块占用的区域将被停用。`)) return;

  try {
    await ResourceManager.deleteResource(type, id);
  } catch (e) {
    console.error(e);
    addLog(`删除模块失败: ${e.message}`);
    if (typeof showToast === 'function') showToast(`删除模块失败: ${e.message}`, 4200);
  }
}

if (typeof window !== 'undefined') {
  window.ResourceManager = ResourceManager;
  window.checkExternalResources = checkExternalResources;
  window.updateExternalResources = updateExternalResources;
  window.clearExternalFlash = clearExternalFlash;
  window.updateSingleResource = updateSingleResource;
  window.uploadSingleResource = uploadSingleResource;
  window.deleteSingleResource = deleteSingleResource;
}
