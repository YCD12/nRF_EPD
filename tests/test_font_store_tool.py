import importlib.util
from pathlib import Path
import re
import struct
import sys
import unittest
import zlib

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("font_store_tool", ROOT / "tools/font_store_tool.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class FontStoreToolTest(unittest.TestCase):
    def test_geometry_matches_firmware(self):
        text = (ROOT / "GUI/font_geometry.c").read_text(encoding="utf-8")
        table = re.search(r"font_slot_geometry\[3\]\s*=\s*\{(.*?)\n\};", text, re.S).group(1)
        rows = re.findall(r"\{\s*(-?\d+(?:\s*,\s*-?\d+){8})\s*\}", table)
        self.assertEqual(len(rows), len(tool.SLOTS))
        for row, slot in zip(rows, tool.SLOTS.values()):
            width, height, x, y, ascent, descent, a_para, d_para, _ = map(int, row.split(","))
            self.assertEqual((x, width, height, y, ascent, descent, a_para, d_para), slot["metrics"])

    def test_shipped_fonts_and_manifest(self):
        _, records = tool.load_all(ROOT / "tools/dist", False)
        raw = tool.make_manifest(records, 42)
        self.assertEqual(tool.DATA_CAPACITY, 0x7E000)
        self.assertEqual(tool.MANIFEST_PRIMARY, 0x40)
        self.assertEqual(tool.MANIFEST_SECONDARY, 0x7F000)
        self.assertEqual(raw[:4], b"FST3")
        self.assertEqual(struct.unpack_from("<H", raw, 4)[0], 3)
        self.assertEqual(struct.unpack_from("<I", raw, 8)[0], 42)
        self.assertEqual(struct.unpack_from("<I", raw, 48)[0], zlib.crc32(raw[:48]))
        self.assertEqual(struct.unpack_from("<I", raw, 52)[0], 0xA55AA55A)

    def test_multiblock_rejected(self):
        data = bytearray((ROOT / "tools/dist/u8g2_font_wqy9_t_lunar.bin").read_bytes())
        header = tool.parse_header(data)
        table = 23 + header["start_unicode"]
        struct.pack_into(">H", data, table + 2, 0x0100)
        with self.assertRaisesRegex(ValueError, "single Unicode block"):
            tool.validate_font_records(data, header)


if __name__ == "__main__":
    unittest.main()
