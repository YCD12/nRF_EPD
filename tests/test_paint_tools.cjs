const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function setup() {
  const elements = new Map(), storage = new Map(), images = [], draw = [];
  const classes = () => ({ values: new Set(), toggle(name, force) {
    if (force === undefined) force = !this.values.has(name);
    if (force) this.values.add(name); else this.values.delete(name);
  }, add(name) { this.values.add(name); }, remove(name) { this.values.delete(name); }, contains(name) { return this.values.has(name); } });
  function element(tagName = 'DIV') {
    return { tagName, value: '', textContent: '', style: { setProperty(name, value) { this[name] = value; } },
      attributes: {}, classList: classes(), children: [], listeners: {}, dataset: {},
      addEventListener(event, listener) { (this.listeners[event] ||= []).push(listener); },
      dispatch(event, payload = {}) { (this.listeners[event] || []).forEach(listener => listener({ target: this, ...payload })); },
      setAttribute(name, value) { this.attributes[name] = value; }, getAttribute(name) { return this.attributes[name]; },
      append(...children) { this.children.push(...children); }, appendChild(child) { this.children.push(child); },
      replaceChildren(...children) { this.children = children; }, focus() { this.focused = true; },
      getBoundingClientRect() { return { left: 10, top: 20, width: 400, height: 300, right: 410, bottom: 320 }; }
    };
  }
  function context() {
    return { font: '20px Arial',
      save() {}, restore() {}, setTransform() {}, transform() {}, translate() {}, setLineDash() {},
      beginPath() {}, moveTo() {}, lineTo() {}, arc(x, y, radius) { draw.push(['arc', x, y, radius]); },
      fill() { draw.push(['fill', this.fillStyle]); }, stroke() { draw.push(['stroke', this.strokeStyle]); },
      fillRect() { draw.push(['background', this.fillStyle]); }, clearRect() {}, strokeRect(...args) { draw.push(['box', ...args]); },
      fillText(text, x, y) { draw.push(['text', text, x, y]); },
      measureText(text) { return { width: String(text).length * (Number(this.font.match(/([\d.]+)px/)?.[1]) || 20) * 0.6 }; },
      drawImage() { draw.push(['image']); }, putImageData(pixels) { draw.push(['base', pixels.label]); },
      getImageData(x, y, width, height) { return { width, height, data: new Uint8ClampedArray(width * height * 4), label: 'photo' }; },
      createImageData(width, height) { return { width, height, data: new Uint8ClampedArray(width * height * 4) }; }
    };
  }
  const defaults = { 'brush-color': '#000000', 'brush-size': 2, 'eraser-size': 16,
    'font-family': 'Microsoft YaHei', 'font-size': 20, 'text-color': '#000000', 'text-align': 'left', 'text-line-height': 1.35,
    'todo-font-family': 'Microsoft YaHei', 'todo-font-size': 18, 'todo-color': '#000000', 'todo-spacing': 1.6 };
  for (const [id, value] of Object.entries(defaults)) {
    const input = element('INPUT'); input.value = value; elements.set(id, input);
  }
  const document = { body: element('BODY'), listeners: {},
    getElementById(id) { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); },
    querySelectorAll() { return []; }, addEventListener(name, callback) { this.listeners[name] = callback; },
    createElement(tag) {
      const node = element(tag.toUpperCase());
      if (tag === 'canvas') { node.getContext = () => context(); node.toDataURL = () => 'data:image/png;base64,mock'; }
      return node;
    } };
  const canvas = element('CANVAS'); canvas.width = 400; canvas.height = 300;
  canvas.parentNode = element(); canvas.toDataURL = () => 'data:image/png;base64,mock';
  const ctx = context();
  let orientation = 0;
  const globals = { document, console, Date, Promise, Uint8ClampedArray, setTimeout,
    localStorage: { getItem: key => storage.get(key) || null, setItem: (key, value) => storage.set(key, value), removeItem: key => storage.delete(key) },
    Image: class { constructor() { images.push(this); } },
    setCanvasTitle() {}, captureCanvasOrientation: () => orientation,
    restoreCanvasOrientation: value => { orientation = value; }
  };
  const sandbox = vm.createContext(globals);
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../html/js/paint.js'), 'utf8') + '\nglobalThis.PaintClass = PaintManager;', sandbox);
  const paint = new sandbox.PaintClass(canvas, ctx);
  return { paint, canvas, ctx, elements, storage, images, draw, document,
    input: (id, value) => { document.getElementById(id).value = value; },
    point: (x, y) => ({ clientX: x + 10, clientY: y + 20, button: 0 }),
    setOrientation: value => { orientation = value; }, orientation: () => orientation };
}

