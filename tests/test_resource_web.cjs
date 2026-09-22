const { test } = require('node:test');
const assert = require('node:assert/strict');
const { ResourceClient, crc32, parsePackage } = require('../html/js/resource_store.js');
function packageBytes() {
  const b = new Uint8Array(61), view = new DataView(b.buffer);
  b.fill(0x39, 32);
  [0x31535245, 0x10002, 1, 29, crc32(b.subarray(32)), 524288].forEach((v, i) => view.setUint32(i * 4, v, true));
  view.setUint32(24, crc32(b.subarray(0, 24)), true);
  view.setUint32(28, 0xa55ac33c, true);
  return b;
}
test('CRC and package rejects corruption/truncation', () => {
  assert.equal(crc32(Buffer.from('123456789')), 0xcbf43926);
  assert.equal(parsePackage(packageBytes()).id, 2);
  const b = packageBytes(); b[50] ^= 1;
  assert.throws(() => parsePackage(b));
  assert.throws(() => parsePackage(new Uint8Array(31)));
});
test('upload uses stop-and-wait packets bounded to default ATT payload', async () => {
  const packets = [];
  const client = new ResourceClient(async p => {
    packets.push(p);
    const a = new Uint8Array(p[1] === 0 ? 16 : 4);
    a.set([0xa7, p[1], p[2], 0]);
    if (p[1] === 0) a[4] = 1;
    client.receive(a);
  });
  await client.upload(packageBytes());
  assert.deepEqual(packets.map(p => p[1]), [0, 6, 5, 2, 3, 3, 3, 4]);
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
test('normal firmware refuses upload before ENTER or erase', async () => {
  const packets = [];
  const client = new ResourceClient(async p => {
    packets.push(p);
    const a = new Uint8Array(16); a.set([0xa7, 0, p[2], 4]);
    client.receive(a);
  });
  await assert.rejects(client.upload(packageBytes()), /维护版本/);
  assert.equal(packets.length, 1);
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
