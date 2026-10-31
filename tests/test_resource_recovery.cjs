'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const firmwarePolicy = require('../html/js/main.js');
const store = require('../html/js/resource_store.js');
const builder = require('../html/js/resource_builder.js');
const glyphs = require('../html/js/font_builder.js');
const sync = require('../html/js/font_sync.js');
const packer = require('../html/js/ui_tools/resource_pack.js');
const fonts = ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn'].map(name =>
  new Uint8Array(fs.readFileSync(`html/resources/fonts/u8g2_font_${name}.bin`)));
const base = packer.create({ fonts }, { builtAt: 1 });
const rasterize = async () => ({ width: 2, height: 2, x: 0, y: 0, advance: 3, pixels: Uint8Array.of(1, 0, 0, 1) });

function fixture({ blank = false, verify = true, offline = false } = {}) {
  const installed = new Map(blank ? [] : base.packages.map(item => [item.pack.id, item.bytes]));
  const nodes = new Map(), uploads = [], backups = [], state = { fail: null, backupFail: false, leaveFail: false };
  const node = id => {
    if (!nodes.has(id)) nodes.set(id, { classList: { add() {}, remove() {} }, addEventListener() {} });
    return nodes.get(id);
  };
  class Client {
    status() { if (state.panelBusy) throw new Error('资源请求超时：屏幕正在刷新');
      const bytes = new Uint8Array(16); bytes.set([0xa7, 0, 0, installed.has(1) ? 0 : 4, (verify ? 0x13 : 0x3) | (offline ? 4 : 0), 1]);
      new DataView(bytes.buffer).setUint32(12, 524288, true); return bytes; }
    partitions() { return base.partitions; }
    inspect(id) { if (!installed.has(id)) return { code: 4 };
      const pack = store.parsePackage(installed.get(id));
      return { code: 0, length: pack.payload.length, crc: pack.crc, revision: pack.revision }; }
    readPackage(partition) { if (state.backupFail) throw new Error('backup transport failed');
      backups.push(partition.id); return installed.get(partition.id).slice(); }
    readRange(offset, length) { const partition = base.partitions.find(part => part.offset === offset);
      const bytes = new Uint8Array(length).fill(255), original = installed.get(partition.id);
      if (original) bytes.set(original.subarray(0, length)); return bytes; }
    upload(bytes) { const pack = store.parsePackage(bytes); uploads.push(pack.id);
      if (state.fail === pack.id) { installed.delete(pack.id); throw new Error(`upload ${pack.id} failed after erase`); }
      installed.set(pack.id, bytes.slice()); }
    leave() { if (state.leaveFail) throw new Error('reload failed'); state.panelBusy = !!state.refreshAfterLeave; }
    runOffline() { if (state.offlineFail) throw new Error('资源请求超时（应用资源并断开连接）'); }
    disconnect() {}
  }
  const context = { document: { getElementById: node }, Uint8Array, TextEncoder,
    EPDResourceStore: { ...store, ResourceClient: Client }, EPDResourcePack: packer, EPDFontSync: sync };
  context.appVersion = firmwarePolicy.newWebMinVersion;
  context.EpdFirmwarePolicy = firmwarePolicy;
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_installer.js'), 'utf8'), context);
  const installer = context.ExternalResourceInstaller;
  function bind(id = 'same-device') { installer.bind({ service: { device: { id } } }); }
  function archive() {
    let captured;
    context.EPDResourceArchive = { createZip(entries) { captured = entries; return new Uint8Array(1); } };
    context.URL = { createObjectURL() { return 'blob:backup'; }, revokeObjectURL() {} };
    context.Blob = Blob; context.setTimeout = () => {};
    context.document.createElement = () => ({ click() {}, remove() {} });
    context.document.body = { appendChild() {} };
    installer.saveRecovery(); delete context.document.createElement;
    return captured;
  }
  return { installed, node, installer, context, uploads, backups, state, bind, archive };
}

test('regular installer does not rewrite identical merged fonts or downgrade their revision', async () => {
  const f = fixture();
  const extended = await glyphs.extendFonts(fonts, [{ slot: 0, text: '生' }], { rasterize });
  const bytes = builder.wrap(7, builder.encodeFonts(extended.fonts), 2);
  f.installed.set(7, bytes);
  await f.installer.loadBundle(base.packages); f.bind();
  await f.installer.install(); await f.installer.install();
  assert.deepEqual(f.uploads, []);
  assert.deepEqual(f.installed.get(7), bytes);
});

test('confirmed activation completes installation without polling status during the first panel refresh', async () => {
  const f = fixture({blank:true});
  f.state.refreshAfterLeave = true;
  await f.installer.loadBundle(base.packages); f.bind();
  await f.installer.install();
  assert.equal(f.installed.size, 7);
  assert.deepEqual(f.uploads, [1,5,6,7,2,3,9]);
  assert.match(f.node('externalResourceStatus').textContent, /资源已校验并应用/);
  assert.doesNotMatch(f.node('externalResourceStatus').textContent, /超时|失败/);
  assert.match(f.node('externalResourceRecoveryStatus').textContent, /发送完成/);
  assert.equal(f.node('externalResourceRetry').disabled, true);
});

