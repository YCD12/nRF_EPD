const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

const root = path.join(__dirname, '..');

function fixture(version = 0x20, choice = 'legacy', options = {}) {
  const events = [], writes = [], logs = [], elements = new Map(), stored = new Map();
  const listeners = new Map();
  let context;
  const run = code => vm.runInContext(code, context);
  const element = id => {
    if (!elements.has(id)) {
      const handlers = new Map();
      elements.set(id, {
        value: id === 'mtusize' ? 244 : 0, style: {}, childNodes: [],
        classList: { toggle() {}, remove() {} },
        addEventListener(type, callback, flags) {
          if (!handlers.has(type)) handlers.set(type, []);
          handlers.get(type).push({ callback, once: flags?.once });
        },
        showModal() {
          events.push({ type: 'dialog' });
          queueMicrotask(() => {
            this.returnValue = choice;
            const active = handlers.get('close') || [];
            handlers.set('close', active.filter(handler => !handler.once));
            for (const handler of active) handler.callback();
          });
        },
      });
    }
    return elements.get(id);
  };
  const characteristic = {
    addEventListener() {},
    async startNotifications() { events.push({ type: 'notifications' }); },
    async writeValueWithResponse(data) {
      writes.push(Array.from(data));
      if (data[0] === 1 && data.length === 3)
        run(`handleNotify(new DataView(Uint8Array.of(0xA6, ${data[2]}, 0, 244, 0).buffer))`);
    },
  };
  const device = {
    name: 'NRF_EPD_COMPAT',
    addEventListener(type, callback) {
      if (!listeners.has(type)) listeners.set(type, new Set());
      listeners.get(type).add(callback);
    },
    removeEventListener(type, callback) { listeners.get(type)?.delete(callback); },
    gatt: {
      connected: true,
      disconnect() {
        events.push({ type: 'disconnect' });
        if (options.disconnectThrows) throw new Error('disconnect failed');
        if (options.disconnectStaysConnected) return;
        this.connected = false;
        if (options.emitDisconnect) {
          run('disconnect()');
          for (const callback of listeners.get('gattserverdisconnected') || []) callback();
        }
      },
      async getPrimaryService() {
        return { async getCharacteristic(uuid) {
          if (!uuid.includes('0003-')) return characteristic;
          return { async readValue() {
            events.push({ type: 'version' });
            if (options.versionError) throw new Error('version unavailable');
            return new DataView(Uint8Array.of(version).buffer);
          } };
        } };
      },
    },
  };
  const location = { pathname: '/console/index.html' };
  Object.defineProperty(location, 'href', {
    get() { return events.findLast(event => event.type === 'navigate')?.page; },
    set(page) {
      events.push({ type: 'navigate', page, connected: device.gatt.connected,
        clean: run('gattServer === null && epdCharacteristic === null') });
    },
  });
  context = vm.createContext({
    console: { log() {}, error() {} }, Uint8Array, DataView, TextDecoder,
    URL, URLSearchParams, Date, Promise,
    setTimeout(callback, delay) { return setTimeout(callback, delay < 1000 ? 0 : delay); },
    clearTimeout,
    window: { location },
    sessionStorage: { setItem(key, value) { stored.set(key, value); } },
    document: { body: {}, getElementById: element, querySelectorAll: () => [] },
    device,
    record: message => logs.push(message),
  });
  if (options.installer) context.window.ExternalResourceInstaller = {
    controls() {}, busy() { return false; }, disconnect() {}, notification() { return false; },
    bind(target) {
      assert.equal(target, characteristic);
      events.push({ type: 'resource-bind' });
    },
    async refresh(owner) {
      assert.equal(owner, run('DeviceOperations.current'));
      events.push({ type: 'resource-refresh', commands: writes.map(packet => packet[0]) });
    },
  };
  let source = fs.readFileSync(path.join(root, 'html/js/main.js'), 'utf8');
  if (options.newWebMinVersion !== undefined) {
    const declaration = /\bconst NEW_WEB_MIN_FIRMWARE_VERSION\s*=\s*0x[0-9a-f]+\s*;/i;
    assert.match(source, declaration);
    source = source.replace(declaration,
      `const NEW_WEB_MIN_FIRMWARE_VERSION = 0x${options.newWebMinVersion.toString(16)};`);
  }
  vm.runInContext(source, context);
  run('bleDevice = device; addLog = record;');
  return { run, events, writes, logs, elements, stored, device, location };
}

