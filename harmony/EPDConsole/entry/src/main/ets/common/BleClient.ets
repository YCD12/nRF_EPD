import { ble, constant } from '@kit.ConnectivityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import {
  Cmd, Rsp, LinkState, frame, parseStatus, TransferStatus,
  EPD_SERVICE_UUID, EPD_CHAR_UUID, EPD_VER_UUID
} from './Protocol';

const LINK_MIN_VERSION: number = 0x22;
const HEARTBEAT_MS: number = 15000;
const LINK_ACK_MS: number = 4000;
const HEARTBEAT_MAX_FAILS: number = 2;

export class DeviceItem {
  deviceId: string = '';
  name: string = '';
  rssi: number = 0;
}

export class ConnState {
  static readonly EXIT: string = 'EXIT';
  static readonly REQUEST: string = 'REQUEST';
  static readonly HOLD: string = 'HOLD';
}

export class BleClient {
  private device: ble.GattClientDevice | null = null;
  private dataChar: ble.BLECharacteristic | null = null;
  private versionChar: ble.BLECharacteristic | null = null;
  private statusWaiter: ((status: TransferStatus) => void) | null = null;
  private scanCallback: ((results: Array<ble.ScanResult>) => void) | null = null;
  private scanPhaseTimer: number = -1;
  private scanEndTimer: number = -1;
  private idleTimer: number = -1;
  private idleTimeoutMs: number = 120000;
  private connectRetries: number = 3;
  private closing: boolean = false;
  private linkWaiter: ((state: number) => void) | null = null;
  private heartbeatActive: boolean = false;
  private heartbeatFails: number = 0;
  private transferring: boolean = false;
  private protocolReady: boolean = false;
  public state: string = ConnState.EXIT;

  public appVersion: number = 0x15;
  public onLog: (msg: string) => void = (msg: string) => {};
  public onNotify: (data: Uint8Array) => void = (data: Uint8Array) => {};
  public onDisconnected: (reason: string) => void = (reason: string) => {};

  isConnected(): boolean {
    return this.device !== null && this.dataChar !== null;
  }

  private delay(ms: number): Promise<void> {
    return new Promise<void>((resolve: () => void) => {
      setTimeout(() => {
        resolve();
      }, ms);
    });
  }

  scanOnce(durationMs: number): Promise<DeviceItem[]> {
    return new Promise<DeviceItem[]>((resolve: (list: DeviceItem[]) => void) => {
      let latest: DeviceItem[] = [];
      this.startScan((list: DeviceItem[]) => {
        latest = list;
      }, durationMs);
      setTimeout(() => {
        resolve(latest);
      }, durationMs + 400);
    });
  }

  startScan(onFound: (list: DeviceItem[]) => void, durationMs: number): void {
    const found: Map<string, DeviceItem> = new Map<string, DeviceItem>();
    this.stopScan();
    const cb = (results: Array<ble.ScanResult>): void => {
      for (let i: number = 0; i < results.length; i++) {
        const r: ble.ScanResult = results[i];
        const item = new DeviceItem();
        item.deviceId = r.deviceId;
        item.name = r.deviceName === undefined ? '' : r.deviceName;
        item.rssi = r.rssi;
        found.set(r.deviceId, item);
      }
      const list: DeviceItem[] = [];
      found.forEach((v: DeviceItem) => {
        list.push(v);
      });
      list.sort((a: DeviceItem, b: DeviceItem) => b.rssi - a.rssi);
      onFound(list);
    };
    this.scanCallback = cb;
    const scanOptions: ble.ScanOptions = { dutyMode: ble.ScanDuty.SCAN_MODE_LOW_LATENCY };
    try {
      ble.on('BLEDeviceFind', cb);
      // 两阶段扫描：先用 EPD 服务/名称过滤（HarmonyOS 7 对空过滤器支持不稳定，
      // 且固件把 Service UUID 放在 scan response），再全量扫描兜底。
      const phase1: number = Math.floor(durationMs / 2);
      const epdFilters: ble.ScanFilter[] = [
        { serviceUuid: EPD_SERVICE_UUID },
        { name: 'NRF_EPD' }
      ];
      try {
        ble.startBLEScan(epdFilters, scanOptions);
        this.onLog('扫描阶段1：EPD 服务/名称过滤');
      } catch (e1) {
        this.onLog('服务过滤扫描不支持，阶段1改为全量');
        ble.startBLEScan([{}], scanOptions);
      }
      this.scanPhaseTimer = setTimeout(() => {
        try {
          ble.stopBLEScan();
        } catch (e2) {
        }
        try {
          ble.startBLEScan([{}], scanOptions);
          this.onLog('扫描阶段2：全量扫描');
        } catch (e3) {
        }
      }, phase1);
      this.scanEndTimer = setTimeout(() => {
        this.stopScan();
      }, durationMs);
    } catch (err) {
      const e = err as BusinessError;
      this.onLog('扫描失败: ' + e.message);
    }
  }

