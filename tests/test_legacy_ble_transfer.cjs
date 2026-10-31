const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function fixture(version, { newWebMinVersion } = {}) {
  let policy = require('../html/js/main.js');
  if (newWebMinVersion !== undefined) {
    const source = fs.readFileSync(path.join(__dirname, '../html/js/main.js'), 'utf8')
      .replace(/\bconst NEW_WEB_MIN_FIRMWARE_VERSION\s*=\s*0x[0-9a-f]+\s*;/i,
        `const NEW_WEB_MIN_FIRMWARE_VERSION = 0x${newWebMinVersion.toString(16)};`);
    const policyContext = vm.createContext({ module: { exports: {} }, window: {}, document: { readyState: 'test' } });
    vm.runInContext(source, policyContext);
    policy = policyContext.module.exports;
  }
  const context = vm.createContext({
    Uint8Array, DataView, appVersion: version,
    console: { log() {}, error() {}, debug() {} },
    Date, setTimeout, clearTimeout, EpdFirmwarePolicy: policy,
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../html/js/ble_transfer.js'), 'utf8'), context);
  const transfer = vm.runInContext('BleTransfer', context);
  transfer.sessionId = 11;
  transfer.expectedBlocks = 9;
  return {
    transfer,
    reply: bytes => transfer.handleNotification(Uint8Array.from(bytes)),
    missing: () => Array.from(transfer.getMissingBlocks(transfer.pendingStatus, 9)),
  };
}

test('the shared firmware policy accepts only known integer versions and supports Node imports', () => {
  const policy = require('../html/js/main.js');
  assert.equal(Object.isFrozen(policy), true);
  assert.equal(policy.newWebMinVersion, 0x21);
  assert.equal(policy.isNewFirmware(0x20), false);
  assert.equal(policy.isNewFirmware(0x21), true);
  for (const version of [undefined, null, NaN, Infinity, '34', 34.5])
    assert.equal(policy.isNewFirmware(version), false);
});

test('changing the shared firmware boundary also changes the accepted CRC status format', () => {
  const below = fixture(0x23, { newWebMinVersion: 0x24 });
  below.reply([0xA1, 9, 0, 9, 0, 11, 1, 0xff, 1]);
  assert.deepEqual(below.missing(), []);
  const supported = fixture(0x24, { newWebMinVersion: 0x24 });
  supported.reply([0xA1, 9, 0, 9, 0, 11, 1, 0xff, 1]);
  assert.equal(supported.transfer.pendingStatus, null);
  supported.reply([0xA1, 9, 0, 9, 0, 11, 1]);
  assert.deepEqual(supported.missing(), []);
});

for (const version of [0x16, 0x19, 0x20]) {
  test(`firmware ${version.toString(16)} accepts bitmap status and verifies every block`, () => {
    const f = fixture(version);
    f.reply([0xA1, 9, 0, 9, 0, 11, 1, 0xFF, 1]);
    assert.equal(f.transfer.pendingStatus.received, 9);
    assert.deepEqual(f.missing(), []);

    // A sparse hole cannot be inferred from received count alone.
    f.reply([0xA1, 9, 0, 8, 0, 11, 1, 0xFD, 1]);
    assert.deepEqual(f.missing(), [1]);
  });

  test(`firmware ${version.toString(16)} accepts seven-byte status without a bitmap`, () => {
    const f = fixture(version);
    f.reply([0xA1, 9, 0, 7, 0, 11, 1]);
    assert.deepEqual(f.missing(), [7, 8]);
  });
}

test('legacy bitmap omissions never report the entire layer as verified', () => {
  const f = fixture(0x20);
  f.reply([0xA1, 9, 0, 9, 0, 11, 1, 0xFF]);
  assert.deepEqual(f.missing(), [8]);
  f.reply([0xA1, 9, 0, 9, 0, 11, 1, 0xFF, 0]);
  assert.deepEqual(f.missing(), [8]);
  f.reply([0xA1, 9, 0, 8, 0, 11, 1, 0xFF, 1]);
  assert.deepEqual(f.missing(), [0, 1, 2, 3, 4, 5, 6, 7, 8]);
});

for (const version of [0x21, 0x22, 0x30, 0x36, 0x37, 0x38, 0x40, undefined, null]) {
  test(`modern or unknown firmware ${version} keeps strict seven-byte status`, () => {
    const f = fixture(version);
    f.reply([0xA1, 9, 0, 9, 0, 11, 1, 0xFF, 1]);
    assert.equal(f.transfer.pendingStatus, null);
    f.reply([0xA1, 9, 0, 9, 0, 11, 1]);
    assert.deepEqual(f.missing(), []);
  });
}

test('legacy status still rejects malformed, stale, inactive and impossible headers', () => {
  for (const bytes of [
    [0xA1, 9, 0, 9, 0, 11],
    [0xA1, 9, 0, 9, 0, 12, 1, 0xFF, 1],
    [0xA1, 8, 0, 8, 0, 11, 1, 0xFF],
    [0xA1, 9, 0, 9, 0, 11, 0, 0xFF, 1],
    [0xA1, 9, 0, 10, 0, 11, 1, 0xFF, 1],
  ]) {
    const f = fixture(0x20);
    f.reply(bytes);
    assert.equal(f.transfer.pendingStatus, null);
    assert.deepEqual(f.missing(), [0, 1, 2, 3, 4, 5, 6, 7, 8]);
  }
});

test('legacy bitmap status resolves an outstanding query only after matching the active layer', () => {
  const f = fixture(0x20);
  let resolved = null;
  f.transfer.statusResolver = status => { resolved = status; };
  f.reply([0xA1, 9, 0, 9, 0, 12, 1, 0xFF, 1]);
  assert.equal(resolved, null);
  f.reply([0xA1, 9, 0, 9, 0, 11, 1, 0xFF, 1]);
  assert.equal(resolved, f.transfer.pendingStatus);
  assert.equal(f.transfer.statusResolver, null);
});

function mainFixture(version, { autoAcks = false } = {}) {
  const elements = new Map(), writes = [], logs = [];
  const context = vm.createContext({
    Uint8Array, DataView, TextDecoder, URL, URLSearchParams,
    console: { log() {}, error() {}, debug() {} },
    Date, setTimeout, clearTimeout, window: {},
    document: {
      body: {}, querySelectorAll: () => [],
      getElementById(id) {
        if (!elements.has(id)) elements.set(id, {
          value: id === 'weekStart' ? 4 : 244,
          style: {}, classList: { toggle() {}, remove() {} },
        });
        return elements.get(id);
      },
    },
    record: message => logs.push(message),
    capture: bytes => {
      writes.push(Array.from(bytes));
      if (!autoAcks) return;
      const reply = bytes[0] === 1 && bytes.length === 3 ? [0xA6, bytes[2], 0, 244, 0]
        : bytes[0] === 0x22 && bytes.length === 9 ? [0xAA, bytes[2], 0, bytes[1]]
        : [2, 3].includes(bytes[0]) && bytes.length === 3 ? [0xAB, bytes[2], 0, bytes[0]] : null;
      if (reply) vm.runInContext(`handleNotify(new DataView(Uint8Array.of(${reply}).buffer))`, context);
    },
    confirm: () => true,
  });
  const run = code => vm.runInContext(code, context);
  for (const file of ['main.js', 'ble_transfer.js']) {
    run(fs.readFileSync(path.join(__dirname, '../html/js', file), 'utf8'));
  }
  run(`addLog = record; appVersion = ${version};
    bleDevice = {gatt: {connected: true}}; gattServer = bleDevice.gatt;
    epdCharacteristic = {
      writeValueWithResponse: async bytes => capture(bytes),
      writeValueWithoutResponse: async bytes => capture(bytes),
    };`);
  return { run, context, writes, logs, elements };
}

for (const version of [0x21, 0x22, 0x36, 0x37, 0x39]) {
  test(`firmware ${version.toString(16)} sends raw debug commands without remapping or business ACKs`, { timeout: 2000 }, async () => {
    const f = mainFixture(version);
    const packets = ['3201', '0300', '01', '220100010000000008'];
    for (const hex of packets) {
      f.context.document.getElementById('cmdTXT').value = hex;
      await f.run('sendcmd()');
      assert.deepEqual(f.writes.at(-1), Array.from(Buffer.from(hex, 'hex')), hex);
      assert.equal(f.run('initWaiter'), null);
      assert.equal(f.run('modeWaiter'), null);
      assert.equal(f.run('displayWaiter'), null);
      assert.equal(f.run('DeviceOperations.current'), null);
    }
    assert.equal(f.writes.length, packets.length);
  });
}

for (const version of [0x21, 0x22, 0x39]) test(`firmware ${version.toString(16)} UI INIT and refresh attach tokens and await completion`, { timeout: 2000 }, async () => {
  const f = mainFixture(version);
  let initialized = false;
  const init = f.run('write(EpdCmd.INIT)').then(result => { initialized = true; return result; });
  await new Promise(setImmediate);
  assert.equal(initialized, false);
  assert.equal(f.writes[0].length, 3);
  assert.deepEqual(f.writes[0].slice(0, 2), [1, 0xff]);
  const initToken = f.writes[0][2];
  f.run(`handleNotify(new DataView(Uint8Array.of(0xA6, ${initToken}, 0, 244, 0).buffer))`);
  assert.equal(await init, true);
  let refreshed = false;
  const refresh = f.run('write(EpdCmd.REFRESH)').then(result => { refreshed = true; return result; });
  await new Promise(setImmediate);
  assert.equal(refreshed, false);
  assert.equal(f.writes[1].length, 3);
  assert.deepEqual(f.writes[1].slice(0, 2), [3, 0]);
  const displayToken = f.writes[1][2];
  f.run(`handleNotify(new DataView(Uint8Array.of(0xAB, ${displayToken}, 0, 3).buffer))`);
  assert.equal(await refresh, true);
});

test('raw debug packets respect the active device operation lock', async () => {
  const f = mainFixture(0x21);
  f.run('busyOperation = DeviceOperations.begin("resources")');
  f.context.document.getElementById('cmdTXT').value = '3201';
  assert.equal(await f.run('sendcmd()'), false);
  assert.equal(await f.run('writeRawPacket(Uint8Array.of(0x32, 1))'), false);
  assert.deepEqual(f.writes, []);
  assert.equal(f.run('DeviceOperations.current === busyOperation'), true);
});

test('a queued raw packet cannot reach a replacement connection or release its operation', async () => {
  const f = mainFixture(0x21);
  let release;
  f.context.blocked = new Promise(resolve => { release = resolve; });
  f.run('gattWriteTail = blocked');
  const pending = f.run('writeRawPacket(Uint8Array.of(0x32, 1))');
  await new Promise(setImmediate);
  assert.deepEqual(f.writes, []);
  const replacementWrites = [];
  f.context.captureReplacement = packet => replacementWrites.push(Array.from(packet));
  f.run(`disconnect(); bleDevice = {gatt: {connected: true}}; gattServer = bleDevice.gatt;
    epdCharacteristic = {writeValueWithResponse: async packet => captureReplacement(packet)};
    replacementOperation = DeviceOperations.begin('control');`);
  release();
  assert.equal(await pending, false);
  assert.deepEqual(f.writes, []);
  assert.deepEqual(replacementWrites, []);
  assert.equal(f.run('DeviceOperations.current === replacementOperation'), true);
});

for (const version of [0x21, 0x22, 0x36, 0x37, 0x38]) {
  const expected = [0x03, 0x04, 0x30, 0x31, 0x32];
  test(`real main.write selects each wire command exactly once for firmware ${version.toString(16)}`, async () => {
    const f = mainFixture(version, { autoAcks: true });
    for (const name of ['REFRESH', 'SLEEP', 'WRITE_BLOCK', 'QUERY_STATUS', 'RESET_TRANSFER']) {
      assert.equal(await f.run(`write(EpdCmd.${name}, [0x7A], true, false)`), true);
    }
    assert.deepEqual(f.writes.map(packet => packet.slice(0, 2)), expected.map(command => [command, 0x7A]));
    assert.equal(f.writes[0].length, 3, 'refresh includes its completion token');
    assert.equal(await f.run('write(EpdCmd.CLEAR, null, true, false)'), true);
    assert.deepEqual(f.writes.at(-1).slice(0, 2), [0x02, 1]);
    assert.equal(f.writes.at(-1).length, 3);
  });

  test(`CRC module uses real main.write for firmware ${version.toString(16)} reset, block and query`, async () => {
    const f = mainFixture(version);
    f.run('BleTransfer.expectedBlocks = 1;');
    await f.run('BleTransfer.resetTransfer(7)');
    await f.run('BleTransfer.sendBlockFast(0, 1, Uint8Array.of(9, 8), false)');
    f.context.capture = bytes => {
      f.writes.push(Array.from(bytes));
      if (bytes[0] === expected[3]) {
        const reply = [0xA1, 1, 0, 1, 0, 7, 1];
        f.run(`handleNotify(new DataView(Uint8Array.of(${reply.join(',')}).buffer))`);
      }
    };
    const status = await f.run('BleTransfer.queryStatus(1000)');
    assert.equal(status.received, 1);
    assert.equal(status.sessionId, 7);
    assert.deepEqual(f.writes[0], [expected[4], 7]);
    const crc = f.run('BleTransfer.crc16(Uint8Array.of(9, 8))');
    assert.deepEqual(f.writes[1], [expected[2], 0, 0, 1, 0, 0x0F, 9, 8, crc & 255, crc >> 8]);
    assert.deepEqual(f.writes[2], [expected[3]]);
  });
}

for (const version of [0x21, 0x22, 0x36, 0x37, 0x38]) test(`firmware ${version.toString(16)} uses the combined mode command and requires its ACK`, { timeout: 2000 }, async () => {
  const f = mainFixture(version);
  let completed = false;
  const work = f.run('sendTimeCommand(2, "时钟+日历")').then(result => { completed = true; return result; });
  await new Promise(setImmediate);
  assert.equal(completed, false);
  assert.equal(f.writes.length, 1);
  assert.equal(f.writes[0][0], 0x22);
  assert.equal(f.writes[0].length, 9);
  assert.equal(f.writes[0][1], 2);
  assert.equal(f.writes[0][3], 4);
  const token = f.writes[0][2];
  f.run(`handleNotify(new DataView(Uint8Array.of(0xAA, ${token}, 0, 2).buffer))`);
  assert.equal(await work, true);
  assert.equal(f.run('modeWaiter'), null);
});
