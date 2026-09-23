/* Dynamic external-font package manager.  The device receives one complete
 * package containing the selected WQY9/WQY12/HELVB18 resources. */
(function () {
  const SLOT_NAMES = ['WQY9', 'WQY12', 'HELVB18'];
  const LEGACY_MAX_PACK_SIZE = 0x7F000;
  const MAX_PACK_SIZE = 0x7E000; // FST3 reserves two directory sectors.
  const OP_GET = 0x00;
  const OP_PACK_BEGIN = 0x06;
  const OP_PACK_DATA = 0x07;
  const OP_PACK_COMMIT = 0x08;
  const OP_PACK_ABORT = 0x09;
  const OP_PACK_STREAM_DATA = 0x0A;
  const RSP_FONT_STREAM_ACK = 0xA5;
  const STREAM_WINDOW_SIZE = 3;
  const STREAM_PACKET_OVERHEAD = 10; // op + seq:u16 + offset:u32 + crc16
  const LIST_TIMEOUT_MS = 10000;
  const ACK_TIMEOUT_MS = {
    [OP_PACK_BEGIN]: 15000,
    [OP_PACK_DATA]: 10000,
    [OP_PACK_COMMIT]: 20000,
    [OP_PACK_ABORT]: 10000,
  };

  const FontStore = {
    slots: SLOT_NAMES.map((name) => ({ name, present: false, length: 0, crc32: 0 })),
    ackWaiter: null,
    streamAckWaiter: null,
    listWaiter: null,
    listLoaded: false,
    operationTail: Promise.resolve(),
    pendingOperations: 0,
    initialized: false,
    session: 0,

    maxPackSize() {
      return typeof appVersion === 'number' && appVersion >= 0x2F ? MAX_PACK_SIZE : LEGACY_MAX_PACK_SIZE;
    },

    validateHeader(slot, data) {
      const geometry = [
        [12, 12, 0, -2, 8, -2, 10, -1],
        [16, 16, 0, -2, 11, -3, 12, -4],
        [13, 22, 0, -3, 19, -5, 19, -5],
      ];
      if (data.length < 23 || data[0] === 0 || data[0] === 255 ||
          Array.from(data.slice(2, 9)).some(bits => bits < 1 || bits > 8) ||
          geometry[slot].some((value, i) => ((data[9 + i] << 24) >> 24) !== value) ||
          [17, 19, 21].some(i => 23 + ((data[i] << 8) | data[i + 1]) >= data.length)) {
        throw new Error(`${SLOT_NAMES[slot]} 字库头或尺寸不匹配，请选择对应槽位的 BIN`);
      }
      const table = 23 + ((data[21] << 8) | data[22]);
      const delta = (data[table] << 8) | data[table + 1];
      const marker = (data[table + 2] << 8) | data[table + 3];
      if (table + 4 > data.length || table + delta >= data.length ||
          (marker !== 0 && (marker !== 0xFFFF || delta !== 4))) {
        throw new Error(`${SLOT_NAMES[slot]} 需要单个 Unicode 分块的字库`);
      }
    },

    skipUnchanged(pack) {
      if (!this.listLoaded || pack.slots.length !== SLOT_NAMES.length ||
          !pack.slots.every((slot, i) => this.slots[i] &&
            slot.length === this.slots[i].length && slot.crc32 === this.slots[i].crc32)) return false;
      const progress = document.getElementById('fontPackProgress');
      if (progress) progress.value = 100;
      addLog('字库内容未变化，无需重新写入');
      return true;
    },

    assertSession(session = this.session) {
      if (session !== this.session || !epdCharacteristic || !gattServer || !gattServer.connected) {
        throw new Error('蓝牙连接已断开，请重新连接设备');
      }
    },

    setUiBusy(busy) {
      const partitionFirmware = typeof appVersion === 'number' && appVersion >= 0x33;
      document.querySelectorAll('.font-store-control').forEach((control) => {
        control.disabled = busy || partitionFirmware;
      });
      const hint = document.getElementById('legacyFontHint');
      if (hint) hint.textContent = partitionFirmware ?
        '当前固件使用 Flash 分区资源，旧版 BIN 字库操作已停用。请使用上方“外置 Flash 一键烧录”更新 fonts.ers。' :
        '适用于支持 FST2 / FST3 字库更新的旧固件。新版分区固件请使用上方资源维护入口上传 fonts.ers。';
    },

    onDisconnect() {
      this.session++;
      const error = new Error('蓝牙连接已断开');
      if (this.ackWaiter) {
        const waiter = this.ackWaiter;
        clearTimeout(waiter.timer);
        this.ackWaiter = null;
        waiter.reject(error);
      }
      if (this.streamAckWaiter) {
        const waiter = this.streamAckWaiter;
        clearTimeout(waiter.timer);
        this.streamAckWaiter = null;
        waiter.reject(error);
      }
      if (this.listWaiter) {
        const waiter = this.listWaiter;
        clearTimeout(waiter.timer);
        this.listWaiter = null;
        waiter.reject(error);
      }
      this.listLoaded = false;
      this.slots = SLOT_NAMES.map((name) => ({ name, present: false, length: 0, crc32: 0 }));
      SLOT_NAMES.forEach((_, slot) => {
        const state = document.getElementById(`fontState${slot}`);
        if (state) state.textContent = '未连接（尚未读取）';
      });
    },

    enqueue(operation) {
      const session = this.session;
      this.pendingOperations++;
      this.setUiBusy(true);
      const result = this.operationTail.then(() => {
        this.assertSession(session);
        return operation();
      });
      this.operationTail = result.catch(() => {});
      const finish = () => {
        this.pendingOperations--;
        if (this.pendingOperations === 0) this.setUiBusy(false);
      };
      result.then(finish, finish);
      return result;
    },

    crc32(data) {
      let crc = 0xFFFFFFFF;
      for (const byte of data) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) {
          crc = (crc & 1) ? ((crc >>> 1) ^ 0xEDB88320) : (crc >>> 1);
        }
      }
      return (crc ^ 0xFFFFFFFF) >>> 0;
    },

    crc16(data) {
      let crc = 0xFFFF;
      for (const byte of data) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) {
          crc = (crc & 1) ? ((crc >>> 1) ^ 0x8408) : (crc >>> 1);
        }
      }
      return crc & 0xFFFF;
    },

    u32(value) {
      return [value & 0xFF, (value >>> 8) & 0xFF,
        (value >>> 16) & 0xFF, (value >>> 24) & 0xFF];
    },

    parseU32(data, offset) {
      return (data[offset] | (data[offset + 1] << 8) |
        (data[offset + 2] << 16) | (data[offset + 3] << 24)) >>> 0;
    },

    handleNotification(value) {
      const data = value instanceof DataView
        ? new Uint8Array(value.buffer, value.byteOffset, value.byteLength)
        : new Uint8Array(value);
      if (data.length === 0) return false;

      if (data[0] === 0xA3 && data.length >= 11) {
        const slot = data[1];
        if (slot < this.slots.length) {
          this.slots[slot] = {
            name: SLOT_NAMES[slot],
            present: data[2] !== 0,
            length: this.parseU32(data, 3),
            crc32: this.parseU32(data, 7),
          };
          this.renderSlot(slot);
          if (this.listWaiter && this.slots.every((item) => item != null)) {
            const resolve = this.listWaiter.resolve;
            clearTimeout(this.listWaiter.timer);
            this.listWaiter = null;
            resolve(this.slots.slice());
          }
        }
        return true;
      }

      if (data[0] === 0xA4 && data.length >= 4) {
        if (this.ackWaiter && this.ackWaiter.operation === data[1] &&
            (this.ackWaiter.slot === 0xFF || this.ackWaiter.slot === data[3])) {
          const waiter = this.ackWaiter;
          clearTimeout(waiter.timer);
          this.ackWaiter = null;
          waiter.resolve({ status: data[2], slot: data[3] });
        }
        return true;
      }
      if (data[0] === RSP_FONT_STREAM_ACK && data.length >= 9) {
        const response = {
          nextSequence: data[1] | (data[2] << 8),
          nextOffset: this.parseU32(data, 3),
          credit: data[7],
          status: data[8],
        };
        if (this.streamAckWaiter) {
          const waiter = this.streamAckWaiter;
          /* A delayed ACK from the previous window must not complete the
           * current waiter.  Equal-to-start ACKs are valid (for example a
           * CRC error on the first packet), while anything outside this
           * window is stale or malformed and is ignored until the timer. */
          if (response.nextSequence < waiter.minSequence ||
              response.nextSequence > waiter.maxSequence ||
              response.nextOffset < waiter.minOffset ||
              response.nextOffset > waiter.maxOffset) {
            return true;
          }
          clearTimeout(waiter.timer);
          this.streamAckWaiter = null;
          waiter.resolve(response);
        }
        return true;
      }
      return false;
    },

    waitAck(operation, slot, timeout = ACK_TIMEOUT_MS[operation] || 10000) {
      if (this.ackWaiter) {
        return Promise.reject(new Error('已有字库操作正在等待设备响应'));
      }
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          this.ackWaiter = null;
          reject(new Error('字库操作响应超时'));
        }, timeout);
        this.ackWaiter = { operation, slot, resolve, reject, timer };
      });
    },

    async requestAck(operation, slot, payload) {
      const session = this.session;
      this.assertSession(session);
      const ack = this.waitAck(operation, slot);
      // A notification/disconnect can reject before the GATT write settles.
      // Observe immediately; awaiting the original promise still reports it.
      ack.catch(() => {});
      const waiter = this.ackWaiter;
      try {
        if (!await write(EpdCmd.FONT_STORE, payload, true, false)) {
          throw new Error('字库命令发送失败');
        }
      } catch (error) {
        if (this.ackWaiter === waiter) {
          clearTimeout(waiter.timer);
          this.ackWaiter = null;
          waiter.reject(error);
        }
        throw error;
      }
      const response = await ack;
      this.assertSession(session);
      if (response.status !== 0) {
        if (response.status === 6) throw new Error('新字库包没有足够的暂存空间，旧字库已保留；请缩小新包');
        if (response.status === 2) throw new Error('设备忙；字库更新间隔至少 60 秒，请稍后重试');
        if (response.status === 5 && operation === OP_PACK_COMMIT) {
          throw new Error('字库提交遇到 Flash 错误，请重启设备并重新读取列表后重试');
        }
        throw new Error(`设备拒绝字库操作（状态 ${response.status}）`);
      }
      return response;
    },

    waitStreamAck(minSequence, minOffset, maxSequence, maxOffset, timeout = 15000) {
      if (this.streamAckWaiter) {
        return Promise.reject(new Error('已有字库数据窗口正在等待设备响应'));
      }
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          this.streamAckWaiter = null;
          reject(new Error('字库数据窗口响应超时'));
        }, timeout);
        this.streamAckWaiter = {
          resolve, reject, timer, minSequence, minOffset, maxSequence, maxOffset,
        };
      });
    },

    makeStreamPayload(sequence, offset, chunk) {
      const payload = [OP_PACK_STREAM_DATA,
        sequence & 0xFF, (sequence >>> 8) & 0xFF,
        ...this.u32(offset), ...chunk];
      const crc = this.crc16(Uint8Array.from(payload));
      payload.push(crc & 0xFF, (crc >>> 8) & 0xFF);
      return payload;
    },

    async listNow() {
      const session = this.session;
      this.assertSession(session);
      if (this.listWaiter) throw new Error('正在读取字库列表');
      this.slots = [null, null, null];
      this.listLoaded = false;
      const result = new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          this.listWaiter = null;
          reject(new Error('字库列表读取超时'));
        }, LIST_TIMEOUT_MS);
        this.listWaiter = { resolve, reject, timer };
      });
      result.catch(() => {});
      try {
        if (!await write(EpdCmd.FONT_STORE, [OP_GET], true, false)) {
          throw new Error('字库列表请求发送失败');
        }
      } catch (error) {
        if (this.listWaiter) {
          const waiter = this.listWaiter;
          clearTimeout(waiter.timer);
          this.listWaiter = null;
          waiter.reject(error);
        }
        throw error;
      }
      const slots = await result;
      this.assertSession(session);
      this.listLoaded = true;
      return slots;
    },

    list() {
      return this.enqueue(() => this.listNow());
    },

    async buildPack() {
      const session = this.session;
      this.assertSession(session);
      if (!this.listLoaded || this.slots.some((item) => item == null)) {
        throw new Error('设备字库列表尚未读取成功，请先重新连接或重试读取');
      }
      const files = [];
      let totalLength = 0;
      const maxPackSize = this.maxPackSize();

      for (let slot = 0; slot < SLOT_NAMES.length; slot++) {
        const input = document.getElementById(`fontFile${slot}`);
        const deleteBox = document.getElementById(`fontDelete${slot}`);
        const file = input && input.files ? input.files[0] : null;
        const deleting = Boolean(deleteBox && deleteBox.checked);

        if (deleting && file) {
          throw new Error(`${SLOT_NAMES[slot]} 不能同时选择文件和删除`);
        }
        if (deleting) {
          files.push({ data: null, length: 0, crc32: 0 });
          continue;
        }
        if (!file) {
          if (this.slots[slot] && this.slots[slot].present) {
            throw new Error(`${SLOT_NAMES[slot]} 当前已安装，请选择 BIN 保留，或勾选删除`);
          }
          files.push({ data: null, length: 0, crc32: 0 });
          continue;
        }
        if (file.size === 0) {
          throw new Error(`${SLOT_NAMES[slot]} 文件为空`);
        }
        if (file.size > maxPackSize - totalLength) {
          throw new Error(`字库总大小超过外置 Flash 可用空间（${maxPackSize} 字节）`);
        }
        const data = new Uint8Array(await file.arrayBuffer());
        this.assertSession(session);
        this.validateHeader(slot, data);
        const item = { data, length: data.length, crc32: this.crc32(data) };
        totalLength += item.length;
        files.push(item);
      }

      if (totalLength > maxPackSize) {
        throw new Error(`字库总大小超过外置 Flash 可用空间（${maxPackSize} 字节）`);
      }

      const data = new Uint8Array(totalLength);
      let offset = 0;
      const slots = files.map((item) => ({ length: item.length, crc32: item.crc32 }));
      files.forEach((item) => {
        if (item.data) {
          data.set(item.data, offset);
          offset += item.data.length;
        }
      });
      return { data, slots };
    },

    async uploadPackLegacyNow() {
      const session = this.session;
      const pack = await this.buildPack();
      this.assertSession(session);
      this.requirePackMtu();
      if (this.skipUnchanged(pack)) return;
      const packCrc32 = this.crc32(pack.data);
      const beginPayload = [OP_PACK_BEGIN,
        ...this.u32(pack.data.length), ...this.u32(packCrc32)];
      pack.slots.forEach((slot) => {
        beginPayload.push(...this.u32(slot.length), ...this.u32(slot.crc32));
      });

      const progress = document.getElementById('fontPackProgress');
      let started = false;
      try {
        started = true;
        await this.requestAck(OP_PACK_BEGIN, 0xFF, beginPayload);

        const mtu = typeof currentMtuValue === 'function' ? currentMtuValue() : 20;
        const chunkSize = Math.max(8, Math.min(180, mtu - 6));
        for (let offset = 0; offset < pack.data.length; offset += chunkSize) {
          const chunk = pack.data.slice(offset, Math.min(offset + chunkSize, pack.data.length));
          await this.requestAck(OP_PACK_DATA, 0xFF,
            [OP_PACK_DATA, ...this.u32(offset), ...chunk]);
          if (progress) {
            progress.value = Math.round(((offset + chunk.length) * 100) / pack.data.length);
          }
        }
        await this.requestAck(OP_PACK_COMMIT, 0xFF, [OP_PACK_COMMIT]);
        if (progress) progress.value = 100;
        addLog(`字库全量更新完成（${pack.data.length} 字节）`);
        try {
          await this.listNow();
        } catch (error) {
          addLog(`字库列表刷新失败（更新已完成）: ${error.message}`);
        }
      } catch (error) {
        if (started && session === this.session && gattServer && gattServer.connected) {
          try {
            await this.requestAck(OP_PACK_ABORT, 0xFF, [OP_PACK_ABORT]);
          } catch (_) {
            /* Best effort: disconnect handler also aborts the device session. */
          }
        }
        throw error;
      }
    },

    async sendStreamPacket(payload) {
      const session = this.session;
      for (let attempt = 0; attempt < 3; attempt++) {
        this.assertSession(session);
        /* The first attempt bypasses the global command queue so the device
         * FIFO can receive a real burst.  If the browser rejects concurrent
         * GATT operations, retry through the serialized queue. */
        const serialize = attempt !== 0;
        if (await write(EpdCmd.FONT_STORE, payload, false, false, serialize)) {
          return true;
        }
        if (attempt < 2) {
          await new Promise((resolve) => setTimeout(resolve, 20 * (attempt + 1)));
        }
      }
      return false;
    },

    requirePackMtu() {
      // BEGIN is 34 bytes including the command byte; MTU=20 cannot carry it.
      const mtu = typeof currentMtuValue === 'function' ? currentMtuValue() : 20;
      if (mtu < 34) throw new Error('字库更新需要至少 34 字节的 BLE 数据长度，请等待 MTU 协商后重试');
    },

    async uploadPackStreamNow() {
      const session = this.session;
      const pack = await this.buildPack();
      this.assertSession(session);
      this.requirePackMtu();
      if (this.skipUnchanged(pack)) return;
      const packCrc32 = this.crc32(pack.data);
      const beginPayload = [OP_PACK_BEGIN,
        ...this.u32(pack.data.length), ...this.u32(packCrc32)];
      pack.slots.forEach((slot) => {
        beginPayload.push(...this.u32(slot.length), ...this.u32(slot.crc32));
      });

      const progress = document.getElementById('fontPackProgress');
      const mtu = typeof currentMtuValue === 'function' ? currentMtuValue() : 20;
      const chunkSize = Math.max(8, Math.min(224, mtu - STREAM_PACKET_OVERHEAD));
      let sequence = 0;
      let offset = 0;
      let windowSize = STREAM_WINDOW_SIZE;
      let recoveryAttempts = 0;
      let started = false;
      const startedAt = Date.now();

      if (progress) progress.value = 0;
      try {
        started = true;
        await this.requestAck(OP_PACK_BEGIN, 0xFF, beginPayload);

        while (offset < pack.data.length) {
          const windowStartSequence = sequence;
          const windowStartOffset = offset;
          const packetsToSend = Math.min(
            windowSize,
            Math.ceil((pack.data.length - offset) / chunkSize),
          );

          const packets = [];
          let packetSequence = sequence;
          let packetOffset = offset;
          for (let packet = 0; packet < packetsToSend; packet++) {
            const chunk = pack.data.slice(packetOffset,
              Math.min(packetOffset + chunkSize, pack.data.length));
            packets.push(this.makeStreamPayload(packetSequence, packetOffset, chunk));
            packetSequence++;
            packetOffset += chunk.length;
          }

          const sentEndSequence = packetSequence;
          const sentEndOffset = packetOffset;
          const ackPromise = this.waitStreamAck(
            windowStartSequence,
            windowStartOffset,
            sentEndSequence,
            sentEndOffset,
          );
          ackPromise.catch(() => {});

          /* Keep packets in strict sequence.  Concurrent Web Bluetooth writes
           * can complete out of order when one direct write falls back to the
           * serialized queue, which makes the MCU report a false sequence
           * error.  The window still batches ACKs; only packet submission is
           * serialized for deterministic ordering. */
          for (const payload of packets) {
            this.assertSession(session);
            if (!await this.sendStreamPacket(payload)) {
              throw new Error('字库高速数据包发送失败');
            }
          }
          sequence = packetSequence;
          offset = packetOffset;

          let response;
          try {
            response = await ackPromise;
            this.assertSession(session);
          } catch (error) {
            this.assertSession(session);
            if (this.streamAckWaiter) {
              const waiter = this.streamAckWaiter;
              clearTimeout(waiter.timer);
              this.streamAckWaiter = null;
              waiter.reject(error);
            }
            // Repeating the same window is safe: the device acknowledges
            // duplicates at its cumulative offset without programming twice.
            if (error.message === '字库数据窗口响应超时' && ++recoveryAttempts <= 3) {
              sequence = windowStartSequence;
              offset = windowStartOffset;
              continue;
            }
            throw error;
          }

          if (response.nextSequence < windowStartSequence ||
              response.nextSequence > sentEndSequence ||
              response.nextOffset < windowStartOffset ||
              response.nextOffset > sentEndOffset ||
              response.nextOffset > pack.data.length ||
              response.nextOffset !== Math.min(pack.data.length,
                windowStartOffset + (response.nextSequence - windowStartSequence) * chunkSize)) {
            throw new Error('设备返回了无效的字库累计 ACK');
          }

          if (response.status !== 0 || response.nextOffset === windowStartOffset) {
            /* CRC/ordering/FIFO errors identify the device's cumulative
             * point.  Rewind and retry from there instead of abandoning the
             * complete package.  A Flash I/O error remains fatal. */
            if (response.status === 0 || response.status === 1 || response.status === 2 || response.status === 3) {
              recoveryAttempts++;
              if (recoveryAttempts > 5) {
                throw new Error(`字库数据重试次数过多（状态 ${response.status}）`);
              }
              sequence = response.nextSequence;
              offset = response.nextOffset;
              windowSize = 1;
              if (response.status === 2) {
                await new Promise((resolve) => setTimeout(resolve, 50));
              }
              continue;
            }
            throw new Error(`设备拒绝字库数据（状态 ${response.status}）`);
          }

          /* If an ACK was lost, the device may acknowledge only part of the
           * window.  Rewind to its cumulative point and resend the remainder. */
          recoveryAttempts = 0;
          sequence = response.nextSequence;
          offset = response.nextOffset;
          if (response.credit > 0) {
            windowSize = Math.max(1, Math.min(STREAM_WINDOW_SIZE, response.credit));
          } else {
            windowSize = 1;
          }
          if (progress) {
            progress.value = pack.data.length === 0 ? 100 :
              Math.round((offset * 100) / pack.data.length);
          }
        }

        await this.requestAck(OP_PACK_COMMIT, 0xFF, [OP_PACK_COMMIT]);
        if (progress) progress.value = 100;
        const elapsed = Math.max(1, Date.now() - startedAt);
        const speed = (pack.data.length * 1000 / elapsed / 1024).toFixed(1);
        addLog(`字库全量更新完成（${pack.data.length} 字节，${speed} KB/s）`);
        try {
          await this.listNow();
        } catch (error) {
          addLog(`字库列表刷新失败（更新已完成）: ${error.message}`);
        }
      } catch (error) {
        if (this.streamAckWaiter) {
          const waiter = this.streamAckWaiter;
          clearTimeout(waiter.timer);
          this.streamAckWaiter = null;
          waiter.reject(error);
        }
        if (started && session === this.session && gattServer && gattServer.connected) {
          try {
            await this.requestAck(OP_PACK_ABORT, 0xFF, [OP_PACK_ABORT]);
          } catch (_) {
            /* Best effort: disconnect handler also aborts the device session. */
          }
        }
        throw error;
      }
    },

    async uploadPackNow() {
      if (typeof appVersion === 'number' && appVersion >= 0x33) {
        throw new Error('当前固件使用分区资源，请使用外置 Flash 一键烧录更新 fonts.ers');
      }
      if (typeof appVersion === 'number' && appVersion >= 0x2E) {
        return this.uploadPackStreamNow();
      }
      return this.uploadPackLegacyNow();
    },

    uploadPack() {
      return this.enqueue(() => this.uploadPackNow());
    },

    renderSlot(slot) {
      const item = this.slots[slot];
      if (!item) return;
      const state = document.getElementById(`fontState${slot}`);
      if (state) {
        state.textContent = item.present ? `已安装 ${item.length} 字节` : '未安装（占位符）';
      }
    },

    async refresh() {
      if (typeof appVersion === 'number' && appVersion >= 0x33) {
        this.setUiBusy(false);
        SLOT_NAMES.forEach((_, slot) => {
          const state = document.getElementById(`fontState${slot}`);
          if (state) state.textContent = '由字体资源分区管理';
        });
        return true;
      }
      try {
        await this.list();
        return true;
      } catch (error) {
        addLog(`读取外置字库失败: ${error.message}`);
        return false;
      }
    },

    init() {
      if (this.initialized) return;
      this.initialized = true;
      const refreshButton = document.getElementById('fontPackRefresh');
      if (refreshButton) {
        refreshButton.addEventListener('click', async () => {
          await this.refresh();
        });
      }
      const updateButton = document.getElementById('fontPackUpdate');
      if (updateButton) {
        updateButton.addEventListener('click', async () => {
          try {
            await this.uploadPack();
          } catch (error) {
            addLog(`字库全量更新失败: ${error.message}`);
          }
        });
      }
    },
  };

  window.FontStore = FontStore;
})();
