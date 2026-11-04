// RLE-compressed image transfer integration tests.
//
// Firmware advertises RLE support after INIT with the notification string
// "mtu=<n> rle=1" (old firmware sends only "mtu=<n>"). The transfer layer then
// compresses each WRITE_BLOCK payload independently and sets block-header
// total bit15 only when the compressed stream is smaller than the raw chunk;
// CRC covers the bytes actually placed on the wire. Old firmware must stay on
// the raw path, so both paths are covered here.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

const RleCodec = require('../html/js/rle.js');
const RLE_SOURCE = fs.readFileSync(path.join(__dirname, '../html/js/rle.js'), 'utf8');
const MAIN_SOURCE = fs.readFileSync(path.join(__dirname, '../html/js/main.js'), 'utf8');
const TRANSFER_SOURCE = fs.readFileSync(path.join(__dirname, '../html/js/ble_transfer.js'), 'utf8');

// Same minimal vm scaffolding as tests/test_ble_lifecycle.cjs: main.js provides
// the command constants/notify plumbing, ble_transfer.js the CRC block sender.
function setup() {
  const elements = new Map();
  const context = vm.createContext({
    console: { log() {}, error() {} }, Uint8Array, DataView, TextDecoder,
    URL, URLSearchParams,
    setTimeout, clearTimeout, Date, Promise,
    window: {},
    document: {
      body: {}, querySelectorAll: () => [],
      getElementById(id) {
        if (!elements.has(id)) elements.set(id, {
          value: id === 'mtusize' ? 244 : 0, style: {}, childNodes: [],
          classList: { toggle() {}, remove() {} },
        });
        return elements.get(id);
      },
    },
  });
  vm.runInContext(MAIN_SOURCE, context);
  vm.runInContext(TRANSFER_SOURCE, context);
  vm.runInContext(RLE_SOURCE, context);
  vm.runInContext('addLog = () => {}; gattServer = {connected: true}; epdCharacteristic = {};', context);
  return { context, elements, run: code => vm.runInContext(code, context) };
}

function advertiseMtu(run, message) {
  const bytes = Array.from(Buffer.from(message, 'utf8'));
  run(`handleNotify(new DataView(Uint8Array.from([${bytes}]).buffer))`);
}

// Independent CRC16-CCITT implementation (poly 0x8408, init 0xFFFF) matching the
// firmware/web transfer path, used to validate the 0x30 packet trailer.
function referenceCrc16(bytes) {
  let crc = 0xFFFF;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i];
    for (let j = 0; j < 8; j++) crc = (crc & 1) ? (crc >>> 1) ^ 0x8408 : crc >>> 1;
  }
  return crc & 0xFFFF;
}

// Packet: [0x30][block_id L][block_id H][total L][total H (bit15=RLE)][cfg]
//         [payload...][crc L][crc H]
function parseBlockPacket(bytes) {
  const payload = bytes.slice(6, bytes.length - 2);
  const totalField = bytes[3] | (bytes[4] << 8);
  return {
    blockId: bytes[1] | (bytes[2] << 8),
    totalBlocks: totalField & 0x7FFF,
    rle: (totalField & 0x8000) !== 0,
    packetLength: bytes.length,
    payload: Uint8Array.from(payload),
    crc: bytes[6 + payload.length] | (bytes[7 + payload.length] << 8),
  };
}

// Send one layer through BleTransfer with the given MTU/rle advertisement and
// return the captured WRITE_BLOCK packets.
async function runRleLayer({ data, advertise = null } = {}) {
  const state = setup();
  const { context, run } = state;
  const packets = [];
  context.capture = (bytes, withResponse) => packets.push({ bytes: Array.from(bytes), withResponse });
  context.setTimeout = (callback, delay) => setTimeout(callback, Math.min(delay, 10));
  if (advertise !== null) advertiseMtu(run, advertise);
  run(`bleDevice = {gatt:{connected:true}}; BleTransfer.logLevel = 0; BleTransfer.BATCH_DELAY_MS = 0;
    document.getElementById('interleavedcount').value = 100;
    epdCharacteristic = {
      writeValueWithResponse: async data => {
        capture(data, true);
        if (data[0] === EpdCmd.QUERY_STATUS) BleTransfer.handleNotification(Uint8Array.of(
          0xa1, BleTransfer.expectedBlocks, 0, BleTransfer.expectedBlocks, 0, BleTransfer.sessionId, 1));
      },
      writeValueWithoutResponse: async data => capture(data, false),
    };`);
  const ok = await run(`BleTransfer.sendImageWithResume(Uint8Array.from([${Array.from(data)}]), "bw")`);
  return {
    ...state, ok,
    blocks: packets.filter(packet => packet.bytes[0] === run('EpdCmd.WRITE_BLOCK'))
      .map(packet => packet.bytes),
  };
}

test('rle.js is loaded as window.RleCodec in the page context', () => {
  const { run } = setup();
  assert.equal(run('typeof window.RleCodec.encodeBlock'), 'function');
  assert.equal(run('typeof BleTransfer.getRleCodec().decode'), 'function');
});