  stopScan(): void {
    if (this.scanPhaseTimer >= 0) {
      clearTimeout(this.scanPhaseTimer);
      this.scanPhaseTimer = -1;
    }
    if (this.scanEndTimer >= 0) {
      clearTimeout(this.scanEndTimer);
      this.scanEndTimer = -1;
    }
    try {
      ble.stopBLEScan();
    } catch (err) {
    }
    try {
      if (this.scanCallback !== null) {
        ble.off('BLEDeviceFind', this.scanCallback);
        this.scanCallback = null;
      }
    } catch (err) {
    }
  }

  private async retry<T>(fn: () => Promise<T>, tries: number, delayMs: number, label: string): Promise<T> {
    let lastErr: Error = new Error('unknown');
    for (let i: number = 0; i < tries; i++) {
      try {
        return await fn();
      } catch (err) {
        const e = err as BusinessError;
        lastErr = e;
        this.onLog(label + ' 第' + (i + 1) + '次失败: ' + e.code + ' ' + e.message);
        await this.delay(delayMs);
      }
    }
    throw lastErr;
  }

  private withTimeout<T>(promise: Promise<T>, timeoutMs: number): Promise<T> {
    return new Promise<T>((resolve: (v: T) => void, reject: (e: Error) => void) => {
      const timer = setTimeout(() => {
        reject(new Error('操作超时(' + timeoutMs + 'ms)'));
      }, timeoutMs);
      promise.then((v: T) => {
        clearTimeout(timer);
        resolve(v);
      }).catch((e: Error) => {
        clearTimeout(timer);
        reject(e);
      });
    });
  }