function assertNoInitialization(state) {
  assert.deepEqual(state.writes, []);
  assert.equal(state.events.some(event => event.type === 'notifications'), false);
  assert.equal(state.run('isConnecting'), false);
  assert.equal(state.run('DeviceOperations.current'), null);
}

for (const version of [0x00, 0x19, 0x20]) {
  test(`firmware 0x${version.toString(16)} opens the old page without explicitly disconnecting`, async () => {
    const state = fixture(version);
    await state.run('connect()');
    assertNoInitialization(state);
    assert.deepEqual(state.events.map(event => event.type), ['version', 'dialog', 'navigate']);
    assert.deepEqual(state.events.at(-1), {
      type: 'navigate', page: 'legacy/index.html', connected: true, clean: false,
    });
    assert.match(state.elements.get('legacyFirmwareMessage').textContent, /0x21/);
    assert.equal(state.stored.size, 0);
  });
}

test('changing the shared page boundary updates legacy routing and its dialog message', async () => {
  const state = fixture(0x23, 'legacy', { newWebMinVersion: 0x24 });
  await state.run('connect()');
  assertNoInitialization(state);
  assert.equal(state.location.href, 'legacy/index.html');
  assert.match(state.elements.get('legacyFirmwareMessage').textContent, /0x24/);
  assert.equal(state.events.some(event => event.type === 'disconnect'), false);
});

test('the changed page boundary permits its exact version without changing protocol opcodes', async () => {
  const state = fixture(0x24, 'legacy', { newWebMinVersion: 0x24 });
  await state.run('connect()');
  assert.equal(state.events.some(event => event.type === 'dialog'), false);
  assert.equal(state.device.gatt.connected, true);
  assert.deepEqual(state.writes.map(packet => packet[0]), [1, 0x23]);
  assert.equal(state.run('EpdCmd.SET_MODE'), 0x22);
  assert.equal(state.location.href, undefined);
});

test('an unavailable upgrade choice cancels the legacy connection without entering DFU', async () => {
  const state = fixture(0x20, 'upgrade', { emitDisconnect: true });
  await state.run('connect()');
  assertNoInitialization(state);
  assert.equal(state.location.href, undefined);
  assert.equal(state.device.gatt.connected, false);
  assert.equal(state.stored.size, 0);
  assert.deepEqual(state.events.map(event => event.type), ['version', 'dialog', 'disconnect']);
  assert.match(state.elements.get('toast').innerText, /开发中/);
});

for (const choice of ['cancel', '']) {
  test(`closing the old-firmware dialog with ${JSON.stringify(choice)} disconnects without navigation`, async () => {
    const state = fixture(0x20, choice);
    await state.run('connect()');
    assertNoInitialization(state);
    assert.equal(state.device.gatt.connected, false);
    assert.equal(state.location.href, undefined);
  });
}

test('unreadable firmware does not open a legacy prompt or send commands', async () => {
  const state = fixture(0x20, 'legacy', { versionError: true });
  await state.run('connect()');
  assertNoInitialization(state);
  assert.equal(state.events.some(event => event.type === 'dialog'), false);
  assert.equal(state.location.href, undefined);
  assert.equal(state.device.gatt.connected, false);
  assert.ok(state.logs.some(message => message.includes('无法读取固件版本')));
});

