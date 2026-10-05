(function (root) {
  'use strict';
  const node = typeof module !== 'undefined' && module.exports;
  const store = node ? require('./resource_store.js') : root.EPDResourceStore;
  const builder = node ? require('./resource_builder.js') : root.EPDResourceBuilder;
  const fonts = node ? require('./font_builder.js') : root.EPDWebFonts;
  const PARTITION = 7;
  const noop = () => {};

  function updatedPackage(original, nextFonts, partitions) {
    if (original.revision === 0xffffffff) throw new Error('字体资源版本已用尽，请先重新安装资源');
    const bytes = builder.wrap(PARTITION, builder.encodeFonts(nextFonts), original.revision + 1);
    store.validateTarget(store.parsePackage(bytes), partitions);
    return bytes;
  }

  // Reuse the resource transaction, including its durable CRC commit and reload.
  // A caller supplies its connection/operation guard; no device data is cached.
  async function ensure(client, requests, options = {}) {
    const check = options.assertCurrent || noop, progress = options.progress || noop;
    const pending = requests.filter(request => request.text);
    if (!pending.length) return { changed: false, added: [] };
    check(); progress('正在检查设备字库…');
    const status = store.parseStatus(await client.status()); check();
    if (status.code !== 0) throw new Error('请先安装有效的设备资源，再保存含文字的标注');
    if (status.active || status.entered) throw new Error('设备正在更新资源，请完成当前更新后重试');
    const partitions = await client.partitions(); check();
    const partition = partitions.find(item => item.id === PARTITION);
    if (!partition) throw new Error('设备未安装字体分区，请先安装设备资源');
    let original;
    try { original = store.parsePackage(await client.readPackage(partition)); }
    catch (error) { throw new Error(`设备字库读取失败，请先修复字体资源：${error.message}`); }
    check();
    const currentFonts = builder.decodeFonts(original.payload);
    const missing = fonts.gatherMissing(currentFonts, pending);
    if (!missing.length) return { changed: false, added: [] };
    if (!status.hotReload) throw new Error('当前固件不支持字体即时加载，请升级固件后重试');
    progress(`正在生成 ${missing.length} 个缺失字模…`);
    const extended = await fonts.extendFonts(currentFonts, pending, options); check();
    const bytes = updatedPackage(original, extended.fonts, partitions);
    const next = store.parsePackage(bytes);
    // Rendering may wait for browser fonts. Verify the source is still current
    // before the first destructive Flash write.
    const header = store.parseHeader(await client.readRange(partition.offset, 32), partition); check();
    if (header.revision !== original.revision || header.crc !== original.crc || header.length !== original.payload.length)
      throw new Error('生成期间设备字库已变化，请重新保存');
    let entered = false;
    try {
      progress('正在保存新增字模到外置 Flash…');
      if (options.onWrite) options.onWrite();
      entered = true;
      await client.upload(bytes, (done, total) => {
        check(); progress(`正在保存字模 ${Math.round(done * 100 / total)}%…`);
      }, partitions); check();
      if (status.verify) {
        const verified = await client.inspect(PARTITION); check();
        if (!verified || verified.code !== 0 || verified.revision !== next.revision ||
            verified.length !== next.payload.length || verified.crc !== next.crc)
          throw new Error('设备未确认新增字模校验通过，请重试');
      }
      progress('字模已提交，正在加载…');
      await client.leave(); check();
      entered = false;
      return { changed: true, added: extended.added };
    } catch (error) {
      if (entered && !client.closed) {
        try { check(); await client.request(5); await client.leave(); }
        catch (_) { /* Keep the original failure; never continue saving text. */ }
      }
      throw error;
    }
  }

  // A regular fonts.ers update keeps device-only characters. Incoming glyphs
  // win when both versions provide the same code point.
  function preserve(incomingBytes, installedBytes, partitions) {
    const incoming = store.parsePackage(incomingBytes), installed = store.parsePackage(installedBytes);
    if (incoming.id !== PARTITION || installed.id !== PARTITION) throw new Error('不是字体资源包');
    const incomingFonts = builder.decodeFonts(incoming.payload);
    let installedFonts;
    try { installedFonts = builder.decodeFonts(installed.payload); }
    catch (_) { return { bytes: incomingBytes, added: [], repair: true }; }
    const result = fonts.mergeFonts(incomingFonts, installedFonts);
    if (!result.added.length) return { bytes: incomingBytes, added: [] };
    const original = { ...incoming, revision: Math.max(incoming.revision, installed.revision) };
    return { bytes: updatedPackage(original, result.fonts, partitions), added: result.added };
  }

  const api = { ensure, preserve };
  if (node) module.exports = api;
  else root.EPDFontSync = api;
})(globalThis);
