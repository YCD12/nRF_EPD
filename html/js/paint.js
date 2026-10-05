class PaintManager {
  constructor(canvas, ctx) {
    this.canvas = canvas;
    this.ctx = ctx;
    this.painting = false;
    this.lastX = null;
    this.lastY = null;
    this.brushColor = '#000000';
    this.brushSize = 2;
    this.eraserSize = 16;
    this.currentTool = null;
    this.textElements = [];
    this.lineSegments = [];
    this.todoItems = [];
    this.isTextPlacementMode = false;
    this.isTodoPlacementMode = false;
    this.selectedTextElement = null;
    this.selectedEditingText = null;
    this.selectedTodoItem = null;
    this.isDraggingText = false;
    this.dragOffsetX = 0;
    this.dragOffsetY = 0;
    this.dragChanged = false;
    this.textBold = false;
    this.textItalic = false;
    this.todoBold = false;
    this.todoItalic = false;
    this.todoColor = '#000000';
    this.scheduleData = null;
    this.scheduleDays = 5;
    this.scheduleClasses = 6;
    this.scheduleFontFamily = 'SimHei';
    this.scheduleFontSize = 12;
    this.scheduleColor = '#000000';
    this.scheduleStartX = 20;
    this.scheduleStartY = 20;
    this.scheduleCellWidth = 60;
    this.scheduleCellHeight = 35;
    this.weekDays = ['周一', '周二', '周三', '周四', '周五', '周六', '周日'];
    this.selectedScheduleCell = null;
    this.showScheduleCellIndicator = false;
    this.scheduleCellFontSizes = null;
    this.brushCursor = null;
    this.selectionOverlay = null;
    this.baseImageData = null;
    this.baseImageUrl = null;
    this.historyStack = [];
    this.historyStep = -1;
    this.MAX_HISTORY = 25;
    this.workspaceRevision = 0;
    this.renderOrder = 0;
    this.touchActive = false;
    this.suppressMouseUntil = 0;
    ['startPaint', 'paint', 'endPaint', 'handleCanvasClick', 'onTouchStart', 'onTouchMove',
      'onTouchEnd', 'handleKeyboard', 'updateBrushCursor', 'hideBrushCursor'].forEach(name => {
      this[name] = this[name].bind(this);
    });
  }

  element(id) { return document.getElementById(id); }

  value(id, fallback = '') {
    const input = this.element(id);
    return input ? input.value : fallback;
  }

  number(value, min, max, fallback) {
    const parsed = Number(value);
    return Number.isFinite(parsed) && String(value).trim() !== ''
      ? Math.min(max, Math.max(min, parsed)) : fallback;
  }

  inputNumber(id, min, max, fallback) {
    const value = this.number(this.value(id), min, max, fallback);
    const input = this.element(id);
    if (input) input.value = value;
    return value;
  }

  setFontInput(id, family) {
    const select = this.element(id);
    if (!select) return;
    if (select.options && !Array.from(select.options).some(option => option.value === family)) {
      const option = document.createElement('option');
      option.value = family; option.textContent = family;
      select.appendChild(option);
    }
    select.value = family;
  }

  assignRenderOrders() {
    const items = [...this.lineSegments, ...this.textElements, ...this.todoItems];
    this.renderOrder = items.reduce((maximum, item) => Math.max(maximum, Number.isFinite(item.renderOrder) ? item.renderOrder : 0), 0);
    // Preserve the original layering of old editable caches.
    items.forEach(item => { if (!Number.isFinite(item.renderOrder)) item.renderOrder = ++this.renderOrder; });
  }

  report(message, error = false) {
    const status = this.element('paint-tool-status');
    if (status) {
      status.textContent = message;
      status.classList.toggle('error', error);
    }
  }

  isCropMode() { return this.canvas.parentNode.classList.contains('crop-mode'); }

  captureBaseImage() {
    this.baseImageData = this.ctx.getImageData(0, 0, this.canvas.width, this.canvas.height);
    this.baseImageUrl = null;
    this.workspaceRevision++;
  }

  resetBaseImage() {
    this.baseImageData = null;
    this.baseImageUrl = null;
    this.workspaceRevision++;
  }

  drawBaseImage() {
    this.ctx.save();
    this.ctx.setTransform(1, 0, 0, 1, 0, 0);
    this.ctx.fillStyle = '#ffffff';
    this.ctx.fillRect(0, 0, this.canvas.width, this.canvas.height);
    if (this.baseImageData) {
      if (this.baseImageData.width === this.canvas.width && this.baseImageData.height === this.canvas.height) {
        this.ctx.putImageData(this.baseImageData, 0, 0);
      } else {
        const baseCanvas = document.createElement('canvas');
        baseCanvas.width = this.baseImageData.width;
        baseCanvas.height = this.baseImageData.height;
        baseCanvas.getContext('2d').putImageData(this.baseImageData, 0, 0);
        this.ctx.drawImage(baseCanvas, 0, 0, this.canvas.width, this.canvas.height);
      }
    }
    this.ctx.restore();
  }

  scheduleSnapshot() {
    if (typeof this.captureScheduleState === 'function') return this.captureScheduleState();
    const state = {};
    ['scheduleData', 'scheduleDays', 'scheduleClasses', 'scheduleFontFamily', 'scheduleFontSize',
      'scheduleColor', 'scheduleStartX', 'scheduleStartY', 'scheduleCellWidth', 'scheduleCellHeight',
      'scheduleCellFontSizes'].forEach(key => { state[key] = this[key]; });
    return JSON.parse(JSON.stringify(state));
  }

  applyScheduleSnapshot(state) {
    if (typeof this.restoreScheduleState === 'function') this.restoreScheduleState(state);
    else if (state) Object.assign(this, JSON.parse(JSON.stringify(state)));
  }

  saveToHistory(persist = true, renderTodos = true) {
    this.workspaceRevision++;
    this.historyStack = this.historyStack.slice(0, this.historyStep + 1);
    this.historyStack.push({
      width: this.canvas.width, height: this.canvas.height,
      baseImageData: this.baseImageData,
      textElements: JSON.parse(JSON.stringify(this.textElements)),
      lineSegments: JSON.parse(JSON.stringify(this.lineSegments)),
      todoItems: JSON.parse(JSON.stringify(this.todoItems)),
      schedule: this.scheduleSnapshot(),
      orientation: typeof captureCanvasOrientation === 'function' ? captureCanvasOrientation() : 0
    });
    if (this.historyStack.length > this.MAX_HISTORY) this.historyStack.shift();
    this.historyStep = this.historyStack.length - 1;
    this.updateUndoRedoButtons();
    if (renderTodos) this.renderTodoList();
    if (persist) this.saveCanvasToLocalStorage();
  }

  saveCanvasToLocalStorage() {
    try {
      if (this.baseImageData && !this.baseImageUrl) {
        const baseCanvas = document.createElement('canvas');
        baseCanvas.width = this.baseImageData.width;
        baseCanvas.height = this.baseImageData.height;
        baseCanvas.getContext('2d').putImageData(this.baseImageData, 0, 0);
        this.baseImageUrl = baseCanvas.toDataURL('image/png');
      }
      localStorage.setItem('canvasState', JSON.stringify({
        version: 2, baseDataUrl: this.baseImageUrl,
        textElements: this.textElements, lineSegments: this.lineSegments,
        todoItems: this.todoItems, schedule: this.scheduleSnapshot(),
        width: this.canvas.width, height: this.canvas.height,
        orientation: typeof captureCanvasOrientation === 'function' ? captureCanvasOrientation() : 0
      }));
    } catch (error) {
      // Keep the last complete cache if the browser storage quota is exhausted.
      this.report('当前画布可继续编辑；浏览器缓存空间不足，请及时保存图片。', true);
    }
  }

  loadCanvasFromLocalStorage() {
    try {
      const raw = localStorage.getItem('canvasState');
      if (!raw) return false;
      const data = JSON.parse(raw);
      if (!Number.isInteger(data.width) || !Number.isInteger(data.height) || data.width < 1 || data.height < 1 ||
          data.width > 4096 || data.height > 4096 || data.width * data.height > 16000000) return false;
      if (data.version !== 2 && (data.width !== this.canvas.width || data.height !== this.canvas.height)) return false;
      const revision = this.workspaceRevision;
      const restore = () => {
        if (revision !== this.workspaceRevision) return false;
        this.canvas.width = data.width;
        this.canvas.height = data.height;
        // Legacy caches store the composed image. Restore it as one bitmap to avoid duplicate text.
        const editable = data.version === 2 || (!data.imageDataUrl && !data.imageData);
        this.textElements = editable && Array.isArray(data.textElements) ? data.textElements : [];
        this.lineSegments = editable && Array.isArray(data.lineSegments) ? data.lineSegments : [];
        this.todoItems = editable && Array.isArray(data.todoItems) ? data.todoItems : [];
        this.assignRenderOrders();
        this.applyScheduleSnapshot(data.schedule || (editable ? { scheduleData: data.scheduleData || null } : { scheduleData: null }));
        this.cancelTextPlacement();
        this.syncUI();
        this.redrawAll();
        if (typeof restoreCanvasOrientation === 'function') restoreCanvasOrientation(data.orientation || 0);
        this.historyStack = []; this.historyStep = -1;
        this.saveToHistory();
        return true;
      };
      const imageUrl = data.version === 2 ? data.baseDataUrl : data.imageDataUrl;
      if (imageUrl) {
        return new Promise(resolve => {
          const image = new Image();
          image.onload = () => {
            if (revision !== this.workspaceRevision) { resolve(false); return; }
            try {
              const baseCanvas = document.createElement('canvas');
              baseCanvas.width = data.width; baseCanvas.height = data.height;
              const baseContext = baseCanvas.getContext('2d');
              baseContext.drawImage(image, 0, 0, data.width, data.height);
              this.baseImageData = baseContext.getImageData(0, 0, data.width, data.height);
              this.baseImageUrl = imageUrl;
              resolve(restore());
            } catch (error) { resolve(false); }
          };
          image.onerror = () => resolve(false);
          image.src = imageUrl;
        });
      }
      if (data.imageData) {
        const pixels = this.ctx.createImageData(this.canvas.width, this.canvas.height);
        pixels.data.set(data.imageData.split(',').map(Number));
        this.baseImageData = pixels;
      }
      return restore();
    } catch (error) { return false; }
  }

  clearCanvasCache() {
    try { localStorage.removeItem('canvasState'); } catch (error) { /* Storage can be disabled. */ }
  }

  undo() {
    if (this.historyStep > 0) { this.historyStep--; this.restoreFromHistory(); }
  }

  redo() {
    if (this.historyStep < this.historyStack.length - 1) { this.historyStep++; this.restoreFromHistory(); }
  }

  restoreFromHistory() {
    const state = this.historyStack[this.historyStep];
    if (!state) return;
    this.workspaceRevision++;
    this.canvas.width = state.width;
    this.canvas.height = state.height;
    this.baseImageData = state.baseImageData;
    this.baseImageUrl = null;
    this.textElements = JSON.parse(JSON.stringify(state.textElements));
    this.lineSegments = JSON.parse(JSON.stringify(state.lineSegments));
    this.todoItems = JSON.parse(JSON.stringify(state.todoItems));
    this.assignRenderOrders();
    this.cancelTextPlacement();
    this.applyScheduleSnapshot(state.schedule);
    this.syncUI();
    this.redrawAll();
    this.updateUndoRedoButtons();
    if (typeof restoreCanvasOrientation === 'function') restoreCanvasOrientation(state.orientation || 0);
    else if (typeof updateOrientationBadge === 'function') updateOrientationBadge();
    this.saveCanvasToLocalStorage();
  }

  updateUndoRedoButtons() {
    const undo = this.element('undo-btn');
    const redo = this.element('redo-btn');
    if (undo) undo.disabled = this.historyStep <= 0;
    if (redo) redo.disabled = this.historyStep >= this.historyStack.length - 1;
  }

  syncUI() {
    this.renderTodoList();
    this.updateTextEditingUI();
    if (typeof this.syncScheduleUI === 'function') this.syncScheduleUI();
    this.updateBrushPreview();
  }

  initPaintTools() {
    const bind = (id, event, listener) => {
      const element = this.element(id);
      if (element) element.addEventListener(event, listener);
    };
    ['brush', 'eraser', 'text', 'todo'].forEach(tool => {
      bind(`${tool}-mode`, 'click', () => this.setActiveTool(this.currentTool === tool ? null : tool));
    });
    bind('brush-color', 'input', e => { this.brushColor = e.target.value; this.updateBrushPreview(); });
    ['brush', 'eraser'].forEach(tool => {
      bind(`${tool}-size`, 'input', () => {
        this[`${tool}Size`] = this.number(this.value(`${tool}-size`), 1, 100, tool === 'brush' ? 2 : 16);
        this.updateBrushPreview();
      });
      bind(`${tool}-size`, 'change', () => {
        this[`${tool}Size`] = this.inputNumber(`${tool}-size`, 1, 100, tool === 'brush' ? 2 : 16);
        this.updateBrushPreview();
      });
    });
    document.querySelectorAll('[data-brush-size]').forEach(button => {
      button.addEventListener('click', () => {
        const tool = this.currentTool === 'eraser' ? 'eraser' : 'brush';
        this[`${tool}Size`] = this.number(button.dataset.brushSize, 1, 100, 2);
        if (this.element(`${tool}-size`)) this.element(`${tool}-size`).value = this[`${tool}Size`];
        this.updateBrushPreview();
      });
    });
    ['text', 'todo'].forEach(type => {
      ['bold', 'italic'].forEach(style => {
        bind(`${type}-${style}`, 'click', () => {
          const property = type + style[0].toUpperCase() + style.slice(1);
          this[property] = !this[property];
          this.updateToggle(`${type}-${style}`, this[property]);
        });
      });
    });
    bind('todo-color', 'input', e => { this.todoColor = e.target.value; });
    bind('add-text-btn', 'click', () => this.startTextPlacement());
    bind('cancel-text-btn', 'click', () => { this.cancelTextPlacement(); this.updateTextEditingUI(); this.report('已取消文字编辑。'); });
    bind('delete-text-btn', 'click', () => this.deleteSelectedText());
    bind('add-todo-btn', 'click', () => this.startTodoPlacement());
    bind('cancel-todo-btn', 'click', () => { this.cancelTodoPlacement(); this.report('已取消待办编辑。'); });
    bind('todo-align-btn', 'click', () => this.alignTodoItems());
    bind('clear-completed-todo-btn', 'click', () => this.clearCompletedTodos());
    bind('undo-btn', 'click', () => this.undo());
    bind('redo-btn', 'click', () => this.redo());
    if (typeof this.initScheduleTools === 'function') this.initScheduleTools();
    this.canvas.addEventListener('mousedown', this.startPaint);
    this.canvas.addEventListener('mousemove', this.paint);
    this.canvas.addEventListener('mouseup', this.endPaint);
    this.canvas.addEventListener('mouseleave', this.endPaint);
    this.canvas.addEventListener('click', this.handleCanvasClick);
    this.canvas.addEventListener('touchstart', this.onTouchStart, { passive: false });
    this.canvas.addEventListener('touchmove', this.onTouchMove, { passive: false });
    this.canvas.addEventListener('touchend', this.onTouchEnd, { passive: false });
    this.canvas.addEventListener('touchcancel', this.onTouchEnd, { passive: false });
    document.addEventListener('keydown', this.handleKeyboard);
    this.canvas.addEventListener('mouseenter', this.updateBrushCursor);
    this.canvas.addEventListener('mousemove', this.updateBrushCursor);
    this.createBrushCursor();
    this.createSelectionOverlay();
    this.setActiveTool(null);
    this.syncUI();
    this.saveToHistory(false);
    const initialRevision = this.workspaceRevision;
    const restoreLegacySchedule = async success => {
      if (success || this.workspaceRevision !== initialRevision) return;
      if (typeof this.loadScheduleFromLocalStorage === 'function') {
        const restoredSchedule = await this.loadScheduleFromLocalStorage();
        if (restoredSchedule) {
          this.syncUI(); this.redrawAll();
          this.historyStack = []; this.historyStep = -1;
          this.saveToHistory();
        }
      }
    };
    // Restore once at startup; changing tools never reloads or erases the workspace.
    const restored = this.loadCanvasFromLocalStorage();
    if (restored && typeof restored.then === 'function') {
      restored.then(restoreLegacySchedule);
    } else if (!restored) restoreLegacySchedule(false);
  }

  handleKeyboard(e) {
    const target = e.target;
    if (target && (target.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName))) return;
    const key = String(e.key).toLowerCase();
    if ((e.ctrlKey || e.metaKey) && key === 'z' && !e.shiftKey) {
      e.preventDefault(); this.undo();
    } else if ((e.ctrlKey || e.metaKey) && (key === 'y' || (key === 'z' && e.shiftKey))) {
      e.preventDefault(); this.redo();
    } else if (key === 'escape') {
      this.cancelTextPlacement(); this.updateTextEditingUI();
      if (this.selectedScheduleCell && typeof this.cancelScheduleInput === 'function') this.cancelScheduleInput();
      this.report('已取消当前选择。');
    }
  }

  updateToggle(id, active) {
    const button = this.element(id);
    if (button) { button.classList.toggle('primary', active); button.setAttribute('aria-pressed', String(active)); }
  }

  setActiveTool(tool, title) {
    this.endPaint();
    this.cancelTextPlacement();
    this.currentTool = tool;
    const hints = {
      brush: '按住并拖动绘画；轻点画布可画圆点。',
      eraser: '按住并拖动擦为白色，画笔颜色和粗细会保留。',
      text: '输入文字后点击画布放置；点击已有文字可编辑，拖动可调整位置。',
      todo: '每行输入一项，点击画布放置整组；点击方框切换完成状态，拖动文字调整位置。',
      schedule: '创建课表后点击单元格编辑，可批量填入课程。'
    };
    ['brush', 'eraser', 'text', 'todo', 'schedule'].forEach(name => {
      this.canvas.parentNode.classList.toggle(`${name}-mode`, tool === name);
      const button = this.element(`${name}-mode`);
      if (button) { button.classList.toggle('active', tool === name); button.setAttribute('aria-pressed', String(tool === name)); }
    });
    const hint = this.element('paint-tool-hint');
    if (hint) hint.textContent = hints[tool] || '选择工具，为图片添加笔迹、文字、待办或课表。';
    if (typeof setCanvasTitle === 'function') setCanvasTitle(title || ({ brush: '画笔', eraser: '橡皮擦', text: '添加文字', todo: '待办清单', schedule: '课程表' }[tool] || ''));
    ['undo-btn', 'redo-btn'].forEach(id => this.element(id)?.classList.remove('hide'));
    this.updateTextEditingUI();
    this.renderTodoList();
    this.updateBrushPreview();
    this.updateSelectionOverlay();
    this.report('');
  }

  createBrushCursor() {
    this.brushCursor = document.createElement('div');
    this.brushCursor.id = 'brush-cursor';
    Object.assign(this.brushCursor.style, { position: 'fixed', border: '1px solid white', borderRadius: '50%',
      pointerEvents: 'none', display: 'none', zIndex: '10000', transform: 'translate(-50%, -50%)',
      boxShadow: '0 0 0 1px black, inset 0 0 0 1px black', boxSizing: 'border-box' });
    document.body.appendChild(this.brushCursor);
    this.updateBrushCursorSize();
  }

  updateBrushPreview() {
    this.updateBrushCursorSize();
    const preview = this.element('brush-preview');
    if (preview) {
      const size = this.currentTool === 'eraser' ? this.eraserSize : this.brushSize;
      preview.style.setProperty('--brush-size', `${Math.min(size, 42)}px`);
      preview.style.setProperty('--brush-color', this.currentTool === 'eraser' ? '#ffffff' : this.brushColor);
      preview.setAttribute('aria-label', `${this.currentTool === 'eraser' ? '橡皮' : '画笔'}直径 ${size} 像素`);
    }
    document.querySelectorAll('[data-brush-size]').forEach(button => {
      const active = Number(button.dataset.brushSize) === (this.currentTool === 'eraser' ? this.eraserSize : this.brushSize);
      button.classList.toggle('active', active); button.setAttribute('aria-pressed', String(active));
    });
  }

  updateBrushCursorSize() {
    if (!this.brushCursor) return;
    const rect = this.canvas.getBoundingClientRect();
    const size = (this.currentTool === 'eraser' ? this.eraserSize : this.brushSize) * Math.min(rect.width / this.canvas.width, rect.height / this.canvas.height);
    this.brushCursor.style.width = `${Math.max(1, size)}px`;
    this.brushCursor.style.height = `${Math.max(1, size)}px`;
  }

  updateBrushCursor(e) {
    if (!this.brushCursor || this.touchActive || this.isCropMode() || !['brush', 'eraser'].includes(this.currentTool)) { this.hideBrushCursor(); return; }
    this.updateBrushCursorSize();
    this.brushCursor.style.display = 'block';
    this.brushCursor.style.left = `${e.clientX}px`;
    this.brushCursor.style.top = `${e.clientY}px`;
    this.canvas.style.cursor = 'none';
  }

  hideBrushCursor() {
    if (this.brushCursor) this.brushCursor.style.display = 'none';
    this.canvas.style.cursor = '';
  }

  createSelectionOverlay() {
    this.selectionOverlay = document.createElement('div');
    this.selectionOverlay.id = 'paint-selection';
    this.selectionOverlay.setAttribute('aria-hidden', 'true');
    Object.assign(this.selectionOverlay.style, { position: 'absolute', border: '1px dashed #2563eb', pointerEvents: 'none', display: 'none', zIndex: '2', boxSizing: 'border-box' });
    this.canvas.parentNode.appendChild(this.selectionOverlay);
  }

  updateSelectionOverlay() {
    if (!this.selectionOverlay) return;
    const selected = this.currentTool === 'text' ? this.selectedEditingText : this.currentTool === 'todo' ? this.selectedTodoItem : null;
    if (!selected || this.isCropMode()) { this.selectionOverlay.style.display = 'none'; return; }
    const bounds = this.getTextBounds(selected, selected.text, this.currentTool === 'todo');
    const rect = this.canvas.getBoundingClientRect();
    const parent = this.canvas.parentNode.getBoundingClientRect();
    const scaleX = rect.width / this.canvas.width;
    const scaleY = rect.height / this.canvas.height;
    Object.assign(this.selectionOverlay.style, { display: 'block',
      left: `${rect.left - parent.left + bounds.minX * scaleX - 3}px`,
      top: `${rect.top - parent.top + bounds.minY * scaleY - 3}px`,
      width: `${(bounds.maxX - bounds.minX) * scaleX + 6}px`,
      height: `${(bounds.maxY - bounds.minY) * scaleY + 6}px` });
  }

  prepareExport() {
    this.endPaint();
    this.redrawAll();
    if (this.selectionOverlay) this.selectionOverlay.style.display = 'none';
    if (typeof this.hideScheduleSelection === 'function') this.hideScheduleSelection();
  }

  canvasPoint(e) {
    const rect = this.canvas.getBoundingClientRect();
    return { x: this.number((e.clientX - rect.left) * this.canvas.width / rect.width, 0, this.canvas.width - 1, 0),
      y: this.number((e.clientY - rect.top) * this.canvas.height / rect.height, 0, this.canvas.height - 1, 0) };
  }

  startPaint(e) {
    if (this.isCropMode() || (!this.touchActive && Date.now() < this.suppressMouseUntil) || !this.currentTool || (e.button !== undefined && e.button !== 0)) return;
    if (this.isTextPlacementMode || this.isTodoPlacementMode) return;
    if (['brush', 'eraser'].includes(this.currentTool)) {
      this.workspaceRevision++;
      this.painting = true; this.lastX = null; this.lastY = null; this.draw(e); return;
    }
    if (this.currentTool === 'schedule') { this.scheduleDragStart?.(e); return; }
    const item = this.currentTool === 'text' ? this.findTextElementAt(e) : this.findTodoItemAt(e);
    if (!item) return;
    if (this.currentTool === 'todo' && this.isTodoCheckboxAt(item, e)) {
      this.toggleTodo(item); this.skipNextCanvasClick = true; return;
    }
    if (this.currentTool === 'text') this.selectText(item);
    else { this.selectedTodoItem = item; this.renderTodoList(); }
    const point = this.canvasPoint(e);
    this.selectedTextElement = this.currentTool === 'text' ? item : null;
    this.dragOffsetX = item.x - point.x; this.dragOffsetY = item.y - point.y;
    this.isDraggingText = true; this.dragChanged = false;
    this.workspaceRevision++;
    this.updateSelectionOverlay();
  }

  endPaint() {
    if (this.currentTool === 'schedule') this.scheduleDragEnd?.();
    if (this.painting || (this.isDraggingText && this.dragChanged)) {
      this.redrawAll(); this.saveToHistory();
    }
    this.painting = false; this.isDraggingText = false;
    this.lastX = null; this.lastY = null;
    this.hideBrushCursor();
  }

  paint(e) {
    if (this.isCropMode() || (!this.touchActive && Date.now() < this.suppressMouseUntil)) return;
    if (this.painting) this.draw(e);
    else if (this.isDraggingText && this.currentTool === 'text') this.dragText(e);
    else if (this.isDraggingText && this.currentTool === 'todo') this.dragTodo(e);
    else if (this.currentTool === 'schedule') this.scheduleDragMove?.(e);
  }

  draw(e) {
    const { x, y } = this.canvasPoint(e);
    const color = this.currentTool === 'eraser' ? '#FFFFFF' : this.brushColor;
    const size = this.currentTool === 'eraser' ? this.eraserSize : this.brushSize;
    const segment = this.lastX === null ? { type: 'dot', x, y, color, size }
      : { type: 'line', x1: this.lastX, y1: this.lastY, x2: x, y2: y, color, size };
    this.lineSegments.push(segment);
    segment.renderOrder = ++this.renderOrder;
    this.drawSegment(segment);
    this.lastX = x; this.lastY = y;
  }

  drawSegment(segment) {
    this.ctx.save();
    this.ctx.lineJoin = 'round'; this.ctx.lineCap = 'round';
    this.ctx.strokeStyle = segment.color; this.ctx.fillStyle = segment.color; this.ctx.lineWidth = segment.size;
    this.ctx.beginPath();
    if (segment.type === 'dot') {
      this.ctx.arc(segment.x, segment.y, segment.size / 2, 0, Math.PI * 2); this.ctx.fill();
    } else { this.ctx.moveTo(segment.x1, segment.y1); this.ctx.lineTo(segment.x2, segment.y2); this.ctx.stroke(); }
    this.ctx.restore();
  }

  handleCanvasClick(e) {
    if (this.isCropMode() || (!this.touchActive && Date.now() < this.suppressMouseUntil)) return;
    if (this.skipNextCanvasClick) { this.skipNextCanvasClick = false; return; }
    if (this.currentTool === 'text' && this.isTextPlacementMode) this.placeText(e);
    else if (this.currentTool === 'todo' && this.isTodoPlacementMode) this.placeTodo(e);
    else if (this.currentTool === 'text' && this.selectedEditingText && !this.findTextElementAt(e)) this.deselectEditingText();
    else if (this.currentTool === 'schedule') {
      const cell = this.getScheduleCellAt(e);
      if (typeof this.selectScheduleCell === 'function') this.selectScheduleCell(cell);
    }
  }

  onTouchStart(e) {
    if (!this.currentTool || this.isCropMode() || e.touches.length !== 1) return;
    e.preventDefault(); this.touchActive = true; this.suppressMouseUntil = Date.now() + 800;
    const point = e.touches[0];
    if (this.isTextPlacementMode || this.isTodoPlacementMode || this.currentTool === 'schedule') this.handleCanvasClick(point);
    else this.startPaint(point);
  }

  onTouchMove(e) {
    if (!this.touchActive || !e.touches.length) return;
    e.preventDefault(); this.paint(e.touches[0]);
  }

  onTouchEnd(e) {
    if (!this.touchActive) return;
    e.preventDefault(); this.endPaint(); this.touchActive = false;
    this.suppressMouseUntil = Date.now() + 800;
    this.skipNextCanvasClick = false;
  }

  dragItem(item, e) {
    if (!item) return;
    const { x, y } = this.canvasPoint(e);
    const nextX = this.number(x + this.dragOffsetX, 0, this.canvas.width - 1, item.x);
    const nextY = this.number(y + this.dragOffsetY, 0, this.canvas.height - 1, item.y);
    if (item.x === nextX && item.y === nextY) return;
    item.x = nextX; item.y = nextY; this.dragChanged = true;
    this.redrawAll();
  }

  dragText(e) { this.dragItem(this.selectedTextElement, e); }
  dragTodo(e) { this.dragItem(this.selectedTodoItem, e); }

  getItemTransformMatrix(item) {
    return { a: Number.isFinite(item?.a) ? item.a : 1, b: Number.isFinite(item?.b) ? item.b : 0,
      c: Number.isFinite(item?.c) ? item.c : 0, d: Number.isFinite(item?.d) ? item.d : 1 };
  }

  transformLocalPoint(item, x, y) {
    const m = this.getItemTransformMatrix(item);
    return { x: item.x + m.a * x + m.c * y, y: item.y + m.b * x + m.d * y };
  }

  localPoint(item, point) {
    const m = this.getItemTransformMatrix(item);
    const determinant = m.a * m.d - m.b * m.c;
    if (Math.abs(determinant) < 0.00001) return { x: Infinity, y: Infinity };
    const x = point.x - item.x, y = point.y - item.y;
    return { x: (m.d * x - m.c * y) / determinant, y: (-m.b * x + m.a * y) / determinant };
  }

  textLayout(item, text = item.text, todo = false) {
    this.ctx.font = item.font;
    const size = Number(item.font.match(/([\d.]+)px/)?.[1]) || 16;
    const lines = String(text || '').split('\n');
    const width = Math.max(0, ...lines.map(line => this.ctx.measureText(line).width));
    const lineHeight = size * this.number(item.lineHeight, 1, 3, 1.35);
    const offset = todo ? size * 1.4 : 0;
    const left = todo ? 0 : item.align === 'center' ? -width / 2 : item.align === 'right' ? -width : 0;
    return { lines, size, width, lineHeight, offset, left, right: left + width + offset,
      top: -size, bottom: Math.max(size * 0.25, (lines.length - 1) * lineHeight + size * 0.25) };
  }

  getTextBounds(item, text, todo = false) {
    const layout = this.textLayout(item, text, todo);
    const corners = [[layout.left, layout.top], [layout.right, layout.top], [layout.right, layout.bottom], [layout.left, layout.bottom]]
      .map(([x, y]) => this.transformLocalPoint(item, x, y));
    return { minX: Math.min(...corners.map(p => p.x)), maxX: Math.max(...corners.map(p => p.x)),
      minY: Math.min(...corners.map(p => p.y)), maxY: Math.max(...corners.map(p => p.y)), textWidth: layout.width };
  }

  findItemAt(items, e, todo = false) {
    const point = this.canvasPoint(e);
    for (let index = items.length - 1; index >= 0; index--) {
      const item = items[index], local = this.localPoint(item, point), bounds = this.textLayout(item, item.text, todo);
      if (local.x >= bounds.left - 4 && local.x <= bounds.right + 4 && local.y >= bounds.top - 4 && local.y <= bounds.bottom + 4) return item;
    }
    return null;
  }

  findTextElementAt(e) { return this.findItemAt(this.textElements, e); }
  findTodoItemAt(e) { return this.findItemAt(this.todoItems, e, true); }
  findTodoDeleteButtonAt() { return null; }

  isTodoCheckboxAt(item, e) {
    const point = this.localPoint(item, this.canvasPoint(e));
    const size = this.textLayout(item).size;
    return point.x >= -3 && point.x <= size + 3 && point.y >= -size - 3 && point.y <= size * 0.15 + 3;
  }

  redrawAll() {
    this.drawBaseImage();
    this.drawSchedule();
    const layers = [...this.lineSegments.map(item => ({ item, draw: this.drawSegment })),
      ...this.textElements.map(item => ({ item, draw: this.drawTextItem })),
      ...this.todoItems.map(item => ({ item, draw: this.drawTodoItem }))];
    layers.sort((a, b) => (a.item.renderOrder || 0) - (b.item.renderOrder || 0));
    layers.forEach(({ item, draw }) => draw.call(this, item));
    this.updateSelectionOverlay();
    if (typeof this.updateScheduleSelection === 'function') this.updateScheduleSelection();
  }

  textConfiguration() {
    return { font: `${this.textItalic ? 'italic ' : ''}${this.textBold ? 'bold ' : ''}${this.inputNumber('font-size', 6, 100, 20)}px ${this.value('font-family', 'Microsoft YaHei')}`,
      color: this.value('text-color', this.brushColor), align: this.value('text-align', 'left'),
      lineHeight: this.inputNumber('text-line-height', 1, 3, 1.35) };
  }

  startTextPlacement() {
    if (this.selectedEditingText) { this.updateSelectedText(); return; }
    if (!this.value('text-input').trim()) { this.report('请输入文字内容，可使用换行排版。', true); this.element('text-input')?.focus(); return; }
    if (this.currentTool !== 'text') this.setActiveTool('text');
    this.textConfiguration();
    this.workspaceRevision++;
    this.isTextPlacementMode = true;
    this.canvas.classList.add('text-placement-mode');
    this.report('点击画布放置文字；取消可返回编辑。');
  }

  cancelTextPlacement() {
    this.isTextPlacementMode = false;
    this.isTodoPlacementMode = false;
    this.isDraggingText = false;
    this.selectedTextElement = null;
    this.selectedEditingText = null;
    this.selectedTodoItem = null;
    this.todoEditing = false;
    const addTodo = this.element('add-todo-btn');
    if (addTodo) addTodo.textContent = '添加待办';
    this.canvas.classList.remove('text-placement-mode');
    this.updateSelectionOverlay();
  }

  updateTextEditingUI() {
    const add = this.element('add-text-btn');
    if (add) add.textContent = this.selectedEditingText ? '更新文字' : '添加文字';
    const remove = this.element('delete-text-btn');
    if (remove) remove.disabled = !this.selectedEditingText;
    this.updateToggle('text-bold', this.textBold); this.updateToggle('text-italic', this.textItalic);
  }

  selectText(item) {
    this.selectedEditingText = item;
    if (this.element('text-input')) this.element('text-input').value = item.text;
    const match = item.font.match(/([\d.]+)px\s+(.*)/);
    if (match) {
      if (this.element('font-size')) this.element('font-size').value = match[1];
      this.setFontInput('font-family', match[2]);
    }
    this.textBold = /bold/.test(item.font); this.textItalic = /italic/.test(item.font);
    ['text-color', 'text-align', 'text-line-height'].forEach(id => {
      const input = this.element(id);
      if (input) input.value = id === 'text-color' ? item.color : id === 'text-align' ? item.align || 'left' : item.lineHeight || 1.35;
    });
    this.updateTextEditingUI(); this.updateSelectionOverlay();
    this.report('已选中文字，可修改内容或拖动位置。');
  }

  updateSelectedText() {
    if (!this.selectedEditingText) return;
    if (!this.value('text-input').trim()) { this.report('文字内容不能为空；需要移除时请点击删除。', true); return; }
    Object.assign(this.selectedEditingText, this.textConfiguration(), { text: this.value('text-input') });
    this.selectedEditingText.renderOrder = ++this.renderOrder;
    this.redrawAll(); this.saveToHistory(); this.report('文字已更新。');
  }

  deleteSelectedText() {
    const index = this.textElements.indexOf(this.selectedEditingText);
    if (index < 0) return;
    this.textElements.splice(index, 1); this.cancelTextPlacement(); this.updateTextEditingUI();
    if (this.element('text-input')) this.element('text-input').value = '';
    this.redrawAll(); this.saveToHistory(); this.report('已删除选中文字。');
  }

  deselectEditingText() {
    this.selectedEditingText = null; this.selectedTextElement = null;
    this.updateTextEditingUI(); this.updateSelectionOverlay();
  }

  placeText(e) {
    const text = this.value('text-input');
    if (!text.trim()) { this.cancelTextPlacement(); this.report('请输入文字内容。', true); return; }
    const point = this.canvasPoint(e), configuration = this.textConfiguration();
    const size = Number(configuration.font.match(/([\d.]+)px/)?.[1]) || 24;
    const item = { text, x: point.x, y: Math.max(size, point.y), ...configuration, a: 1, b: 0, c: 0, d: 1, renderOrder: ++this.renderOrder };
    const layout = this.textLayout(item);
    if (layout.width > this.canvas.width - 4 || layout.bottom - layout.top > this.canvas.height - 4) {
      this.report('文字超出画布，请减小字号、行距或使用换行排版。', true); return;
    }
    item.x = Math.max(2 - layout.left, Math.min(item.x, this.canvas.width - 2 - layout.right));
    item.y = Math.max(2 - layout.top, Math.min(item.y, this.canvas.height - 2 - layout.bottom));
    this.textElements.push(item);
    this.isTextPlacementMode = false; this.canvas.classList.remove('text-placement-mode');
    this.selectText(item); this.redrawAll(); this.saveToHistory();
    this.report('文字已放置，可拖动或继续编辑。');
  }

  drawTextItem(item) {
    const m = this.getItemTransformMatrix(item), layout = this.textLayout(item);
    this.ctx.save(); this.ctx.translate(item.x, item.y); this.ctx.transform(m.a, m.b, m.c, m.d, 0, 0);
    this.ctx.font = item.font; this.ctx.fillStyle = item.color;
    this.ctx.textAlign = ['left', 'center', 'right'].includes(item.align) ? item.align : 'left';
    this.ctx.textBaseline = 'alphabetic';
    layout.lines.forEach((line, index) => this.ctx.fillText(line, 0, index * layout.lineHeight));
    this.ctx.restore();
  }

  redrawTextElements() { this.textElements.forEach(item => this.drawTextItem(item)); }
  redrawLineSegments() { this.lineSegments.forEach(segment => this.drawSegment(segment)); }

  todoConfiguration() {
    return { font: `${this.todoItalic ? 'italic ' : ''}${this.todoBold ? 'bold ' : ''}${this.inputNumber('todo-font-size', 6, 100, 18)}px ${this.value('todo-font-family', 'Microsoft YaHei')}`,
      color: this.value('todo-color', this.todoColor), lineHeight: 1.35 };
  }

  startTodoPlacement() {
    const lines = this.value('todo-input').split(/\r?\n/).map(line => line.trim()).filter(Boolean);
    if (!lines.length) { this.report('请输入待办内容，每行一项。', true); this.element('todo-input')?.focus(); return; }
    if (this.selectedTodoItem && this.todoEditing) {
      if (lines.length > 1) { this.report('编辑单项待办时请保留一行；取消编辑后可批量添加。', true); return; }
      Object.assign(this.selectedTodoItem, this.todoConfiguration(), { text: lines[0] });
      this.selectedTodoItem.renderOrder = ++this.renderOrder;
      this.redrawAll(); this.saveToHistory(); this.cancelTodoPlacement(); this.report('待办已更新。'); return;
    }
    if (this.currentTool !== 'todo') this.setActiveTool('todo');
    this.todoConfiguration();
    this.workspaceRevision++;
    this.isTodoPlacementMode = true; this.canvas.classList.add('text-placement-mode');
    this.report(`点击画布放置 ${lines.length} 项待办；每项方框可切换完成状态。`);
  }

  placeTodo(e) {
    const lines = this.value('todo-input').split(/\r?\n/).map(line => line.trim()).filter(Boolean);
    if (!lines.length) { this.cancelTodoPlacement(); return; }
    const configuration = this.todoConfiguration(), point = this.canvasPoint(e);
    const size = Number(configuration.font.match(/([\d.]+)px/)?.[1]) || 20;
    this.ctx.font = configuration.font;
    const width = Math.max(...lines.map(text => this.ctx.measureText(text).width)) + size * 1.4;
    if (width > this.canvas.width - 4) {
      this.report('待办内容超出画布宽度，请减小字号或缩短单项内容。', true); return;
    }
    point.x = Math.min(point.x, this.canvas.width - width - 2);
    const spacing = size * this.inputNumber('todo-spacing', 1.2, 3, 1.6);
    if (Math.max(size, point.y) + (lines.length - 1) * spacing + size * 0.25 > this.canvas.height) {
      this.report('剩余高度不足以放下清单，请减小字号、行距或选择更靠上的位置。', true); return;
    }
    const items = lines.map((text, index) => ({ text, x: point.x, y: Math.max(size, point.y) + index * spacing,
      ...configuration, completed: false, a: 1, b: 0, c: 0, d: 1, renderOrder: ++this.renderOrder }));
    this.todoItems.push(...items);
    this.cancelTodoPlacement();
    if (this.element('todo-input')) this.element('todo-input').value = '';
    this.redrawAll(); this.saveToHistory(); this.report(`已添加 ${items.length} 项待办。`);
  }

  cancelTodoPlacement() {
    this.isTodoPlacementMode = false; this.todoEditing = false; this.selectedTodoItem = null;
    this.canvas.classList.remove('text-placement-mode');
    const add = this.element('add-todo-btn'); if (add) add.textContent = '添加待办';
    this.updateSelectionOverlay(); this.renderTodoList();
  }

  editTodoItem(item) {
    if (this.currentTool !== 'todo') this.setActiveTool('todo');
    this.selectedTodoItem = item; this.todoEditing = true;
    const match = item.font.match(/([\d.]+)px\s+(.*)/);
    if (this.element('todo-input')) this.element('todo-input').value = item.text;
    if (match) {
      if (this.element('todo-font-size')) this.element('todo-font-size').value = match[1];
      this.setFontInput('todo-font-family', match[2]);
    }
    this.todoBold = /bold/.test(item.font); this.todoItalic = /italic/.test(item.font);
    this.todoColor = item.color;
    if (this.element('todo-color')) this.element('todo-color').value = item.color;
    this.updateToggle('todo-bold', this.todoBold); this.updateToggle('todo-italic', this.todoItalic);
    if (this.element('add-todo-btn')) this.element('add-todo-btn').textContent = '更新待办';
    this.element('todo-input')?.focus(); this.updateSelectionOverlay(); this.renderTodoList();
    this.report('已选中待办，可修改内容和样式。');
  }

  toggleTodo(item) {
    item.completed = !item.completed; this.redrawAll(); this.saveToHistory();
  }

  deleteTodoItem(item) {
    const index = this.todoItems.indexOf(item); if (index < 0) return;
    this.todoItems.splice(index, 1);
    if (this.selectedTodoItem === item) this.cancelTodoPlacement();
    this.redrawAll(); this.saveToHistory(); this.report('已删除待办。');
  }

  moveTodoItem(item, delta) {
    const index = this.todoItems.indexOf(item), target = index + delta;
    if (index < 0 || target < 0 || target >= this.todoItems.length) return;
    const other = this.todoItems[target];
    [item.x, other.x] = [other.x, item.x]; [item.y, other.y] = [other.y, item.y];
    [this.todoItems[index], this.todoItems[target]] = [other, item];
    this.redrawAll(); this.saveToHistory();
  }

  clearCompletedTodos() {
    const count = this.todoItems.filter(item => item.completed).length;
    if (!count) { this.report('没有已完成的待办。'); return; }
    this.todoItems = this.todoItems.filter(item => !item.completed);
    if (this.selectedTodoItem?.completed) this.cancelTodoPlacement();
    this.redrawAll(); this.saveToHistory(); this.report(`已清理 ${count} 项已完成待办。`);
  }

  alignTodoItems() {
    if (!this.todoItems.length) return;
    const first = this.todoItems[0], spacing = this.inputNumber('todo-spacing', 1.2, 3, 1.6);
    const sizes = this.todoItems.map(item => this.textLayout(item).size);
    const startY = Math.max(first.y, sizes[0]);
    const height = sizes.slice(0, -1).reduce((sum, size, index) => sum + Math.max(size, sizes[index + 1]) * spacing, 0) + sizes.at(-1) * 0.25;
    if (startY + height > this.canvas.height) { this.report('画布高度不足，请减小行距或将第一项向上移动。', true); return; }
    let y = startY;
    this.todoItems.forEach((item, index) => {
      item.x = first.x; item.y = y; item.a = 1; item.b = 0; item.c = 0; item.d = 1;
      y += Math.max(sizes[index], sizes[index + 1] || sizes[index]) * spacing;
    });
    this.redrawAll(); this.saveToHistory(); this.report('待办已按当前顺序对齐。');
  }

  renderTodoList() {
    const summary = this.element('todo-summary'), list = this.element('todo-list');
    const completed = this.todoItems.filter(item => item.completed).length;
    if (summary) summary.textContent = this.todoItems.length ? `${this.todoItems.length} 项 · 已完成 ${completed} 项` : '0 项待办';
    const clear = this.element('clear-completed-todo-btn'); if (clear) clear.disabled = !completed;
    const align = this.element('todo-align-btn'); if (align) align.disabled = this.todoItems.length < 2;
    if (!list) return;
    const signature = JSON.stringify([this.todoItems.map(item => [item.text, item.completed]), this.todoItems.indexOf(this.selectedTodoItem)]);
    const sameReferences = this.renderedTodoItems && this.renderedTodoItems.length === this.todoItems.length &&
      this.todoItems.every((item, index) => item === this.renderedTodoItems[index]);
    if (signature === this.todoListSignature && sameReferences) return;
    this.todoListSignature = signature;
    this.renderedTodoItems = this.todoItems.slice();
    list.replaceChildren();
    if (!this.todoItems.length) {
      const empty = document.createElement('p'); empty.className = 'todo-editor-empty';
      empty.textContent = '每行输入一项，批量放置清单；也可在这里编辑、排序与勾选完成。';
      list.appendChild(empty);
    }
    this.todoItems.forEach((item, index) => {
      const row = document.createElement('div'); row.className = 'todo-editor-row';
      row.classList.toggle('completed', item.completed); row.classList.toggle('selected', item === this.selectedTodoItem);
      const checkbox = document.createElement('input'); checkbox.type = 'checkbox'; checkbox.checked = item.completed;
      checkbox.setAttribute('aria-label', `${item.completed ? '恢复' : '完成'}待办：${item.text}`);
      checkbox.addEventListener('change', () => this.toggleTodo(item));
      const content = document.createElement('input'); content.type = 'text'; content.value = item.text; content.className = 'todo-list-text';
      content.setAttribute('aria-label', `第 ${index + 1} 项待办内容`);
      content.addEventListener('change', () => {
        if (!content.value.trim()) { content.value = item.text; this.report('待办内容不能为空；可使用删除按钮移除。', true); return; }
        item.text = content.value.trim(); item.renderOrder = ++this.renderOrder;
        this.redrawAll();
        this.todoListSignature = JSON.stringify([this.todoItems.map(todo => [todo.text, todo.completed]), this.todoItems.indexOf(this.selectedTodoItem)]);
        // Keep this row in the DOM so the button clicked immediately after editing still receives its click.
        this.saveToHistory(true, false);
      });
      row.append(checkbox, content);
      const actions = document.createElement('div'); actions.className = 'todo-row-actions';
      [['上移', () => this.moveTodoItem(item, -1), index === 0], ['下移', () => this.moveTodoItem(item, 1), index === this.todoItems.length - 1],
        ['编辑', () => this.editTodoItem(item), false], ['删除', () => this.deleteTodoItem(item), false]].forEach(([label, action, disabled]) => {
        const button = document.createElement('button'); button.type = 'button'; button.className = 'secondary'; button.textContent = label;
        button.disabled = disabled; button.setAttribute('aria-label', `${label}第 ${index + 1} 项待办`);
        button.addEventListener('click', action); actions.appendChild(button);
      });
      row.appendChild(actions); list.appendChild(row);
    });
  }

  drawTodoItem(item) {
    const m = this.getItemTransformMatrix(item), layout = this.textLayout(item);
    const size = layout.size, box = size * 0.8, offset = size * 1.4;
    this.ctx.save(); this.ctx.translate(item.x, item.y); this.ctx.transform(m.a, m.b, m.c, m.d, 0, 0);
    this.ctx.strokeStyle = item.color; this.ctx.fillStyle = item.color; this.ctx.lineWidth = Math.max(1, size / 14);
    this.ctx.strokeRect(0, -box, box, box);
    if (item.completed) {
      this.ctx.beginPath(); this.ctx.moveTo(box * 0.2, -box * 0.5); this.ctx.lineTo(box * 0.42, -box * 0.22);
      this.ctx.lineTo(box * 0.82, -box * 0.8); this.ctx.stroke();
    }
    this.ctx.font = item.font; this.ctx.textAlign = 'left'; this.ctx.textBaseline = 'alphabetic';
    this.ctx.fillText(item.text, offset, 0);
    if (item.completed) {
      this.ctx.beginPath(); this.ctx.moveTo(offset, -size * 0.35); this.ctx.lineTo(offset + this.ctx.measureText(item.text).width, -size * 0.35); this.ctx.stroke();
    }
    this.ctx.restore();
  }

  redrawTodoItems() { this.todoItems.forEach(item => this.drawTodoItem(item)); }

  createSchedule() {
    // Get schedule configuration from form inputs
    this.scheduleDays = parseInt(document.getElementById('schedule-days').value);
    this.scheduleClasses = parseInt(document.getElementById('schedule-classes').value);
    this.scheduleFontFamily = document.getElementById('schedule-font-family').value;
    this.scheduleFontSize = parseInt(document.getElementById('schedule-font-size').value);
    this.scheduleColor = document.getElementById('schedule-color').value;

    // Clear old content (drawings, text, todos) to make room for new schedule
    this.lineSegments = [];
    this.textElements = [];
    this.todoItems = [];

    // Calculate cell dimensions based on canvas
    this.calculateScheduleDimensions();

    // Initialize schedule data (2D array: rows = classes + 1 (header), cols = days + 1 (time col))
    this.scheduleData = [];
    this.scheduleCellFontSizes = [];
    for (let i = 0; i <= this.scheduleClasses; i++) {
      this.scheduleData[i] = [];
      this.scheduleCellFontSizes[i] = [];
      for (let j = 0; j <= this.scheduleDays; j++) {
        this.scheduleCellFontSizes[i][j] = this.scheduleFontSize;
        if (i === 0 && j === 0) {
          this.scheduleData[i][j] = ''; // Top-left corner - leave empty
        } else if (i === 0) {
          this.scheduleData[i][j] = this.weekDays[j - 1]; // Header row - weekdays
        } else if (j === 0) {
          this.scheduleData[i][j] = `第${i}节`; // Time column
        } else {
          this.scheduleData[i][j] = ''; // Empty cells for courses
        }
      }
    }

    // Draw the schedule on canvas
    this.redrawAll();
    this.saveScheduleToLocalStorage(); // Save schedule to cache
    this.saveToHistory();
  }

  calculateScheduleDimensions() {
    // Calculate cell size based on canvas dimensions
    const padding = 20;
    const availableWidth = this.canvas.width - 2 * padding;
    const availableHeight = this.canvas.height - 2 * padding;

    // Calculate cell dimensions to fit content
    const cellWidth = Math.floor(availableWidth / (this.scheduleDays + 1)); // +1 for time column
    const cellHeight = Math.floor(availableHeight / (this.scheduleClasses + 1)); // +1 for header

    // Ensure cells are large enough for text
    this.scheduleCellWidth = Math.max(cellWidth, this.scheduleFontSize * 4);
    this.scheduleCellHeight = Math.max(cellHeight, this.scheduleFontSize * 2);

    // Adjust start position based on available space
    this.scheduleStartX = padding;
    this.scheduleStartY = padding;
  }

  drawSchedule() {
    if (!this.scheduleData) return;

    const cellWidth = this.scheduleCellWidth;
    const cellHeight = this.scheduleCellHeight;
    const startX = this.scheduleStartX;
    const startY = this.scheduleStartY;

    this.ctx.fillStyle = this.scheduleColor;
    this.ctx.strokeStyle = '#000000';
    this.ctx.lineWidth = 1;

    // Draw table grid and text
    for (let i = 0; i < this.scheduleData.length; i++) {
      for (let j = 0; j < this.scheduleData[i].length; j++) {
        const x = startX + j * cellWidth;
        const y = startY + i * cellHeight;

        // Draw cell border
        this.ctx.strokeRect(x, y, cellWidth, cellHeight);

        // Draw cell text with per-cell font size and multi-line support
        const text = this.scheduleData[i][j];
        if (text) {
          const cellFontSize = (this.scheduleCellFontSizes && this.scheduleCellFontSizes[i] && this.scheduleCellFontSizes[i][j])
            ? this.scheduleCellFontSizes[i][j] : this.scheduleFontSize;
          const font = `${cellFontSize}px ${this.scheduleFontFamily}`;
          this.ctx.font = font;
          this.ctx.fillStyle = this.scheduleColor;

          const lines = text.split('\n');
          const lineHeight = cellFontSize * 1.2;
          const totalTextHeight = lines.length * lineHeight;
          const textStartY = y + (cellHeight - totalTextHeight) / 2 + cellFontSize * 0.85;

          for (let l = 0; l < lines.length; l++) {
            const lineText = lines[l];
            const textX = x + (cellWidth - this.ctx.measureText(lineText).width) / 2;
            const textY = textStartY + l * lineHeight;
            this.ctx.fillText(lineText, textX, textY);
          }
        }
      }
    }

    // Draw selection indicator if enabled and a cell is selected
    if (this.showScheduleCellIndicator && this.selectedScheduleCell) {
      const row = this.selectedScheduleCell.row;
      const col = this.selectedScheduleCell.col;
      const x = startX + col * cellWidth;
      const y = startY + row * cellHeight;

      // Draw a small black dot in the top-right corner of selected cell
      this.ctx.fillStyle = '#000000';
      this.ctx.beginPath();
      this.ctx.arc(x + cellWidth - 5, y + 5, 3, 0, Math.PI * 2);
      this.ctx.fill();
    }
  }

  updateScheduleCell(row, col, text) {
    if (this.scheduleData && row >= 0 && col >= 0 && row <= this.scheduleClasses && col <= this.scheduleDays) {
      this.scheduleData[row][col] = text;
      this.redrawAll();
      this.saveScheduleToLocalStorage(); // Save schedule to cache
      this.saveToHistory();
    }
  }

  getScheduleCellAt(e) {
    if (!this.scheduleData) return null;

    const rect = this.canvas.getBoundingClientRect();
    const scaleX = this.canvas.width / rect.width;
    const scaleY = this.canvas.height / rect.height;
    const x = (e.clientX - rect.left) * scaleX;
    const y = (e.clientY - rect.top) * scaleY;

    const cellWidth = this.scheduleCellWidth;
    const cellHeight = this.scheduleCellHeight;
    const startX = this.scheduleStartX;
    const startY = this.scheduleStartY;

    // Calculate which cell was clicked
    const col = Math.floor((x - startX) / cellWidth);
    const row = Math.floor((y - startY) / cellHeight);

    // Check if click is within schedule bounds - allow all cells including header row and time column
    if (col >= 0 && row >= 0 && col <= this.scheduleDays && row <= this.scheduleClasses) {
      return { row, col };
    }

    return null;
  }

  confirmScheduleInput() {
    if (this.selectedScheduleCell) {
      const text = document.getElementById('schedule-input').value;
      this.updateScheduleCell(this.selectedScheduleCell.row, this.selectedScheduleCell.col, text);
      this.cancelScheduleInput();
    }
  }

  cancelScheduleInput() {
    this.selectedScheduleCell = null;
    document.getElementById('schedule-input').value = '';
    // Restore global font size display
    document.getElementById('schedule-font-size').value = this.scheduleFontSize;
    // Hide the input buttons
    const allScheduleTools = document.querySelectorAll('.schedule-tools');
    if (allScheduleTools.length > 2) {
      allScheduleTools[2].style.display = 'none';
    }
    // Redraw to remove the selection indicator
    if (this.scheduleData) {
      this.redrawAll();
    }
  }

  saveScheduleToLocalStorage() {
    try {
      const scheduleCache = {
        scheduleData: this.scheduleData,
        scheduleDays: this.scheduleDays,
        scheduleClasses: this.scheduleClasses,
        scheduleFontFamily: this.scheduleFontFamily,
        scheduleFontSize: this.scheduleFontSize,
        scheduleColor: this.scheduleColor,
        scheduleCellWidth: this.scheduleCellWidth,
        scheduleCellHeight: this.scheduleCellHeight,
        scheduleStartX: this.scheduleStartX,
        scheduleStartY: this.scheduleStartY,
        scheduleCellFontSizes: this.scheduleCellFontSizes
      };
      localStorage.setItem('scheduleCache', JSON.stringify(scheduleCache));
    } catch (e) {
      console.error('Failed to save schedule to localStorage:', e);
    }
  }

  async loadScheduleFromLocalStorage() {
    try {
      const savedData = localStorage.getItem('scheduleCache');
      if (!savedData) return false;

      const scheduleCache = JSON.parse(savedData);

      // Restore schedule configuration
      this.scheduleData = scheduleCache.scheduleData;
      this.scheduleDays = scheduleCache.scheduleDays;
      this.scheduleClasses = scheduleCache.scheduleClasses;
      this.scheduleFontFamily = scheduleCache.scheduleFontFamily;
      this.scheduleFontSize = scheduleCache.scheduleFontSize;
      this.scheduleColor = scheduleCache.scheduleColor;
      this.scheduleCellFontSizes = scheduleCache.scheduleCellFontSizes || null;

      // Initialize scheduleCellFontSizes if missing (old cache compatibility)
      if (!this.scheduleCellFontSizes && this.scheduleData) {
        this.scheduleCellFontSizes = [];
        for (let i = 0; i < this.scheduleData.length; i++) {
          this.scheduleCellFontSizes[i] = [];
          for (let j = 0; j < this.scheduleData[i].length; j++) {
            this.scheduleCellFontSizes[i][j] = this.scheduleFontSize;
          }
        }
      }

      // Recalculate dimensions based on current canvas size for proper adaptation
      this.calculateScheduleDimensions();

      if (this.scheduleData) {
        // Clear other elements to ensure a clean schedule view, matching createSchedule behavior
        this.lineSegments = [];
        this.textElements = [];
        this.todoItems = [];

        // Use redrawAll to clear canvas and draw the schedule correctly
        this.redrawAll();
        return true;
      }
      return false;
    } catch (e) {
      console.error('Failed to load schedule from localStorage:', e);
      return false;
    }
  }

  clearScheduleCache() {
    try {
      localStorage.removeItem('scheduleCache');
    } catch (e) {
      console.error('Failed to clear schedule cache:', e);
    }
  }

  transformElements(transformType, oldWidth, oldHeight, newWidth, newHeight) {
    const transformMatrix = (() => {
      if (transformType === 'rotate90') return { a: 0, b: 1, c: -1, d: 0 };
      if (transformType === 'mirror') return { a: -1, b: 0, c: 0, d: 1 };
      if (transformType === 'flip') return { a: 1, b: 0, c: 0, d: -1 };
      return { a: 1, b: 0, c: 0, d: 1 };
    })();

    const transformPoint = (x, y) => {
      const maxX = Math.max(0, newWidth - 1);
      const maxY = Math.max(0, newHeight - 1);
      const clamp = (value, max) => Math.max(0, Math.min(value, max));
      let mapped;
      if (transformType === 'rotate90') {
        mapped = { x: oldHeight - 1 - y, y: x };
      } else if (transformType === 'mirror') {
        mapped = { x: oldWidth - 1 - x, y: y };
      } else if (transformType === 'flip') {
        mapped = { x: x, y: oldHeight - 1 - y };
      } else {
        mapped = { x, y };
      }
      return {
        x: clamp(mapped.x, maxX),
        y: clamp(mapped.y, maxY)
      };
    };

    const multiplyMatrix = (m1, m2) => {
      return {
        a: m1.a * m2.a + m1.c * m2.b,
        b: m1.b * m2.a + m1.d * m2.b,
        c: m1.a * m2.c + m1.c * m2.d,
        d: m1.b * m2.c + m1.d * m2.d
      };
    };

    const originalSelectedTextElement = this.selectedTextElement;
    const originalSelectedEditingText = this.selectedEditingText;
    const originalSelectedTodoItem = this.selectedTodoItem;
    const textElementMap = new Map();
    const todoItemMap = new Map();

    this.textElements = this.textElements.map((item) => {
      const p = transformPoint(item.x, item.y);
      const currentMatrix = this.getItemTransformMatrix(item);
      const transformedMatrix = multiplyMatrix(transformMatrix, currentMatrix);
      const transformed = { ...item, x: p.x, y: p.y, ...transformedMatrix };
      textElementMap.set(item, transformed);
      return transformed;
    });

    this.todoItems = this.todoItems.map((item) => {
      const p = transformPoint(item.x, item.y);
      const currentMatrix = this.getItemTransformMatrix(item);
      const transformedMatrix = multiplyMatrix(transformMatrix, currentMatrix);
      const transformed = {
        ...item,
        x: p.x,
        y: p.y,
        ...transformedMatrix,
        deleteButtonCenterX: null,
        deleteButtonCenterY: null,
        deleteButtonHitRadius: null
      };
      todoItemMap.set(item, transformed);
      return transformed;
    });

    this.lineSegments = this.lineSegments.map((segment) => {
      if (segment.type === 'dot') {
        const p = transformPoint(segment.x, segment.y);
        return { ...segment, x: p.x, y: p.y };
      }
      const p1 = transformPoint(segment.x1, segment.y1);
      const p2 = transformPoint(segment.x2, segment.y2);
      return { ...segment, x1: p1.x, y1: p1.y, x2: p2.x, y2: p2.y };
    });

    if (this.scheduleData && typeof this.transformSchedule === 'function') {
      this.transformSchedule(transformType, oldWidth, oldHeight, newWidth, newHeight);
    }

    this.selectedTextElement = originalSelectedTextElement ? (textElementMap.get(originalSelectedTextElement) || null) : null;
    this.selectedEditingText = originalSelectedEditingText ? (textElementMap.get(originalSelectedEditingText) || null) : null;
    this.selectedTodoItem = originalSelectedTodoItem ? (todoItemMap.get(originalSelectedTodoItem) || null) : null;
    this.syncUI();
  }

  resizeElements(oldWidth, oldHeight, newWidth, newHeight) {
    const scaleX = newWidth / oldWidth, scaleY = newHeight / oldHeight;
    [...this.textElements, ...this.todoItems].forEach(item => {
      const matrix = this.getItemTransformMatrix(item);
      item.x *= scaleX; item.y *= scaleY;
      Object.assign(item, { a: matrix.a * scaleX, b: matrix.b * scaleY,
        c: matrix.c * scaleX, d: matrix.d * scaleY });
    });
    this.lineSegments.forEach(segment => {
      if (segment.type === 'dot') { segment.x *= scaleX; segment.y *= scaleY; }
      else { segment.x1 *= scaleX; segment.x2 *= scaleX; segment.y1 *= scaleY; segment.y2 *= scaleY; }
      segment.size *= (scaleX + scaleY) / 2;
    });
    this.workspaceRevision++;
    this.updateSelectionOverlay();
  }

  clearElements() {
    this.textElements = [];
    this.lineSegments = [];
    this.todoItems = [];
    this.scheduleData = null;
    this.scheduleCellFontSizes = null;
    this.selectedScheduleCell = null;
    this.cancelTextPlacement();
    this.todoEditing = false;
    this.resetBaseImage();
    this.syncUI();
  }
}