for (const version of [0x21, 0x22, 0x31, 0x32, 0x36, 0x37, 0x38]) {
  test(`firmware 0x${version.toString(16)} initializes without legacy navigation`, async () => {
    const state = fixture(version);
    await state.run('connect()');
    assert.equal(state.events.some(event => event.type === 'dialog'), false);
    assert.equal(state.events.some(event => event.type === 'notifications'), true);
    assert.deepEqual(state.writes.map(packet => packet[0]), [1, 0x23]);
    assert.equal(state.writes[0].length, 3);
    assert.equal(state.device.gatt.connected, true);
    assert.equal(state.location.href, undefined);
    assert.equal(state.run('isConnecting'), false);
    assert.equal(state.run('DeviceOperations.current'), null);
  });
}

for (const mode of ['disconnectThrows', 'disconnectStaysConnected']) {
  test(`disabled DFU navigation never attempts Bluetooth disconnect (${mode})`, async () => {
    const state = fixture(0x20, 'upgrade', { [mode]: true });
    assert.equal(await state.run("navigateToDevicePage('dfu.html')"), false);
    assert.equal(state.location.href, undefined);
    assert.equal(state.events.some(event => event.type === 'disconnect'), false);
    assert.equal(state.stored.size, 0);
    assert.equal(state.run('internalDisconnectInProgress'), false);
    assert.match(state.elements.get('toast').innerText, /开发中/);
  });
}

test('calling the disabled upgrade entry reports development status without touching Bluetooth', async () => {
  const state = fixture(0x40);
  assert.equal(await state.run('openDfuPage()'), false);
  assert.equal(state.location.href, undefined);
  assert.equal(state.events.length, 0);
  assert.equal(state.stored.size, 0);
  assert.match(state.elements.get('toast').innerText, /开发中/);
});

test('connection and operation state updates keep the upgrade button disabled', () => {
  const state = fixture(0x40);
  for (const setup of [
    'gattServer = null; isConnecting = false; DeviceOperations.current = null;',
    'gattServer = device.gatt; isConnecting = false; DeviceOperations.current = null;',
    'gattServer = device.gatt; isConnecting = true; DeviceOperations.current = null;',
    'gattServer = device.gatt; isConnecting = false; DeviceOperations.current = {kind: "control"};',
  ]) {
    state.run(setup + 'updateButtonStatus();');
    assert.equal(state.elements.get('openDfuPageButton').disabled, true);
  }
});

test('the home page prioritizes the legacy page and presents upgrade controls as unavailable', () => {
  const html = fs.readFileSync(path.join(root, 'html/index.html'), 'utf8');
  const firstScript = html.match(/<script\b[^>]*\bsrc="([^"]+)"[^>]*>/)?.[0];
  assert.match(firstScript, /\bsrc="js\/main\.js(?:\?|\")/);
  assert.match(firstScript, /\sdefer(?:[\s=>])/);
  const header = html.match(/<button\b[^>]*\bid="openDfuPageButton"[^>]*>[\s\S]*?<\/button>/)?.[0];
  assert.ok(header);
  assert.match(header, /\sdisabled(?:[\s=>])/);
  assert.match(header, /开发中/);
  const dialog = html.match(/<dialog\b[^>]*\bid="legacyFirmwareDialog"[^>]*>([\s\S]*?)<\/dialog>/)?.[1];
  assert.ok(dialog);
  const legacy = dialog.match(/<button\b[^>]*\bvalue="legacy"[^>]*>/)?.[0];
  const upgrade = dialog.match(/<button\b[^>]*\bvalue="upgrade"[^>]*>[\s\S]*?<\/button>/)?.[0];
  assert.match(legacy, /\bclass="[^"]*\bprimary\b/);
  assert.match(legacy, /\sautofocus(?:[\s=>])/);
  assert.match(upgrade, /\sdisabled(?:[\s=>])/);
  assert.match(upgrade, /开发中/);
});

