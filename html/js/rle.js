/**
 * RLE codec (byte-oriented) for EPD image transfer.
 *
 * Wire format (MUST match the firmware decoder; see the upstream
 * `rle_decompress_from` in EPD-nRF5 and the future fork firmware port):
 *
 *   repeat run : control = 0x80 | (count - 3), count in [3, 130]
 *                followed by exactly 1 byte, the repeated value.
 *   literal run: control = count - 1,          count in [1, 128]
 *                followed by exactly `count` literal bytes.
 *
 * Because a repeat control always has bit7 set and a literal control is
 * `count-1 <= 127`, the two codes are unambiguous. The decoder rejects
 * incomplete / illegal input with a RangeError (repeat run without its
 * value byte, literal run whose bytes would exceed the input).
 *
 * Every `encode` / `encodeBlock` result is a self-contained stream: the
 * decoder needs no state carried across blocks, so a caller may split an
 * image into independent chunks and concatenate the decoded outputs in
 * the same order. Compression is not guaranteed: callers should compare
 * `encodeBlock(chunk).length` with `chunk.length` and fall back to raw
 * bytes when the encoded form is not smaller.
 *
 * The module is self contained, exposes `window.RleCodec` in browsers and
 * `module.exports` under Node, and creates no other global state.
 */
(function () {
  'use strict';

  var MAX_RUN = 130;      // repeat control stores count-3 in 7 bits
  var MAX_LITERAL = 128;  // literal control stores count-1 in 7 bits

  /**
   * Normalize the input into a Uint8Array without copying when possible.
   * Accepts Uint8Array, other ArrayBuffer views and plain byte arrays.
   * @param {Uint8Array|ArrayLike<number>} data
   * @returns {Uint8Array}
   */
  function toBytes(data) {
    if (data instanceof Uint8Array) return data;
    if (data == null) throw new TypeError('RLE input must be a byte array');
    return Uint8Array.from(data);
  }

  /**
   * Compress a byte array into a self-contained RLE stream.
   *
   * Longest-possible repeat runs (>= 3) are emitted first, capped at 130
   * bytes per run (e.g. 131 identical bytes -> run 130 + literal 1). The
   * remaining bytes are emitted as literal runs capped at `maxLiteral`
   * (<= 128); a literal run is cut short as soon as the next three input
   * bytes are identical so the encoder can switch to a repeat run.
   *
   * @param {Uint8Array|ArrayLike<number>} data
   * @param {number} [maxLiteral=128] literal run cap, clamped to [1, 128]
   * @returns {Uint8Array} encoded stream (may be longer than the input)
   */
  function encode(data, maxLiteral) {
    var input = toBytes(data);
    var limit = maxLiteral === undefined ? MAX_LITERAL : Number(maxLiteral);
    if (!Number.isFinite(limit)) limit = MAX_LITERAL;
    limit = Math.max(1, Math.min(MAX_LITERAL, Math.floor(limit)));

    var length = input.length;
    // Upper bound: payload bytes + one control byte per literal run
    // (literal runs are capped at `limit` bytes; repeat runs are smaller).
    var out = new Uint8Array(length + Math.ceil(length / limit) + 1);
    var o = 0; // output write cursor
    var i = 0; // input read cursor

    while (i < length) {
      // Count the repeat run starting at i, capped to the wire maximum.
      var run = 1;
      while (i + run < length && run < MAX_RUN && input[i + run] === input[i]) run++;

      if (run >= 3) {
        out[o++] = 0x80 | (run - 3);
        out[o++] = input[i];
        i += run;
        continue;
      }

      // Literal run: stop at maxLiteral or where a repeat run (>= 3) starts.
      var start = i;
      var literal = 0;
      while (i < length && literal < limit) {
        if (i + 2 < length && input[i] === input[i + 1] && input[i] === input[i + 2]) break;
        i++;
        literal++;
      }
      // Defensive fallback: a repeat run of >= 3 bytes is always consumed by
      // the branch above, so `literal` cannot be 0 here; keep the stream
      // decodable even if that invariant ever changes.
      if (literal === 0) {
        out[o++] = 0x00;
        out[o++] = input[i++];
      } else {
        out[o++] = literal - 1;
        for (var j = start; j < start + literal; j++) out[o++] = input[j];
      }
    }

    return out.subarray(0, o);
  }

  /**
   * Decode a self-contained RLE stream. Throws RangeError on truncated or
   * otherwise illegal codes (see wire format above). Empty input -> empty.
   *
   * @param {Uint8Array|ArrayLike<number>} bytes
   * @returns {Uint8Array}
   */
  function decode(bytes) {
    var input = toBytes(bytes);
    var length = input.length;
    var out = [];
    var i = 0;

    while (i < length) {
      var control = input[i++];
      if (control & 0x80) {
        var repeat = (control & 0x7F) + 3;
        if (i >= length) throw new RangeError('RLE repeat run is missing its value byte');
        var value = input[i++];
        for (var r = 0; r < repeat; r++) out.push(value);
      } else {
        var count = control + 1;
        if (i + count > length) throw new RangeError('RLE literal run exceeds the input length');
        for (var l = 0; l < count; l++) out.push(input[i++]);
      }
    }

    return Uint8Array.from(out);
  }

  /**
   * Encode one independent block. Same output as `encode`; kept as a named
   * hook for the transfer layer so block-splitting call sites read clearly.
   * @param {Uint8Array|ArrayLike<number>} chunk
   * @returns {Uint8Array}
   */
  function encodeBlock(chunk) {
    return encode(chunk);
  }

  var RleCodec = {
    encode: encode,
    decode: decode,
    encodeBlock: encodeBlock,
    // Names used by the upstream web page / task description:
    rleCompress: encode,
    rleDecode: decode,
    rleEncodeBlock: encodeBlock,
    MAX_RUN: MAX_RUN,
    MAX_LITERAL: MAX_LITERAL
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = RleCodec;
  if (typeof window !== 'undefined') window.RleCodec = RleCodec;
})();
