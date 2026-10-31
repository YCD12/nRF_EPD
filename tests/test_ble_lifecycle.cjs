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
  for (const file of ['main.js', 'ble_transfer.js']) {
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../html/js', file), 'utf8'), context);
  }
  context.record = message => logs.push(message);
  vm.runInContext('addLog = record; gattServer = {connected: true}; epdCharacteristic = {};', context);
  return { context, elements, logs, run: code => vm.runInContext(code, context) };
}

function setupTimedImageSend(version = 0x39) {
  const state = setup();
  const { context, elements, run } = state;
  const layers = [];
  const writes = [];
  let now = 1000;
  context.Date = class extends Date {
    constructor(...args) { super(...(args.length ? args : [now])); }
    static now() { return now; }
  };
  context.advanceClock = milliseconds => { now += milliseconds; };
  context.capture = packet => writes.push(Array.from(packet));
  context.completeLayer = async (data, step) => {
    layers.push({ step, data: Array.from(data) });
    now += step === 'bw' ? 1250 : 2500;
  };
  context.processImageData = () => Uint8Array.of(1, 2, 3, 4, 5, 6, 7, 8);
  context.setTimeout = (callback, delay) => {
    const timer = setTimeout(callback, delay);
    if (delay === 5000) timer.unref();
    return timer;
  };
  context.document.getElementById('canvasSize').value = 'test_8_4';
  context.document.getElementById('ditherMode').value = 'threeColor';
  const driver = context.document.getElementById('epddriver');
  driver.value = '03';
  driver.selectedIndex = 0;
  driver.options = [{ getAttribute: name => name === 'data-size' ? 'test_8_4' : 'threeColor' }];
  context.document.getElementById('status').parentElement = { style: {} };
  run(`appVersion = ${version}; bleDevice = {gatt: {connected: true}};
    canvas = {width: 8, height: 4}; ctx = {getImageData: () => ({width: 8, height: 4})};
    cropManager = {isCropMode: () => false}; paintManager = {};
    waitForNegotiatedMtu = async () => { advanceClock(5000); return true; };
    writeImageCRC = async (data, step, owner) => {
      DeviceOperations.assert(owner); await completeLayer(data, step); DeviceOperations.assert(owner);
      return true;
    };
    epdCharacteristic = {writeValueWithResponse: async data => capture(data)};`);
  return { ...state, layers, writes, advanceClock: context.advanceClock };
}

function setupRestart() {
  const state = setup(), listeners = new Set(), writes = [], toasts = [];
  const device = {name: 'NRF_EPD_test', gatt: {connected: true},
    addEventListener(type, listener) { assert.equal(type, 'gattserverdisconnected'); listeners.add(listener); },
    removeEventListener(type, listener) { assert.equal(type, 'gattserverdisconnected'); listeners.delete(listener); },
  };
  state.context.restartTestDevice = device;
  state.context.captureRestart = data => writes.push(Array.from(data));
  state.context.showToast = message => toasts.push(message);
  state.run(`bleDevice = restartTestDevice; appVersion = 0x40;
    epdCharacteristic = {writeValueWithResponse: async data => captureRestart(data)};`);
  const drop = () => {
    device.gatt.connected = false;
    state.run('disconnect()');
    for (const listener of listeners) listener();
  };
  state.context.dropRestartConnection = drop;
  return {...state, device, listeners, writes, toasts, drop};
}

test('time packets preserve local wall time in whole, half and quarter-hour zones', () => {
  const utc = 1767225300;
  for (const offsetMinutes of [480, 330, 345, -210, -150, 765, 0, -240]) {
    const { context, run } = setup();
    let snapshots = 0;
    context.Date = class extends Date {
      constructor(...args) {
        super(...(args.length ? args : [utc * 1000]));
        if (!args.length) snapshots++;
      }
      getTimezoneOffset() { return -offsetMinutes; }
    };
    const payload = run('buildTimeData(4)');
    const epoch = new DataView(payload.buffer, payload.byteOffset, payload.byteLength).getUint32(0, false);
    const timezone = payload[4] > 127 ? payload[4] - 256 : payload[4];
    assert.equal(epoch + timezone * 3600, utc + offsetMinutes * 60, `${offsetMinutes} minutes`);
    assert.equal(timezone, Math.trunc(offsetMinutes / 60));
    assert.equal(payload[5], 4);
    assert.equal(snapshots, 1, 'epoch and timezone must come from the same instant');
  }
});

