const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { ResourceClient, crc32, parsePackage, parseHeader, parseTable, parseStatus, validateTarget, prepareBundle, installationPlan } = require('../html/js/resource_store.js');
const layout = require('../docs/external-flash-layout-v1.draft.json');
function wrap(id, payload) {
  const b = new Uint8Array(32 + payload.length), v = new DataView(b.buffer);
  b.set(payload, 32);
  [0x31535245, 0x10000 | id, 1, payload.length, crc32(payload), 524288].forEach((n, i) => v.setUint32(i * 4, n, true));
  v.setUint32(24, crc32(b.subarray(0, 24)), true); v.setUint32(28, 0xa55ac33c, true);
  return b;
}
function tableBytes({ todo = true } = {}) {
  const entries = layout.partitions.filter(partition => todo || partition.id !== 9).map(partition =>
    !todo && partition.id === 8 ? { ...partition, offset: '0x58000', size_bytes: 163840 } : partition);
  const b = new Uint8Array(entries.length * 16), v = new DataView(b.buffer);
  entries.forEach((p, i) => {
    v.setUint16(i * 16, p.id, true); v.setUint16(i * 16 + 2, p.id, true);
    v.setUint32(i * 16 + 4, Number(p.offset), true); v.setUint32(i * 16 + 8, p.size_bytes, true);
    v.setUint16(i * 16 + 12, 1, true);
  });
  return wrap(1, b);
}
function statusBytes(token, code = 0) {
  const b = new Uint8Array(16); b.set([0xa7, 0, token, code, 1, 1]);
  new DataView(b.buffer).setUint32(12, 524288, true); return b;
}
function deviceClient({ table = tableBytes(), status = null, dataLength = 20, bufferedReply = null } = {}) {
  const memory = new Uint8Array(524288).fill(255), packets = [];
  memory.set(table); memory.set(packageBytes(), 4096);
  let payloadLength = 0;
  const client = new ResourceClient(async p => {
    packets.push(p);
    let a = Uint8Array.of(0xa7, p[1], p[2], 0);
    if (p[1] === 0) { a = status || statusBytes(p[2]); a[2] = p[2]; }
    if (p[1] === 1) {
      const at = new DataView(p.buffer).getUint32(3, true);
      a = Uint8Array.of(...a, ...memory.subarray(at, at + p[7]));
    }
    if (p[1] === 2) payloadLength = new DataView(p.buffer).getUint32(4, true);
    if (p[1] === 10) {
      const accepted = new DataView(p.buffer).getUint32(3, true) + p.length - 7;
      const durable = accepted === payloadLength ? accepted : Math.max(0, Math.floor((accepted + 32) / 256) * 256 - 32);
      a = new Uint8Array(12); a.set([0xa7, 10, p[2], 0]);
      const v = new DataView(a.buffer); v.setUint32(4, accepted, true); v.setUint32(8, durable, true);
      if (bufferedReply) a = bufferedReply(a, p);
    }
    client.receive(a);
  }, 5000, () => dataLength);
  return { client, packets, memory };
}
function packageBytes() {
  const b = new Uint8Array(61), view = new DataView(b.buffer);
  b.fill(0x39, 32);
  [0x31535245, 0x10002, 1, 29, crc32(b.subarray(32)), 524288].forEach((v, i) => view.setUint32(i * 4, v, true));
  view.setUint32(24, crc32(b.subarray(0, 24)), true);
  view.setUint32(28, 0xa55ac33c, true);
  return b;
}
function completeBundle({ todo = false, legacy = false } = {}) {
  const entries = [
    { name: 'calendar-annotations.ers', bytes: wrap(3, Uint8Array.of(3)) },
    { name: 'fonts.ers', bytes: wrap(7, Uint8Array.of(7)) },
    { name: 'partition-table.ers', bytes: tableBytes({ todo: !legacy }) },
    { name: 'ui.ers', bytes: wrap(6, Uint8Array.of(6)) },
    { name: 'epd-config.ers', bytes: packageBytes() },
    { name: 'calendar.ers', bytes: wrap(5, Uint8Array.of(5)) },
  ];
  if (todo) entries.push({ name: 'todo.ers', bytes: wrap(9,
    require('../html/js/todo_calendar.js').pack({ items: [], limit: 5, fontSize: 12 })) });
  return entries;
}
test('CRC and package rejects corruption/truncation', () => {
  assert.equal(crc32(Buffer.from('123456789')), 0xcbf43926);
  assert.equal(parsePackage(packageBytes()).id, 2);
  const b = packageBytes(); b[50] ^= 1;
  assert.throws(() => parsePackage(b));
  assert.throws(() => parsePackage(new Uint8Array(31)));
});
test('one-click bundle is complete, validated and sorted in safe install order', () => {
  const result = prepareBundle(completeBundle());
  assert.deepEqual(result.packages.map(item => item.pack.id), [1, 5, 6, 7, 2, 3]);
  assert.equal(result.partitions.length, 9);
  assert.equal(result.total, parsePackage(tableBytes()).payload.length + 33);

  assert.throws(() => prepareBundle(completeBundle().slice(1)), /缺少/);
  assert.throws(() => prepareBundle([...completeBundle(), { name: 'duplicate.ers', bytes: wrap(2, Uint8Array.of(2)) }]), /重复/);
  assert.throws(() => prepareBundle([...completeBundle(), { name: 'unsupported.ers', bytes: wrap(10, Uint8Array.of(10)) }]), /不支持/);
});

