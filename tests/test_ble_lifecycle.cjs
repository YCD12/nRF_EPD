const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

function setup() {
  const elements = new Map();
  const logs = [];
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
  for (const file of ['ble_transfer.js', 'main.js']) {
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../html/js', file), 'utf8'), context);
  }
  context.record = message => logs.push(message);
  vm.runInContext('addLog = record; gattServer = {connected: true}; epdCharacteristic = {};', context);
  return { context, elements, logs, run: code => vm.runInContext(code, context) };
}

test('external resource installer stays on the main page', () => {
  const { run } = setup();
  assert.equal(run('typeof openResourcesPage'), 'undefined');
  const html = fs.readFileSync(path.join(__dirname, '../html/index.html'), 'utf8');
  assert.match(html, /id="externalResourceInstall"/);
  assert.doesNotMatch(html, /openResourcesPage/);
});

test('INIT waits for matching device completion before the next GATT write', async () => {
  const { run, context, elements } = setup();
  const writes = [];
  context.capture = data => { writes.push(Array.from(data)); };
  run('appVersion = 0x32; epdCharacteristic = {writeValueWithResponse: async data => capture(data)};');
  let completed = false;
  const init = run('write(EpdCmd.INIT, [9])').then(ok => { completed = true; return ok; });
  const next = run('write(EpdCmd.SET_TIME, [1])');
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(completed, false);
  assert.equal(writes.length, 1);
  assert.deepEqual(writes[0].slice(0, 2), [1, 9]);
  const token = writes[0][2];
  run(`handleNotify(new DataView(Uint8Array.of(0xa6, ${(token + 1) & 255}, 0, 244, 0).buffer))`);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(completed, false);
  run(`handleNotify(new DataView(Uint8Array.of(0xa6, ${token}, 0, 244, 0).buffer))`);
  assert.equal(await init, true);
  assert.equal(await next, true);
  assert.equal(writes.length, 2);
  assert.equal(elements.get('mtusize').value, 244);
  assert.equal(run('initWaiter'), null);
});

test('INIT handles an early completion, device failure, and disconnect', async () => {
  const { run } = setup();
  run(`appVersion = 0x32;
    epdCharacteristic = {writeValueWithResponse: async data => {
      handleNotify(new DataView(Uint8Array.of(0xa6, data[2], 0, 244, 0).buffer));
    }};`);
  assert.equal(await run('write(EpdCmd.INIT)'), true);
  run(`epdCharacteristic.writeValueWithResponse = async data => {
    handleNotify(new DataView(Uint8Array.of(0xa6, data[2], 1, 244, 0).buffer));
  };`);
  assert.equal(await run('write(EpdCmd.INIT)'), false);
  run('epdCharacteristic.writeValueWithResponse = async () => {};');
  const pending = run('write(EpdCmd.INIT)');
  await new Promise(resolve => setImmediate(resolve));
  run('resetVariables()');
  assert.equal(await pending, false);
  assert.equal(run('initWaiter'), null);
});

test('driver setup does not report success after a failed INIT', async () => {
  const { run, context, logs } = setup();
  context.confirm = () => true;
  run('write = async cmd => cmd !== EpdCmd.INIT;');
  await run('setDriver()');
  assert.equal(logs.some(message => message.includes('驱动配置已设置')), false);
  assert.equal(logs.some(message => message.includes('驱动初始化失败')), true);
});

test('display operation waits for matching completion, rejects BUSY, and cancels on disconnect', async () => {
  const { run, context } = setup();
  const packets = [];
  context.capture = data => packets.push(Array.from(data));
  run(`appVersion = 0x39; epdCharacteristic = {writeValueWithResponse: async data => capture(data)};`);
  const work = run('runDeviceOperation("image", owner => write(EpdCmd.REFRESH, null, true, false, true, owner))');
  await new Promise(setImmediate);
  const token = packets[0][2];
  assert.equal(run('DeviceOperations.current.kind'), 'image');
  for (const reply of [[0xab, token + 1, 0, 3], [0xab, token, 0, 2], [0xab, token, 0]]) {
    run(`handleNotify(new DataView(Uint8Array.of(${reply}).buffer))`);
    assert.equal(run('DeviceOperations.current.kind'), 'image');
  }
  assert.equal(await run('sendTimeCommand(1, "calendar")'), false);
  run(`handleNotify(new DataView(Uint8Array.of(0xab, ${token}, 0, 3).buffer))`);
  assert.equal(await work, true);
  assert.equal(run('DeviceOperations.current'), null);
  run(`epdCharacteristic.writeValueWithResponse = async data => {
    capture(data); handleNotify(new DataView(Uint8Array.of(0xab, data[2], 2, data[0]).buffer));
  };`);
  assert.equal(await run('write(EpdCmd.CLEAR)'), false);
  assert.equal(packets.at(-1)[1], 1); // Preserve the legacy refresh=true default.
  run('epdCharacteristic.writeValueWithResponse = async () => {};');
  const pending = run('write(EpdCmd.CLEAR, [0])');
  await new Promise(setImmediate);
  run('resetVariables()');
  assert.equal(await pending, false);
  assert.equal(run('displayWaiter'), null);
  run(`appVersion = 0x38; gattServer = {connected:true};
    epdCharacteristic = {writeValueWithResponse: async data => capture(data)};`);
  assert.equal(await run('write(EpdCmd.REFRESH)'), true);
  assert.deepEqual(packets.at(-1), [3]);
});

