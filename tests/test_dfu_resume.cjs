'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const filename = path.resolve(__dirname, '../html/js/dfu.js');
const source = fs.readFileSync(filename, 'utf8');
const endOfModule = /\}\)\(\);\s*$/;
assert.match(source, endOfModule);
const window = {};
vm.runInNewContext(source.replace(endOfModule,
  'window.__dfuResume = { LocalSecureDfu, makeCrc32, OP };})();'),
{ window, navigator: {}, TextDecoder, setTimeout, clearTimeout }, { filename });
const { LocalSecureDfu, makeCrc32, OP } = window.__dfuResume;
const crc32 = makeCrc32();
const PAGE = 4096;
const init = Uint8Array.from({ length: 64 }, (_, i) => (i * 17 + 5) & 255);
const firmware = Uint8Array.from({ length: PAGE * 2 + 808 },
  (_, i) => ((i * 13) ^ (i >>> 8)) & 255);

function response(words) {
  const view = new DataView(new ArrayBuffer(words.length * 4));
  words.forEach((word, i) => view.setUint32(i * 4, word >>> 0, true));
  return view;
}

function dfuError(code) {
  return Object.assign(new Error(`DFU result 0x${code.toString(16)}`), { dfuResult: code });
}

// The model keeps received and committed offsets separate, matching the bundled
// SDK's nrf_dfu_req_handler.c CREATE (447-449) and EXECUTE (600-617). Repeated
// EXECUTE is allowed after commit; an empty newly created object rejects it.
function rig({
  initOffset = 0, initExecuted = false, dataOffset = 0, committed = 0,
  objectSize = dataOffset > committed ? Math.min(PAGE, firmware.length - committed) : 0,
  corruptInit = false, corruptDataAt = null, maxSize = PAGE,
  executeError = null, checksumOffset = null, checksumCorrupt = false,
} = {}) {
  const state = {
    initOffset, initExecuted, dataOffset, committed, objectSize,
    initSize: init.length, current: 'init', trace: [], packets: [], verifyNextSelect: false,
    init: new Uint8Array(init.length), data: new Uint8Array(firmware.length),
  };
  state.init.set(init.slice(0, initOffset));
  state.data.set(firmware.slice(0, dataOffset));
  if (corruptInit) state.init[0] ^= 1;
  if (corruptDataAt !== null) state.data[corruptDataAt] ^= 1;
  const client = new LocalSecureDfu(crc32, {}, 0, { prn: 0 });
  client.delay = 0;
  client.delayPromise = async () => {};
  client.packetSize = 244;
  client.writePacket = async packet => {
    const bytes = new Uint8Array(packet);
    state.packets.push({ type: state.current, length: bytes.length });
    state.verifyNextSelect = true;
    if (state.current === 'init') {
      assert.ok(state.initOffset + bytes.length <= state.initSize);
      state.init.set(bytes, state.initOffset);
      state.initOffset += bytes.length;
    } else {
      assert.ok(state.initExecuted, 'data requires an executed init');
      assert.ok(state.dataOffset - state.committed + bytes.length <= state.objectSize);
      state.data.set(bytes, state.dataOffset);
      state.dataOffset += bytes.length;
    }
  };
  client.sendControl = async (operation, payload) => {
    const name = Object.keys(OP).find(key => OP[key] === operation);
    state.trace.push({ name, current: state.current, offset: state.dataOffset, committed: state.committed });
    if (name === 'SELECT_COMMAND') {
      state.current = 'init';
      const offset = state.verifyNextSelect ? (checksumOffset ?? state.initOffset) : state.initOffset;
      const crc = crc32(state.init.slice(0, offset)) ^ (state.verifyNextSelect && checksumCorrupt ? 1 : 0);
      state.verifyNextSelect = false;
      return response([256, offset, crc]);
    }
    if (name === 'SELECT_DATA') {
      state.current = 'data';
      const offset = state.verifyNextSelect ? (checksumOffset ?? state.dataOffset) : state.dataOffset;
      const crc = crc32(state.data.slice(0, offset)) ^ (state.verifyNextSelect && checksumCorrupt ? 1 : 0);
      state.verifyNextSelect = false;
      return response([maxSize, offset, crc]);
    }
    if (name === 'CREATE_COMMAND') {
      state.current = 'init';
      state.initOffset = 0;
      state.initExecuted = false;
      state.initSize = new DataView(payload).getUint32(0, true);
      // nrf_dfu_validation_init_cmd_create resets all progress, not just init.
      state.dataOffset = state.committed = state.objectSize = 0;
      return;
    }
    if (name === 'CREATE_DATA') {
      if (!state.initExecuted) throw dfuError(0x08);
      const size = new DataView(payload).getUint32(0, true);
      if (state.committed + size > firmware.length) throw dfuError(0x08);
      if (size !== PAGE && state.committed + size !== firmware.length) throw dfuError(0x03);
      state.current = 'data';
      state.objectSize = size;
      state.dataOffset = state.committed;
      return;
    }
    if (name === 'EXECUTE') {
      if (executeError !== null) throw dfuError(executeError);
      if (state.current === 'init') {
        if (state.initOffset !== state.initSize) throw dfuError(0x08);
        assert.deepEqual(state.init, init);
        state.initExecuted = true;
      } else {
        if (state.dataOffset - state.committed !== state.objectSize) throw dfuError(0x08);
        state.objectSize = 0;
        state.committed = state.dataOffset;
      }
      return;
    }
    throw new Error(`Unexpected operation ${name}`);
  };
  return { client, state };
}

