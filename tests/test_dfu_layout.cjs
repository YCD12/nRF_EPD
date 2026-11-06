'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { createZip } = require('../html/js/resource_archive.js');

const filename = path.resolve(__dirname, '../html/js/dfu.js');
const source = fs.readFileSync(filename, 'utf8');
const endOfModule = /\}\)\(\);\s*$/;
assert.match(source, endOfModule, 'DFU module must remain an IIFE');
// Expose the real private function only in this VM; the browser API is unchanged.
const instrumented = source.replace(endOfModule,
  'window.__dfuLayoutPrecheck = updateVersionPrecheck;\n' +
  'window.__dfuPackage = () => dfuPackage;\n' +
  'window.__setDfuSession = setPendingManualSession;\n})();');

function fixture() {
  const elements = new Map();
  const element = id => {
    if (!elements.has(id)) {
      const classes = new Set();
      elements.set(id, {
        textContent: '', style: {}, dataset: {}, classes, listeners: new Map(),
        addEventListener(name, listener) { this.listeners.set(name, listener); },
        classList: {
          toggle(name, enabled) { if (enabled) classes.add(name); else classes.delete(name); },
          remove(name) { classes.delete(name); },
        },
      });
    }
    return elements.get(id);
  };
  const forbidPrompt = () => { throw new Error('Layout precheck must not request confirmation'); };
  const window = { confirm: forbidPrompt };
  const context = vm.createContext({
    window,
    document: { getElementById: element },
    confirm: forbidPrompt,
    TextDecoder,
    addLog() {},
  });
  vm.runInContext(instrumented, context, { filename });
  window.initDfuPanel();
  return {
    check: window.__dfuLayoutPrecheck,
    text: () => element('dfuPrecheck').textContent,
    visible: () => element('dfuPrecheck').style.display,
    warned: () => element('dfuPrecheck').classes.has('warn'),
    status: () => element('dfuStatus').textContent,
    loadedName: () => window.__dfuPackage()?.file.name,
    pending: value => window.__setDfuSession(value),
    fileDisabled: () => element('dfuPackageFile').disabled,
    select: file => element('dfuPackageFile').listeners.get('change')({ target: { files: [file] } }),
  };
}

function image(size, type = 'application') {
  return { type, initData: new ArrayBuffer(0), imageData: new ArrayBuffer(size) };
}

function packageFile(name, appSize, baseSize = null) {
  const manifest = { application: { dat_file: 'app.dat', bin_file: 'app.bin' } };
  const files = [
    { name: 'app.dat', bytes: Uint8Array.of(0x18, 1) },
    { name: 'app.bin', bytes: new Uint8Array(appSize) },
  ];
  if (baseSize !== null) {
    manifest.softdevice_bootloader = { dat_file: 'base.dat', bin_file: 'base.bin' };
    files.push({ name: 'base.dat', bytes: Uint8Array.of(0x18, 1) },
      { name: 'base.bin', bytes: new Uint8Array(baseSize) });
  }
  files.push({ name: 'manifest.json', bytes: new TextEncoder().encode(JSON.stringify({ manifest })) });
  const bytes = createZip(files);
  return { name, arrayBuffer: async () => bytes.buffer };
}

const migrationHint = /4 KiB 保留区 Bootloader|旧 Bootloader 上限|需要已安装/;
const cacheEstimateHint = /DFU 缓存估算 56\.00 KB/;

for (const { size, oversize } of [
  { size: 56 * 1024 - 1, oversize: false },
  { size: 56 * 1024, oversize: false },
  { size: 56 * 1024 + 1, oversize: true },
  { size: 60 * 1024, oversize: true },
]) {
  test(`application ${size} B respects the 56 KiB limit with no bootloader migration hint`, () => {
    const f = fixture();
    const result = f.check(null, image(size));
    assert.match(f.text(), /上限 56\.00 KB \(起始 0x19000\)/);
    assert.equal(f.visible(), 'block');
    assert.doesNotMatch(f.text(), migrationHint);
    assert.equal(Boolean(result.sizeError), oversize);
    assert.equal(result.capacityWarn, false);
    assert.equal(f.warned(), oversize);
    if (oversize) assert.match(f.text(), /错误: 应用镜像超过工程配置应用区上限/);
  });
}

for (const { size, capacityWarn } of [
  { size: 56 * 1024 - 1, capacityWarn: false },
  { size: 56 * 1024, capacityWarn: false },
  { size: 56 * 1024 + 1, capacityWarn: true },
  { size: 60 * 1024, capacityWarn: true },
]) {
  test(`combo base ${size} B uses the 56 KiB cache estimate independently of application limit`, () => {
    const f = fixture();
    const result = f.check(image(size, 'softdevice_bootloader'), image(56 * 1024));
    assert.equal(result.sizeError, '');
    assert.equal(result.capacityWarn, capacityWarn);
    assert.equal(f.warned(), capacityWarn);
    assert.equal(cacheEstimateHint.test(result.capacityHint), capacityWarn);
    assert.doesNotMatch(f.text(), migrationHint);
    assert.match(result.capacityHint, /取决于设备当前固件实际占用/);
    if (!capacityWarn) assert.match(result.capacityHint, /网页只能做近似预检查/);
  });
}

test('combo application oversize does not create a cache warning for a base image within the estimate', () => {
  const f = fixture();
  const result = f.check(image(56 * 1024, 'softdevice_bootloader'), image(56 * 1024 + 1));
  assert.ok(result.sizeError);
  assert.equal(result.capacityWarn, false);
  assert.doesNotMatch(f.text(), migrationHint);
  assert.match(f.text(), /基础镜像\(softdevice_bootloader\): 56\.00 KB/);
  assert.equal(f.warned(), true);
});

