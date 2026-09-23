const { test } = require('node:test');
const assert = require('node:assert/strict');
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
function statusBytes(token, writable = true, code = 0) {
  const b = new Uint8Array(16); b.set([0xa7, 0, token, code, Number(writable), 1]);
  new DataView(b.buffer).setUint32(12, 524288, true); return b;
}
function deviceClient({ writable = true, table = tableBytes(), status = null } = {}) {
  const memory = new Uint8Array(524288).fill(255), packets = [];
  memory.set(table); memory.set(packageBytes(), 4096);
  const client = new ResourceClient(async p => {
    packets.push(p);
    let a = Uint8Array.of(0xa7, p[1], p[2], 0);
    if (p[1] === 0) { a = status || statusBytes(p[2], writable); a[2] = p[2]; }
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
test('install plan initializes only a missing table and preserves an existing table', () => {
  const bundle = prepareBundle(completeBundle());
  const partitions = parseTable(parsePackage(tableBytes()));
  assert.deepEqual(installationPlan(bundle, 4, null).map(item => item.pack.id), [1, 5, 6, 7, 2, 3]);
  assert.deepEqual(installationPlan(bundle, 0, partitions).map(item => item.pack.id), [5, 6, 7, 2, 3]);
  assert.throws(() => installationPlan(bundle, 3, partitions), /不能按空白/);
});
test('upload uses stop-and-wait packets bounded to default ATT payload', async () => {
  const { client, packets } = deviceClient();
  await client.upload(packageBytes());
  assert.deepEqual(packets.filter(p => p[1] !== 1).map(p => p[1]), [0, 6, 5, 2, 3, 3, 3, 4]);
  assert.ok(packets.every(p => p.length <= 20));
  assert.deepEqual(packets.filter(p => p[1] === 3).map(p => new DataView(p.buffer).getUint32(3, true)), [0, 13, 26]);
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
test('read-only firmware refuses upload before ENTER or erase', async () => {
  const packets = [];
  const client = new ResourceClient(async p => {
    packets.push(p);
    const a = statusBytes(p[2], false, 4);
    client.receive(a);
  });
  await assert.rejects(client.upload(packageBytes()), /full HEX/);
  assert.equal(packets.length, 1);
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

test('writable firmware can repair absent table, but cannot upload other resources without it', async () => {
  const { client, packets } = deviceClient({ table: new Uint8Array(160).fill(255), status: statusBytes(0, true, 4) });
  await assert.rejects(client.upload(packageBytes()), /未安装/);
  assert.ok(packets.every(p => p[1] < 2));
  await client.upload(tableBytes());
  assert.ok(packets.some(p => p[1] === 2 && p[3] === 1));
});

test('short or incompatible capability and an active transaction cannot start upload', async () => {
  for (const [i, status] of [Uint8Array.of(0xa7, 0, 0, 5), statusBytes(0), statusBytes(0), statusBytes(0)].entries()) {
    if (status.length === 16) {
      if (i === 1) status[5] = 2;
      else if (i === 2) status[12] = 1;
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