async function sendAll(client) {
  await client.transfer(init.buffer, 'init', OP.SELECT_COMMAND, OP.CREATE_COMMAND);
  await client.transfer(firmware.buffer, 'firmware', OP.SELECT_DATA, OP.CREATE_DATA);
}

function dataTransfer(client) {
  return client.transfer(firmware.buffer, 'firmware', OP.SELECT_DATA, OP.CREATE_DATA);
}

function names(state) {
  return state.trace.map(entry => entry.name);
}

test('fresh init and multiple firmware objects complete byte-exactly', async () => {
  const { client, state } = rig();
  await sendAll(client);
  assert.equal(state.committed, firmware.length);
  assert.deepEqual(state.data, firmware);
});

for (const initExecuted of [false, true]) {
  test(`complete init is executed on resume (previously executed: ${initExecuted})`, async () => {
    const { client, state } = rig({ initOffset: init.length, initExecuted });
    await sendAll(client);
    assert.deepEqual(names(state).slice(0, 3), ['SELECT_COMMAND', 'EXECUTE', 'SELECT_DATA']);
    assert.equal(state.packets.some(packet => packet.type === 'init'), false);
    assert.equal(state.committed, firmware.length);
    assert.deepEqual(state.data, firmware);
  });
}

for (const options of [{ initOffset: 23 }, { initOffset: init.length, corruptInit: true }]) {
  test(`incomplete or corrupt init restarts safely: ${JSON.stringify(options)}`, async () => {
    const { client, state } = rig(options);
    await sendAll(client);
    assert.equal(names(state)[1], 'CREATE_COMMAND');
    assert.equal(state.committed, firmware.length);
    assert.deepEqual(state.data, firmware);
  });
}

for (const [description, options] of [
  ['full first page not executed', { dataOffset: PAGE }],
  ['full second page not executed', { dataOffset: PAGE * 2, committed: PAGE }],
  ['first page already executed', { dataOffset: PAGE, committed: PAGE }],
  ['next page created with no bytes', { dataOffset: PAGE, committed: PAGE, objectSize: PAGE }],
  ['empty first page already created', { objectSize: PAGE }],
  ['partial first page', { dataOffset: 1222 }],
  ['partial second page', { dataOffset: PAGE + 1222, committed: PAGE }],
  ['partial final page', { dataOffset: PAGE * 2 + 300, committed: PAGE * 2 }],
  ['full final page not executed', { dataOffset: firmware.length, committed: PAGE * 2 }],
  ['whole firmware already executed', { dataOffset: firmware.length, committed: firmware.length }],
]) {
  test(`firmware resume: ${description}`, async () => {
    const { client, state } = rig({ initOffset: init.length, initExecuted: true, ...options });
    await sendAll(client);
    assert.equal(state.committed, firmware.length);
    assert.deepEqual(state.data, firmware);
    assert.equal(names(state).includes('CREATE_COMMAND'), false, 'valid init must preserve progress');
    if (options.dataOffset === PAGE && !options.objectSize) {
      const selected = names(state).indexOf('SELECT_DATA');
      assert.equal(names(state)[selected + 1], 'EXECUTE', 'commit boundary before CREATE_DATA');
    }
  });
}

