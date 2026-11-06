const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const store = require('../html/js/resource_store.js');
const { createZip } = require('../html/js/resource_archive.js');

function checkArchive(bytes, entries) {
  assert.ok(bytes instanceof Uint8Array);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const end = bytes.length - 22;
  assert.equal(view.getUint32(end, true), 0x06054b50);
  assert.equal(view.getUint16(end + 4, true), 0);
  assert.equal(view.getUint16(end + 6, true), 0);
  assert.equal(view.getUint16(end + 8, true), entries.length);
  assert.equal(view.getUint16(end + 10, true), entries.length);
  assert.equal(view.getUint16(end + 20, true), 0);
  const directorySize = view.getUint32(end + 12, true);
  const directoryOffset = view.getUint32(end + 16, true);
  assert.equal(directoryOffset + directorySize, end);
  let localEnd = 0, directoryAt = directoryOffset;
  for (const entry of entries) {
    assert.equal(view.getUint32(directoryAt, true), 0x02014b50);
    assert.equal(view.getUint16(directoryAt + 6, true), 20);
    assert.equal(view.getUint16(directoryAt + 8, true), 0);
    assert.equal(view.getUint16(directoryAt + 10, true), 0);
    const crc = view.getUint32(directoryAt + 16, true);
    assert.equal(crc, store.crc32(entry.bytes));
    assert.equal(view.getUint32(directoryAt + 20, true), entry.bytes.length);
    assert.equal(view.getUint32(directoryAt + 24, true), entry.bytes.length);
    const nameLength = view.getUint16(directoryAt + 28, true);
    assert.equal(view.getUint16(directoryAt + 30, true), 0);
    assert.equal(view.getUint16(directoryAt + 32, true), 0);
    assert.equal(view.getUint16(directoryAt + 34, true), 0);
    const localAt = view.getUint32(directoryAt + 42, true);
    assert.equal(localAt, localEnd);
    assert.equal(Buffer.from(bytes.subarray(directoryAt + 46, directoryAt + 46 + nameLength)).toString('ascii'), entry.name);
    assert.equal(view.getUint32(localAt, true), 0x04034b50);
    assert.equal(view.getUint16(localAt + 4, true), 20);
    assert.equal(view.getUint16(localAt + 6, true), 0);
    assert.equal(view.getUint16(localAt + 8, true), 0);
    assert.equal(view.getUint32(localAt + 14, true), crc);
    assert.equal(view.getUint32(localAt + 18, true), entry.bytes.length);
    assert.equal(view.getUint32(localAt + 22, true), entry.bytes.length);
    assert.equal(view.getUint16(localAt + 26, true), nameLength);
    assert.equal(view.getUint16(localAt + 28, true), 0);
    assert.equal(Buffer.from(bytes.subarray(localAt + 30, localAt + 30 + nameLength)).toString('ascii'), entry.name);
    const dataAt = localAt + 30 + nameLength;
    assert.deepEqual(bytes.subarray(dataAt, dataAt + entry.bytes.length), entry.bytes);
    localEnd = dataAt + entry.bytes.length;
    directoryAt += 46 + nameLength;
  }
  assert.equal(localEnd, directoryOffset);
  assert.equal(directoryAt, end);
}

test('STORE archive preserves all six ERS files and the manifest byte for byte', () => {
  const names = ['partition-table.ers', 'calendar.ers', 'ui.ers', 'fonts.ers', 'epd-config.ers', 'calendar-annotations.ers'];
  const entries = names.map((name, i) => ({ name, bytes: Uint8Array.from([0x45, 0x52, 0x53, 0x31, i, 0, 0xff, 0x80]) }));
  entries.push({ name: 'manifest.json', bytes: new TextEncoder().encode('{"source":"device","deviceName":"墨水屏"}\n') });
  const original = entries.map(entry => entry.bytes.slice());
  checkArchive(createZip(entries), entries);
  entries.forEach((entry, i) => assert.deepEqual(entry.bytes, original[i]));
});

test('archive preserves subarray boundaries, a large payload, and an empty file', () => {
  const backing = Uint8Array.of(99, 0, 1, 2, 255, 88);
  const payload = new Uint8Array(524256);
  for (let i = 0; i < payload.length; ++i) payload[i] = i & 255;
  checkArchive(createZip([
    { name: 'part.ers', bytes: backing.subarray(1, 5) },
    { name: 'fonts.ers', bytes: payload },
    { name: 'empty.txt', bytes: new Uint8Array() },
  ]), [
    { name: 'part.ers', bytes: Uint8Array.of(0, 1, 2, 255) },
    { name: 'fonts.ers', bytes: payload },
    { name: 'empty.txt', bytes: new Uint8Array() },
  ]);
});

test('empty archive has a valid end record', () => {
  const archive = createZip([]);
  assert.equal(archive.length, 22);
  checkArchive(archive, []);
});

test('archive rejects ambiguous paths, duplicate names, and invalid byte data', () => {
  for (const name of ['', ' ', '.', '..', '../ui.ers', 'dir/ui.ers', 'dir\\ui.ers', 'C:ui.ers', '日历.ers', 'bad\nname', 'x'.repeat(65536)]) {
    assert.throws(() => createZip([{ name, bytes: Uint8Array.of(1) }]), /文件名/);
  }
  assert.throws(() => createZip([{ name: 'ui.ers', bytes: [] }]), /数据无效/);
  assert.throws(() => createZip([{ name: 'ui.ers', bytes: Uint8Array.of(1) }, { name: 'ui.ers', bytes: Uint8Array.of(2) }]), /重复/);
  assert.throws(() => createZip(null), /列表无效/);
  assert.throws(() => createZip(new Array(65536)), /列表无效/);
});

test('browser export uses the supplied resource CRC implementation', () => {
  let checks = 0;
  const context = vm.createContext({ Uint8Array, DataView, EPDResourceStore: {
    crc32(bytes) { checks++; return store.crc32(bytes); },
  } });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../html/js/resource_archive.js'), 'utf8'), context);
  const entries = [{ name: 'ui.ers', bytes: Uint8Array.of(1, 2, 3) }];
  checkArchive(context.EPDResourceArchive.createZip(entries), entries);
  assert.equal(checks, 1);
});
