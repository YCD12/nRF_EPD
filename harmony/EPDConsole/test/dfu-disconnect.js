// Exercise the production ArkTS class with mocked platform GATT, not a copied implementation.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(require.resolve('@ohos/hvigor/node_modules/typescript', {
  paths: [path.resolve(__dirname, '..'), path.resolve(__dirname, '../../../..', 'EPDConsole')]
}));
const sourcePath = path.resolve(__dirname, '../entry/src/main/ets/common/DfuClient.ets');
const compiled = ts.transpileModule(fs.readFileSync(sourcePath, 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  fileName: 'DfuClient.ts'
}).outputText;
const CONNECTED = 2, DISCONNECTED = 0;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
function fixture() {
  const devices = [];
  class MockGatt {
    constructor(id) { this.id = id; this.callbacks = new Map(); this.disconnectCalls = 0; this.closeCalls = 0; }
    on(name, callback) { this.callbacks.set(name, callback); }
    emitState(state) { this.callbacks.get('BLEConnectionStateChange')({ state }); }
    connect() { this.emitState(CONNECTED); }
    disconnect() { this.disconnectCalls++; }
    close() { this.closeCalls++; }
    async getServices() {
      return [{ serviceUuid: '0000fe59-0000-1000-8000-00805f9b34fb', characteristics: [{
        characteristicUuid: '8ec90003-f315-4f60-9fb8-838830daea50',
        properties: { write: true, indicate: true }, characteristicValue: new ArrayBuffer(0)
      }] }];
    }
  }
  const exports = {};
  const platform = {
    ble: { createGattClientDevice(id) { const dev = new MockGatt(id); devices.push(dev); return dev; } },
    connection: {}, constant: { ProfileConnectionState: { STATE_CONNECTED: CONNECTED, STATE_DISCONNECTED: DISCONNECTED } }
  };
  vm.runInNewContext(compiled, {
    exports, require(name) {
      if (name === '@kit.ConnectivityKit') return platform;
      if (name === '@kit.BasicServicesKit') return {};
      throw new Error('Unexpected import: ' + name);
    }, setTimeout, clearTimeout, Date, Uint8Array, ArrayBuffer, Map, console
  }, { filename: sourcePath });
  return { client: new exports.DfuClient(), devices };
}
async function rejectsTimeout(wait) {
  await assert.rejects(wait, error => /超时|timeout|未主动断开/i.test(error.message));
}
(async () => {
  let count = 0;
  async function test(name, action) { await action(); count++; console.log('PASS ' + name); }
  await test('device disconnect before wait is observed', async () => {
    const { client, devices } = fixture();
    await client.connect('first'); devices[0].emitState(DISCONNECTED);
    await client.waitForDeviceDisconnect(100);
    assert.equal(devices[0].disconnectCalls, 0);
    client.disconnect();
    assert.equal(devices[0].disconnectCalls, 0); assert.equal(devices[0].closeCalls, 1);
  });
  await test('wait resolves on device disconnect without host disconnect', async () => {
    const { client, devices } = fixture(); await client.connect('first');
    let resolved = false;
    const wait = client.waitForDeviceDisconnect(500).then(() => { resolved = true; });
    await sleep(20); assert.equal(resolved, false);
    devices[0].emitState(DISCONNECTED); await wait;
    assert.equal(devices[0].disconnectCalls, 0); assert.equal(devices[0].closeCalls, 0);
  });
  await test('timeout rejects without disconnecting or closing GATT', async () => {
    const { client, devices } = fixture(); await client.connect('first');
    await rejectsTimeout(client.waitForDeviceDisconnect(30));
    assert.equal(devices[0].disconnectCalls, 0); assert.equal(devices[0].closeCalls, 0);
  });
  await test('each connect resets previous disconnection state', async () => {
    const { client, devices } = fixture(); await client.connect('first');
    devices[0].emitState(DISCONNECTED); await client.waitForDeviceDisconnect(100);
    await client.connect('second');
    await rejectsTimeout(client.waitForDeviceDisconnect(30));
    assert.equal(client.hasButtonless(), true);
    devices[1].emitState(DISCONNECTED); await client.waitForDeviceDisconnect(100);
  });
  await test('late old-session disconnect does not affect current session', async () => {
    const { client, devices } = fixture(); let disconnectEvents = 0;
    client.onDisconnected = () => { disconnectEvents++; };
    await client.connect('first');
    const lateCallback = devices[0].callbacks.get('BLEConnectionStateChange');
    client.disconnect(); await client.connect('second');
    lateCallback({ state: DISCONNECTED });
    assert.equal(disconnectEvents, 0);
    await rejectsTimeout(client.waitForDeviceDisconnect(30));
    devices[1].emitState(DISCONNECTED); await client.waitForDeviceDisconnect(100);
    assert.equal(disconnectEvents, 1); assert.equal(devices[1].disconnectCalls, 0);
  });
  console.log('DFU disconnect tests: ' + count + ' passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