test('brush strokes starting at zero connect correctly, and eraser keeps its own color and size', () => {
  const s = setup(), p = s.paint;
  p.setActiveTool('brush'); p.brushColor = '#FF0000'; p.brushSize = 3;
  p.startPaint(s.point(0, 0)); p.paint(s.point(10, 10)); p.endPaint();
  assert.equal(p.lineSegments[0].type, 'dot'); assert.equal(p.lineSegments[1].type, 'line');
  assert.deepEqual(Array.from([p.lineSegments[1].x1, p.lineSegments[1].y1]), [0, 0]);
  p.setActiveTool('eraser'); p.eraserSize = 19; p.startPaint(s.point(20, 20)); p.endPaint();
  assert.equal(p.lineSegments[2].color, '#FFFFFF'); assert.equal(p.lineSegments[2].size, 19);
  p.setActiveTool('brush'); assert.equal(p.brushColor, '#FF0000'); assert.equal(p.brushSize, 3);
});

test('switching tools preserves base image, timetable and all editable objects', () => {
  const s = setup(), p = s.paint;
  p.baseImageData = { width: 400, height: 300, label: 'photo' };
  p.scheduleData = [['节次', '周一'], ['1', '语文']];
  p.textElements = [{ text: '标题', x: 50, y: 50, font: '20px Arial', color: '#000000', renderOrder: 1 }];
  p.loadCanvasFromLocalStorage = () => { throw new Error('must not reload during tool switch'); };
  for (const tool of ['brush', 'eraser', 'text', 'todo', 'schedule', null]) p.setActiveTool(tool);
  assert.equal(p.scheduleData[1][1], '语文'); assert.equal(p.textElements[0].text, '标题');
  p.redrawAll(); assert.ok(s.draw.some(op => op[0] === 'base' && op[1] === 'photo'));
});

test('multiline aligned text can be dragged and edited while retaining transforms', () => {
  const s = setup(), p = s.paint;
  p.setActiveTool('text'); s.input('text-input', '第一行\n第二行'); s.input('text-align', 'center'); s.input('text-line-height', 2);
  p.startTextPlacement(); p.placeText(s.point(150, 70));
  const item = p.textElements[0]; assert.equal(item.align, 'center');
  const textCalls = s.draw.filter(op => op[0] === 'text');
  assert.ok(textCalls.some(op => op[1] === '第二行' && op[3] === 40));
  item.a = 0; item.b = 1; item.c = -1; item.d = 0;
  p.selectedTextElement = item; p.dragOffsetX = 0; p.dragOffsetY = 0;
  p.dragText(s.point(180, 100));
  assert.equal(item.x, 180); assert.equal(item.y, 100); assert.equal(item.b, 1);
  s.input('text-input', '改成三行\n第二行\n第三行'); p.updateSelectedText();
  assert.equal(item.b, 1); assert.match(item.text, /第三行/);
  assert.ok(s.draw.filter(op => op[0] === 'background').length >= 2);
  p.deleteSelectedText(); assert.equal(p.textElements.length, 0); p.undo(); assert.equal(p.textElements.length, 1);
});

