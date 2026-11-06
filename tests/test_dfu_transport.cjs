'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const filename = path.resolve(__dirname, '../html/js/dfu.js');
const source = fs.readFileSync(filename, 'utf8');

function harness(prn = 0) {
  const timers = new Map();
  let nextTimer = 0;
  const window = {};
  vm.runInNewContext(source.replace(/\}\)\(\);\s*$/,
    'window.__transport = { LocalSecureDfu, makeCrc32, OP };})();'), {
    window, navigator: {}, TextDecoder,
    setTimeout(fn, ms) { const id = ++nextTimer; timers.set(id, { fn, ms }); return id; },
    clearTimeout(id) { timers.delete(id); },
  }, { filename });
  const { LocalSecureDfu, makeCrc32, OP } = window.__transport;
  const crc32 = makeCrc32();
  const client = new LocalSecureDfu(crc32, {}, 0, { prn });
  client.delay = 0;
  return {
    client, crc32, OP, timers,
    fire(ms) {
      const pending = [...timers].filter(([, timer]) => timer.ms === ms);
      assert.ok(pending.length, `expected a ${ms}ms timer`);
      for (const [id, timer] of pending) { timers.delete(id); timer.fn(); }
    },
  };
}

async function flush() {
  for (let i = 0; i < 12; i++) await Promise.resolve();
}

function characteristic() {
  const listeners = new Set();
  return {
    addEventListener(event, listener) { listeners.add(listener); },
    removeEventListener(event, listener) { listeners.delete(listener); },
    notify(opcode, words = [], result = 1) {
      const bytes = new Uint8Array(3 + 4 * words.length);
      bytes.set([0x60, opcode, result]);
      this.value = new DataView(bytes.buffer);
      words.forEach((word, i) => this.value.setUint32(3 + i * 4, word >>> 0, true));
      for (const listener of listeners) listener({ target: this });
    },
  };
}

function device() {
  const listeners = new Set();
  return {
    gatt: { connected: true },
    addEventListener(event, listener) { listeners.add(listener); },
    removeEventListener(event, listener) { listeners.delete(listener); },
    disconnect() {
      this.gatt.connected = false;
      for (const listener of listeners) listener();
    },
  };
}

function bind(client, control) {
  client.controlChar = control;
  client.bindNotificationHandler(control, 'control');
}

test('a late CREATE response cancels the delayed retry after packets have started', async () => {
  const { client, OP, fire, timers } = harness();
  const control = characteristic();
  bind(client, control);
  let creates = 0;
  let received = 0;
  control.writeValue = async bytes => {
    if (bytes[0] === OP.CREATE_DATA[0]) {
      creates++;
      received = 0;
      throw new Error('GATT operation failed after the device received CREATE');
    }
    control.notify(bytes[0], [4096, received, 0]);
  };
  const creation = client.sendControl(OP.CREATE_DATA, new Uint32Array([40]).buffer);
  await flush();
  control.notify(OP.CREATE_DATA[0]);
  await creation;
  received = 20;
  const selected = await client.sendControl(OP.SELECT_DATA);
  assert.equal(selected.getUint32(4, true), 20);
  fire(500);
  await flush();
  assert.equal(creates, 1, 'a stale CREATE must not erase the next object');
  assert.equal(received, 20);
  assert.equal(timers.size, 0);
});

test('notification before write rejection does not schedule a retry', async () => {
  const { client, OP, timers } = harness();
  const control = characteristic();
  bind(client, control);
  let writes = 0;
  control.writeValue = async bytes => {
    writes++;
    control.notify(bytes[0]);
    throw new Error('late write rejection');
  };
  await client.sendControl(OP.EXECUTE);
  await flush();
  assert.equal(writes, 1);
  assert.equal(timers.size, 0);
});

test('an unanswered failed write can retry once and resolve through notifications', async () => {
  const { client, OP, fire, timers } = harness();
  const control = characteristic();
  bind(client, control);
  let writes = 0;
  control.writeValue = async bytes => {
    if (++writes === 1) throw new Error('GATT busy');
    control.notify(bytes[0]);
  };
  const operation = client.sendControl(OP.EXECUTE);
  await flush();
  fire(500);
  await operation;
  assert.equal(writes, 2);
  assert.equal(timers.size, 0);
  assert.equal(Object.keys(client.notifyFns).length, 0);
});