test('device operation owns all writes and stale completion cannot release a new operation', async () => {
  const { run, context } = setup();
  const writes = [];
  context.capture = data => writes.push(Array.from(data));
  run(`epdCharacteristic = {writeValueWithResponse: async data => capture(data)};
    globalThis.oldOwner = DeviceOperations.begin('image');`);
  assert.equal(await run('write(EpdCmd.SYNC_TIME_SILENT, [0])'), false);
  assert.equal(await run('sendTimeCommand(1, "calendar")'), false);
  assert.equal(writes.length, 0);
  assert.equal(await run('write(EpdCmd.SYNC_TIME_SILENT, [0], true, false, true, oldOwner)'), true);
  run(`resetVariables(); gattServer = {connected:true};
    epdCharacteristic = {writeValueWithResponse: async data => capture(data)};
    globalThis.newOwner = DeviceOperations.begin('resources'); DeviceOperations.end(oldOwner);`);
  assert.equal(run('DeviceOperations.current === newOwner'), true);
  assert.equal(await run('write(EpdCmd.SYNC_TIME_SILENT, [0], true, false, true, oldOwner)'), false);
  assert.equal(writes.length, 1);
  run('DeviceOperations.end(newOwner)');
});

test('image status rejects old sessions, wrong totals and truncated notifications; cancellation settles its waiter', async () => {
  const { run } = setup();
  run(`bleDevice = {gatt:{connected:true}}; epdCharacteristic = {writeValueWithResponse: async () => {}};
    BleTransfer.sessionId = 42; BleTransfer.expectedBlocks = 3; BleTransfer.logLevel = 0;`);
  const pending = run('BleTransfer.queryStatus(1000)');
  await new Promise(setImmediate);
  for (const bytes of [[0xa1,3,0,3,0,41,1], [0xa1,4,0,4,0,42,1], [0xa1,3,0,4,0,42,1], [0xa1,3,0], [0xa1,3,0,3,0,42,0]]) {
    run(`BleTransfer.handleNotification(Uint8Array.of(${bytes}))`);
    assert.equal(run('BleTransfer.pendingStatus'), null);
  }
  run('BleTransfer.handleNotification(Uint8Array.of(0xa1,3,0,3,0,42,1))');
  assert.equal((await pending).received, 3);
  const cancelled = run('BleTransfer.queryStatus(1000)');
  run('resetVariables()');
  assert.equal(await cancelled, null);
});

test('image packets fit MTU 23 and consecutive layers use distinct sessions', async () => {
  const { run, context } = setup();
  const packets = [];
  context.capture = data => packets.push(Array.from(data));
  run('document.getElementById("mtusize").value = 20');
  run(`bleDevice = {gatt:{connected:true}}; BleTransfer.logLevel = 0; BleTransfer.BATCH_DELAY_MS = 0;
    epdCharacteristic = {writeValueWithResponse: async data => {
      capture(data);
      if (data[0] === EpdCmd.QUERY_STATUS) BleTransfer.handleNotification(Uint8Array.of(
        0xa1, BleTransfer.expectedBlocks, 0, BleTransfer.expectedBlocks, 0, BleTransfer.sessionId, 1));
    }, writeValueWithoutResponse: async data => capture(data)};`);
  assert.equal(await run('BleTransfer.sendImageWithResume(new Uint8Array(25), "bw")'), true);
  const first = run('BleTransfer.sessionId');
  assert.equal(await run('BleTransfer.sendImageWithResume(new Uint8Array(25), "red")'), true);
  assert.notEqual(run('BleTransfer.sessionId'), first);
  assert.ok(packets.every(p => p.length <= 20));
  assert.deepEqual(packets.filter(p => p[0] === 0x30).map(p => p.length), [20, 20, 9, 20, 20, 9]);
});