test('white eraser stays above existing text after every redraw and new text can cover it', () => {
  const s = setup(), p = s.paint;
  p.setActiveTool('text'); s.input('text-input', '原文字'); p.placeText(s.point(50, 70));
  p.setActiveTool('eraser'); p.startPaint(s.point(50, 70)); p.endPaint();
  s.draw.length = 0; p.redrawAll();
  const oldText = s.draw.findIndex(op => op[0] === 'text' && op[1] === '原文字');
  const erased = s.draw.findIndex(op => op[0] === 'fill' && op[1] === '#FFFFFF');
  assert.ok(erased > oldText);
  p.setActiveTool('text'); s.input('text-input', '新文字'); p.placeText(s.point(50, 70));
  s.draw.length = 0; p.redrawAll();
  assert.ok(s.draw.findIndex(op => op[0] === 'text' && op[1] === '新文字') > s.draw.findIndex(op => op[0] === 'fill' && op[1] === '#FFFFFF'));
});

test('todo batches are real checkboxes with editable order and no export delete glyph', () => {
  const s = setup(), p = s.paint;
  p.setActiveTool('todo'); s.input('todo-input', '第一项\n\n第二项\n第三项'); p.startTodoPlacement(); p.placeTodo(s.point(20, 30));
  assert.equal(p.todoItems.length, 3); assert.ok(p.todoItems[1].y > p.todoItems[0].y);
  const list = s.document.getElementById('todo-list'); assert.equal(list.children.length, 3);
  assert.equal(list.children[0].children[0].type, 'checkbox');
  list.children[0].children[0].dispatch('change'); assert.equal(p.todoItems[0].completed, true);
  const original = p.todoItems[2]; p.moveTodoItem(original, -1); assert.equal(p.todoItems[1], original);
  p.editTodoItem(original); s.input('todo-input', '修改后的待办'); p.startTodoPlacement(); assert.equal(original.text, '修改后的待办');
  p.alignTodoItems(); assert.equal(p.todoItems[0].x, p.todoItems[2].x);
  p.clearCompletedTodos(); assert.equal(p.todoItems.length, 2);
  s.draw.length = 0; p.prepareExport(); assert.ok(s.draw.some(op => op[0] === 'box'));
  assert.equal(s.draw.some(op => op[0] === 'text' && op[1] === '×'), false);
});

test('inline todo content commit retains DOM buttons for the immediate following click', () => {
  const s = setup(), p = s.paint;
  p.setActiveTool('todo'); s.input('todo-input', '旧内容'); p.placeTodo(s.point(20, 30));
  const list = s.document.getElementById('todo-list'), row = list.children[0];
  const input = row.children[1], remove = row.children[2].children[3];
  input.value = '新内容'; input.dispatch('change');
  assert.equal(list.children[0], row); assert.equal(p.todoItems[0].text, '新内容');
  remove.dispatch('click'); assert.equal(p.todoItems.length, 0);
});

test('history restores dimensions, orientation, schedule settings and todo panel state', () => {
  const s = setup(), p = s.paint;
  p.saveToHistory(); s.canvas.width = 300; s.canvas.height = 400; s.setOrientation(1);
  p.scheduleFontSize = 19; p.scheduleDays = 7; p.scheduleData = [['课表']];
  p.todoItems = [{ text: '完成项', completed: true, x: 20, y: 30, font: '18px Arial', color: '#000000' }];
  p.saveToHistory(); p.undo(); assert.equal(s.canvas.width, 400); assert.equal(s.orientation(), 0); assert.equal(p.todoItems.length, 0);
  p.redo(); assert.equal(s.canvas.height, 400); assert.equal(s.orientation(), 1); assert.equal(p.scheduleDays, 7); assert.equal(p.scheduleFontSize, 19);
  assert.match(s.document.getElementById('todo-summary').textContent, /已完成 1/);
  assert.equal(JSON.parse(s.storage.get('canvasState')).orientation, 1);
});

