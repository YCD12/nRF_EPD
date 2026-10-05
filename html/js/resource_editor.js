(function (root) {
  'use strict';
  const builder = root.EPDResourceBuilder, store = root.EPDResourceStore;
  const $ = id => document.getElementById(id);
  let base = null, model = null;
  function report(message, error = false) {
    $('resourceEditorStatus').textContent = message;
    $('resourceEditorStatus').classList.toggle('error', error);
  }
  function load(bundle) {
    // This editor changes fixed CAL1/UIR1 records; it is not a device-side JSON engine.
    const next = builder.open(bundle);
    base = bundle; model = next;
    $('resourceCalendarSource').value = JSON.stringify(model.calendar, null, 2);
    $('resourceLayoutSource').value = JSON.stringify(model.ui, null, 2);
    for (let i = 0; i < 3; ++i) $('resourceFont' + i).value = '';
    preview();
    report('资源已载入，可编辑后生成。新增文字的缺失字模会自动生成。');
  }
  function editModel() {
    if (!base) throw new Error('请先加载默认模板或选择资源目录');
    return { calendar: JSON.parse($('resourceCalendarSource').value),
      ui: JSON.parse($('resourceLayoutSource').value), fonts: model.fonts };
  }
  function preview() {
    try {
      const next = editModel();
      builder.encodeUI(next.ui);
      const canvas = $('resourceLayoutPreview'), ctx = canvas.getContext('2d');
      ctx.clearRect(0, 0, 800, 480); ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, 800, 480);
      const mode = Number($('resourcePreviewMode').value);
      ctx.font = '14px sans-serif';
      next.ui.components.filter(c => c.mode === mode).forEach(c => {
        ctx.strokeStyle = c.kind === 4 ? '#b42318' : '#6b7280';
        ctx.fillStyle = c.kind === 4 ? '#fff1f0' : '#eef3f7';
        ctx.fillRect(c.x, c.y, c.width, c.height); ctx.strokeRect(c.x, c.y, c.width, c.height);
        ctx.fillStyle = '#182431';
        if (c.height > 15) ctx.fillText(`${c.id} · ${['', '文字组', '分隔线', '月历', '时钟'][c.kind]}`, c.x + 3, c.y + 15);
      });
      return true;
    } catch (error) { report(error.message, true); return false; }
  }
  async function generate() {
    try {
      if (root.ExternalResourceInstaller.busy()) throw new Error('请等待当前设备操作完成');
      const next = editModel(), selectedBase = base;
      const fonts = await Promise.all(next.fonts.map(async (font, i) => {
        const file = $('resourceFont' + i).files[0];
        return file ? new Uint8Array(await file.arrayBuffer()) : font;
      }));
      if (base !== selectedBase) throw new Error('资源目录已变化，请重新生成');
      builder.encodeCalendar(next.calendar); builder.encodeUI(next.ui);
      let text = '';
      function checkText(value) {
        if (typeof value === 'string') {
          text += value;
        } else if (Array.isArray(value)) value.forEach(checkText);
        else if (value && typeof value === 'object') Object.values(value).forEach(checkText);
      }
      checkText(next.calendar);
      const extended = await root.EPDWebFonts.extendFonts(fonts, [{ slot: 0, text }, { slot: 1, text }]);
      if (base !== selectedBase || root.ExternalResourceInstaller.busy()) throw new Error('资源或设备任务已变化，请重新生成');
      next.fonts = extended.fonts;
      const result = builder.build(selectedBase, next);
      await root.ExternalResourceInstaller.loadBundle(result.packages, '网页生成的资源');
      report(`已生成 ${result.total} 字节，可一键烧录或下载。` +
        (extended.added.length ? ` 已补充 ${extended.added.length} 个字模。` : ''));
      return result;
    } catch (error) { report(error.message, true); return null; }
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
    save(item.name, item.bytes, 'application/octet-stream');
  }
  function bytes64(bytes) {
    // Avoid argument limits for large font packages.
    let binary = '';
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return btoa(binary);
  }
  async function exportProject() {
    const result = await generate();
    if (!result) return;
    const project = { format: 'EPD-RESOURCE-PROJECT-1', files: result.packages.map(p => ({ name: p.name, base64: bytes64(p.bytes) })) };
    save('epd-resources.json', JSON.stringify(project, null, 2), 'application/json');
  }
  async function importProject(project, label) {
    if (project.format !== 'EPD-RESOURCE-PROJECT-1' || !Array.isArray(project.files) || project.files.length !== 6)
      throw new Error('不是完整的设备资源工程');
    const entries = project.files.map(file => {
      if (typeof file.name !== 'string' || /[\\/]/.test(file.name) || typeof file.base64 !== 'string' || file.base64.length > 700000)
        throw new Error('工程文件名或长度无效');
      return { name: file.name, bytes: Uint8Array.from(atob(file.base64), c => c.charCodeAt(0)) };
    });
    const bundle = store.prepareBundle(entries);
    builder.open(bundle); // Reject semantic damage before replacing the current project.
    await root.ExternalResourceInstaller.loadBundle(entries, label);
  }
  async function loadDefault() {
    try {
      const response = await fetch('resources/default-project.json', { cache: 'no-store' });
      if (!response.ok) throw new Error('无法读取默认模板，请通过本地网页服务打开页面或选择资源目录');
      await importProject(await response.json(), '默认模板（2025–2027）');
    } catch (error) { report(error.message, true); }
  }
  $('resourceProjectImport').addEventListener('change', async event => {
    try {
      const file = event.target.files[0];
      if (!file) return;
      if (file.size > 1500000) throw new Error('工程文件过大');
      await importProject(JSON.parse(await file.text()), file.name);
    } catch (error) { report(error.message, true); }
  });
  root.ExternalResourceEditor = { load, loadDefault, preview, generate, download, exportProject };
  const bundle = root.ExternalResourceInstaller.getBundle();
  if (bundle) {
    try { load(bundle); } catch (error) { report(error.message, true); }
  }
})(globalThis);
