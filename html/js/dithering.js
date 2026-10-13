// Ported from: https://e-paper-display.cn/usb2epd.html

// 固定的六色调色板
const rgbPalette = [
  { name: "黄色", r: 255, g: 255, b: 0, value: 0xe2 },
  { name: "绿色", r: 41, g: 204, b: 20, value: 0x96 },
  { name: "蓝色", r: 0, g: 0, b: 255, value: 0x1d },
  { name: "红色", r: 255, g: 0, b: 0, value: 0x4c },
  { name: "黑色", r: 0, g: 0, b: 0, value: 0x00 },
  { name: "白色", r: 255, g: 255, b: 255, value: 0xff }
];

// 四色调色板
const fourColorPalette = [
  { name: "黑色", r: 0, g: 0, b: 0, value: 0x00 },
  { name: "白色", r: 255, g: 255, b: 255, value: 0x01 },
  { name: "红色", r: 255, g: 0, b: 0, value: 0x03 },
  { name: "黄色", r: 255, g: 255, b: 0, value: 0x02 }
];

// 三色调色板
const threeColorPalette = [
  { name: "黑色", r: 0, g: 0, b: 0, value: 0x00 },
  { name: "白色", r: 255, g: 255, b: 255, value: 0x01 },
  { name: "红色", r: 255, g: 0, b: 0, value: 0x02 }
];

function adjustContrast(imageData, factor) {
  const data = imageData.data;
  for (let i = 0; i < data.length; i += 4) {
    data[i] = Math.min(255, Math.max(0, (data[i] - 128) * factor + 128));
    data[i + 1] = Math.min(255, Math.max(0, (data[i + 1] - 128) * factor + 128));
    data[i + 2] = Math.min(255, Math.max(0, (data[i + 2] - 128) * factor + 128));
  }
  return imageData;
}

function rgbToLab(r, g, b) {
  r = r / 255;
  g = g / 255;
  b = b / 255;

  r = r > 0.04045 ? Math.pow((r + 0.055) / 1.055, 2.4) : r / 12.92;
  g = g > 0.04045 ? Math.pow((g + 0.055) / 1.055, 2.4) : g / 12.92;
  b = b > 0.04045 ? Math.pow((b + 0.055) / 1.055, 2.4) : b / 12.92;

  r *= 100;
  g *= 100;
  b *= 100;

  let x = r * 0.4124 + g * 0.3576 + b * 0.1805;
  let y = r * 0.2126 + g * 0.7152 + b * 0.0722;
  let z = r * 0.0193 + g * 0.1192 + b * 0.9505;

  x /= 95.047;
  y /= 100.0;
  z /= 108.883;

  x = x > 0.008856 ? Math.pow(x, 1 / 3) : (7.787 * x) + (16 / 116);
  y = y > 0.008856 ? Math.pow(y, 1 / 3) : (7.787 * y) + (16 / 116);
  z = z > 0.008856 ? Math.pow(z, 1 / 3) : (7.787 * z) + (16 / 116);

  const l = (116 * y) - 16;
  const a = 500 * (x - y);
  const bLab = 200 * (y - z);

  return { l, a, b: bLab };
}

function labDistance(lab1, lab2) {
  const dl = lab1.l - lab2.l;
  const da = lab1.a - lab2.a;
  const db = lab1.b - lab2.b;
  return Math.sqrt(0.2 * dl * dl + 3 * da * da + 3 * db * db);
}

const paletteLabs = new Map([rgbPalette, fourColorPalette].map(palette =>
  [palette, palette.map(color => rgbToLab(color.r, color.g, color.b))]));

