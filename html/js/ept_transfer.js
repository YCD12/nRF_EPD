/* EPT1 full-image PackBits transfer. The protocol transports a negotiated
 * number of complete rows per GATT write, keeping
 * MCU work bounded while reducing packet count for compressible images. */
const EptTransfer = {
  MIN_VALUE_LEN: 110,
  ROW_BYTES: 100,
  PLANE_ROWS: 480,
  DEFAULT_ROWS_PER_PACKET: 2,
  MAX_PLANE_RETRIES: 1,
  BUSY_RETRY_DELAY_MS: 1000,
  MAX_BUSY_WAIT_MS: 45000,
  IDLE_FENCE_TIMEOUT_MS: 45000,
  PERF_STORAGE_KEY: 'epd_ept_perf_v2',
  DEFAULT_EPT_BPS: 1200,
  DEFAULT_LEGACY_BPS: 6500,
  DEFAULT_EPT_PACKET_MS: 40,
  DEFAULT_LEGACY_PACKET_MS: 35,
  PREDICTED_FIXED_MS: 300,
  REQUIRED_TIME_GAIN: 0.20,
  PERF_EMA_ALPHA: 0.35,
  SLOW_PATH_PROBE_PACKETS: 32,
  // A raw/literal-heavy EPT packet is nearly ATT-sized but still requires
  // PackBits parsing, two CRC32 passes and synchronous SPI work in the MCU.
  // Evaluate each plane independently: a small red plane must not hide a
  // slow, low-yield black plane that is always sent first.
  RAW_LIKE_PACKET_PAYLOAD_BYTES: 160,
  RAW_LIKE_MAX_WIRE_RATIO: 0.25,
  ERROR_NAMES: {
    1: 'BAD_HEADER',
    2: 'BAD_ID',
    3: 'BAD_OFFSET',
    4: 'BAD_RLE',
    5: 'BAD_LENGTH',
    6: 'BAD_CRC',
    7: 'TIMEOUT',
    8: 'BUSY',
    9: 'MTU',
    10: 'LOW_BATTERY',
    11: 'SCHEDULER'
  },
  caps: null,
  waiters: {},
  lastError: null,
  lastStats: null,
  activeId: 0,
  nextTransactionId: 1,

  init() {
    this.caps = null;
    this.waiters = {};
    this.lastError = null;
    this.lastStats = null;
    this.activeId = 0;
  },

  u32le(value) {
    return [value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff];
  },

  u16le(value) {
    return [value & 0xff, (value >>> 8) & 0xff];
  },

  now() {
    return (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
  },

  loadPerfProfile() {
    const defaults = {
      eptBps: this.DEFAULT_EPT_BPS,
      legacyBps: this.DEFAULT_LEGACY_BPS,
      eptSamples: 0,
      legacySamples: 0,
      eptPacketMs: this.DEFAULT_EPT_PACKET_MS,
      legacyPacketMs: this.DEFAULT_LEGACY_PACKET_MS,
      eptPacketSamples: 0,
      legacyPacketSamples: 0
    };
    try {
      if (typeof localStorage === 'undefined') return defaults;
      const saved = JSON.parse(localStorage.getItem(this.PERF_STORAGE_KEY) || 'null');
      if (!saved || typeof saved !== 'object') return defaults;
      for (const key of ['eptBps', 'legacyBps']) {
        if (!Number.isFinite(saved[key]) || saved[key] < 200 || saved[key] > 50000) return defaults;
      }
      for (const key of ['eptPacketMs', 'legacyPacketMs']) {
        if (!Number.isFinite(saved[key]) || saved[key] < 1 || saved[key] > 1000) return defaults;
      }
      return {
        eptBps: saved.eptBps,
        legacyBps: saved.legacyBps,
        eptSamples: Math.max(0, Number(saved.eptSamples) || 0),
        legacySamples: Math.max(0, Number(saved.legacySamples) || 0),
        eptPacketMs: saved.eptPacketMs,
        legacyPacketMs: saved.legacyPacketMs,
        eptPacketSamples: Math.max(0, Number(saved.eptPacketSamples) || 0),
        legacyPacketSamples: Math.max(0, Number(saved.legacyPacketSamples) || 0)
      };
    } catch (_) {
      return defaults;
    }
  },

  recordThroughput(kind, wireBytes, elapsedMs) {
    if ((kind !== 'ept' && kind !== 'legacy') ||
        !Number.isFinite(wireBytes) || wireBytes <= 0 ||
        !Number.isFinite(elapsedMs) || elapsedMs < 300) return;
    const measuredBps = wireBytes * 1000 / elapsedMs;
    if (!Number.isFinite(measuredBps) || measuredBps < 200 || measuredBps > 50000) return;
    const profile = this.loadPerfProfile();
    const speedKey = kind === 'ept' ? 'eptBps' : 'legacyBps';
    const sampleKey = kind === 'ept' ? 'eptSamples' : 'legacySamples';
    profile[speedKey] = profile[sampleKey] === 0
      ? measuredBps
      : profile[speedKey] * (1 - this.PERF_EMA_ALPHA) + measuredBps * this.PERF_EMA_ALPHA;
    profile[sampleKey]++;
    try {
      if (typeof localStorage !== 'undefined') {
        localStorage.setItem(this.PERF_STORAGE_KEY, JSON.stringify(profile));
      }
    } catch (_) {}
  },

  recordPacketTiming(kind, packets, elapsedMs) {
    if ((kind !== 'ept' && kind !== 'legacy') ||
        !Number.isFinite(packets) || packets <= 0 ||
        !Number.isFinite(elapsedMs) || elapsedMs <= 0) return;
    const measuredMs = elapsedMs / packets;
    if (!Number.isFinite(measuredMs) || measuredMs < 1 || measuredMs > 1000) return;
    const profile = this.loadPerfProfile();
    const timingKey = kind === 'ept' ? 'eptPacketMs' : 'legacyPacketMs';
    const sampleKey = kind === 'ept' ? 'eptPacketSamples' : 'legacyPacketSamples';
    profile[timingKey] = profile[sampleKey] === 0
      ? measuredMs
      : profile[timingKey] * (1 - this.PERF_EMA_ALPHA) + measuredMs * this.PERF_EMA_ALPHA;
    profile[sampleKey]++;
    try {
      if (typeof localStorage !== 'undefined') {
        localStorage.setItem(this.PERF_STORAGE_KEY, JSON.stringify(profile));
      }
    } catch (_) {}
  },

  recordLegacyResult(wireBytes, elapsedMs, packets) {
    this.recordThroughput('legacy', wireBytes, elapsedMs);
    this.recordPacketTiming('legacy', packets, elapsedMs);
  },

  evaluateTransport(stats) {
    if (stats.wireBytes >= stats.legacyWireBytes * 0.95) {
      return { use: false, reason: '压缩后链路字节收益不足 5%' };
    }
    if (stats.rawLikePlane) {
      const plane = stats.rawLikePlane;
      return {
        use: false,
        reason:
          `${plane.name} plane 为接近满载的逐行数据（平均 ${plane.avgPackedBytesPerPacket.toFixed(0)}B/包，` +
          `该层链路 ${(plane.ratio * 100).toFixed(1)}%），低收益且易慢于旧协议`
      };
    }
    const avgPackedBytesPerPacket = stats.packets > 0
      ? stats.packedPayloadBytes / stats.packets
      : 0;
    stats.avgPackedBytesPerPacket = avgPackedBytesPerPacket;
    if (avgPackedBytesPerPacket >= this.RAW_LIKE_PACKET_PAYLOAD_BYTES &&
        stats.ratio >= this.RAW_LIKE_MAX_WIRE_RATIO) {
      return {
        use: false,
        reason:
          `候选为接近满载的逐行数据（平均 ${avgPackedBytesPerPacket.toFixed(0)}B/包，` +
          `链路 ${(stats.ratio * 100).toFixed(1)}%），低收益且易慢于旧协议`
      };
    }
    const profile = this.loadPerfProfile();
    stats.eptBps = profile.eptBps;
    stats.legacyBps = profile.legacyBps;
    stats.eptPacketMs = profile.eptPacketMs;
    stats.legacyPacketMs = profile.legacyPacketMs;
    stats.predictedEptMs = stats.packets * profile.eptPacketMs + this.PREDICTED_FIXED_MS;
    stats.predictedLegacyMs = stats.legacyPackets * profile.legacyPacketMs + this.PREDICTED_FIXED_MS;
    if (stats.predictedEptMs > stats.predictedLegacyMs * (1 - this.REQUIRED_TIME_GAIN)) {
      return {
        use: false,
        reason:
          `预计 EPT1 ${(stats.predictedEptMs / 1000).toFixed(1)}s` +
          `（${profile.eptPacketMs.toFixed(1)}ms/包），旧协议 ` +
          `${(stats.predictedLegacyMs / 1000).toFixed(1)}s` +
          `（${profile.legacyPacketMs.toFixed(1)}ms/包），未达到 20% 时延收益`
      };
    }
    return { use: true, reason: '' };
  },

  readU32(data, offset) {
    return (data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) |
      (data[offset + 3] << 24)) >>> 0;
  },

  crc32(data) {
    let crc = 0xffffffff;
    for (let i = 0; i < data.length; i++) {
      crc ^= data[i];
      for (let bit = 0; bit < 8; bit++) crc = (crc & 1) ? ((crc >>> 1) ^ 0xedb88320) : (crc >>> 1);
    }
    return (~crc) >>> 0;
  },

  transactionId() {
    if (window.crypto && window.crypto.getRandomValues) {
      const value = new Uint32Array(1);
      window.crypto.getRandomValues(value);
      return value[0] || 1;
    }
    this.nextTransactionId = (this.nextTransactionId + 1) >>> 0;
    return this.nextTransactionId || 1;
  },

  packBits(row) {
    const out = [];
    let pos = 0;
    while (pos < row.length) {
      let run = 1;
      while (pos + run < row.length && row[pos + run] === row[pos] && run < 128) run++;
      if (run >= 3) {
        out.push((257 - run) & 0xff, row[pos]);
        pos += run;
        continue;
      }
      const literalStart = pos;
      pos += run;
      while (pos < row.length) {
        run = 1;
        while (pos + run < row.length && row[pos + run] === row[pos] && run < 128) run++;
        if (run >= 3 || pos - literalStart + run > 128) break;
        pos += run;
      }
      const count = pos - literalStart;
      out.push(count - 1, ...row.slice(literalStart, pos));
    }
    return Uint8Array.from(out);
  },

  encodePlane(raw, useRowFill = false) {
    if (!(raw instanceof Uint8Array) || raw.length !== this.ROW_BYTES * this.PLANE_ROWS) {
      throw new Error('EPT1 only supports a complete 800x480 plane');
    }
    const records = [];
    let packedLength = 0;
    let rowFillRecords = 0;
    let rowFillRows = 0;
    for (let offset = 0; offset < raw.length; offset += this.ROW_BYTES) {
      const row = raw.slice(offset, offset + this.ROW_BYTES);
      const fillValue = row[0];
      let isFillRow = useRowFill;
      for (let index = 1; isFillRow && index < row.length; index++) {
        if (row[index] !== fillValue) isFillRow = false;
      }
      if (isFillRow) {
        let repeatRows = 1;
        const maxRows = this.rowsPerPacket();
        while (repeatRows < maxRows && offset + (repeatRows + 1) * this.ROW_BYTES <= raw.length) {
          const nextOffset = offset + repeatRows * this.ROW_BYTES;
          let sameFill = true;
          for (let index = 0; index < this.ROW_BYTES; index++) {
            if (raw[nextOffset + index] !== fillValue) {
              sameFill = false;
              break;
            }
          }
          if (!sameFill) break;
          repeatRows++;
        }
        const data = Uint8Array.from([0x00, repeatRows, fillValue]);
        records.push({ data, rows: repeatRows });
        packedLength += data.length;
        rowFillRecords++;
        rowFillRows += repeatRows;
        offset += (repeatRows - 1) * this.ROW_BYTES;
        continue;
      }
      const packed = this.packBits(row);
      const data = packed.length < this.ROW_BYTES
        ? Uint8Array.from([0x80 | packed.length, ...packed])
        : Uint8Array.from([this.ROW_BYTES, ...row]);
      records.push({ data, rows: 1 });
      packedLength += data.length;
    }
    const packedBytes = new Uint8Array(packedLength);
    let packedOffset = 0;
    for (const record of records) {
      packedBytes.set(record.data, packedOffset);
      packedOffset += record.data.length;
    }
    return {
      records, packedLength, rowFillRecords, rowFillRows,
      rawCrc: this.crc32(raw), packedCrc: this.crc32(packedBytes)
    };
  },

  valueLengthLimit() {
    return Math.floor(Number(document.getElementById('mtusize').value));
  },

  rowsPerPacket() {
    const advertised = this.caps ? Number(this.caps.rowsPerPacket) : 0;
    return Number.isFinite(advertised) && advertised >= 1
      ? Math.floor(advertised)
      : this.DEFAULT_ROWS_PER_PACKET;
  },

  packetRecordCount(records, start, valueLengthLimit) {
    let bytes = 9; // command + transaction id + packed offset
    let count = 0;
    let rows = 0;
    const rowLimit = this.rowsPerPacket();
    while (start + count < records.length) {
      const record = records[start + count];
      const nextLength = record.data.length;
      if (rows + record.rows > rowLimit) break;
      if (bytes + nextLength > valueLengthLimit) break;
      bytes += nextLength;
      rows += record.rows;
      count++;
    }
    return count;
  },

  wireStats(encoded, valueLengthLimit) {
    let bytes = 28 + 5; // BEGIN + END
    let packets = 0;
    for (let row = 0; row < encoded.records.length;) {
      const count = this.packetRecordCount(encoded.records, row, valueLengthLimit);
      if (count === 0) return { bytes: Number.POSITIVE_INFINITY, packets: Number.POSITIVE_INFINITY };
      bytes += 9;
      for (let i = 0; i < count; i++) bytes += encoded.records[row + i].data.length;
      row += count;
      packets++;
    }
    return { bytes, packets };
  },

  resetWriteStats() {
    if (!this.lastStats) return;
    this.lastStats.actualDataPackets = 0;
    this.lastStats.withResponseWrites = 0;
    this.lastStats.withoutResponseWrites = 0;
    this.lastStats.writeAwaitMs = 0;
    this.lastStats.withResponseWaitMs = 0;
    this.lastStats.withoutResponseWaitMs = 0;
    this.lastStats.maxWriteAwaitMs = 0;
  },

  async writePackedDataFast(id, offset, bytes, withResponse) {
    if (!epdCharacteristic) throw new Error('EPT1 characteristic unavailable');
    if (typeof bleDevice !== 'undefined' && bleDevice && bleDevice.gatt && !bleDevice.gatt.connected) {
      throw new Error('EPT1 BLE disconnected');
    }
    const packet = new Uint8Array(9 + bytes.length);
    packet[0] = EpdCmd.PACKED_DATA;
    packet.set(this.u32le(id), 1);
    packet.set(this.u32le(offset), 5);
    packet.set(bytes, 9);
    const startedAt = this.now();
    if (withResponse) {
      await epdCharacteristic.writeValueWithResponse(packet);
    } else {
      await epdCharacteristic.writeValueWithoutResponse(packet);
    }
    const elapsedMs = Math.max(0, this.now() - startedAt);
    if (this.lastStats) {
      this.lastStats.actualDataPackets++;
      if (withResponse) {
        this.lastStats.withResponseWrites++;
        this.lastStats.withResponseWaitMs += elapsedMs;
      } else {
        this.lastStats.withoutResponseWrites++;
        this.lastStats.withoutResponseWaitMs += elapsedMs;
      }
      this.lastStats.writeAwaitMs += elapsedMs;
      this.lastStats.maxWriteAwaitMs = Math.max(this.lastStats.maxWriteAwaitMs, elapsedMs);
    }
  },

  abortIfSlowPath() {
    const stats = this.lastStats;
    if (!stats || stats.actualDataPackets < this.SLOW_PATH_PROBE_PACKETS ||
        stats.writeAwaitMs <= 0 || stats.predictedLegacyMs <= 0) return;
    const observedMsPerPacket = stats.writeAwaitMs / stats.actualDataPackets;
    const projectedMs = observedMsPerPacket * stats.packets + this.PREDICTED_FIXED_MS;
    stats.liveProjectedEptMs = projectedMs;
    if (projectedMs <= stats.predictedLegacyMs) return;
    this.recordPacketTiming('ept', stats.actualDataPackets, stats.writeAwaitMs);
    stats.performanceAbortReason =
      `前 ${stats.actualDataPackets} 包实测 ${observedMsPerPacket.toFixed(1)}ms/包，` +
      `预计整图 ${(projectedMs / 1000).toFixed(1)}s，慢于旧协议 ` +
      `${(stats.predictedLegacyMs / 1000).toFixed(1)}s`;
    throw new Error(`EPT1 slow path: ${stats.performanceAbortReason}`);
  },

  popcount8(value) {
    value &= 0xff;
    value = value - ((value >>> 1) & 0x55);
    value = (value & 0x33) + ((value >>> 2) & 0x33);
    return (value + (value >>> 4)) & 0x0f;
  },

  normalizePlanes(black, red) {
    if (!red) return { black, red: null, overlapPixels: 0 };
    if (!(black instanceof Uint8Array) || !(red instanceof Uint8Array) || black.length !== red.length) {
      throw new Error('black/red plane size mismatch');
    }
    let normalizedBlack = black;
    let overlapPixels = 0;
    for (let i = 0; i < black.length; i++) {
      const overlap = (~(black[i] | red[i])) & 0xff;
      if (overlap === 0) continue;
      if (normalizedBlack === black) normalizedBlack = black.slice();
      // Red takes precedence, matching convertUC8159() and the legacy path.
      normalizedBlack[i] |= (~red[i]) & 0xff;
      overlapPixels += this.popcount8(overlap);
    }
    return { black: normalizedBlack, red, overlapPixels };
  },

  constantPlaneValue(raw) {
    if (!(raw instanceof Uint8Array) || raw.length !== this.ROW_BYTES * this.PLANE_ROWS) {
      throw new Error('EPT1 only supports a complete 800x480 plane');
    }
    const value = raw[0];
    for (let i = 1; i < raw.length; i++) {
      if (raw[i] !== value) return null;
    }
    return value;
  },

  async waitFor(kind, id, timeoutMs = 1800) {
    const key = `${kind}:${id}`;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        delete this.waiters[key];
        reject(new Error(`${kind} timeout`));
      }, timeoutMs);
      this.waiters[key] = {
        resolve: value => { clearTimeout(timer); delete this.waiters[key]; resolve(value); },
        reject: error => { clearTimeout(timer); delete this.waiters[key]; reject(error); }
      };
    });
  },

  resolve(kind, id, value) {
    const waiter = this.waiters[`${kind}:${id}`];
    if (waiter) waiter.resolve(value);
  },

  async queryCaps() {
    if (typeof EpdCmd === 'undefined' || !epdCharacteristic) return null;
    const pending = this.waitFor('caps', 0, 1200);
    if (!await write(EpdCmd.GET_CAPS, null, true, false)) return null;
    try { return await pending; } catch (_) { return null; }
  },

  async queryStatus(id) {
    const pending = this.waitFor('status', id, 1200);
    if (!await write(EpdCmd.QUERY_PACKED_STATUS, this.u32le(id), true, false)) throw new Error('EPT1 status write failed');
    return pending;
  },

  async waitForDisplayIdle() {
    if (!this.caps || !this.caps.idleFence) return { waitedMs: 0, supported: false };
    const startedAt = this.now();
    const pending = this.waitFor('idle', 0, this.IDLE_FENCE_TIMEOUT_MS);
    if (!await write(EpdCmd.WAIT_FOR_IDLE, null, true, false)) {
      throw new Error('屏幕空闲栅栏写入失败');
    }
    const state = await pending;
    if (state.busy) throw new Error('设备仍处于刷新状态');
    return { waitedMs: Math.max(0, this.now() - startedAt), supported: true };
  },

  retryablePlaneError(id) {
    return this.lastError && this.lastError.id === id &&
      this.lastError.reason >= 3 && this.lastError.reason <= 7;
  },

  async sendPlane(raw, plane, encoded, progress) {
    let planeRetries = 0;
    let busyWaitMs = 0;
    let busyNotified = false;
    while (true) {
      this.lastError = null;
      try {
        await this.sendPlaneOnce(raw, plane, encoded, progress);
        return;
      } catch (error) {
        const failedId = this.activeId;
        const currentError = this.lastError && this.lastError.id === failedId ? this.lastError : null;
        if (currentError && currentError.reason === 8 && busyWaitMs < this.MAX_BUSY_WAIT_MS) {
          if (!busyNotified && typeof addLog === 'function') {
            addLog('设备仍在刷新，EPT1 将等待屏幕空闲后自动继续');
            busyNotified = true;
          }
          const waitMs = Math.min(this.BUSY_RETRY_DELAY_MS, this.MAX_BUSY_WAIT_MS - busyWaitMs);
          const waitStartedAt = Date.now();
          await new Promise(resolve => setTimeout(resolve, waitMs));
          const actualWaitMs = Math.max(waitMs, Date.now() - waitStartedAt);
          busyWaitMs += actualWaitMs;
          if (this.lastStats) this.lastStats.busyWaitMs += actualWaitMs;
          continue;
        }
        if (planeRetries < this.MAX_PLANE_RETRIES && this.retryablePlaneError(failedId)) {
          planeRetries++;
          continue;
        }
        throw error;
      }
    }
  },

  async sendPlaneOnce(raw, plane, encoded, progress) {
    const id = this.transactionId();
    this.activeId = id;
    const begin = [1, plane, 0, ...this.u32le(id),
      ...this.u16le(this.ROW_BYTES), ...this.u16le(this.PLANE_ROWS),
      ...this.u32le(raw.length), ...this.u32le(encoded.packedLength),
      ...this.u32le(encoded.rawCrc), ...this.u32le(encoded.packedCrc)];
    if (begin.length !== 27) throw new Error(`EPT1 invalid BEGIN payload length ${begin.length}`);
    const beginWait = this.waitFor('begin', id);
    if (!await write(EpdCmd.BEGIN_PACKED_PLANE, begin, true, false)) throw new Error('EPT1 begin write failed');
    await beginWait;

    const valueLengthLimit = this.valueLengthLimit();
    let offset = 0;
    let packetIndex = 0;
    let sentRows = 0;
    for (let row = 0; row < encoded.records.length;) {
      const recordCount = this.packetRecordCount(encoded.records, row, valueLengthLimit);
      if (recordCount === 0) throw new Error('EPT1 ATT value length is too small for one row');
      const records = encoded.records.slice(row, row + recordCount);
      const bytesLength = records.reduce((sum, record) => sum + record.data.length, 0);
      const bytes = new Uint8Array(bytesLength);
      let bytesOffset = 0;
      for (const record of records) {
        bytes.set(record.data, bytesOffset);
        bytesOffset += record.data.length;
      }
      packetIndex++;
      const lastPacket = row + recordCount === encoded.records.length;
      const response = (packetIndex % (this.caps.rxWindow || 16)) === 0 || lastPacket;
      await this.writePackedDataFast(id, offset, bytes, response);
      if (response) this.abortIfSlowPath();
      offset += bytes.length;
      sentRows += records.reduce((sum, record) => sum + record.rows, 0);
      if (progress) progress(sentRows, this.PLANE_ROWS);
      row += recordCount;
    }
    const doneWait = this.waitFor('done', id, 1800);
    if (!await write(EpdCmd.END_PACKED_PLANE, this.u32le(id), true, false)) throw new Error('EPT1 end write failed');
    try {
      await doneWait;
    } catch (_) {
      const status = await this.queryStatus(id);
      const complete = plane === 0x0f ? (status.state & 0x08) : (status.state & 0x10);
      if (!complete) throw new Error('EPT1 plane did not commit');
    }
    this.activeId = 0;
  },

  async sendSolidImageOnce(blackValue, redValue, progress) {
    const id = this.transactionId();
    this.activeId = id;
    const fill = [1, 0, ...this.u32le(id), blackValue, redValue];
    if (fill.length !== 8) throw new Error(`EPT1 invalid solid-fill payload length ${fill.length}`);
    const doneWait = this.waitFor('done', id, 2500);
    const writeStartedAt = this.now();
    const writeOk = await write(EpdCmd.FILL_SOLID_IMAGE, fill, true, false);
    const writeElapsedMs = Math.max(0, this.now() - writeStartedAt);
    if (this.lastStats) {
      this.lastStats.actualDataPackets = 1;
      this.lastStats.withResponseWrites = 1;
      this.lastStats.withResponseWaitMs = writeElapsedMs;
      this.lastStats.writeAwaitMs = writeElapsedMs;
      this.lastStats.maxWriteAwaitMs = writeElapsedMs;
    }
    if (!writeOk) throw new Error('EPT1 solid-fill write failed');
    if (progress) progress('solid', 0, 1);
    try {
      await doneWait;
    } catch (_) {
      const status = await this.queryStatus(id);
      if ((status.state & 0x18) !== 0x18) throw new Error('EPT1 solid image did not commit');
    }
    if (progress) progress('solid', 1, 1);
    this.activeId = 0;
  },

  async sendSolidImage(blackValue, redValue, progress) {
    let retries = 0;
    let busyWaitMs = 0;
    let busyNotified = false;
    while (true) {
      this.lastError = null;
      try {
        await this.sendSolidImageOnce(blackValue, redValue, progress);
        return;
      } catch (error) {
        const failedId = this.activeId;
        const currentError = this.lastError && this.lastError.id === failedId ? this.lastError : null;
        if (currentError && currentError.reason === 8 && busyWaitMs < this.MAX_BUSY_WAIT_MS) {
          if (!busyNotified && typeof addLog === 'function') {
            addLog('设备仍在刷新，EPT1 纯色填充将等待屏幕空闲后自动继续');
            busyNotified = true;
          }
          const waitMs = Math.min(this.BUSY_RETRY_DELAY_MS, this.MAX_BUSY_WAIT_MS - busyWaitMs);
          const waitStartedAt = Date.now();
          await new Promise(resolve => setTimeout(resolve, waitMs));
          const actualWaitMs = Math.max(waitMs, Date.now() - waitStartedAt);
          busyWaitMs += actualWaitMs;
          if (this.lastStats) this.lastStats.busyWaitMs += actualWaitMs;
          continue;
        }
        if (retries < this.MAX_PLANE_RETRIES && this.retryablePlaneError(failedId)) {
          retries++;
          continue;
        }
        throw error;
      }
    }
  },

  async sendImage(black, red, progress) {
    this.lastStats = null;
    const valueLengthLimit = this.valueLengthLimit();
    if (!this.caps || this.caps.version !== 1 || this.caps.minValueLen > valueLengthLimit) {
      return false;
    }
    const normalized = this.normalizePlanes(black, red);
    black = normalized.black;
    red = normalized.red;
    const rawWire = black.length + (red ? red.length : 0);
    const planeCount = red ? 2 : 1;
    const legacyChunkSize = Math.max(valueLengthLimit - 8, 20);
    const legacyPackets = planeCount * Math.ceil(black.length / legacyChunkSize);
    const legacyWireBytes = rawWire + legacyPackets * 8;
    const blackFill = this.constantPlaneValue(black);
    const redFill = red ? this.constantPlaneValue(red) : null;
    if (red && this.caps.solidFill && blackFill !== null && redFill !== null) {
      this.lastStats = {
        rawBytes: rawWire,
        packedPayloadBytes: 2,
        wireBytes: 9,
        packets: 1,
        legacyWireBytes,
        legacyPackets,
        rowsPerPacket: this.rowsPerPacket(),
        overlapPixels: normalized.overlapPixels,
        ratio: 9 / legacyWireBytes,
        solidImage: true,
        solidBlackValue: blackFill,
        solidRedValue: redFill,
        elapsedMs: 0,
        totalElapsedMs: 0,
        busyWaitMs: 0,
        actualDataPackets: 0,
        withResponseWrites: 0,
        withoutResponseWrites: 0,
        writeAwaitMs: 0,
        withResponseWaitMs: 0,
        withoutResponseWaitMs: 0,
        maxWriteAwaitMs: 0,
        predictedEptMs: 0,
        predictedLegacyMs: 0,
        eptBps: 0,
        legacyBps: 0,
        eptPacketMs: 0,
        legacyPacketMs: 0,
        avgPackedBytesPerPacket: 0,
        liveProjectedEptMs: 0,
        performanceAbortReason: '',
        fallbackReason: ''
      };
      this.resetWriteStats();
      const transportDecision = this.evaluateTransport(this.lastStats);
      if (!transportDecision.use) {
        this.lastStats.fallbackReason = transportDecision.reason;
        return false;
      }
      const startedAt = this.now();
      await this.sendSolidImage(blackFill, redFill, progress);
      const finishedAt = this.now();
      this.lastStats.totalElapsedMs = Math.max(0, finishedAt - startedAt);
      this.lastStats.elapsedMs = Math.max(0, this.lastStats.totalElapsedMs - this.lastStats.busyWaitMs);
      // This packet waits for 96 KB of MCU SPI writes; do not train the normal
      // PackBits per-write performance model with this fundamentally different path.
      return true;
    }
    const useRowFill = !!this.caps.rowFill;
    const blackEncoded = this.encodePlane(black, useRowFill);
    const redEncoded = red ? this.encodePlane(red, useRowFill) : null;
    const blackWire = this.wireStats(blackEncoded, valueLengthLimit);
    const redWire = redEncoded ? this.wireStats(redEncoded, valueLengthLimit) : { bytes: 0, packets: 0 };
    const packedWire = blackWire.bytes + redWire.bytes;
    const legacyPlaneWireBytes = black.length + Math.ceil(black.length / legacyChunkSize) * 8;
    const planeCandidates = [
      {
        name: 'black',
        packedBytesPerPacket: blackWire.packets > 0 ? blackEncoded.packedLength / blackWire.packets : 0,
        ratio: blackWire.bytes / legacyPlaneWireBytes
      }
    ];
    if (redEncoded) {
      planeCandidates.push({
        name: 'red',
        packedBytesPerPacket: redWire.packets > 0 ? redEncoded.packedLength / redWire.packets : 0,
        ratio: redWire.bytes / legacyPlaneWireBytes
      });
    }
    const rawLikePlane = planeCandidates.find(plane =>
      plane.packedBytesPerPacket >= this.RAW_LIKE_PACKET_PAYLOAD_BYTES &&
      plane.ratio >= this.RAW_LIKE_MAX_WIRE_RATIO);
    this.lastStats = {
      rawBytes: rawWire,
      packedPayloadBytes: blackEncoded.packedLength + (redEncoded ? redEncoded.packedLength : 0),
      wireBytes: packedWire,
      packets: blackWire.packets + redWire.packets,
      legacyWireBytes,
      legacyPackets,
      rowsPerPacket: this.rowsPerPacket(),
      rowFillRecords: blackEncoded.rowFillRecords + (redEncoded ? redEncoded.rowFillRecords : 0),
      rowFillRows: blackEncoded.rowFillRows + (redEncoded ? redEncoded.rowFillRows : 0),
      overlapPixels: normalized.overlapPixels,
      ratio: packedWire / legacyWireBytes,
      rawLikePlane: rawLikePlane ? {
        name: rawLikePlane.name,
        avgPackedBytesPerPacket: rawLikePlane.packedBytesPerPacket,
        ratio: rawLikePlane.ratio
      } : null,
      elapsedMs: 0,
      totalElapsedMs: 0,
      busyWaitMs: 0,
      actualDataPackets: 0,
      withResponseWrites: 0,
      withoutResponseWrites: 0,
      writeAwaitMs: 0,
      withResponseWaitMs: 0,
      withoutResponseWaitMs: 0,
      maxWriteAwaitMs: 0,
      predictedEptMs: 0,
      predictedLegacyMs: 0,
      eptBps: 0,
      legacyBps: 0,
      eptPacketMs: 0,
      legacyPacketMs: 0,
      avgPackedBytesPerPacket: 0,
      liveProjectedEptMs: 0,
      performanceAbortReason: '',
      fallbackReason: ''
    };
    this.resetWriteStats();
    const transportDecision = this.evaluateTransport(this.lastStats);
    if (!transportDecision.use) {
      this.lastStats.fallbackReason = transportDecision.reason;
      return false;
    }
    const startedAt = this.now();
    await this.sendPlane(black, 0x0f, blackEncoded, (done, total) => progress && progress('black', done, total));
    if (redEncoded) await this.sendPlane(red, 0x00, redEncoded, (done, total) => progress && progress('red', done, total));
    const finishedAt = this.now();
    this.lastStats.totalElapsedMs = Math.max(0, finishedAt - startedAt);
    this.lastStats.elapsedMs = Math.max(0, this.lastStats.totalElapsedMs - this.lastStats.busyWaitMs);
    this.recordThroughput('ept', this.lastStats.wireBytes, this.lastStats.elapsedMs);
    this.recordPacketTiming('ept', this.lastStats.actualDataPackets, this.lastStats.writeAwaitMs);
    return true;
  },

  handleNotification(value) {
    const data = value instanceof DataView ? new Uint8Array(value.buffer, value.byteOffset, value.byteLength) : new Uint8Array(value);
    if (data.length < 1) return;
    if (data[0] === 0xa9 && data.length >= 8) {
      this.caps = {
        version: data[1], minValueLen: data[2] | (data[3] << 8), rowsPerPacket: data[4],
        widthBytes: data[5], color: data[6], rxWindow: data[7],
        solidFill: data.length >= 10 && (data[9] & 0x01) !== 0,
        rowFill: data.length >= 10 && (data[9] & 0x02) !== 0,
        idleFence: data.length >= 10 && (data[9] & 0x04) !== 0
      };
      this.resolve('caps', 0, this.caps);
    } else if (data[0] === 0xa8 && data.length >= 7) {
      this.resolve('begin', this.readU32(data, 1), { plane: data[5], rxWindow: data[6] });
    } else if (data[0] === 0xa5 && data.length >= 6) {
      this.resolve('done', this.readU32(data, 1), { plane: data[5] });
    } else if (data[0] === 0xa7 && data.length >= 12) {
      this.resolve('status', this.readU32(data, 1), { state: data[5], plane: data[6], offset: this.readU32(data, 7), rxWindow: data[11] });
    } else if (data[0] === 0xaa && data.length >= 2) {
      this.resolve('idle', 0, { busy: data[1] !== 0 });
    } else if (data[0] === 0xa6 && data.length >= 11) {
      this.lastError = { id: this.readU32(data, 1), plane: data[5], reason: data[6], offset: this.readU32(data, 7) };
      const waiter = this.waiters[`begin:${this.lastError.id}`] || this.waiters[`done:${this.lastError.id}`];
      const reasonName = this.ERROR_NAMES[this.lastError.reason] || 'UNKNOWN';
      if (waiter) {
        waiter.reject(new Error(
          `EPT1 ${reasonName} (${this.lastError.reason}), plane=0x${this.lastError.plane.toString(16)}, offset=${this.lastError.offset}`));
      }
    }
  }
};

if (typeof window !== 'undefined') window.EptTransfer = EptTransfer;