for (const options of [
  { dataOffset: PAGE, corruptDataAt: PAGE - 1 },
  { dataOffset: PAGE + 1222, committed: PAGE, corruptDataAt: PAGE + 2 },
  { dataOffset: firmware.length, committed: PAGE * 2, corruptDataAt: firmware.length - 1 },
]) {
  test(`corrupt uncommitted object is recreated from intact prefix: ${options.dataOffset}`, async () => {
    const { client, state } = rig({ initOffset: init.length, initExecuted: true, ...options });
    await dataTransfer(client);
    assert.equal(names(state)[1], 'CREATE_DATA', 'corrupt bytes must never be executed');
    assert.equal(state.committed, firmware.length);
    assert.deepEqual(state.data, firmware);
  });
}

for (const dataOffset of [PAGE, PAGE + 1222]) {
  test(`corrupt committed prefix rejects before packet writes: ${dataOffset}`, async () => {
    const { client, state } = rig({
      initOffset: init.length, initExecuted: true, dataOffset, committed: PAGE, corruptDataAt: 5,
    });
    await assert.rejects(dataTransfer(client), /已提交数据与固件不一致/);
    assert.equal(state.committed, PAGE);
    assert.equal(state.packets.length, 0);
    assert.equal(names(state).includes('EXECUTE'), false);
  });
}

for (const options of [{ maxSize: 0 }, { dataOffset: firmware.length + 1 }]) {
  test(`invalid SELECT response rejects before creating an object: ${JSON.stringify(options)}`, async () => {
    const { client, state } = rig({ initExecuted: true, ...options });
    await assert.rejects(dataTransfer(client), /无效|超出固件长度/);
    assert.deepEqual(names(state), ['SELECT_DATA']);
    assert.equal(state.packets.length, 0);
  });
}

for (const options of [{ checksumOffset: 100 }, { checksumCorrupt: true }]) {
  test(`checksum must have expected offset and CRC: ${JSON.stringify(options)}`, async () => {
    const { client, state } = rig({ initExecuted: true, ...options });
    await assert.rejects(dataTransfer(client), /CRC/);
    assert.equal(state.committed, 0);
    assert.equal(names(state).includes('EXECUTE'), false);
  });
}

for (const code of [0x05, 0x0a]) {
  test(`boundary EXECUTE failure 0x${code.toString(16)} is not hidden`, async () => {
    const { client, state } = rig({ initExecuted: true, dataOffset: PAGE, executeError: code });
    await assert.rejects(dataTransfer(client), error => error.dfuResult === code);
    assert.deepEqual(names(state), ['SELECT_DATA', 'EXECUTE']);
    assert.equal(state.packets.length, 0);
  });
}

test('0x08 cannot hide an init validation failure', async () => {
  const { client, state } = rig({ initOffset: init.length, executeError: 0x08 });
  await assert.rejects(sendAll(client), error => error.dfuResult === 0x08);
  assert.deepEqual(names(state), ['SELECT_COMMAND', 'EXECUTE']);
});

test('0x08 cannot hide a final firmware execute failure', async () => {
  const { client, state } = rig({
    initExecuted: true, dataOffset: firmware.length, committed: PAGE * 2, executeError: 0x08,
  });
  await assert.rejects(dataTransfer(client), error => error.dfuResult === 0x08);
  assert.deepEqual(names(state), ['SELECT_DATA', 'EXECUTE']);
});

test('control notification preserves numeric DFU result for boundary recovery', async () => {
  const client = new LocalSecureDfu(crc32, {}, 0, { prn: 0 });
  client.delay = 0;
  client.controlChar = {
    async writeValue(bytes) {
      assert.equal(bytes[0], OP.EXECUTE[0]);
      client.handleNotification({
        target: { value: new DataView(Uint8Array.of(0x60, OP.EXECUTE[0], 0x08).buffer) },
      });
    },
  };
  await assert.rejects(client.sendControl(OP.EXECUTE), error => error.dfuResult === 0x08);
});
