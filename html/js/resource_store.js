(function (root) {
  'use strict';
  const u32 = (b, at) => new DataView(b.buffer, b.byteOffset, b.byteLength).getUint32(at, true);
  function put32(b, at, value) { new DataView(b.buffer).setUint32(at, value, true); }
  function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
      crc ^= byte;
      for (let i = 0; i < 8; ++i) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
  }
  function parsePackage(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length < 33 ||
        u32(bytes, 0) !== 0x31535245 || u32(bytes, 28) !== 0xa55ac33c ||
        (u32(bytes, 4) >>> 16) !== 1 || u32(bytes, 20) !== 524288 ||
        crc32(bytes.subarray(0, 24)) !== u32(bytes, 24) ||
        u32(bytes, 12) !== bytes.length - 32 ||
        crc32(bytes.subarray(32)) !== u32(bytes, 16)) throw new Error('资源包格式或 CRC 错误');
    const id = u32(bytes, 4) & 0xffff;
    if (!id || id > 255) throw new Error('不支持的分区 ID');
    return { id, revision: u32(bytes, 8), crc: u32(bytes, 16), payload: bytes.subarray(32) };
  }
  class ResourceClient {
    constructor(write, timeout = 5000) {
      this.write = write; this.timeout = timeout; this.token = 0;
      this.pending = null; this.busy = false; this.closed = false;
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
      if (this.closed || this.busy || body.length > 17) throw new Error('连接不可用或请求正在执行');
      this.busy = true;
      const packet = Uint8Array.of(0x26, op, this.token++ & 255, ...body);
      try {
        for (let attempt = 0; attempt < 3; ++attempt) {
          let timer;
          try {
            const answer = new Promise((resolve, reject) => {
              this.pending = { op, token: packet[2], resolve, reject };
              timer = setTimeout(() => reject(new Error('资源请求超时')), this.timeout);
            });
            const result = await Promise.all([answer, Promise.resolve().then(() => this.write(packet))]);
            const reply = result[0];
            if (reply[3] !== 0 && !allowStatus) throw Object.assign(new Error(`设备返回状态 ${reply[3]}`), { device: true });
            return reply;
          } catch (error) {
            if (this.closed || error.device || attempt === 2) throw error;
          } finally { clearTimeout(timer); this.pending = null; }
        }
      } finally { this.busy = false; }
    }
    status() { return this.request(0, new Uint8Array(), true); }
    async read(address, length) {
      if (!Number.isInteger(address) || !Number.isInteger(length) || address < 0 ||
          length < 1 || length > 16 || address + length > 524288) throw new Error('读取范围错误');
      const body = new Uint8Array(5);
      put32(body, 0, address); body[4] = length;
      const reply = await this.request(1, body);
      if (reply.length !== length + 4) throw new Error('读取响应长度错误');
      return reply.subarray(4);
    }
    async exportFlash(progress = () => {}) {
      const data = new Uint8Array(524288);
      for (let at = 0; at < data.length; at += 16) {
        data.set(await this.read(at, 16), at);
        if ((at + 16) % 4096 === 0) progress(at + 16, data.length);
      }
      return data;
    }
    async upload(bytes, progress = () => {}) {
      const pack = parsePackage(bytes);
      const status = await this.status();
      if (status.length !== 16 || status[4] !== 1) throw new Error('当前固件只支持读取；写入需要维护版本');
      await this.request(6, Uint8Array.of(77, 73, 71, 49));
      await this.request(5);
      const begin = new Uint8Array(13);
      begin[0] = pack.id;
      put32(begin, 1, pack.payload.length); put32(begin, 5, pack.crc); put32(begin, 9, pack.revision);
      await this.request(2, begin);
      for (let at = 0; at < pack.payload.length; at += 13) {
        const part = pack.payload.subarray(at, at + 13);
        const body = new Uint8Array(4 + part.length);
        put32(body, 0, at); body.set(part, 4);
        await this.request(3, body); progress(at + part.length, pack.payload.length);
      }
      await this.request(4);
    }
  }
  const api = { ResourceClient, parsePackage, crc32 };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.EPDResourceStore = api;
})(globalThis);
