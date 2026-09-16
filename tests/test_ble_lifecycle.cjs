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
  for (const file of ['main.js', 'font_store.js']) {
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../html/js', file), 'utf8'), context);
  }
  context.record = message => logs.push(message);
  vm.runInContext('addLog = record; gattServer = {connected: true}; epdCharacteristic = {};', context);
  return { context, elements, logs, store: context.window.FontStore, run: code => vm.runInContext(code, context) };
}

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
      return uuid.includes('0003-') ? { async readValue() { return new DataView(Uint8Array.of(0x2e).buffer); } } : characteristic;
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

test('queued font operations cannot move to a new connection', async () => {
  const { store, run } = setup();
  let release;
  store.operationTail = new Promise(resolve => { release = resolve; });
  let invoked = false;
  const pending = store.enqueue(() => { invoked = true; });
  store.onDisconnect();
  run('gattServer = {connected: true}; epdCharacteristic = {};');
  release();
  await assert.rejects(pending, /连接已断开/);
  assert.equal(invoked, false);
});

test('control/list write failure rejects cleanly without dangling waiters', async () => {
  const { store, run } = setup();
  run('write = async () => false;');
  await assert.rejects(store.requestAck(6, 255, [6]), /发送失败/);
  await assert.rejects(store.listNow(), /发送失败/);
  assert.equal(store.ackWaiter, null);
  assert.equal(store.listWaiter, null);
  await new Promise(resolve => setImmediate(resolve));
});

test('disconnect while reading a BIN cannot begin an upload on a new link', async () => {
  const { store, context } = setup();
  store.listLoaded = true;
  let release;
  const bytes = new Promise(resolve => { release = resolve; });
  context.document.getElementById('fontFile0').files = [{ size: 1, arrayBuffer: () => bytes }];
  const pack = store.buildPack();
  store.onDisconnect();
  release(Uint8Array.of(1).buffer);
  await assert.rejects(pack, /连接已断开/);
});

test('small MTU fails before BEGIN mutates Flash', async () => {
  const { store, context } = setup();
  store.listLoaded = true;
  context.document.getElementById('mtusize').value = 20;
  let writes = 0;
  store.requestAck = async () => { writes++; };
  await assert.rejects(store.uploadPackStreamNow(), /34/);
  assert.equal(writes, 0);
});

test('stream write failure cleans up its ACK waiter', async () => {
  const { store } = setup();
  store.buildPack = async () => ({ data: Uint8Array.of(1), slots: [] });
  store.requestAck = async () => ({ status: 0 });
  store.sendStreamPacket = async () => false;
  await assert.rejects(store.uploadPackStreamNow(), /发送失败/);
  assert.equal(store.streamAckWaiter, null);
  await new Promise(resolve => setImmediate(resolve));
});

test('success ACK with zero progress terminates after bounded retries', async () => {
  const { store } = setup();
  store.buildPack = async () => ({ data: Uint8Array.of(1), slots: [] });
  store.requestAck = async () => ({ status: 0 });
  let packets = 0;
  store.sendStreamPacket = async () => {
    packets++;
    store.handleNotification(Uint8Array.of(0xa5, 0, 0, 0, 0, 0, 0, 3, 0));
    return true;
  };
  await assert.rejects(store.uploadPackStreamNow(), /重试次数过多/);
  assert.equal(packets, 6);
});

test('lost stream ACK resends the same window and commits once', async () => {
  const { store } = setup();
  store.buildPack = async () => ({ data: Uint8Array.of(1), slots: [] });
  const operations = [];
  store.requestAck = async op => { operations.push(op); return { status: 0 }; };
  store.listNow = async () => [];
  const wait = store.waitStreamAck.bind(store);
  let windows = 0;
  store.waitStreamAck = (...args) => ++windows === 1
    ? Promise.reject(new Error('字库数据窗口响应超时')) : wait(...args);
  const packets = [];
  store.sendStreamPacket = async payload => {
    packets.push(payload);
    if (windows > 1) store.handleNotification(Uint8Array.of(0xa5, 1, 0, 1, 0, 0, 0, 3, 0));
    return true;
  };
  await store.uploadPackStreamNow();
  assert.equal(packets.length, 2);
  assert.deepEqual(packets[0], packets[1]);
  assert.deepEqual(operations, [6, 8]);
});

test('stream write retries stop when the connection changes', async () => {
  const { store, context, run } = setup();
  let writes = 0;
  context.send = async () => { writes++; store.onDisconnect(); return false; };
  run('write = send;');
  await assert.rejects(store.sendStreamPacket([10]), /连接已断开/);
  assert.equal(writes, 1);
});

test('FST3 validates slot geometry before BEGIN and uses the new capacity', async () => {
  const { store, run, context } = setup();
  run('appVersion = 0x2F;');
  assert.equal(store.maxPackSize(), 0x7e000);
  store.listLoaded = true;
  const data = fs.readFileSync(path.join(__dirname, '../tools/dist/u8g2_font_wqy9_t_lunar.bin'));
  context.document.getElementById('fontFile1').files = [{
    size: data.length, arrayBuffer: async () => Uint8Array.from(data).buffer,
  }];
  let writes = 0;
  store.requestAck = async () => { writes++; };
  await assert.rejects(store.uploadPackStreamNow(), /尺寸不匹配/);
  assert.equal(writes, 0);
  run('appVersion = 0x2E;');
  assert.equal(store.maxPackSize(), 0x7f000);
});

test('unchanged package skips Flash writes', async () => {
  const { store } = setup();
  store.listLoaded = true;
  store.slots = [{ length: 10, crc32: 123 }, { length: 0, crc32: 0 }, { length: 0, crc32: 0 }];
  store.buildPack = async () => ({ data: new Uint8Array(10), slots: store.slots });
  store.requestAck = async () => { throw new Error('unchanged package should not write'); };
  await store.uploadPackStreamNow();
});

test('all shipped font headers match browser geometry', () => {
  const { store } = setup();
  ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn'].forEach((name, slot) => {
    const bytes = fs.readFileSync(path.join(__dirname, `../tools/dist/u8g2_font_${name}.bin`));
    store.validateHeader(slot, bytes);
  });
});