test('todo controls target the current restored model after undoing an unrelated brush stroke', () => {
  const s = setup(), p = s.paint;
  p.saveToHistory(); p.setActiveTool('todo'); s.input('todo-input', '有效待办'); p.placeTodo(s.point(20, 30));
  const oldItem = p.todoItems[0];
  p.setActiveTool('brush'); p.startPaint(s.point(200, 200)); p.endPaint(); p.undo();
  assert.notEqual(p.todoItems[0], oldItem);
  const list = s.document.getElementById('todo-list');
  list.children[0].children[0].dispatch('change'); assert.equal(p.todoItems[0].completed, true);
  assert.equal(oldItem.completed, false);
  list.children[0].children[2].children[3].dispatch('click'); assert.equal(p.todoItems.length, 0);
});

test('keyboard undo stays inside form controls and crop interaction is left to crop manager', () => {
  const s = setup(), p = s.paint;
  let undos = 0, prevented = 0; p.undo = () => undos++;
  for (const tagName of ['TEXTAREA', 'INPUT', 'SELECT']) p.handleKeyboard({ key: 'z', ctrlKey: true, target: { tagName }, preventDefault() { prevented++; } });
  assert.equal(undos, 0); assert.equal(prevented, 0);
  p.handleKeyboard({ key: 'z', ctrlKey: true, target: { tagName: 'CANVAS' }, preventDefault() { prevented++; } });
  assert.equal(undos, 1);
  p.setActiveTool('brush'); s.canvas.parentNode.classList.add('crop-mode'); p.startPaint(s.point(10, 10)); assert.equal(p.lineSegments.length, 0);
});

test('cache retains all strokes, restores nondefault canvas, and rejects a late bitmap over new ink', async () => {
  const s = setup(), p = s.paint;
  p.lineSegments = Array.from({ length: 150 }, (_, index) => ({ type: 'dot', x: index, y: 20, size: 2, color: '#000000' }));
  p.saveCanvasToLocalStorage(); assert.equal(JSON.parse(s.storage.get('canvasState')).lineSegments.length, 150);
  s.storage.set('canvasState', JSON.stringify({ version: 2, width: 300, height: 400, orientation: 1, textElements: [], lineSegments: [], todoItems: [] }));
  assert.equal(p.loadCanvasFromLocalStorage(), true); assert.equal(s.canvas.width, 300); assert.equal(s.orientation(), 1);
  s.storage.set('canvasState', JSON.stringify({ version: 2, width: 300, height: 400, baseDataUrl: 'data:old', lineSegments: [], textElements: [], todoItems: [] }));
  const restore = p.loadCanvasFromLocalStorage(); p.setActiveTool('brush'); p.startPaint(s.point(10, 10)); p.endPaint();
  s.images.at(-1).onload(); assert.equal(await restore, false); assert.equal(p.lineSegments.length, 1);
});

test('legacy composed image caches preserve the bitmap without rendering duplicate vectors', async () => {
  const s = setup(), p = s.paint;
  s.storage.set('canvasState', JSON.stringify({ width: 400, height: 300, imageDataUrl: 'data:legacy', textElements: [{ text: '重复文字' }], lineSegments: [], todoItems: [{ text: '重复待办' }], scheduleData: [['重复课表']] }));
  const restore = p.loadCanvasFromLocalStorage(); s.images.at(-1).onload(); assert.equal(await restore, true);
  assert.equal(p.textElements.length, 0); assert.equal(p.todoItems.length, 0); assert.equal(p.scheduleData, null);
  assert.ok(p.baseImageData);
});

test('resize scales editable coordinates and affine text bounds without losing their model', () => {
  const s = setup(), p = s.paint;
  const text = { text: '保留', x: 100, y: 100, font: '20px Arial', color: '#000000', a: 0, b: 1, c: -1, d: 0 };
  p.textElements.push(text); p.lineSegments.push({ type: 'line', x1: 100, y1: 100, x2: 200, y2: 200, size: 4 });
  p.resizeElements(400, 300, 200, 150);
  assert.equal(text.x, 50); assert.equal(text.y, 50); assert.equal(text.b, 0.5); assert.equal(text.c, -0.5);
  assert.equal(p.lineSegments[0].x2, 100); assert.equal(p.lineSegments[0].size, 2);
});
