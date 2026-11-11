export class RawImage {
  data: Uint8ClampedArray = new Uint8ClampedArray(0);
  width: number = 0;
  height: number = 0;

  constructor(data: Uint8ClampedArray, width: number, height: number) {
    this.data = data;
    this.width = width;
    this.height = height;
  }
}

export function rotate90(src: RawImage): RawImage {
  const w: number = src.width;
  const h: number = src.height;
  const out = new Uint8ClampedArray(w * h * 4);
  for (let y: number = 0; y < h; y++) {
    for (let x: number = 0; x < w; x++) {
      const si: number = (y * w + x) * 4;
      const nx: number = h - 1 - y;
      const ny: number = x;
      const di: number = (ny * h + nx) * 4;
      out[di] = src.data[si];
      out[di + 1] = src.data[si + 1];
      out[di + 2] = src.data[si + 2];
      out[di + 3] = src.data[si + 3];
    }
  }
  return new RawImage(out, h, w);
}

export function mirrorH(src: RawImage): RawImage {
  const w: number = src.width;
  const h: number = src.height;
  const out = new Uint8ClampedArray(w * h * 4);
  for (let y: number = 0; y < h; y++) {
    for (let x: number = 0; x < w; x++) {
      const si: number = (y * w + x) * 4;
      const di: number = (y * w + (w - 1 - x)) * 4;
      out[di] = src.data[si];
      out[di + 1] = src.data[si + 1];
      out[di + 2] = src.data[si + 2];
      out[di + 3] = src.data[si + 3];
    }
  }
  return new RawImage(out, w, h);
}

export function mirrorV(src: RawImage): RawImage {
  const w: number = src.width;
  const h: number = src.height;
  const out = new Uint8ClampedArray(w * h * 4);
  const row: number = w * 4;
  for (let y: number = 0; y < h; y++) {
    const s: number = y * row;
    const d: number = (h - 1 - y) * row;
    out.set(src.data.subarray(s, s + row), d);
  }
  return new RawImage(out, w, h);
}

function sample(src: RawImage, fx: number, fy: number, out: Uint8ClampedArray, di: number): void {
  const x0: number = Math.max(0, Math.min(src.width - 1, Math.floor(fx)));
  const y0: number = Math.max(0, Math.min(src.height - 1, Math.floor(fy)));
  const x1: number = Math.min(src.width - 1, x0 + 1);
  const y1: number = Math.min(src.height - 1, y0 + 1);
  const tx: number = Math.max(0, Math.min(1, fx - x0));
  const ty: number = Math.max(0, Math.min(1, fy - y0));
  const i00: number = (y0 * src.width + x0) * 4;
  const i10: number = (y0 * src.width + x1) * 4;
  const i01: number = (y1 * src.width + x0) * 4;
  const i11: number = (y1 * src.width + x1) * 4;
  for (let c: number = 0; c < 3; c++) {
    const top: number = src.data[i00 + c] * (1 - tx) + src.data[i10 + c] * tx;
    const bottom: number = src.data[i01 + c] * (1 - tx) + src.data[i11 + c] * tx;
    out[di + c] = top * (1 - ty) + bottom * ty;
  }
  out[di + 3] = 255;
}

export function fitToCanvas(src: RawImage, cw: number, ch: number, mode: number): RawImage {
  const out = new Uint8ClampedArray(cw * ch * 4);
  out.fill(255);
  let scaleX: number = cw / src.width;
  let scaleY: number = ch / src.height;
  let offX: number = 0;
  let offY: number = 0;
  if (mode === 1) {
    const s: number = Math.max(scaleX, scaleY);
    scaleX = s;
    scaleY = s;
    offX = (cw - src.width * s) / 2;
    offY = (ch - src.height * s) / 2;
  } else if (mode === 2) {
    const s: number = Math.min(scaleX, scaleY);
    scaleX = s;
    scaleY = s;
    offX = (cw - src.width * s) / 2;
    offY = (ch - src.height * s) / 2;
  }
  for (let y: number = 0; y < ch; y++) {
    for (let x: number = 0; x < cw; x++) {
      const sx: number = (x - offX) / scaleX;
      const sy: number = (y - offY) / scaleY;
      const di: number = (y * cw + x) * 4;
      if (sx < 0 || sy < 0 || sx > src.width - 1 || sy > src.height - 1) {
        continue;
      }
      sample(src, sx, sy, out, di);
    }
  }
  return new RawImage(out, cw, ch);
}