test('combo application oversize and cache warning remain independent', () => {
  const f = fixture();
  const result = f.check(image(56 * 1024 + 1, 'softdevice_bootloader'), image(56 * 1024 + 1));
  assert.ok(result.sizeError);
  assert.equal(result.capacityWarn, true);
  assert.match(f.text(), /应用镜像超过工程配置应用区上限/);
  assert.match(result.capacityHint, cacheEstimateHint);
  assert.equal(f.warned(), true);
});

test('rechecking a package within 56 KiB clears stale size errors', () => {
  const f = fixture();
  assert.ok(f.check(null, image(56 * 1024 + 1)).sizeError);
  assert.equal(f.warned(), true);
  const result = f.check(null, image(56 * 1024));
  assert.equal(result.sizeError, '');
  assert.doesNotMatch(f.text(), migrationHint);
  assert.doesNotMatch(f.text(), /错误:/);
  assert.equal(f.warned(), false);
});

test('selecting another real ZIP package clears prior size and combo-cache warnings', async () => {
  const f = fixture();
  await f.select(packageFile('oversize.zip', 56 * 1024 + 1));
  assert.match(f.status(), /固件包已就绪：oversize\.zip/);
  assert.match(f.text(), /错误: 应用镜像超过工程配置应用区上限/);
  assert.equal(f.warned(), true);

  await f.select(packageFile('combo.zip', 56 * 1024, 56 * 1024 + 1));
  assert.match(f.status(), /固件包已就绪：combo\.zip/);
  assert.doesNotMatch(f.text(), /错误:/);
  assert.match(f.text(), cacheEstimateHint);
  assert.equal(f.warned(), true);

  await f.select(packageFile('app.zip', 56 * 1024));
  assert.match(f.status(), /固件包已就绪：app\.zip/);
  assert.match(f.text(), /应用镜像: 56\.00 KB \/ 上限 56\.00 KB/);
  assert.doesNotMatch(f.text(), /错误:|基础镜像|DFU 缓存估算/);
  assert.doesNotMatch(f.text(), migrationHint);
  assert.equal(f.warned(), false);
});

test('empty precheck does not invent application size or a bootloader migration', () => {
  const f = fixture();
  const result = f.check(null, null);
  assert.equal(result.sizeError, '');
  assert.equal(result.capacityWarn, false);
  assert.doesNotMatch(f.text(), /应用镜像:|需要已安装/);
  assert.equal(f.warned(), false);
});

function deferred() {
  let resolve, reject;
  const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
  return { promise, resolve, reject };
}

test('an invalid replacement ZIP cannot leave the previous firmware selected', async () => {
  const f = fixture();
  await f.select(packageFile('old.zip', 32));
  await f.select({ name: 'broken.zip', arrayBuffer: async () => new ArrayBuffer(0) });
  assert.match(f.status(), /固件包加载失败/);
  assert.equal(f.loadedName(), undefined);
  assert.equal(f.visible(), 'none');
});

for (const manifest of [{}, { application: { dat_file: 'missing.dat', bin_file: 'missing.bin' } }]) {
  test(`a parsed manifest without usable images cannot become the active package: ${JSON.stringify(manifest)}`, async () => {
    const f = fixture();
    await f.select(packageFile('old.zip', 32));
    const zip = createZip([{ name: 'manifest.json',
      bytes: new TextEncoder().encode(JSON.stringify({ manifest })) }]);
    await f.select({ name: 'invalid-images.zip', arrayBuffer: async () => zip.buffer });
    assert.match(f.status(), /固件包加载失败/);
    assert.equal(f.loadedName(), undefined);
    assert.equal(f.visible(), 'none');
  });
}

test('selecting or clearing a file immediately invalidates the previously loaded package', async () => {
  const f = fixture();
  await f.select(packageFile('old.zip', 32));
  const bytes = deferred();
  const loading = f.select({ name: 'slow.zip', arrayBuffer: () => bytes.promise });
  assert.equal(f.loadedName(), undefined, 'starting during parsing must not use the old package');
  await f.select(null);
  assert.match(f.status(), /未选择 DFU 固件包/);
  bytes.resolve(await packageFile('slow.zip', 64).arrayBuffer());
  await loading;
  assert.equal(f.loadedName(), undefined, 'clearing must invalidate an in-flight ZIP load');
});

for (const fails of [false, true]) {
  test(`latest file selection owns the package and status when an old load finishes (fails=${fails})`, async () => {
    const f = fixture();
    const bytes = deferred();
    const oldLoad = f.select({ name: 'old.zip', arrayBuffer: () => bytes.promise });
    await f.select(packageFile('latest.zip', 64));
    if (fails) bytes.reject(new Error('old read failed'));
    else bytes.resolve(await packageFile('old.zip', 32).arrayBuffer());
    await oldLoad;
    assert.equal(f.loadedName(), 'latest.zip');
    assert.match(f.status(), /固件包已就绪：latest\.zip/);
  });
}

test('firmware selection is locked while a resumable DFU session owns its images', async () => {
  const f = fixture();
  await f.select(packageFile('active.zip', 32));
  f.pending({ remainingImages: [] });
  assert.equal(f.fileDisabled(), true);
  await f.select(packageFile('other.zip', 64));
  assert.equal(f.loadedName(), 'active.zip');
  f.pending(null);
  assert.equal(f.fileDisabled(), false);
  await f.select(packageFile('other.zip', 64));
  assert.equal(f.loadedName(), 'other.zip');
});