test('disconnect during image reset delay never sends blocks to the replacement connection', async () => {
  const { run, context } = setup();
  const packets = [];
  context.capture = data => packets.push(Array.from(data));
  run(`bleDevice = {gatt:{connected:true}}; BleTransfer.logLevel = 0;
    epdCharacteristic = {writeValueWithResponse: async data => capture(data)};`);
  const work = run('runDeviceOperation("image", owner => BleTransfer.sendImageWithResume(new Uint8Array(20), "bw", null, owner))');
  await new Promise(setImmediate);
  run(`resetVariables(); gattServer = {connected:true}; bleDevice = {gatt:{connected:true}};
    epdCharacteristic = {writeValueWithResponse: async data => capture(data)};
    globalThis.owner = DeviceOperations.begin('resources');`);
  assert.equal(await work, false);
  assert.equal(run('DeviceOperations.current === owner'), true);
  assert.deepEqual(packets.map(p => p[0]), [0x32]);
  run('DeviceOperations.end(owner)');
});

test('selecting each GUI mode sends its command while keeping the connection', async () => {
  const { run, context, logs } = setup();
  const writes = [];
  context.capture = data => writes.push(Array.from(data));
  run(`appVersion = 0x38; lastWeekStartSent = 0;
    epdCharacteristic = {writeValueWithResponse: async data => {
      capture(data);
      if (data[0] === EpdCmd.SET_MODE)
        handleNotify(new DataView(Uint8Array.of(0xaa, data[2], 0, data[1]).buffer));
    }};`);
  for (const mode of [1, 2, 3]) {
    writes.length = 0;
    assert.equal(await run(`sendTimeCommand(${mode}, 'test mode')`), true);
    assert.deepEqual(writes.map(p => p[0]), [run('EpdCmd.SET_MODE')]);
    assert.equal(writes[0].length, 9);
    assert.equal(writes[0][1], mode);
    assert.equal(run('gattServer.connected'), true);
  }
  assert.ok(logs.some(message => message.includes('无需断开蓝牙')));
  assert.ok(!logs.some(message => message.includes('断开连接后刷新')));
});

test('mode write waits for matching ACK and reports busy or disconnect', async () => {
  const { run, context } = setup();
  const writes = [];
  context.capture = data => writes.push(Array.from(data));
  run('appVersion = 0x38; epdCharacteristic = {writeValueWithResponse: async data => capture(data)};');
  let settled = false;
  const first = run('write(EpdCmd.SET_MODE, [2, 0, 1, 0, 0, 0, 1, 8])')
    .then(result => { settled = true; return result; });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(writes.length, 1);
  assert.equal(settled, false);
  const token = writes[0][2];
  run(`handleNotify(new DataView(Uint8Array.of(0xaa, ${(token + 1) & 255}, 0, 2).buffer))`);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(settled, false);
  run(`handleNotify(new DataView(Uint8Array.of(0xaa, ${token}, 0, 2).buffer))`);
  assert.equal(await first, true);

  const second = run('write(EpdCmd.SET_MODE, [3, 0, 1, 0, 0, 0, 2, 8])');
  await new Promise(resolve => setImmediate(resolve));
  run(`handleNotify(new DataView(Uint8Array.of(0xaa, ${writes[1][2]}, 2, 3).buffer))`);
  assert.equal(await second, false);

  const third = run('write(EpdCmd.SET_MODE, [1, 0, 1, 0, 0, 0, 3, 8])');
  await new Promise(resolve => setImmediate(resolve));
  run('resetVariables()');
  assert.equal(await third, false);
});

test('current page rejects older firmware before sending INIT', async () => {
  const { run, context, logs } = setup();
  const writes = [];
  context.device = { name: 'NRF_EPD_OLD', gatt: {
    connected: true, disconnect() { this.connected = false; },
    async getPrimaryService() { return { async getCharacteristic(uuid) {
      return uuid.includes('0003-') ? { async readValue() { return new DataView(Uint8Array.of(0x37).buffer); } } :
        { addEventListener() {}, async startNotifications() {}, async writeValueWithResponse(data) { writes.push(data[0]); } };
    } }; },
  } };
  run('bleDevice = device; epdCharacteristic = null;');
  await run('connect()');
  assert.deepEqual(writes, []);
  assert.equal(context.device.gatt.connected, false);
  assert.ok(logs.some(message => message.includes('0x38')));
});

