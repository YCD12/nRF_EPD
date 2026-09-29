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

test('selecting each GUI mode sends its command while keeping the connection', async () => {
  const { run, context, logs } = setup();
  const writes = [];
  context.capture = data => writes.push(Array.from(data));
  run(`appVersion = 0x35; lastWeekStartSent = 0;
    epdCharacteristic = {writeValueWithResponse: async data => capture(data)};`);
  for (const mode of [1, 2, 3]) {
    writes.length = 0;
    assert.equal(await run(`sendTimeCommand(${mode}, 'test mode')`), true);
    assert.deepEqual(writes.map(p => p[0]), [run('EpdCmd.SYNC_TIME_SILENT'), run('EpdCmd.SET_MODE')]);
    assert.deepEqual(writes[1], [run('EpdCmd.SET_MODE'), mode]);
    assert.equal(run('gattServer.connected'), true);
  }
  assert.ok(logs.some(message => message.includes('无需断开蓝牙')));
  assert.ok(!logs.some(message => message.includes('断开连接后刷新')));
});

test('current page rejects older firmware before sending INIT', async () => {
  const { run, context, logs } = setup();
  const writes = [];
  context.device = { name: 'NRF_EPD_OLD', gatt: {
    connected: true, disconnect() { this.connected = false; },
    async getPrimaryService() { return { async getCharacteristic(uuid) {
      return uuid.includes('0003-') ? { async readValue() { return new DataView(Uint8Array.of(0x36).buffer); } } :
        { addEventListener() {}, async startNotifications() {}, async writeValueWithResponse(data) { writes.push(data[0]); } };
    } }; },
  } };
  run('bleDevice = device; epdCharacteristic = null;');
  await run('connect()');
  assert.deepEqual(writes, []);
  assert.equal(context.device.gatt.connected, false);
  assert.ok(logs.some(message => message.includes('0x37')));
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
      return uuid.includes('0003-') ? { async readValue() { return new DataView(Uint8Array.of(0x37).buffer); } } : characteristic;
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
