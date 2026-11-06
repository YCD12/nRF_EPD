const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const serviceUuid = '62750001-d828-918d-fb46-b6c11c675aec';
const commandUuid = '62750002-d828-918d-fb46-b6c11c675aec';
const versionUuid = '62750003-d828-918d-fb46-b6c11c675aec';

function fixture(version, options = {}) {
  const elements = new Map(), events = [], writes = [], logs = [], errors = [];
  const timers = new Map(), asynchronous = new Set(), deviceListeners = new Set();
  const config = Uint8Array.from(options.config || [1, 2, 3, 4, 5, 6, 7, 7, 0, 0, 8, 0, 1, 0]);
  const encoder = new TextEncoder();
  let context, currentCharacteristic;
  const run = code => vm.runInContext(code, context);
  const driverOption = {
    textContent: '7.5寸 (三色, UC8179)',
    getAttribute(name) { return { 'data-size': '7.5_800_480', 'data-color': 'threeColor' }[name]; },
  };
  function element(id) {
    if (!elements.has(id)) elements.set(id, {
      value: ({ mtusize: 20, epddriver: '07', canvasSize: '7.5_800_480', weekStart: '1' })[id] ?? '',
      style: {}, childNodes: [], files: [], selectedIndex: 0, options: [driverOption],
      classList: { add() {}, remove() {} },
      querySelector(selector) { return selector === 'option[value="07"]' ? driverOption : null; },
      getContext() { return { fillRect() {} }; },
    });
    return elements.get(id);
  }
  function schedule(callback, delay) {
    const timer = setTimeout(() => {
      timers.delete(timer);
      const task = Promise.resolve().then(callback);
      asynchronous.add(task);
      task.catch(error => errors.push(error)).finally(() => asynchronous.delete(task));
    }, delay <= 1500 ? 0 : delay);
    timers.set(timer, delay);
    if (delay > 1500) timer.unref();
    return timer;
  }
  function notify(value) {
    const bytes = typeof value === 'string' ? encoder.encode(value) : value;
    currentCharacteristic.value = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    for (const callback of [...currentCharacteristic.listeners]) callback({ target: currentCharacteristic });
  }
  function makeCharacteristic() {
    return {
      listeners: new Set(),
      addEventListener(type, callback) {
        assert.equal(type, 'characteristicvaluechanged');
        events.push('listener');
        this.listeners.add(callback);
      },
      async startNotifications() {
        events.push('notifications');
        if (options.notificationError) throw new Error('notifications failed');
        if (options.earlyText) {
          notify('mtu=244');
          notify('v=3300');
        }
        if (options.earlyConfig) notify(config);
        return this;
      },
      async writeValueWithResponse(data) {
        assert.equal(device.gatt.connected, true, 'writes require an active Bluetooth connection');
        writes.push(Array.from(data));
        if (options.onWrite) await options.onWrite(data);
        if (data[0] !== 1) return;
        if (options.initError) throw new Error('INIT write failed');
        if (options.initDisconnect) { device.gatt.disconnect(); return; }
        if (!options.earlyConfig) notify(config);
        notify('mtu=244');
        notify('t=1767225600');
        notify('v=3300');
      },
      async writeValueWithoutResponse(data) { return this.writeValueWithResponse(data); },
    };
  }
  const device = {
    id: 'old-device', name: 'NRF_EPD_LEGACY',
    addEventListener(type, callback) {
      assert.equal(type, 'gattserverdisconnected');
      deviceListeners.add(callback);
    },
    removeEventListener(type, callback) { deviceListeners.delete(callback); },
    gatt: {
      connected: false,
      async connect() {
        events.push('connect');
        this.connected = true;
        if (!options.reuseCharacteristic || !currentCharacteristic) currentCharacteristic = makeCharacteristic();
        return this;
      },
      disconnect() {
        events.push('disconnect');
        this.connected = false;
        for (const callback of [...deviceListeners]) callback({ target: device });
      },
      async getPrimaryService(uuid) {
        assert.equal(uuid, serviceUuid);
        return { async getCharacteristic(uuid) {
          if (uuid === commandUuid) return currentCharacteristic;
          assert.equal(uuid, versionUuid);
          return { async readValue() {
            events.push('read-version');
            if (options.versionError) throw new Error('version read failed');
            if (options.emptyVersion) return new DataView(new ArrayBuffer(0));
            return new DataView(Uint8Array.of(version).buffer);
          } };
        } };
      },
    },
  };
  const location = { href: '/legacy/index.html', pathname: '/legacy/index.html', search: '' };
  const FixedDate = class extends Date {
    constructor(...args) { super(...(args.length ? args : [1767225600000])); }
    static now() { return 1767225600000; }
    getTimezoneOffset() { return -480; }
  };
  context = vm.createContext({
    console: { log() {}, warn() {}, error() {}, debug() {} },
    Uint8Array, Uint8ClampedArray, DataView, TextDecoder, Date: FixedDate,
    URL, URLSearchParams, Promise,
    setTimeout: schedule,
    clearTimeout(timer) { timers.delete(timer); clearTimeout(timer); },
    window: { location }, location,
    navigator: { bluetooth: { async requestDevice(request) {
      events.push('request-device');
      if (options.requestCancelled) {
        const error = new Error('user cancelled');
        error.name = 'NotFoundError';
        throw error;
      }
      assert.deepEqual(JSON.parse(JSON.stringify(request)), {
        optionalServices: [serviceUuid],
        ...(options.unfiltered ? { acceptAllDevices: true } : { filters: [{ namePrefix: 'NRF_EPD' }] }),
      });
      return device;
    } } },
    document: { body: {}, getElementById: element },
    alert(message) { events.push(`alert:${message}`); },
    confirm() { return options.confirm !== false; },
    record(message) { logs.push(message); },
  });
  for (const filename of ['ble_transfer.js', 'main.js']) {
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../html/legacy/js', filename), 'utf8'), context);
  }
  run('addLog = record; canvas = document.getElementById("canvas"); ctx = canvas.getContext("2d");');
  if (options.unfiltered) run('toggleDeviceFilter()');
  element('connectbutton').innerHTML = '连接';
  async function settle() {
    for (let turn = 0; turn < 100; turn++) {
      await new Promise(resolve => setTimeout(resolve, 2));
      if (![...timers.values()].some(delay => delay <= 1500) && !asynchronous.size && !run('isConnecting')) {
        assert.deepEqual(errors, [], 'scheduled connection callbacks must settle without exceptions');
        return;
      }
    }
    assert.fail('legacy connection did not settle');
  }
  async function connect() { await run('preConnect()'); await settle(); }
  return { run, events, writes, logs, element, device, location, notify, connect, settle };
}

