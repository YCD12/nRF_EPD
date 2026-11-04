'use strict';

/**
 * Unit tests for html/js/rle.js.
 *
 * The wire format under test is the one the firmware decoder will implement
 * (same semantics as upstream EPD-nRF5 `rle_decompress_from`):
 *   - repeat : control = 0x80 | (count - 3), count 3..130, then 1 value byte
 *   - literal: control = count - 1,          count 1..128, then count bytes
 * Every encoded block is a self-contained stream (no cross-block state), which
 * is what allows the transfer layer to split an image into independent
 * WRITE_BLOCK payloads and let the MCU decode each block on its own.
 *
 * Run with: node --test tests/test_rle.cjs
 */

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const RLE_PATH = path.join(__dirname, '..', 'html', 'js', 'rle.js');
const codec = require(RLE_PATH);

const { encode, decode, encodeBlock } = codec;

// Deterministic pseudo-random generator so failures are reproducible.
function makeRandom(seed) {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 0x100000000;
  };
}

function randomBytes(length, seed) {
  const next = makeRandom(seed);
  const out = new Uint8Array(length);
  for (let i = 0; i < length; i++) out[i] = Math.floor(next() * 256);
  return out;
}

function repeated(value, length) {
  return new Uint8Array(length).fill(value);
}

function assertRoundTrip(data) {
  const encoded = encode(data);
  assert.ok(encoded instanceof Uint8Array, 'encode must return Uint8Array');
  const decoded = decode(encoded);
  assert.ok(decoded instanceof Uint8Array, 'decode must return Uint8Array');
  assert.deepStrictEqual(decoded, data);
}

test('module exports the codec under Node (require works, no external deps)', () => {
  assert.strictEqual(typeof codec.encode, 'function');
  assert.strictEqual(typeof codec.decode, 'function');
  assert.strictEqual(typeof codec.encodeBlock, 'function');
  // Upstream-compatible aliases requested by the task description.
  assert.strictEqual(codec.rleCompress, codec.encode);
  assert.strictEqual(codec.rleDecode, codec.decode);
  assert.strictEqual(codec.rleEncodeBlock, codec.encodeBlock);
  assert.strictEqual(codec.MAX_RUN, 130);
  assert.strictEqual(codec.MAX_LITERAL, 128);
});

test('round-trips empty input and the 1/2/3/236/1024/48000 byte sizes', () => {
  for (const length of [0, 1, 2, 3, 236, 1024, 48000]) {
    assertRoundTrip(randomBytes(length, 0x51ed + length));
  }
});

test('round-trips long uniform, alternating and two-byte-repeat arrays', () => {
  assertRoundTrip(repeated(0xff, 236));
  assertRoundTrip(repeated(0x00, 48000));
  const alternating = new Uint8Array(1024);
  for (let i = 0; i < alternating.length; i++) alternating[i] = i % 2 === 0 ? 0x00 : 0xff;
  assertRoundTrip(alternating);
  assertRoundTrip(Uint8Array.of(0x55, 0x55, 0xaa, 0x55, 0x55)); // 2-runs stay literal
  assertRoundTrip(Uint8Array.of(1, 2, 2, 3, 3, 3, 4, 4));
});

test('round-trips run lengths 3, 130 and 131 and matches the firmware codes', () => {
  // 3 identical: repeat control = 0x80 | (3-3) = 0x80.
  assert.deepStrictEqual(encode(repeated(0xaa, 3)), Uint8Array.of(0x80, 0xaa));
  // 130 identical: 0x80 | (130-3) = 0xFF.
  assert.deepStrictEqual(encode(repeated(0xaa, 130)), Uint8Array.of(0xff, 0xaa));
  // 131 identical cannot fit one run: run 130 + literal 1.
  assert.deepStrictEqual(encode(repeated(0xaa, 131)), Uint8Array.of(0xff, 0xaa, 0x00, 0xaa));
  for (const run of [3, 4, 129, 130, 131, 132, 260, 261]) assertRoundTrip(repeated(0x5a, run));
});

test('round-trips literal runs around the 128-byte boundary', () => {
  // 1-byte literal: control = 0.
  assert.deepStrictEqual(encode(Uint8Array.of(0x42)), Uint8Array.of(0x00, 0x42));
  // 2-byte literal: control = 1.
  assert.deepStrictEqual(encode(Uint8Array.of(0x42, 0x43)), Uint8Array.of(0x01, 0x42, 0x43));

  // 128 unique bytes: exactly one literal with control 127.
  const literal128 = new Uint8Array(128);
  for (let i = 0; i < 128; i++) literal128[i] = i;
  const encoded128 = encode(literal128);
  assert.strictEqual(encoded128.length, 129);
  assert.strictEqual(encoded128[0], 127);
  assert.deepStrictEqual(encoded128.subarray(1), literal128);

  // 129 unique bytes: literal 128 + literal 1.
  const literal129 = new Uint8Array(129);
  for (let i = 0; i < 129; i++) literal129[i] = i;
  const encoded129 = encode(literal129);
  assert.deepStrictEqual(encoded129, Uint8Array.from([127, ...literal129.subarray(0, 128), 0, 128]));
  assertRoundTrip(literal128);
  assertRoundTrip(literal129);
});

test('literal run is cut short when a >=3 repeat run begins', () => {
  // [1,2,3,4] is a 4-byte literal, then 9,9,9 is a repeat run.
  assert.deepStrictEqual(encode(Uint8Array.of(1, 2, 3, 4, 9, 9, 9)),
    Uint8Array.of(0x03, 1, 2, 3, 4, 0x80, 9));
  // A repeat run followed by another literal.
  assert.deepStrictEqual(encode(Uint8Array.of(7, 7, 7, 8, 9)),
    Uint8Array.of(0x80, 7, 0x01, 8, 9));
});