test('both legacy six-package projects and optional todo backups prepare successfully', () => {
  const old = prepareBundle(completeBundle({ legacy: true }));
  assert.equal(old.packages.length, 6); assert.equal(old.partitions.length, 8);
  assert.equal(old.partitions.some(partition => partition.id === 9), false);
  const modern = prepareBundle(completeBundle({ todo: true }));
  assert.deepEqual(modern.packages.map(item => item.pack.id), [1, 5, 6, 7, 2, 3, 9]);
  assert.equal(modern.partitions.find(partition => partition.id === 9).size, 4096);
  assert.throws(() => prepareBundle(completeBundle({ legacy: true, todo: true })), /不存在目标分区/);
});
test('install plan initializes blank Flash and preserves the table and personal data on existing devices', () => {
  const bundle = prepareBundle(completeBundle());
  const partitions = parseTable(parsePackage(tableBytes()));
  assert.deepEqual(installationPlan(bundle, 4, null).map(item => item.pack.id), [1, 5, 6, 7, 2, 3]);
  assert.deepEqual(installationPlan(bundle, 0, partitions).map(item => item.pack.id), [5, 6, 7]);
  assert.throws(() => installationPlan(bundle, 3, partitions), /不能按空白/);
  const withTodos = prepareBundle(completeBundle({ todo: true }));
  assert.deepEqual(installationPlan(withTodos, 4, null).map(item => item.pack.id), [1, 5, 6, 7, 2, 3, 9]);
  assert.deepEqual(installationPlan(withTodos, 0, partitions).map(item => item.pack.id), [5, 6, 7]);
  const legacy = parseTable(parsePackage(tableBytes({ todo: false })));
  assert.deepEqual(installationPlan(withTodos, 0, legacy).map(item => item.pack.id), [5, 6, 7]);
});
test('upload uses stop-and-wait packets bounded to default ATT payload', async () => {
  const { client, packets } = deviceClient();
  await client.upload(packageBytes());
  assert.deepEqual(packets.filter(p => p[1] !== 1).map(p => p[1]), [0, 6, 5, 2, 3, 3, 3, 4]);
  assert.ok(packets.every(p => p.length <= 20));
  assert.deepEqual(packets.filter(p => p[1] === 3).map(p => new DataView(p.buffer).getUint32(3, true)), [0, 13, 26]);
});
test('upload reuses a previously validated partition table', async () => {
  const { client, packets } = deviceClient();
  const partitions = parseTable(parsePackage(tableBytes()));
  await client.upload(packageBytes(), () => {}, partitions);
  assert.equal(packets.filter(p => p[1] === 1).length, 0);
  assert.ok(packets.some(p => p[1] === 4));
});
test('selecting output with several resource folders explains which folder to open', async () => {
  const nodes = new Map(), listeners = new Map();
  function node(id) {
    if (!nodes.has(id)) nodes.set(id, {
      classList: { add() {}, remove() {} },
      addEventListener(event, listener) { listeners.set(`${id}:${event}`, listener); }
    });
    return nodes.get(id);
  }
  const context = { document: { getElementById: node }, EPDResourceStore: require('../html/js/resource_store.js') };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_installer.js'), 'utf8'), context);
  const older = 'external-resources-r1-20260924-182356/partition-table.ers';
  const newer = 'external-resources-r1-20260924-182648/fonts.ers';
  await listeners.get('externalResourceFolder:change')({ target: { files: [
    { name: 'partition-table.ers', webkitRelativePath: older },
    { name: 'fonts.ers', webkitRelativePath: newer }
  ] } });
  const message = node('externalResourceBundle').textContent;
  assert.match(message, /2 套资源包/);
  assert.match(message, /external-resources-r1-20260924-182356/);
  assert.match(message, /时间最新的是 external-resources-r1-20260924-182648/);
  assert.equal(node('externalResourceUsage').textContent, '尚未加载分区表');
});
test('selecting one resource folder shows its package name', async () => {
  const nodes = new Map(), listeners = new Map();
  function node(id) {
    if (!nodes.has(id)) nodes.set(id, {
      classList: { add() {}, remove() {} },
      addEventListener(event, listener) { listeners.set(`${id}:${event}`, listener); }
    });
    return nodes.get(id);
  }
  const context = {
    document: { getElementById: node },
    EPDResourceStore: { ...require('../html/js/resource_store.js'), prepareBundle: () => prepareBundle(completeBundle()) }
  };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_installer.js'), 'utf8'), context);
  await listeners.get('externalResourceFolder:change')({ target: { files: [
    { name: 'partition-table.ers', webkitRelativePath: 'external-resources-r1-20260924-183132/partition-table.ers', arrayBuffer: async () => new ArrayBuffer(1) }
  ] } });
  assert.match(node('externalResourceBundle').textContent, /已加载 external-resources-r1-20260924-183132/);
});
test('installer keeps the completed message when the device restarts', async () => {
  const bundle = prepareBundle(completeBundle());
  const nodes = new Map(), listeners = new Map(), uploads = [];
  function node(id) {
    if (!nodes.has(id)) nodes.set(id, {
      classList: { add() {}, remove() {} },
      addEventListener(event, listener) { listeners.set(`${id}:${event}`, listener); }
    });
    return nodes.get(id);
  }
  class FakeClient {
    status() { return statusBytes(0, 4); }
    async upload(bytes, progress, partitions) {
      const pack = parsePackage(bytes);
      uploads.push({ id: pack.id, partitions });
      progress(pack.payload.length);
    }
    async leave() {}
    disconnect() {}
  }
  const context = {
    document: { getElementById: node },
    EPDResourceStore: { ...require('../html/js/resource_store.js'), ResourceClient: FakeClient, prepareBundle: () => bundle }
  };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_installer.js'), 'utf8'), context);
  await listeners.get('externalResourceFolder:change')({ target: { files: [{ name: 'bundle.ers', arrayBuffer: async () => new ArrayBuffer(1) }] } });
  context.ExternalResourceInstaller.bind({ writeValueWithResponse: async () => {} });
  await context.ExternalResourceInstaller.install();
  assert.deepEqual(uploads.map(item => item.id), [1, 5, 6, 7, 2, 3]);
  assert.equal(uploads[0].partitions, null);
  assert.equal(uploads[1].partitions, bundle.partitions);
  context.ExternalResourceInstaller.disconnect();
  assert.match(node('externalResourceStatus').textContent, /资源已写入/);
});
test('lost ACK retries identical packet and token', async () => {
  const packets = [];
  const client = new ResourceClient(async p => {
    packets.push(p);
    if (packets.length === 2) client.receive(Uint8Array.of(0xa7, p[1], p[2], 0));
  }, 5);
  await client.request(4);
  assert.deepEqual(packets[0], packets[1]);
});

test('large resource packets require capability and negotiated data length, buffered progress is durable', async () => {
  for (const [caps, mtu, chunk] of [[1, 244, 13], [0x39, 20, 13], [0x39, 100, 93], [0x39, 244, 237]]) {
    const status = statusBytes(0); status[4] = caps;
    const { client, packets, memory } = deviceClient({ status, dataLength: mtu });
    const payload = Uint8Array.from({ length: 500 }, (_, i) => i & 255);
    const progress = [];
    await client.upload(wrap(2, payload), (...p) => progress.push(p), parseTable(parsePackage(tableBytes())));
    const writes = packets.filter(p => p[1] === (caps & 32 ? 10 : 3));
    assert.equal(writes.length, Math.ceil(payload.length / chunk));
    assert.ok(writes.every(p => p.length <= chunk + 7));
    assert.deepEqual(progress.at(-1), [500, 500, 500]);
    if (caps & 32) assert.ok(progress.some(p => p[2] < p[0]));
    assert.deepEqual(await client.readRange(32, 500), memory.slice(32, 532));
    const readLimit = caps & 8 ? mtu - 4 : 16;
    assert.equal(packets.filter(p => p[1] === 1).length, Math.ceil(500 / readLimit));
  }
});

