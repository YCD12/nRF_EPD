// BWR（黑/白/红）三色抖动分类回归：验证调色板最近色分类、双平面互斥，
// 以及“预览像素（decodeProcessedData）→ 发送平面（processImageData）”
// 使用同一分类函数。modern 页与 legacy 页分别加载、分别断言。
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

class Pixels {
  constructor(data, width, height) {
    if (typeof data === 'number') { this.width = data; this.height = width; this.data = new Uint8ClampedArray(data * width * 4); }
    else { this.data = data; this.width = width; this.height = height; }
  }
}

function loadDithering(file) {
  const sandbox = vm.createContext({ console, ImageData: Pixels, Uint8ClampedArray, Uint8Array, Float32Array });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '..', file), 'utf8'), sandbox);
  return { run: code => vm.runInContext(code, sandbox), sandbox };
}

const modern = loadDithering('html/js/dithering.js');
const legacy = loadDithering('html/legacy/js/dithering.js');

const THREE_COLOR_BLACK = 0x00, THREE_COLOR_WHITE = 0x01, THREE_COLOR_RED = 0x02;

function image(width, height, colors) {
  const data = new Uint8ClampedArray(width * height * 4);
  for (let i = 0; i < width * height; i++) {
    const [r, g, b] = colors[i] || [255, 255, 255];
    data.set([r, g, b, 255], i * 4);
  }
  return new Pixels(data, width, height);
}

// 确定性的混合测试图：灰阶 + 各类饱和/暗色，覆盖分类边界与红色优先路径。
function reviewImage(width = 64, height = 48) {
  const colors = [];
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const gray = (x * 17 + y * 29) % 256;
    switch ((x * 3 + y * 7) % 6) {
      case 0: colors.push([gray, gray, gray]); break;
      case 1: colors.push([gray, 0, 0]); break;
      case 2: colors.push([255, gray, gray]); break;
      case 3: colors.push([gray, gray, 0]); break;
      case 4: colors.push([0, 0, gray]); break;
      default: colors.push([255 - gray, gray / 2, 0]); break;
    }
  }
  return image(width, height, colors);
}

function classify(run, r, g, b) {
  return run(`findClosestColor(${r}, ${g}, ${b}, 'threeColor')`).value;
}

function planes(run, bytes, width, height) {
  const byteWidth = Math.ceil(width / 8), planeLength = byteWidth * height;
  assert.equal(bytes.length, planeLength * 2, 'three-color output is black/white plane + red plane');
  const bw = [], red = [];
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const byteIndex = y * byteWidth + Math.floor(x / 8), bitIndex = 7 - (x % 8);
    bw.push((bytes[byteIndex] >> bitIndex) & 1);
    red.push((bytes[planeLength + byteIndex] >> bitIndex) & 1);
  }
  return { bw, red };
}

function expectedBits(closestValue) {
  const isRed = closestValue === THREE_COLOR_RED;
  // UC8179 黑/红平面低有效：红像素黑/白平面必须写白(1)，红平面写 0。
  return { bw: isRed || closestValue === THREE_COLOR_WHITE ? 1 : 0, red: isRed ? 0 : 1 };
}

function assertNoDualPlane(run, width, height, label) {
  const source = reviewImage(width, height);
  const bytes = run('processImageData')(source, 'threeColor');
  const { bw, red } = planes(run, bytes, width, height);
  for (let i = 0; i < bw.length; i++)
    assert.ok(bw[i] === 1 || red[i] === 1,
      `${label}: pixel ${i} activates both the black and red plane`);
  return { source, bytes, bw, red };
}

