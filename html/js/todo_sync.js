(function (root) {
  'use strict';
  const node = typeof module !== 'undefined' && module.exports;
  const store = node ? require('./resource_store.js') : root.EPDResourceStore;
  const builder = node ? require('./resource_builder.js') : root.EPDResourceBuilder;
  const TABLE = { id: 1, type: 1, offset: 0, size: 4096 };
  const noop = () => {};

  function nextRevision(revision) {
    if (revision >= 0xffffffff) throw new Error('资源版本已用尽，请重新安装资源');
    return revision + 1;
  }
  function tableWithTodos(original) {
    const partitions = store.parseTable(original);
    const existing = partitions.find(part => part.id === 9);
    if (existing) {
      if (existing.type !== 9 || existing.size < 4096) throw new Error('待办分区不兼容');
      return { partitions, bytes: null };
    }
    const reserved = partitions.find(part => part.id === 8 && part.type === 8 && part.size >= 8192);
    if (!reserved || partitions.length >= 32) throw new Error('设备没有可用的待办存储空间');
    const added = { id: 9, type: 9, offset: reserved.offset, size: 4096, schema: 1, flags: 0 };
    const updated = partitions.map(part => part === reserved
      ? { ...part, offset: part.offset + 4096, size: part.size - 4096 } : part).concat(added)
      .sort((a, b) => a.offset - b.offset);
    const payload = new Uint8Array(updated.length * 16), view = new DataView(payload.buffer);
    updated.forEach((part, index) => {
      const at = index * 16;
      view.setUint16(at, part.id, true); view.setUint16(at + 2, part.type, true);
      view.setUint32(at + 4, part.offset, true); view.setUint32(at + 8, part.size, true);
      view.setUint16(at + 12, part.schema, true); view.setUint16(at + 14, part.flags, true);
    });
    const bytes = builder.wrap(1, payload, nextRevision(original.revision));
    store.parseTable(store.parsePackage(bytes));
    return { partitions: updated, bytes };
  }
  async function available(client, check) {
    const status = store.parseStatus(await client.status()); check();
    if (status.code !== 0 || !status.hotReload || !status.verify)
      throw new Error('请先安装有效资源并升级支持待办的固件');
    if (status.entered || status.active) throw new Error('设备正在更新资源，请稍后重试');
    return status;
  }
  async function read(client, options = {}) {
    const check = options.assertCurrent || noop;
    await available(client, check);
    const partitions = await client.partitions(); check();
    const partition = partitions.find(part => part.id === 9);
    if (!partition) return null;
    if (partition.type !== 9) throw new Error('待办分区类型不兼容');
    const info = await client.inspect(9); check();
    if (info?.code === 4) return null;
    if (!info || info.code !== 0) throw new Error('待办数据校验失败，请恢复设备资源');
    const pack = store.parsePackage(await client.readPackage(partition)); check();
    if (pack.id !== 9) throw new Error('待办数据分区错误');
    if (pack.revision !== info.revision || pack.payload.length !== info.length || pack.crc !== info.crc)
      throw new Error('读取期间待办数据已变化，请重新读取');
    if (options.validate) options.validate(pack.payload);
    return pack.payload.slice();
  }
  async function save(client, payload, requests, options = {}) {
    const check = options.assertCurrent || noop, progress = options.progress || noop;
    check();
    if (options.validate) options.validate(payload);
    await available(client, check);
    // Validate every known failure before the font's first sector erase. The
    // old packages are also the recovery snapshots for this one connection.
    const originalBytes = (await client.readPackage(TABLE)).slice(); check();
    const original = store.parsePackage(originalBytes), plan = tableWithTodos(original);
    const partition = plan.partitions.find(part => part.id === 9);
    const installed = !plan.bytes ? await client.inspect(9) : { code: 4 }; check();
    if (!installed || ![0, 4].includes(installed.code)) throw new Error('待办数据校验失败，请恢复设备资源');
    let oldTodoBytes = null;
    if (installed.code === 0) {
      oldTodoBytes = (await client.readPackage(partition)).slice(); check();
      const old = store.parsePackage(oldTodoBytes);
      if (old.id !== 9 || old.revision !== installed.revision || old.payload.length !== installed.length || old.crc !== installed.crc)
        throw new Error('读取期间待办数据已变化，请重新保存');
      if (options.validate) options.validate(old.payload);
    }
    const bytes = builder.wrap(9, payload, nextRevision(installed.code === 0 ? installed.revision : 0));
    const expected = store.parsePackage(bytes);
    store.validateTarget(expected, plan.partitions);
    let entered = false, tableWritten = false, todoWritten = false, fontResult;
    const acquire = async () => {
      const until = Date.now() + (options.busyTimeout ?? 90000);
      while (!entered) {
        check();
        try { await client.request(6, Uint8Array.of(77, 73, 71, 49)); entered = true; check(); }
        catch (error) {
          if (error.code !== 2 || Date.now() >= until) throw error;
          progress('正在等待设备完成当前刷新…');
          await (options.pause || (ms => new Promise(resolve => setTimeout(resolve, ms))))(250);
        }
      }
    };
    const verifyRestored = async snapshot => {
      const pack = store.parsePackage(snapshot), info = await client.inspect(pack.id); check();
      if (!info || info.code !== 0 || info.revision !== pack.revision ||
          info.length !== pack.payload.length || info.crc !== pack.crc)
        throw new Error(`设备未确认分区 ${pack.id} 恢复校验通过`);
    };
    try {
      // Commit glyphs and items in one session, then reload once. This keeps
      // the old list from being rendered using a newly selected font.
      if (requests.length) {
        fontResult = await options.ensureFonts(requests);
        entered = fontResult?.deferred === true; check();
      }
      // Existing panel activity may finish before the session can be acquired.
      await acquire();
      // Verify again inside the acquired session, after any BUSY wait and
      // before the first sector erase. The table remains untouched on error.
      const current = store.parseHeader(await client.readRange(0, 32), TABLE); check();
      if (current.revision !== original.revision || current.crc !== original.crc || current.length !== original.payload.length)
        throw new Error('设备分区表已变化，请重新保存');
      if (options.onWrite) options.onWrite();
      if (plan.bytes) {
        progress('正在创建待办存储空间…');
        tableWritten = true;
        await client.upload(plan.bytes, noop, plan.partitions); check();
      }
      progress('正在保存待办到设备…');
      todoWritten = true;
      await client.upload(bytes, (done, total) => {
        check(); progress(`正在保存待办 ${Math.round(done * 100 / total)}%…`);
      }, plan.partitions); check();
      const verified = await client.inspect(9); check();
      if (!verified || verified.code !== 0 || verified.revision !== expected.revision ||
          verified.length !== payload.length || verified.crc !== expected.crc)
        throw new Error('设备未确认待办校验通过');
      progress('待办已提交，正在应用…');
      await client.leave(); check();
      entered = false;
      return true;
    } catch (error) {
      const changed = tableWritten || todoWritten || fontResult?.changed === true;
      if (entered || changed) {
        try {
          check();
          if (client.closed) throw new Error('蓝牙已断开');
          if (changed) progress('保存失败，正在恢复设备原有待办和字库…');
          await client.request(5); check();
          if (changed) {
            // LEAVE may have applied successfully while its ACK was lost.
            // Reacquire maintenance before restoring any coupled packages.
            entered = false; await acquire();
            if (tableWritten) {
              await client.upload(originalBytes, noop, plan.partitions); check();
              await verifyRestored(originalBytes);
            } else if (todoWritten) {
              if (oldTodoBytes) {
                await client.upload(oldTodoBytes, noop, plan.partitions); check();
                await verifyRestored(oldTodoBytes);
              } else {
                // BEGIN erases the package header; ABORT leaves the original
                // uninstalled state instead of retaining a newly committed list.
                const begin = new Uint8Array(13), view = new DataView(begin.buffer);
                begin[0] = 9; view.setUint32(1, payload.length, true);
                await client.request(2, begin); check();
                await client.request(5); check();
                const absent = await client.inspect(9); check();
                if (!absent || absent.code !== 4) throw new Error('设备未确认待办恢复为未安装状态');
              }
            }
            if (fontResult?.rollback) { await fontResult.rollback(); check(); }
          }
          await client.leave(); check();
          entered = false;
          if (changed) progress('设备原有待办和字库已恢复，请重试保存。');
        } catch (failure) {
          const result = new Error(`${error.message || error}；原有待办/字库恢复未完成：${failure.message || failure}，请重新连接并修复设备资源`);
          result.cause = error; result.recoveryIncomplete = true;
          throw result;
        }
      }
      throw error;
    }
  }
  const api = { tableWithTodos, read, save };
  if (node) module.exports = api;
  else root.EPDTodoSync = api;
})(globalThis);