test('automatic time and mode sync use the send instant after waiting in the GATT queue', async () => {
  for (const operation of ['silent', 'mode']) {
    for (const finalOffset of [480, 345, -210]) {
      const { context, run, elements } = setup();
      const writes = [];
      let now = 1767225300123, offsetMinutes = 0, snapshots = 0, release;
      context.Date = class extends Date {
        constructor(...args) {
          super(...(args.length ? args : [now]));
          if (!args.length) snapshots++;
        }
        getTimezoneOffset() { return -offsetMinutes; }
      };
      context.blocked = new Promise(resolve => { release = resolve; });
      context.capture = data => writes.push(Array.from(data));
      elements.set('weekStart', { value: 1 });
      run(`appVersion = 0x3B; gattWriteTail = blocked;
        epdCharacteristic = {writeValueWithResponse: async data => {
          capture(data);
          if (data[0] === EpdCmd.SET_MODE)
            handleNotify(new DataView(Uint8Array.of(0xaa, data[2], 0, data[1]).buffer));
        }};`);
      const pending = run(operation === 'silent' ? 'syncTimeOnConnect()' : 'sendTimeCommand(4, "test mode")');
      await new Promise(setImmediate);
      assert.equal(writes.length, 0);
      assert.equal(snapshots, 0, 'clock snapshot must wait until the queued operation runs');
      now += 42876;
      offsetMinutes = finalOffset;
      release();
      assert.equal(await pending, true);
      assert.equal(writes.length, 1);
      const packet = Uint8Array.from(writes[0]);
      const timeOffset = operation === 'silent' ? 1 : 4;
      const epoch = new DataView(packet.buffer).getUint32(timeOffset, false);
      const timezoneByte = packet[timeOffset + 4];
      const timezone = timezoneByte > 127 ? timezoneByte - 256 : timezoneByte;
      assert.equal(epoch + timezone * 3600, Math.floor(now / 1000) + finalOffset * 60);
      assert.equal(timezone, Math.trunc(finalOffset / 60));
      assert.equal(snapshots, 1, 'epoch and timezone use one send-time snapshot');
      if (operation === 'mode') {
        assert.equal(packet[1], 4);
        assert.notEqual(packet[2], 0, 'mode acknowledgement token is retained');
        assert.equal(packet[3], 1, 'week-start setting is retained');
      }
    }
  }
});