function assertConnected(state, version) {
  assert.equal(state.run('appVersion'), version);
  assert.equal(state.run('isConnecting'), false);
  assert.equal(state.device.gatt.connected, true);
  assert.equal(state.element('connectbutton').innerHTML, '断开');
  assert.equal(!!state.element('sendimgbutton').disabled, false);
  assert.equal(state.element('epdpins').value, '0102030405060708');
  assert.equal(state.element('epddriver').value, '07');
  assert.equal(Number(state.element('mtusize').value), 244);
  assert.ok(state.logs.includes('v=3300'), 'voltage notification must reach handleNotify');
  assert.equal(state.run('BleTransfer.isConnected()'), true);
  assert.equal(state.run('BleTransfer.statusResolver'), null);
}

for (const version of [0x16, 0x19, 0x20, 0x21]) {
  test(`legacy 0x${version.toString(16)} selects, reads version, initializes and consumes device information`, async () => {
    const state = fixture(version);
    await state.connect();
    assertConnected(state, version);
    assert.equal(state.events[0], 'request-device');
    assert.ok(state.events.indexOf('read-version') < state.events.indexOf('notifications'));
    assert.deepEqual(state.writes, [[1]]);
    assert.equal(state.element('legacyclockbutton').style.display, 'inline-block');
  });

  test(`legacy 0x${version.toString(16)} preserves configuration arriving inside startNotifications`, async () => {
    const state = fixture(version, { earlyConfig: true });
    await state.connect();
    assertConnected(state, version);
    assert.ok(state.events.indexOf('listener') < state.events.indexOf('notifications'));
    assert.deepEqual(state.writes, [[1]]);
  });

  test(`legacy 0x${version.toString(16)} sets the week start and time using legacy mode packets`, async () => {
    const state = fixture(version);
    await state.connect();
    await state.run('syncTime(1)');
    assert.deepEqual(state.writes.slice(1), [[0x21, 1], [0x20, 0x69, 0x55, 0xb9, 0x00, 8, 1]]);
    await state.run('syncTime(2)');
    assert.deepEqual(state.writes.at(-1), [0x20, 0x69, 0x55, 0xb9, 0x00, 8, 2]);
    assert.equal(state.writes.filter(packet => packet[0] === 0x21).length, 1);
    state.element('weekStart').value = '0';
    await state.run('syncTime(1)');
    assert.deepEqual(state.writes.at(-2), [0x21, 0]);
  });
}

