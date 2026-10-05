(function (root) {
  'use strict';

  const WEEK_DAYS = ['周一', '周二', '周三', '周四', '周五', '周六', '周日'];
  const DEFAULT_TITLE = '本周课程表';
  const clone = value => JSON.parse(JSON.stringify(value));
  const number = (value, fallback, minimum, maximum) => {
    const parsed = value == null || value === '' ? NaN : Number(value);
    return Math.max(minimum, Math.min(maximum, Number.isFinite(parsed) ? parsed : fallback));
  };
  const integer = (value, fallback, minimum, maximum) => Math.round(number(value, fallback, minimum, maximum));
  const text = value => String(value == null ? '' : value).slice(0, 2000);
  const element = id => root.document ? root.document.getElementById(id) : null;
  const read = (id, fallback) => element(id) ? element(id).value : fallback;
  const color = value => /^#[\da-f]{6}$/i.test(String(value)) ? value : '#000000';
  const family = value => String(value || 'SimHei').slice(0, 80);
  const identity = () => ({ a: 1, b: 0, c: 0, d: 1 });
  const matrix = value => {
    const candidate = value || identity();
    const { a, b, c, d } = candidate;
    return [a, b, c, d].every(part => [-1, 0, 1].includes(part)) && Math.abs(a * d - b * c) === 1 && a * a + b * b === 1 && c * c + d * d === 1 && a * c + b * d === 0 ? { a, b, c, d } : identity();
  };

  function pixelGrid(layout, transform, columns, rows, canvas) {
    const point = (x, y) => ({
      x: layout.x + transform.a * x + transform.c * y,
      y: layout.y + transform.b * x + transform.d * y
    });
    const pixel = (value, maximum) => Math.max(0, Math.min(maximum - 1, Math.round(value)));
    const rectangle = (first, second) => {
      const left = pixel(Math.min(first.x, second.x), canvas.width);
      const right = pixel(Math.max(first.x, second.x), canvas.width);
      const top = pixel(Math.min(first.y, second.y), canvas.height);
      const bottom = pixel(Math.max(first.y, second.y), canvas.height);
      return [left, top, right - left + 1, bottom - top + 1];
    };
    const corners = [point(0, 0), point(layout.width, 0), point(0, layout.height), point(layout.width, layout.height)];
    const background = rectangle(
      { x: Math.min(...corners.map(p => p.x)), y: Math.min(...corners.map(p => p.y)) },
      { x: Math.max(...corners.map(p => p.x)), y: Math.max(...corners.map(p => p.y)) }
    );
    const lines = [];
    for (let col = 0; col <= columns; col++) {
      const x = col * layout.cellWidth;
      lines.push(rectangle(point(x, layout.titleHeight), point(x, layout.height)));
    }
    for (let row = 0; row <= rows; row++) {
      const y = layout.titleHeight + row * layout.cellHeight;
      lines.push(rectangle(point(0, y), point(layout.width, y)));
    }
    return { background, lines };
  }

  function wrapLines(ctx, value, width) {
    const lines = [];
    for (const paragraph of text(value).replace(/\r\n?/g, '\n').split('\n')) {
      if (!paragraph) {
        lines.push('');
        continue;
      }
      let current = '';
      for (const character of Array.from(paragraph)) {
        if (current && ctx.measureText(current + character).width > width) {
          lines.push(current.trimEnd());
          current = character === ' ' ? '' : character;
        } else {
          current += character;
        }
      }
      lines.push(current.trimEnd());
    }
    return lines;
  }

  function fitText(ctx, value, width, height, requestedSize, fontFamily, bold) {
    let size = integer(requestedSize, 12, 6, 32);
    let lines;
    for (; size >= 6; size--) {
      ctx.font = `${bold ? '600 ' : ''}${size}px ${fontFamily}`;
      lines = wrapLines(ctx, value, width);
      if (lines.length * size * 1.2 <= height && lines.every(line => ctx.measureText(line).width <= width)) break;
    }
    size = Math.max(6, size);
    ctx.font = `${bold ? '600 ' : ''}${size}px ${fontFamily}`;
    const lineHeight = size * 1.2;
    const maximumLines = Math.max(1, Math.floor(height / lineHeight));
    if (lines.length > maximumLines) {
      lines = lines.slice(0, maximumLines);
      let last = lines[lines.length - 1];
      while (last && ctx.measureText(last + '…').width > width) last = Array.from(last).slice(0, -1).join('');
      lines[lines.length - 1] = last + '…';
    }
    return { size, lines, lineHeight };
  }

  function parseBulk(value) {
    const cleaned = String(value || '').replace(/\r\n?/g, '\n').replace(/\n+$/, '');
    if (cleaned.length > 180000) throw new Error('课程文本过长，请分批导入。');
    if (!cleaned.trim()) throw new Error('请先粘贴课程内容。');
    const rows = cleaned.split('\n').map(row => row.split('\t').map(cell => text(cell.trim())));
    if (rows.length > 12 || rows.some(row => row.length > 7)) throw new Error('最多支持 12 节、7 天，请检查行数和 Tab 分列。');
    return rows;
  }

  const methods = {
    initScheduleTools() {
      if (this._scheduleToolsInitialized) return;
      this._scheduleToolsInitialized = true;
      this.weekDays = WEEK_DAYS.slice();
      this.scheduleTitle = this.scheduleTitle == null ? DEFAULT_TITLE : this.scheduleTitle;
      this.showScheduleCellIndicator = false;
      const bind = (id, type, callback) => {
        const target = element(id);
        if (target) target.addEventListener(type, callback);
      };
      bind('schedule-mode', 'click', () => {
        this.setActiveTool(this.currentTool === 'schedule' ? null : 'schedule', '点击课表单元格编辑课程');
        if (this.currentTool !== 'schedule') this.cancelScheduleInput();
        this.syncScheduleUI();
      });
      bind('create-schedule-btn', 'click', () => this.createSchedule());
      bind('schedule-fit-btn', 'click', () => this.fitScheduleToCanvas(true));
      bind('schedule-example-btn', 'click', () => this.fillScheduleExample());
      bind('schedule-clear-btn', 'click', () => this.clearScheduleCourses());
      bind('schedule-remove-btn', 'click', () => this.removeSchedule());
      bind('schedule-bulk-apply-btn', 'click', () => this.applyScheduleBulk(read('schedule-bulk-input', '')));
      bind('schedule-input-confirm-btn', 'click', () => this.confirmScheduleInput());
      bind('schedule-input-cancel-btn', 'click', () => this.cancelScheduleInput());
      bind('schedule-input', 'keydown', event => {
        if (event.key === 'Escape') this.cancelScheduleInput();
        else if (event.key === 'Enter' && (event.ctrlKey || event.metaKey)) {
          event.preventDefault();
          this.confirmScheduleInput();
        }
      });
      bind('schedule-preset', 'change', event => {
        const days = event.target.value === 'week' ? 5 : event.target.value === 'full' ? 7 : null;
        if (days == null) return;
        if (element('schedule-days')) element('schedule-days').value = days;
        if (this.scheduleData) this.createSchedule();
      });
      for (const id of ['schedule-days', 'schedule-classes']) bind(id, 'change', () => {
        const input = element(id);
        input.value = integer(input.value, id === 'schedule-days' ? 5 : 6, 1, id === 'schedule-days' ? 7 : 12);
        if (element('schedule-preset')) element('schedule-preset').value = Number(read('schedule-days', 5)) === 5 ? 'week' : Number(read('schedule-days', 5)) === 7 ? 'full' : 'custom';
        this._scheduleDimensionsDraft = true;
        this._scheduleMessage = this.scheduleData ? '尺寸已修改，点击“更新课表”应用并保留已有课程。' : '';
        this.updateScheduleSummary();
      });
      bind('schedule-title', 'change', () => {
        this.scheduleTitle = String(read('schedule-title', DEFAULT_TITLE)).trim().slice(0, 80);
        if (this.scheduleData) this.commitSchedule();
      });
      bind('schedule-font-family', 'change', () => {
        this.scheduleFontFamily = family(read('schedule-font-family', 'SimHei'));
        if (this.scheduleData) this.commitSchedule();
      });
      bind('schedule-color', 'change', () => {
        this.scheduleColor = color(read('schedule-color', '#000000'));
        if (this.scheduleData) this.commitSchedule();
      });
      bind('schedule-font-size', 'change', () => this.changeScheduleFontSize(read('schedule-font-size', 12)));
      bind('schedule-font-increase-btn', 'click', () => this.changeScheduleFontSize(Number(read('schedule-font-size', this.scheduleFontSize)) + 1));
      bind('schedule-font-decrease-btn', 'click', () => this.changeScheduleFontSize(Number(read('schedule-font-size', this.scheduleFontSize)) - 1));
      for (const [id, dx, dy] of [
        ['schedule-move-up-btn', 0, -10], ['schedule-move-down-btn', 0, 10],
        ['schedule-move-left-btn', -10, 0], ['schedule-move-right-btn', 10, 0]
      ]) bind(id, 'click', () => this.moveSchedule(dx, dy));
      bind('schedule-zoom-in-btn', 'click', () => this.zoomSchedule(1.1));
      bind('schedule-zoom-out-btn', 'click', () => this.zoomSchedule(1 / 1.1));
      this.syncScheduleUI();
    },

    captureScheduleState() {
      return clone({
        scheduleData: this.scheduleData || null,
        scheduleDays: this.scheduleDays,
        scheduleClasses: this.scheduleClasses,
        scheduleTitle: this.scheduleTitle == null ? DEFAULT_TITLE : this.scheduleTitle,
        scheduleFontFamily: this.scheduleFontFamily,
        scheduleFontSize: this.scheduleFontSize,
        scheduleColor: this.scheduleColor,
        scheduleCellWidth: this.scheduleCellWidth,
        scheduleCellHeight: this.scheduleCellHeight,
        scheduleStartX: this.scheduleStartX,
        scheduleStartY: this.scheduleStartY,
        scheduleTransformMatrix: matrix(this.scheduleTransformMatrix),
        scheduleTitleHeight: this.scheduleTitleHeight,
        scheduleCellFontSizes: this.scheduleCellFontSizes || null
      });
    },

    restoreScheduleState(state) {
      const cache = state || {};
      this.scheduleDays = integer(cache.scheduleDays, Array.isArray(cache.scheduleData) && Array.isArray(cache.scheduleData[0]) ? cache.scheduleData[0].length - 1 : 5, 1, 7);
      this.scheduleClasses = integer(cache.scheduleClasses, Array.isArray(cache.scheduleData) ? cache.scheduleData.length - 1 : 6, 1, 12);
      this.scheduleTitle = cache.scheduleTitle == null ? DEFAULT_TITLE : String(cache.scheduleTitle).slice(0, 80);
      this.scheduleFontFamily = family(cache.scheduleFontFamily);
      this.scheduleFontSize = integer(cache.scheduleFontSize, 12, 6, 32);
      this.scheduleColor = color(cache.scheduleColor);
      this.weekDays = WEEK_DAYS.slice();
      this.selectedScheduleCell = null;
      this.scheduleData = null;
      this.scheduleCellFontSizes = null;
      if (Array.isArray(cache.scheduleData) && cache.scheduleData.length) this.buildScheduleGrid(this.scheduleDays, this.scheduleClasses, cache.scheduleData, cache.scheduleCellFontSizes);
      this.scheduleCellWidth = number(cache.scheduleCellWidth, 60, 1, 10000);
      this.scheduleCellHeight = number(cache.scheduleCellHeight, 35, 1, 10000);
      this.scheduleStartX = number(cache.scheduleStartX, 12, 0, 10000);
      this.scheduleStartY = number(cache.scheduleStartY, 12, 0, 10000);
      this.scheduleTransformMatrix = matrix(cache.scheduleTransformMatrix);
      this.scheduleTitleHeight = number(cache.scheduleTitleHeight, Math.min(36, Math.max(18, this.canvas.height * 0.09)), 0, 100);
      this.constrainScheduleToCanvas();
      this._scheduleMessage = '';
      this._scheduleDimensionsDraft = false;
      this.syncScheduleUI();
    },

    buildScheduleGrid(days, classes, existingData, existingSizes) {
      this._scheduleDimensionsDraft = false;
      this.scheduleDays = integer(days, 5, 1, 7);
      this.scheduleClasses = integer(classes, 6, 1, 12);
      this.scheduleData = Array.from({ length: this.scheduleClasses + 1 }, (_, row) => Array.from({ length: this.scheduleDays + 1 }, (_, col) => {
        if (existingData && Array.isArray(existingData[row]) && existingData[row][col] != null) return text(existingData[row][col]);
        return row === 0 ? (col === 0 ? '节次' : WEEK_DAYS[col - 1]) : col === 0 ? `第${row}节` : '';
      }));
      this.scheduleCellFontSizes = this.scheduleData.map((row, i) => row.map((_, j) => integer(existingSizes && existingSizes[i] && existingSizes[i][j], this.scheduleFontSize, 6, 32)));
    },

    createSchedule() {
      const wasEmpty = !this.scheduleData;
      this.scheduleTitle = String(read('schedule-title', this.scheduleTitle == null ? DEFAULT_TITLE : this.scheduleTitle)).trim().slice(0, 80);
      this.scheduleFontFamily = family(read('schedule-font-family', this.scheduleFontFamily));
      this.scheduleFontSize = integer(this.selectedScheduleCell ? this.scheduleFontSize : read('schedule-font-size', this.scheduleFontSize), 12, 6, 32);
      this.scheduleColor = color(read('schedule-color', this.scheduleColor));
      this.buildScheduleGrid(read('schedule-days', this.scheduleDays), read('schedule-classes', this.scheduleClasses), this.scheduleData, this.scheduleCellFontSizes);
      this.cancelScheduleInput();
      if (wasEmpty) this.calculateScheduleDimensions();
      this._scheduleMessage = wasEmpty ? '课表已生成，点击单元格填写课程。' : '课表已更新，重叠区域的课程已保留。';
      this.commitSchedule();
    },

    getScheduleTitleHeight() {
      return this.scheduleTitle ? number(this.scheduleTitleHeight, Math.min(36, Math.max(18, this.canvas.height * 0.09)), 0, 100) : 0;
    },

    calculateScheduleDimensions() {
      this.scheduleTransformMatrix = identity();
      this.scheduleTitleHeight = Math.min(36, Math.max(18, this.canvas.height * 0.09));
      const padding = Math.min(20, Math.max(4, Math.min(this.canvas.width, this.canvas.height) * 0.04));
      this.scheduleStartX = padding;
      this.scheduleStartY = padding;
      this.scheduleCellWidth = Math.max(1, (this.canvas.width - padding * 2) / (this.scheduleDays + 1));
      this.scheduleCellHeight = Math.max(1, (this.canvas.height - padding * 2 - this.getScheduleTitleHeight()) / (this.scheduleClasses + 1));
      this.constrainScheduleToCanvas();
    },

    constrainScheduleToCanvas() {
      if (!this.scheduleData) return;
      const transform = matrix(this.scheduleTransformMatrix);
      this.scheduleTransformMatrix = transform;
      const localWidth = transform.a === 0 ? this.canvas.height : this.canvas.width;
      const localHeight = transform.a === 0 ? this.canvas.width : this.canvas.height;
      const titleHeight = Math.min(this.getScheduleTitleHeight(), Math.max(0, localHeight - this.scheduleClasses - 1));
      const width = Math.max(1, localWidth);
      const height = Math.max(1, localHeight - titleHeight);
      this.scheduleCellWidth = number(this.scheduleCellWidth, 60, 0.1, width / (this.scheduleDays + 1));
      this.scheduleCellHeight = number(this.scheduleCellHeight, 35, 0.1, height / (this.scheduleClasses + 1));
      const tableWidth = this.scheduleCellWidth * (this.scheduleDays + 1);
      const tableHeight = titleHeight + this.scheduleCellHeight * (this.scheduleClasses + 1);
      const xs = [0, transform.a * tableWidth, transform.c * tableHeight, transform.a * tableWidth + transform.c * tableHeight];
      const ys = [0, transform.b * tableWidth, transform.d * tableHeight, transform.b * tableWidth + transform.d * tableHeight];
      this.scheduleStartX = number(this.scheduleStartX, 0, -Math.min(...xs), this.canvas.width - Math.max(...xs));
      this.scheduleStartY = number(this.scheduleStartY, 0, -Math.min(...ys), this.canvas.height - Math.max(...ys));
    },

    getScheduleLayout() {
      this.constrainScheduleToCanvas();
      const transform = matrix(this.scheduleTransformMatrix);
      const localHeight = transform.a === 0 ? this.canvas.width : this.canvas.height;
      const titleHeight = Math.min(this.getScheduleTitleHeight(), Math.max(0, localHeight - this.scheduleClasses - 1));
      return {
        x: this.scheduleStartX, y: this.scheduleStartY,
        gridY: this.scheduleStartY + titleHeight, titleHeight,
        cellWidth: this.scheduleCellWidth, cellHeight: this.scheduleCellHeight,
        width: this.scheduleCellWidth * (this.scheduleDays + 1),
        height: titleHeight + this.scheduleCellHeight * (this.scheduleClasses + 1)
      };
    },

    fitScheduleToCanvas(commit = false) {
      if (!this.scheduleData) return;
      this.calculateScheduleDimensions();
      if (commit) {
        this._scheduleMessage = '课表已适配当前画布。';
        this.commitSchedule();
      }
    },

    transformSchedule(type, oldWidth, oldHeight, newWidth, newHeight) {
      if (!this.scheduleData) return;
      const current = matrix(this.scheduleTransformMatrix);
      let operation;
      const x = this.scheduleStartX;
      const y = this.scheduleStartY;
      if (type === 'rotate90') {
        operation = { a: 0, b: 1, c: -1, d: 0 };
        this.scheduleStartX = oldHeight - y;
        this.scheduleStartY = x;
      } else if (type === 'mirror') {
        operation = { a: -1, b: 0, c: 0, d: 1 };
        this.scheduleStartX = oldWidth - x;
      } else if (type === 'flip') {
        operation = { a: 1, b: 0, c: 0, d: -1 };
        this.scheduleStartY = oldHeight - y;
      } else return;
      this.scheduleTransformMatrix = {
        a: operation.a * current.a + operation.c * current.b,
        b: operation.b * current.a + operation.d * current.b,
        c: operation.a * current.c + operation.c * current.d,
        d: operation.b * current.c + operation.d * current.d
      };
      // The caller has already resized the canvas to newWidth/newHeight.
      this.constrainScheduleToCanvas();
      this.cancelScheduleInput();
    },

    moveSchedule(dx, dy) {
      if (!this.scheduleData) return;
      this.scheduleStartX += dx;
      this.scheduleStartY += dy;
      this.commitSchedule();
    },

    zoomSchedule(ratio) {
      if (!this.scheduleData) return;
      this.scheduleCellWidth *= ratio;
      this.scheduleCellHeight *= ratio;
      this.commitSchedule();
    },

    changeScheduleFontSize(value) {
      const size = integer(value, this.scheduleFontSize, 6, 32);
      if (this.selectedScheduleCell && this.scheduleCellFontSizes) {
        const { row, col } = this.selectedScheduleCell;
        this.scheduleCellFontSizes[row][col] = size;
      } else {
        this.scheduleFontSize = size;
        if (this.scheduleData) this.scheduleCellFontSizes = this.scheduleData.map(row => row.map(() => size));
      }
      if (this.scheduleData) this.commitSchedule();
      else this.syncScheduleUI();
    },

    drawSchedule() {
      if (!this.scheduleData) return;
      const ctx = this.ctx;
      const original = this.getScheduleLayout();
      const layout = { ...original, x: 0, y: 0, gridY: original.titleHeight };
      const transform = matrix(this.scheduleTransformMatrix);
      const grid = pixelGrid(original, transform, this.scheduleDays + 1, this.scheduleClasses + 1, this.canvas);
      ctx.save();
      ctx.globalAlpha = 1;
      ctx.globalCompositeOperation = 'source-over';
      ctx.fillStyle = '#ffffff';
      ctx.fillRect(...grid.background);
      ctx.save();
      ctx.translate(original.x, original.y);
      ctx.transform(transform.a, transform.b, transform.c, transform.d, 0, 0);
      ctx.beginPath();
      ctx.rect(layout.x, layout.y, layout.width, layout.height);
      ctx.clip();
      if (layout.titleHeight) {
        const fitted = fitText(ctx, this.scheduleTitle, Math.max(1, layout.width - 8), Math.max(1, layout.titleHeight - 4), Math.min(24, this.scheduleFontSize + 4), this.scheduleFontFamily, true);
        ctx.fillStyle = this.scheduleColor;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        fitted.lines.forEach((line, index) => ctx.fillText(line, layout.x + layout.width / 2, layout.y + (layout.titleHeight - fitted.lines.length * fitted.lineHeight) / 2 + (index + 0.5) * fitted.lineHeight));
      }
      for (let row = 0; row <= this.scheduleClasses; row++) {
        for (let col = 0; col <= this.scheduleDays; col++) {
          const x = layout.x + col * layout.cellWidth;
          const y = layout.gridY + row * layout.cellHeight;
          const cellText = this.scheduleData[row][col];
          if (!cellText) continue;
          ctx.save();
          ctx.beginPath();
          const padding = Math.min(5, layout.cellWidth * 0.1, layout.cellHeight * 0.1);
          ctx.rect(x + padding, y + padding, Math.max(0.1, layout.cellWidth - padding * 2), Math.max(0.1, layout.cellHeight - padding * 2));
          ctx.clip();
          const fitted = fitText(ctx, cellText, Math.max(0.1, layout.cellWidth - padding * 2), Math.max(0.1, layout.cellHeight - padding * 2), this.scheduleCellFontSizes && this.scheduleCellFontSizes[row] ? this.scheduleCellFontSizes[row][col] : this.scheduleFontSize, this.scheduleFontFamily, row === 0 || col === 0);
          ctx.fillStyle = this.scheduleColor;
          ctx.textAlign = 'center';
          ctx.textBaseline = 'middle';
          fitted.lines.forEach((line, index) => ctx.fillText(line, x + layout.cellWidth / 2, y + (layout.cellHeight - fitted.lines.length * fitted.lineHeight) / 2 + (index + 0.5) * fitted.lineHeight));
          ctx.restore();
        }
      }
      ctx.restore();
      // Integer, opaque pixels survive the device palette conversion without dithering.
      // Draw after the fractional text clip so outer borders remain fully visible.
      ctx.fillStyle = '#000000';
      grid.lines.forEach(line => ctx.fillRect(...line));
      ctx.restore();
    },

    getScheduleCellAt(event) {
      if (!this.scheduleData) return null;
      const rect = this.canvas.getBoundingClientRect();
      const x = event.clientX != null ? (event.clientX - rect.left) * this.canvas.width / rect.width : event.x;
      const y = event.clientY != null ? (event.clientY - rect.top) * this.canvas.height / rect.height : event.y;
      const layout = this.getScheduleLayout();
      const transform = matrix(this.scheduleTransformMatrix);
      const determinant = transform.a * transform.d - transform.b * transform.c;
      const localX = (transform.d * (x - layout.x) - transform.c * (y - layout.y)) / determinant;
      const localY = (-transform.b * (x - layout.x) + transform.a * (y - layout.y)) / determinant;
      const col = Math.floor(localX / layout.cellWidth);
      const row = Math.floor((localY - layout.titleHeight) / layout.cellHeight);
      return row >= 0 && row <= this.scheduleClasses && col >= 0 && col <= this.scheduleDays ? { row, col } : null;
    },

    selectScheduleCell(cell) {
      if (!cell || !this.scheduleData || !this.scheduleData[cell.row] || cell.col < 0 || cell.col > this.scheduleDays) {
        this.cancelScheduleInput();
        return;
      }
      this.selectedScheduleCell = { row: cell.row, col: cell.col };
      const input = element('schedule-input');
      if (input) input.value = this.scheduleData[cell.row][cell.col];
      this.syncScheduleUI();
      if (input) input.focus({ preventScroll: true });
    },

    updateScheduleCell(row, col, value) {
      if (!this.scheduleData || row < 0 || row > this.scheduleClasses || col < 0 || col > this.scheduleDays) return false;
      this.scheduleData[row][col] = text(value).replace(/\r\n?/g, '\n');
      this._scheduleMessage = '课程内容已保存。';
      this.commitSchedule();
      return true;
    },

    confirmScheduleInput() {
      if (!this.selectedScheduleCell) return;
      const { row, col } = this.selectedScheduleCell;
      this.updateScheduleCell(row, col, read('schedule-input', ''));
      this.cancelScheduleInput();
    },

    cancelScheduleInput() {
      this.selectedScheduleCell = null;
      if (element('schedule-input')) element('schedule-input').value = '';
      this.syncScheduleUI();
    },

    scheduleDragStart() {},
    scheduleDragMove() {},
    scheduleDragEnd() {},

    clearScheduleCourses() {
      if (!this.scheduleData) return;
      for (let row = 1; row <= this.scheduleClasses; row++) for (let col = 1; col <= this.scheduleDays; col++) this.scheduleData[row][col] = '';
      this.cancelScheduleInput();
      this._scheduleMessage = '课程已清空，标题和表格设置已保留。';
      this.commitSchedule();
    },

    removeSchedule() {
      if (!this.scheduleData) return;
      this.scheduleData = null;
      this.scheduleCellFontSizes = null;
      this._scheduleDimensionsDraft = false;
      this.cancelScheduleInput();
      this._scheduleMessage = '课表已移除，可撤销恢复。';
      this.commitSchedule();
    },

    fillScheduleExample() {
      if (!this.scheduleData) {
        this.buildScheduleGrid(read('schedule-days', 5), read('schedule-classes', 6));
        this.scheduleTitle = String(read('schedule-title', DEFAULT_TITLE)).slice(0, 80);
        this.calculateScheduleDimensions();
      }
      const courses = ['语文', '数学', '英语', '物理', '历史', '体育', '自习'];
      for (let row = 1; row <= this.scheduleClasses; row++) for (let col = 1; col <= this.scheduleDays; col++) this.scheduleData[row][col] = courses[(row + col - 2) % courses.length];
      this.cancelScheduleInput();
      this._scheduleMessage = '已填入示例课程，可点击单元格替换。';
      this.commitSchedule();
    },

    applyScheduleBulk(value) {
      let rows;
      try {
        rows = parseBulk(value);
      } catch (error) {
        this._scheduleMessage = error.message;
        this.updateScheduleSummary();
        return false;
      }
      const days = Math.max(this.scheduleData ? this.scheduleDays : integer(read('schedule-days', 5), 5, 1, 7), ...rows.map(row => row.length));
      const classes = Math.max(this.scheduleData ? this.scheduleClasses : integer(read('schedule-classes', 6), 6, 1, 12), rows.length);
      const wasEmpty = !this.scheduleData;
      this.buildScheduleGrid(days, classes, this.scheduleData, this.scheduleCellFontSizes);
      if (wasEmpty) {
        this.scheduleTitle = String(read('schedule-title', DEFAULT_TITLE)).slice(0, 80);
        this.calculateScheduleDimensions();
      }
      rows.forEach((row, i) => row.forEach((course, j) => { this.scheduleData[i + 1][j + 1] = course; }));
      this.cancelScheduleInput();
      this._scheduleMessage = `已导入 ${rows.length} 行课程，粘贴区域之外的课程已保留。`;
      this.commitSchedule();
      return true;
    },

    commitSchedule() {
      this.constrainScheduleToCanvas();
      this.redrawAll();
      this.saveScheduleToLocalStorage();
      this.saveToHistory();
      this.syncScheduleUI();
    },

    updateScheduleSummary() {
      const target = element('schedule-summary');
      let count = 0;
      if (this.scheduleData) for (let row = 1; row <= this.scheduleClasses; row++) for (let col = 1; col <= this.scheduleDays; col++) if (this.scheduleData[row][col].trim()) count++;
      if (target) target.textContent = this.scheduleData ? `${this.scheduleDays} 天 × ${this.scheduleClasses} 节 · ${count} 格` : '尚未生成';
      const status = element('schedule-status');
      if (status) status.textContent = this._scheduleMessage || (this.scheduleData ? '点击单元格编辑课程；文字会自动换行、适配格子。' : '生成课表后点击单元格编辑，也可粘贴表格。');
    },

    syncScheduleUI() {
      const selected = this.selectedScheduleCell && this.scheduleData && this.scheduleData[this.selectedScheduleCell.row];
      const controls = {
        'schedule-title': this.scheduleTitle == null ? DEFAULT_TITLE : this.scheduleTitle,
        'schedule-days': this.scheduleDays, 'schedule-classes': this.scheduleClasses,
        'schedule-font-family': this.scheduleFontFamily, 'schedule-color': this.scheduleColor,
        'schedule-font-size': selected && this.scheduleCellFontSizes ? this.scheduleCellFontSizes[this.selectedScheduleCell.row][this.selectedScheduleCell.col] : this.scheduleFontSize,
        'schedule-preset': this.scheduleDays === 5 ? 'week' : this.scheduleDays === 7 ? 'full' : 'custom'
      };
      for (const [id, value] of Object.entries(controls)) {
        const target = element(id);
        if (!target || value == null || this._scheduleDimensionsDraft && ['schedule-days', 'schedule-classes', 'schedule-preset'].includes(id)) continue;
        if (target.tagName === 'SELECT' && target.options && !Array.from(target.options).some(option => option.value === String(value)) && root.document.createElement) {
          const option = root.document.createElement('option');
          option.value = value;
          option.textContent = id === 'schedule-font-family' ? value : `自定义 (${value})`;
          target.appendChild(option);
        }
        target.value = value;
      }
      const label = element('schedule-cell-label');
      if (label) {
        const cell = this.selectedScheduleCell;
        label.textContent = selected ? cell.row === 0 ? cell.col === 0 ? '节次表头' : `${WEEK_DAYS[cell.col - 1]} · 星期表头` : cell.col === 0 ? `第 ${cell.row} 节 · 节次标签` : `${WEEK_DAYS[cell.col - 1]} · 第 ${cell.row} 节` : '点击课表单元格编辑';
      }
      if (element('schedule-cell-editor')) element('schedule-cell-editor').hidden = !selected;
      if (element('create-schedule-btn')) element('create-schedule-btn').textContent = this.scheduleData ? '更新课表' : '生成课表';
      for (const id of ['schedule-fit-btn', 'schedule-clear-btn', 'schedule-remove-btn', 'schedule-move-up-btn', 'schedule-move-down-btn', 'schedule-move-left-btn', 'schedule-move-right-btn', 'schedule-zoom-in-btn', 'schedule-zoom-out-btn']) if (element(id)) element(id).disabled = !this.scheduleData;
      this.updateScheduleSummary();
    },

    saveScheduleToLocalStorage() {
      if (!root.localStorage) return;
      try {
        if (this.scheduleData) root.localStorage.setItem('scheduleCache', JSON.stringify(this.captureScheduleState()));
        else root.localStorage.removeItem('scheduleCache');
      } catch (error) {
        console.warn('课表本地缓存保存失败:', error);
      }
    },

    async loadScheduleFromLocalStorage() {
      if (!root.localStorage) return false;
      try {
        const cached = root.localStorage.getItem('scheduleCache');
        if (!cached) return false;
        const state = JSON.parse(cached);
        if (!Array.isArray(state.scheduleData) || !state.scheduleData.length) return false;
        this.restoreScheduleState(state);
        this.redrawAll();
        return true;
      } catch (error) {
        console.warn('课表本地缓存读取失败:', error);
        return false;
      }
    },

    clearScheduleCache() {
      try { if (root.localStorage) root.localStorage.removeItem('scheduleCache'); } catch (error) { console.warn('课表本地缓存清除失败:', error); }
    }
  };

  const api = { install(PaintClass) { Object.assign(PaintClass.prototype, methods); }, methods, fitText, wrapLines, parseBulk };
  if (typeof PaintManager !== 'undefined') api.install(PaintManager);
  root.EPDPaintSchedule = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : window);