test('INIT disconnect stops all subsequent initialization and success UI', async () => {
  const { run, context, elements, logs } = setup();
  const writes = [];
  const characteristic = {
    addEventListener() {}, async startNotifications() {},
    async writeValueWithResponse(data) {
      writes.push(data[0]);
      run('gattServer.connected = false; disconnect();');
      throw new Error('GATT Server is disconnected');
    },
  };
  context.device = { name: 'NRF_EPD_TEST', gatt: {
    connected: true,
    async getPrimaryService() { return { async getCharacteristic(uuid) {
      return uuid.includes('0003-') ? { async readValue() { return new DataView(Uint8Array.of(0x38).buffer); } } : characteristic;
    } }; },
  } };
  run('bleDevice = device; epdCharacteristic = null;');
  await run('connect()');
  assert.deepEqual(writes, [1]);
  assert.equal(elements.get('connectbutton').innerHTML, '连接');
  assert.equal(elements.get('mtusize').value, 20);
  assert.equal(elements.get('reconnectbutton').disabled, false);
  assert.equal(logs.some(message => message.includes('已重新连接')), false);
  assert.equal(logs.some(message => message.includes('读取外置字库')), false);
});

test('queued GATT writes never run after disconnect/reconnect', async () => {
  const { run, context } = setup();
  let release;
  context.blocked = new Promise(resolve => { release = resolve; });
  run('gattWriteTail = blocked; epdCharacteristic = {writeValueWithResponse: () => { throw new Error("stale write ran"); }};');
  const pending = run('write(1)');
  run('disconnect(); gattServer = {connected:true}; epdCharacteristic = {};');
  release();
  assert.equal(await pending, false);
});

test('migration export reads the saved config and rejects stale or changed connections', async () => {
  const { run, context } = setup();
  let exported = null;
  context.URL = { createObjectURL() { return 'blob:test'; }, revokeObjectURL() {} };
  context.Blob = class { constructor(parts) { exported = JSON.parse(parts.join('')); } };
  context.setTimeout = (fn, ms) => setTimeout(fn, Math.min(ms, 5));
  context.document.createElement = () => ({ click() {} });
  context.window.CalendarAnnotations = {
    async loadFromDevice() { return true; }, pack() { return Uint8Array.of(1, 2); },
    cancelCommitWait() {},
  };
  run(`appVersion = CONFIG_READBACK_MIN_FIRMWARE_VERSION; lastDeviceConfig = new Uint8Array(14);
    epdCharacteristic = {writeValueWithResponse: async data => {
      if (data[0] === EpdCmd.GET_CONFIG) {
        const response = new Uint8Array(17);
        response[0] = 0xa9; response[1] = data[1]; response[2] = 0; response[15] = 1;
        handleNotify(new DataView(response.buffer));
      }
    }};`);
  assert.equal(await run('exportMigrationBundle()'), true);
  assert.equal(Buffer.from(exported.epd_config_hex, 'hex')[12], 1);

  exported = null;
  context.window.CalendarAnnotations.loadFromDevice = async () => {
    run('connectionGeneration++; epdCharacteristic = {};');
    return true;
  };
  assert.equal(await run('exportMigrationBundle()'), false);
  assert.equal(exported, null);

  exported = null;
  context.window.CalendarAnnotations.loadFromDevice = async () => true;
  run(`appVersion = CONFIG_READBACK_MIN_FIRMWARE_VERSION - 1;
    lastDeviceConfig = new Uint8Array(14); legacyConfigDirty = true;`);
  assert.equal(await run('exportMigrationBundle()'), false);
  assert.equal(exported, null);
});

test('current page uses compact display and CRC image command IDs', () => {
  const { run } = setup();
  assert.deepEqual(Array.from(run('[EpdCmd.REFRESH, EpdCmd.SLEEP, EpdCmd.WRITE_BLOCK, EpdCmd.QUERY_STATUS, EpdCmd.RESET_TRANSFER]')),
                   [0x03, 0x04, 0x30, 0x31, 0x32]);
  assert.equal(run('EpdCmd.WRITE_IMG'), undefined);
  assert.equal(run('EpdCmd.SEND_CMD'), undefined);
  assert.equal(run('EpdCmd.SEND_DATA'), undefined);
});

test('CRC transfer errors do not fall back to the retired image command', async () => {
  const { run } = setup();
  run('BleTransfer.sendImageWithResume = async () => { throw new Error("CRC failed"); };');
  await assert.rejects(run('writeImageCRC(Uint8Array.of(255))'), /CRC failed/);
  assert.equal(run('typeof writeImage'), 'undefined');
});

test('CRC blocks use the shared export lock and never reach GATT during export', async () => {
  const { run } = setup();
  run(`appVersion = CONFIG_READBACK_MIN_FIRMWARE_VERSION; bleDevice = {gatt: {connected: true}};
    migrationExport = {}; rawWrites = 0;
    epdCharacteristic = {
      writeValueWithResponse: async () => rawWrites++,
      writeValueWithoutResponse: async () => rawWrites++,
    };`);
  await assert.rejects(run('BleTransfer.sendBlockFast(0, 1, Uint8Array.of(255))'),
                       /BLE write rejected/);
  assert.equal(run('rawWrites'), 0);
});