test('modern three-color classification is the black/white/red palette nearest color', () => {
  const { run } = modern;
  // 任务要求的关键点：暗红必须判红；灰按亮度落到黑/白；纯色精确匹配。
  assert.equal(classify(run, 150, 20, 20), THREE_COLOR_RED, '暗红 (150,20,20)');
  assert.equal(classify(run, 255, 0, 0), THREE_COLOR_RED, '纯红 (255,0,0)');
  assert.equal(classify(run, 255, 255, 255), THREE_COLOR_WHITE, '纯白 (255,255,255)');
  assert.equal(classify(run, 0, 0, 0), THREE_COLOR_BLACK, '纯黑 (0,0,0)');
  assert.equal(classify(run, 128, 128, 128) !== THREE_COLOR_RED, true, '灰 (128,128,128) 不得判红');
  assert.equal(classify(run, 90, 90, 90), THREE_COLOR_BLACK, '暗灰按亮度落黑');
  assert.equal(classify(run, 170, 170, 170), THREE_COLOR_WHITE, '亮灰按亮度落白');
  // 与四色共用最近色实现：三色不再有独立的亮度/红色阈值分支。
  assert.equal(run('findClosestColor(150,20,20,"threeColor") === threeColorPalette[2]'), true);
});

test('modern three-color bytes encode the required pixels with red-first mutual exclusion', () => {
  const { run } = modern;
  const source = image(8, 2, [
    [150, 20, 20], [128, 128, 128], [255, 0, 0], [255, 255, 255],
    [0, 0, 0], [90, 90, 90], [170, 170, 170], [150, 20, 20],
    // 第二行全白，用来验证按行打包
  ]);
  const bytes = run('processImageData')(source, 'threeColor');
  // 预期值由同一分类函数 + red-first 编码规则推导，避免把 Lab 实现细节写死。
  const bw = [], red = [];
  for (let i = 0; i < 8; i++) {
    const color = source.data.slice(i * 4, i * 4 + 3);
    const bits = expectedBits(classify(run, color[0], color[1], color[2]));
    bw.push(bits.bw); red.push(bits.red);
  }
  bw.push(1, 1, 1, 1, 1, 1, 1, 1);
  red.push(1, 1, 1, 1, 1, 1, 1, 1);
  const pack = bits => bits.reduce((byte, bit) => (byte << 1) | bit, 0);
  assert.deepEqual(Array.from(bytes), [
    pack(bw.slice(0, 8)), pack(bw.slice(8, 16)),
    pack(red.slice(0, 8)), pack(red.slice(8, 16)),
  ]);
  const { bw: encodedBw, red: encodedRed } = planes(run, bytes, 8, 2);
  // 逐像素确认关键对照值（黑/白平面 bit=1 表示白，红平面 bit=0 表示红）。
  assert.deepEqual([encodedBw[0], encodedRed[0]], [1, 0], '暗红是红：黑白平面白、红平面激活');
  assert.deepEqual([encodedBw[2], encodedRed[2]], [1, 0], '纯红是红');
  assert.deepEqual([encodedBw[3], encodedRed[3]], [1, 1], '纯白');
  assert.deepEqual([encodedBw[4], encodedRed[4]], [0, 1], '纯黑');
  assert.deepEqual([encodedBw[1], encodedRed[1]], [1, 1], '灰 128 落白且不激活红平面');
  for (let i = 0; i < encodedBw.length; i++)
    assert.ok(encodedBw[i] === 1 || encodedRed[i] === 1, `pixel ${i} 不允许黑+红双平面同时激活`);
});

test('modern preview restore and send planes agree for every pixel and dither algorithm', () => {
  const { run } = modern;
  const source = reviewImage(32, 16);
  const encode = run('processImageData'), decode = run('decodeProcessedData');
  for (const algorithm of ['none', 'floydSteinberg', 'atkinson', 'bayer']) {
    const input = new Pixels(new Uint8ClampedArray(source.data), source.width, source.height);
    const rendered = run('ditherImage')(input, algorithm, 1, 'threeColor');
    const packed = encode(rendered, 'threeColor');
    const preview = decode(packed, source.width, source.height, 'threeColor');
    // 抖动结果本身已是调色板像素，预览还原必须逐像素一致。
    assert.deepEqual(Array.from(preview.data), Array.from(rendered.data), `${algorithm}: preview pixels`);
    // 发送时对预览画布重新分类，必须得到同样的平面字节。
    assert.deepEqual(Array.from(encode(preview, 'threeColor')), Array.from(packed), `${algorithm}: send planes`);
  }
});

