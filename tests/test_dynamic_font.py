"""Verify browser-generated glyph streams against the firmware decoder."""
import base64
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WRITER = r"""
const fs = require('node:fs');
const assert = require('node:assert/strict');
const glyphs = require('./html/js/font_builder.js');
const builder = require('./html/js/resource_builder.js');
const fonts = ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn'].map(
  name => new Uint8Array(fs.readFileSync(`tools/dist/u8g2_font_${name}.bin`)));
(async () => {
  const result = await glyphs.extendFonts(fonts,
    [{ slot: 0, text: '\u00a1\ue000' }, { slot: 1, text: 'A\ue000' }], {
      rasterize: async (codePoint, slot) => {
        const privateUse = codePoint === 0xe000;
        const width = privateUse ? (slot === 0 ? 12 : 16) : (slot === 0 ? 8 : 10);
        const height = privateUse ? width : (slot === 0 ? 7 : 11);
        const pixels = new Uint8Array(width * height);
        for (let y = 0; y < height; ++y)
          for (let x = 0; x < width; ++x)
            pixels[y * width + x] = privateUse ? Number(y >= height / 2) : (x + y) & 1;
        return { width, height, x: privateUse ? 0 : 1, y: privateUse ? -2 : 0,
          advance: privateUse ? width : (slot === 0 ? 10 : 12), pixels };
      }
    });
  assert.equal(result.added.length, 4);
  assert.equal(glyphs.gatherMissing(result.fonts,
    [{ slot: 0, text: '\u00a1\ue000' }, { slot: 1, text: 'A\ue000' }]).length, 0);
  process.stdout.write(Buffer.from(builder.encodeFonts(result.fonts)).toString('base64'));
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
    payload = out / 'dynamic-font-pack.bin'
    payload.write_bytes(base64.b64decode(generated.stdout, validate=True))
    executable = out / 'test_dynamic_font'
    sources = ['EPD/font_store.c', 'EPD/resource_crc.c', 'EPD/resource_crc_flash.c',
               'GUI/u8g2_font.c', 'tests/test_dynamic_font.c']
    subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) + [
        '-Wall', '-Werror', '-std=c11', '-I.', '-IEPD', '-IGUI',
        '-ISDK/17.1.0_ddde560/components/libraries/util',
        '-ISDK/17.1.0_ddde560/components/softdevice/s112/headers',
        '-DFONT_STORE_HOST_TEST', *sources, '-o', str(executable)], cwd=ROOT, check=True)
    subprocess.run([str(executable.resolve()), str(payload.resolve())], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