test('explicit time writes retain their queued payload and stale factories are not evaluated', async () => {
  const { context, run } = setup();
  const writes = [];
  let release;
  context.blocked = new Promise(resolve => { release = resolve; });
  context.capture = data => writes.push(Array.from(data));
  run(`gattWriteTail = blocked;
    epdCharacteristic = {writeValueWithResponse: async data => capture(data)};
    explicitTime = Uint8Array.of(0, 0, 3, 232, 8, 4);`);
  const pending = run('write(EpdCmd.SET_TIME, explicitTime)');
  run('explicitTime.fill(0)');
  release();
  assert.equal(await pending, true);
  assert.deepEqual(writes, [[0x20, 0, 0, 3, 232, 8, 4]]);

  context.blocked = new Promise(resolve => { release = resolve; });
  run('gattWriteTail = blocked;');
  const stale = run('write(EpdCmd.SYNC_TIME_SILENT, () => { throw new Error("stale clock read"); })');
  run('disconnect(); gattServer = {connected: true}; epdCharacteristic = {};');
  release();
  assert.equal(await stale, false);
  assert.equal(writes.length, 1);
});

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
  run('appVersion = NEW_WEB_MIN_FIRMWARE_VERSION; epdCharacteristic = {writeValueWithResponse: async data => capture(data)};');
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
  run(`appVersion = NEW_WEB_MIN_FIRMWARE_VERSION;
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

test('system restart sends only SYS_RESET, holds the operation until disconnect and allows reconnect', async () => {
  const {run, elements, listeners, writes, toasts, drop} = setupRestart();
  const work = run('restartDevice()');
  await new Promise(setImmediate);
  assert.deepEqual(writes, [[0x91]]);
  assert.equal(run('DeviceOperations.current.kind'), 'restarting');
  assert.equal(elements.get('restartbutton').disabled, true);
  assert.equal(await run('sendTimeCommand(1, "calendar")'), false);
  drop();
  assert.equal(await work, true);
  assert.equal(run('DeviceOperations.current'), null);
  assert.equal(elements.get('restartbutton').disabled, true);
  assert.equal(elements.get('reconnectbutton').disabled, false);
  assert.equal(listeners.size, 0);
  assert.match(toasts.at(-1), /已断开.*重连/);
});

test('restart tolerates disconnect before GATT acknowledgement without claiming confirmed reboot', async () => {
  const {run, writes, logs, listeners} = setupRestart();
  run(`epdCharacteristic.writeValueWithResponse = async data => {
    captureRestart(data); dropRestartConnection(); throw new Error('GATT disconnected');
  };`);
  assert.equal(await run('restartDevice()'), true);
  assert.deepEqual(writes, [[0x91]]);
  assert.equal(listeners.size, 0);
  assert.ok(logs.some(message => /已断开.*重连/.test(message)));
  assert.ok(!logs.some(message => /重启成功|重启完成|重启指令发送失败/.test(message)));
});

test('restart reports accepted command without inventing a reboot when the device stays connected', async () => {
  const {run, context, logs, listeners} = setupRestart();
  context.setTimeout = (callback, delay) => setTimeout(callback, delay === 5000 ? 0 : delay);
  assert.equal(await run('restartDevice()'), true);
  assert.equal(run('gattServer.connected'), true);
  assert.equal(run('DeviceOperations.current'), null);
  assert.equal(listeners.size, 0);
  assert.ok(logs.some(message => /指令已发送.*尚未检测到断连/.test(message)));
  assert.ok(!logs.some(message => /重启成功|重启完成/.test(message)));
});

test('restart transport failure releases the operation and reports failure while still connected', async () => {
  const {run, context, logs, listeners} = setupRestart();
  context.setTimeout = (callback, delay) => setTimeout(callback, delay === 5000 ? 0 : delay);
  run('epdCharacteristic.writeValueWithResponse = async () => { throw new Error("radio error"); };');
  assert.equal(await run('restartDevice()'), false);
  assert.equal(run('DeviceOperations.current'), null);
  assert.equal(run('gattServer.connected'), true);
  assert.equal(listeners.size, 0);
  assert.ok(logs.some(message => /重启指令发送失败/.test(message)));
});

test('restart is blocked offline or during resource updates without writing a reset command', async () => {
  const {run, writes, listeners, elements} = setupRestart();
  run('globalThis.resourceOwner = DeviceOperations.begin("resources")');
  assert.equal(elements.get('restartbutton').disabled, true);
  assert.equal(await run('restartDevice()'), false);
  assert.equal(run('DeviceOperations.current === resourceOwner'), true);
  run('DeviceOperations.end(resourceOwner); gattServer.connected = false; restartTestDevice.gatt.connected = false; updateButtonStatus()');
  assert.equal(await run('restartDevice()'), false);
  assert.equal(elements.get('restartbutton').disabled, true);
  assert.equal(listeners.size, 0);
  assert.deepEqual(writes, []);
});

test('a late restart result cannot notify or unlock a replacement connection', async () => {
  const {run, context, drop, toasts, listeners} = setupRestart();
  let release;
  context.delayedRestartWrite = () => new Promise(resolve => { release = resolve; });
  run('epdCharacteristic.writeValueWithResponse = delayedRestartWrite');
  const work = run('restartDevice()');
  await new Promise(setImmediate);
  drop();
  run(`gattServer = {connected:true}; epdCharacteristic = {};
    bleDevice = {gatt:{connected:true}}; globalThis.newOwner = DeviceOperations.begin('resources');`);
  release();
  assert.equal(await work, false);
  assert.equal(run('DeviceOperations.current === newOwner'), true);
  assert.equal(toasts.length, 0);
  assert.equal(listeners.size, 0);
  run('DeviceOperations.end(newOwner)');
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
});

test('image transfer reports the combined CRC time before refresh completion and excludes preparation', async () => {
  const { run, elements, logs, layers, writes, advanceClock } = setupTimedImageSend();
  let completed = false;
  const work = run('sendimg()').then(result => { completed = true; return result; });
  await new Promise(setImmediate);

  assert.deepEqual(layers, [
    { step: 'bw', data: [1, 2, 3, 4] },
    { step: 'red', data: [5, 6, 7, 8] },
  ]);
  assert.equal(writes.length, 1);
  assert.equal(writes[0][0], run('EpdCmd.REFRESH'));
  assert.equal(completed, false);
  assert.equal(run('DeviceOperations.current.kind'), 'image');
  const transferMessage = '传图完成！传图耗时: 3.750s，等待屏幕刷新…';
  assert.equal(elements.get('status').innerHTML, transferMessage);
  assert.equal(logs.filter(message => message === transferMessage).length, 1);
  assert.equal(logs.some(message => message.includes('图片刷新完成')), false);

  // A global progress timer changing during refresh cannot alter this operation's total.
  run('startTime = 0');
  advanceClock(9000);
  run(`handleNotify(new DataView(Uint8Array.of(0xab, ${writes[0][2]}, 0, 3).buffer))`);
  await work;
  assert.equal(completed, true);
  assert.equal(run('DeviceOperations.current'), null);
  const completion = '图片刷新完成！传图耗时: 3.750s · 刷新耗时: 9.000s · 总耗时: 17.750s';
  assert.equal(elements.get('status').innerHTML, completion);
  assert.ok(logs.includes(completion));
});

test('minimum supported firmware confirms refresh completion before reporting image success', async () => {
  const { run, elements, logs, writes } = setupTimedImageSend(0x21);
  const work = run('sendimg()');
  await new Promise(setImmediate);
  assert.equal(writes.length, 1);
  assert.equal(writes[0].length, 3);
  assert.equal(run('DeviceOperations.current.kind'), 'image');
  assert.equal(logs.some(message => message.includes('图片刷新完成')), false);
  run(`handleNotify(new DataView(Uint8Array.of(0xab, ${writes[0][2]}, 0, 3).buffer))`);
  await work;
  assert.equal(run('DeviceOperations.current'), null);
  assert.match(elements.get('status').innerHTML, /图片刷新完成.*传图耗时: 3\.750s/);
});

test('failed refresh preserves the completed transfer duration and releases the operation', async () => {
  const { run, logs, writes, advanceClock } = setupTimedImageSend();
  const work = run('sendimg()');
  await new Promise(setImmediate);
  advanceClock(4000);
  run(`handleNotify(new DataView(Uint8Array.of(0xab, ${writes[0][2]}, 2, 3).buffer))`);
  await work;
  assert.ok(logs.includes('传图完成！传图耗时: 3.750s，等待屏幕刷新…'));
  assert.ok(logs.some(message => message.includes('刷新指令发送失败')));
  assert.equal(logs.some(message => message.includes('图片刷新完成')), false);
  assert.equal(run('DeviceOperations.current'), null);
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
  run(`appVersion = 0x3B; lastWeekStartSent = 0;
    epdCharacteristic = {writeValueWithResponse: async data => {
      capture(data);
      if (data[0] === EpdCmd.SET_MODE)
        handleNotify(new DataView(Uint8Array.of(0xaa, data[2], 0, data[1]).buffer));
    }};`);
  for (const mode of [1, 2, 3, 4, 5]) {
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

test('both todo modes use the shared new-firmware boundary', async () => {
  const { run, context, logs, elements } = setup(); const writes = [];
  context.capture = data => writes.push(Array.from(data));
  run(`appVersion = NEW_WEB_MIN_FIRMWARE_VERSION - 1; epdCharacteristic = {writeValueWithResponse: async data => {
    capture(data);
    if (data[0] === EpdCmd.SET_MODE) handleNotify(new DataView(Uint8Array.of(0xaa, data[2], 0, data[1]).buffer));
  }}; updateButtonStatus();`);
  assert.equal(elements.get('todocalendarmodebutton').disabled, true);
  assert.equal(elements.get('todofullmodebutton').disabled, true);
  assert.equal(await run('sendTimeCommand(4, "待办日历")'), false);
  assert.equal(await run('sendTimeCommand(5, "全屏待办")'), false);
  assert.equal(writes.length, 0);
  run('appVersion = NEW_WEB_MIN_FIRMWARE_VERSION; updateButtonStatus()');
  assert.equal(elements.get('todocalendarmodebutton').disabled, false);
  assert.equal(elements.get('todofullmodebutton').disabled, false);
  assert.equal(await run('sendTimeCommand(4, "待办日历")'), true);
  assert.equal(await run('sendTimeCommand(5, "全屏待办")'), true);
  assert.equal(run('displayModeName(5)'), '全屏待办');
  run('updateButtonStatus(true)'); assert.equal(elements.get('todofullmodebutton').disabled, true);
});

test('workspace mode activation can observe cancellation and device acceptance', async () => {
  const { run, context } = setup(); let mode = null;
  context.confirm = () => false;
  context.captureMode = next => { mode = next; };
  run('sendTimeCommand = async mode => { captureMode(mode); return true; }');
  assert.equal(await run('syncTime(5)'), false); assert.equal(mode, null);
  context.confirm = () => true;
  assert.equal(await run('syncTime(5)'), true); assert.equal(mode, 5);
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

test('current page disconnects firmware below 0x21 before sending INIT', async () => {
  const { run, context, logs } = setup();
  const writes = [];
  context.device = { name: 'NRF_EPD_OLD', gatt: {
    connected: true, disconnect() { this.connected = false; },
    async getPrimaryService() { return { async getCharacteristic(uuid) {
      return uuid.includes('0003-') ? { async readValue() { return new DataView(Uint8Array.of(0x20).buffer); } } :
        { addEventListener() {}, async startNotifications() {}, async writeValueWithResponse(data) { writes.push(data[0]); } };
    } }; },
  } };
  run("bleDevice = device; epdCharacteristic = null; showLegacyFirmwareDialog = async () => 'cancel';");
  await run('connect()');
  assert.deepEqual(writes, []);
  assert.equal(context.device.gatt.connected, false);
  assert.ok(logs.some(message => message.includes('0x20')));
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
  run(`appVersion = NEW_WEB_MIN_FIRMWARE_VERSION; lastDeviceConfig = new Uint8Array(14);
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
  run(`appVersion = NEW_WEB_MIN_FIRMWARE_VERSION - 1;
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
  run(`appVersion = NEW_WEB_MIN_FIRMWARE_VERSION; bleDevice = {gatt: {connected: true}};
    migrationExport = {}; rawWrites = 0;
    epdCharacteristic = {
      writeValueWithResponse: async () => rawWrites++,
      writeValueWithoutResponse: async () => rawWrites++,
    };`);
  await assert.rejects(run('BleTransfer.sendBlockFast(0, 1, Uint8Array.of(255))'),
                       /BLE write rejected/);
  assert.equal(run('rawWrites'), 0);
});