test('a failed retry rejects and removes the response timeout', async () => {
  const { client, OP, fire, timers } = harness();
  const control = characteristic();
  bind(client, control);
  let writes = 0;
  control.writeValue = async () => { writes++; throw new Error('persistent write failure'); };
  const rejected = assert.rejects(client.sendControl(OP.EXECUTE), /persistent write failure/);
  await flush();
  fire(500);
  await rejected;
  assert.equal(writes, 2);
  assert.equal(timers.size, 0);
});

test('response timeout cancels a delayed write retry', async () => {
  const { client, OP, fire, timers } = harness();
  const control = characteristic();
  bind(client, control);
  let writes = 0;
  control.writeValue = async () => { writes++; throw new Error('write rejected'); };
  const rejected = assert.rejects(client.sendControl(OP.EXECUTE), /DFU response timeout/);
  await flush();
  fire(10000);
  await rejected;
  fire(500);
  await flush();
  assert.equal(writes, 1);
  assert.equal(timers.size, 0);
});

for (const disconnect of [true, false]) {
  test(`a delayed retry cannot cross connection generations (disconnect=${disconnect})`, async () => {
    const { client, OP, fire, timers } = harness();
    const original = device();
    client.bindDisconnectHandler(original);
    const control = characteristic();
    bind(client, control);
    let writes = 0;
    control.writeValue = async () => { writes++; throw new Error('write rejected'); };
    const rejected = assert.rejects(client.sendControl(OP.EXECUTE), /disconnected|connection changed/);
    await flush();
    const staleDisconnect = client.disconnectHandler;
    if (disconnect) original.disconnect();
    original.gatt.connected = true;
    client.bindDisconnectHandler(original);
    const replacement = characteristic();
    bind(client, replacement);
    staleDisconnect();
    assert.equal(client.controlChar, replacement, 'an old disconnect must not clear the new connection');
    fire(500);
    await rejected;
    assert.equal(writes, 1);
    assert.equal(timers.size, 0);
  });
}

test('old retry cleanup preserves a new connection request with the same opcode', async () => {
  const { client, OP, fire, timers } = harness();
  client.bindDisconnectHandler(device());
  const original = characteristic();
  bind(client, original);
  original.writeValue = async () => { throw new Error('write rejected'); };
  const oldRejected = assert.rejects(client.sendControl(OP.EXECUTE), /connection changed/);
  await flush();
  client.bindDisconnectHandler(device());
  const replacement = characteristic();
  bind(client, replacement);
  replacement.writeValue = async () => {};
  const newOperation = client.sendControl(OP.EXECUTE);
  fire(500);
  await oldRejected;
  assert.ok(client.notifyFns[OP.EXECUTE[0]], 'old cleanup must leave the new waiter installed');
  replacement.notify(OP.EXECUTE[0]);
  await newOperation;
  assert.equal(timers.size, 0);
});

// Exercise actual sendControl/sendOperation/handleNotification, rather than
// mocking sendControl as the resume state-machine tests do.
function transferRig({ prn = 0, wrongOffset = false, wrongCrc = false, stalePrn = false } = {}) {
  const rig = harness(prn);
  const { client, crc32, OP } = rig;
  client.delayPromise = async () => {};
  const control = characteristic();
  bind(client, control);
  const firmware = Uint8Array.from({ length: 60 }, (_, i) => i * 3 + 1);
  const received = new Uint8Array(firmware.length);
  const state = { offset: 0, committed: 0, writes: [], verification: false };
  client.packetChar = {
    properties: { write: true, writeWithoutResponse: false },
    async writeValue(packet) {
      const bytes = new Uint8Array(packet);
      received.set(bytes, state.offset);
      state.offset += bytes.length;
      state.verification = true;
      if (prn > 0 && state.offset % (20 * prn) === 0) {
        control.notify(OP.CALCULATE_CHECKSUM[0], [state.offset, crc32(received.slice(0, state.offset))]);
      }
    },
  };
  control.writeValue = async bytes => {
    state.writes.push(bytes[0]);
    if (bytes[0] === OP.SELECT_DATA[0]) {
      if (state.verification && stalePrn) {
        control.notify(OP.CALCULATE_CHECKSUM[0], [20, crc32(received.slice(0, 20))]);
      }
      const offset = state.verification && wrongOffset ? state.offset - 1 : state.offset;
      const crc = crc32(received.slice(0, offset)) ^ (state.verification && wrongCrc ? 1 : 0);
      control.notify(bytes[0], [4096, offset, crc]);
    } else if (bytes[0] === OP.CREATE_DATA[0]) {
      state.offset = state.committed;
      control.notify(bytes[0]);
    } else if (bytes[0] === OP.EXECUTE[0]) {
      state.committed = state.offset;
      control.notify(bytes[0]);
    } else {
      assert.fail(`unexpected control opcode ${bytes[0]}`);
    }
  };
  return { ...rig, state, received, firmware,
    transfer: () => client.transfer(firmware.buffer, 'firmware', OP.SELECT_DATA, OP.CREATE_DATA) };
}