test('legacy notification parsing accepts MTU and voltage before the configuration packet', async () => {
  const state = fixture(0x21, { earlyConfig: true, earlyText: true });
  await state.connect();
  assertConnected(state, 0x21);
});

test('legacy device selection also supports the unfiltered system picker', async () => {
  const state = fixture(0x21, { unfiltered: true });
  await state.connect();
  assertConnected(state, 0x21);
});

test('cancelling the legacy device picker does not connect or initialize', async () => {
  const state = fixture(0x21, { requestCancelled: true });
  await state.connect();
  assert.equal(state.device.gatt.connected, false);
  assert.deepEqual(state.writes, []);
  assert.deepEqual(state.events, ['request-device']);
  assert.equal(state.element('connectbutton').innerHTML, '连接');
});

test('legacy upgrade button stays disabled before connection, while connected and after disconnect', async () => {
  const state = fixture(0x21);
  const assertDisabled = () => {
    state.element('openDfuPageButton').disabled = false;
    state.run('updateButtonStatus()');
    assert.equal(state.element('openDfuPageButton').disabled, true);
  };
  assertDisabled();
  await state.connect();
  assertDisabled();
  state.device.gatt.disconnect();
  assertDisabled();
});

test('legacy upgrade entry only reports development status without changing Bluetooth or navigation', async () => {
  const state = fixture(0x21);
  const assertBlocked = async () => {
    const before = { events: [...state.events], writes: state.writes.map(packet => [...packet]),
      connected: state.device.gatt.connected, href: state.location.href };
    assert.equal(await state.run('openDfuPage()'), false);
    assert.deepEqual(state.events, before.events);
    assert.deepEqual(state.writes, before.writes);
    assert.equal(state.device.gatt.connected, before.connected);
    assert.equal(state.location.href, before.href);
    assert.match(state.element('toast').innerText, /固件升级开发中/);
  };
  await assertBlocked();
  await state.connect();
  await assertBlocked();
  state.device.gatt.disconnect();
  await assertBlocked();
});

