const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

class Pixels {
  constructor(data, width, height) {
    if (typeof data === 'number') { this.width = data; this.height = width; this.data = new Uint8ClampedArray(data * width * 4); }
    else { this.data = data; this.width = width; this.height = height; }
  }
}
const clone = image => new Pixels(new Uint8ClampedArray(image.data), image.width, image.height);
function picture(width = 32, height = 8) {
  const image = new Pixels(width, height);
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const at = (y * width + x) * 4, shade = Math.round(x * 255 / (width - 1));
    image.data.set([shade, shade, shade, 255], at);
  }
  return image;
}
function fixture() {
  const nodes = new Map(), storage = new Map(), frames = new Map(), images = [], cached = new Map();
  let frameId = 0, crop = false, cropFinishes = 0;
  const classes = () => ({ contains: () => crop, add() {}, remove() {}, toggle() {} });
  function node() {
    return { value: '', files: [], style: {}, classList: classes(), listeners: {},
      addEventListener(name, callback) { (this.listeners[name] ||= []).push(callback); },
      dispatch(name) { (this.listeners[name] || []).forEach(callback => callback({ target: this })); },
      appendChild() {}, replaceChildren() {}, setAttribute() {} };
  }
  function makeCanvas() {
    const canvas = { ...node(), width: 32, height: 8, parentNode: node() };
    canvas.getBoundingClientRect = () => ({left:0,top:0,width:canvas.width,height:canvas.height});
    const ensure = () => {
      if (!canvas.pixels || canvas.pixels.width !== canvas.width || canvas.pixels.height !== canvas.height)
        canvas.pixels = new Pixels(canvas.width, canvas.height);
    };
    const ctx = { save() {}, restore() {}, setTransform() {}, translate() {}, rotate() {}, scale() {}, transform() {},
      clearRect() {}, strokeRect() {}, beginPath() {}, moveTo() {}, lineTo() {}, stroke() {}, arc() {}, fill() {}, fillText() {},
      fillRect() { ensure(); canvas.pixels.data.fill(255); },
      getImageData() { ensure(); return clone(canvas.pixels); }, putImageData(image) { canvas.pixels = clone(image); },
      drawImage(source) {
        ensure(); const pixels = source.pixels;
        if (!pixels) return;
        for (let y = 0; y < canvas.height; y++) for (let x = 0; x < canvas.width; x++) {
          const at = (Math.floor(y * pixels.height / canvas.height) * pixels.width + Math.floor(x * pixels.width / canvas.width)) * 4;
          canvas.pixels.data.set(pixels.data.subarray(at, at + 4), (y * canvas.width + x) * 4);
        }
      }, measureText: text => ({ width: String(text).length * 8 }) };
    canvas.getContext = () => ctx;
    canvas.toDataURL = () => { ensure(); const url = 'data:canvas-' + cached.size; cached.set(url, clone(canvas.pixels)); return url; };
    return canvas;
  }
  const document = { readyState: 'test', visibilityState: 'visible', listeners: {}, body: node(),
    getElementById(id) { if (!nodes.has(id)) nodes.set(id, node()); return nodes.get(id); },
    createElement: tag => tag === 'canvas' ? makeCanvas() : node(), querySelectorAll: () => [],
    addEventListener(name, callback) { this.listeners[name] = callback; }, querySelector: () => null };
  for (const [id, value] of Object.entries({ ditherAlg: 'floydSteinberg', ditherStrength: '1', ditherContrast: '1.2',
    ditherMode: 'blackWhiteColor', canvasSize: '1.54_152_152' })) document.getElementById(id).value = value;
  document.getElementById('epddriver').options = [{ getAttribute: () => null }];
  document.getElementById('epddriver').selectedIndex = 0;
  const canvas = makeCanvas(), ctx = canvas.getContext('2d');
  const sandbox = vm.createContext({ console, ImageData: Pixels, Uint8ClampedArray, Uint8Array, Float32Array, TextDecoder,
    URLSearchParams, URL: { createObjectURL: () => 'blob:test', revokeObjectURL() {} }, Date, setTimeout, clearTimeout,
    cancelAnimationFrame: id => frames.delete(id), requestAnimationFrame: callback => { frames.set(++frameId, callback); return frameId; },
    window: { location: { search: '' } }, document,
    Image: class { constructor() { images.push(this); } },
    localStorage: { getItem: key => storage.get(key) || null, setItem: (key, value) => storage.set(key, value), removeItem: key => storage.delete(key) },
    testCanvas: canvas, testContext: ctx, cropState: () => crop, finishCrop: callback => { cropFinishes++; callback(); } });
  for (const file of ['main', 'dithering', 'paint'])
    vm.runInContext(fs.readFileSync(`html/js/${file}.js`, 'utf8'), sandbox);
  const run = code => vm.runInContext(code, sandbox);
  run('canvas = testCanvas; ctx = testContext; paintManager = new PaintManager(canvas, ctx); cropManager = {isCropMode: cropState, finishCrop}; addLog = () => {}; showToast = () => {};');
  const paint = run('paintManager');
  paint.drawSchedule = () => {}; paint.updateSelectionOverlay = () => {}; paint.renderTodoList = () => {};
  paint.syncUI = () => {};
  run('initEventHandlers()');
  const input = (id, value, event) => { const control = document.getElementById(id); control.value = String(value); if (event) control.dispatch(event); };
  return { sandbox, run, paint, canvas, ctx, document, input, nodes, storage, images, cached, frames,
    setCrop: value => { crop = value; }, cropFinishes: () => cropFinishes,
    renderFrames() { for (const [id, callback] of frames) { frames.delete(id); callback(); } },
    load(image = picture()) { ctx.putImageData(image); paint.captureBaseImage(); run('convertDithering()'); } };
}