test('buffered ACK must confirm accepted bytes and a fully durable tail before COMMIT', async () => {
  for (const corrupt of [
    a => a.slice(0, 4),
    a => { new DataView(a.buffer).setUint32(4, 1, true); return a; },
    a => { new DataView(a.buffer).setUint32(8, 501, true); return a; },
    a => { new DataView(a.buffer).setUint32(8, 0, true); return a; },
  ]) {
    const status = statusBytes(0); status[4] = 0x39;
    const { client, packets } = deviceClient({ status, dataLength: 244, bufferedReply: corrupt });
    await assert.rejects(client.upload(wrap(2, new Uint8Array(500)), () => {}, parseTable(parsePackage(tableBytes()))));
    assert.ok(!packets.some(p => p[1] === 4));
  }
});

test('verified metadata distinguishes corrupt content from transport and storage failures', async () => {
  let code = 0;
  const client = new ResourceClient(async p => {
    const a = new Uint8Array(code ? 4 : 16); a.set([0xa7, p[1], p[2], code]);
    if (!code) { const v = new DataView(a.buffer); v.setUint32(4, 7, true); v.setUint32(8, 500, true); v.setUint32(12, 1234, true); }
    client.receive(a);
  });
  client.capabilities = { verify: true };
  assert.deepEqual(await client.inspect(7), { code: 0, revision: 7, length: 500, crc: 1234 });
  code = 3; assert.deepEqual(await client.inspect(7), { code: 3 });
  code = 5; await assert.rejects(client.inspect(7), /校验失败/);
  code = 2; await assert.rejects(client.inspect(7), /校验失败/);
});

test('installer skips CRC-verified identical resources, repairs corruption, and supports forced rewrite', async () => {
  const uploads = [], inspected = [];
  let corrupted = false;
  class Client {
    status() { const b = statusBytes(0); b[4] = 0x3f; return b; }
    partitions() { return parseTable(parsePackage(tableBytes())); }
    inspect(id) {
      inspected.push(id);
      const pack = completeBundle().map(p => parsePackage(p.bytes)).find(p => p.id === id);
      if (!pack || (corrupted && id === 7)) return { code: 3 };
      return { code: 0, revision: 1, length: pack.payload.length, crc: pack.crc };
    }
    async upload(bytes) { uploads.push(parsePackage(bytes).id); }
    async leave() {}
    disconnect() {}
  }
  const { installer, node } = installerFixture(Client);
  await installer.loadBundle(completeBundle(), 'test'); installer.bind({});
  await installer.install(); assert.deepEqual(uploads, []); assert.deepEqual(inspected.slice(0, 3), [5, 6, 7]);
  corrupted = true;
  await installer.install(); assert.deepEqual(uploads, [7]);
  uploads.length = 0; node('externalResourceForce').checked = true;
  await installer.install(); assert.deepEqual(uploads, [5, 6, 7]);
});

test('capability flags accept hot reload and offline mode while retaining legacy support', async () => {
  const status = statusBytes(0);
  assert.equal(parseStatus(status).hotReload, false);
  status[4] = 7;
  assert.equal(parseStatus(status).hotReload, true);
  assert.equal(parseStatus(status).offline, true);
  const { client, packets } = deviceClient({ status });
  await client.runOffline();
  assert.deepEqual(Array.from(packets[0]).slice(0, 2), [0x26, 8]);
});

test('new installer applies without reset, syncs time before offline and keeps the disconnect result', async () => {
  const nodes = new Map(), calls = [];
  const node = id => {
    if (!nodes.has(id)) nodes.set(id, { checked: true, classList: { add() {}, remove() {} }, addEventListener() {} });
    return nodes.get(id);
  };
  let activationError = false;
  class FakeClient {
    status() { const b = statusBytes(0, 4); b[4] = 7; return b; }
    async upload() { calls.push('upload'); }
    async leave() { calls.push('activate'); if (activationError) throw new Error('semantic failure'); }
    async runOffline() { calls.push('offline'); }
    disconnect() {}
  }
  const context = { document: { getElementById: node }, Uint8Array,
    buildTimeData: () => [1,2,3,4,8],
    EPDResourceStore: { ...require('../html/js/resource_store.js'), ResourceClient: FakeClient } };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_installer.js'), 'utf8'), context);
  const installer = context.ExternalResourceInstaller;
  await installer.loadBundle(completeBundle(), 'test');
  installer.bind({ writeValueWithResponse: async b => { calls.push(b[0]); } });
  await installer.install();
  assert.deepEqual(calls.slice(-3), ['activate', 0x23, 'offline']);
  assert.ok(!calls.includes(0x91));
  installer.disconnect();
  assert.match(node('externalResourceStatus').textContent, /断开连接.*间歇广播/);
  activationError = true; calls.length = 0;
  installer.bind({ writeValueWithResponse: async b => { calls.push(b[0]); } });
  await installer.install();
  assert.ok(!calls.includes('offline') && !calls.includes(0x91));
  assert.match(node('externalResourceStatus').textContent, /semantic failure/);
});
test('actual device partition table is read and validated; payload export verifies full CRC', async () => {
  const { client, memory } = deviceClient();
  const entries = await client.partitions();
  assert.equal(entries.length, 9);
  assert.equal(entries.find(p => p.id === 7).size, 262144);
  const p = entries.find(p => p.id === 2);
  assert.deepEqual(await client.readPackage(p), packageBytes());
  memory[4096 + 50] ^= 1;
  assert.equal(parseHeader(memory.subarray(4096, 4128), p).revision, 1);
  await assert.rejects(client.readPackage(p), /CRC/);
});

test('bad, overlapping, duplicate and incompatible partition tables fail before any destructive command', async () => {
  for (const change of [
    v => v.setUint32(20, 0, true), // overlap
    v => v.setUint16(16, 1, true), // duplicate ID
    v => v.setUint32(24, 524288, true), // overflow
    v => v.setUint16(28, 2, true), // schema
    v => v.setUint32(4, 4096, true), // bad locator
  ]) {
    const payload = parsePackage(tableBytes()).payload.slice(); change(new DataView(payload.buffer));
    const b = wrap(1, payload);
    assert.throws(() => parseTable(parsePackage(b)));
    const { client, packets } = deviceClient();
    await assert.rejects(client.upload(b));
    assert.ok(packets.every(p => p[1] < 2));
  }
});

test('missing partition, reserved regions and oversized packages are refused before ENTER', async () => {
  for (const b of [wrap(4, Uint8Array.of(1)), wrap(8, Uint8Array.of(1)), wrap(10, Uint8Array.of(1)), wrap(2, new Uint8Array(4065)), wrap(9, new Uint8Array(4065))]) {
    const { client, packets } = deviceClient();
    await assert.rejects(client.upload(b));
    assert.ok(packets.every(p => p[1] < 2));
  }
  assert.throws(() => validateTarget(parsePackage(packageBytes()), []), /先安装/);
  const old = deviceClient({ table: tableBytes({ todo: false }) });
  await assert.rejects(old.client.upload(wrap(9, Uint8Array.of(9))), /不存在目标分区/);
  assert.ok(old.packets.every(packet => packet[1] < 2));
});