test('legacy upgrade controls and the direct upgrade page are static local development placeholders', () => {
  const legacyRoot = path.join(__dirname, '../html/legacy');
  const home = fs.readFileSync(path.join(legacyRoot, 'index.html'), 'utf8');
  const button = home.match(/<button\b[^>]*\bid="openDfuPageButton"[^>]*>[\s\S]*?<\/button>/)?.[0];
  assert.ok(button);
  assert.match(button, /\sdisabled(?:[\s=>])/);
  assert.match(button, /固件升级.*开发中/);
  const page = fs.readFileSync(path.join(legacyRoot, 'dfu.html'), 'utf8');
  assert.match(page, /开发中/);
  assert.match(page, /<a\b[^>]*\bhref="index\.html"/);
  assert.doesNotMatch(page, /<(?:script|input|button|select|textarea|form)\b/i);
  const assets = [...page.matchAll(/<(?:link|img)\b[^>]*\b(?:src|href)=["']([^"']+)["']/gi)]
    .map(match => match[1].split(/[?#]/)[0]);
  assert.deepEqual(assets.sort(), ['css/main.css', 'favicon.png']);
  for (const asset of assets) {
    const filename = path.resolve(legacyRoot, asset);
    assert.equal(path.relative(legacyRoot, filename).startsWith('..'), false);
    assert.ok(fs.statSync(filename).isFile(), asset);
  }
});

test('legacy page reconnects the selected device and reapplies configuration without another device picker', async () => {
  const state = fixture(0x21);
  await state.connect();
  state.device.gatt.disconnect();
  assert.equal(state.element('connectbutton').innerHTML, '连接');
  assert.equal(!!state.element('sendimgbutton').disabled, true);
  assert.equal(!!state.element('reconnectbutton').disabled, false);
  assert.equal(state.run('BleTransfer.isConnected()'), false);
  assert.equal(state.run('appVersion'), null);
  assert.equal(Number(state.element('mtusize').value), 20);
  await state.run('reConnect()');
  await state.settle();
  assertConnected(state, 0x21);
  assert.equal(state.events.filter(event => event === 'request-device').length, 1);
  assert.equal(state.events.filter(event => event === 'connect').length, 2);
  assert.deepEqual(state.writes, [[1], [1]]);
});

test('reusing a GATT characteristic on reconnect does not process notifications through stale listeners', async () => {
  const state = fixture(0x21, { reuseCharacteristic: true, earlyConfig: true });
  await state.connect();
  state.device.gatt.disconnect();
  await state.run('reConnect()');
  await state.settle();
  assertConnected(state, 0x21);
  const before = state.logs.length;
  state.notify('v=3123');
  assert.deepEqual(state.logs.slice(before), ['v=3123']);
});

test('legacy configuration tails are optional across the old 8 to 13 byte records', async () => {
  const state = fixture(0x21);
  await state.connect();
  const full = Uint8Array.of(1, 2, 3, 4, 5, 6, 7, 7, 0, 0, 8, 0, 6, 0);
  for (const length of [8, 10, 11, 12, 13]) {
    state.element('weekStart').value = '3';
    state.notify(full.slice(0, length));
    assert.equal(state.element('epdpins').value, length > 10 ? '0102030405060708' : '01020304050607', `${length} bytes`);
    assert.equal(state.element('epddriver').value, '07');
    assert.equal(state.element('weekStart').value, length > 12 ? '6' : '3');
  }
});

test('a packet with an unknown driver is logged without replacing the selected configuration', async () => {
  const state = fixture(0x21);
  await state.connect();
  const pins = state.element('epdpins').value;
  const unknown = Uint8Array.of(9, 9, 9, 9, 9, 9, 9, 0xfe, 0, 0, 9, 0, 6, 0);
  const before = state.logs.length;
  state.notify(unknown);
  assert.equal(state.element('epdpins').value, pins);
  assert.equal(state.element('epddriver').value, '07');
  assert.equal(state.element('weekStart').value, '1');
  assert.equal(state.logs.length, before + 1);
  assert.equal(state.logs.at(-1).startsWith('收到配置'), false);
});

test('a pending legacy mode change cannot send its time packet to a replacement connection', async () => {
  let release;
  const blocked = new Promise(resolve => { release = resolve; });
  const state = fixture(0x21, {
    onWrite(packet) { if (packet[0] === 0x21) return blocked; },
  });
  await state.connect();
  const oldModeChange = state.run('sendTimeCommand(2, "old clock request")');
  assert.deepEqual(state.writes.at(-1), [0x21, 1]);
  state.device.gatt.disconnect();
  await state.run('reConnect()');
  await state.settle();
  assertConnected(state, 0x21);
  release();
  assert.equal(await oldModeChange, false);
  assert.deepEqual(state.writes, [[1], [0x21, 1], [1]]);
  assert.equal(state.logs.some(message => message.includes('old clock request已启用')), false);
});

test('legacy CRC image packets fit a 20 byte payload and preserve every image byte', async () => {
  let state, session, total = 0;
  const received = new Map();
  state = fixture(0x20, { onWrite(packet) {
    if (packet[0] === 0x33) { session = packet[1]; received.clear(); }
    if (packet[0] === 0x31) {
      const blockId = packet[1] | packet[2] << 8;
      total = packet[3] | packet[4] << 8;
      received.set(blockId, Array.from(packet.slice(6, -2)));
    }
    if (packet[0] === 0x32) {
      const bitmap = new Uint8Array(Math.ceil(total / 8));
      for (const blockId of received.keys()) bitmap[blockId >> 3] |= 1 << (blockId & 7);
      state.notify(Uint8Array.of(0xa1, total & 255, total >> 8,
        received.size & 255, received.size >> 8, session, 1, ...bitmap));
    }
  } });
  await state.connect();
  state.element('mtusize').value = 20;
  assert.equal(await state.run('BleTransfer.sendImageWithResume(Uint8Array.from({length: 25}, (_, i) => i + 1), "bw")'), true);
  const blocks = state.writes.filter(packet => packet[0] === 0x31);
  assert.equal(total, 3);
  assert.deepEqual(blocks.map(packet => packet.length), [20, 20, 9]);
  assert.deepEqual(blocks.map(packet => packet[1] | packet[2] << 8), [0, 1, 2]);
  assert.deepEqual(blocks.map(packet => packet[5]), [0x0f, 0xff, 0xff]);
  assert.deepEqual([...received.values()].flat(), Array.from({ length: 25 }, (_, i) => i + 1));
  assert.deepEqual([...received.values()].map(bytes => bytes.length), [12, 12, 1]);
  for (const packet of blocks) {
    const crc = state.run(`BleTransfer.crc16(Uint8Array.of(${packet.slice(6, -2).join(',')}))`);
    assert.equal(packet.at(-2) | packet.at(-1) << 8, crc);
    assert.equal(packet.length <= 20, true);
  }
});

for (const accept of [true, false]) {
  test(`firmware 0x15 ${accept ? 'opens' : 'declines'} the older external page without sending INIT`, async () => {
    const state = fixture(0x15, { confirm: accept });
    await state.connect();
    assert.deepEqual(state.writes, []);
    assert.equal(state.events.includes('notifications'), false);
    assert.equal(state.element('connectbutton').innerHTML, '连接');
    assert.equal(state.location.href, accept ? 'https://tsl0922.github.io/EPD-nRF5/v1.5' : '/legacy/index.html');
    if (!accept) assert.equal(state.device.gatt.connected, false);
  });
}

for (const error of ['versionError', 'emptyVersion', 'notificationError', 'initError', 'initDisconnect']) {
  test(`legacy ${error} stops initialization and keeps the page disconnected`, async () => {
    const state = fixture(0x21, { [error]: true });
    await state.connect();
    assert.equal(state.element('connectbutton').innerHTML, '连接');
    assert.equal(!!state.element('sendimgbutton').disabled, true);
    assert.equal(state.device.gatt.connected, false);
    assert.equal(state.run('isConnecting'), false);
    assert.equal(state.run('epdCharacteristic'), null);
    assert.equal(state.logs.some(message => /已重新连接/.test(message)), false);
    if (!['initError', 'initDisconnect'].includes(error)) assert.deepEqual(state.writes, []);
  });
}
