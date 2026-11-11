export const EPD_SERVICE_UUID: string = '62750001-d828-918d-fb46-b6c11c675aec';
export const EPD_CHAR_UUID: string = '62750002-d828-918d-fb46-b6c11c675aec';
export const EPD_VER_UUID: string = '62750003-d828-918d-fb46-b6c11c675aec';

export class Cmd {
  static readonly SET_PINS: number = 0x00;
  static readonly INIT: number = 0x01;
  static readonly CLEAR: number = 0x02;
  static readonly SEND_COMMAND: number = 0x03;
  static readonly SEND_DATA: number = 0x04;
  static readonly REFRESH: number = 0x05;
  static readonly SLEEP: number = 0x06;
  static readonly SET_TIME: number = 0x20;
  static readonly SET_WEEK_START: number = 0x21;
  static readonly WRITE_IMG: number = 0x30;
  static readonly WRITE_BLOCK: number = 0x31;
  static readonly QUERY_STATUS: number = 0x32;
  static readonly RESET_TRANSFER: number = 0x33;
  static readonly LINK_STATE: number = 0x70;
  static readonly SET_CONFIG: number = 0x90;
  static readonly SYS_RESET: number = 0x91;
  static readonly SYS_SLEEP: number = 0x92;
  static readonly CFG_ERASE: number = 0x99;
}

export class Rsp {
  static readonly BLOCK_ACK: number = 0xA0;
  static readonly STATUS: number = 0xA1;
  static readonly LINK_STATE: number = 0xB0;
}

// 与固件 EPD_service.h 的 epd_link_state_t 保持一致
export class LinkState {
  static readonly IDLE: number = 0x00;
  static readonly HOLD: number = 0x01;
  static readonly TRANSFER: number = 0x02;
}

export function crc16(data: Uint8Array): number {
  let crc: number = 0xFFFF;
  for (let i: number = 0; i < data.length; i++) {
    crc ^= data[i];
    for (let j: number = 0; j < 8; j++) {
      crc = (crc & 1) === 1 ? ((crc >>> 1) ^ 0x8408) : (crc >>> 1);
    }
  }
  return crc & 0xFFFF;
}

export function frame(cmd: number, data: Uint8Array | null = null): ArrayBuffer {
  const len: number = 1 + (data === null ? 0 : data.length);
  const out = new Uint8Array(len);
  out[0] = cmd;
  if (data !== null) {
    out.set(data, 1);
  }
  return out.buffer as ArrayBuffer;
}

export function buildBlockFrame(blockId: number, total: number, cfg: number, payload: Uint8Array): ArrayBuffer {
  const crc: number = crc16(payload);
  const pkt = new Uint8Array(8 + payload.length);
  pkt[0] = Cmd.WRITE_BLOCK;
  pkt[1] = blockId & 0xFF;
  pkt[2] = (blockId >> 8) & 0xFF;
  pkt[3] = total & 0xFF;
  pkt[4] = (total >> 8) & 0xFF;
  pkt[5] = cfg;
  pkt.set(payload, 6);
  pkt[6 + payload.length] = crc & 0xFF;
  pkt[7 + payload.length] = (crc >> 8) & 0xFF;
  return pkt.buffer as ArrayBuffer;
}

export function buildTimeData(mode: number): Uint8Array {
  const ts: number = Math.floor(Date.now() / 1000);
  const tzHours: number = -Math.round(new Date().getTimezoneOffset() / 60);
  const data = new Uint8Array(6);
  data[0] = (ts >>> 24) & 0xFF;
  data[1] = (ts >>> 16) & 0xFF;
  data[2] = (ts >>> 8) & 0xFF;
  data[3] = ts & 0xFF;
  data[4] = tzHours & 0xFF;
  data[5] = mode & 0xFF;
  return data;
}

export class TransferStatus {
  total: number = 0;
  received: number = 0;
  session: number = 0;
  active: boolean = false;
  bitmap: Uint8Array = new Uint8Array(0);
}

export function parseStatus(data: Uint8Array): TransferStatus | null {
  if (data.length < 7 || data[0] !== Rsp.STATUS) {
    return null;
  }
  const st = new TransferStatus();
  st.total = (data[1] | (data[2] << 8)) & 0xFFFF;
  st.received = (data[3] | (data[4] << 8)) & 0xFFFF;
  st.session = data[5];
  st.active = data[6] === 1;
  st.bitmap = data.slice(7);
  return st;
}

export function missingBlocks(status: TransferStatus, total: number): number[] {
  const missing: number[] = [];
  for (let i: number = 0; i < total; i++) {
    const byteIdx: number = Math.floor(i / 8);
    const bitIdx: number = i % 8;
    if (byteIdx >= status.bitmap.length || (status.bitmap[byteIdx] & (1 << bitIdx)) === 0) {
      missing.push(i);
    }
  }
  return missing;
}

export function hexToBytes(hex: string): Uint8Array {
  const clean: string = hex.replace(/[^0-9a-fA-F]/g, '');
  const out = new Uint8Array(Math.floor(clean.length / 2));
  for (let i: number = 0; i < out.length; i++) {
    out[i] = parseInt(clean.substring(i * 2, i * 2 + 2), 16);
  }
  return out;
}

export function bytesToHex(data: Uint8Array): string {
  let s: string = '';
  for (let i: number = 0; i < data.length; i++) {
    s += data[i].toString(16).padStart(2, '0');
  }
  return s;
}