test('the new todo resource is writable only through its declared partition', async () => {
  const { client, packets } = deviceClient();
  await client.upload(completeBundle({ todo: true }).find(item => item.name === 'todo.ers').bytes);
  assert.ok(packets.some(packet => packet[1] === 2 && packet[3] === 9));
  assert.ok(packets.some(packet => packet[1] === 4));
});

test('firmware can repair absent table, but cannot upload other resources without it', async () => {
  const { client, packets } = deviceClient({ table: new Uint8Array(160).fill(255), status: statusBytes(0, 4) });
  await assert.rejects(client.upload(packageBytes()), /未安装/);
  assert.ok(packets.every(p => p[1] < 2));
  await client.upload(tableBytes());
  assert.ok(packets.some(p => p[1] === 2 && p[3] === 1));
});

test('short or incompatible capability and an active transaction cannot start upload', async () => {
  for (const [i, status] of [Uint8Array.of(0xa7, 0, 0, 5), statusBytes(0), statusBytes(0), statusBytes(0), statusBytes(0)].entries()) {
    if (status.length === 16) {
      if (i === 1) status[5] = 2;
      else if (i === 2) status[12] = 1;
      else if (i === 3) status[4] = 0;
      else status[6] = 2;
    }
    const { client, packets } = deviceClient({ status });
    await assert.rejects(client.upload(tableBytes()));
    assert.equal(packets.length, 1);
  }
  assert.throws(() => parseStatus(new Uint8Array(4)));
  assert.throws(() => parseHeader(new Uint8Array(3)), /长度/);
});

test('range reads handle a partial final packet and report completion', async () => {
  const { client, packets, memory } = deviceClient();
  const progress = [];
  assert.deepEqual(await client.readRange(32, 29, (n, total) => progress.push([n, total])), memory.slice(32, 61));
  assert.deepEqual(packets.map(p => p[7]), [16, 13]);
  assert.deepEqual(progress, [[29, 29]]);
  await assert.rejects(client.readRange(524287, 2));
  assert.equal(packets.length, 2);
});
test('disconnect rejects pending request and disables reuse', async () => {
  const client = new ResourceClient(async () => { client.disconnect(); });
  await assert.rejects(client.status(), /断开/);
  await assert.rejects(client.status(), /连接不可用/);
});
test('wrong token is ignored; out of range read never reaches transport', async () => {
  let writes = 0;
  const client = new ResourceClient(async p => {
    writes++;
    client.receive(Uint8Array.of(0xa7, p[1], p[2] + 1, 0));
    client.receive(Uint8Array.of(0xa7, p[1], p[2], 0, 0x73));
  });
  assert.deepEqual(await client.read(0, 1), Uint8Array.of(0x73));
  await assert.rejects(client.read(524288, 1));
  assert.equal(writes, 1);
});

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
function installerFixture(Client = ResourceClient, fetch) {
  const nodes = new Map(), listeners = new Map();
  const node = id => {
    if (!nodes.has(id)) nodes.set(id, {
      classList: { add() {}, remove() {} },
      addEventListener(event, listener) { listeners.set(`${id}:${event}`, listener); }
    });
    return nodes.get(id);
  };
  const context = { document: { getElementById: node }, fetch, Uint8Array,
    EPDResourceStore: { ...require('../html/js/resource_store.js'), ResourceClient: Client } };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_installer.js'), 'utf8'), context);
  return { installer: context.ExternalResourceInstaller, node, context,
    select: files => listeners.get('externalResourceFolder:change')({ target: { files } }) };
}

test('font synchronization keeps the parent operation owner and propagates failures', async () => {
  let session;
  class Client { constructor() { session = this; } disconnect() { this.closed = true; } }
  const { installer, context } = installerFixture(Client);
  installer.bind({});
  const owner = {};
  context.DeviceOperations = { current: owner, assert: value => assert.equal(value, owner), end() { throw new Error('must retain parent'); } };
  context.EPDFontSync = { ensure: async (client, requests, options) => {
    assert.equal(client, session); assert.equal(client.owner, owner);
    assert.equal(installer.busy(), true); options.assertCurrent();
    throw new Error('font load failed');
  } };
  await assert.rejects(installer.ensureFonts([{ slot: 0, text: '生' }], owner), /font load failed/);
  assert.equal(installer.busy(), false);
  assert.equal(session.owner, null);
  assert.equal(context.DeviceOperations.current, owner);
  installer.disconnect();
  await assert.rejects(installer.ensureFonts([{ slot: 0, text: '生' }], owner), /连接不可用/);
});

test('late font synchronization cannot unlock a newer connection operation', async () => {
  class Client { disconnect() {} }
  const { installer, context } = installerFixture(Client);
  const started = deferred(), release = deferred(), owner = {};
  installer.bind({});
  context.DeviceOperations = { current: owner, assert: value => {
    if (value !== context.DeviceOperations.current) throw new Error('operation changed');
  } };
  context.EPDFontSync = { ensure: async (client, requests, options) => {
    started.resolve(); await release.promise; options.assertCurrent(); return { changed: false, added: [] };
  } };
  const old = installer.ensureFonts([{ slot: 0, text: '生' }], owner);
  await started.promise;
  installer.bind({});
  const currentOwner = context.DeviceOperations.current = {};
  release.resolve();
  await assert.rejects(old, /连接已变化/);
  assert.equal(context.DeviceOperations.current, currentOwner);
});

