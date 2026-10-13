(function (global) {
  'use strict';

  const THEME_STORAGE_KEY = 'epd_console_theme';
  let initialized = false;
  let themeInitialized = false;
  let connectionState = {};
  let navigationInitialized = false;

  function getDocument() {
    return global.document || (typeof document !== 'undefined' ? document : null);
  }

  function byId(id) {
    const doc = getDocument();
    return doc && doc.getElementById ? doc.getElementById(id) : null;
  }

  function setClass(element, name, enabled) {
    if (!element || !element.classList) return;
    const method = enabled ? 'add' : 'remove';
    if (typeof element.classList[method] === 'function') element.classList[method](name);
  }

  function setText(id, text) {
    const element = byId(id);
    if (element) element.textContent = text;
  }

  function notify(message, isError = false) {
    if (typeof global.showToast === 'function') global.showToast(message, isError ? 4200 : 2800);
    if (isError && typeof global.addLog === 'function') global.addLog(message);
  }

  function supportsBluetooth() {
    return !!(global.navigator && global.navigator.bluetooth && global.isSecureContext !== false);
  }

  function updateConnection(state) {
    connectionState = Object.assign({}, connectionState, state || {});
    const supported = supportsBluetooth();
    const connected = !!connectionState.connected;
    const connecting = !!connectionState.connecting;
    const busy = !!connectionState.busy;
    let label = '未连接设备';
    let detail = '连接墨水屏后，即可同步时间、传送图片和管理资源。';
    let status = 'disconnected';
    const operationNames = {
      connecting: '正在初始化设备',
      image: '正在传送图片',
      mode: '正在切换显示模式',
      control: '正在执行设备操作',
      export: '正在导出设备备份',
      resources: '正在更新设备资源',
      resource: '正在更新设备资源',
      annotations: '正在更新日期标注',
    };
    if (connecting) {
      label = '正在连接';
      detail = '请在浏览器中选择设备，连接完成后可开始操作。';
      status = 'connecting';
    } else if (connected && busy) {
      label = '设备操作中';
      detail = operationNames[connectionState.operation] || '设备正在处理操作，请等待完成。';
      status = 'busy';
    } else if (connected) {
      label = '设备已连接';
      detail = connectionState.deviceName || '墨水屏已就绪，可以开始操作。';
      status = 'connected';
    } else if (!supported) {
      label = '蓝牙暂不可用';
      detail = global.isSecureContext === false
        ? '请通过 HTTPS 或本机地址打开页面，以使用浏览器蓝牙。'
        : '建议使用支持 Web Bluetooth 的 Chrome 或 Edge 浏览器；图片编辑仍可使用。';
      status = 'unsupported';
    }
    const badge = byId('connectionBadge');
    if (badge) {
      if (badge.setAttribute) badge.setAttribute('data-state', status);
      for (const name of ['connected', 'connecting', 'busy', 'unsupported']) {
        setClass(badge, 'is-' + name, status === name);
      }
    }
    setText('connectionLabel', label);
    setText('connectionDetail', detail);
  }

  function initTheme() {
    if (themeInitialized) return;
    const doc = getDocument();
    if (!doc || !doc.body) return;
    themeInitialized = true;
    let preference = null;
    try { preference = global.localStorage.getItem(THEME_STORAGE_KEY); } catch (_) { }
    const systemDark = typeof global.matchMedia === 'function' && global.matchMedia('(prefers-color-scheme: dark)').matches;
    let dark = preference === 'dark' || (preference !== 'light' && systemDark);
    const button = byId('themeToggle');
    const applyTheme = () => {
      setClass(doc.body, 'theme-dark', dark);
      if (!button) return;
      const label = dark ? '切换到浅色主题' : '切换到深色主题';
      button.title = label;
      if (button.setAttribute) {
        button.setAttribute('aria-label', label);
        button.setAttribute('aria-pressed', String(dark));
      }
      const text = button.querySelector ? button.querySelector('[data-theme-label]') : null;
      if (text) text.textContent = dark ? '浅色主题' : '深色主题';
      else if (!button.children || !button.children.length) button.textContent = dark ? '浅色主题' : '深色主题';
    };
    applyTheme();
    if (button && button.addEventListener) button.addEventListener('click', () => {
      dark = !dark;
      applyTheme();
      try { global.localStorage.setItem(THEME_STORAGE_KEY, dark ? 'dark' : 'light'); } catch (_) { }
    });
  }

  function initNavigation() {
    if (navigationInitialized) return;
    const nav = byId('workspaceNav');
    if (!nav || !nav.querySelectorAll) return;
    const items = Array.from(nav.querySelectorAll('a[href^="#"]')).map(link => {
      let id;
      try { id = decodeURIComponent(link.getAttribute('href').slice(1)); } catch (_) { return null; }
      const section = byId(id);
      const panel = section && section.closest('[data-workspace-panel]');
      return panel ? { link, section, panel, id } : null;
    }).filter(Boolean);
    if (!items.length) return;
    navigationInitialized = true;
    const viewport = byId('workspaceViewport');
    const scrollPositions = new Map();
    let activeId = null;
    const activate = (id, focus = false) => {
      const current = items.find(item => item.id === id) || items[0];
      const changed = activeId !== current.id;
      if (changed && viewport && activeId) scrollPositions.set(activeId, viewport.scrollTop);
      // Keep every panel mounted: Bluetooth, transfers and editor state stay alive.
      for (const item of items) {
        const selected = item === current;
        item.panel.hidden = !selected;
        setClass(item.link, 'active', selected);
        if (item.link.setAttribute && selected) item.link.setAttribute('aria-current', 'page');
        else if (item.link.removeAttribute) item.link.removeAttribute('aria-current');
      }
      activeId = current.id;
      if (viewport && changed) viewport.scrollTop = scrollPositions.get(activeId) || 0;
      if (current.id === 'todoWorkspace' && global.TodoCalendar) {
        global.TodoCalendar.previewGeometry();
      }
      if (current.id === 'externalResources') global.ExternalResourceEditor?.ensureReady();
      if (focus && typeof current.section.focus === 'function') {
        current.section.focus({ preventScroll: true });
        if (viewport) viewport.scrollTop = 0;
      }
    };
    const activateHash = () => {
      let hash = '';
      try { hash = decodeURIComponent((global.location && global.location.hash || '').slice(1)); } catch (_) { }
      const target = byId(hash);
      const panel = target && target.closest('[data-workspace-panel]');
      const current = items.find(item => item.panel === panel);
      activate(current ? current.id : items[0].id);
    };
    const navigate = (event, item, focus = false) => {
      if (event.button > 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
      event.preventDefault();
      const hash = '#' + item.id;
      if (global.location.hash !== hash) {
        // Change the URL without navigating or running the page initialization again.
        global.history.pushState(global.history.state, '', hash);
      }
      activate(item.id, focus);
    };
    items.forEach(item => {
      item.link.setAttribute('aria-controls', item.panel.id);
      item.link.addEventListener('click', event => navigate(event, item));
    });
    const doc = getDocument();
    const skipLink = doc.querySelector('.skip-link');
    if (skipLink) skipLink.addEventListener('click', event => navigate(event, items[0], true));
    activateHash();
    if (global.addEventListener) {
      global.addEventListener('hashchange', activateHash);
      global.addEventListener('popstate', activateHash);
    }
  }

  function validateImageFile(file) {
    if (!file) return '未选择图片，请先选择一个图片文件。';
    if (file.size === 0) return '图片文件为空，请重新选择图片。';
    const extensionValid = /\.(png|jpe?g|bmp|webp)$/i.test(file.name || '');
    const mimeValid = /^image\/(png|jpe?g|bmp|x-ms-bmp|webp)$/i.test(file.type || '');
    if ((!file.type && !extensionValid) || (file.type && !mimeValid)) {
      return '图片格式不支持，请选择 PNG、JPG、BMP 或 WebP 文件。';
    }
    return '';
  }

  function refreshImageFileName() {
    const input = byId('imageFile');
    const file = input && input.files && input.files[0];
    setText('imageFileName', file ? file.name : '尚未选择图片');
  }

  function initImageDrop() {
    const zone = byId('imageDropZone');
    const input = byId('imageFile');
    if (!input) return;
    if (input.addEventListener) input.addEventListener('change', refreshImageFileName);
    refreshImageFileName();
    if (!zone || !zone.addEventListener) return;
    let dragDepth = 0;
    zone.addEventListener('dragenter', event => {
      event.preventDefault();
      dragDepth++;
      setClass(zone, 'is-dragging', true);
    });
    zone.addEventListener('dragover', event => {
      event.preventDefault();
      if (event.dataTransfer) event.dataTransfer.dropEffect = 'copy';
    });
    zone.addEventListener('dragleave', event => {
      event.preventDefault();
      dragDepth = Math.max(0, dragDepth - 1);
      if (!dragDepth) setClass(zone, 'is-dragging', false);
    });
    zone.addEventListener('drop', event => {
      event.preventDefault();
      dragDepth = 0;
      setClass(zone, 'is-dragging', false);
      const files = event.dataTransfer && event.dataTransfer.files;
      if (!files || files.length !== 1) {
        notify('每次请选择一张图片进行编辑。', true);
        return;
      }
      const validationError = validateImageFile(files[0]);
      if (validationError) { notify(validationError, true); return; }
      try {
        input.files = files;
        if (!input.files || input.files[0] !== files[0]) throw new Error('FileList assignment failed');
      } catch (_) {
        try {
          const transfer = new global.DataTransfer();
          transfer.items.add(files[0]);
          input.files = transfer.files;
          if (!input.files || input.files[0] !== files[0]) throw new Error('FileList assignment failed');
        } catch (_) {
          notify('当前浏览器无法接收拖放图片，请点击“选择图片”载入文件。', true);
          return;
        }
      }
      refreshImageFileName();
      if (typeof global.updateImage === 'function') global.updateImage();
    });
  }

  function saveDownload(href, filename, revoke) {
    const doc = getDocument();
    const link = doc.createElement('a');
    link.href = href;
    link.download = filename;
    if (doc.body && doc.body.appendChild) doc.body.appendChild(link);
    link.click();
    if (typeof link.remove === 'function') link.remove();
    else if (link.parentNode) link.parentNode.removeChild(link);
    if (revoke) global.setTimeout(() => global.URL.revokeObjectURL(href), 1000);
  }

  function downloadBlob(blob, filename) {
    const url = global.URL.createObjectURL(blob);
    try { saveDownload(url, filename, true); }
    catch (error) { global.URL.revokeObjectURL(url); throw error; }
  }

  function exportLog() {
    const log = byId('log');
    const lines = log && log.children ? Array.from(log.children).map(line => line.textContent).join('\n') : '';
    const text = lines || log && (log.innerText || log.textContent) || '';
    if (!text.trim()) { notify('暂无日志可导出。'); return; }
    try {
      const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
      downloadBlob(new global.Blob(['\uFEFF' + text + '\n'], { type: 'text/plain;charset=utf-8' }), `epd-log-${timestamp}.txt`);
      notify('日志已导出。');
    } catch (_) { notify('日志导出失败，请重试。', true); }
  }

  function downloadPreview() {
    const canvas = byId('canvas');
    if (!canvas || !canvas.width || !canvas.height) { notify('画布未准备好，请稍后重试。', true); return; }
    if (canvas.parentNode && canvas.parentNode.classList && canvas.parentNode.classList.contains('crop-mode')) {
      notify('请先完成图片裁剪，再下载预览。');
      return;
    }
    const filename = `epd-preview-${canvas.width}x${canvas.height}.png`;
    try {
      if (typeof global.prepareCanvasExport === 'function') global.prepareCanvasExport();
      if (typeof canvas.toBlob === 'function') {
        canvas.toBlob(blob => {
          if (!blob) { notify('预览导出失败，请重试。', true); return; }
          try { downloadBlob(blob, filename); notify('预览已下载。'); }
          catch (_) { notify('预览导出失败，请重试。', true); }
        }, 'image/png');
      } else {
        saveDownload(canvas.toDataURL('image/png'), filename, false);
        notify('预览已下载。');
      }
    } catch (_) { notify('预览导出失败，请重试。', true); }
  }

  function resetImageAdjustments() {
    for (const [id, value] of [['ditherAlg', 'floydSteinberg'], ['ditherStrength', '1.0'], ['ditherContrast', '1.2']]) {
      const control = byId(id);
      if (control) control.value = value;
    }
    setText('ditherStrengthValue', '1.0');
    setText('ditherContrastValue', '1.2');
    const canvas = byId('canvas');
    if (canvas && canvas.parentNode && canvas.parentNode.classList && canvas.parentNode.classList.contains('crop-mode')) {
      notify('调整参数已恢复默认值，完成裁剪后应用。');
      return;
    }
    if (typeof global.applyDither === 'function') global.applyDither();
    notify('图片调整已恢复默认值。');
  }

  function init() {
    if (initialized) return;
    initialized = true;
    initTheme();
    initNavigation();
    initImageDrop();
    const actions = {
      exportLogButton: exportLog,
      downloadPreviewButton: downloadPreview,
      resetImageAdjustmentsButton: resetImageAdjustments,
    };
    for (const id of Object.keys(actions)) {
      const button = byId(id);
      if (button && button.addEventListener) button.addEventListener('click', actions[id]);
    }
    updateConnection(connectionState);
  }

  global.ConsoleUI = { init, initTheme, updateConnection, validateImageFile, refreshImageFileName, exportLog, downloadPreview, resetImageAdjustments };
  const doc = getDocument();
  if (doc) {
    if (doc.readyState === 'loading') doc.addEventListener('DOMContentLoaded', init, { once: true });
    else init();
  }
})(typeof window !== 'undefined' ? window : globalThis);