function findClosestColor(r, g, b, mode) {
  if (mode === 'blackWhiteColor')
    return 0.299 * r + 0.587 * g + 0.114 * b < 140 ? threeColorPalette[0] : threeColorPalette[1];
  let palette;

  if (mode === 'fourColor') {
    palette = fourColorPalette;
  } else if (mode === 'threeColor') {
    palette = threeColorPalette;
  } else {
    palette = rgbPalette;
  }

  // 蓝色特殊情况（仅限非三色、四色模式）
  if (mode !== 'fourColor' && mode !== 'threeColor' && r < 50 && g < 150 && b > 100) {
    return rgbPalette[2]; // 蓝色
  }

  // 三色模式下优先检测红色
  if (mode === 'threeColor') {
    // 与发送阶段使用同一红色判定，避免预览与实际像素分类不一致。
    if (r > 160 && r > g && r > b) {
      return threeColorPalette[2]; // 红色
    }
    // 否则根据亮度选择黑或白
    const luminance = 0.299 * r + 0.587 * g + 0.114 * b;
    return luminance < 140 ? threeColorPalette[0] : threeColorPalette[1]; // 黑色或白色
  }

  const inputLab = rgbToLab(r, g, b);
  let minDistance = Infinity;
  let closestColor = palette[0];

  for (let i = 0; i < palette.length; i++) {
    const color = palette[i], colorLab = paletteLabs.get(palette)[i];
    const distance = labDistance(inputLab, colorLab);
    if (distance < minDistance) {
      minDistance = distance;
      closestColor = color;
    }
  }

  return closestColor;
}

// Keep fractional errors until each pixel is quantized. A clamped byte buffer
// rounds every small diffusion contribution away, especially at low strength.
function errorDiffusionDither(imageData, strength, mode, kernel, divisor) {
  const { width, height, data } = imageData;
  const errors = new Float32Array(data);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const at = (y * width + x) * 4;
      const r = Math.min(255, Math.max(0, errors[at]));
      const g = Math.min(255, Math.max(0, errors[at + 1]));
      const b = Math.min(255, Math.max(0, errors[at + 2]));
      const closest = findClosestColor(r, g, b, mode);
      data[at] = closest.r; data[at + 1] = closest.g; data[at + 2] = closest.b;
      const er = (r - closest.r) * strength, eg = (g - closest.g) * strength, eb = (b - closest.b) * strength;
      for (const [dx, dy, weight] of kernel) {
        const nx = x + dx, ny = y + dy;
        if (nx < 0 || nx >= width || ny >= height) continue;
        const next = (ny * width + nx) * 4, fraction = weight / divisor;
        errors[next] += er * fraction; errors[next + 1] += eg * fraction; errors[next + 2] += eb * fraction;
      }
    }
  }
  return imageData;
}

function floydSteinbergDither(imageData, strength, mode) {
  return errorDiffusionDither(imageData, strength, mode,
    [[1,0,7], [-1,1,3], [0,1,5], [1,1,1]], 16);
}

function atkinsonDither(imageData, strength, mode) {
  return errorDiffusionDither(imageData, strength, mode,
    [[1,0,1], [2,0,1], [-1,1,1], [0,1,1], [1,1,1], [0,2,1]], 8);
}

function stuckiDither(imageData, strength, mode) {
  return errorDiffusionDither(imageData, strength, mode,
    [[1,0,8], [2,0,4], [-2,1,2], [-1,1,4], [0,1,8], [1,1,4], [2,1,2],
      [-2,2,1], [-1,2,2], [0,2,4], [1,2,2], [2,2,1]], 42);
}

function jarvisDither(imageData, strength, mode) {
  return errorDiffusionDither(imageData, strength, mode,
    [[1,0,7], [2,0,5], [-2,1,3], [-1,1,5], [0,1,7], [1,1,5], [2,1,3],
      [-2,2,1], [-1,2,3], [0,2,5], [1,2,3], [2,2,1]], 48);
}

function bayerDither(imageData, strength, mode) {
  const width = imageData.width;
  const height = imageData.height;
  const data = imageData.data;

  // 8x8 Bayer matrix (normalized to 0-1 range)
  const bayerMatrix = [
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21]
  ];

  const matrixSize = 8;
  const maxThreshold = 64;

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const idx = (y * width + x) * 4;
      const r = data[idx];
      const g = data[idx + 1];
      const b = data[idx + 2];

      // Get threshold from Bayer matrix
      const matrixX = x % matrixSize;
      const matrixY = y % matrixSize;
      const threshold = ((bayerMatrix[matrixY][matrixX] + 0.5) / maxThreshold) * 255;

      // Apply dithering with strength factor
      const adjustedR = r + (threshold - 127.5) * strength;
      const adjustedG = g + (threshold - 127.5) * strength;
      const adjustedB = b + (threshold - 127.5) * strength;

      // Clamp values
      const clampedR = Math.min(255, Math.max(0, adjustedR));
      const clampedG = Math.min(255, Math.max(0, adjustedG));
      const clampedB = Math.min(255, Math.max(0, adjustedB));

      // Find closest color in palette
      const closest = findClosestColor(clampedR, clampedG, clampedB, mode);

      data[idx] = closest.r;
      data[idx + 1] = closest.g;
      data[idx + 2] = closest.b;
    }
  }

  return imageData;
}