test('regular resource installation merges actual device-only glyphs into the incoming font pack', async () => {
  const builder = require('../html/js/resource_builder.js'), glyphs = require('../html/js/font_builder.js');
  const sync = require('../html/js/font_sync.js');
  const template = require('../html/resources/default-project.json');
  const entries = template.files.map(f => ({ name: f.name, bytes: new Uint8Array(Buffer.from(f.base64, 'base64')) }));
  const base = prepareBundle(entries), fonts = builder.open(base).fonts;
  const generated = await glyphs.extendFonts(fonts, [{ slot: 0, text: '生' }], {
    rasterize: async () => ({ width: 2, height: 2, x: 0, y: 0, advance: 3, pixels: Uint8Array.of(1,0,0,1) })
  });
  const installed = new Map(base.packages.map(p => [p.pack.id, p.bytes]));
  installed.set(7, builder.wrap(7, builder.encodeFonts(generated.fonts), 2));
  const incomingFonts = fonts.map(f => f.slice()); incomingFonts[1][0] = 100;
  const incoming = entries.map(entry => parsePackage(entry.bytes).id === 7
    ? { ...entry, bytes: builder.wrap(7, builder.encodeFonts(incomingFonts), 3) } : entry);
  const uploads = [];
  class Client {
    status() { const b = statusBytes(0); b[4] = 0x13; return b; }
    partitions() { return base.partitions; }
    inspect(id) { const pack = parsePackage(installed.get(id)); return { code: 0, revision: pack.revision, crc: pack.crc, length: pack.payload.length }; }
    readPackage(partition) { return installed.get(partition.id); }
    upload(bytes) { uploads.push(bytes); installed.set(parsePackage(bytes).id, bytes); }
    leave() {}
    disconnect() {}
  }
  const { installer, context } = installerFixture(Client);
  context.EPDFontSync = sync;
  await installer.loadBundle(incoming);
  installer.bind({}); await installer.install();
  assert.equal(uploads.length, 1);
  const written = builder.decodeFonts(parsePackage(uploads[0]).payload);
  assert.equal(builder.validateFont(written[0], 0).has('生'.codePointAt(0)), true);
  assert.equal(written[1][0], 100);
});

test('offline application ACK remains authoritative if the final GATT write closes', async () => {
  const client = new ResourceClient(async packet => {
    client.receive(Uint8Array.of(0xa7, 8, packet[2], 0));
    client.disconnect();
    throw new Error('GATT closed');
  });
  assert.equal((await client.runOffline())[3], 0);
});

test('disconnect without a successful offline ACK never reports success', async () => {
  for (const status of [null, 2]) {
    const client = new ResourceClient(async packet => {
      if (status !== null) client.receive(Uint8Array.of(0xa7, 8, packet[2], status));
      client.disconnect();
      throw new Error('GATT closed');
    });
    await assert.rejects(client.runOffline());
  }
});

test('late upload progress and failure cannot overwrite or unlock a new connection', async () => {
  const uploading = deferred(), oldUpload = deferred(), newStatus = deferred();
  let clients = 0, oldProgress;
  class Client {
    constructor() { this.id = ++clients; }
    status() { return this.id === 1 ? statusBytes(0, 4) : newStatus.promise; }
    upload(bytes, progress) { oldProgress = progress; uploading.resolve(); return oldUpload.promise; }
    leave() { throw new Error('old session must not leave'); }
    disconnect() {}
  }
  const { installer, node } = installerFixture(Client);
  await installer.loadBundle(completeBundle(), 'manual');
  installer.bind({});
  const oldWork = installer.install();
  await uploading.promise;
  installer.bind({}); // Also exercise replacement without a preceding disconnect event.
  const freshWork = installer.refresh();
  node('externalResourceStatus').textContent = 'new connection';
  oldProgress(1);
  oldUpload.reject(new Error('old upload failed'));
  await oldWork;
  assert.equal(node('externalResourceStatus').textContent, 'new connection');
  assert.equal(installer.busy(), true);
  newStatus.resolve(statusBytes(0, 4));
  await freshWork;
  assert.equal(installer.busy(), false);
});

test('late offline success cannot become the completion message of a new connection', async () => {
  const started = deferred(), ack = deferred();
  class Client {
    status() { const b = statusBytes(0, 4); b[4] = 7; return b; }
    runOffline() { started.resolve(); return ack.promise; }
    disconnect() {}
  }
  const { installer, node } = installerFixture(Client);
  installer.bind({});
  const oldWork = installer.runOffline();
  await started.promise;
  installer.disconnect();
  installer.bind({});
  node('externalResourceStatus').textContent = 'new connection';
  ack.resolve();
  await oldWork;
  assert.equal(node('externalResourceStatus').textContent, 'new connection');
  installer.disconnect();
  assert.equal(node('externalResourceStatus').textContent, '设备未连接');
});

test('offline completion survives its own intentional disconnect', async () => {
  const started = deferred(), ack = deferred();
  class Client {
    status() { const b = statusBytes(0, 4); b[4] = 7; return b; }
    runOffline() { started.resolve(); return ack.promise; }
    disconnect() {}
  }
  const { installer, node } = installerFixture(Client);
  installer.bind({});
  const work = installer.runOffline();
  await started.promise;
  installer.disconnect();
  ack.resolve();
  await work;
  assert.match(node('externalResourceStatus').textContent, /断开连接.*间歇广播/);
});

test('a slow folder read cannot replace a later folder selection', async () => {
  const slow = deferred();
  const { installer, node, select } = installerFixture();
  const files = (label, wait) => completeBundle().map((p, i) => ({
    name: p.name, webkitRelativePath: `${label}/${p.name}`,
    arrayBuffer: async () => { if (wait && i === 0) await slow.promise; return p.bytes.buffer; }
  }));
  const oldWork = select(files('old', true));
  await select(files('new', false));
  const selected = installer.getBundle();
  slow.resolve();
  await oldWork;
  assert.equal(installer.getBundle(), selected);
  assert.match(node('externalResourceBundle').textContent, /已加载 new/);
});

test('background output loading cannot replace a manually prepared bundle', async () => {
  const fetching = deferred(), slow = deferred();
  const fetch = async url => {
    if (url.endsWith('.txt')) return { ok: true, text: async () => 'external-resources-r1-20260928-120000' };
    fetching.resolve();
    await slow.promise;
    const pack = completeBundle().find(p => url.endsWith(p.name));
    return { ok: true, arrayBuffer: async () => pack.bytes.buffer };
  };
  const { installer, node } = installerFixture(ResourceClient, fetch);
  const loading = installer.loadLatestOutput();
  await fetching.promise;
  await installer.loadBundle(completeBundle(), 'manual');
  const selected = installer.getBundle();
  slow.resolve();
  await loading;
  assert.equal(installer.getBundle(), selected);
  assert.match(node('externalResourceBundle').textContent, /已加载 manual/);
});

test('resource discovery waits for use and checks a missing output only once', async () => {
  const requests = [];
  const { installer, context } = installerFixture(ResourceClient, async url => {
    requests.push(url);
    return { ok: false };
  });
  context.location = { href: 'http://localhost/html/index.html' };
  context.URL = URL;
  assert.deepEqual(requests, []);
  await Promise.all([installer.loadLatestOutput(), installer.loadLatestOutput()]);
  await installer.loadLatestOutput();
  assert.deepEqual(requests, ['../output/external-resources-latest.txt']);
  assert.equal(installer.getBundle(), null);
});

