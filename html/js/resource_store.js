(function (root) {
  'use strict';
  const u32 = (b, at) => new DataView(b.buffer, b.byteOffset, b.byteLength).getUint32(at, true);
  const CAPACITY = 524288;
  // LEAVE resumes synchronous panel work in firmware main. Its ACK may arrive
  // before a full refresh, so later requests can also wait behind that frame.
  const REFRESH_RETRY_MS = 30000, REFRESH_WINDOW_MS = 90000;
  const INSTALL_ORDER = [1, 5, 6, 7, 2, 3, 9];
  const STATUS_NAMES = ['有效', '格式错误', '正在更新', '校验失败', '未安装或未提交', '读写失败', '版本不兼容', '写入受保护'];
  function statusName(code) { return STATUS_NAMES[code] || `未知状态 ${code}`; }
  function put32(b, at, value) { new DataView(b.buffer).setUint32(at, value, true); }
  function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
      crc ^= byte;
      for (let i = 0; i < 8; ++i) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
  }
  function parseHeader(bytes, partition) {
    if (!(bytes instanceof Uint8Array) || bytes.length < 32) throw new Error('资源包头长度错误');
    if (!partition) partition = { id: u32(bytes, 4) & 0xffff, size: CAPACITY };
    if (u32(bytes, 0) !== 0x31535245 || u32(bytes, 28) !== 0xa55ac33c) throw new Error('未安装或未提交');
    if (u32(bytes, 4) !== ((1 << 16) | partition.id) || u32(bytes, 20) !== CAPACITY) throw new Error('资源格式或分区 ID 不兼容');
    if (crc32(bytes.subarray(0, 24)) !== u32(bytes, 24)) throw new Error('资源包头 CRC 错误');
    const length = u32(bytes, 12);
    if (!length || length > partition.size - 32) throw new Error('资源包超出分区容量');
    return { id: partition.id, revision: u32(bytes, 8), length, crc: u32(bytes, 16) };
  }
  function parseTable(pack) {
    const b = pack.payload;
    if (pack.id !== 1 || !b.length || b.length % 16 || b.length > 32 * 16) throw new Error('分区表长度或 ID 错误');
    const view = new DataView(b.buffer, b.byteOffset, b.byteLength), entries = [];
    for (let at = 0; at < b.length; at += 16) {
      const p = { id: view.getUint16(at, true), type: view.getUint16(at + 2, true),
        offset: u32(b, at + 4), size: u32(b, at + 8), schema: view.getUint16(at + 12, true), flags: view.getUint16(at + 14, true) };
      if (!p.id || !p.type || p.schema !== 1 || p.flags || !p.size || p.offset % 4096 || p.size % 4096 || p.offset + p.size > CAPACITY ||
          entries.some(q => q.id === p.id || (p.offset < q.offset + q.size && q.offset < p.offset + p.size))) throw new Error('分区表包含重复、重叠、越界或不兼容条目');
      entries.push(p);
    }
    if (!entries.some(p => p.id === 1 && p.type === 1 && p.offset === 0 && p.size === 4096)) throw new Error('分区表定位条目错误');
    return entries;
  }
  function parseStatus(reply) {
    if (reply.length !== 16) throw new Error(`无法读取资源能力：${statusName(reply[3])}（旧固件可能不支持资源协议）`);
    if (!(reply[4] & 1) || reply[5] !== 1 || u32(reply, 12) !== CAPACITY) throw new Error('设备不支持外置 Flash 读写或资源 ABI/容量不兼容');
    return { code: reply[3], abi: reply[5], active: reply[6], entered: !!reply[7],
      hotReload: !!(reply[4] & 2), offline: !!(reply[4] & 4),
      largePackets: !!(reply[4] & 8), verify: !!(reply[4] & 16), buffered: !!(reply[4] & 32), erase: !!(reply[4] & 64),
      received: u32(reply, 8), capacity: u32(reply, 12) };
  }
  function validateTarget(pack, partitions) {
    const p = pack.id === 1 ? { id: 1, type: 1, offset: 0, size: 4096 } : partitions.find(p => p.id === pack.id);
    if (!p) throw new Error('设备分区表中不存在目标分区，请先安装分区表');
    if (p.type === 8 || p.id === 4) throw new Error('更新状态区和保留区不可上传');
    if (pack.payload.length > p.size - 32) throw new Error('资源包超出目标分区容量（含 32 字节包头）');
    if (pack.id === 1) parseTable(pack);
    return p;
  }
  function parseStamp(bytes) {
    if (!bytes.length) return null;
    if (bytes.length !== 12 || u32(bytes, 0) !== 0x54535245 || crc32(bytes.subarray(0, 8)) !== u32(bytes, 8))
      throw new Error('资源包编译时间戳错误');
    return u32(bytes, 4);
  }
  function verifyPayload(bytes, info) {
    const end = info.length + 32;
    if (bytes.length < end || crc32(bytes.subarray(32, end)) !== info.crc)
      throw new Error('资源包格式或 CRC 错误');
    return { id: info.id, revision: info.revision, crc: info.crc, builtAt: parseStamp(bytes.subarray(end)), payload: bytes.subarray(32, end) };
  }
  function parsePackage(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length < 33) throw new Error('资源包格式或 CRC 错误');
    const info = parseHeader(bytes);
    if (!info.id || info.id > 255) throw new Error('不支持的分区 ID');
    return verifyPayload(bytes, info);
  }
  function prepareBundle(files) {
    if (!files || !files.length) throw new Error('请选择包含 ERS 资源包的文件夹');
    const packages = [];
    const ids = new Set();
    for (const file of files) {
      const bytes = file.bytes;
      const pack = parsePackage(bytes);
      if (ids.has(pack.id)) throw new Error(`资源包中存在重复的分区 ID ${pack.id}`);
      ids.add(pack.id);
      packages.push({ name: file.name || `partition-${pack.id}.ers`, bytes, pack });
    }
    const missing = INSTALL_ORDER.filter(id => id !== 9 && !ids.has(id));
    if (missing.length) throw new Error(`资源包不完整，缺少：${missing.map(id => `分区 ${id}`).join('、')}`);
    const extra = packages.filter(item => !INSTALL_ORDER.includes(item.pack.id));
    if (extra.length) throw new Error(`一键烧录不支持分区：${extra.map(item => item.pack.id).join('、')}`);
    const table = packages.find(item => item.pack.id === 1);
    const partitions = parseTable(table.pack);
    for (const item of packages) validateTarget(item.pack, partitions);
    packages.sort((a, b) => INSTALL_ORDER.indexOf(a.pack.id) - INSTALL_ORDER.indexOf(b.pack.id));
    return { packages, partitions, total: packages.reduce((sum, item) => sum + item.pack.payload.length, 0) };
  }
  function installationPlan(bundle, statusCode, installedPartitions) {
    if (!bundle || !Array.isArray(bundle.packages)) throw new Error('资源目录尚未就绪');
    if (statusCode === 4) return bundle.packages.slice();
    if (statusCode !== 0) throw new Error(`分区表状态为“${statusName(statusCode)}”，不能按空白设备处理`);
    // Existing devices own their config and annotations. Factory records are
    // installed only alongside a new partition table on blank Flash.
    const packages = bundle.packages.filter(item => ![1, 2, 3, 9].includes(item.pack.id));
    for (const item of packages) validateTarget(item.pack, installedPartitions || []);
    return packages;
  }
  class ResourceClient {
    constructor(write, timeout = 5000, dataLength = () => 20) {
      this.write = write; this.timeout = timeout; this.token = 0;
      this.pending = null; this.busy = false; this.closed = false;
      this.dataLength = dataLength; this.capabilities = null;
      this.refreshUntil = 0;
    }
    packetLimit() {
      const n = Number(typeof this.dataLength === 'function' ? this.dataLength() : this.dataLength);
      return this.capabilities?.largePackets && Number.isInteger(n) ? Math.max(20, Math.min(244, n)) : 20;
    }
    receive(bytes) {
      const p = this.pending;
      if (p && bytes.length >= 4 && bytes[0] === 0xa7 && bytes[1] === p.op && bytes[2] === p.token) {
        this.pending = null; p.resolve(Uint8Array.from(bytes));
      }
    }
    disconnect() {
      this.closed = true;
      if (this.pending) this.pending.reject(new Error('蓝牙已断开；重新连接后重新上传'));
      this.pending = null;
    }
    async request(op, body = new Uint8Array(), allowStatus = false) {
      if (this.closed || this.busy || body.length > this.packetLimit() - 3) throw new Error('连接不可用或请求正在执行');
      this.busy = true;
      const packet = Uint8Array.of(0x26, op, this.token++ & 255, ...body);
      const activation = op === 7 || op === 8;
      const timeout = activation || Date.now() < this.refreshUntil
        ? Math.max(this.timeout, REFRESH_RETRY_MS) : this.timeout;
      const action = op === 7 ? '应用资源' : op === 8 ? '应用资源并断开连接'
        : op === 0 ? '读取资源状态' : `子命令 ${op}`;
      try {
        for (let attempt = 0; attempt < 3; ++attempt) {
          let timer, acknowledged;
          try {
            const answer = new Promise((resolve, reject) => {
              this.pending = { op, token: packet[2], resolve: reply => { acknowledged = reply; resolve(reply); }, reject };
              timer = setTimeout(() => reject(new Error(`资源请求超时（${action}）`)), timeout);
            });
            const written = Promise.resolve().then(() => this.write(packet)).catch(error => {
              // RUN_OFFLINE may disconnect GATT after its successful application ACK.
              if (op !== 8 || !acknowledged || acknowledged[3] !== 0) throw error;
            });
            const result = await Promise.all([answer, written]);
            const reply = result[0];
            if (activation && reply[3] === 0) this.refreshUntil = Date.now() + REFRESH_WINDOW_MS;
            if (reply[3] !== 0 && !allowStatus) throw Object.assign(new Error(`设备返回：${statusName(reply[3])}`), { device: true, code: reply[3] });
            return reply;
          } catch (error) {
            if (this.closed || error.device || attempt === 2) throw error;
          } finally { clearTimeout(timer); this.pending = null; }
        }
      } finally { this.busy = false; }
    }
    async status() {
      const reply = await this.request(0, new Uint8Array(), true);
      this.capabilities = parseStatus(reply);
      return reply;
    }
    async inspect(partition) {
      if (!this.capabilities?.verify) return null;
      const reply = await this.request(9, Uint8Array.of(partition), true);
      if (reply[3] !== 0) {
        if ([1, 3, 4, 6].includes(reply[3])) return { code: reply[3] };
        throw new Error(`资源校验失败：${statusName(reply[3])}`);
      }
      if (reply.length !== 16) throw new Error('资源校验响应长度错误');
      return { code: 0, revision: u32(reply, 4), length: u32(reply, 8), crc: u32(reply, 12) };
    }
    async eraseSector(id, offset) {
      if (!this.capabilities?.erase) throw new Error('删除功能需要固件 0x3F 或更新版本');
      if (!Number.isInteger(id) || id < 0 || id > 65535 || !Number.isInteger(offset) ||
          offset < 0 || offset >= CAPACITY || offset % 4096) throw new Error('删除范围错误');
      const body = new Uint8Array(6);
      new DataView(body.buffer).setUint16(0, id, true); put32(body, 2, offset);
      return this.request(11, body);
    }
    leave() { return this.request(7); }
    runOffline() { return this.request(8); }
    async read(address, length) {
      if (!Number.isInteger(address) || !Number.isInteger(length) || address < 0 ||
          length < 1 || length > this.packetLimit() - 4 || address + length > 524288) throw new Error('读取范围错误');
      const body = new Uint8Array(5);
      put32(body, 0, address); body[4] = length;
      const reply = await this.request(1, body);
      if (reply.length !== length + 4) throw new Error('读取响应长度错误');
      return reply.subarray(4);
    }
    async readRange(address, length, progress = () => {}) {
      if (!Number.isInteger(address) || !Number.isInteger(length) || address < 0 || length < 1 || address + length > CAPACITY) throw new Error('读取范围错误');
      const data = new Uint8Array(length);
      let reported = 0;
      for (let at = 0; at < data.length;) {
        const n = Math.min(this.packetLimit() - 4, data.length - at);
        data.set(await this.read(address + at, n), at);
        at += n;
        if (at - reported >= 256 || at === length) { progress(at, data.length); reported = at; }
      }
      return data;
    }
    async readPackage(partition, progress = () => {}) {
      const header = await this.readRange(partition.offset, 32);
      const info = parseHeader(header, partition);
      const bytes = new Uint8Array(32 + info.length);
      bytes.set(header); bytes.set(await this.readRange(partition.offset + 32, info.length, progress), 32);
      verifyPayload(bytes, info);
      return bytes;
    }
    async partitions() {
      const bytes = await this.readPackage({ id: 1, offset: 0, size: 4096 });
      return parseTable({ id: 1, payload: bytes.subarray(32) });
    }
    async upload(bytes, progress = () => {}, knownPartitions = null) {
      const pack = parsePackage(bytes);
      const status = parseStatus(await this.status());
      if (status.active) throw new Error('设备正在更新，请先终止未完成事务');
      validateTarget(pack, pack.id === 1 ? [] : knownPartitions || await this.partitions());
      await this.request(6, Uint8Array.of(77, 73, 71, 49));
      await this.request(5);
      const begin = new Uint8Array(13);
      begin[0] = pack.id;
      put32(begin, 1, pack.payload.length); put32(begin, 5, pack.crc); put32(begin, 9, pack.revision);
      await this.request(2, begin);
      const buffered = status.buffered;
      let durable = 0;
      for (let at = 0; at < pack.payload.length;) {
        const part = pack.payload.subarray(at, at + this.packetLimit() - 7);
        const body = new Uint8Array(4 + part.length);
        put32(body, 0, at); body.set(part, 4);
        const reply = await this.request(buffered ? 10 : 3, body);
        at += part.length;
        if (buffered) {
          if (reply.length !== 12 || u32(reply, 4) !== at || u32(reply, 8) < durable || u32(reply, 8) > at)
            throw new Error('资源写入进度确认错误');
          durable = u32(reply, 8);
        } else durable = at;
        progress(at, pack.payload.length, durable);
      }
      if (durable !== pack.payload.length) throw new Error('资源尾页尚未写入 Flash');
      await this.request(4);
    }
  }
  const api = { ResourceClient, parsePackage, parseHeader, parseTable, parseStatus, validateTarget, prepareBundle, installationPlan, statusName, crc32 };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.EPDResourceStore = api;
})(globalThis);