test('maxLiteral caps literal runs and still round-trips', () => {
  const data = new Uint8Array(300); // strictly incremental: no repeat runs
  for (let i = 0; i < data.length; i++) data[i] = (i * 7 + 3) & 0xff;
  const encoded = encode(data, 32);
  // ceil(300 / 32) = 10 literal control bytes, no extra controls.
  assert.strictEqual(encoded.length, 310);
  assert.deepStrictEqual(decode(encoded), data);
  // Values above the wire limit are clamped to 128 rather than producing an
  // undecodable control byte.
  assertRoundTripWithLimit(data, 200);
});

function assertRoundTripWithLimit(data, maxLiteral) {
  assert.deepStrictEqual(decode(encode(data, maxLiteral)), data);
}

test('encodeBlock is equivalent to encode and produces self-contained blocks', () => {
  const first = randomBytes(1000, 0xabc);
  const second = randomBytes(1000, 0xdef);
  assert.deepStrictEqual(encodeBlock(first), encode(first));
  assert.deepStrictEqual(encodeBlock(second), encode(second));
  // Independent blocks: decoding each block yields exactly its own bytes.
  assert.deepStrictEqual(decode(encodeBlock(first)), first);
  assert.deepStrictEqual(decode(encodeBlock(second)), second);
  // Consecutive blocks must not rely on decoder state from a previous block.
  const whole = new Uint8Array(first.length + second.length);
  whole.set(first, 0);
  whole.set(second, first.length);
  const concatenated = new Uint8Array(encodeBlock(first).length + encodeBlock(second).length);
  concatenated.set(encodeBlock(first), 0);
  concatenated.set(encodeBlock(second), encodeBlock(first).length);
  assert.deepStrictEqual(decode(concatenated), whole);
});

test('compression ratio on the required cases', () => {
  // A single repeat code carries at most 130 bytes, so a 2-byte stream only
  // covers inputs <= 130 B. (The task text mentioned "236B -> 2B"; that is
  // impossible under the 130-byte run cap, so the assertions below use the
  // exact formula: 2 * ceil(length / 130).)
  assert.strictEqual(encode(repeated(0xff, 130)).length, 2);
  assert.strictEqual(encode(repeated(0xff, 236)).length, 4);   // 130 + 106 runs
  // 48000 = 369 * 130 + 30 => 370 runs * 2 bytes.
  assert.strictEqual(encode(repeated(0x00, 48000)).length, 2 * 370);

  for (const length of [1024, 48000]) {
    const data = randomBytes(length, 0xfeed + length);
    const encoded = encode(data);
    assert.ok(encoded.length <= data.length * 1.5,
      `random ${length}B encoded to ${encoded.length}B (> 1.5x)`);
    assert.deepStrictEqual(decode(encoded), data);
  }
});

test('decode rejects truncated repeat runs (missing value byte)', () => {
  assert.throws(() => decode(Uint8Array.of(0x80)), RangeError);
  assert.throws(() => decode(Uint8Array.of(0xff)), RangeError);
  assert.throws(() => decode(Uint8Array.of(0x80, 0x01, 0x80)), RangeError);
  // A valid repeat followed by a truncated one still throws.
  assert.throws(() => decode(Uint8Array.of(0x80, 0xaa, 0x81)), RangeError);
});

test('decode rejects literal runs that exceed the input length', () => {
  assert.throws(() => decode(Uint8Array.of(0x00)), RangeError);              // needs 1 byte
  assert.throws(() => decode(Uint8Array.of(0x02, 1, 2)), RangeError);        // needs 3 bytes
  assert.throws(() => decode(Uint8Array.of(0x7f, 1, 2, 3, 4)), RangeError);  // needs 128 bytes
  // Valid literal followed by a truncated literal also throws.
  assert.throws(() => decode(Uint8Array.of(0x01, 1, 2, 0x02, 3, 4)), RangeError);
  // Boundary: a literal that exactly consumes the input is valid.
  assert.deepStrictEqual(decode(Uint8Array.of(0x02, 1, 2, 3)), Uint8Array.of(1, 2, 3));
});

test('decode of empty input is empty', () => {
  assert.deepStrictEqual(decode(new Uint8Array(0)), new Uint8Array(0));
});

test('decoding encoded output type is Uint8Array and content is byte-exact', () => {
  const data = randomBytes(777, 0x777);
  const encoded = encode(data);
  assert.ok(encoded instanceof Uint8Array);
  assert.ok(!Array.isArray(encoded));
  const decoded = decode(encoded);
  assert.ok(decoded instanceof Uint8Array);
  // Uint8Array.from / subarray results are views or copies, but both expose
  // exactly the original bytes.
  assert.strictEqual(decoded.length, data.length);
  assert.deepStrictEqual(Array.from(decoded), Array.from(data));
});

test('browser path exposes window.RleCodec and pollutes no other global', () => {
  const source = fs.readFileSync(RLE_PATH, 'utf8');
  const sandbox = { window: {} };
  vm.runInNewContext(source, sandbox);
  assert.strictEqual(typeof sandbox.window.RleCodec.encode, 'function');
  assert.strictEqual(typeof sandbox.window.RleCodec.decode, 'function');
  assert.strictEqual(typeof sandbox.window.RleCodec.encodeBlock, 'function');
  // The IIFE assigns only window.RleCodec; no extra keys appear on the sandbox.
  assert.deepStrictEqual(Object.keys(sandbox), ['window']);
  assert.deepStrictEqual(Object.keys(sandbox.window), ['RleCodec']);
});