test('modern three-color planes match findClosestColor for all pixels and never activate both', () => {
  const { run } = modern;
  const { source, bytes, bw, red } = assertNoDualPlane(run, 64, 48, 'modern');
  for (let i = 0; i < bw.length; i++) {
    const at = i * 4;
    const bits = expectedBits(classify(run, source.data[at], source.data[at + 1], source.data[at + 2]));
    assert.equal(bw[i], bits.bw, `modern pixel ${i} black/white plane`);
    assert.equal(red[i], bits.red, `modern pixel ${i} red plane`);
  }
});

test('legacy three-color preview classification and send planes now agree (red-first mutex)', () => {
  const { run } = legacy;
  // legacy 保留自己的分类阈值（红: r>120 且显著高于 g/b；否则亮度 <128 为黑），
  // 但 processImageData 必须复用 findClosestColor，不能另算一套。
  assert.equal(classify(run, 150, 20, 20), THREE_COLOR_RED, '暗红 (150,20,20)');
  assert.equal(classify(run, 200, 0, 0), THREE_COLOR_RED, '亮红 (200,0,0)');
  assert.equal(classify(run, 255, 255, 255), THREE_COLOR_WHITE, '纯白');
  assert.equal(classify(run, 0, 0, 0), THREE_COLOR_BLACK, '纯黑');
  assert.equal(classify(run, 128, 128, 128) !== THREE_COLOR_RED, true, '灰 128 不得判红');
  assert.equal(classify(run, 160, 160, 160), THREE_COLOR_WHITE, '亮灰按亮度落白');
  assert.equal(classify(run, 100, 100, 100), THREE_COLOR_BLACK, '灰 100 按亮度落黑');

  const source = image(8, 1, [
    [150, 20, 20], [200, 0, 0], [255, 0, 0], [255, 255, 255],
    [0, 0, 0], [128, 128, 128], [100, 100, 100], [90, 90, 90],
  ]);
  const bytes = run('processImageData')(source, 'threeColor');
  const { bw, red } = planes(run, bytes, 8, 1);
  // 修复点：旧实现里 200,0,0 会同时写黑平面(bit=0)与红平面(bit=0)。
  for (let i = 0; i < bw.length; i++)
    assert.ok(bw[i] === 1 || red[i] === 1, `legacy pixel ${i} 不允许黑+红双平面同时激活`);
  // 与 findClosestColor 分类逐像素一致（预览=发送）。
  for (let i = 0; i < 8; i++) {
    const at = i * 4;
    const bits = expectedBits(classify(run, source.data[at], source.data[at + 1], source.data[at + 2]));
    assert.deepEqual([bw[i], red[i]], [bits.bw, bits.red], `legacy pixel ${i}`);
  }
  assert.deepEqual([bw[0], red[0]], [1, 0], 'legacy 暗红按红发送');
  assert.deepEqual([bw[1], red[1]], [1, 0], 'legacy 亮红按红发送（旧实现会黑+红双激活）');
});

test('legacy preview restore and send planes agree and never activate both', () => {
  const { run } = legacy;
  const source = reviewImage(48, 24);
  const encode = run('processImageData'), decode = run('decodeProcessedData');
  const packed = encode(source, 'threeColor');
  const preview = decode(packed, source.width, source.height, 'threeColor');
  assert.deepEqual(Array.from(encode(preview, 'threeColor')), Array.from(packed), 'legacy preview->send planes');
  const { bw, red } = planes(run, packed, source.width, source.height);
  for (let i = 0; i < bw.length; i++) {
    assert.ok(bw[i] === 1 || red[i] === 1, `legacy pixel ${i} 双平面激活`);
    const at = i * 4;
    const bits = expectedBits(classify(run, source.data[at], source.data[at + 1], source.data[at + 2]));
    assert.deepEqual([bw[i], red[i]], [bits.bw, bits.red], `legacy pixel ${i} 分类/平面一致`);
  }
});