test('local Flash figures are labelled estimates and exclude system and reserved space', async () => {
  const { installer, node } = installerFixture();
  await installer.loadBundle(completeBundle(), 'local');
  const usage = node('externalResourceUsage').textContent;
  assert.match(usage, /资源包估算/);
  assert.match(usage, /资源分区余量/);
  assert.match(usage, /系统区占用未统计/);
  assert.match(usage, /保留区占用未统计/);
  assert.doesNotMatch(usage, /设备读回|已校验|校验失败/);
});

test('device usage counts verified package headers and leaves corrupt or absent resources unknown', async () => {
  const inspected = [];
  class Client {
    status() { const bytes = statusBytes(0); bytes[4] |= 16; return bytes; }
    partitions() { return parseTable(parsePackage(tableBytes())); }
    inspect(id) {
      inspected.push(id);
      if (id === 6) return { code: 3 };
      if (id === 7) return { code: 4 };
      const pack = completeBundle().map(item => parsePackage(item.bytes)).find(item => item.id === id);
      if (!pack) return { code: 4 };
      return { code: 0, revision: pack.revision, length: pack.payload.length, crc: pack.crc };
    }
    disconnect() {}
  }
  const { installer, node } = installerFixture(Client);
  installer.bind({});
  await installer.refresh();
  assert.deepEqual(inspected, [1, 2, 3, 5, 6, 7, 9]);
  const usage = node('externalResourceUsage').textContent;
  assert.match(usage, /设备读回/);
  assert.match(usage, /日历资源：占用 33 B/);
  assert.match(usage, /UI 布局：64\.0 KiB · 占用与余量未确认/);
  assert.match(usage, /字体资源：256 KiB · 占用与余量未确认/);
  assert.match(node('deviceResourceVersions').textContent, /UI 布局：—.*校验失败/);
  assert.match(node('deviceResourceVersions').textContent, /字体资源：—.*未安装或未提交/);
  assert.match(node('deviceResourceVersions').textContent, /待办事项：—.*未安装或未提交/);
});

test('legacy package headers do not claim verified CRC or confirmed free space', async () => {
  const reads = [];
  class Client {
    status() { return statusBytes(0); }
    partitions() { return parseTable(parsePackage(tableBytes())); }
    readRange(offset) {
      const id = this.partitions().find(item => item.offset === offset).id;
      reads.push(id);
      const item = completeBundle().find(item => parsePackage(item.bytes).id === id);
      return item ? item.bytes.subarray(0, 32) : new Uint8Array(32).fill(255);
    }
    disconnect() {}
  }
  const { installer, node } = installerFixture(Client);
  await installer.loadBundle(completeBundle(), 'local');
  installer.bind({});
  await installer.refresh();
  assert.deepEqual(reads, [1, 2, 3, 5, 6, 7, 9]);
  assert.match(node('externalResourceUsage').textContent, /资源占用 未确认，资源分区余量 未确认/);
  assert.match(node('externalResourceUsage').textContent, /包头申报 33 B/);
  assert.match(node('deviceResourceVersions').textContent, /仅读包头/);
  assert.doesNotMatch(node('deviceResourceVersions').textContent, /已校验|内容一致/);
});

test('local loading preserves device figures and compares both CRC and length', async () => {
  class Client {
    status() { const bytes = statusBytes(0); bytes[4] |= 16; return bytes; }
    partitions() { return parseTable(parsePackage(tableBytes())); }
    inspect(id) {
      const pack = completeBundle().map(item => parsePackage(item.bytes)).find(item => item.id === id);
      if (!pack) return { code: 4 };
      return { code: 0, revision: 2, length: pack.payload.length + (id === 5 ? 1 : 0),
        crc: id === 6 ? pack.crc ^ 1 : pack.crc };
    }
    disconnect() {}
  }
  const { installer, node } = installerFixture(Client);
  installer.bind({});
  await installer.refresh();
  const deviceUsage = node('externalResourceUsage').textContent;
  await installer.loadBundle(completeBundle(), 'local');
  assert.equal(node('externalResourceUsage').textContent, deviceUsage);
  const versions = node('deviceResourceVersions').textContent;
  assert.match(versions, /日历资源：v2.*本地 v1 · 待更新/);
  assert.match(versions, /UI 布局：v2.*本地 v1 · 待更新/);
  assert.match(versions, /字体资源：v2.*本地 v1 · 内容一致/);
  await installer.loadBundle([], 'bad');
  assert.equal(node('externalResourceUsage').textContent, deviceUsage);
  assert.match(node('deviceResourceVersions').textContent, /设备已读回/);
  installer.disconnect();
  assert.equal(node('externalResourceUsage').textContent, '尚未加载分区表');
  assert.match(node('deviceResourceVersions').textContent, /设备未连接/);
  assert.doesNotMatch(node('deviceResourceVersions').textContent, /v2|已校验/);
  installer.bind({});
  assert.match(node('deviceResourceVersions').textContent, /尚未读取/);
});

test('upload marks a previously read device snapshot as requiring refresh', async () => {
  class Client {
    status() { const bytes = statusBytes(0); bytes[4] = 0x13; return bytes; }
    partitions() { return parseTable(parsePackage(tableBytes())); }
    inspect(id) {
      const pack = completeBundle().map(item => parsePackage(item.bytes)).find(item => item.id === id);
      if (!pack) return { code: 4 };
      return { code: 0, revision: 1, length: pack.payload.length, crc: pack.crc };
    }
    async upload() {}
    async leave() {}
    disconnect() {}
  }
  const { installer, node } = installerFixture(Client);
  await installer.loadBundle(completeBundle(), 'local');
  installer.bind({});
  await installer.refresh();
  node('externalResourceForce').checked = true;
  await installer.install();
  assert.match(node('externalResourceUsage').textContent, /上次读取 · 待刷新/);
  assert.match(node('deviceResourceVersions').textContent, /上次读取 · 待刷新/);
  assert.doesNotMatch(node('deviceResourceVersions').textContent, /内容一致/);
});

test('forced resource installation preserves existing todo bytes from a seven-package project', async () => {
  const files = completeBundle({ todo: true }), stored = new Map(files.map(item => [parsePackage(item.bytes).id, item.bytes]));
  const todo = stored.get(9).slice(), uploads = [];
  class Client {
    status() { const bytes = statusBytes(0); bytes[4] = 0x13; return bytes; }
    partitions() { return parseTable(parsePackage(stored.get(1))); }
    inspect(id) {
      const pack = parsePackage(stored.get(id));
      return { code: 0, revision: pack.revision, length: pack.payload.length, crc: pack.crc };
    }
    async upload(bytes) { const id = parsePackage(bytes).id; uploads.push(id); stored.set(id, bytes); }
    async leave() {}
    disconnect() {}
  }
  const { installer, node } = installerFixture(Client);
  await installer.loadBundle(files, 'with-todos'); installer.bind({});
  node('externalResourceForce').checked = true;
  await installer.install();
  assert.deepEqual(uploads, [5, 6, 7]);
  assert.deepEqual(stored.get(9), todo);
});