test('a later auto-offline timeout reports that phase without marking confirmed packages for reinstall', async () => {
  const f=fixture({blank:true,offline:true});f.state.offlineFail=true;
  f.node('externalResourceAutoOffline').checked=true;
  await f.installer.loadBundle(base.packages);f.bind();await f.installer.install();
  assert.equal(f.installed.size,7);
  assert.match(f.node('externalResourceStatus').textContent,/资源已校验并应用.*自动断开未完成.*超时/);
  assert.match(f.node('externalResourceRecoveryStatus').textContent,/发送完成/);
  assert.equal(f.node('externalResourceRetry').disabled,true);
});

test('normal and forced installs repair malformed RLE and retain downloadable original bytes', async () => {
  for (const force of [false, true]) {
    const f = fixture();
    const corruptFonts = fonts.map(font => font.slice()); corruptFonts[0][2] = corruptFonts[0][3] = 8;
    const corrupt = builder.wrap(7, builder.encodeFonts(corruptFonts), 2);
    f.installed.set(7, corrupt);
    await f.installer.loadBundle(base.packages); f.bind(); f.node('externalResourceForce').checked = force;
    await f.installer.install();
    assert.ok(f.uploads.includes(7));
    builder.decodeFonts(store.parsePackage(f.installed.get(7)).payload).forEach((font, slot) => glyphs.validateGlyphs(font, slot));
    assert.match(f.node('externalResourceRecoveryStatus').textContent, /已备份原始资源/);
    const archive = f.archive();
    assert.deepEqual(archive.find(entry => entry.name === 'before/fonts.ers').bytes, corrupt);
    assert.ok(archive.some(entry => entry.name === 'planned/fonts.ers'));
  }
});

test('interrupted update retries exact prepared fonts after reconnect, skipping confirmed packages even with force enabled', async () => {
  const f = fixture();
  const extended = await glyphs.extendFonts(fonts, [{ slot: 0, text: '生' }], { rasterize });
  const original = builder.wrap(7, builder.encodeFonts(extended.fonts), 2);
  f.installed.set(7, original);
  await f.installer.loadBundle(base.packages); f.bind(); f.node('externalResourceForce').checked = true;
  f.state.fail = 7; f.state.leaveFail = true;
  await f.installer.install();
  assert.deepEqual(f.uploads, [5, 6, 7]);
  assert.match(f.node('externalResourceRecoveryStatus').textContent, /日历资源：已提交.*字体资源：未完成/);
  assert.match(f.node('externalResourceStatus').textContent, /upload 7.*设备应用失败/);
  assert.deepEqual(f.archive().find(entry => entry.name === 'before/fonts.ers').bytes, original);
  f.installer.disconnect(); f.bind(); f.uploads.length = 0; f.state.fail = null; f.state.leaveFail = false;
  assert.equal(f.node('externalResourceRetry').disabled, false);
  await f.installer.retry();
  assert.deepEqual(f.uploads, [7]);
  assert.deepEqual(f.installed.get(7), original);
  assert.equal(f.node('externalResourceRetry').disabled, true);
});

test('blank-device retry completes personal resource initialization after its table has committed', async () => {
  const f = fixture({ blank: true });
  await f.installer.loadBundle(base.packages); f.bind(); f.state.fail = 7;
  await f.installer.install();
  assert.deepEqual(f.uploads, [1, 5, 6, 7]);
  f.installer.disconnect(); f.bind(); f.uploads.length = 0; f.state.fail = null;
  await f.installer.retry();
  assert.deepEqual(f.uploads, [7, 2, 3, 9]);
  assert.equal(f.installed.size, 7);
});

test('legacy retry verifies committed contents locally before skipping them', async () => {
  const f = fixture({ blank: true, verify: false });
  await f.installer.loadBundle(base.packages); f.bind(); f.state.fail = 7;
  await f.installer.install(); f.installer.disconnect(); f.bind(); f.uploads.length = 0; f.state.fail = null;
  await f.installer.retry();
  assert.deepEqual(f.uploads, [7, 2, 3, 9]);
});

test('recovery refuses a different device or changed resource selection and backup transport failures write nothing', async () => {
  const f = fixture({ blank: true });
  await f.installer.loadBundle(base.packages); f.bind(); f.state.fail = 7;
  await f.installer.install(); f.uploads.length = 0;
  f.installer.disconnect(); f.bind('another-device');
  await f.installer.retry(); assert.deepEqual(f.uploads, []);
  assert.equal(f.node('externalResourceRetry').disabled, true);
  f.bind(); await f.installer.loadBundle(base.packages);
  await f.installer.retry(); assert.deepEqual(f.uploads, []);
  const backup = fixture(); await backup.installer.loadBundle(base.packages); backup.bind();
  backup.node('externalResourceForce').checked = true; backup.state.backupFail = true;
  await backup.installer.install(); assert.deepEqual(backup.uploads, []);
  assert.match(backup.node('externalResourceStatus').textContent, /backup transport failed/);
});
