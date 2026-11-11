import { ble, connection, constant } from '@kit.ConnectivityKit';
import { BusinessError } from '@kit.BasicServicesKit';

export const EPD_SERVICE_UUID: string = '62750001-d828-918d-fb46-b6c11c675aec';
export const EPD_DATA_UUID: string = '62750002-d828-918d-fb46-b6c11c675aec';
const EPD_CMD_LINK_STATE: number = 0x70;
const EPD_LINK_TRANSFER: number = 0x02;
export const DFU_SERVICE_UUID: string = '0000fe59-0000-1000-8000-00805f9b34fb';
export const DFU_SERVICE_ALT: string = '00001530-1212-efde-1523-785feabcd123';
export const DFU_CONTROL_UUID: string = '8ec90001-f315-4f60-9fb8-838830daea50';
export const DFU_PACKET_UUID: string = '8ec90002-f315-4f60-9fb8-838830daea50';
export const DFU_BUTTONLESS_UUID: string = '8ec90003-f315-4f60-9fb8-838830daea50';
export const DFU_ADV_NAME: string = 'DfuTarg';

const OP_CREATE: number = 0x01;
const OP_SET_PRN: number = 0x02;
const OP_CHECKSUM: number = 0x03;
const OP_EXECUTE: number = 0x04;
const OP_SELECT: number = 0x06;
const OP_RESPONSE: number = 0x60;
const BUTTONLESS_RESPONSE: number = 0x20;
const RESULT_SUCCESS: number = 0x01;
const RESULT_EXT_ERROR: number = 0x0B;
const OBJECT_MAX: number = 4096;

export class DfuScanResult {
  deviceId: string = '';
  name: string = '';
  rssi: number = 0;
}

class SelectInfo {
  maxSize: number = 0;
  offset: number = 0;
  crc: number = 0;
}

class CrcInfo {
  offset: number = 0;
  crc: number = 0;
}