test('opening the upgrade page directly shows development status without loading DFU runtimes', () => {
  const html = fs.readFileSync(path.join(root, 'html/dfu.html'), 'utf8');
  assert.match(html, /开发中/);
  const scripts = [...html.matchAll(/<script\b[^>]*\bsrc=["']([^"']+)["']/gi)].map(match => match[1]);
  assert.equal(scripts.some(script => /(?:^|\/)dfu(?:_page)?\.js(?:[?#]|$)/.test(script)), false);
  const retiredWorkbench = html.match(/<(?:main|section|div)\b[^>]*\bhidden(?:[\s=>])[^>]*>/g) || [];
  assert.ok(retiredWorkbench.some(tag => /\sinert(?:[\s=>])/.test(tag)));
});

for (const version of [0x21, 0x22, 0x33, 0x34, 0x38]) {
  test(`supported firmware 0x${version.toString(16)} initializes resources after INIT and time sync`, async () => {
    const state = fixture(version, 'legacy', { installer: true });
    await state.run('connect()');
    assert.deepEqual(state.writes.map(packet => packet[0]), [1, 0x23]);
    assert.equal(state.device.gatt.connected, true);
    assert.equal(state.location.href, undefined);
    const resourceEvents = state.events.filter(event => event.type.startsWith('resource-'));
    assert.deepEqual(resourceEvents, [
      { type: 'resource-bind' },
      { type: 'resource-refresh', commands: [1, 0x23] },
    ]);
  });
}

test('legacy navigation never calls the device disconnect method', async () => {
  const state = fixture(0x20, 'legacy', { disconnectThrows: true });
  await assert.doesNotReject(state.run('connect()'));
  assertNoInitialization(state);
  assert.equal(state.location.href, 'legacy/index.html');
  assert.equal(state.events.some(event => event.type === 'disconnect'), false);
  assert.equal(state.stored.size, 0);
});

test('connect handles a cancellation disconnect exception without navigating or rejecting its promise', async () => {
  const state = fixture(0x20, 'cancel', { disconnectThrows: true });
  await assert.doesNotReject(state.run('connect()'));
  assertNoInitialization(state);
  assert.equal(state.device.gatt.connected, true);
  assert.equal(state.location.href, undefined);
  assert.ok(state.logs.some(message => message.includes('disconnect failed')));
});

test('legacy page and its runtime assets are self-contained in the legacy directory', () => {
  const entry = path.join(root, 'html/legacy/index.html');
  const legacyRoot = path.dirname(entry);
  const html = fs.readFileSync(entry, 'utf8');
  const assets = [...html.matchAll(/<(?:script|link|img)\b[^>]*\b(?:src|href)=["']([^"']+)["']/gi)]
    .map(match => match[1].split(/[?#]/)[0]);
  assert.ok(assets.some(asset => asset === 'js/main.js'));
  const checked = new Set();
  function checkAsset(asset, owner = entry) {
    assert.ok(asset && !/^(?:[a-z][a-z\d+.-]*:|[\\/])/i.test(asset), `resource must be local: ${asset}`);
    const filename = path.resolve(path.dirname(owner), decodeURIComponent(asset));
    const relative = path.relative(legacyRoot, filename);
    assert.ok(!relative.startsWith('..') && !path.isAbsolute(relative), `resource leaves legacy: ${asset}`);
    assert.ok(fs.statSync(filename).isFile(), `missing resource: ${asset}`);
    const realRelative = path.relative(legacyRoot, fs.realpathSync(filename));
    assert.ok(!realRelative.startsWith('..') && !path.isAbsolute(realRelative), `resource resolves outside legacy: ${asset}`);
    if (checked.has(filename)) return;
    checked.add(filename);
    if (filename.endsWith('.css')) {
      const css = fs.readFileSync(filename, 'utf8');
      for (const match of css.matchAll(/url\(\s*['"]?([^'"\s)]+)['"]?\s*\)|@import\s+['"]([^'"]+)['"]/gi)) {
        const reference = match[1] || match[2];
        if (!reference.startsWith('data:')) checkAsset(reference.split(/[?#]/)[0], filename);
      }
    }
  }
  for (const asset of assets) checkAsset(asset);
  const current = fs.readFileSync(path.join(root, 'html/index.html'), 'utf8');
  assert.match(current, /<dialog[^>]*id="legacyFirmwareDialog"/);
  assert.match(current, /<form[^>]*method="dialog"/);
  for (const choice of ['upgrade', 'legacy', 'cancel']) assert.match(current, new RegExp(`value="${choice}"`));
});