test('contrast and every dither algorithm change the original gradient without modifying it', () => {
  const f = fixture(), original = picture(), before = clone(original);
  const render = (algorithm, strength, contrast) => f.sandbox.renderImageAdjustments(original,
    { algorithm, strength, contrast, mode: 'blackWhiteColor' });
  for (const algorithm of ['floydSteinberg', 'atkinson', 'stucki', 'jarvis', 'bayer']) {
    assert.notDeepEqual(render(algorithm, 0, 1).data, render(algorithm, 1, 1).data, algorithm);
    assert.notDeepEqual(render(algorithm, 1, 0.5).data, render(algorithm, 1, 2).data, algorithm);
  }
  assert.notDeepEqual(render('none', 0, 0.5).data, render('none', 0, 2).data);
  assert.deepEqual(original.data, before.data);
});

test('small Floyd-Steinberg errors retain fractional precision instead of disappearing', () => {
  const f = fixture(), image = new Pixels(8, 1);
  for (let x = 0; x < 8; x++) image.data.set([145, 145, 145, 255], x * 4);
  const result = f.sandbox.ditherImage(image, 'floydSteinberg', 0.1, 'blackWhiteColor');
  assert.deepEqual(Array.from(result.data).filter((_, index) => index % 4 === 0), [255,255,0,255,255,0,255,255]);
});

test('black-white classification never uses chromatic palette entries, and transparency has a white backing', () => {
  const f = fixture();
  assert.equal(f.sandbox.findClosestColor(0, 255, 0, 'blackWhiteColor').r, 255);
  assert.equal(f.sandbox.findClosestColor(255, 0, 0, 'blackWhiteColor').r, 0);
  const transparent = new Pixels(Uint8ClampedArray.of(0,0,0,0), 1, 1);
  const rendered = f.sandbox.renderImageAdjustments(transparent, { algorithm: 'none', contrast: 1, strength: 0, mode: 'blackWhiteColor' });
  assert.deepEqual(Array.from(rendered.data), [255,255,255,255]);
});