function readbackDevice({ beforeReply = async () => {}, code = 0, active = 0, todo = 'missing', legacy = false, verify = !legacy } = {}) {
  const fixture = installerFixture();
  const partitions = parseTable(parsePackage(tableBytes({ todo: !legacy })));
  const memory = new Uint8Array(524288).fill(255), packets = [];
  const deviceFiles = completeBundle({ todo: todo === 'present' || todo === 'corrupt', legacy }).map(item => {
    const bytes = item.bytes.slice(), view = new DataView(bytes.buffer);
    view.setUint32(8, 9, true); view.setUint32(24, crc32(bytes.subarray(0, 24)), true);
    const partition = partitions.find(p => p.id === parsePackage(bytes).id);
    memory.set(bytes, partition.offset);
    return { name: item.name, bytes };
  });
  if (todo === 'corrupt') memory[partitions.find(partition => partition.id === 9).offset + 32] ^= 1;
  const connection = { async writeValueWithResponse(packet) {
    packets.push(packet);
    await beforeReply(packet);
    let reply;
    if (packet[1] === 0) {
      reply = statusBytes(packet[2], code); reply[6] = active;
      if (verify) reply[4] |= 16;
    }
    else if (packet[1] === 1) {
      const offset = new DataView(packet.buffer, packet.byteOffset, packet.byteLength).getUint32(3, true);
      reply = Uint8Array.of(0xa7, 1, packet[2], 0, ...memory.subarray(offset, offset + packet[7]));
    } else if (packet[1] === 9) {
      const partition = partitions.find(item => item.id === packet[3]);
      try {
        const header = parseHeader(memory.subarray(partition.offset, partition.offset + 32), partition);
        const pack = parsePackage(memory.subarray(partition.offset, partition.offset + 32 + header.length));
        reply = new Uint8Array(16); reply.set([0xa7, 9, packet[2], 0]);
        const view = new DataView(reply.buffer);
        view.setUint32(4, pack.revision, true); view.setUint32(8, pack.payload.length, true); view.setUint32(12, pack.crc, true);
      } catch (error) {
        reply = Uint8Array.of(0xa7, 9, packet[2], /未安装或未提交/.test(error.message) ? 4 : 3);
      }
    } else throw new Error('Readback must not mutate the device');
    fixture.installer.notification(reply);
  } };
  fixture.installer.bind(connection);
  return { ...fixture, memory, packets, deviceFiles, connection };
}

function captureReadbackDownload(fixture) {
  const archives = [], downloads = [], revoked = [];
  fixture.context.TextEncoder = TextEncoder;
  fixture.context.Blob = Blob;
  fixture.context.EPDResourceArchive = { createZip(entries) { archives.push(entries); return Uint8Array.of(80, 75); } };
  fixture.context.URL = { createObjectURL() { return 'blob:readback'; }, revokeObjectURL(url) { revoked.push(url); } };
  fixture.context.setTimeout = callback => callback();
  fixture.context.document.body = { appendChild() {} };
  fixture.context.document.createElement = () => ({ click() { downloads.push(this.download); }, remove() {} });
  return { archives, downloads, revoked };
}

test('readback downloads verified device bytes independently of the selected local bundle, including after disconnect', async () => {
  const fixture = readbackDevice(), { installer, node, packets, deviceFiles } = fixture;
  await installer.loadBundle(completeBundle(), 'local');
  const selected = installer.getBundle();
  assert.equal(node('externalResourceSave').disabled, true);
  await installer.readback();
  assert.equal(installer.getBundle(), selected);
  assert.equal(node('externalResourceSave').disabled, false);
  assert.equal(node('externalResourceProgress').value, 1);
  assert.match(node('deviceResourceVersions').textContent, /日历资源：v9/);
  assert.match(node('externalResourceReadbackStatus').textContent, /6 个资源包.*CRC 校验通过/);
  assert.ok(packets.every(packet => [0, 1, 9].includes(packet[1])));
  installer.disconnect();
  assert.equal(node('externalResourceSave').disabled, false);
  const capture = captureReadbackDownload(fixture);
  installer.saveReadback(); installer.saveReadback();
  assert.equal(capture.downloads.length, 2);
  assert.equal(capture.downloads[0], capture.downloads[1]);
  assert.match(capture.downloads[0], /^epd-resources-readback-\d{8}-\d{6}\.zip$/);
  for (const entry of capture.archives[0].filter(item => item.name.endsWith('.ers'))) {
    assert.deepEqual(entry.bytes, deviceFiles.find(item => item.name === entry.name).bytes);
  }
  const manifest = JSON.parse(new TextDecoder().decode(capture.archives[0].find(item => item.name === 'manifest.json').bytes));
  assert.equal(manifest.resources.length, 6);
  assert.ok(manifest.resources.every(item => item.revision === 9));
  assert.equal(manifest.flashCapacity, 524288);
  assert.deepEqual(capture.revoked, ['blob:readback', 'blob:readback']);
});

test('legacy devices export the original six packages without requiring todo inspection', async () => {
  const fixture = readbackDevice({ legacy: true });
  await fixture.installer.readback();
  assert.equal(fixture.node('externalResourceSave').disabled, false);
  assert.ok(fixture.packets.every(packet => [0, 1].includes(packet[1])));
  const capture = captureReadbackDownload(fixture); fixture.installer.saveReadback();
  assert.equal(capture.downloads.length, 1);
  const files = capture.archives[0].filter(item => item.name.endsWith('.ers'));
  assert.equal(files.length, 6); assert.equal(files.some(item => item.name === 'todo.ers'), false);
  const prepared = prepareBundle(files);
  assert.equal(prepared.partitions.length, 8); assert.equal(prepared.packages.length, 6);
});

test('valid optional todo data is included byte for byte in the seven-package backup', async () => {
  const fixture = readbackDevice({ todo: 'present' });
  await fixture.installer.readback();
  assert.equal(fixture.node('externalResourceSave').disabled, false);
  assert.match(fixture.node('externalResourceReadbackStatus').textContent, /7 个资源包/);
  assert.ok(fixture.packets.some(packet => packet[1] === 9 && packet[3] === 9));
  assert.ok(fixture.packets.every(packet => [0, 1, 9].includes(packet[1])));
  const capture = captureReadbackDownload(fixture); fixture.installer.saveReadback();
  assert.equal(capture.downloads.length, 1);
  const archive = capture.archives[0];
  const saved = archive.find(item => item.name === 'todo.ers');
  assert.deepEqual(saved.bytes, fixture.deviceFiles.find(item => item.name === 'todo.ers').bytes);
  const manifest = JSON.parse(new TextDecoder().decode(archive.find(item => item.name === 'manifest.json').bytes));
  assert.equal(manifest.resources.length, 7);
  assert.equal(manifest.resources.find(item => item.id === 9).revision, 9);
  assert.equal(prepareBundle(archive.filter(item => item.name.endsWith('.ers'))).packages.length, 7);
});

