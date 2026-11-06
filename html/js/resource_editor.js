(function (root) {
  'use strict';
  const builder = root.EPDResourceBuilder, packer = root.EPDResourcePack, project = root.EPDResourceProject;
  const $ = id => document.getElementById(id);
  let base = null, model = null, working = false, initialization = null, pendingInitialization = false;
  let cleanState = '', cancelled = null, draftTimer = null, draftQueue = Promise.resolve(), draftVersion = 0;
  let loadingGenerated = false, restoredBins = [];
  let draftBlocked = false, recoverySnapshot = null;
  let draftEpoch = 0, draftPending = false;
  const fontSource = root.EPDFontSources?.create(), draft = root.EPDResourceDraft?.create();
  const sourceIds = ['resourceCalendarSource', 'resourceLayoutSource', 'resourceMigrationSource', 'resourceBuildRevision',
    'resourceFontFamily', 'resourceFontPixelSize', 'resourceFontTarget', 'resourceFontMode', 'resourceExtraText'];
  function state() {
    return JSON.stringify([draftVersion, sourceIds.map(id => $(id)?.value || ''), fontSource?.get()?.family || '',
      [0, 1, 2].map(i => {
        const file = $('resourceFont' + i)?.files?.[0];
        return file ? [file.name, file.size, file.lastModified] : restoredBins[i] ? 'draft-bin' : '';
      })]);
  }
  function isDirty() { return !!base && state() !== cleanState; }
  function showDirty() {
    if ($('resourceDirtyStatus')) $('resourceDirtyStatus').textContent = isDirty()
      ? '有未生成的修改；发送前会自动生成并校验。' : '资源已就绪。';
  }
  function changed() {
    ++draftVersion; showDirty();
    if(base) {preview();root.TodoCalendar?.renderResourcePreview?.();root.CalendarAnnotations?.renderResourcePreview?.();}
    root.ExternalResourceInstaller.controls?.();
    if ($('resourceFontPreviewInfo')) $('resourceFontPreviewInfo').textContent = '编辑内容已变化，请重新预览点阵与容量。';
    if (draft && !draftBlocked) {
      draftPending = true;
      if ($('resourceDraftStatus')) $('resourceDraftStatus').textContent = '草稿待保存…';
      if (draftTimer !== null) root.clearTimeout?.(draftTimer);
      draftTimer = setTimeout(() => { draftTimer = null; saveDraft(); }, 500);
    }
  }
  function deviceBusy() {
    return root.ExternalResourceInstaller.busy() || !!root.DeviceOperations?.current;
  }
  function controls() {
    const busy = working || deviceBusy();
    document.querySelectorAll?.('[data-resource-editor-action]').forEach(node => { node.disabled = busy; });
    if ($('resourceGenerationCancel')) $('resourceGenerationCancel').disabled = !cancelled;
    if (pendingInitialization && !busy) {
      pendingInitialization = false;
      Promise.resolve().then(ensureReady);
    }
  }
  async function work(action) {
    if (working || deviceBusy()) { report('请等待当前资源生成或设备操作完成', true); return null; }
    working = true; showFontIssues(); controls(); root.ExternalResourceInstaller.controls?.();
    try { return await action(); }
    catch (error) { showFontIssues(error.fontIssues); report(error.message, true); return null; }
    finally { cancelled = null; working = false; controls(); root.ExternalResourceInstaller.controls?.(); showDirty(); }
  }
  function report(message, error = false) {
    $('resourceEditorStatus').textContent = message;
    $('resourceEditorStatus').classList.toggle('error', error);
  }
  function showFontIssues(issues = []) {
    const panel = $('resourceFontIssues'), detail = $('resourceFontIssueDetails');
    if (panel) panel.hidden = !issues.length;
    if (detail) detail.textContent = issues.map(issue => {
      const character = String.fromCodePoint(issue.codePoint);
      return `槽位 ${issue.slot} · ${character} (U+${issue.codePoint.toString(16).toUpperCase().padStart(4, '0')})：${issue.message}`;
    }).join('\n');
  }
  function load(bundle, checkedModel, repairedSlots = []) {
    // Compile the edited CAL1/UIR2 model into validated external resources.
    const validated = checkedModel ? bundle : packer.validateBundle(bundle), next = checkedModel || builder.open(validated);
    base = validated; model = next;
    $('resourceCalendarSource').value = JSON.stringify(model.calendar, null, 2);
    $('resourceLayoutSource').value = JSON.stringify(model.ui, null, 2);
    for (let i = 0; i < 3; ++i) $('resourceFont' + i).value = '';
    restoredBins = [];
    if (!loadingGenerated) {
      for (const id of ['resourceBuildRevision', 'resourceMigrationSource', 'resourceExtraText']) if ($(id)) $(id).value = '';
      if ($('resourceFontFamily')) $('resourceFontFamily').value = '';
      if ($('resourceFontPixelSize')) $('resourceFontPixelSize').value = '';
      if ($('resourceFontMode')) $('resourceFontMode').value = 'supplement';
      fontSource?.clear();
      if ($('resourceFontSourceInfo')) $('resourceFontSourceInfo').textContent = '可将字体拖到这里；文件仅在浏览器内处理。';
      $('resourceFontPreview')?.getContext('2d')?.clearRect(0, 0, 800, 160);
      if ($('resourceFontPreviewSecond')) $('resourceFontPreviewSecond').hidden = true;
      if ($('resourceFontPreviewSecondInfo')) $('resourceFontPreviewSecondInfo').hidden = true;
      if ($('resourceFontPreviewInfo')) $('resourceFontPreviewInfo').textContent = '资源已载入，请预览最终点阵与容量。';
    }
    preview(); root.TodoCalendar?.renderResourcePreview?.(); root.CalendarAnnotations?.renderResourcePreview?.();
    report('资源已载入，可编辑后生成。新增文字的缺失字模会自动生成。');
    if (repairedSlots.length) report('资源已载入，旧字模的偏移已校正，完整点阵已保留。可编辑后生成。');
    cleanState = state(); showDirty();
    if (!loadingGenerated && draft) Promise.resolve().then(saveDraft);
  }
  function editModel() {
    if (!base) throw new Error('请先生成默认资源或选择资源目录');
    return { calendar: JSON.parse($('resourceCalendarSource').value),
      ui: JSON.parse($('resourceLayoutSource').value), fonts: model.fonts };
  }
  function extraText() { return ($('resourceExtraText')?.value || '').replace(/[\r\n\t]/g, ''); }
  function fontSettings() {
    const target = $('resourceFontTarget')?.value || 'both';
    const slots = target === 'both' ? [0, 1] : [Number(target)];
    if (slots.some(slot => ![0, 1, 2].includes(slot))) throw new Error('请选择有效的字体槽位');
    const family = $('resourceFontFamily')?.value || '';
    const selected = family === 'imported' ? fontSource?.options() : family ? { fontFamily: family } : {};
    if (family === 'imported' && !selected) throw new Error('请先导入 TTF / OTF 字体');
    const pixelSize=Number($('resourceFontPixelSize')?.value)||0;
    if(pixelSize && (!Number.isInteger(pixelSize)||pixelSize<8||pixelSize>43)) throw new Error('界面字体像素字号应为 8–43');
    if(pixelSize) selected.pixelSize=pixelSize;
    const redraw = $('resourceFontMode')?.value === 'redraw' || !!pixelSize;
    if (redraw && !family) throw new Error('重绘字体前请选择系统字体或导入 TTF / OTF');
    return { slots, rebuildSlots: redraw ? slots : [], slotOptions: slot => slots.includes(slot) ? selected : {} };
  }
  async function previewFonts() {
    return work(async () => {
      const next = editModel(), selectedBase = base, settings = fontSettings();
      builder.encodeCalendar(next.calendar); builder.encodeUI(next.ui);
      const migrationText = $('resourceMigrationSource')?.value.trim();
      const requests = packer.dependencies(base, next, migrationText ? JSON.parse(migrationText) : undefined).requests;
      settings.slots.forEach(slot => requests.push({ slot, text: extraText() }));
      const input = await Promise.all(next.fonts.map(async (font, i) => {
        const file = $('resourceFont' + i)?.files?.[0];
        if (file?.size > 262144) throw new Error('字模 BIN 超过字体分区容量');
        return file ? new Uint8Array(await file.arrayBuffer()) : restoredBins[i] || font;
      }));
      if (next.fonts.length === 4) input.customKey = next.fonts.customKey;
      cancelled = { aborted: false }; controls();
      const result = await root.EPDWebFonts.extendFonts(input, requests, { signal: cancelled, collectIssues: true,
        rebuildSlots: settings.rebuildSlots, slotOptions: settings.slotOptions,
        maxPayloadBytes: base.partitions.find(part => part.id === 7).size - 32,
        onProgress: ({ completed, total }) => report(`正在准备点阵预览 ${completed}/${total}…`) });
      if (base !== selectedBase || deviceBusy() || cancelled.aborted) throw new Error('预览已取消或资源发生变化');
      const payload = builder.encodeFonts(result.fonts), partition = base.partitions.find(part => part.id === 7);
      if (payload.length + 32 > partition.size) throw new Error('生成的字模超过字体分区容量');
      const messages = settings.slots.map((slot, index) => drawFontPreview(result.fonts[slot], slot,
        index ? 'resourceFontPreviewSecond' : 'resourceFontPreview', payload.length, partition.size - 32));
      if ($('resourceFontPreviewSecond')) $('resourceFontPreviewSecond').hidden = settings.slots.length < 2;
      if ($('resourceFontPreviewSecondInfo')) {
        $('resourceFontPreviewSecondInfo').hidden = settings.slots.length < 2;
        $('resourceFontPreviewSecondInfo').textContent = messages[1] || '';
      }
      $('resourceFontPreviewInfo').textContent = messages[0]; report('字体检查通过，点阵预览已完成。确认后点击重新生成并载入。');
      return result;
    });
  }
  function drawFontPreview(font, slot, id, payloadBytes, budget) {
      const text = Array.from(extraText() || (slot === 2 ? '0123456789-' : '年月日0123')).slice(0, 24);
      const canvas = $(id), ctx = canvas.getContext('2d');
      const glyphs = root.EPDWebFonts.glyphs(font, slot);
      ctx.clearRect(0, 0, canvas.width, canvas.height); ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
      const scale = 4, baseline = 112; let x = 12, maximum = [0, 0];
      ctx.strokeStyle = '#94a3b8'; ctx.beginPath(); ctx.moveTo(0, baseline); ctx.lineTo(canvas.width, baseline); ctx.stroke();
      for (const char of text) {
        const glyph = glyphs.get(char.codePointAt(0));
        if (!glyph) continue;
        maximum = [Math.max(maximum[0], glyph.width), Math.max(maximum[1], glyph.height)];
        if (x + (glyph.x + glyph.width) * scale > canvas.width) break;
        ctx.fillStyle = '#17212b';
        for (let row = 0; row < glyph.height; row++) for (let col = 0; col < glyph.width; col++)
          if (glyph.pixels[row * glyph.width + col]) ctx.fillRect(x + (glyph.x + col) * scale,
            baseline - (glyph.y + glyph.height - row) * scale, scale, scale);
        x += Math.max(1, glyph.advance) * scale + 8;
      }
      return `槽位 ${slot} · 4 倍点阵 · 样本最大 ${maximum[0]}×${maximum[1]} 像素 · 字体载荷 ${payloadBytes} / ${budget} 字节`;
  }
  async function importFont(file) {
    return work(async () => {
      if (!file) return;
      if (!fontSource) throw new Error('字体导入模块未加载');
      const selectedBase = base, prepared = await fontSource.prepare(file);
      if (base !== selectedBase || deviceBusy()) throw new Error('资源或设备任务已变化，请重新导入字体');
      const imported = fontSource.commit(prepared);
      $('resourceFontFamily').value = 'imported';
      $('resourceFontSourceInfo').textContent = `${imported.name} · ${imported.bytes.length} 字节，已在本地加载`;
      changed(); await saveDraft(); report('字体已导入。选择补字或重绘，预览点阵后生成资源。');
      return imported;
    });
  }
  async function captureSnapshot() {
      if (!base) throw new Error('请先生成默认资源或选择资源工程');
      const snapshot = { version: 1, savedAt: Date.now(), files: base.packages.map(item => ({ name: item.name, bytes: item.bytes.slice() })),
        values: Object.fromEntries(sourceIds.map(id => [id, $(id)?.value || ''])), bins: [] };
      const source = fontSource?.get();
      if (source) snapshot.font = { name: source.name, bytes: source.bytes };
      snapshot.bins = await Promise.all([0, 1, 2].map(async i => {
        const file = $('resourceFont' + i)?.files?.[0];
        if (file?.size > 262144) throw new Error('字模 BIN 过大，无法保存草稿');
        return file ? new Uint8Array(await file.arrayBuffer()) : restoredBins[i] || null;
      }));
      return snapshot;
  }
  async function saveDraft() {
    if (!draft || !base || draftBlocked) return;
    if (draftTimer !== null) { root.clearTimeout?.(draftTimer); draftTimer = null; }
    draftPending = true;
    const version = draftVersion, epoch = draftEpoch, selectedBase = base;
    const current = () => version === draftVersion && epoch === draftEpoch && base === selectedBase && !draftBlocked;
    if ($('resourceDraftStatus')) $('resourceDraftStatus').textContent = '正在保存草稿…';
    try {
      const snapshot = await captureSnapshot();
      if (!current()) return;
      draftQueue = draftQueue.catch(() => {}).then(() => epoch === draftEpoch && !draftBlocked ? draft.save(snapshot) : undefined); await draftQueue;
      if (!current()) return;
      draftPending = false;
      const backup = draft.status?.().backend === 'localStorage' ? '备用存储' : '此浏览器';
      if ($('resourceDraftStatus')) $('resourceDraftStatus').textContent = `草稿已保存在${backup}，包含导入的字体；刷新后自动恢复。`;
      return snapshot;
    } catch (error) {
      if (current() && $('resourceDraftStatus')) $('resourceDraftStatus').textContent = `草稿未保存：${error.message}。请导出完整工程。`;
    }
  }
  function flushDraft() {
    // Browsers may terminate a page before async storage completes. Flush on
    // hiding as well as pagehide, while keeping an explicit saved-state indicator.
    if (draftPending && !draftBlocked) return saveDraft();
  }
  async function restoreDraft() {
    if (!draft) return null;
    return work(async () => {
      const selectedBase = base, version = root.ExternalResourceInstaller.selectionVersion?.();
      if (draftTimer !== null) { root.clearTimeout?.(draftTimer); draftTimer = null; }
      ++draftEpoch;
      try {
        await draftQueue.catch(() => {});
        const snapshot = await draft.read();
        if (base !== selectedBase || version !== root.ExternalResourceInstaller.selectionVersion?.() || deviceBusy()) throw new Error('当前资源已变化，未恢复旧草稿');
        if (!snapshot) { draftBlocked = false; draftPending = false; recoverySnapshot = null; return null; }
        recoverySnapshot = snapshot;
        const bundle = await applySnapshot(snapshot, '本地资源草稿');
        draftBlocked = false; draftPending = false; recoverySnapshot = null;
        if ($('resourceDraftRecoveryExport')) $('resourceDraftRecoveryExport').hidden = true;
        $('resourceDraftStatus').textContent = '本地草稿已恢复；发送前会重新生成并校验。';
        report('已恢复本地草稿，包含编辑内容和导入字体。发送前会重新生成并校验。');
        return bundle;
      } catch (error) {
        draftBlocked = true;
        $('resourceDraftStatus').textContent = `恢复失败：${error.message}。原草稿已保留，自动保存已暂停；可重试恢复或导出保留的草稿。清除草稿后可重新保存。`;
        if ($('resourceDraftRecoveryExport')) $('resourceDraftRecoveryExport').hidden = !recoverySnapshot;
        throw error;
      }
    });
  }
  async function applySnapshot(snapshot, label, preparedBundle) {
    if (snapshot.version !== 1 || !Array.isArray(snapshot.files)) throw new Error('草稿格式不受支持');
    const values = project.values(snapshot.values), bundle = preparedBundle || packer.repairFontBounds(snapshot.files);
    if (!Array.isArray(snapshot.bins) || snapshot.bins.length > 3 || snapshot.bins.some(bytes => bytes !== null &&
        (!(bytes instanceof Uint8Array) || bytes.length > 262144))) throw new Error('草稿字模 BIN 无效');
    if (values.resourceFontFamily === 'imported' && !snapshot.font) throw new Error('草稿缺少所选的 TTF / OTF 字体');
    const selectedBase = base, version = root.ExternalResourceInstaller.selectionVersion?.();
    if (snapshot.font && !fontSource) throw new Error('字体导入模块未加载');
    const preparedFont = snapshot.font ? await fontSource.prepare(snapshot.font) : null;
    if (base !== selectedBase || version !== root.ExternalResourceInstaller.selectionVersion?.() || deviceBusy()) throw new Error('当前资源已变化，未恢复旧草稿');
    loadingGenerated = true;
    try { await root.ExternalResourceInstaller.loadBundle(bundle.packages, label); }
    finally { loadingGenerated = false; }
    if (preparedFont) fontSource.commit(preparedFont); else fontSource?.clear();
    for (const id of sourceIds) if ($(id)) $(id).value = values[id];
    restoredBins = snapshot.bins;
    cleanState = ''; showDirty(); preview();
    if ($('resourceFontSourceInfo')) $('resourceFontSourceInfo').textContent = snapshot.font
      ? `已恢复 ${snapshot.font.name} · ${snapshot.font.bytes.length} 字节` : '可将字体拖到这里；文件仅在浏览器内处理。';
    return bundle;
  }
  async function clearDraft() {
    if (working || deviceBusy() || !draft) return;
    ++draftEpoch;
    draftPending = false;
    if (draftTimer !== null) { root.clearTimeout?.(draftTimer); draftTimer = null; }
    try {
      await draftQueue.catch(() => {}); await draft.clear(); draftBlocked = false; recoverySnapshot = null;
      if ($('resourceDraftRecoveryExport')) $('resourceDraftRecoveryExport').hidden = true;
      $('resourceDraftStatus').textContent = '本地草稿已清除，当前编辑内容仍保留。';
    }
    catch (error) { report(error.message, true); }
  }
  function preview() {
    try {
      const next = editModel();
      builder.encodeUI(next.ui);
      const mode = Number($('resourcePreviewMode').value);
      root.EPDUIRenderer.draw($('resourceLayoutPreview'),next.ui,next.calendar,next.fonts,
        previewRuntime(mode),root.TodoCalendar||{},root.CalendarAnnotations?.annotations||[]);
      return true;
    } catch (error) { report(error.message, true); return false; }
  }
  function previewRuntime(mode) {
    return {mode,timestamp:root.TodoCalendar?.previewNow?.() ?? Math.floor(Date.now()/1000),
      temperature:root.TodoCalendar?._temperature ?? 25,voltage_mv:3100,ssid:root.bleDevice?.name||'EPD-1234',
      week_start:Number($('weekStart')?.value)||0};
  }
  function previewModel() { return base ? editModel() : null; }
  async function generate() {
    return work(async () => {
      const next = editModel(), selectedBase = base;
      const revisionText = $('resourceBuildRevision')?.value.trim();
      const options = {};
      if (revisionText) options.revision = Number(revisionText);
      const migrationText = $('resourceMigrationSource')?.value.trim();
      if (migrationText) {
        options.migration = JSON.parse(migrationText);
        builder.decodeMigration(options.migration);
      }
      builder.encodeCalendar(next.calendar); builder.encodeUI(next.ui);
      report('正在生成资源并检查字模…');
      const fonts = await Promise.all(next.fonts.map(async (font, i) => {
        const file = $('resourceFont' + i)?.files?.[0];
        if (file?.size > 262144) throw new Error('字模 BIN 超过字体分区容量');
        return file ? new Uint8Array(await file.arrayBuffer()) : restoredBins[i] || font;
      }));
      if (next.fonts.length === 4) fonts.customKey = next.fonts.customKey;
      if (base !== selectedBase) throw new Error('资源目录已变化，请重新生成');
      const requests = packer.dependencies(selectedBase, next, options.migration).requests;
      const settings = fontSettings();
      settings.slots.forEach(slot => requests.push({ slot, text: extraText() }));
      cancelled = { aborted: false }; controls();
      const extended = await root.EPDWebFonts.extendFonts(fonts, requests, {
        signal: cancelled, collectIssues: true, rebuildSlots: settings.rebuildSlots, slotOptions: settings.slotOptions,
        maxPayloadBytes: base.partitions.find(part => part.id === 7).size - 32,
        onProgress: ({ completed, total }) => {
          const progress = $('resourceGenerationProgress');
          if (progress) { progress.max = Math.max(1, total); progress.value = completed; }
          report(total ? `正在生成字模 ${completed}/${total}…` : '字模已齐全，正在封装资源…');
        }
      });
      if (cancelled.aborted) throw new Error('已取消生成，保留之前的资源');
      if (base !== selectedBase || deviceBusy()) throw new Error('资源或设备任务已变化，请重新生成');
      next.fonts = extended.fonts;
      const result = packer.build(selectedBase, next, options);
      cancelled = null; controls();
      loadingGenerated = true;
      try { await root.ExternalResourceInstaller.loadBundle(result.packages, '网页生成的资源'); }
      finally { loadingGenerated = false; }
      if ($('resourceFontMode')) $('resourceFontMode').value = 'supplement';
      cleanState = state(); showDirty(); await saveDraft();
      if ($('resourceGenerationProgress')) $('resourceGenerationProgress').value = $('resourceGenerationProgress').max;
      report(`已生成并自动载入 ${result.packages.length} 个资源包，共 ${result.total} 字节，可发送到设备或下载。` +
        (extended.added.length ? ` 已补充 ${extended.added.length} 个字模。` : ''));
      return result;
    });
  }
  function save(name, data, type) {
    const url = URL.createObjectURL(new Blob([data], { type })), a = document.createElement('a');
    a.href = url; a.download = name; a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  async function download() {
    const result = await generate();
    if (!result) return;
    const item = result.packages.find(p => p.pack.id === Number($('resourceDownloadPart').value));
    if (!item) { report('当前工程不包含所选资源包', true); return; }
    save(item.name, item.bytes, 'application/octet-stream');
  }
  async function exportProject(recovery = false) {
    return work(async () => {
      const selectedBase = base, version = draftVersion;
      const snapshot = recovery || !base ? recoverySnapshot : await captureSnapshot();
      if (!snapshot) throw new Error('没有可导出的资源工程；请先生成资源或重试恢复草稿');
      if (base !== selectedBase || version !== draftVersion) throw new Error('编辑内容已变化，请重新导出');
      save('epd-resources.json', JSON.stringify(project.encode(snapshot)), 'application/json');
      report(recovery ? '已导出保留的原草稿，包含原字体和编辑内容。' : '已导出完整工程，包含当前编辑内容、导入字体和未生成的 BIN；导入后可继续编辑。');
    });
  }
  async function importProject(data, label) {
    const prepared = project.decode(data);
    if (prepared.snapshot) await applySnapshot(prepared.snapshot, label, prepared.bundle);
    else await root.ExternalResourceInstaller.loadBundle(prepared.bundle.packages, label);
    await saveDraft();
    report('完整工程已导入，可继续编辑；发送前会生成并校验。');
    return prepared.bundle;
  }
  async function loadDefault(onlyIfEmpty = false) {
    if (onlyIfEmpty && root.ExternalResourceInstaller.getBundle()) return;
    return work(async () => {
      const selectedBase = base;
      const version = root.ExternalResourceInstaller.selectionVersion?.();
      report('正在生成默认资源…');
      const result = await packer.generateDefault({ fetch: root.fetch });
      if (base !== selectedBase || version !== root.ExternalResourceInstaller.selectionVersion?.() || deviceBusy())
        throw new Error('资源或设备任务已变化，请重新生成默认资源');
      await root.ExternalResourceInstaller.loadBundle(result.packages, '浏览器生成的默认资源（2025–2027）');
      if ($('resourceBuildRevision')) $('resourceBuildRevision').value = '';
      if ($('resourceMigrationSource')) $('resourceMigrationSource').value = '';
      cleanState = state(); showDirty(); await saveDraft();
      report(`已生成并自动载入 ${result.packages.length} 个默认资源包，可直接发送到设备。`);
      return result;
    });
  }
  function ensureReady() {
    if (root.ExternalResourceInstaller.getBundle()) return Promise.resolve();
    if (initialization) return initialization;
    if (working || deviceBusy()) { pendingInitialization = true; return Promise.resolve(); }
    if (!initialization) initialization = (async () => {
      const restored = await restoreDraft();
      if (!restored && !draftBlocked && !root.ExternalResourceInstaller.getBundle()) await loadDefault(true);
    })().finally(() => { initialization = null; });
    return initialization;
  }
  async function downloadAll() {
    const result = await generate();
    if (!result) return;
    try {
      const generatedAt = new Date().toISOString();
      const manifest = { format: 'EPD-RESOURCE-BUNDLE-1', generatedAt, flashCapacity: 524288,
        resources: result.packages.map(item => ({ id: item.pack.id, name: item.name, revision: item.pack.revision,
          payloadBytes: item.pack.payload.length, crc32: item.pack.crc, builtAt: item.pack.builtAt })) };
      const entries = packer.exportFiles(result);
      const encoder = new TextEncoder();
      const edited = builder.open(result);
      for (const [name, data] of [['manifest.json', manifest], ['calendar-data.json', edited.calendar], ['ui-layout.json', edited.ui]])
        entries.push({ name, bytes: encoder.encode(JSON.stringify(data, null, 2) + '\n') });
      entries.push({ name: 'epd-resources.json', bytes: encoder.encode(JSON.stringify(project.encode(await captureSnapshot()))) });
      save('epd-external-resources.zip', root.EPDResourceArchive.createZip(entries), 'application/zip');
      report(`已下载全部 ${result.packages.length} 个 ERS 资源包及完整工程。解压后导入 epd-resources.json 可恢复字体与编辑设置。`);
    } catch (error) { report(error.message, true); }
  }
  $('resourceProjectImport').addEventListener('change', async event => {
    await work(async () => {
      const file = event.target.files[0], selectedBase = base;
      const version = root.ExternalResourceInstaller.selectionVersion?.();
      if (!file) return;
      if (file.size > project.MAX_PROJECT) throw new Error('工程文件超过 64 MiB');
      const data = JSON.parse(await file.text());
      if (base !== selectedBase || version !== root.ExternalResourceInstaller.selectionVersion?.() || deviceBusy())
        throw new Error('资源或设备任务已变化，请重新导入');
      await importProject(data, file.name);
    });
  });
  $('resourceCalendarImport')?.addEventListener('change', async event => {
    await work(async () => {
      const file = event.target.files[0], selectedBase = base;
      if (!file) return;
      if (file.size > 1500000) throw new Error('日历数据文件过大');
      const data = JSON.parse(await file.text());
      if (base !== selectedBase || deviceBusy()) throw new Error('资源或设备任务已变化，请重新导入');
      const calendar = builder.importCalendarData(editModel().calendar, data);
      $('resourceCalendarSource').value = JSON.stringify(calendar, null, 2);
      changed(); await saveDraft();
      report('日历数据已导入，点击重新生成并载入后发送到设备。');
    });
  });
  $('resourceMigrationImport')?.addEventListener('change', async event => {
    await work(async () => {
      const file = event.target.files[0], selectedBase = base;
      if (!file) return;
      if (file.size > 16000) throw new Error('迁移备份文件过大');
      const data = JSON.parse(await file.text());
      builder.decodeMigration(data);
      if (base !== selectedBase || deviceBusy()) throw new Error('资源或设备任务已变化，请重新导入');
      $('resourceMigrationSource').value = JSON.stringify(data, null, 2);
      changed(); await saveDraft();
      report('迁移备份已导入，重新生成后将打包配置与日期标注。');
    });
  });
  for (const id of sourceIds) {
    $(id)?.addEventListener('input', changed);
    $(id)?.addEventListener('change', changed);
  }
  for (let i = 0; i < 3; i++) $('resourceFont' + i)?.addEventListener('change', changed);
  $('resourceFontImport')?.addEventListener('change', event => importFont(event.target.files[0]));
  $('resourceExtraImport')?.addEventListener('change', event => work(async () => {
    const file = event.target.files[0]; if (!file) return;
    if (file.size > 65536) throw new Error('补字 TXT 超过 64 KiB');
    const selectedBase = base, text = await file.text();
    if (base !== selectedBase || deviceBusy()) throw new Error('资源或设备任务已变化，请重新导入');
    $('resourceExtraText').value = text; changed(); await saveDraft(); report('补字文本已导入，可预览并生成。');
  }));
  const drop = $('resourceFontDrop');
  drop?.addEventListener('dragover', event => { event.preventDefault(); });
  drop?.addEventListener('drop', event => { event.preventDefault(); if (!working && !deviceBusy()) importFont(event.dataTransfer.files[0]); });
  document.addEventListener?.('visibilitychange', () => { if (document.visibilityState === 'hidden') flushDraft(); });
  root.addEventListener?.('pagehide', flushDraft);
  root.ExternalResourceEditor = { load, loadDefault, ensureReady, preview, generate, download, downloadAll, exportProject,
    previewFonts, previewModel, previewRuntime, importFont, saveDraft, restoreDraft, clearDraft, isDirty,
    cancel: () => { if (cancelled) cancelled.aborted = true; }, controls, busy: () => working };
  const bundle = root.ExternalResourceInstaller.getBundle();
  if (bundle) {
    try { load(bundle); } catch (error) { report(error.message, true); }
  }
  controls();
})(globalThis);