for (const prn of [0, 2]) {
  test(`real notification transport completes with stale PRNs interleaved (PRN=${prn})`, async () => {
    const { transfer, firmware, received, state, OP, timers } = transferRig({ prn, stalePrn: true });
    await transfer();
    assert.equal(state.committed, firmware.length);
    assert.deepEqual(received, firmware);
    assert.equal(state.writes.includes(OP.CALCULATE_CHECKSUM[0]), false);
    assert.equal(timers.size, 0);
  });
}

for (const fault of ['wrongOffset', 'wrongCrc']) {
  for (const prn of [0, 2]) {
    test(`real notification transport rejects ${fault} immediately despite stale PRNs (PRN=${prn})`, async () => {
      const { transfer, state, OP, timers } = transferRig({ [fault]: true, prn, stalePrn: true });
      await assert.rejects(transfer(), /object CRC校验失败/);
      assert.equal(state.committed, 0);
      assert.equal(state.writes.includes(OP.EXECUTE[0]), false);
      assert.equal(timers.size, 0, 'invalid progress must not become a response timeout');
    });
  }
}

test('real notification transport executes init and every firmware page with PRN enabled', async () => {
  const { client, crc32, OP, timers } = harness(2);
  client.delayPromise = async () => {};
  const control = characteristic();
  bind(client, control);
  const init = Uint8Array.from({ length: 64 }, (_, i) => i * 3 + 7);
  const firmware = Uint8Array.from({ length: 4096 * 2 + 38 }, (_, i) => (i * 13) ^ (i >>> 8));
  const objects = [null, init, firmware].map(bytes => bytes && ({
    expected: bytes, received: new Uint8Array(bytes.length), offset: 0, committed: 0,
    end: 0, executions: 0, max: bytes === init ? 256 : 4096,
  }));
  let active = 1;
  let packetCount = 0;
  let notifications = 0;
  control.writeValue = async bytes => {
    const opcode = bytes[0];
    if (opcode === OP.SELECT_DATA[0]) {
      active = bytes[1];
      const obj = objects[active];
      control.notify(opcode, [obj.max, obj.offset, crc32(obj.received.slice(0, obj.offset))]);
    } else if (opcode === OP.CREATE_DATA[0]) {
      active = bytes[1];
      const obj = objects[active];
      if (active === 2) assert.equal(objects[1].executions, 1, 'init must execute before data');
      assert.equal(obj.offset, obj.committed, 'previous page must execute before CREATE');
      obj.end = obj.committed + new DataView(bytes.buffer, bytes.byteOffset + 2, 4).getUint32(0, true);
      assert.ok(obj.end <= obj.expected.length);
      packetCount = 0;
      control.notify(opcode);
    } else if (opcode === OP.EXECUTE[0]) {
      const obj = objects[active];
      assert.equal(obj.offset, obj.end);
      assert.deepEqual(obj.received.slice(0, obj.offset), obj.expected.slice(0, obj.offset));
      obj.committed = obj.offset;
      obj.executions++;
      control.notify(opcode);
    } else {
      assert.fail(`unexpected opcode ${opcode}`);
    }
  };
  client.packetChar = {
    properties: { write: true, writeWithoutResponse: false },
    async writeValue(packet) {
      const obj = objects[active];
      const bytes = new Uint8Array(packet);
      assert.ok(obj.offset + bytes.length <= obj.end);
      obj.received.set(bytes, obj.offset);
      obj.offset += bytes.length;
      if (++packetCount % 2 === 0) {
        notifications++;
        control.notify(OP.CALCULATE_CHECKSUM[0], [obj.offset, crc32(obj.received.slice(0, obj.offset))]);
      }
    },
  };
  await client.transfer(init.buffer, 'init', OP.SELECT_COMMAND, OP.CREATE_COMMAND);
  await client.transfer(firmware.buffer, 'firmware', OP.SELECT_DATA, OP.CREATE_DATA);
  assert.equal(objects[1].executions, 1);
  assert.equal(objects[2].executions, 3);
  assert.equal(objects[2].committed, firmware.length);
  assert.ok(notifications > 200);
  assert.equal(timers.size, 0);
});