test('preview pixels round-trip the exported palette bytes for every mode and algorithm, including odd widths', () => {
  const f = fixture(), original = new Pixels(3, 2);
  [[0,0,0], [255,255,255], [255,0,0], [255,255,0], [0,0,255], [41,204,20]].forEach((color, index) => original.data.set([...color,255], index * 4));
  for (const mode of ['blackWhiteColor', 'threeColor', 'fourColor', 'sixColor'])
    for (const algorithm of ['none', 'floydSteinberg', 'atkinson', 'stucki', 'jarvis', 'bayer']) {
      const preview = f.sandbox.renderImageAdjustments(original, { mode, algorithm, strength: 1, contrast: 1 });
      const packed = f.sandbox.processImageData(preview, mode);
      const decoded = f.sandbox.decodeProcessedData(packed, original.width, original.height, mode);
      assert.deepEqual(decoded.data, preview.data, `${mode}/${algorithm}`);
    }
});

test('partial row bytes remain white and red pixels never activate both planes', () => {
  const f = fixture(), image = new Pixels(3, 2);
  image.data.fill(255); image.data.set([200,0,0,255], 0); image.data.set([0,0,0,255], 4);
  const bytes = f.sandbox.processImageData(image, 'threeColor');
  assert.deepEqual(Array.from(bytes), [0xbf,0xff,0x7f,0xff]);
  const bw = f.sandbox.processImageData(image, 'blackWhiteColor');
  assert.equal(bw[0] & 0x1f, 0x1f); assert.equal(bw[1], 0xff);
});

test('repeated adjustments and changing color mode preserve the source and exactly restore prior effects', () => {
  const f = fixture(); f.load();
  const raw = clone(f.paint.baseImageData), preview = clone(f.paint.processedBaseImageData);
  f.input('ditherContrast', 0.5); f.run('applyDither()');
  assert.notDeepEqual(f.paint.processedBaseImageData.data, preview.data);
  f.input('ditherStrength', 0); f.run('applyDither()');
  f.input('ditherStrength', 1); f.input('ditherContrast', 1.2); f.run('applyDither()');
  assert.deepEqual(f.paint.baseImageData.data, raw.data); assert.deepEqual(f.paint.processedBaseImageData.data, preview.data);
  const red = new Pixels(2,1); red.data.set([255,0,0,255,255,255,255,255]);
  f.canvas.width = 2; f.canvas.height = 1; f.ctx.putImageData(red); f.paint.captureBaseImage();
  for (const mode of ['threeColor','blackWhiteColor','threeColor']) { f.input('ditherMode', mode); f.run('applyDither()'); }
  assert.equal(f.paint.processedBaseImageData.data[0], 255); assert.equal(f.paint.processedBaseImageData.data[1], 0);
});

test('slider input is batched, one gesture is undoable and redo restores settings plus source', () => {
  const f = fixture(); f.load(); const original = clone(f.paint.baseImageData), initial = clone(f.paint.processedBaseImageData);
  const count = f.paint.historyStack.length;
  for (const contrast of [0.5,0.6,0.7]) f.input('ditherContrast', contrast, 'input');
  assert.equal(f.frames.size, 1); f.renderFrames(); assert.equal(f.paint.historyStack.length, count);
  const adjusted = clone(f.paint.processedBaseImageData);
  f.nodes.get('ditherContrast').dispatch('change'); assert.equal(f.paint.historyStack.length, count + 1);
  f.paint.undo(); assert.equal(Number(f.nodes.get('ditherContrast').value), 1.2); assert.deepEqual(f.paint.processedBaseImageData.data, initial.data);
  f.paint.redo(); assert.equal(Number(f.nodes.get('ditherContrast').value), 0.7); assert.deepEqual(f.paint.processedBaseImageData.data, adjusted.data);
  assert.deepEqual(f.paint.baseImageData.data, original.data);
});