test('corrupt optional todo data prevents export rather than silently dropping personal tasks', async () => {
  const fixture = readbackDevice({ todo: 'corrupt' });
  await fixture.installer.readback();
  assert.equal(fixture.node('externalResourceSave').disabled, true);
  assert.match(fixture.node('externalResourceReadbackStatus').textContent, /待办资源校验失败/);
  const capture = captureReadbackDownload(fixture); fixture.installer.saveReadback();
  assert.equal(capture.downloads.length, 0);
});

test('firmware without VERIFY backs up optional todo using local CRC checks', async () => {
  for (const todo of ['present', 'missing']) {
    const fixture = readbackDevice({ verify: false, todo });
    await fixture.installer.readback();
    assert.equal(fixture.node('externalResourceSave').disabled, false);
    assert.ok(fixture.packets.every(packet => [0, 1].includes(packet[1])));
    const capture = captureReadbackDownload(fixture); fixture.installer.saveReadback();
    const files = capture.archives[0].filter(item => item.name.endsWith('.ers'));
    assert.equal(files.length, todo === 'present' ? 7 : 6);
    const saved = files.find(item => item.name === 'todo.ers');
    if (todo === 'present') assert.deepEqual(saved.bytes, fixture.deviceFiles.find(item => item.name === 'todo.ers').bytes);
    else assert.equal(saved, undefined);
  }
});

test('local CRC failure on optional todo also blocks backup without VERIFY', async () => {
  const fixture = readbackDevice({ verify: false, todo: 'corrupt' });
  await fixture.installer.readback();
  assert.equal(fixture.node('externalResourceSave').disabled, true);
  assert.match(fixture.node('externalResourceReadbackStatus').textContent, /待办事项回读失败.*CRC/);
  const capture = captureReadbackDownload(fixture); fixture.installer.saveReadback();
  assert.equal(capture.downloads.length, 0);
});

test('readback holds the device operation lock and disables competing resource actions', async () => {
  const started = deferred(), release = deferred();
  const fixture = readbackDevice({ async beforeReply(packet) {
    if (packet[1] === 1 && new DataView(packet.buffer).getUint32(3, true) === 0x18000) {
      started.resolve(); await release.promise;
    }
  } });
  fixture.context.DeviceOperations = {
    current: null,
    begin() { if (this.current) throw new Error('busy'); return this.current = {}; },
    assert(owner) { assert.equal(this.current, owner); },
    end(owner) { if (this.current === owner) { this.current = null; fixture.installer.controls(); } }
  };
  const work = fixture.installer.readback();
  await started.promise;
  for (const id of ['externalResourceReadback', 'externalResourceSave', 'externalResourceRefresh', 'externalResourceFolder'])
    assert.equal(fixture.node(id).disabled, true);
  assert.equal(fixture.installer.busy(), true);
  const packetCount = fixture.packets.length;
  await fixture.installer.refresh();
  assert.equal(fixture.packets.length, packetCount);
  release.resolve(); await work;
  assert.equal(fixture.context.DeviceOperations.current, null);
  assert.equal(fixture.installer.busy(), false);
  assert.equal(fixture.node('externalResourceReadback').disabled, false);
  assert.equal(fixture.node('externalResourceSave').disabled, false);
});

test('corrupt, missing or failed resource reads never expose an incomplete download', async () => {
  for (const failure of ['crc', 'missing', 'transport']) {
    const fixture = readbackDevice({ async beforeReply(packet) {
      if (failure === 'transport' && packet[1] === 1 && new DataView(packet.buffer).getUint32(3, true) >= 0x18000)
        throw new Error('GATT read failed');
    } });
    if (failure === 'crc') fixture.memory[0x18000 + 32] ^= 1;
    if (failure === 'missing') fixture.memory.fill(255, 0x18000, 0x18000 + 32);
    await fixture.installer.readback();
    assert.equal(fixture.node('externalResourceSave').disabled, true);
    assert.match(fixture.node('externalResourceReadbackStatus').textContent, /字体资源回读失败/);
    assert.equal(fixture.installer.busy(), false);
    const capture = captureReadbackDownload(fixture);
    fixture.installer.saveReadback();
    assert.equal(capture.downloads.length, 0);
  }
});

test('incompatible or actively updating devices cannot start a complete resource readback', async () => {
  for (const options of [{ code: 4 }, { code: 3 }, { active: 7 }]) {
    const fixture = readbackDevice(options);
    await fixture.installer.readback();
    assert.equal(fixture.packets.length, 1);
    assert.equal(fixture.node('externalResourceSave').disabled, true);
    assert.match(fixture.node('externalResourceReadbackStatus').textContent, /回读失败/);
  }
});

test('restarting a readback clears the old download, and a failed retry cannot download previous data', async () => {
  const fixture = readbackDevice();
  await fixture.installer.readback();
  assert.equal(fixture.node('externalResourceSave').disabled, false);
  fixture.memory[0x18000 + 32] ^= 1;
  const work = fixture.installer.readback();
  assert.equal(fixture.node('externalResourceSave').disabled, true);
  await work;
  assert.equal(fixture.node('externalResourceSave').disabled, true);
});

test('replacing a connection rejects late readback results and clears successful cached bytes', async () => {
  const started = deferred(), release = deferred();
  let wait = false;
  class Client {
    status() { const bytes = statusBytes(0); bytes[4] |= 16; return bytes; }
    partitions() { return parseTable(parsePackage(tableBytes())); }
    inspect(id) { assert.equal(id, 9); return { code: 4 }; }
    async readPackage(partition) {
      if (wait && partition.id === 7) { started.resolve(); await release.promise; }
      return completeBundle().find(item => parsePackage(item.bytes).id === partition.id).bytes;
    }
    disconnect() {}
  }
  const fixture = installerFixture(Client);
  fixture.installer.bind({});
  await fixture.installer.readback();
  assert.equal(fixture.node('externalResourceSave').disabled, false);
  fixture.installer.bind({});
  assert.equal(fixture.node('externalResourceSave').disabled, true);
  wait = true;
  const oldWork = fixture.installer.readback();
  await started.promise;
  fixture.installer.bind({});
  fixture.node('externalResourceStatus').textContent = 'new connection';
  release.resolve(); await oldWork;
  assert.equal(fixture.node('externalResourceSave').disabled, true);
  assert.equal(fixture.node('externalResourceStatus').textContent, 'new connection');
  assert.equal(fixture.installer.busy(), false);
});