function ditherImage(imageData, alg, strength, mode) {
  strength = Number.isFinite(strength) ? Math.max(0, Math.min(5, strength)) : 1;
  switch (alg) {
    case 'floydSteinberg':
      return floydSteinbergDither(imageData, strength, mode);
    case 'atkinson':
      return atkinsonDither(imageData, strength, mode);
    case 'stucki':
      return stuckiDither(imageData, strength, mode);
    case 'jarvis':
      return jarvisDither(imageData, strength, mode);
    case 'bayer':
      return bayerDither(imageData, strength, mode);
    case 'none':
    default:
      for (let i = 0; i < imageData.data.length; i += 4) {
        const color = findClosestColor(imageData.data[i], imageData.data[i + 1], imageData.data[i + 2], mode);
        imageData.data[i] = color.r; imageData.data[i + 1] = color.g; imageData.data[i + 2] = color.b;
      }
      return imageData;
  }
}

function decodeProcessedData(processedData, width, height, mode) {
  const imageData = new ImageData(width, height);
  const data = imageData.data;

  if (mode === 'sixColor') {
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const newIndex = (x * height) + (height - 1 - y);
        const value = processedData[newIndex];
        const color = rgbPalette.find(c => c.value === value) || rgbPalette[5]; // 默认白色
        const index = (y * width + x) * 4;
        data[index] = color.r;
        data[index + 1] = color.g;
        data[index + 2] = color.b;
        data[index + 3] = 255; // Alpha 透明度
      }
    }
  } else if (mode === 'fourColor') {
    const fourColorValues = [
      { value: 0x00, r: 0, g: 0, b: 0 },      // 黑色
      { value: 0x01, r: 255, g: 255, b: 255 }, // 白色
      { value: 0x03, r: 255, g: 0, b: 0 },     // 红色
      { value: 0x02, r: 255, g: 255, b: 0 }    // 黄色
    ];
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const newIndex = (y * width + x) / 4 | 0;
        const shift = 6 - (((y * width + x) % 4) * 2);
        const value = (processedData[newIndex] >> shift) & 0x03;
        const color = fourColorValues.find(c => c.value === value) || fourColorValues[1]; // 默认白色
        const index = (y * width + x) * 4;
        data[index] = color.r;
        data[index + 1] = color.g;
        data[index + 2] = color.b;
        data[index + 3] = 255;
      }
    }
  } else if (mode === 'blackWhiteColor') {
    const byteWidth = Math.ceil(width / 8);
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const byteIndex = y * byteWidth + Math.floor(x / 8);
        const bitIndex = 7 - (x % 8);
        const bit = (processedData[byteIndex] >> bitIndex) & 1;
        const index = (y * width + x) * 4;
        data[index] = bit ? 255 : 0; // 白或黑
        data[index + 1] = bit ? 255 : 0;
        data[index + 2] = bit ? 255 : 0;
        data[index + 3] = 255;
      }
    }
  } else if (mode === 'threeColor') {
    const byteWidth = Math.ceil(width / 8);
    const blackWhiteData = processedData.slice(0, byteWidth * height);
    const redWhiteData = processedData.slice(byteWidth * height);
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const byteIndex = y * byteWidth + Math.floor(x / 8);
        const bitIndex = 7 - (x % 8);
        const blackWhiteBit = (blackWhiteData[byteIndex] >> bitIndex) & 1;
        const redWhiteBit = (redWhiteData[byteIndex] >> bitIndex) & 1;
        const index = (y * width + x) * 4;
        if (!redWhiteBit) {
          // 红色
          data[index] = 255;
          data[index + 1] = 0;
          data[index + 2] = 0;
        } else {
          // 黑或白
          data[index] = blackWhiteBit ? 255 : 0;
          data[index + 1] = blackWhiteBit ? 255 : 0;
          data[index + 2] = blackWhiteBit ? 255 : 0;
        }
        data[index + 3] = 255;
      }
    }
  }

  return imageData;
}

