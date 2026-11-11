const assert = require('node:assert/strict');
(async () => {
  const D = await import('./dither.mts');
  const source = new Uint8ClampedArray([80, 100, 120, 231, 160, 60, 40, 255]);
  const neutral = new D.ProcessOptions(); neutral.contrast = 1.2;
  const expected = new Uint8ClampedArray(source); D.adjustContrast(expected, 1.2);
  const actual = new Uint8ClampedArray(source); D.applyImageTone(actual, neutral);
  assert.deepEqual(actual, expected);
  for (const [key, value] of [['brightness', 0.2], ['gamma', 1.5]]) {
    const opt = new D.ProcessOptions(); opt[key] = value;
    const work = new Uint8ClampedArray(source); D.applyImageTone(work, opt);
    for (let i = 0; i < work.length; i++) {
      if (i % 4 === 3) assert.equal(work[i], source[i]);
      else assert.ok(work[i] > source[i], key + ' should brighten midtones');
    }
  }
  const opt = new D.ProcessOptions(); opt.saturation = 0;
  const work = new Uint8ClampedArray(source); D.applyImageTone(work, opt);
  for (let i = 0; i < work.length; i += 4) {
    assert.equal(work[i], work[i + 1]); assert.equal(work[i], work[i + 2]);
    assert.equal(work[i + 3], source[i + 3]);
  }
  console.log('Tone checks passed: neutral parity, brightness, gamma, saturation, alpha');
})();