  private waitConnected(dev: ble.GattClientDevice, timeoutMs: number): Promise<void> {
    return new Promise<void>((resolve: () => void, reject: (e: Error) => void) => {
      let settled: boolean = false;
      const timer = setTimeout(() => {
        if (!settled) {
          settled = true;
          reject(new Error('连接超时(' + timeoutMs + 'ms)'));
        }
      }, timeoutMs);
      dev.on('BLEConnectionStateChange', (state: ble.BLEConnectionChangeState) => {
        this.onLog('连接状态变化: ' + state.state);
        if (state.state === constant.ProfileConnectionState.STATE_CONNECTED && !settled) {
          settled = true;
          clearTimeout(timer);
          resolve();
        }
      });
      try {
        dev.connect();
      } catch (err) {
        const e = err as BusinessError;
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          reject(new Error(e.code + ' ' + e.message));
        }
      }
    });
  }

  async connect(deviceId: string): Promise<void> {
    this.state = ConnState.REQUEST;
    this.protocolReady = false;
    this.transferring = false;
    this.heartbeatFails = 0;
    this.linkWaiter = null;
    let lastMsg: string = '';
    for (let attempt: number = 1; attempt <= this.connectRetries; attempt++) {
      const dev = ble.createGattClientDevice(deviceId);
      this.device = dev;
      this.dataChar = null;
      this.versionChar = null;
      dev.on('BLEConnectionStateChange', (state: ble.BLEConnectionChangeState) => {
        if (state.state === constant.ProfileConnectionState.STATE_DISCONNECTED) {
          this.dataChar = null;
          this.versionChar = null;
          this.clearIdleTimer();
          this.stopHeartbeat();
          this.state = ConnState.EXIT;
          const reason: string = this.closing ? 'local' : 'remote';
          this.closing = false;
          this.onDisconnected(reason);
        }
      });
      try {
        this.onLog('等待设备广播窗口（最长 75 秒）...');
        await this.waitConnected(dev, 75000);
        this.onLog('GATT 已连接，开始服务发现');
        const services: Array<ble.GattService> = await this.withTimeout<Array<ble.GattService>>(dev.getServices(), 8000);
        let service: ble.GattService | null = null;
        for (let i: number = 0; i < services.length; i++) {
          if (services[i].serviceUuid.toLowerCase() === EPD_SERVICE_UUID) {
            service = services[i];
          }
        }
        if (service === null) {
          throw new Error('未找到 EPD 服务');
        }
        for (let i: number = 0; i < service.characteristics.length; i++) {
          const ch: ble.BLECharacteristic = service.characteristics[i];
          const uuid: string = ch.characteristicUuid.toLowerCase();
          if (uuid === EPD_CHAR_UUID) {
            this.dataChar = ch;
          } else if (uuid === EPD_VER_UUID) {
            this.versionChar = ch;
          }
        }
        if (this.dataChar === null || this.versionChar === null) {
          throw new Error('EPD 特征不完整');
        }
        this.onLog('服务发现完成，特征数: ' + service.characteristics.length);
        const value: ble.BLECharacteristic = await this.withTimeout<ble.BLECharacteristic>(
          dev.readCharacteristicValue(this.versionChar), 5000);
        const bytes = new Uint8Array(value.characteristicValue);
        this.appVersion = bytes.length > 0 ? bytes[0] : 0x15;
        this.protocolReady = this.appVersion >= LINK_MIN_VERSION;
        this.onLog('版本读取成功: 0x' + this.appVersion.toString(16) +
          (this.protocolReady ? '（支持链路状态码）' : '（旧固件，兼容模式）'));
        dev.on('BLECharacteristicChange', (ch: ble.BLECharacteristic) => {
          if (ch.characteristicUuid.toLowerCase() === EPD_CHAR_UUID) {
            this.handleData(new Uint8Array(ch.characteristicValue));
          }
        });
        await this.withTimeout<void>(dev.setCharacteristicChangeNotification(this.dataChar, true), 5000);
        this.state = ConnState.HOLD;
        if (this.protocolReady) {
          let ok: boolean = await this.setLinkState(LinkState.HOLD, LINK_ACK_MS);
          if (!ok) {
            await this.delay(250);
            ok = await this.setLinkState(LinkState.HOLD, LINK_ACK_MS);
          }
          if (ok) {
            this.onLog('链路状态：HOLD（连接保持）');
            this.startHeartbeat();
          } else {
            // 链路状态握手失败不应阻断连接；退回兼容模式，保持可用。
            this.protocolReady = false;
            this.onLog('HOLD 未确认，进入兼容模式（保持连接，不启用心跳）');
          }
        }
        this.startIdleTimer();
        this.onLog('连接保持：已就绪');
        return;
      } catch (err) {
        lastMsg = (err as Error).message;
        this.onLog('连接失败(第' + attempt + '/' + this.connectRetries + '次): ' + lastMsg);
        this.stopHeartbeat();
        this.releaseDevice(dev);
        if (attempt < this.connectRetries) {
          await this.delay(1200);
        }
      }
    }
    this.state = ConnState.EXIT;
    throw new Error('连接失败（已重试' + this.connectRetries + '次）: ' + lastMsg);
  }

  setIdleTimeout(ms: number): void {
    this.idleTimeoutMs = ms;
    if (this.state === ConnState.HOLD) {
      this.startIdleTimer();
    }
  }

  private rawWrite(buffer: ArrayBuffer, withResponse: boolean): Promise<void> {
    const dev = this.device;
    const ch = this.dataChar;
    if (dev === null || ch === null) {
      return Promise.reject(new Error('蓝牙未连接'));
    }
    ch.characteristicValue = buffer;
    const type: ble.GattWriteType = withResponse ? ble.GattWriteType.WRITE : ble.GattWriteType.WRITE_NO_RESPONSE;
    return dev.writeCharacteristicValue(ch, type);
  }

  // 发送链路状态码并等待设备 ACK（0xB0, state）。无回复返回 false。
  private setLinkState(state: number, timeoutMs: number): Promise<boolean> {
    if (!this.protocolReady) {
      return Promise.resolve(true);
    }
    if (this.linkWaiter !== null) {
      return Promise.resolve(true);
    }
    return new Promise<boolean>((resolve: (ok: boolean) => void) => {
      let done: boolean = false;
      const timer = setTimeout(() => {
        if (!done) {
          done = true;
          this.linkWaiter = null;
          resolve(false);
        }
      }, timeoutMs);
      this.linkWaiter = (st: number) => {
        if (!done) {
          done = true;
          clearTimeout(timer);
          this.linkWaiter = null;
          resolve(st === state);
        }
      };
      this.rawWrite(frame(Cmd.LINK_STATE, new Uint8Array([state])), true).catch((err: Error) => {
        if (!done) {
          done = true;
          clearTimeout(timer);
          this.linkWaiter = null;
          resolve(false);
        }
      });
    });
  }

  // 连接保持心跳：每 15 s 发送 HOLD，连续 2 次无回复则断开。
  private startHeartbeat(): void {
    if (!this.protocolReady || this.heartbeatActive) {
      return;
    }
    this.heartbeatActive = true;
    this.heartbeatFails = 0;
    this.heartbeatTick();
  }

  private async heartbeatTick(): Promise<void> {
    while (this.heartbeatActive && this.state === ConnState.HOLD) {
      await this.delay(HEARTBEAT_MS);
      if (!this.heartbeatActive || this.state !== ConnState.HOLD) {
        return;
      }
      if (this.transferring) {
        continue;
      }
      const ok: boolean = await this.setLinkState(LinkState.HOLD, LINK_ACK_MS);
      if (ok) {
        this.heartbeatFails = 0;
        continue;
      }
      this.heartbeatFails++;
      this.onLog('连接保持心跳无回复（' + this.heartbeatFails + '/' + HEARTBEAT_MAX_FAILS + '）');
      if (this.heartbeatFails >= HEARTBEAT_MAX_FAILS) {
        this.onLog('设备连续无回复，断开连接');
        this.disconnect('设备无回复');
        this.onDisconnected('no-reply');
        return;
      }
    }
  }

  private stopHeartbeat(): void {
    this.heartbeatActive = false;
    this.heartbeatFails = 0;
  }

  // 上传图片/固件前进入 TRANSFER；图片传输结束后回到 HOLD。
  async beginTransfer(): Promise<void> {
    if (!this.protocolReady) {
      return;
    }
    this.transferring = true;
    this.heartbeatFails = 0;
    const ok: boolean = await this.setLinkState(LinkState.TRANSFER, LINK_ACK_MS + 1000);
    if (!ok) {
      // 不因状态码 ACK 丢失而断开；实际图片块 ACK 仍会验证链路。
      this.onLog('TRANSFER 未确认，继续传输（不影响连接）');
    } else {
      this.onLog('链路状态：TRANSFER（传输中）');
    }
  }

  async endTransfer(): Promise<void> {
    if (!this.protocolReady) {
      return;
    }
    this.transferring = false;
    this.heartbeatFails = 0;
    const ok: boolean = await this.setLinkState(LinkState.HOLD, LINK_ACK_MS);
    if (ok) {
      this.onLog('链路状态：HOLD（连接保持）');
    } else {
      this.onLog('传输结束但 HOLD 未确认');
    }
  }

  private async sendDisconnectCode(): Promise<void> {
    if (!this.protocolReady || !this.isConnected()) {
      return;
    }
    this.stopHeartbeat();
    const ok: boolean = await this.setLinkState(LinkState.IDLE, 1000);
    this.onLog(ok ? '链路状态：IDLE（已发送断开码）' : '断开码未收到确认');
  }

  async disconnectGracefully(reason: string): Promise<void> {
    if (this.state === ConnState.HOLD) {
      await this.sendDisconnectCode();
    }
    this.disconnect(reason);
  }


  touch(): void {
    if (this.state === ConnState.HOLD) {
      this.startIdleTimer();
    }
  }

  private clearIdleTimer(): void {
    if (this.idleTimer >= 0) {
      clearTimeout(this.idleTimer);
      this.idleTimer = -1;
    }
  }

  private startIdleTimer(): void {
    this.clearIdleTimer();
    if (this.idleTimeoutMs <= 0) {
      return;
    }
    this.idleTimer = setTimeout(() => {
      this.idleTimer = -1;
      if (this.state !== ConnState.HOLD) {
        return;
      }
      this.onLog('空闲超时（' + Math.round(this.idleTimeoutMs / 1000) + 's），发送断开码');
      this.handleIdleTimeout();
    }, this.idleTimeoutMs);
  }

  private async handleIdleTimeout(): Promise<void> {
    await this.sendDisconnectCode();
    this.disconnect('idle');
    this.onDisconnected('local');
  }

  private releaseDevice(dev: ble.GattClientDevice): void {
    this.closing = true;
    try {
      dev.disconnect();
      dev.close();
    } catch (err) {
    }
    if (this.device === dev) {
      this.device = null;
      this.dataChar = null;
      this.versionChar = null;
    }
  }

  private handleData(data: Uint8Array): void {
    if (data.length === 2 && data[0] === Rsp.LINK_STATE && this.linkWaiter !== null) {
      const w = this.linkWaiter;
      this.linkWaiter = null;
      w(data[1]);
      return;
    }
    if (data.length > 0 && data[0] === Rsp.STATUS) {
      const st: TransferStatus | null = parseStatus(data);
      if (st !== null) {
        const waiter = this.statusWaiter;
        this.statusWaiter = null;
        if (waiter !== null) {
          waiter(st);
        }
      }
      return;
    }
    if (data.length > 0 && data[0] === Rsp.BLOCK_ACK) {
      return;
    }
    this.onNotify(data);
  }

  async write(buffer: ArrayBuffer, withResponse: boolean): Promise<void> {
    const dev = this.device;
    const ch = this.dataChar;
    if (dev === null || ch === null) {
      throw new Error('蓝牙未连接');
    }
    this.touch();
    ch.characteristicValue = buffer;
    const type: ble.GattWriteType = withResponse ? ble.GattWriteType.WRITE : ble.GattWriteType.WRITE_NO_RESPONSE;
    await dev.writeCharacteristicValue(ch, type);
  }

  async queryStatus(): Promise<TransferStatus> {
    this.touch();
    return new Promise<TransferStatus>((resolve: (s: TransferStatus) => void, reject: (e: Error) => void) => {
      const timer = setTimeout(() => {
        if (this.statusWaiter !== null) {
          this.statusWaiter = null;
          reject(new Error('状态查询超时'));
        }
      }, 3000);
      this.statusWaiter = (st: TransferStatus) => {
        clearTimeout(timer);
        resolve(st);
      };
      this.write(frame(Cmd.QUERY_STATUS), true).catch((err: Error) => {
        clearTimeout(timer);
        this.statusWaiter = null;
        reject(err);
      });
    });
  }

  async refreshConfig(): Promise<void> {
    const dev = this.device;
    const ch = this.dataChar;
    if (dev === null || ch === null) {
      throw new Error('蓝牙未连接');
    }
    this.touch();
    await dev.setCharacteristicChangeNotification(ch, false);
    await this.delay(150);
    await dev.setCharacteristicChangeNotification(ch, true);
  }

  trySetMtu(mtu: number): void {
    if (this.device !== null) {
      try {
        this.device.setBLEMtuSize(mtu);
      } catch (err) {
      }
    }
  }

  disconnect(reason?: string): void {
    this.closing = true;
    this.stopScan();
    this.stopHeartbeat();
    this.clearIdleTimer();
    this.linkWaiter = null;
    this.transferring = false;
    this.state = ConnState.EXIT;
    const dev = this.device;
    this.device = null;
    this.dataChar = null;
    this.versionChar = null;
    this.statusWaiter = null;
    if (dev !== null) {
      try {
        dev.disconnect();
        dev.close();
      } catch (err) {
      }
    } else {
      this.closing = false;
    }
    if (reason !== undefined) {
      this.onLog('连接退出: ' + reason);
    }
  }
}