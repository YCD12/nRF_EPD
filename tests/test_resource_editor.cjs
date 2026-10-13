'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), vm = require('node:vm'), path = require('node:path');
const builder = require('../html/js/resource_builder.js');
const packer = require('../html/js/ui_tools/resource_pack.js');
const project = require('../html/js/ui_tools/resource_project.js');
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};
async function fixture(options = {}) {
  const nodes = new Map(), events = new Map(), timers = new Map();
  let sequence = 0, current = null, saved = null, writes = 0;
  const node = id => {
    if (!nodes.has(id)) nodes.set(id, {
      value: id === 'resourcePreviewMode' ? '1' : '', files: [], textContent: '',
      addEventListener(type, listener) { events.set(id + ':' + type, listener); },
      classList: { toggle() {} }, width: 800, height: 160,
      getContext: () => ({ clearRect() {}, fillRect() {}, strokeRect() {}, fillText() {} })
    });
    return nodes.get(id);
  };
  const draft = {
    read: options.read || (async () => saved),
    save: async snapshot => { writes++; await options.save?.(snapshot); saved = snapshot; },
    clear: async () => { saved = null; }
  };
  const installer = {
    busy: () => false, controls() {}, getBundle: () => current,
    async loadBundle(entries) { current = packer.validateBundle(entries); context.ExternalResourceEditor.load(current); }
  };
  const document = { getElementById: node, querySelectorAll: () => [], visibilityState: 'visible',
    addEventListener(type, listener) { events.set('document:' + type, listener); } };
  const context = { Uint8Array, TextEncoder, EPDResourceBuilder: builder, EPDResourcePack: packer,
    EPDResourceProject: project, EPDResourceDraft: { create: () => draft },
    EPDWebFonts: { extendFonts: options.extendFonts }, ExternalResourceInstaller: installer, document,
    setTimeout(callback) { const id = ++sequence; timers.set(id, callback); return id; },
    clearTimeout(id) { timers.delete(id); },
    addEventListener(type, listener) { events.set('window:' + type, listener); }
  };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_editor.js'), 'utf8'), context);
  const fonts = ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn'].map(name =>
    new Uint8Array(fs.readFileSync(path.join(__dirname, '../html/resources/fonts/u8g2_font_' + name + '.bin'))));
  await installer.loadBundle(packer.create({ fonts }).packages);
  await context.ExternalResourceEditor.saveDraft();
  return { editor: context.ExternalResourceEditor, node, document, installer, events, timers,
    saved: () => saved, writes: () => writes,
    input(value) { node('resourceExtraText').value = value; events.get('resourceExtraText:input')(); } };
}

test('draft status never claims newer edits were saved by an older in-flight write', async () => {
  let gate = null;
  const f = await fixture({ save: async () => { if (gate) await gate.promise; } });
  f.input('第一版'); assert.match(f.node('resourceDraftStatus').textContent, /待保存/);
  gate = deferred(); const old = f.editor.saveDraft();
  assert.match(f.node('resourceDraftStatus').textContent, /正在保存/);
  for (let i = 0; i < 5; i++) await Promise.resolve();
  f.input('第二版'); gate.resolve(); await old;
  assert.match(f.node('resourceDraftStatus').textContent, /待保存/);
  gate = null; await f.editor.saveDraft();
  assert.equal(f.saved().values.resourceExtraText, '第二版');
  assert.match(f.node('resourceDraftStatus').textContent, /草稿已保存/);
});

test('pagehide flushes pending edits before the debounce timer and clear prevents resurrection', async () => {
  const f = await fixture();
  f.input('立即离开页面'); assert.equal(f.timers.size, 1);
  await f.events.get('window:pagehide')();
  assert.equal(f.timers.size, 0);
  assert.equal(f.saved().values.resourceExtraText, '立即离开页面');
  f.input('清除后留在编辑器'); await f.editor.clearDraft();
  const writes = f.writes(); await f.events.get('window:pagehide')();
  assert.equal(f.saved(), null); assert.equal(f.writes(), writes);
});

test('hiding the page starts a pending save and preserves a failed save indication', async () => {
  let failing = false;
  const f = await fixture({ save: async () => { if (failing) throw new Error('Quota exceeded'); } });
  failing = true; f.input('未保存'); f.document.visibilityState = 'hidden';
  f.events.get('document:visibilitychange')();
  for (let i = 0; i < 20; i++) await Promise.resolve();
  assert.match(f.node('resourceDraftStatus').textContent, /草稿未保存.*Quota exceeded/);
  failing = false; await f.events.get('window:pagehide')();
  assert.equal(f.saved().values.resourceExtraText, '未保存');
});

test('editing after restoration failure preserves the recovery message and blocks automatic writes', async () => {
  const f = await fixture({ read: async () => { throw new Error('storage unavailable'); } });
  await f.editor.restoreDraft(); const before = f.writes();
  f.input('仍需保留旧草稿'); await f.events.get('window:pagehide')();
  assert.equal(f.writes(), before); assert.equal(f.timers.size, 0);
  assert.match(f.node('resourceDraftStatus').textContent, /原草稿已保留.*自动保存已暂停/);
});

test('font compatibility failures list every affected character and retain the ready bundle', async () => {
  const issues = [
    { slot: 0, codePoint: 0x65b0, message: '字体缺字' },
    { slot: 1, codePoint: 0x5b57, message: '字形尺寸超限' }
  ];
  const f = await fixture({ extendFonts: async (fonts, requests, settings) => {
    assert.equal(settings.collectIssues, true);
    throw Object.assign(new Error('2 个字形不兼容'), { fontIssues: issues });
  } });
  const before = f.installer.getBundle();
  assert.equal(await f.editor.previewFonts(), null);
  assert.equal(f.node('resourceFontIssues').hidden, false);
  assert.match(f.node('resourceFontIssueDetails').textContent, /新.*字体缺字/);
  assert.match(f.node('resourceFontIssueDetails').textContent, /字.*尺寸超限/);
  assert.equal(f.installer.getBundle(), before);
  assert.equal(await f.editor.generate(), null); assert.equal(f.installer.getBundle(), before);
});