test('undo flushes an unfinished slider gesture and export uses its latest settings', () => {
  const f = fixture(); f.load(); const before = clone(f.paint.processedBaseImageData);
  f.input('ditherContrast', 0.5, 'input'); f.paint.undo();
  assert.equal(f.frames.size, 0); assert.deepEqual(f.paint.processedBaseImageData.data, before.data);
  f.input('ditherContrast', 2, 'input'); f.run('prepareCanvasExport()');
  assert.equal(f.frames.size, 0); assert.equal(f.paint.imageAdjustments.contrast, 2);
});

test('cache stores the original image and parameters, and reload derives the same editable preview', async () => {
  const f = fixture(); f.load(); f.input('ditherContrast', 0.5); f.run('applyDither()');
  const raw = clone(f.paint.baseImageData), preview = clone(f.paint.processedBaseImageData);
  const cache = JSON.parse(f.storage.get('canvasState'));
  assert.deepEqual(f.cached.get(cache.baseDataUrl).data, raw.data); assert.equal(cache.imageAdjustments.contrast, 0.5);
  f.paint.resetBaseImage(); const pending = f.paint.loadCanvasFromLocalStorage();
  const image = f.images.at(-1); image.pixels = f.cached.get(cache.baseDataUrl); image.onload();
  assert.equal(await pending, true); assert.deepEqual(f.paint.baseImageData.data, raw.data);
  assert.deepEqual(f.paint.processedBaseImageData.data, preview.data);
});

test('a new brush stroke commits the pending adjustment as a separate undo step', () => {
  const f = fixture(); f.load(); f.input('ditherContrast',0.5,'input');
  f.paint.currentTool = 'brush'; f.paint.startPaint({clientX:2,clientY:2,button:0}); f.paint.endPaint();
  assert.equal(f.frames.size,0); assert.equal(f.paint.lineSegments.length,1);
  f.paint.undo(); assert.equal(f.paint.lineSegments.length,0); assert.equal(f.paint.imageAdjustments.contrast,0.5);
  f.paint.undo(); assert.equal(f.paint.imageAdjustments.contrast,1.2);
});

test('parameter changes keep crop mode open until completion and no-dither disables strength', () => {
  const f = fixture(); f.load(); f.setCrop(true);
  f.input('ditherContrast', 0.5, 'input'); f.run('applyDither()');
  assert.equal(f.cropFinishes(), 0); assert.equal(f.frames.size, 0);
  f.input('ditherAlg', 'none'); f.run('applyDither()'); assert.equal(f.nodes.get('ditherStrength').disabled, true);
  f.run('applyDither(true)'); assert.equal(f.cropFinishes(), 1);
});

test('canvas resize keeps the current source instead of rereading the selected file', () => {
  const f = fixture(); f.load(); f.document.getElementById('imageFile').files = [{ name: 'old.png' }];
  f.paint.textElements.push({ text: '标题', x: 16, y: 4, font: '12px Arial' });
  f.run('updateCanvasSize()'); assert.equal(f.images.length, 0);
  assert.equal(f.paint.baseImageData.width, 152); assert.equal(f.paint.baseImageData.height, 152);
  assert.equal(f.paint.textElements[0].x, 76); assert.equal(f.paint.textElements[0].y, 76);
});

for (const [mode, hint] of [['sixColor', '六色'], ['fourColor', '四色']]) {
  test(`${mode} sends stop before device negotiation or any transfer`, async () => {
    const f = fixture(); f.load(); f.input('ditherMode', mode);
    const messages = [];
    f.sandbox.recordMessage = message => messages.push(message);
    f.run('addLog = recordMessage; showToast = recordMessage;');
    let negotiation = 0; f.sandbox.waitForNegotiatedMtu = () => { negotiation++; throw new Error('must not negotiate'); };
    await f.run('sendImageOperation({})'); assert.equal(negotiation, 0);
    assert.ok(messages.includes(`当前固件不支持发送${hint}图片；可下载预览或导出数组。`));
  });
}