test('without the rle=1 capability every block stays raw (total bit15 clear)', async () => {
  const data = new Uint8Array(500).fill(0x00);
  const { ok, blocks, run } = await runRleLayer({ data, advertise: 'mtu=244' });
  assert.equal(ok, true);
  assert.equal(blocks.length, 3);
  assert.equal(run('BleTransfer.rleSupported'), false, 'plain "mtu=<n>" disables RLE');
  blocks.forEach((bytes, index) => {
    const info = parseBlockPacket(bytes);
    const raw = data.slice(index * 236, Math.min((index + 1) * 236, data.length));
    assert.equal(info.blockId, index);
    assert.equal(info.totalBlocks, 3);
    assert.equal(info.rle, false, `block ${index}: no capability -> raw`);
    assert.equal(info.packetLength, 8 + raw.length);
    assert.deepEqual(Array.from(info.payload), Array.from(raw));
    assert.equal(info.crc, referenceCrc16(raw));
  });
});

test('rle=1 capability compresses flat blocks and sets total bit15 with a valid CRC', async () => {
  const data = new Uint8Array(500).fill(0xff);
  const { ok, blocks, run } = await runRleLayer({ data, advertise: 'mtu=244 rle=1' });
  assert.equal(ok, true);
  assert.equal(run('BleTransfer.rleSupported'), true);
  assert.equal(blocks.length, 3);
  let savedBytes = 0;
  blocks.forEach((bytes, index) => {
    const info = parseBlockPacket(bytes);
    const raw = data.slice(index * 236, Math.min((index + 1) * 236, data.length));
    assert.equal(info.blockId, index);
    assert.equal(info.totalBlocks, 3, 'bit15 must be masked out of the real block count');
    assert.equal(info.rle, true, `block ${index}: flat data must use RLE`);
    assert.ok(info.payload.length * 4 < raw.length, `block ${index}: ${info.payload.length}B vs ${raw.length}B raw`);
    assert.equal(info.packetLength, 8 + info.payload.length, 'total packet length matches payload');
    assert.equal(info.crc, referenceCrc16(info.payload), 'CRC covers the compressed payload');
    assert.deepEqual(Array.from(RleCodec.decode(info.payload)), Array.from(raw), 'block decodes to its raw bytes');
    savedBytes += raw.length - info.payload.length;
  });
  assert.ok(savedBytes > 400, `compressed payload substantially smaller (saved ${savedBytes}B)`);
  // Progress/speed statistics stay in raw-byte semantics, not wire bytes.
  assert.equal(run('BleTransfer.transferStats.bytesSent'), data.length, 'bytesSent counts raw bytes');
  assert.equal(run('BleTransfer.transferStats.blocksSent'), blocks.length);
});

test('mixed data falls back to raw per block when compression does not shrink it', async () => {
  const data = new Uint8Array(36);
  data.fill(0x00, 0, 12);
  for (let i = 0; i < 12; i++) data[12 + i] = 0x10 + i; // 12 distinct bytes -> 13B literal > 12B raw
  data.fill(0xff, 24, 36);
  const { ok, blocks, run } = await runRleLayer({ data, advertise: 'mtu=20 rle=1' });
  assert.equal(ok, true);
  assert.equal(run('BleTransfer.rleSupported'), true);
  assert.equal(blocks.length, 3);
  const infos = blocks.map(parseBlockPacket);
  assert.deepEqual(infos.map(info => info.rle), [true, false, true], 'only compressible blocks set bit15');
  infos.forEach((info, index) => {
    const raw = data.slice(index * 12, (index + 1) * 12);
    assert.equal(info.blockId, index);
    assert.equal(info.totalBlocks, 3);
    assert.equal(info.crc, referenceCrc16(info.payload));
    if (info.rle) {
      assert.ok(info.payload.length < raw.length);
      assert.deepEqual(Array.from(RleCodec.decode(info.payload)), Array.from(raw));
    } else {
      assert.equal(info.payload.length, raw.length);
      assert.deepEqual(Array.from(info.payload), Array.from(raw), 'incompressible block keeps raw bytes');
    }
  });
});

test('RLE capability is cleared by connection reset and by transfer module init', () => {
  const { run } = setup();
  advertiseMtu(run, 'mtu=244 rle=1');
  assert.equal(run('BleTransfer.rleSupported'), true);
  run('resetVariables()');
  assert.equal(run('BleTransfer.rleSupported'), false, 'disconnect/reset clears the capability');
  advertiseMtu(run, 'mtu=244 rle=1');
  assert.equal(run('BleTransfer.rleSupported'), true);
  run('BleTransfer.init()');
  assert.equal(run('BleTransfer.rleSupported'), false, 'init() clears the capability');
  advertiseMtu(run, 'mtu=244');
  assert.equal(run('BleTransfer.rleSupported'), false, 'old firmware mtu-only notification keeps RLE off');
});