function crc32Bytes(data: Uint8Array, start: number, end: number): number {
  let crc: number = 0xFFFFFFFF;
  for (let i: number = start; i < end; i++) {
    crc ^= data[i];
    for (let j: number = 0; j < 8; j++) {
      crc = (crc >>> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

function u32le(v: number): number[] {
  return [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF];
}

function le32(d: Uint8Array, off: number): number {
  return (d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | (d[off + 3] << 24)) >>> 0;
}

function resultName(code: number): string {
  switch (code) {
    case 0x01: return 'SUCCESS';
    case 0x02: return 'OP_CODE_NOT_SUPPORTED';
    case 0x03: return 'INVALID_PARAMETER';
    case 0x04: return 'INSUFFICIENT_RESOURCES';
    case 0x05: return 'INVALID_OBJECT';
    case 0x07: return 'UNSUPPORTED_TYPE';
    case 0x08: return 'OPERATION_NOT_PERMITTED';
    case 0x0A: return 'OPERATION_FAILED';
    case 0x0B: return 'EXT_ERROR';
    default: return 'UNKNOWN';
  }
}

export class DfuClient {
  private device: ble.GattClientDevice | null = null;
  private control: ble.BLECharacteristic | null = null;
  private packet: ble.BLECharacteristic | null = null;
  private buttonless: ble.BLECharacteristic | null = null;
  private epdData: ble.BLECharacteristic | null = null;
  private queue: Uint8Array[] = [];
  private waiter: ((d: Uint8Array) => void) | null = null;
  private indicationQueue: Uint8Array[] = [];
  private indicationWaiter: ((d: Uint8Array | null) => void) | null = null;
  private scanCb: ((r: Array<ble.ScanResult>) => void) | null = null;
  private connectedWaiter: (() => void) | null = null;
  private mtuPayload: number = 20;
  private disconnected: boolean = false;
  public onLog: (m: string) => void = (m: string) => {};
  public onProgress: (pct: number, text: string) => void = (pct: number, text: string) => {};
  public onDisconnected: () => void = () => {};

  private delay(ms: number): Promise<void> {
    return new Promise<void>((resolve: () => void) => {
      setTimeout(() => {
        resolve();
      }, ms);
    });
  }

  async scan(serviceUuid: string, timeoutMs: number, name: string = ''): Promise<DfuScanResult[]> {
    const found: Map<string, DfuScanResult> = new Map<string, DfuScanResult>();
    const cb = (results: Array<ble.ScanResult>): void => {
      for (let i: number = 0; i < results.length; i++) {
        const r: ble.ScanResult = results[i];
        const item = new DfuScanResult();
        item.deviceId = r.deviceId;
        item.name = r.deviceName === undefined ? '' : r.deviceName;
        item.rssi = r.rssi;
        found.set(r.deviceId, item);
      }
    };
    this.scanCb = cb;
    const filters: ble.ScanFilter[] = [];
    if (serviceUuid.length > 0) {
      filters.push({ serviceUuid: serviceUuid });
    }
    if (name.length > 0) {
      filters.push({ name: name });
    }
    if (filters.length === 0) {
      this.scanCb = null;
      throw new Error('扫描需要至少一个过滤条件');
    }
    try {
      const scanOptions: ble.ScanOptions = { dutyMode: ble.ScanDuty.SCAN_MODE_LOW_LATENCY };
      ble.on('BLEDeviceFind', cb);
      ble.startBLEScan(filters, scanOptions);
    } catch (err) {
      this.onLog('扫描启动失败: ' + (err as Error).message);
      return [];
    }
    await this.delay(timeoutMs);
    try {
      ble.stopBLEScan();
    } catch (err) {
    }
    if (this.scanCb !== null) {
      try {
        ble.off('BLEDeviceFind', this.scanCb);
      } catch (err) {
      }
      this.scanCb = null;
    }
    const list: DfuScanResult[] = [];
    found.forEach((v: DfuScanResult) => {
      list.push(v);
    });
    list.sort((a: DfuScanResult, b: DfuScanResult) => b.rssi - a.rssi);
    return list;
  }

  private withTimeout<T>(p: Promise<T>, ms: number): Promise<T> {
    return new Promise<T>((resolve: (v: T) => void, reject: (e: Error) => void) => {
      const timer = setTimeout(() => {
        reject(new Error('服务发现超时'));
      }, ms);
      p.then((v: T) => {
        clearTimeout(timer);
        resolve(v);
      }).catch((e: Error) => {
        clearTimeout(timer);
        reject(e);
      });
    });
  }

  private waitConnected(ms: number): Promise<void> {
    if (this.device === null) {
      return Promise.reject(new Error('未创建 GATT 客户端'));
    }
    return new Promise<void>((resolve: () => void, reject: (e: Error) => void) => {
      const timer = setTimeout(() => {
        this.connectedWaiter = null;
        reject(new Error('连接超时'));
      }, ms);
      this.connectedWaiter = () => {
        clearTimeout(timer);
        resolve();
      };
    });
  }

  private async discover(dev: ble.GattClientDevice): Promise<void> {
    for (let attempt: number = 0; attempt < 3; attempt++) {
      try {
        const services: Array<ble.GattService> = await this.withTimeout<Array<ble.GattService>>(dev.getServices(), 8000);
        this.parseServices(services);
        if (this.control !== null || this.buttonless !== null) {
          return;
        }
      } catch (err) {
        this.onLog('服务发现失败(' + (attempt + 1) + '/3): ' + (err as Error).message);
      }
      await this.delay(600);
    }
    throw new Error('DFU 服务发现失败，请重试（设备仍在应用模式）');
  }

  private parseServices(services: Array<ble.GattService>): void {
    for (let i: number = 0; i < services.length; i++) {
      const svc: ble.GattService = services[i];
      const su: string = svc.serviceUuid.toLowerCase();
      if (su === EPD_SERVICE_UUID) {
        for (let k: number = 0; k < svc.characteristics.length; k++) {
          const ech: ble.BLECharacteristic = svc.characteristics[k];
          if (ech.characteristicUuid.toLowerCase() === EPD_DATA_UUID) {
            this.epdData = ech;
          }
        }
        continue;
      }
      if (su !== DFU_SERVICE_UUID && su !== DFU_SERVICE_ALT) {
        continue;
      }
      for (let j: number = 0; j < svc.characteristics.length; j++) {
        const ch: ble.BLECharacteristic = svc.characteristics[j];
        const cu: string = ch.characteristicUuid.toLowerCase();
        const props = ch.properties;
        const canWrite: boolean = props !== undefined && props.write === true;
        const canWnr: boolean = props !== undefined && props.writeNoResponse === true;
        const canIndicate: boolean = props !== undefined && props.indicate === true;
        this.onLog('DFU 特征 ' + cu + ' write=' + canWrite + ' wnr=' + canWnr + ' indicate=' + canIndicate);
        if (cu === DFU_CONTROL_UUID) {
          this.control = ch;
        } else if (cu === DFU_PACKET_UUID) {
          this.packet = ch;
        } else if (cu === DFU_BUTTONLESS_UUID) {
          this.buttonless = ch;
        }
      }
    }
  }

  async connect(deviceId: string): Promise<void> {
    const dev = ble.createGattClientDevice(deviceId);
    this.device = dev;
    this.control = null;
    this.packet = null;
    this.buttonless = null;
    this.epdData = null;
    this.queue = [];
    this.waiter = null;
    this.indicationQueue = [];
    this.indicationWaiter = null;
    this.mtuPayload = 20;
    this.disconnected = false;
    dev.on('BLEConnectionStateChange', (s: ble.BLEConnectionChangeState) => {
      if (this.device !== dev) {
        return;
      }
      this.onLog('GATT 连接状态事件: ' + s.state);
      if (s.state === constant.ProfileConnectionState.STATE_CONNECTED) {
        if (this.connectedWaiter !== null) {
          const w = this.connectedWaiter;
          this.connectedWaiter = null;
          w();
        }
      } else if (s.state === constant.ProfileConnectionState.STATE_DISCONNECTED) {
        this.disconnected = true;
        this.control = null;
        this.packet = null;
        if (this.indicationWaiter !== null) {
          const iw = this.indicationWaiter;
          this.indicationWaiter = null;
          iw(null);
        }
        this.onDisconnected();
      }
    });
    const connected = this.waitConnected(75000);
    dev.connect();
    await connected;
    await this.discover(dev);
    if (this.control !== null) {
      dev.on('BLECharacteristicChange', (ch: ble.BLECharacteristic) => {
        if (ch.characteristicUuid.toLowerCase() === DFU_CONTROL_UUID) {
          this.onNotify(new Uint8Array(ch.characteristicValue));
        }
      });
      await dev.setCharacteristicChangeNotification(this.control, true);
      try {
        dev.on('BLEMtuChange', (mtu: number) => {
          this.mtuPayload = Math.max(20, Math.min(244, mtu - 3));
          this.onLog('MTU 协商为 ' + mtu + '，DFU 分片 ' + this.mtuPayload + ' 字节');
        });
        dev.setBLEMtuSize(247);
        await this.delay(500);
      } catch (err) {
        this.onLog('MTU 协商失败，使用 20 字节分片');
      }
    } else if (this.buttonless !== null) {
      dev.on('BLECharacteristicChange', (ch: ble.BLECharacteristic) => {
        if (ch.characteristicUuid.toLowerCase() === DFU_BUTTONLESS_UUID) {
          this.onIndication(new Uint8Array(ch.characteristicValue));
        }
      });
    }
  }

  hasDfuService(): boolean {
    return this.control !== null && this.packet !== null;
  }

  hasButtonless(): boolean {
    return this.buttonless !== null;
  }

  async pair(deviceId: string): Promise<boolean> {
    try {
      const st = connection.getPairState(deviceId);
      this.onLog('配对前状态: ' + st);
      if (st === connection.BondState.BOND_STATE_BONDED) {
        return true;
      }
    } catch (e) {
    }
    return new Promise<boolean>((resolve: (ok: boolean) => void) => {
      let done: boolean = false;
      let timer: number = -1;
      const handler = (data: connection.BondStateParam): void => {
        if (data.deviceId === deviceId) {
          this.onLog('配对状态: ' + data.state);
          if (data.state === connection.BondState.BOND_STATE_BONDED) {
            finish(true);
          }
        }
      };
      const finish = (ok: boolean): void => {
        if (done) {
          return;
        }
        done = true;
        if (timer >= 0) {
          clearTimeout(timer);
        }
        try {
          connection.off('bondStateChange', handler);
        } catch (e) {
        }
        resolve(ok);
      };
      connection.on('bondStateChange', handler);
      timer = setTimeout(() => {
        this.onLog('配对超时');
        finish(false);
      }, 10000);
      connection.pairDevice(deviceId).then(() => {
        this.onLog('配对请求已返回');
        setTimeout(() => {
          try {
            if (connection.getPairState(deviceId) === connection.BondState.BOND_STATE_BONDED) {
              finish(true);
            }
          } catch (e) {
          }
        }, 800);
      }).catch((err: Error) => {
        this.onLog('配对请求失败: ' + err.message);
        finish(false);
      });
    });
  }

  private onIndication(d: Uint8Array): void {
    let hex: string = '';
    for (let i: number = 0; i < d.length; i++) {
      const h: string = d[i].toString(16);
      hex += (h.length < 2 ? '0' + h : h) + ' ';
    }
    this.onLog('buttonless 事件 ' + d.length + ' 字节: ' + hex);
    // 忽略 CCCD 配置等非 DFU 响应，只接受 0x20 op result
    if (d.length < 3 || d[0] !== BUTTONLESS_RESPONSE) {
      return;
    }
    if (this.indicationWaiter !== null) {
      const w = this.indicationWaiter;
      this.indicationWaiter = null;
      w(d);
    } else {
      this.indicationQueue.push(d);
    }
  }

  private waitIndication(ms: number): Promise<Uint8Array | null> {
    if (this.indicationQueue.length > 0) {
      const v = this.indicationQueue.shift();
      return Promise.resolve(v === undefined ? null : v);
    }
    if (this.disconnected) {
      return Promise.resolve(null);
    }
    return new Promise<Uint8Array | null>((resolve: (v: Uint8Array | null) => void) => {
      const timer = setTimeout(() => {
        if (this.indicationWaiter !== null) {
          this.indicationWaiter = null;
          resolve(null);
        }
      }, ms);
      this.indicationWaiter = (d: Uint8Array | null) => {
        clearTimeout(timer);
        resolve(d);
      };
    });
  }

  // 固件升级前通知设备进入 TRANSFER 状态（0x70 0x02），语义与图片传输一致。
  private async sendTransferState(): Promise<void> {
    const dev = this.device;
    const ch = this.epdData;
    if (dev === null || ch === null) {
      return;
    }
    try {
      ch.characteristicValue = new Uint8Array([EPD_CMD_LINK_STATE, EPD_LINK_TRANSFER]).buffer as ArrayBuffer;
      await dev.writeCharacteristicValue(ch, ble.GattWriteType.WRITE);
      this.onLog('已通知设备：TRANSFER（固件升级）');
    } catch (e) {
      this.onLog('TRANSFER 状态发送失败（继续升级）: ' + (e as Error).message);
    }
  }

  async writeButtonless(): Promise<void> {
    const ch = this.buttonless;
    const dev = this.device;
    if (ch === null || dev === null) {
      throw new Error('设备未进入 DFU 服务');
    }
    await this.sendTransferState();
    // 官方 ble_dfu.c:95-110：未把 CCCD 配成 indication 时写入会被固件拒绝（2900099）
    try {
      await dev.setCharacteristicChangeIndication(ch, true);
      this.onLog('buttonless indication 已开启');
      await this.delay(200);
      this.indicationQueue = [];
    } catch (e) {
      throw new Error('开启 DFU indication 失败: ' + (e as Error).message);
    }
    ch.characteristicValue = new Uint8Array([0x01]).buffer as ArrayBuffer;
    try {
      await dev.writeCharacteristicValue(ch, ble.GattWriteType.WRITE);
      this.onLog('已写入 0x01（Enter Bootloader）');
    } catch (e) {
      const be = e as BusinessError;
      this.onLog('写入 0x01 返回错误: ' + be.code + ' ' + be.message);
      if (!this.disconnected) {
        throw new Error('写入 Enter Bootloader 失败: ' + be.message);
      }
    }
    const resp = await this.waitIndication(6000);
    if (resp === null) {
      this.onLog('未收到 indication 响应（连接已断开或超时），继续尝试扫描 DfuTarg');
      return;
    }
    if (resp.length < 3 || resp[0] !== BUTTONLESS_RESPONSE || resp[1] !== 0x01) {
      throw new Error('buttonless 响应格式错误');
    }
    if (resp[2] !== RESULT_SUCCESS) {
      throw new Error('设备拒绝进入 DFU（result=' + resultName(resp[2]) + '）');
    }
    this.onLog('设备已接受进入 DFU 请求（0x20 0x01 0x01），尚未确认 bootloader');
  }

  async waitForDeviceDisconnect(timeoutMs: number = 15000): Promise<void> {
    const started: number = Date.now();
    this.onLog('等待设备主动断开，最多 ' + timeoutMs + ' 毫秒');
    // Keep GATT alive so the stack can confirm the indication before the
    // peripheral finishes its buttonless transition and resets.
    while (!this.disconnected && Date.now() - started < timeoutMs) {
      await this.delay(50);
    }
    if (!this.disconnected) {
      throw new Error('设备在 ' + timeoutMs + ' 毫秒内未主动断开，尚未确认进入 bootloader');
    }
    this.onLog('已收到设备断开事件，等待 ' + (Date.now() - started) + ' 毫秒；仍需扫描确认 DFU');
  }

  private onNotify(d: Uint8Array): void {
    if (this.waiter !== null) {
      const w = this.waiter;
      this.waiter = null;
      w(d);
    } else {
      this.queue.push(d);
    }
  }

  private readResponse(timeoutMs: number): Promise<Uint8Array> {
    if (this.queue.length > 0) {
      const v = this.queue.shift();
      if (v !== undefined) {
        return Promise.resolve(v);
      }
    }
    return new Promise<Uint8Array>((resolve: (v: Uint8Array) => void, reject: (e: Error) => void) => {
      const timer = setTimeout(() => {
        if (this.waiter !== null) {
          this.waiter = null;
          reject(new Error('DFU 响应超时'));
        }
      }, timeoutMs);
      this.waiter = (d: Uint8Array) => {
        clearTimeout(timer);
        resolve(d);
      };
    });
  }

  private async command(payload: Uint8Array, opcode: number, expectLen: number, timeoutMs: number): Promise<Uint8Array> {
    const dev = this.device;
    const ch = this.control;
    if (dev === null || ch === null) {
      throw new Error('DFU 未连接');
    }
    ch.characteristicValue = payload.buffer as ArrayBuffer;
    await dev.writeCharacteristicValue(ch, ble.GattWriteType.WRITE);
    const resp = await this.readResponse(timeoutMs);
    if (resp.length < 3 || resp[0] !== OP_RESPONSE) {
      throw new Error('DFU 响应格式错误');
    }
    if (resp[1] !== opcode) {
      throw new Error('DFU 响应 opcode 不匹配: 0x' + resp[1].toString(16));
    }
    if (resp[2] !== RESULT_SUCCESS) {
      let detail: string = resultName(resp[2]);
      if (resp[2] === RESULT_EXT_ERROR && resp.length >= 4) {
        detail += '（扩展错误 0x' + resp[3].toString(16) + '）';
      }
      throw new Error('DFU 命令失败: opcode 0x' + opcode.toString(16) + ' result=' + detail);
    }
    const body = resp.slice(3);
    if (body.length < expectLen) {
      throw new Error('DFU 响应长度不足');
    }
    return body;
  }

  private async setPrn(n: number): Promise<void> {
    await this.command(new Uint8Array([OP_SET_PRN, n & 0xFF, (n >>> 8) & 0xFF]), OP_SET_PRN, 0, 6000);
  }

  private async select(type: number): Promise<SelectInfo> {
    const body = await this.command(new Uint8Array([OP_SELECT, type]), OP_SELECT, 12, 6000);
    const s = new SelectInfo();
    s.maxSize = le32(body, 0);
    s.offset = le32(body, 4);
    s.crc = le32(body, 8);
    return s;
  }

  private async create(type: number, size: number): Promise<void> {
    const s = u32le(size);
    await this.command(new Uint8Array([OP_CREATE, type, s[0], s[1], s[2], s[3]]), OP_CREATE, 0, 8000);
  }

  private async crcCmd(): Promise<CrcInfo> {
    const body = await this.command(new Uint8Array([OP_CHECKSUM]), OP_CHECKSUM, 8, 8000);
    const c = new CrcInfo();
    c.offset = le32(body, 0);
    c.crc = le32(body, 4);
    return c;
  }

  private async execute(): Promise<void> {
    await this.command(new Uint8Array([OP_EXECUTE]), OP_EXECUTE, 0, 20000);
  }

  private async executeOnce(isFinal: boolean): Promise<void> {
    try {
      if (isFinal) {
        // 固件在发送响应后立即复位，响应可能丢失；官方 nrf_dfu_req_handler.c:559-577
        await this.command(new Uint8Array([OP_EXECUTE]), OP_EXECUTE, 0, 6000);
      } else {
        await this.execute();
      }
    } catch (err) {
      if (isFinal && (this.disconnected || (err as Error).message.indexOf('超时') >= 0)) {
        this.onLog('最终 EXECUTE 未收到响应，按官方完成流程处理，随后以设备重启扫描确认');
        return;
      }
      throw err as Error;
    }
  }

  async abort(): Promise<void> {
    try {
      await this.command(new Uint8Array([0x0C]), 0x0C, 0, 3000);
    } catch (err) {
    }
  }

  private async writePacket(data: Uint8Array): Promise<void> {
    const dev = this.device;
    const ch = this.packet;
    if (dev === null || ch === null) {
      throw new Error('DFU 未连接');
    }
    ch.characteristicValue = data.buffer as ArrayBuffer;
    await dev.writeCharacteristicValue(ch, ble.GattWriteType.WRITE_NO_RESPONSE);
  }

  private async writeRange(bytes: Uint8Array, start: number, end: number): Promise<void> {
    let i: number = start;
    while (i < end) {
      const n: number = Math.min(this.mtuPayload, end - i);
      const part: Uint8Array = bytes.slice(i, i + n);
      await this.writePacket(part);
      i += n;
      await this.delay(2);
    }
  }

  private async sendChunk(type: number, bytes: Uint8Array, start: number, end: number,
    chunk: number, isFinal: boolean): Promise<void> {
    let from: number = start;
    for (let attempt: number = 0; attempt < 3; attempt++) {
      if (attempt > 0) {
        // 官方 DfuAbstractTransport.js:199-217：失败后回退到分片边界并重新 CREATE
        from = start - (start % chunk);
        this.onLog('对象 ' + end + ' 校验失败，回退到 ' + from + ' 重试（第 ' + (attempt + 1) + ' 次）');
        await this.create(type, end - from);
      }
      await this.writeRange(bytes, from, end);
      const c: CrcInfo = await this.crcCmd();
      const expectCrc: number = crc32Bytes(bytes, 0, end);
      if (c.offset === end && c.crc === expectCrc) {
        await this.executeOnce(isFinal);
        return;
      }
      this.onLog('CRC 不一致：offset ' + c.offset + '/' + end + '，crc 0x' +
        c.crc.toString(16) + ' ≠ 0x' + expectCrc.toString(16));
    }
    throw new Error('对象 ' + end + ' 校验失败（已重试 3 次）');
  }

  private async sendObject(type: number, bytes: Uint8Array, base: number, total: number): Promise<void> {
    let sel: SelectInfo = await this.select(type);
    let chunk: number = sel.maxSize > 0 ? sel.maxSize : OBJECT_MAX;
    if (chunk > OBJECT_MAX) {
      chunk = OBJECT_MAX;
    }
    this.onLog('对象类型 ' + type + '：max=' + chunk + ' offset=' + sel.offset + ' crc=0x' + sel.crc.toString(16));
    let pos: number = 0;
    if (sel.offset > 0) {
      if (sel.offset > bytes.length) {
        throw new Error('设备报告的传输进度超出固件长度');
      }
      const prefixCrc: number = crc32Bytes(bytes, 0, sel.offset);
      if (prefixCrc !== sel.crc) {
        throw new Error('检测到上次未完成的传输，但续传 CRC 校验失败。请断电重启墨水屏后重新升级。');
      }
      pos = sel.offset;
      this.onLog('断点续传：' + pos + ' / ' + bytes.length + ' 字节，CRC 匹配');
      if (pos === bytes.length) {
        this.onLog('该对象已传输完成，补发 EXECUTE');
        await this.executeOnce(type === 2 && base + bytes.length >= total);
        return;
      }
      if ((pos % chunk) !== 0) {
        const objStart: number = pos - (pos % chunk);
        const end: number = Math.min(bytes.length, objStart + chunk);
        this.onLog('续传当前未完成对象：' + pos + ' → ' + end);
        const isLast: boolean = (type === 2 && base + end >= total);
        await this.sendChunk(type, bytes, pos, end, chunk, isLast);
        pos = end;
      } else {
        this.onLog('断点在对象边界，先补发 EXECUTE（幂等）再重新读取进度');
        await this.executeOnce(false);
        sel = await this.select(type);
        if (sel.offset !== pos) {
          if (sel.offset > bytes.length || crc32Bytes(bytes, 0, sel.offset) !== sel.crc) {
            throw new Error('续传进度变化后 CRC 不一致，请断电重启墨水屏后重试');
          }
          pos = sel.offset;
        }
        if (pos === bytes.length) {
          await this.executeOnce(type === 2 && base + bytes.length >= total);
          return;
        }
      }
    }
    while (pos < bytes.length) {
      const end: number = Math.min(pos + chunk, bytes.length);
      const isFinal: boolean = type === 2 && end === bytes.length;
      await this.create(type, end - pos);
      await this.sendChunk(type, bytes, pos, end, chunk, isFinal);
      pos = end;
      this.onProgress((base + pos) / total, type === 1 ? '写入初始化包' : '写入固件');
    }
  }

  async run(dat: Uint8Array, bin: Uint8Array): Promise<void> {
    await this.setPrn(0);
    this.onLog('DFU 已就绪（PRN=0），开始传输');
    const total: number = dat.length + bin.length;
    await this.sendObject(1, dat, 0, total);
    await this.sendObject(2, bin, dat.length, total);
    this.onProgress(1, '传输完成');
  }

  disconnect(): void {
    const dev = this.device;
    const wasDisconnected: boolean = this.disconnected;
    this.device = null;
    this.control = null;
    this.packet = null;
    this.buttonless = null;
    this.epdData = null;
    this.waiter = null;
    this.queue = [];
    this.indicationWaiter = null;
    this.indicationQueue = [];
    this.disconnected = true;
    if (dev !== null) {
      if (!wasDisconnected) {
        this.onLog('主动断开 GATT 连接（清理）');
        try {
          dev.disconnect();
        } catch (err) {
        }
      }
      try {
        dev.close();
      } catch (err) {
      }
    }
  }
}
