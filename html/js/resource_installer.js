(function (root) {
  'use strict';
  const api = root.EPDResourceStore;
  const names = { 1: '分区表', 2: '设备配置', 3: '日期标注', 5: '日历资源', 6: 'UI 布局', 7: '字体资源' };
  const $ = id => document.getElementById(id);
  let client = null, characteristic = null, binding = 0, bundle = null, running = false, installationCompleted = false;

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
  }

  function bind(nextCharacteristic) {
    if (client) client.disconnect();
    characteristic = nextCharacteristic;
    installationCompleted = false;
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
    if (status) status.textContent = installationCompleted ? '资源已写入，设备已重启，请重新连接。' : '设备未连接';
    controls();
  }

  function notification(data) {
    if (!client || !data || data[0] !== 0xA7) return false;
    client.receive(data);
    return true;
  }

  async function readStatus(silent = false) {
    if (!client) throw new Error('请先连接设备');
    const status = api.parseStatus(await client.status());
    let message = `外置 Flash：可读写 · ${status.capacity / 1024} KiB · 分区表：${api.statusName(status.code)}`;
    if (status.code === 0) {
      const partitions = await client.partitions();
      message += ` · ${partitions.length} 个分区`;
      if (!silent) report(message);
      return { status, partitions };
    }
    if (!silent) report(message);
    return { status, partitions: null };
  }

  async function refresh() {
    if (running) return;
    running = true; controls();
    try { await readStatus(); }
    catch (error) { report(error.message, true); }
    finally { running = false; controls(); }
  }

  async function install() {
    if (running) return;
    if (!client) return report('请先连接设备', true);
    if (!bundle) return report('请先选择资源目录', true);
    running = true; controls();
    const progress = $('externalResourceProgress');
    progress.value = 0;
    let entered = false;
    try {
      let current = await readStatus(true);
      if (current.status.active) {
        await client.request(5);
        current = await readStatus(true);
      }
      const initializing = current.status.code === 4;
      const packages = api.installationPlan(bundle, current.status.code, current.partitions);
      const total = packages.reduce((sum, item) => sum + item.pack.payload.length, 0);
      let completedBytes = 0;
      for (let i = 0; i < packages.length; ++i) {
        const item = packages[i];
        const title = names[item.pack.id] || `分区 ${item.pack.id}`;
        report(`正在写入 ${i + 1}/${packages.length}：${title}`);
        entered = true;
        await client.upload(item.bytes, n => {
          const done = completedBytes + n;
          progress.value = done / total;
          $('externalResourceStatus').textContent = `正在写入 ${i + 1}/${packages.length}：${title} · ${Math.round(done * 100 / total)}%`;
        }, current.partitions);
        completedBytes += item.pack.payload.length;
        if (item.pack.id === 1) current.partitions = bundle.partitions;
      }
      await client.leave();
      entered = false;
      progress.value = 1;
      await readStatus(true);
      installationCompleted = true;
      report(initializing ? '外置 Flash 初始化完成，设备正在重启。' : '外置 Flash 资源更新完成，设备正在重启。');
      try { await characteristic.writeValueWithResponse(Uint8Array.of(0x91)); }
      catch (_) { /* Reset commonly closes GATT before the write promise settles. */ }
    } catch (error) {
      report(error.message, true);
    } finally {
      if (entered && client) {
        try { await client.leave(); } catch (_) { /* Disconnect cleanup also exits the session. */ }
      }
      running = false; controls();
    }
  }

  $('externalResourceFolder').addEventListener('change', async event => {
    bundle = null;
    const box = $('externalResourceBundle');
    box.className = 'external-resource-message';
    box.textContent = '正在校验资源目录…';
    try {
      const files = Array.from(event.target.files).filter(file => file.name.toLowerCase().endsWith('.ers'));
      const entries = await Promise.all(files.map(async file => ({ name: file.name, bytes: new Uint8Array(await file.arrayBuffer()) })));
      bundle = api.prepareBundle(entries);
      box.className = 'external-resource-message ready';
      box.textContent = `已就绪：${bundle.packages.map(item => names[item.pack.id]).join('、')}，共 ${bundle.total} 字节`;
    } catch (error) {
      box.className = 'external-resource-message error';
      box.textContent = error.message;
    }
    controls();
  });

  controls();
  root.ExternalResourceInstaller = { bind, disconnect, notification, refresh, install, busy: () => running };
})(globalThis);
