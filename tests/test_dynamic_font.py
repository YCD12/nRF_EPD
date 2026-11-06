"""Verify browser-generated glyph streams against the firmware decoder."""
import base64
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import json

ROOT = Path(__file__).resolve().parents[1]
WRITER = r"""
const fs = require('node:fs');
const assert = require('node:assert/strict');
const glyphs = require('./html/js/font_builder.js');
const builder = require('./html/js/resource_builder.js');
const fonts = ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn'].map(
  name => new Uint8Array(fs.readFileSync(`tools/dist/u8g2_font_${name}.bin`)));
(async () => {
  const rasterize = async (codePoint, slot) => {
    const privateUse = codePoint === 0xe000;
    const width = privateUse ? (slot === 0 ? 12 : 16) : (slot === 0 ? 8 : 10);
    const height = privateUse ? width : (slot === 0 ? 7 : 11);
    const pixels = new Uint8Array(width * height);
    for (let y = 0; y < height; ++y)
      for (let x = 0; x < width; ++x)
        pixels[y * width + x] = privateUse ? Number(y >= height / 2) : (x + y) & 1;
    return { width, height, x: privateUse ? 0 : 1, y: privateUse ? -2 : 0,
      advance: privateUse ? width : (slot === 0 ? 10 : 12), pixels };
  };
  const result = await glyphs.extendFonts(fonts,
    [{ slot: 0, text: '\u00a1\ue000' }, { slot: 1, text: 'A\ue000' }], { rasterize });
  assert.equal(result.added.length, 4);
  assert.equal(glyphs.gatherMissing(result.fonts,
    [{ slot: 0, text: '\u00a1\ue000' }, { slot: 1, text: 'A\ue000' }]).length, 0);
  const packs = [{ bytes: builder.encodeFonts(result.fonts), size: 0, key: 0, extended: false }];
  for (const sourceSlot of [0, 1]) {
    const requests = [{ slot: 3, sourceSlot, fontFamily: 'SimSun', text: 'A\ue000' }];
    const custom = await glyphs.extendFonts(result.fonts, requests, { rasterize });
    assert.equal(custom.added.length, 2);
    assert.equal(glyphs.gatherMissing(custom.fonts, requests).length, 0);
    for (let slot = 0; slot < 3; ++slot) assert.deepEqual(custom.fonts[slot], result.fonts[slot]);
    packs.push({ bytes: builder.encodeFonts(custom.fonts), size: sourceSlot === 0 ? 9 : 12,
      key: builder.fontKey('SimSun', sourceSlot), extended: false });
  }
  for (const fontSize of [9, 12, 16, 20, 24, 32]) {
    for (let style = 0; style < 4; ++style) {
      const fontBold = Boolean(style & 1), fontItalic = Boolean(style & 2);
      const requests = [{ slot: 3, sourceSlot: fontSize === 9 ? 0 : 1, fontSize,
        fontBold, fontItalic, fontFamily: style & 1 ? 'SimSun' : '', text: 'A\ue000' }];
      const rasterizeExtended = async (codePoint, slot, header, options) => {
        assert.equal(options.fontSize, fontSize);
        assert.equal(options.fontBold, fontBold); assert.equal(options.fontItalic, fontItalic);
        const [width, height, x, y] = builder.todoFontGeometry(fontSize);
        const pixels = new Uint8Array(width * height), privateUse = codePoint === 0xe000;
        for (let gy = 0; gy < height; ++gy)
          for (let gx = 0; gx < width; ++gx)
            pixels[gy * width + gx] = privateUse ? Number(gy >= height / 2) :
              Number(gx < 3 || gx >= width - 3 || gy < 3 || gy >= height - 3);
        return { width, height, x, y, advance: Math.ceil(fontSize * 4 / 3) + 1, pixels };
      };
      const custom = await glyphs.extendFonts(result.fonts, requests, { rasterize: rasterizeExtended });
      assert.equal(custom.added.length, 2);
      assert.equal(glyphs.gatherMissing(custom.fonts, requests).length, 0);
      packs.push({ bytes: builder.encodeFonts(custom.fonts), size: fontSize,
        key: builder.todoFontKey(requests[0].fontFamily, fontSize, fontBold, fontItalic), extended: true });
    }
  }
  process.stdout.write(JSON.stringify(packs.map(pack => ({ ...pack,
    bytes: Buffer.from(pack.bytes).toString('base64') }))));
})().catch(error => { console.error(error); process.exitCode = 1; });
"""


def main():
    node = os.environ.get('NODE_BINARY') or shutil.which('node')
    if not node:
        print('SKIP: browser/firmware font ABI test requires Node.js (or NODE_BINARY).')
        return
    out = Path(os.environ.get('HOST_TEST_OUT', ROOT / '_build/host_tests'))
    out.mkdir(parents=True, exist_ok=True)
    generated = subprocess.run([node, '-e', WRITER], cwd=ROOT, check=True,
                               stdout=subprocess.PIPE, text=True)
    packs = json.loads(generated.stdout)
    assert len(packs) == 27, generated.stdout
    executable = out / 'test_dynamic_font'
    sources = ['EPD/storage/font_store.c', 'EPD/storage/resource_crc.c', 'EPD/storage/resource_crc_flash.c',
               'GUI/u8g2_font.c', 'GUI/font_geometry.c', 'tests/test_dynamic_font.c']
    subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) + [
        '-Wall', '-Werror', '-std=c11', '-I.', '-I.', '-IEPD/hal', '-IEPD/driver', '-IEPD/board', '-IEPD/storage', '-IEPD/app', '-IEPD/service', '-IGUI',
        '-ISDK/17.1.0_ddde560/components/libraries/util',
        '-ISDK/17.1.0_ddde560/components/softdevice/s112/headers',
        '-DFONT_STORE_HOST_TEST', *sources, '-o', str(executable)], cwd=ROOT, check=True)
    for index, pack in enumerate(packs):
        payload = out / f'dynamic-font-pack-{index}.bin'
        payload.write_bytes(base64.b64decode(pack['bytes'], validate=True))
        args = [] if index == 0 else [str(pack['size']), str(pack['key'])]
        if pack['extended']:
            args.append('extended')
        subprocess.run([str(executable.resolve()), str(payload.resolve()), *args], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
