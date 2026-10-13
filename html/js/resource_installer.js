(function (root) {
  'use strict';
  const api = root.EPDResourceStore;
  const names = { 1: '分区表', 2: '设备配置', 3: '日期标注', 4: '更新状态', 5: '日历资源', 6: 'UI 布局', 7: '字体资源', 8: '保留区', 9: '待办事项' };
  function formatSize(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    const kib = bytes / 1024;
    return `${kib >= 100 ? Math.round(kib) : kib.toFixed(1)} KiB`;
  }
  const resourceIds = new Set([1, 2, 3, 5, 6, 7, 9]);
  function isResource(partition) { return resourceIds.has(partition.id) && partition.type !== 8; }
  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function canRenderDom() { return typeof document.createElement === 'function'; }
  function panelHeading(box, title, state, stateClass) {
    const heading = element('div', 'resource-panel-heading');
    heading.appendChild(element('h3', '', title));
    heading.appendChild(element('span', `resource-state ${stateClass}`, state));
    box.appendChild(heading);
  }
  function percent(used, capacity) { return capacity > 0 ? Math.max(0, Math.min(100, used * 100 / capacity)) : 0; }
  function systemNote(partition) { return partition.id === 8 || partition.type === 8 ? '保留区占用未统计' : '系统区占用未统计'; }
  function localSnapshot() {
    return bundle && { source: 'local', partitions: bundle.partitions, capacity: 524288,
      modules: new Map(bundle.packages.map(item => [item.pack.id,
        { state: 'local', used: item.pack.payload.length + 32, ...item.pack }])) };
  }
  function renderUsage() {
    const box = $('externalResourceUsage');
    if (!box) return;
    const snapshot = deviceSnapshot || localSnapshot();
    const source = snapshot?.source || (client ? 'device' : 'offline');
    const sourceLabel = source === 'local' ? '资源包估算' : source === 'offline' ? '设备未连接' : snapshot?.stale ? '上次读取 · 待刷新' : snapshot ? '设备读回' : '尚未读取';
    const partitions = snapshot?.partitions || [], modules = snapshot?.modules || new Map();
    const capacity = snapshot?.capacity;
    const confirmed = partitions.filter(partition => isResource(partition) && ['verified', 'local'].includes(modules.get(partition.id)?.state));
    const totalUsed = confirmed.reduce((sum, partition) => sum + modules.get(partition.id).used, 0);
    const remaining = confirmed.reduce((sum, partition) => sum + Math.max(0, partition.size - modules.get(partition.id).used), 0);
    const uncertain = partitions.some(partition => isResource(partition) && !['verified', 'local'].includes(modules.get(partition.id)?.state));
    const usedLabel = confirmed.length ? `${uncertain ? '已确认 ' : ''}${formatSize(totalUsed)}` : '未确认';
    const remainingLabel = confirmed.length ? `${uncertain ? '已确认 ' : ''}${formatSize(remaining)}` : '未确认';
    const note = snapshot?.message || (snapshot?.stale ? '数据为上次读取，资源已变更，请重新读取分区。余量仅统计资源分区，系统区与保留区未纳入。' : source === 'local'
      ? '按本地资源包估算，含每个模块的 32 字节包头；实际设备用量需连接后读取。系统区与保留区占用未统计。'
      : snapshot ? '仅统计 CRC 已校验的有效资源（含 32 字节包头）。仅读包头、未安装或校验失败的资源占用未确认；余量仅统计资源分区，系统区与保留区未纳入。'
        : client ? '连接已建立，读取分区后查看设备资源用量。' : '连接设备并读取分区，查看资源用量。');
    const lines = partitions.map(partition => {
      const info = modules.get(partition.id);
      const title = names[partition.id] || `分区 ${partition.id}`;
      if (!isResource(partition)) return `${title}：${formatSize(partition.size)} · ${systemNote(partition)}`;
      const known = info && ['verified', 'local'].includes(info.state);
      return known ? `${title}：占用 ${formatSize(info.used)} / ${formatSize(partition.size)}，剩余 ${formatSize(Math.max(0, partition.size - info.used))}`
        : `${title}：${formatSize(partition.size)} · 占用与余量未确认${info?.state === 'header-only' ? `（包头申报 ${formatSize(info.used)}）` : ''}`;
    });
    box.className = 'resource-panel flash-panel';
    box.textContent = '';
    if (!canRenderDom()) {
      if (!snapshot) { box.textContent = '尚未加载分区表'; return; }
      box.textContent = [`外置 Flash · ${sourceLabel}：${capacity ? formatSize(capacity) : '容量未读取'}，资源占用 ${usedLabel}，资源分区余量 ${remainingLabel}`, note, ...lines].join('\n');
      return;
    }
    panelHeading(box, '外置 Flash', sourceLabel, `source ${source}`);
    const metrics = element('div', 'flash-metrics');
    for (const [label, value] of [['总容量', capacity ? formatSize(capacity) : '—'], ['资源占用', usedLabel], ['资源分区余量', remainingLabel]]) {
      const metric = element('div', 'flash-metric');
      metric.appendChild(element('span', 'metric-label', label));
      metric.appendChild(element('strong', 'flash-metric-value', value));
      metrics.appendChild(metric);
    }
    box.appendChild(metrics);
    const bar = element('div', 'flash-capacity-bar');
    const fill = element('span');
    fill.style.width = `${percent(totalUsed, capacity)}%`;
    bar.appendChild(fill); box.appendChild(bar);
    box.appendChild(element('p', 'flash-capacity-label', confirmed.length
      ? `${source === 'local' ? '估算资源' : '已校验资源'}占总容量 ${percent(totalUsed, capacity).toFixed(1)}%`
      : '资源占用尚未确认'));
    box.appendChild(element('p', 'resource-panel-note', note));
    const list = element('div', 'flash-partitions');
    partitions.forEach(partition => {
      const info = modules.get(partition.id), known = info && ['verified', 'local'].includes(info.state);
      const row = element('div', `flash-partition ${info?.state || 'unmeasured'}`);
      row.setAttribute('data-partition', partition.id);
      const heading = element('div', 'flash-partition-heading');
      heading.appendChild(element('span', '', names[partition.id] || `分区 ${partition.id}`));
      heading.appendChild(element('span', '', `${known ? formatSize(info.used) : '—'} / ${formatSize(partition.size)}`));
      row.appendChild(heading);
      const track = element('div', 'flash-partition-track'), partFill = element('span');
      partFill.style.width = `${known ? percent(info.used, partition.size) : 0}%`;
      track.appendChild(partFill); row.appendChild(track);
      row.appendChild(element('p', 'flash-partition-note', !isResource(partition) ? systemNote(partition)
        : known ? `${source === 'local' ? '估算余量' : '分区余量'} ${formatSize(Math.max(0, partition.size - info.used))}`
          : info?.state === 'header-only' ? `包头申报 ${formatSize(info.used)}（含包头）；内容未校验，占用与余量未确认`
            : `占用与余量未确认${info?.note ? ` · ${info.note}` : ''}`));
      list.appendChild(row);
    });
    box.appendChild(list);
  }
  function renderVersions() {
    const box = $('deviceResourceVersions');
    if (!box) return;
    const snapshot = deviceSnapshot;
    const stateLabel = snapshot?.stale ? '上次读取 · 待刷新' : snapshot ? snapshot.message ? '读取待确认' : '设备已读回' : client ? '尚未读取' : '设备未连接';
    const rows = snapshot ? snapshot.partitions.filter(isResource).map(partition => {
      const info = snapshot.modules.get(partition.id) || { state: 'error', note: '读取未完成' };
      const local = bundle?.packages.find(item => item.pack.id === partition.id)?.pack;
      const localHint = local ? `本地 v${local.revision} · ${snapshot.stale ? '待重新读取' : info.state === 'verified'
        ? info.crc === local.crc && info.length === local.payload.length && info.revision === local.revision ? '内容一致' : '待更新'
        : '一致性未确认'}` : '';
      return { partition, info, localHint };
    }) : [];
    const labels = { verified: '已校验', 'header-only': '仅读包头', missing: '未安装或未提交', error: '读取或校验失败' };
    box.className = 'resource-panel resource-versions-panel';
    box.textContent = '';
    if (!canRenderDom()) {
      box.textContent = ['设备资源版本 · ' + stateLabel, snapshot?.message || '', ...rows.map(({ partition, info, localHint }) =>
        `${names[partition.id]}：${info.revision === undefined ? '—' : `v${info.revision}`} · ${labels[info.state]}${info.note ? ` · ${info.note}` : ''}${localHint ? ` · ${localHint}` : ''}`)].filter(Boolean).join('\n');
      return;
    }
    panelHeading(box, '设备资源版本', stateLabel, snapshot?.stale ? 'pending' : snapshot?.message ? 'error' : snapshot ? 'device' : client ? 'pending' : 'offline');
    if (!rows.length) box.appendChild(element('p', 'resource-panel-note', snapshot?.message || (client ? '读取分区后显示各模块的设备版本与校验结果。' : '连接设备后读取资源版本。')));
    const list = element('div', 'resource-version-list');
    rows.forEach(({ partition, info, localHint }) => {
      const row = element('div', 'resource-version-row');
      row.appendChild(element('span', 'resource-version-name', names[partition.id]));
      row.appendChild(element('strong', 'resource-version-value', info.revision === undefined ? '—' : `v${info.revision}`));
      row.appendChild(element('span', `resource-state ${info.state}`, snapshot.stale && ['verified', 'header-only'].includes(info.state)
        ? `上次${labels[info.state]}` : info.note || labels[info.state]));
      if (localHint) row.appendChild(element('span', 'local-version-hint', localHint));
      list.appendChild(row);
    });
    box.appendChild(list);
  }
  function renderBundle(message = '', error = false) {
    const box = $('externalResourceBundle');
    if (!box) return;
    box.className = 'resource-panel resource-bundle-panel';
    box.textContent = '';
    const heading = bundle ? (bundleLabel ? `已加载 ${bundleLabel}，共 ${bundle.total} 字节` : `已就绪，共 ${bundle.total} 字节`) : message || '尚未选择资源目录';
    if (!canRenderDom()) {
      box.textContent = [heading, ...(bundle && message ? [`${message}（保留原资源）`] : []),
        ...(bundle ? bundle.packages.map(item => `${names[item.pack.id]}：v${item.pack.revision} · ${formatBuiltAt(item.pack.builtAt)}`) : [])].join('\n');
      return;
    }
    panelHeading(box, '待烧录资源包', bundle ? '本地包已就绪' : error ? '资源包不可用' : '等待资源包', bundle ? 'ready' : error ? 'error' : 'pending');
    box.appendChild(element('p', 'resource-bundle-name', bundle ? bundleLabel || '本地资源目录' : heading));
    if (bundle && message) box.appendChild(element('p', error ? 'external-resource-message error' : 'external-resource-message', `${message}（保留原资源）`));
    if (!bundle) return;
    box.appendChild(element('div', 'resource-inline-stats', `${bundle.packages.length} 个资源包 · 数据总大小 ${formatSize(bundle.total)}`));
    const details = element('details', 'resource-package-details');
    details.appendChild(element('summary', '', '包内模块版本与生成时间'));
    bundle.packages.forEach(item => {
      const row = element('div', 'resource-package-row');
      row.appendChild(element('span', '', names[item.pack.id]));
      row.appendChild(element('strong', '', `v${item.pack.revision}`));
      row.appendChild(element('time', '', formatBuiltAt(item.pack.builtAt)));
      details.appendChild(row);
    });
    box.appendChild(details);
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
  let bundleLabel = '', deviceSnapshot = null, readbackData = null;
  // Retained across BLE reconnects until the next installation, so an erased
  // font partition can be retried with the exact merged glyphs prepared before
  // the failure. This is an in-page recovery journal, not an atomic rollback.
  let recoveryData = null;
  const readbackFiles = [
    { id: 1, name: 'partition-table.ers' }, { id: 5, name: 'calendar.ers' },
    { id: 6, name: 'ui.ers' }, { id: 7, name: 'fonts.ers' },
    { id: 2, name: 'epd-config.ers' }, { id: 3, name: 'calendar-annotations.ers' }
  ];

  function renderReadback() {
    const status = $('externalResourceReadbackStatus');
    if (!status) return;
    status.classList.remove('error');
    status.textContent = readbackData
      ? `已回读 ${readbackData.entries.length} 个资源包 · ${formatSize(readbackData.total)} · CRC 校验通过。${readbackData.stale ? '这是更新前的回读数据；保存当前资源请重新回读。' : '可保存为 ZIP，解压后得到原始 ERS 文件。'}`
      : '完整回读分区表、日历、布局、字体、设备配置和日期标注后，可保存为 ZIP。';
  }

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
    const busy = running || !!root.DeviceOperations?.current || !!root.ExternalResourceEditor?.busy();
    $('externalResourceInstall').disabled = busy || !connected || !bundle;
    $('externalResourceRefresh').disabled = busy || !connected;
    $('externalResourceFolder').disabled = busy;
    if ($('externalResourceReadback')) $('externalResourceReadback').disabled = busy || !connected;
    if ($('externalResourceSave')) $('externalResourceSave').disabled = running || !readbackData;
    if ($('externalResourceForce')) $('externalResourceForce').disabled = busy;
    if ($('externalResourceSaveRecovery')) $('externalResourceSaveRecovery').disabled = running || !recoveryData;
    if ($('externalResourceRetry')) $('externalResourceRetry').disabled = busy || !canRetry();
    const offline = $('externalResourceOffline');
    if (offline) offline.disabled = busy || !connected || !offlineSupported;
    root.ExternalResourceEditor?.controls();
  }

  async function task(action, parent = null) {
    const session = client;
    if (running || !session || root.ExternalResourceEditor?.busy()) return;
    let owner;
    try {
      owner = parent || root.DeviceOperations?.begin('resources');
      if (owner) root.DeviceOperations.assert(owner);
      session.owner = owner;
      return await action();
    } catch (error) {
      if (client === session) report(error.message, true);
    } finally {
      if (session.owner === owner) session.owner = null;
      if (!parent && owner) root.DeviceOperations.end(owner);
    }
  }

  async function deviceWrite(session, connection, bytes) {
    if (typeof root.write === 'function') {
      if (!await root.write(bytes[0], bytes.subarray(1), true, false, true, session.owner))
        throw new Error('蓝牙写入未完成');
    } else await connection.writeValueWithResponse(bytes);
  }

  function bind(nextCharacteristic) {
    if (client) client.disconnect();
    characteristic = nextCharacteristic;
    running = false;
    installationCompleted = false;
    offlineSupported = false; completionMessage = '';
    const token = ++binding;
    const session = new api.ResourceClient(bytes => {
      if (token !== binding || characteristic !== nextCharacteristic)
        throw new Error('蓝牙连接已变化');
      return deviceWrite(session, nextCharacteristic, bytes);
    }, 5000, () => typeof root.currentMtuValue === 'function' ? root.currentMtuValue() : 20);
    client = session;
    deviceSnapshot = null; readbackData = null;
    renderUsage(); renderVersions();
    renderReadback();
    renderRecovery();
    controls();
  }

  function disconnect() {
    ++binding;
    if (client) client.disconnect();
    client = null; characteristic = null; running = false;
    deviceSnapshot = null;
    const status = $('externalResourceStatus');
    if (status) status.textContent = completionMessage || (installationCompleted ? '资源已写入，设备已重启，请重新连接。' : '设备未连接');
    renderUsage(); renderVersions(); renderReadback();
    renderRecovery();
    controls();
  }

  function notification(data) {
    if (!client || !data || data[0] !== 0xA7) return false;
    client.receive(data);
    return true;
  }

  function markResourcesChanged() {
    if (readbackData) { readbackData.stale = true; renderReadback(); }
    if (deviceSnapshot) { deviceSnapshot.stale = true; renderUsage(); renderVersions(); }
  }

  async function ensureFonts(requests, parent = null, progress = () => {}) {
    const session = client;
    if (running || !session) throw new Error('设备资源连接不可用或正在执行其他任务');
    if (!root.EPDFontSync) throw new Error('网页字体模块未加载，请刷新页面');
    const owner = parent || root.DeviceOperations?.begin('fonts');
    try {
      if (owner) root.DeviceOperations.assert(owner);
      session.owner = owner;
      running = true; controls();
      return await root.EPDFontSync.ensure(session, requests, {
        assertCurrent: () => { ensureCurrent(session); if (owner) root.DeviceOperations.assert(owner); },
        progress, onWrite: markResourcesChanged
      });
    } finally {
      if (session.owner === owner) session.owner = null;
      if (session === client) { running = false; controls(); }
      if (!parent && owner) root.DeviceOperations.end(owner);
    }
  }

  async function todoOperation(action, parent = null) {
    const session = client;
    if (running || !session || !root.EPDTodoSync) throw new Error('待办资源模块或设备连接不可用');
    const owner = parent || root.DeviceOperations?.begin('todos');
    try {
      if (owner) root.DeviceOperations.assert(owner);
      session.owner = owner;
      running = true; controls();
      const assertCurrent = () => { ensureCurrent(session); if (owner) root.DeviceOperations.assert(owner); };
      return await action(session, {
        assertCurrent, onWrite: markResourcesChanged,
        validate: bytes => root.TodoCalendar.unpack(bytes),
        ensureFonts: requests => root.EPDFontSync.ensure(session, requests, {
          assertCurrent, progress: action.progress || (() => {}), onWrite: markResourcesChanged, deferApply: true
        })
      });
    } finally {
      if (session.owner === owner) session.owner = null;
      if (session === client) { running = false; controls(); }
      if (!parent && owner) root.DeviceOperations.end(owner);
    }
  }

  async function saveTodo(bytes, requests, parent = null, progress = () => {}) {
    const action = (session, options) => root.EPDTodoSync.save(session, bytes, requests, { ...options, progress });
    action.progress = progress;
    return todoOperation(action, parent);
  }

  async function readTodo(parent = null) {
    return todoOperation((session, options) => root.EPDTodoSync.read(session, options), parent);
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
        const modules = new Map();
        const moduleLines = [];
        for (const partition of partitions) {
          const title = names[partition.id] || `分区 ${partition.id}`;
          if (!isResource(partition)) continue;
          if (status.verify) {
            const info = await session.inspect(partition.id);
            ensureCurrent(session);
            if (info.code === 0 && Number.isInteger(info.length) && info.length > 0 && info.length + 32 <= partition.size) {
              modules.set(partition.id, { ...info, state: 'verified', used: 32 + info.length });
            } else {
              modules.set(partition.id, { state: info.code === 4 ? 'missing' : 'error',
                note: info.code === 0 ? '资源大小超出分区容量' : api.statusName(info.code) });
            }
            moduleLines.push(`${title}：${info.code === 0 ? `版本 ${info.revision} · 已校验` : api.statusName(info.code)}`);
          } else {
            const header = await session.readRange(partition.offset, 32);
            ensureCurrent(session);
            try {
              const info = api.parseHeader(header, partition);
              modules.set(partition.id, { ...info, state: 'header-only', used: 32 + info.length });
              moduleLines.push(`${title}：版本 ${info.revision} · 仅读包头`);
            } catch (error) {
              modules.set(partition.id, { state: /未安装或未提交/.test(error.message) ? 'missing' : 'error', note: error.message });
              moduleLines.push(`${title}：${error.message}`);
            }
          }
        }
        ensureCurrent(session);
        deviceSnapshot = { source: 'device', partitions, modules, capacity: status.capacity };
        renderUsage(); renderVersions();
        report(message);
        moduleLines.forEach(line => {
          if (typeof root.addLog === 'function') root.addLog(`外置 Flash ${line}`);
        });
      }
      return { status, partitions };
    }
    if (!silent) {
      deviceSnapshot = { source: 'device', partitions: [], modules: new Map(), capacity: status.capacity,
        message: `分区表${api.statusName(status.code)}，资源占用与版本未确认。` };
      renderUsage(); renderVersions();
      report(message);
    }
    return { status, partitions: null };
  }

  async function refresh(parent = null) { return task(refreshOperation, parent); }
  async function refreshOperation() {
    if (running) return;
    const session = client;
    deviceSnapshot = null;
    renderUsage(); renderVersions();
    running = true; controls();
    try { await readStatus(false, session); }
    catch (error) {
      if (client === session) {
        deviceSnapshot = { source: 'device', partitions: [], modules: new Map(), message: `读取失败：${error.message}，资源占用与版本未确认。` };
        renderUsage(); renderVersions();
        report(error.message, true);
      }
    }
    finally { if (client === session) { running = false; controls(); } }
  }

  async function readback() { return task(readbackOperation); }
  async function readbackOperation() {
    const session = client;
    if (running || !session) return;
    running = true; readbackData = null;
    renderReadback(); controls();
    const progress = $('externalResourceProgress');
    const note = $('externalResourceReadbackStatus');
    progress.value = 0;
    let title = '分区表';
    try {
      report('正在准备完整回读设备资源…');
      const status = api.parseStatus(await session.status());
      ensureCurrent(session);
      if (status.active || status.entered) throw new Error('设备正在更新资源，请等待更新完成后回读');
      if (status.code !== 0) throw new Error(`分区表${api.statusName(status.code)}，无法完整回读资源`);
      const partitions = await session.partitions();
      ensureCurrent(session);
      const files = readbackFiles.slice();
      const todoPartition = partitions.find(partition => partition.id === 9 && isResource(partition));
      if (todoPartition) {
        let installed;
        if (status.verify) {
          const todo = await session.inspect(9);
          ensureCurrent(session);
          if (!todo || ![0, 4].includes(todo.code)) throw new Error('待办资源校验失败');
          installed = todo.code === 0;
        } else {
          // Older firmware can still back up a new partition table. The
          // readPackage loop below checks the complete payload CRC locally.
          const header = await session.readRange(todoPartition.offset, 32);
          ensureCurrent(session);
          try { api.parseHeader(header, todoPartition); installed = true; }
          catch (error) {
            if (!/未安装或未提交/.test(error.message)) throw error;
            installed = false;
          }
        }
        if (installed) files.push({ id: 9, name: 'todo.ers' });
      }
      for (const file of readbackFiles) {
        if (!partitions.some(partition => partition.id === file.id && isResource(partition)))
          throw new Error(`设备分区表缺少${names[file.id]}，无法完整回读资源`);
      }
      const entries = [], modules = new Map();
      for (const [index, file] of files.entries()) {
        title = names[file.id];
        const partition = partitions.find(item => item.id === file.id);
        report(`正在回读 ${index + 1}/${files.length}：${title}`);
        if (note) note.textContent = `正在回读${title}，完成全部资源校验后可保存。`;
        let lastProgress = 0;
        const bytes = await session.readPackage(partition, (done, total) => {
          if (client !== session) return;
          const now = Date.now();
          if (done !== total && now - lastProgress < 100) return;
          lastProgress = now;
          progress.value = (index + done / total) / files.length;
          $('externalResourceStatus').textContent = `正在回读 ${index + 1}/${files.length}：${title} · ${Math.round(done * 100 / total)}%`;
        });
        ensureCurrent(session);
        const pack = api.parsePackage(bytes);
        api.validateTarget(pack, partitions);
        if (pack.id !== file.id) throw new Error('回读资源的分区 ID 不匹配');
        entries.push({ name: file.name, bytes, pack });
        modules.set(file.id, { ...pack, length: pack.payload.length, state: 'verified', used: bytes.length });
        progress.value = (index + 1) / files.length;
      }
      const table = entries.find(item => item.pack.id === 1);
      if (JSON.stringify(api.parseTable(table.pack)) !== JSON.stringify(partitions))
        throw new Error('回读期间分区表发生变化，请重新回读');
      ensureCurrent(session);
      const createdAt = new Date();
      const stamp = createdAt.toISOString().replace(/[-:]/g, '').replace('T', '-').slice(0, 15);
      readbackData = { entries, partitions, capacity: status.capacity, createdAt: createdAt.toISOString(),
        filename: `epd-resources-readback-${stamp}.zip`, total: entries.reduce((sum, item) => sum + item.bytes.length, 0) };
      deviceSnapshot = { source: 'device', partitions, modules, capacity: status.capacity };
      renderUsage(); renderVersions(); renderReadback();
      report(`资源回读完成！${entries.length} 个资源包 · ${formatSize(readbackData.total)}，可保存到本地。`);
    } catch (error) {
      if (client === session) {
        const message = `${title}回读失败：${error.message}。请重新完整回读后保存。`;
        if (note) { note.textContent = message; note.classList.add('error'); }
        report(message, true);
      }
    } finally {
      if (client === session) { running = false; controls(); }
    }
  }

  function saveReadback() {
    if (running) return;
    if (!readbackData) return report('请先完整回读设备资源', true);
    let url;
    try {
      const manifest = { format: 'EPD-RESOURCE-READBACK-1', readAt: readbackData.createdAt,
        flashCapacity: readbackData.capacity, partitions: readbackData.partitions,
        resources: readbackData.entries.map(({ name, bytes, pack }) => ({ name, id: pack.id,
          revision: pack.revision, bytes: bytes.length, payloadBytes: pack.payload.length,
          crc32: pack.crc.toString(16).padStart(8, '0') })) };
      const entries = [...readbackData.entries, { name: 'manifest.json',
        bytes: new TextEncoder().encode(JSON.stringify(manifest, null, 2) + '\n') }];
      const archive = root.EPDResourceArchive.createZip(entries);
      url = URL.createObjectURL(new Blob([archive], { type: 'application/zip' }));
      const link = document.createElement('a');
      link.href = url; link.download = readbackData.filename;
      document.body.appendChild(link);
      try { link.click(); } finally { link.remove(); }
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      report(`已发起下载：${readbackData.filename}`);
    } catch (error) {
      if (url) URL.revokeObjectURL(url);
      report(`保存回读数据失败：${error.message}`, true);
    }
  }

  function deviceIdentity(connection = characteristic) { return connection?.service?.device?.id || null; }
  function canRetry() {
    return !!(client && recoveryData?.state === 'failed' && bundleGeneration === recoveryData.selection &&
      !root.ExternalResourceEditor?.isDirty?.() && (recoveryData.deviceId
        ? deviceIdentity() === recoveryData.deviceId : client === recoveryData.session));
  }
  function renderRecovery() {
    const note = $('externalResourceRecoveryStatus');
    if (!note) return;
    if (!recoveryData) { note.textContent = '发送前会保留待更新资源的恢复备份；关闭或刷新网页前请保存到本地。'; return; }
    const labels = { pending: '未写入', writing: '写入中', committed: '已提交', skipped: '已校验一致', failed: '未完成' };
    const state = recoveryData.state === 'complete' ? '发送完成' : recoveryData.state === 'failed' ? '发送未完成' : '正在发送';
    note.textContent = `${state}。${recoveryData.tasks.map(item => `${names[item.pack.id]}：${labels[item.state]}`).join('；')}。` +
      `${recoveryData.entries.length} 份更新前备份可保存；备份和重试进度仅保留在当前网页。` +
      (recoveryData.state === 'failed' ? '连接同一设备后可重试未完成资源；已提交部分会重新校验。' : '') +
      (recoveryData.diagnostics.length ? ` 修复说明：${recoveryData.diagnostics.slice(0, 4).join('；')}${recoveryData.diagnostics.length > 4 ? '；其余详情见恢复备份。' : ''}` : '');
  }
  function saveRecovery() {
    if (running || !recoveryData) return;
    let url;
    try {
      const manifest = { format: 'EPD-RESOURCE-RECOVERY-1', createdAt: recoveryData.createdAt,
        deviceId: recoveryData.deviceId, initializing: recoveryData.initializing, state: recoveryData.state,
        error: recoveryData.error, partitions: recoveryData.partitions, diagnostics: recoveryData.diagnostics,
        resources: recoveryData.tasks.map(item => ({ id: item.pack.id, name: item.name, state: item.state,
          revision: item.pack.revision, crc32: item.pack.crc.toString(16).padStart(8, '0') })) };
      const entries = [...recoveryData.entries, ...recoveryData.tasks.map(item => ({ name: `planned/${item.name}`, bytes: item.bytes })),
        { name: 'manifest.json', bytes: new TextEncoder().encode(JSON.stringify(manifest, null, 2) + '\n') }];
      const archive = root.EPDResourceArchive.createZip(entries);
      url = URL.createObjectURL(new Blob([archive], { type: 'application/zip' }));
      const link = document.createElement('a');
      link.href = url; link.download = `epd-resources-recovery-${recoveryData.createdAt.replace(/[-:]/g, '').replace('T', '-').slice(0, 15)}.zip`;
      document.body.appendChild(link);
      try { link.click(); } finally { link.remove(); }
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      report('已发起下载恢复备份，before 为更新前数据，planned 为本次发送内容。');
    } catch (error) {
      if (url) URL.revokeObjectURL(url);
      report(`保存恢复备份失败：${error.message}`, true);
    }
  }
  async function retry() {
    if (!canRetry()) return report('请连接原设备并保留原资源选择后重试；资源已编辑时请重新生成并发送。', true);
    const previous = recoveryData;
    return task(() => installOperation(previous));
  }
  async function inspectForRetry(session, item, partitions, verify) {
    if (verify) return session.inspect(item.pack.id);
    const partition = partitions.find(part => part.id === item.pack.id);
    const header = await session.readRange(partition.offset, 32);
    ensureCurrent(session);
    let info;
    try { info = api.parseHeader(header, partition); } catch (_) { return { code: 3 }; }
    // Legacy firmware lacks VERIFY; check complete bytes locally rather than
    // trusting the old journal or rewriting already committed partitions.
    const bytes = await session.readRange(partition.offset, info.length + 32);
    ensureCurrent(session);
    try {
      const pack = api.parsePackage(bytes);
      return { code: 0, revision: pack.revision, crc: pack.crc, length: pack.payload.length };
    } catch (_) { return { code: 3 }; }
  }

  async function install() {
    // Generate before acquiring the device lock; generation itself must not
    // run while BLE owns that lock. Failed validation must never send old data.
    const editor = root.ExternalResourceEditor;
    if (editor?.isDirty?.()) {
      const session = client;
      if (!await editor.generate()) return;
      if (client !== session || editor.isDirty()) return report('资源或连接已变化，请重新发送', true);
    }
    if (bundle && root.EPDResourcePack?.validateBundle) {
      try { root.EPDResourcePack.validateBundle(bundle); }
      catch (error) { report(error.message, true); return; }
    }
    return task(installOperation);
  }
  async function installOperation(previous = null) {
    if (running) return;
    if (!client) return report('请先连接设备', true);
    if (!bundle) return report('请先选择资源目录', true);
    running = true; controls();
    const session = client, connection = characteristic, selected = bundle;
    const progress = $('externalResourceProgress');
    progress.value = 0;
    let entered = false, attempt = null, activeItem = null;
    try {
      let current = await readStatus(true, session);
      ensureCurrent(session);
      if (current.status.active) {
        await session.request(5);
        current = await readStatus(true, session);
        ensureCurrent(session);
      }
      const initializing = current.status.code === 4;
      let packages = api.installationPlan(selected, current.status.code, current.partitions);
      const backups = new Map(), diagnostics = previous?.diagnostics.slice() || [];
      if (previous) {
        // Keep the original initialization plan even when its partition table
        // already committed. Recomputing an ordinary update would omit config,
        // annotations and TODO resources on a partially initialized device.
        packages = previous.tasks.map(item => ({ ...item }));
        if (!initializing) packages.forEach(item => api.validateTarget(item.pack, current.partitions));
      }
      if (!previous && !initializing && root.EPDFontSync) {
        const fontItem = packages.find(item => item.pack.id === 7);
        if (fontItem) {
          const fontPartition = current.partitions.find(item => item.id === 7);
          const info = await session.inspect(7);
          ensureCurrent(session);
          // Invalid/missing old fonts can be repaired from the incoming pack.
          // Transport failures propagate so a read failure never loses glyphs.
          if (!info || info.code === 0) {
            report('正在保留设备已补充的字模…');
            const installed = await session.readPackage(fontPartition);
            ensureCurrent(session);
            backups.set(7, installed.slice());
            const retained = root.EPDFontSync.preserve(fontItem.bytes, installed, current.partitions, { preserveCustom: true });
            if (retained.repair) {
              diagnostics.push(`旧字体存在 ${retained.damaged?.length || 1} 处损坏，已备份原始资源，保留可解码字模并用所选字体修复`);
              report(diagnostics.at(-1));
              for (const item of retained.damaged || []) diagnostics.push(`${item.slot === undefined ? '' : `槽位 ${item.slot}：`}${item.message}`);
            }
            // Revision-only results also matter: they prevent a valid device
            // package from being downgraded and rewritten on every send.
            packages = packages.map(item => item === fontItem
              ? { ...item, bytes: retained.bytes, pack: api.parsePackage(retained.bytes) } : item);
            if (retained.added.length || retained.customPreserved) {
              report(retained.customPreserved ? `已保留设备待办字体，保留了 ${retained.added.length} 个设备字模`
                : `保留了 ${retained.added.length} 个设备字模`);
            }
          }
        }
      }
      attempt = { createdAt: previous?.createdAt || new Date().toISOString(), deviceId: previous?.deviceId || deviceIdentity(),
        session, selection: bundleGeneration, initializing: previous?.initializing || initializing,
        partitions: previous?.partitions || current.partitions || selected.partitions,
        entries: previous?.entries.slice() || [], diagnostics, state: 'preparing',
        tasks: packages.map(item => ({ ...item, bytes: item.bytes.slice(), state: 'pending' })) };
      recoveryData = attempt;
      packages = attempt.tasks;
      renderRecovery();
      if (!initializing && (current.status.verify || previous) && (previous || !$('externalResourceForce')?.checked)) {
        const changed = [];
        report('正在校验设备资源，跳过未变化的内容…');
        for (const item of packages) {
          const info = await inspectForRetry(session, item, current.partitions, current.status.verify);
          ensureCurrent(session);
          if (info.code !== 0 || info.length !== item.pack.payload.length || info.crc !== item.pack.crc || info.revision !== item.pack.revision)
            changed.push(item);
          else { item.state = 'skipped'; report(`${names[item.pack.id]}内容一致，已跳过`); }
        }
        packages = changed;
      }
      // Complete every available backup before starting the first erase. A
      // transport error is fatal here: it must not discard personal glyphs.
      if (!attempt.initializing && typeof session.readPackage === 'function') {
        for (const item of packages) {
          const name = `before/${item.name}`;
          if (attempt.entries.some(entry => entry.name === name || entry.name === name + '.raw.bin')) continue;
          const partition = current.partitions.find(part => part.id === item.pack.id);
          let bytes = backups.get(item.pack.id), raw = false;
          if (!bytes) {
            const info = current.status.verify ? await session.inspect(item.pack.id) : null;
            ensureCurrent(session);
            if (info?.code === 4) continue; // Uninstalled/uncommitted resources have no recoverable ERS.
            if (info && info.code !== 0) {
              bytes = await session.readRange(partition.offset, partition.size); raw = true;
            } else bytes = await session.readPackage(partition);
            ensureCurrent(session);
          }
          attempt.entries.push({ name: name + (raw ? '.raw.bin' : ''), bytes: bytes.slice() });
          renderRecovery();
        }
      }
      attempt.state = 'writing'; renderRecovery();
      const total = packages.reduce((sum, item) => sum + item.pack.payload.length, 0);
      let completedBytes = 0;
      for (let i = 0; i < packages.length; ++i) {
        ensureCurrent(session);
        const item = packages[i];
        activeItem = item; item.state = 'writing'; renderRecovery();
        const title = names[item.pack.id] || `分区 ${item.pack.id}`;
        report(`正在写入 ${i + 1}/${packages.length}：${title}`);
        entered = true;
        ensureCurrent(session);
        if (readbackData && !readbackData.stale) { readbackData.stale = true; renderReadback(); }
        if (deviceSnapshot && !deviceSnapshot.stale) {
          deviceSnapshot.stale = true;
          renderUsage(); renderVersions();
        }
        let lastProgress = 0;
        await session.upload(item.bytes, (n, size, durable = n) => {
          if (client !== session) return;
          const now = Date.now();
          if (n !== size && now - lastProgress < 100) return;
          lastProgress = now;
          const done = completedBytes + durable;
          progress.value = done / total;
          $('externalResourceStatus').textContent = n === size ? `${title}已写入，正在校验并提交…` :
            `正在写入 ${i + 1}/${packages.length}：${title} · ${Math.round(done * 100 / total)}%`;
        }, current.partitions);
        ensureCurrent(session);
        item.state = 'committed'; activeItem = null; renderRecovery();
        completedBytes += item.pack.payload.length;
        if (item.pack.id === 1) current.partitions = selected.partitions;
      }
      ensureCurrent(session);
      if (entered || previous) {
        report('资源已提交，正在应用…');
        await session.leave();
      }
      ensureCurrent(session);
      entered = false;
      progress.value = 1;
      await readStatus(true, session);
      ensureCurrent(session);
      installationCompleted = true;
      attempt.state = 'complete'; renderRecovery();
      if (current.status.hotReload) {
        completionMessage = '资源已应用，设备已断开连接。';
        report(packages.length ? '资源已校验并应用，设备将刷新屏幕，无需断开连接。' : '设备资源内容一致，无需写入。');
        if (current.status.offline && $('externalResourceAutoOffline')?.checked)
          await enterOffline(session, connection);
      } else {
        report(initializing ? '外置 Flash 初始化完成，设备正在重启。' : '外置 Flash 资源更新完成，设备正在重启。');
        completionMessage = '资源已写入，设备已重启，请重新连接。';
        try { await deviceWrite(session, connection, Uint8Array.of(0x91)); }
        catch (_) { /* Legacy reset may close GATT before the write settles. */ }
      }
    } catch (error) {
      if (attempt) {
        if (activeItem) activeItem.state = 'failed';
        attempt.state = 'failed'; attempt.error = error.message;
        if (recoveryData === attempt) renderRecovery();
      }
      if (client === session) report(error.message, true);
    } finally {
      if (entered && client === session) {
        try { await session.leave(); }
        catch (error) {
          if (attempt) attempt.error = `${attempt.error || '发送未完成'}；设备应用失败：${error.message}`;
          if (client === session) report(`${attempt?.error || error.message}。请保留恢复备份并重试未完成资源。`, true);
        }
      }
      if (client === session) { running = false; controls(); }
    }
  }

  async function enterOffline(session, connection) {
    ensureCurrent(session);
    const token = binding;
    if (typeof root.buildTimeData === 'function')
      await deviceWrite(session, connection, Uint8Array.of(0x23, ...root.buildTimeData()));
    ensureCurrent(session);
    report('正在应用数据并断开连接…');
    await session.runOffline();
    // The final ACK precedes the intentional disconnect; keep its result visible.
    if (binding !== token && (client || binding !== token + 1)) return;
    completionMessage = '资源已应用，设备已断开连接，蓝牙继续按原策略间歇广播。';
    report(completionMessage);
  }

  async function runOffline() { return task(offlineOperation); }
  async function offlineOperation() {
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
    if (running || root.DeviceOperations?.current) throw new Error('请等待当前设备操作完成');
    ++bundleGeneration;
    try {
      const inspected = root.EPDResourcePack?.inspectBundle?.(entries, { repairFontBounds: true });
      const next = inspected?.bundle || (root.EPDResourcePack?.validateBundle ? root.EPDResourcePack.validateBundle(entries) : api.prepareBundle(entries));
      if (root.ExternalResourceEditor) root.ExternalResourceEditor.load(next, inspected?.model, inspected?.repairedSlots);
      bundle = next;
      bundleLabel = folderLabel || '';
      renderBundle(); renderUsage(); renderVersions();
    } catch (error) {
      renderBundle(error.message, true); renderUsage(); renderVersions();
      throw error;
    } finally {
      controls();
    }
  }

  function showBundleError(message) {
    renderBundle(message, true); renderUsage(); renderVersions();
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
    if (running || root.DeviceOperations?.current || root.ExternalResourceEditor?.busy()) return;
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
      if (token !== bundleGeneration || running || root.DeviceOperations?.current || root.ExternalResourceEditor?.busy()) return;
      const folder = [...groups.keys()][0] || '';
      await loadBundle(entries, folder ? folder.split('/').pop() : '');
    } catch (error) {
      if (token === bundleGeneration && !running) showBundleError(error.message);
    }
  });

  let latestOutputPromise = null;
  function loadLatestOutput() {
    if (bundle || running) return Promise.resolve();
    if (!latestOutputPromise) latestOutputPromise = findLatestOutput();
    return latestOutputPromise;
  }

  async function findLatestOutput() {
    const token = bundleGeneration;
    const candidates = ['../output/external-resources-latest.txt', '/output/external-resources-latest.txt'];
    const pointers = candidates.filter((pointer, index) => !index || !root.location ||
      new URL(pointer, root.location.href).href !== new URL(candidates[0], root.location.href).href);
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
      if (complete) {
        try {
          const response = await fetch(`${prefix}${folder}/todo.ers`, { cache: 'no-store' });
          if (response.ok) entries.push({ name: 'todo.ers', bytes: new Uint8Array(await response.arrayBuffer()) });
        } catch (_) { /* Older resource directories have no optional todo package. */ }
      }
      if (token !== bundleGeneration || running || root.DeviceOperations?.current || root.ExternalResourceEditor?.busy()) return;
      if (!complete) continue;
      await loadBundle(entries, `output/${folder}`);
      return;
    }
  }
  renderBundle(); renderUsage(); renderVersions(); renderReadback(); renderRecovery();
  controls();
  root.ExternalResourceInstaller = { bind, disconnect, notification, refresh, readback, saveReadback, saveRecovery, retry, install, runOffline,
    ensureFonts, saveTodo, readTodo, loadBundle, loadLatestOutput, controls, getBundle: () => bundle,
    selectionVersion: () => bundleGeneration, busy: () => running };
})(globalThis);
