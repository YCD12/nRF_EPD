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
function tableBytes() {
  const b = new Uint8Array(layout.partitions.length * 16), v = new DataView(b.buffer);
  layout.partitions.forEach((p, i) => {
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
function deviceClient({ table = tableBytes(), status = null } = {}) {
  const memory = new Uint8Array(524288).fill(255), packets = [];
  memory.set(table); memory.set(packageBytes(), 4096);
  const client = new ResourceClient(async p => {
    packets.push(p);
    let a = Uint8Array.of(0xa7, p[1], p[2], 0);
    if (p[1] === 0) { a = status || statusBytes(p[2]); a[2] = p[2]; }
    if (p[1] === 1) {
      const at = new DataView(p.buffer).getUint32(3, true);
      a = Uint8Array.of(...a, ...memory.subarray(at, at + p[7]));
    }
    client.receive(a);
  });
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
function completeBundle() {
  return [
    { name: 'calendar-annotations.ers', bytes: wrap(3, Uint8Array.of(3)) },
    { name: 'fonts.ers', bytes: wrap(7, Uint8Array.of(7)) },
    { name: 'partition-table.ers', bytes: tableBytes() },
    { name: 'ui.ers', bytes: wrap(6, Uint8Array.of(6)) },
    { name: 'epd-config.ers', bytes: packageBytes() },
    { name: 'calendar.ers', bytes: wrap(5, Uint8Array.of(5)) },
  ];
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
  assert.equal(result.partitions.length, 8);
  assert.equal(result.total, parsePackage(tableBytes()).payload.length + 33);

  assert.throws(() => prepareBundle(completeBundle().slice(1)), /缺少/);
  assert.throws(() => prepareBundle([...completeBundle(), { name: 'duplicate.ers', bytes: wrap(2, Uint8Array.of(2)) }]), /重复/);
  assert.throws(() => prepareBundle([...completeBundle().slice(0, -1), { name: 'unsupported.ers', bytes: wrap(9, Uint8Array.of(9)) }]), /缺少|不支持/);
});
test('install plan initializes blank Flash and preserves the table and personal data on existing devices', () => {
  const bundle = prepareBundle(completeBundle());
  const partitions = parseTable(parsePackage(tableBytes()));
  assert.deepEqual(installationPlan(bundle, 4, null).map(item => item.pack.id), [1, 5, 6, 7, 2, 3]);
  assert.deepEqual(installationPlan(bundle, 0, partitions).map(item => item.pack.id), [5, 6, 7]);
  assert.throws(() => installationPlan(bundle, 3, partitions), /不能按空白/);
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
  assert.equal(entries.length, 8);
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
  for (const b of [wrap(4, Uint8Array.of(1)), wrap(8, Uint8Array.of(1)), wrap(9, Uint8Array.of(1)), wrap(2, new Uint8Array(4065))]) {
    const { client, packets } = deviceClient();
    await assert.rejects(client.upload(b));
    assert.ok(packets.every(p => p[1] < 2));
  }
  assert.throws(() => validateTarget(parsePackage(packageBytes()), []), /先安装/);
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
  return { installer: context.ExternalResourceInstaller, node,
    select: files => listeners.get('externalResourceFolder:change')({ target: { files } }) };
}

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
  await fetching.promise;
  await installer.loadBundle(completeBundle(), 'manual');
  const selected = installer.getBundle();
  slow.resolve();
  await new Promise(setImmediate);
  assert.equal(installer.getBundle(), selected);
  assert.match(node('externalResourceBundle').textContent, /已加载 manual/);
});