function processImageData(imageData, mode) {
  const width = imageData.width;
  const height = imageData.height;
  const data = imageData.data;

  let processedData;

  if (mode === 'sixColor') {
    processedData = new Uint8Array(width * height);
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const index = (y * width + x) * 4;
        const r = data[index];
        const g = data[index + 1];
        const b = data[index + 2];

        const closest = findClosestColor(r, g, b, mode);
        const newIndex = (x * height) + (height - 1 - y);
        processedData[newIndex] = closest.value;
      }
    }
  } else if (mode === 'fourColor') {
    processedData = new Uint8Array(Math.ceil((width * height) / 4)).fill(0x55);
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const index = (y * width + x) * 4;
        const r = data[index];
        const g = data[index + 1];
        const b = data[index + 2];
        const closest = findClosestColor(r, g, b, mode); // 使用 fourColorPalette
        const colorValue = closest.value; // 0x00 (黑), 0x01 (白), 0x02 (黄), 0x03 (红)
        const newIndex = (y * width + x) / 4 | 0;
        const shift = 6 - (((y * width + x) % 4) * 2);
        processedData[newIndex] = (processedData[newIndex] & ~(3 << shift)) | (colorValue << shift);
      }
    }
  } else if (mode === 'blackWhiteColor') {
    const byteWidth = Math.ceil(width / 8);
    processedData = new Uint8Array(byteWidth * height).fill(0xff);
    const threshold = 140;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const index = (y * width + x) * 4;
        const r = data[index];
        const g = data[index + 1];
        const b = data[index + 2];
        const grayscale = 0.299 * r + 0.587 * g + 0.114 * b;
        const bit = grayscale >= threshold ? 1 : 0;
        const byteIndex = y * byteWidth + Math.floor(x / 8);
        const bitIndex = 7 - (x % 8);
        processedData[byteIndex] = (processedData[byteIndex] & ~(1 << bitIndex)) | (bit << bitIndex);
      }
    }
  } else if (mode === 'threeColor') {
    const byteWidth = Math.ceil(width / 8);
    const blackWhiteThreshold = 140;
    const redThreshold = 160;

    const blackWhiteData = new Uint8Array(height * byteWidth).fill(0xff);
    const redWhiteData = new Uint8Array(height * byteWidth).fill(0xff);

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const index = (y * width + x) * 4;
        const r = data[index];
        const g = data[index + 1];
        const b = data[index + 2];
        const grayscale = 0.299 * r + 0.587 * g + 0.114 * b;
        // UC8179 uses active-low black/red planes. Classify red first and
        // force its black bit to white so the two planes are always mutually
        // exclusive. This matches the legacy renderer's existing red-first
        // precedence and prevents dark-red pixels from being black+red.
        const isRed = r > redThreshold && r > g && r > b;
        const blackWhiteBit = (isRed || grayscale >= blackWhiteThreshold) ? 1 : 0;
        const blackWhiteByteIndex = y * byteWidth + Math.floor(x / 8);
        const blackWhiteBitIndex = 7 - (x % 8);
        if (blackWhiteBit) {
          blackWhiteData[blackWhiteByteIndex] |= (0x01 << blackWhiteBitIndex);
        } else {
          blackWhiteData[blackWhiteByteIndex] &= ~(0x01 << blackWhiteBitIndex);
        }

        const redWhiteBit = isRed ? 0 : 1;
        const redWhiteByteIndex = y * byteWidth + Math.floor(x / 8);
        const redWhiteBitIndex = 7 - (x % 8);
        if (redWhiteBit) {
          redWhiteData[redWhiteByteIndex] |= (0x01 << redWhiteBitIndex);
        } else {
          redWhiteData[redWhiteByteIndex] &= ~(0x01 << redWhiteBitIndex);
        }
      }
    }

    processedData = new Uint8Array(blackWhiteData.length + redWhiteData.length);
    processedData.set(blackWhiteData, 0);
    processedData.set(redWhiteData, blackWhiteData.length);
  }

  return processedData;
}
