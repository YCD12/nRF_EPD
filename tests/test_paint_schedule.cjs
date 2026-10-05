const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function fixture(width = 400, height = 300) {
  const controls = new Map();
  for (const [id, value] of Object.entries({
    'schedule-days': '5', 'schedule-classes': '6', 'schedule-title': '本周课程表',
    'schedule-font-family': 'SimHei', 'schedule-font-size': '12', 'schedule-color': '#000000',
    'schedule-preset': 'week', 'schedule-input': '', 'schedule-summary': '', 'schedule-cell-label': '',
    'schedule-cell-editor': '', 'schedule-bulk-input': '',
    'schedule-mode': '', 'create-schedule-btn': '', 'schedule-fit-btn': '', 'schedule-clear-btn': '', 'schedule-remove-btn': '',
    'schedule-example-btn': '', 'schedule-bulk-apply-btn': '', 'schedule-input-confirm-btn': '',
    'schedule-input-cancel-btn': '', 'schedule-font-increase-btn': '', 'schedule-font-decrease-btn': '',
    'schedule-move-up-btn': '', 'schedule-move-down-btn': '', 'schedule-move-left-btn': '', 'schedule-move-right-btn': '',
    'schedule-zoom-in-btn': '', 'schedule-zoom-out-btn': ''
  })) controls.set(id, {
    value, hidden: false, disabled: false, textContent: '', listeners: {},
    addEventListener(type, fn) { this.listeners[type] = fn; },
    focus() { this.focused = true; }
  });
  const calls = [];
  const fills = [];
  const states = [];
  const ctx = {
    font: '12px SimHei', fillStyle: '', strokeStyle: '', lineWidth: 1,
    matrix: [1, 0, 0, 1, 0, 0], clipped: false, globalAlpha: 1, globalCompositeOperation: 'source-over',
    save() {
      states.push({ font: this.font, fillStyle: this.fillStyle, strokeStyle: this.strokeStyle, lineWidth: this.lineWidth,
        matrix: this.matrix.slice(), clipped: this.clipped, globalAlpha: this.globalAlpha, globalCompositeOperation: this.globalCompositeOperation });
      calls.push(['save']);
    },
    restore() { Object.assign(this, states.pop()); calls.push(['restore']); },
    beginPath() { calls.push(['beginPath']); }, clip() { this.clipped = true; calls.push(['clip']); }, stroke() { calls.push(['stroke']); },
    rect(...args) { calls.push(['rect', ...args]); },
    fillRect(...args) {
      calls.push(['fillRect', this.fillStyle, ...args]);
      fills.push({ color: this.fillStyle, rect: args, matrix: this.matrix.slice(), clipped: this.clipped, alpha: this.globalAlpha, operation: this.globalCompositeOperation });
    },
    moveTo(...args) { calls.push(['moveTo', ...args]); }, lineTo(...args) { calls.push(['lineTo', ...args]); },
    fillText(...args) { calls.push(['fillText', this.font, this.fillStyle, ...args]); },
    translate(x, y) {
      const [a, b, c, d, e, f] = this.matrix;
      this.matrix = [a, b, c, d, e + a * x + c * y, f + b * x + d * y];
      calls.push(['translate', x, y]);
    },
    transform(a, b, c, d, e, f) {
      const [oa, ob, oc, od, oe, of] = this.matrix;
      this.matrix = [oa * a + oc * b, ob * a + od * b, oa * c + oc * d, ob * c + od * d, oa * e + oc * f + oe, ob * e + od * f + of];
      calls.push(['transform', a, b, c, d, e, f]);
    },
    measureText(value) { return { width: Array.from(value).length * Number(/(\d+)px/.exec(this.font)[1]) * 0.8 }; }
  };
  const cache = new Map();
  const context = vm.createContext({
    module: { exports: {} }, console,
    document: { getElementById(id) { return controls.get(id) || null; } },
    localStorage: { getItem: key => cache.get(key), setItem: (key, value) => cache.set(key, value), removeItem: key => cache.delete(key) }
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../html/js/paint_schedule.js'), 'utf8'), context);
  class PaintFixture {
    constructor() {
      this.canvas = { width, height, getBoundingClientRect: () => ({ left: 10, top: 20, width: width / 2, height: height / 2 }) };
      this.ctx = ctx;
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
      this.scheduleCellFontSizes = null;
      this.selectedScheduleCell = null;
      this.textElements = [{ text: '保留文字' }];
      this.lineSegments = [{ points: [[1, 2], [3, 4]] }];
      this.todoItems = [{ text: '保留待办' }];
      this.history = [];
      this.renders = 0;
      this.currentTool = null;
    }
    redrawAll() { this.renders++; this.drawSchedule(); }
    saveToHistory() { this.history.push(this.captureScheduleState()); }
    setActiveTool(value) { this.currentTool = value; }
  }
  context.module.exports.install(PaintFixture);
  const paint = new PaintFixture();
  paint.initScheduleTools();
  return { paint, controls, calls, fills, cache, api: context.module.exports };
}

function plain(value) { return JSON.parse(JSON.stringify(value)); }

function bounds(paint) {
  const layout = paint.getScheduleLayout();
  const transform = paint.scheduleTransformMatrix;
  const corners = [[0, 0], [layout.width, 0], [0, layout.height], [layout.width, layout.height]].map(([x, y]) => ({
    x: layout.x + transform.a * x + transform.c * y,
    y: layout.y + transform.b * x + transform.d * y
  }));
  assert.ok(corners.every(p => p.x >= -0.00001 && p.y >= -0.00001 && p.x <= paint.canvas.width + 0.00001 && p.y <= paint.canvas.height + 0.00001));
}

test('课表生成与更新保留其他内容和重叠课程，最大格数适配小画布', () => {
  const { paint, controls } = fixture(128, 296);
  const other = plain([paint.lineSegments, paint.textElements, paint.todoItems]);
  paint.createSchedule();
  assert.equal(paint.scheduleCellFontSizes[1][1], 12);
  paint.updateScheduleCell(1, 1, '数学\n张老师');
  controls.get('schedule-days').value = '7';
  controls.get('schedule-classes').value = '12';
  paint.createSchedule();
  assert.equal(paint.scheduleData[1][1], '数学\n张老师');
  assert.equal(paint.scheduleData[0][7], '周日');
  assert.equal(paint.scheduleData[12][0], '第12节');
  assert.deepEqual(plain([paint.lineSegments, paint.textElements, paint.todoItems]), other);
  bounds(paint);
  for (let i = 0; i < 30; i++) { paint.zoomSchedule(1.5); paint.moveSchedule(100, 100); }
  bounds(paint);
  paint.fitScheduleToCanvas(true);
  bounds(paint);
  assert.equal(paint.scheduleData[1][1], '数学\n张老师');
});

test('课表长文本自动换行缩小且裁剪，单元格选中不会进入画面', () => {
  const { paint, calls } = fixture(128, 128);
  paint.createSchedule();
  paint.scheduleData[1][1] = '非常长的课程名称与教室地点'.repeat(5);
  calls.length = 0;
  paint.drawSchedule();
  const first = plain(calls);
  const body = calls.filter(call => call[0] === 'fillText');
  assert.ok(body.some(call => call[1].startsWith('600 ')), '表头加粗');
  assert.ok(body.some(call => call[3].includes('…')), '小格子长内容有省略');
  assert.ok(calls.filter(call => call[0] === 'clip').length >= 2, '整表与各有文字格子均裁剪');
  paint.selectScheduleCell({ row: 1, col: 1 });
  calls.length = 0;
  paint.drawSchedule();
  assert.deepEqual(plain(calls), first, '选择单元格只改变UI，不产生选中圆点');
});

test('Tab粘贴按节次导入，保留逗号及区域外课程并拒绝超限数据', () => {
  const { paint, controls } = fixture();
  controls.get('schedule-days').value = '2';
  controls.get('schedule-classes').value = '2';
  paint.createSchedule();
  paint.updateScheduleCell(2, 2, '原有课程');
  assert.equal(paint.applyScheduleBulk('语文,教室A\t数学\t英语\r\n物理\r\n历史\t体育'), true);
  assert.equal(paint.scheduleDays, 3);
  assert.equal(paint.scheduleClasses, 3);
  assert.equal(paint.scheduleData[1][1], '语文,教室A');
  assert.equal(paint.scheduleData[2][2], '原有课程');
  const before = plain(paint.captureScheduleState());
  const historyCount = paint.history.length;
  assert.equal(paint.applyScheduleBulk(Array(13).fill('课程').join('\n')), false);
  assert.equal(paint.applyScheduleBulk(Array(8).fill('课程').join('\t')), false);
  assert.equal(paint.applyScheduleBulk('   '), false);
  assert.deepEqual(plain(paint.captureScheduleState()), before);
  assert.equal(paint.history.length, historyCount);
});

test('课表触摸坐标按缩放转换，编辑标签和单格字号准确，取消不改内容', () => {
  const { paint, controls } = fixture();
  paint.createSchedule();
  const layout = paint.getScheduleLayout();
  const x = layout.x + layout.cellWidth * 2.5;
  const y = layout.gridY + layout.cellHeight * 3.5;
  assert.deepEqual(plain(paint.getScheduleCellAt({ clientX: 10 + x / 2, clientY: 20 + y / 2 })), { row: 3, col: 2 });
  assert.equal(paint.getScheduleCellAt({ x: layout.x + 10, y: layout.y + 1 }), null, '标题不是单元格');
  paint.selectScheduleCell({ row: 3, col: 2 });
  assert.equal(controls.get('schedule-cell-label').textContent, '周二 · 第 3 节');
  assert.equal(controls.get('schedule-cell-editor').hidden, false);
  paint.changeScheduleFontSize(26);
  assert.equal(paint.scheduleCellFontSizes[3][2], 26);
  assert.equal(paint.scheduleCellFontSizes[3][1], 12);
  controls.get('schedule-input').value = '音乐\n艺术楼';
  paint.confirmScheduleInput();
  assert.equal(paint.scheduleData[3][2], '音乐\n艺术楼');
  assert.equal(controls.get('schedule-cell-editor').hidden, true);
  paint.selectScheduleCell({ row: 3, col: 2 });
  controls.get('schedule-input').value = '未保存';
  paint.cancelScheduleInput();
  assert.equal(paint.scheduleData[3][2], '音乐\n艺术楼');
});

test('完整课表状态支持历史恢复，旧缓存补齐配置但保留其他层', async () => {
  const { paint, controls, cache } = fixture();
  paint.createSchedule();
  paint.updateScheduleCell(1, 1, '数学');
  paint.selectScheduleCell({ row: 1, col: 1 });
  paint.changeScheduleFontSize(22);
  const state = paint.captureScheduleState();
  controls.get('schedule-title').value = '临时标题';
  controls.get('schedule-title').listeners.change();
  paint.clearScheduleCourses();
  paint.restoreScheduleState(state);
  assert.equal(paint.scheduleTitle, '本周课程表');
  assert.equal(paint.scheduleData[1][1], '数学');
  assert.equal(paint.scheduleCellFontSizes[1][1], 22);
  paint.scheduleData[1][1] = '不污染快照';
  assert.equal(state.scheduleData[1][1], '数学');
  cache.set('scheduleCache', JSON.stringify({ scheduleData: [['', '周一'], ['第1节', '旧课程']] }));
  const other = plain([paint.lineSegments, paint.textElements, paint.todoItems]);
  assert.equal(await paint.loadScheduleFromLocalStorage(), true);
  assert.equal(paint.scheduleTitle, '本周课程表');
  assert.equal(paint.scheduleDays, 1);
  assert.equal(paint.scheduleClasses, 1);
  assert.equal(paint.scheduleData[1][1], '旧课程');
  assert.equal(paint.scheduleCellFontSizes[1][1], 12);
  assert.deepEqual(plain([paint.lineSegments, paint.textElements, paint.todoItems]), other);
  bounds(paint);
});

test('旋转镜像保持课程模型，整体坐标变换后仍可命中原始单元格', () => {
  const { paint } = fixture(400, 300);
  paint.createSchedule();
  paint.updateScheduleCell(1, 2, '周二第一节');
  const original = plain(paint.captureScheduleState());
  const hit = () => {
    const layout = paint.getScheduleLayout();
    const m = paint.scheduleTransformMatrix;
    const x = layout.cellWidth * 2.5;
    const y = layout.titleHeight + layout.cellHeight * 1.5;
    assert.deepEqual(plain(paint.getScheduleCellAt({ x: layout.x + m.a * x + m.c * y, y: layout.y + m.b * x + m.d * y })), { row: 1, col: 2 });
    bounds(paint);
    assert.deepEqual(plain(paint.scheduleData), original.scheduleData);
  };
  for (let i = 0; i < 4; i++) {
    const oldW = paint.canvas.width;
    const oldH = paint.canvas.height;
    paint.canvas.width = oldH;
    paint.canvas.height = oldW;
    paint.transformSchedule('rotate90', oldW, oldH, oldH, oldW);
    hit();
  }
  assert.deepEqual(plain(paint.scheduleTransformMatrix), { a: 1, b: 0, c: 0, d: 1 });
  for (const type of ['mirror', 'mirror', 'flip', 'flip']) {
    paint.transformSchedule(type, paint.canvas.width, paint.canvas.height, paint.canvas.width, paint.canvas.height);
    hit();
  }
  assert.ok(Math.abs(paint.scheduleStartX - original.scheduleStartX) < 0.001);
  assert.ok(Math.abs(paint.scheduleStartY - original.scheduleStartY) < 0.001);
  paint.transformSchedule('mirror', 400, 300, 400, 300);
  paint.fitScheduleToCanvas(true);
  assert.deepEqual(plain(paint.scheduleTransformMatrix), { a: 1, b: 0, c: 0, d: 1 });
  hit();
});

test('示例清空与工具切换均保留课表和其他画面内容，样式会写入缓存和历史', () => {
  const { paint, controls, cache } = fixture();
  paint.fillScheduleExample();
  assert.equal(paint.scheduleData[1][1], '语文');
  controls.get('schedule-mode').listeners.click();
  controls.get('schedule-mode').listeners.click();
  assert.equal(paint.scheduleData[1][1], '语文');
  controls.get('schedule-font-family').value = 'KaiTi';
  controls.get('schedule-font-family').listeners.change();
  controls.get('schedule-color').value = '#FF0000';
  controls.get('schedule-color').listeners.change();
  const cached = JSON.parse(cache.get('scheduleCache'));
  assert.equal(cached.scheduleFontFamily, 'KaiTi');
  assert.equal(cached.scheduleColor, '#FF0000');
  assert.equal(paint.history.at(-1).scheduleColor, '#FF0000');
  paint.clearScheduleCourses();
  assert.equal(paint.scheduleData[1][1], '');
  assert.equal(paint.scheduleData[0][1], '周一');
  assert.equal(paint.scheduleData[1][0], '第1节');
  assert.equal(paint.todoItems.length, 1);
});

test('移除整张课表仅清课表层和缓存，完整历史快照可恢复', () => {
  const { paint, controls, cache } = fixture();
  paint.fillScheduleExample();
  const other = plain([paint.lineSegments, paint.textElements, paint.todoItems]);
  const beforeRemoval = paint.history.at(-1);
  paint.selectScheduleCell({ row: 1, col: 1 });
  controls.get('schedule-remove-btn').listeners.click();
  assert.equal(paint.scheduleData, null);
  assert.equal(paint.scheduleCellFontSizes, null);
  assert.equal(paint.selectedScheduleCell, null);
  assert.equal(cache.has('scheduleCache'), false);
  assert.equal(controls.get('schedule-remove-btn').disabled, true);
  assert.equal(controls.get('schedule-cell-editor').hidden, true);
  assert.equal(paint.history.at(-1).scheduleData, null);
  assert.deepEqual(plain([paint.lineSegments, paint.textElements, paint.todoItems]), other);
  paint.restoreScheduleState(beforeRemoval);
  assert.equal(paint.scheduleData[1][1], '语文');
  assert.equal(controls.get('schedule-remove-btn').disabled, false);
});

function assertSolidGrid(view) {
  const { paint, calls, fills } = view;
  calls.length = 0;
  fills.length = 0;
  paint.drawSchedule();
  const { width, height } = paint.canvas;
  const black = fills.filter(fill => fill.color === '#000000');
  assert.equal(black.length, paint.scheduleDays + paint.scheduleClasses + 4, '每条边界均由一条实心像素线绘制');
  assert.equal(fills.filter(fill => fill.color === '#ffffff').length, 1, '课表只用纯白底，不产生灰色表头');
  assert.ok(fills.every(fill => ['#ffffff', '#000000'].includes(fill.color)));
  assert.ok(!calls.some(call => call[0] === 'stroke'), '网格不经过抗锯齿细线描边');
  const pixels = new Uint8Array(width * height);
  for (const fill of fills) {
    assert.deepEqual(fill.matrix, [1, 0, 0, 1, 0, 0], '像素线与背景使用画布原始坐标');
    assert.equal(fill.clipped, false, '外边框不受小数坐标的文字区域裁剪');
    assert.equal(fill.alpha, 1);
    assert.equal(fill.operation, 'source-over');
    assert.ok(fill.rect.every(Number.isInteger), '像素矩形全为整数');
    const [x, y, w, h] = fill.rect;
    assert.ok(x >= 0 && y >= 0 && x + w <= width && y + h <= height, '完整边框在可见画布内');
    if (fill.color !== '#000000') continue;
    assert.ok(w === 1 || h === 1, '横竖网格保持 1 像素线宽');
    for (let row = y; row < y + h; row++) pixels.fill(1, row * width + x, row * width + x + w);
  }
  const layout = paint.getScheduleLayout();
  const m = paint.scheduleTransformMatrix;
  const point = (x, y) => [
    Math.max(0, Math.min(width - 1, Math.round(layout.x + m.a * x + m.c * y))),
    Math.max(0, Math.min(height - 1, Math.round(layout.y + m.b * x + m.d * y)))
  ];
  const uninterrupted = (first, second) => {
    const [x1, y1] = point(...first);
    const [x2, y2] = point(...second);
    assert.ok(x1 === x2 || y1 === y2);
    for (let y = Math.min(y1, y2); y <= Math.max(y1, y2); y++) {
      for (let x = Math.min(x1, x2); x <= Math.max(x1, x2); x++) {
        assert.equal(pixels[y * width + x], 1, `网格在 ${x},${y} 保留纯黑像素`);
      }
    }
  };
  for (let col = 0; col <= paint.scheduleDays + 1; col++) {
    const x = col * layout.cellWidth;
    uninterrupted([x, layout.titleHeight], [x, layout.height]);
  }
  for (let row = 0; row <= paint.scheduleClasses + 1; row++) {
    const y = layout.titleHeight + row * layout.cellHeight;
    uninterrupted([0, y], [layout.width, y]);
  }
}

test('非整除尺寸的七天十二节课表所有网格都包含连续纯黑像素', () => {
  for (const [width, height] of [[128, 296], [101, 131], [299, 127], [400, 300]]) {
    const view = fixture(width, height);
    view.controls.get('schedule-days').value = '7';
    view.controls.get('schedule-classes').value = '12';
    view.paint.createSchedule();
    assertSolidGrid(view);
    view.paint.scheduleTitle = '';
    view.paint.fitScheduleToCanvas(true);
    assertSolidGrid(view);
  }
});

test('小数移动缩放与贴边、画布尺寸变化均不会让课表网格变浅或被裁断', () => {
  const view = fixture(131, 299);
  const { paint, controls } = view;
  controls.get('schedule-days').value = '7';
  controls.get('schedule-classes').value = '12';
  paint.createSchedule();
  for (const [ratio, dx, dy] of [[0.73, 0.37, 0.61], [1.1, 13.4, -12.7], [20, 1000, 1000], [0.4, -1000, -1000]]) {
    paint.zoomSchedule(ratio);
    paint.moveSchedule(dx, dy);
    assertSolidGrid(view);
  }
  paint.canvas.width = 97;
  paint.canvas.height = 173;
  assertSolidGrid(view);
  paint.fitScheduleToCanvas(true);
  assertSolidGrid(view);
});

test('八种旋转镜像方向的课表网格保持 1 像素完整黑线', () => {
  for (let turns = 0; turns < 4; turns++) {
    for (const mirrored of [false, true]) {
      const view = fixture(127, 299);
      const { paint, controls } = view;
      controls.get('schedule-days').value = '7';
      controls.get('schedule-classes').value = '12';
      paint.createSchedule();
      paint.zoomSchedule(0.81);
      paint.moveSchedule(0.23, 0.71);
      for (let turn = 0; turn < turns; turn++) {
        const oldW = paint.canvas.width;
        const oldH = paint.canvas.height;
        paint.canvas.width = oldH;
        paint.canvas.height = oldW;
        paint.transformSchedule('rotate90', oldW, oldH, oldH, oldW);
      }
      if (mirrored) paint.transformSchedule('mirror', paint.canvas.width, paint.canvas.height, paint.canvas.width, paint.canvas.height);
      assertSolidGrid(view);
      paint.zoomSchedule(10);
      paint.moveSchedule(1000, -1000);
      assertSolidGrid(view);
    }
  }
});

test('课表文字保留红蓝配色和加粗表头，纯色网格绘制后还原画布状态', () => {
  const view = fixture(400, 300);
  const { paint, calls } = view;
  paint.createSchedule();
  paint.scheduleData[1][1] = '数学';
  paint.ctx.globalAlpha = 0.4;
  paint.ctx.globalCompositeOperation = 'destination-out';
  for (const color of ['#000000', '#FF0000', '#0000FF']) {
    paint.scheduleColor = color;
    assertSolidGrid(view);
    const labels = calls.filter(call => call[0] === 'fillText');
    assert.ok(labels.every(call => call[2] === color));
    assert.ok(labels.some(call => call[1].startsWith('600 ')), '白底表头通过加粗区分');
    assert.ok(labels.some(call => !call[1].startsWith('600 ')), '课程正文保留普通字重');
    assert.equal(paint.ctx.globalAlpha, 0.4);
    assert.equal(paint.ctx.globalCompositeOperation, 'destination-out');
    assert.deepEqual(paint.ctx.matrix, [1, 0, 0, 1, 0, 0]);
    assert.equal(paint.ctx.clipped, false);
  }
});
