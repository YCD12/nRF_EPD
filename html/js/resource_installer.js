(function (root) {
  'use strict';
  const api = root.EPDResourceStore;
  const names = { 1: '分区表', 2: '设备配置', 3: '日期标注', 4: '更新状态', 5: '日历资源', 6: 'UI 布局', 7: '字体资源', 8: '保留区' };
  function formatSize(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    const kib = bytes / 1024;
    return `${kib >= 100 ? Math.round(kib) : kib.toFixed(1)} KiB`;
  }
  function renderUsage(partitions, usedById, capacity) {
    const box = $('externalResourceUsage');
    if (!box) return;
    const totalUsed = partitions.reduce((sum, partition) => sum + (usedById.get(partition.id) || 0), 0);
    const lines = partitions.map(partition => {
      const used = usedById.get(partition.id) || 0;
      const title = names[partition.id] || `分区 ${partition.id}`;
      return `${title}：占用 ${formatSize(used)} / ${formatSize(partition.size)}，剩余 ${formatSize(Math.max(0, partition.size - used))}`;
    });
    box.className = 'external-resource-message ready';
    box.textContent = [`外置 Flash：${formatSize(capacity)}，已占用 ${formatSize(totalUsed)}，剩余 ${formatSize(Math.max(0, capacity - totalUsed))}`, ...lines].join('\n');
  }
  function formatBuiltAt(seconds) {
    if (!seconds) return '无编译时间戳';
    const time = new Date(seconds * 1000);
    const pad = value => String(value).padStart(2, '0');
    return `${time.getFullYear()}-${pad(time.getMonth() + 1)}-${pad(time.getDate())} ${pad(time.getHours())}:${pad(time.getMinutes())}:${pad(time.getSeconds())}`;
  }
  const $ = id => document.getElementById(id);
  let client = null, characteristic = null, binding = 0, bundle = null, running = false, installationCompleted = false;
  let offlineSupported = false, completionMessage = '';
  let bundleGeneration = 0;

  function ensureCurrent(session) {
    if (!session || session !== client) throw new Error('蓝牙连接已变化，请重新连接');
  }

  function report(message, error = false) {
    const status = $('externalResourceStatus');
    if (status) status.textContent = message;
    if (typeof root.addLog === 'function') root.addLog(message);
    if (error && status) status.classList.add('error');
    else if (status) status.classList.remove('error');
  }

  function controls() {
    const connected = !!client;
    $('externalResourceInstall').disabled = running || !connected || !bundle;
    $('externalResourceRefresh').disabled = running || !connected;
    $('externalResourceFolder').disabled = running;
    const offline = $('externalResourceOffline');
    if (offline) offline.disabled = running || !connected || !offlineSupported;
  }

  function bind(nextCharacteristic) {
    if (client) client.disconnect();
    characteristic = nextCharacteristic;
    running = false;
    installationCompleted = false;
    offlineSupported = false; completionMessage = '';
    const token = ++binding;
    client = new api.ResourceClient(bytes => {
      if (token !== binding || characteristic !== nextCharacteristic)
        throw new Error('蓝牙连接已变化');
      return nextCharacteristic.writeValueWithResponse(bytes);
    });
    controls();
  }

  function disconnect() {
    ++binding;
    if (client) client.disconnect();
    client = null; characteristic = null; running = false;
    const status = $('externalResourceStatus');
    if (status) status.textContent = completionMessage || (installationCompleted ? '资源已写入，设备已重启，请重新连接。' : '设备未连接');
    const versions = $('deviceResourceVersions');
    if (versions && !installationCompleted) {
      versions.className = 'external-resource-message';
      versions.textContent = '尚未读取设备资源版本';
    }
    controls();
  }

  function notification(data) {
    if (!client || !data || data[0] !== 0xA7) return false;
    client.receive(data);
    return true;
  }

  async function readStatus(silent = false, session = client) {
    ensureCurrent(session);
    const status = api.parseStatus(await session.status());
    ensureCurrent(session);
    offlineSupported = status.offline;
    let message = `外置 Flash：可读写 · ${status.capacity / 1024} KiB · 分区表：${api.statusName(status.code)}`;
    if (status.code === 0) {
      const partitions = await session.partitions();
      ensureCurrent(session);
      message += ` · ${partitions.length} 个分区`;
      if (!silent) {
        const used = new Map();
        const moduleLines = [];
        for (const partition of partitions) {
          const title = names[partition.id] || `分区 ${partition.id}`;
          try {
            const info = api.parseHeader(await session.readRange(partition.offset, 32), partition);
            used.set(partition.id, 32 + info.length);
            moduleLines.push(`${title}：版本 ${info.revision}`);
          } catch (_) {
            used.set(partition.id, 0);
            moduleLines.push(`${title}：未安装`);
          }
        }
        ensureCurrent(session);
        renderUsage(partitions, used, status.capacity);
        const versions = $('deviceResourceVersions');
        if (versions) {
          versions.className = 'external-resource-message ready';
          versions.textContent = ['设备端资源版本', ...moduleLines].join('\n');
        }
        report(message);
        moduleLines.forEach(line => {
          if (typeof root.addLog === 'function') root.addLog(`外置 Flash ${line}`);
        });
      }
      return { status, partitions };
    }
    if (!silent) report(message);
    return { status, partitions: null };
  }

  async function refresh() {
    if (running) return;
    const session = client;
    running = true; controls();
    try { await readStatus(false, session); }
    catch (error) { if (client === session) report(error.message, true); }
    finally { if (client === session) { running = false; controls(); } }
  }

  async function install() {
    if (running) return;
    if (!client) return report('请先连接设备', true);
    if (!bundle) return report('请先选择资源目录', true);
    running = true; controls();
    const session = client, connection = characteristic, selected = bundle;
    const progress = $('externalResourceProgress');
    progress.value = 0;
    let entered = false;
    try {
      let current = await readStatus(true, session);
      ensureCurrent(session);
      if (current.status.active) {
        await session.request(5);
        current = await readStatus(true, session);
        ensureCurrent(session);
      }
      const initializing = current.status.code === 4;
      const packages = api.installationPlan(selected, current.status.code, current.partitions);
      const total = packages.reduce((sum, item) => sum + item.pack.payload.length, 0);
      let completedBytes = 0;
      for (let i = 0; i < packages.length; ++i) {
        ensureCurrent(session);
        const item = packages[i];
        const title = names[item.pack.id] || `分区 ${item.pack.id}`;
        report(`正在写入 ${i + 1}/${packages.length}：${title}`);
        entered = true;
        ensureCurrent(session);
        await session.upload(item.bytes, n => {
          if (client !== session) return;
          const done = completedBytes + n;
          progress.value = done / total;
          $('externalResourceStatus').textContent = `正在写入 ${i + 1}/${packages.length}：${title} · ${Math.round(done * 100 / total)}%`;
        }, current.partitions);
        completedBytes += item.pack.payload.length;
        if (item.pack.id === 1) current.partitions = selected.partitions;
      }
      ensureCurrent(session);
      await session.leave();
      ensureCurrent(session);
      entered = false;
      progress.value = 1;
      await readStatus(true, session);
      ensureCurrent(session);
      installationCompleted = true;
      if (current.status.hotReload) {
        completionMessage = '资源已应用，设备已断开连接。';
        report('资源已校验并应用，设备将刷新屏幕，无需断开连接。');
        if (current.status.offline && $('externalResourceAutoOffline')?.checked)
          await enterOffline(session, connection);
      } else {
        report(initializing ? '外置 Flash 初始化完成，设备正在重启。' : '外置 Flash 资源更新完成，设备正在重启。');
        completionMessage = '资源已写入，设备已重启，请重新连接。';
        try { await connection.writeValueWithResponse(Uint8Array.of(0x91)); }
        catch (_) { /* Legacy reset may close GATT before the write settles. */ }
      }
    } catch (error) {
      if (client === session) report(error.message, true);
    } finally {
      if (entered && client === session) {
        try { await session.leave(); } catch (_) { /* Disconnect cleanup also exits the session. */ }
      }
      if (client === session) { running = false; controls(); }
    }
  }

  async function enterOffline(session, connection) {
    ensureCurrent(session);
    const token = binding;
    if (typeof root.buildTimeData === 'function')
      await connection.writeValueWithResponse(Uint8Array.of(0x23, ...root.buildTimeData()));
    ensureCurrent(session);
    report('正在应用数据并断开连接…');
    await session.runOffline();
    // The final ACK precedes the intentional disconnect; keep its result visible.
    if (binding !== token && (client || binding !== token + 1)) return;
    completionMessage = '资源已应用，设备已断开连接，蓝牙继续按原策略间歇广播。';
    report(completionMessage);
  }

  async function runOffline() {
    if (running || !client) return;
    const session = client, connection = characteristic;
    running = true; controls();
    try {
      const { status } = await readStatus(true, session);
      if (!status.offline) throw new Error('当前固件不支持应用后自动断开连接，请手动断开');
      await enterOffline(session, connection);
    } catch (error) { if (client === session) report(error.message, true); }
    finally { if (client === session) { running = false; controls(); } }
  }

  const resourcePackageNames = ['partition-table.ers', 'calendar.ers', 'ui.ers', 'fonts.ers', 'epd-config.ers', 'calendar-annotations.ers'];
  async function loadBundle(entries, folderLabel) {
    if (running) throw new Error('请等待当前设备操作完成');
    ++bundleGeneration;
    bundle = null;
    const box = $('externalResourceBundle');
    box.className = 'external-resource-message';
    box.textContent = '正在校验资源目录…';
    try {
      bundle = api.prepareBundle(entries);
      box.className = 'external-resource-message ready';
      const heading = folderLabel ? `已加载 ${folderLabel}，共 ${bundle.total} 字节` : `已就绪，共 ${bundle.total} 字节`;
      box.textContent = [heading].concat(bundle.packages.map(item =>
        `${names[item.pack.id]}：${formatBuiltAt(item.pack.builtAt)}`)).join('\n');
      renderUsage(bundle.partitions, new Map(bundle.packages.map(item => [item.pack.id, item.pack.payload.length + 32])), 524288);
      if (root.ExternalResourceEditor) root.ExternalResourceEditor.load(bundle);
    } catch (error) {
      bundle = null;
      box.className = 'external-resource-message error';
      box.textContent = error.message;
      const usage = $('externalResourceUsage');
      if (usage) {
        usage.className = 'external-resource-message';
        usage.textContent = '尚未加载分区表';
      }
    }
    controls();
  }

  function showBundleError(message) {
    bundle = null;
    const box = $('externalResourceBundle');
    box.className = 'external-resource-message error';
    box.textContent = message;
    const usage = $('externalResourceUsage');
    if (usage) {
      usage.className = 'external-resource-message';
      usage.textContent = '尚未加载分区表';
    }
    controls();
  }

  function resourceFolders(files) {
    const groups = new Map();
    for (const file of files) {
      const relative = file.webkitRelativePath || file.name;
      const slash = relative.lastIndexOf('/');
      const folder = slash >= 0 ? relative.slice(0, slash) : '';
      if (!groups.has(folder)) groups.set(folder, []);
      groups.get(folder).push(file);
    }
    return groups;
  }

  $('externalResourceFolder').addEventListener('change', async event => {
    if (running) return;
    const token = ++bundleGeneration;
    const files = Array.from(event.target.files).filter(file => file.name.toLowerCase().endsWith('.ers'));
    const groups = resourceFolders(files);
    if (groups.size > 1) {
      const folders = [...groups.keys()].map(path => path.split('/').pop()).sort();
      const latest = folders.filter(name => /^external-resources-r\d+-\d{8}-\d{6}$/.test(name)).at(-1);
      const hint = latest ? `\n时间最新的是 ${latest}，请只选择这一层文件夹。` : '\n请只选择其中一套资源包所在的文件夹。';
      showBundleError(`选中的目录里有 ${folders.length} 套资源包，不能直接烧录。请不要选择 output 这种上级目录，改选其中一个文件夹：\n${folders.join('\n')}${hint}`);
      return;
    }
    try {
      const entries = await Promise.all(files.map(async file => ({ name: file.name, bytes: new Uint8Array(await file.arrayBuffer()) })));
      if (token !== bundleGeneration || running) return;
      const folder = [...groups.keys()][0] || '';
      await loadBundle(entries, folder ? folder.split('/').pop() : '');
    } catch (error) {
      if (token === bundleGeneration && !running) showBundleError(error.message);
    }
  });

  async function loadLatestOutput() {
    const token = bundleGeneration;
    const pointers = ['../output/external-resources-latest.txt', '/output/external-resources-latest.txt'];
    for (const pointer of pointers) {
      let folder = '';
      try {
        const response = await fetch(pointer, { cache: 'no-store' });
        if (!response.ok) continue;
        folder = (await response.text()).trim();
      } catch (_) {
        continue;
      }
      if (!/^external-resources-r\d+-\d{8}-\d{6}$/.test(folder)) continue;
      const prefix = pointer.slice(0, pointer.lastIndexOf('/') + 1);
      const entries = [];
      let complete = true;
      for (const name of resourcePackageNames) {
        try {
          const response = await fetch(`${prefix}${folder}/${name}`, { cache: 'no-store' });
          if (!response.ok) { complete = false; break; }
          entries.push({ name, bytes: new Uint8Array(await response.arrayBuffer()) });
        } catch (_) {
          complete = false;
          break;
        }
      }
      if (token !== bundleGeneration || running) return;
      if (!complete) continue;
      await loadBundle(entries, `output/${folder}`);
      return;
    }
  }
  loadLatestOutput();

  controls();
  root.ExternalResourceInstaller = { bind, disconnect, notification, refresh, install, runOffline,
    loadBundle, getBundle: () => bundle, busy: () => running };
})(globalThis);
