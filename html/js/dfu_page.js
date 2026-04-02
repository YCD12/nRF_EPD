(function () {
  'use strict';

  const DFU_PAGE_HANDOFF_KEY = 'epd_dfu_page_handoff';
  let toastTimer = null;

  function getDfuLog() {
    return document.getElementById('log');
  }

  function showToast(message, duration = 2600) {
    const toast = document.getElementById('toast');
    if (!toast) return;
    toast.innerText = message;
    toast.style.display = 'block';
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(() => {
      toast.style.display = 'none';
      toastTimer = null;
    }, duration);
  }

  function addLog(logTXT, action = '') {
    const log = getDfuLog();
    if (!log) return;
    const autoFollow = (log.scrollHeight - (log.scrollTop + log.clientHeight)) < 24;
    const now = new Date();
    const time = String(now.getHours()).padStart(2, '0') + ':' +
      String(now.getMinutes()).padStart(2, '0') + ':' +
      String(now.getSeconds()).padStart(2, '0') + ' ';

    const logEntry = document.createElement('div');
    const timeSpan = document.createElement('span');
    logEntry.className = 'log-line';
    timeSpan.className = 'time';
    timeSpan.textContent = time;
    logEntry.appendChild(timeSpan);

    if (action !== '') {
      const actionSpan = document.createElement('span');
      actionSpan.className = 'action';
      actionSpan.innerHTML = action;
      logEntry.appendChild(actionSpan);
    }
    logEntry.appendChild(document.createTextNode(logTXT));
    log.appendChild(logEntry);

    while (log.childNodes.length > 200) {
      log.removeChild(log.firstChild);
    }

    if (autoFollow) {
      log.scrollTop = log.scrollHeight;
    }

    const isErr = /失败|超时|Error|异常|disconnected|断开|warn|warning/i.test(logTXT);
    if (isErr) console.warn('[DFU]', `${time}${action}${logTXT}`);
    else console.log('[DFU]', `${time}${action}${logTXT}`);
  }

  function clearLog() {
    const log = getDfuLog();
    if (log) log.innerHTML = '';
  }

  function getRuntimeState() {
    if (typeof window.getDfuPageState === 'function') {
      return window.getDfuPageState();
    }
    return {
      busy: false,
      hasPendingSession: false,
      manualBootloaderEntryRequired: false
    };
  }

  function shouldProtectLeave() {
    const state = getRuntimeState();
    return !!(state.busy || state.hasPendingSession);
  }

  function renderDfuModeState(state) {
    const badge = document.getElementById('dfuModeBadge');
    const hint = document.getElementById('dfuModeHint');
    const flowGuide = document.getElementById('dfuFlowGuide');
    const abandonBtn = document.getElementById('dfuAbandonSessionButton');
    const resetBtn = document.getElementById('dfuResetModeButton');
    if (!badge || !hint || !flowGuide || !abandonBtn || !resetBtn) return;

    abandonBtn.disabled = !!(state.busy || !state.hasPendingSession);

    if (state.manualBootloaderEntryRequired) {
      badge.textContent = '手动 DfuTarg';
      hint.textContent = state.hasPendingSession
        ? '当前存在待继续的 DFU 会话，请优先完成或放弃当前会话；下一次新的升级尝试会直接要求选择 DfuTarg。'
        : '当前已切换到手动 DfuTarg 流程。请先让设备进入 DfuTarg，再点击“选择设备并升级”；如果想恢复默认 buttonless 流程，可点“恢复默认流程”。';
      flowGuide.textContent = state.hasPendingSession
        ? '当前页面处于手动 DfuTarg 模式，且存在一个可恢复会话。此时不要刷新或直接离开页面；如果决定终止，可以点“放弃当前会话”，之后再决定是否恢复默认流程。'
        : '当前页面处于手动 DfuTarg 模式。页面不会再先选应用设备，而是要求你先把设备切到 DfuTarg，再直接选择 DFU 设备继续升级。';
      resetBtn.disabled = !!(state.busy || state.hasPendingSession);
      return;
    }

    badge.textContent = '默认流程';
    hint.textContent = '当前按 Nordic 官方 unbonded buttonless DFU 流程执行：先选择应用设备，再切换到 DfuTarg。';
    flowGuide.textContent = state.hasPendingSession
      ? '当前仍有一个可恢复的 DFU 会话。建议优先继续或放弃这个会话；如果直接离开页面，这次可恢复状态会丢失。'
      : '页面跳转不会继承浏览器中的 GATT 连接，所以从主界面跳到本页后，升级阶段会重新选择应用设备；切到 Bootloader 后，如果浏览器尚未持有 DfuTarg 授权，页面会提示你再次确认。';
    resetBtn.disabled = true;
  }

  function loadHandoffState() {
    try {
      const raw = sessionStorage.getItem(DFU_PAGE_HANDOFF_KEY);
      sessionStorage.removeItem(DFU_PAGE_HANDOFF_KEY);
      return raw ? JSON.parse(raw) : null;
    } catch (_) {
      return null;
    }
  }

  function applyHandoffNotice(handoff) {
    const note = document.getElementById('dfuEntryNotice');
    const source = document.getElementById('dfuEntrySource');
    if (!note || !source) return;

    if (!handoff) {
      source.textContent = '独立打开';
      note.textContent = '当前页面未检测到主界面跳转状态。由于页面之间不能继承 Web Bluetooth 的 GATT 连接，升级时会重新选择应用设备或 DfuTarg。';
      addLog('未检测到主界面跳转状态，按独立 DFU 页面处理');
      return;
    }

    source.textContent = handoff.source === 'index' ? '主界面跳转' : '未知来源';
    if (handoff.hadConnectedDevice) {
      note.textContent = handoff.disconnectedBeforeNavigate
        ? `已从主界面进入 DFU 升级页。跳转前已断开设备 ${handoff.deviceName || ''} 的主连接；进入本页后需要重新选择应用设备或 DfuTarg 才能升级。`
        : '已从主界面进入 DFU 升级页。浏览器不会跨页面保留蓝牙连接；进入本页后需要重新选择应用设备或 DfuTarg 才能升级。';
      addLog(`从主界面进入 DFU 页，原连接设备: ${handoff.deviceName || 'Unknown Device'}`);
    } else {
      note.textContent = '已从主界面进入 DFU 升级页。主界面当时没有活动蓝牙连接，本页将从选择设备开始执行 DFU。';
      addLog('从主界面进入 DFU 页，未携带活动连接');
    }
  }

  function bindPageActions() {
    const backBtn = document.getElementById('dfuBackButton');
    const clearBtn = document.getElementById('dfuClearLogButton');
    const abandonBtn = document.getElementById('dfuAbandonSessionButton');
    const resetModeBtn = document.getElementById('dfuResetModeButton');
    if (backBtn) {
      backBtn.addEventListener('click', () => {
        const state = getRuntimeState();
        if (state.busy) {
          showToast('DFU 升级进行中，请先等待完成或失败后再返回', 3200);
          addLog('已阻止返回主界面：当前 DFU 正在进行中');
          return;
        }
        if (state.hasPendingSession) {
          const confirmed = window.confirm('当前有可恢复的 DFU 会话，离开页面后将无法继续。确定返回主界面吗？');
          if (!confirmed) {
            addLog('已取消返回主界面：保留当前可恢复的 DFU 会话');
            return;
          }
        }
        window.location.href = 'index.html';
      });
    }
    if (clearBtn) {
      clearBtn.addEventListener('click', clearLog);
    }
    if (abandonBtn) {
      abandonBtn.addEventListener('click', () => {
        const state = getRuntimeState();
        if (state.busy) {
          showToast('DFU 升级进行中，暂时不能放弃当前会话', 3200);
          addLog('已阻止放弃当前会话：当前 DFU 正在进行中');
          return;
        }
        if (!state.hasPendingSession) {
          showToast('当前没有可放弃的 DFU 会话');
          return;
        }
        const confirmed = window.confirm('放弃后将无法继续当前 DFU 会话。确定要放弃吗？');
        if (!confirmed) {
          addLog('已取消放弃当前 DFU 会话');
          return;
        }
        if (typeof window.abandonPendingDfuSession === 'function' && window.abandonPendingDfuSession()) {
          renderDfuModeState(getRuntimeState());
          showToast('已放弃当前 DFU 会话');
        }
      });
    }
    if (resetModeBtn) {
      resetModeBtn.addEventListener('click', () => {
        const state = getRuntimeState();
        if (state.busy || state.hasPendingSession) {
          showToast('当前仍有进行中或待继续的 DFU 会话，暂时不能恢复默认流程', 3200);
          addLog('已阻止恢复默认流程：当前仍存在活动或待继续的 DFU 会话');
          return;
        }
        if (typeof window.resetDfuMode === 'function' && window.resetDfuMode()) {
          renderDfuModeState(getRuntimeState());
          showToast('已恢复默认升级流程');
        }
      });
    }
  }

  window.showToast = showToast;
  window.addLog = addLog;
  window.onDfuPageStateChange = renderDfuModeState;

  window.addEventListener('beforeunload', event => {
    if (shouldProtectLeave()) {
      event.preventDefault();
      event.returnValue = '';
    }
  });

  window.addEventListener('load', () => {
    bindPageActions();
    applyHandoffNotice(loadHandoffState());
    if (typeof initDfuPanel === 'function') {
      initDfuPanel();
    }
    renderDfuModeState(getRuntimeState());
  });
})();
