#!/usr/bin/env python3
"""Validate and package the three dynamic external-font slots.

This tool deliberately does not patch compressed u8g2 glyph streams in place.
Adding/removing glyphs must be done by regenerating the font from its source
font/BDF.  The tool validates the generated .bin files, checks the glyphs used
    by the current UI, and can build a 512 KiB image matching font_store.c.
    Record bounds are checked here; compressed glyph payloads are guarded by
    the u8g2 decoder at runtime so this tool does not duplicate that decoder.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path


FLASH_SIZE = 0x80000
SECTOR_SIZE = 0x1000
DATA_BASE = 0x01000
MANIFEST_PRIMARY = 0x40
MANIFEST_SECONDARY = FLASH_SIZE - SECTOR_SIZE
DATA_CAPACITY = MANIFEST_SECONDARY - DATA_BASE
MANIFEST_SIZE = 60
MANIFEST_RECORD_SIZE = 12
MANIFEST_CRC_OFFSET = 48
MANIFEST_COMMIT_OFFSET = 52
MANIFEST_STATE_OFFSET = 56
MANIFEST_MAGIC = 0x33545346  # FST3, little endian
MANIFEST_VERSION = 3
MANIFEST_COMMIT = 0xA55AA55A
MANIFEST_STATE_READY = 0x5AA55AA5

SLOTS = {
    "WQY9": {
        "file": "u8g2_font_wqy9_t_lunar.bin",
        "metrics": (0, 12, 12, -2, 8, -2, 10, -1),
    },
    "WQY12": {
        "file": "u8g2_font_wqy12_t_lunar.bin",
        "metrics": (0, 16, 16, -2, 11, -3, 12, -4),
    },
    "HELVB18": {
        "file": "u8g2_font_helvB18_tn.bin",
        "metrics": (0, 13, 22, -3, 19, -5, 19, -5),
    },
}

# Characters used by the current UI while each slot is selected.  Space is
# intentionally not required in WQY12: the firmware treats an absent space as
# a zero-width formatting character.
UI_TEXT = {
    "WQY9": (
        "年月日星期周"
        "正月二月三月四月五月六月七月八月九月十月冬月腊月闰"
        "初一二三四五六七八九十廿"
        "猴鸡狗猪鼠牛虎兔龙蛇马羊"
        "庚辛壬癸甲乙丙丁戊己申酉戌亥子丑寅卯辰巳午未"
        "离还有天元旦节情人妇女植树愚人清明劳动青年儿童建党建军教师国庆万圣平安夜圣诞"
        "春节元宵龙抬头端午七夕中元中秋重阳寒衣腊八除夕休班"
        "℃"
    ),
    "WQY12": (
        "年月日星期周"
        "正月二月三月四月五月六月七月八月九月十月冬月腊月闰"
        "初一二三四五六七八九十廿"
        "猴鸡狗猪鼠牛虎兔龙蛇马羊"
        "庚辛壬癸甲乙丙丁戊己申酉戌亥子丑寅卯辰巳午未"
        "离还有天元旦节情人妇女植树愚人清明劳动青年儿童建党建军教师国庆万圣平安夜圣诞"
        "春节元宵龙抬头端午七夕中元中秋重阳寒衣腊八除夕休班"
        "℃"
    ),
    "HELVB18": "-0123456789",
}


def read_u16_be(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise ValueError(f"read outside font at 0x{offset:X}")
    return struct.unpack_from(">H", data, offset)[0]


def signed_byte(value: int) -> int:
    return value - 256 if value >= 128 else value


def parse_header(data: bytes) -> dict:
    if len(data) < 23:
        raise ValueError("font is shorter than the 23-byte u8g2 header")
    start_upper = read_u16_be(data, 17)
    start_lower = read_u16_be(data, 19)
    start_unicode = read_u16_be(data, 21)
    offsets = {
        "start_upper_A": start_upper,
        "start_lower_a": start_lower,
        "start_unicode": start_unicode,
    }
    for name, value in offsets.items():
        if 23 + value >= len(data):
            raise ValueError(f"{name}=0x{value:X} points outside the file")
    return {
        "glyph_count": data[0],
        "bbx_mode": data[1],
        "bits_per_0": data[2],
        "bits_per_1": data[3],
        "bits_per_char_width": data[4],
        "bits_per_char_height": data[5],
        "bits_per_char_x": data[6],
        "bits_per_char_y": data[7],
        "bits_per_delta_x": data[8],
        "max_char_width": data[9],
        "max_char_height": data[10],
        "x_offset": signed_byte(data[11]),
        "y_offset": signed_byte(data[12]),
        "ascent": signed_byte(data[13]),
        "descent": signed_byte(data[14]),
        "ascent_para": signed_byte(data[15]),
        "descent_para": signed_byte(data[16]),
        **offsets,
    }


def ascii_glyphs(data: bytes, start: int) -> set[int]:
    result: set[int] = set()
    pos = start
    while pos + 1 < len(data):
        code = data[pos]
        size = data[pos + 1]
        if size == 0:
            break
        if size < 2 or pos + size > len(data):
            raise ValueError(f"invalid ASCII glyph record at 0x{pos:X}")
        result.add(code)
        pos += size
    return result


def validate_ascii_records(data: bytes, start: int) -> None:
    offset = start
    for _ in range(len(data)):
        if offset + 2 > len(data):
            raise ValueError(f"invalid ASCII glyph record at 0x{offset:X}")
        size = data[offset + 1]
        if size == 0:
            return
        if size < 2 or offset + size > len(data):
            raise ValueError(f"invalid ASCII glyph record at 0x{offset:X}")
        offset += size
    raise ValueError("ASCII glyph table has no terminator")


def validate_unicode_records(data: bytes, start: int) -> None:
    offset = start
    for _ in range(len(data)):
        if offset + 2 > len(data):
            raise ValueError(f"invalid Unicode glyph record at 0x{offset:X}")
        code = read_u16_be(data, offset)
        if code == 0:
            return
        if offset + 3 > len(data):
            raise ValueError(f"invalid Unicode glyph record at 0x{offset:X}")
        size = data[offset + 2]
        if size < 3 or offset + size > len(data):
            raise ValueError(f"invalid Unicode glyph record at 0x{offset:X}")
        offset += size
    raise ValueError("Unicode glyph table has no terminator")


def validate_font_records(data: bytes, header: dict) -> None:
    validate_ascii_records(data, 23)
    validate_ascii_records(data, 23 + header["start_upper_A"])
    if header["start_lower_a"] != header["start_upper_A"]:
        validate_ascii_records(data, 23 + header["start_lower_a"])

    table = 23 + header["start_unicode"]
    lookup_offset = read_u16_be(data, table)
    first_codepoint = read_u16_be(data, table + 2)
    block = table + lookup_offset
    if block >= len(data):
        raise ValueError("Unicode lookup block points outside the font")
    if first_codepoint != 0:
        if first_codepoint != 0xFFFF or lookup_offset != 4:
            raise ValueError("slot fonts must use a single Unicode block")
        validate_unicode_records(data, block)


def has_glyph(data: bytes, codepoint: int) -> bool:
    header = parse_header(data)
    if codepoint <= 0xFF:
        start = 23
        if codepoint >= ord("a"):
            start += header["start_lower_a"]
        elif codepoint >= ord("A"):
            start += header["start_upper_A"]
        return codepoint in ascii_glyphs(data, start)

    font = 23 + header["start_unicode"]
    lookup = font
    # Mirror u8g2_font_get_glyph_data(): lookup offsets are cumulative and each
    # lookup entry selects the first Unicode value of a contiguous block.
    for _ in range(4096):
        offset = read_u16_be(data, lookup)
        first = read_u16_be(data, lookup + 2)
        if first == 0:
            return False
        font += offset
        if first >= codepoint:
            break
        lookup += 4
    else:
        raise ValueError("Unicode lookup table has no terminating/usable entry")

    for _ in range(4096):
        if font + 3 > len(data):
            raise ValueError(f"invalid Unicode glyph record at 0x{font:X}")
        value = read_u16_be(data, font)
        size = data[font + 2]
        if value == 0:
            return False
        if size < 3 or font + size > len(data):
            raise ValueError(f"invalid Unicode glyph size at 0x{font:X}")
        if value == codepoint:
            return True
        font += size
    raise ValueError("Unicode glyph table is unexpectedly long")


def check_slot(slot_name: str, data: bytes) -> dict:
    slot = SLOTS[slot_name]
    if len(data) == 0 or len(data) > DATA_CAPACITY:
        raise ValueError(f"{slot_name}: size {len(data)} is outside 1..{DATA_CAPACITY} bytes")
    header = parse_header(data)
    if header["glyph_count"] in (0, 0xFF):
        raise ValueError(f"{slot_name}: invalid glyph count {header['glyph_count']}")
    for field in (
        "bits_per_0", "bits_per_1", "bits_per_char_width",
        "bits_per_char_height", "bits_per_char_x", "bits_per_char_y",
        "bits_per_delta_x",
    ):
        if not 1 <= header[field] <= 8:
            raise ValueError(
                f"{slot_name}: {field}={header[field]} is outside the u8g2 range 1..8"
            )
    expected = slot["metrics"]
    actual = tuple(header[name] for name in (
        "x_offset", "max_char_width", "max_char_height", "y_offset",
        "ascent", "descent", "ascent_para", "descent_para",
    ))
    if actual != expected:
        raise ValueError(f"{slot_name}: header metrics {actual} != expected {expected}")
    validate_font_records(data, header)

    missing = []
    for char in dict.fromkeys(UI_TEXT[slot_name]):
        if not has_glyph(data, ord(char)):
            missing.append(f"U+{ord(char):04X}({char})")
    if missing:
        raise ValueError(f"{slot_name}: missing current-UI glyphs: {', '.join(missing)}")

    return {
        "slot": slot_name,
        "offset": 0,
        "capacity": DATA_CAPACITY,
        "file": slot["file"],
        "length": len(data),
        "crc32": zlib.crc32(data) & 0xFFFFFFFF,
        "header": header,
        "missing_ui_glyphs": [],
    }


def load_slot_file(slot_name: str, path: Path) -> tuple[bytes, dict]:
    data = path.read_bytes()
    return data, check_slot(slot_name, data)


def assign_offsets(records: list[dict]) -> int:
    offset = 0
    for record in records:
        if record["length"] == 0:
            record["offset"] = 0
            continue
        record["offset"] = offset
        offset += record["length"]
    if offset > DATA_CAPACITY:
        raise ValueError(f"font package size {offset} exceeds {DATA_CAPACITY} bytes")
    return offset


def make_manifest(records: list[dict], generation: int = 1) -> bytes:
    assign_offsets(records)
    raw = bytearray([0xFF] * MANIFEST_SIZE)
    struct.pack_into("<I", raw, 0, MANIFEST_MAGIC)
    struct.pack_into("<H", raw, 4, MANIFEST_VERSION)
    raw[6] = 3
    raw[7] = 0
    struct.pack_into("<I", raw, 8, generation)
    for index, record in enumerate(records):
        base = 12 + index * MANIFEST_RECORD_SIZE
        struct.pack_into("<I", raw, base, record["offset"])
        struct.pack_into("<I", raw, base + 4, record["length"])
        struct.pack_into("<I", raw, base + 8, record["crc32"])
    struct.pack_into("<I", raw, MANIFEST_CRC_OFFSET, zlib.crc32(raw[:MANIFEST_CRC_OFFSET]) & 0xFFFFFFFF)
    struct.pack_into("<I", raw, MANIFEST_COMMIT_OFFSET, MANIFEST_COMMIT)
    struct.pack_into("<I", raw, MANIFEST_STATE_OFFSET, MANIFEST_STATE_READY)
    return bytes(raw)


def default_input_dir() -> Path:
    return Path(__file__).resolve().parent / "dist"


def load_all(input_dir: Path, allow_missing: bool) -> tuple[list[bytes | None], list[dict]]:
    payloads: list[bytes | None] = []
    records: list[dict] = []
    for slot_name, slot in SLOTS.items():
        path = input_dir / slot["file"]
        if not path.exists():
            if not allow_missing:
                raise ValueError(f"missing {slot_name}: {path}")
            payloads.append(None)
            records.append({
                "slot": slot_name,
                "offset": 0,
                "capacity": DATA_CAPACITY,
                "file": slot["file"],
                "length": 0,
                "crc32": 0,
                "header": None,
                "missing_ui_glyphs": [],
            })
            continue
        data, record = load_slot_file(slot_name, path)
        payloads.append(data)
        records.append(record)
    return payloads, records


def command_check(args: argparse.Namespace) -> int:
    input_dir = Path(args.input_dir).resolve()
    _, records = load_all(input_dir, args.allow_missing)
    print(json.dumps({"input_dir": str(input_dir), "slots": records}, ensure_ascii=False, indent=2))
    return 0


def command_inspect(args: argparse.Namespace) -> int:
    path = Path(args.file).resolve()
    data = path.read_bytes()
    slot_name = args.slot.upper()
    record = check_slot(slot_name, data)
    record["path"] = str(path)
    record["ascii_glyphs"] = [
        f"0x{value:02X}"
        for value in sorted(ascii_glyphs(data, 23 + record["header"]["start_upper_A"]))
    ]
    print(json.dumps(record, ensure_ascii=False, indent=2))
    return 0


def command_pack(args: argparse.Namespace) -> int:
    input_dir = Path(args.input_dir).resolve()
    output = Path(args.output).resolve()
    payloads, records = load_all(input_dir, args.allow_missing)
    total_length = assign_offsets(records)
    image = bytearray([0xFF] * FLASH_SIZE)
    image[MANIFEST_PRIMARY:MANIFEST_PRIMARY + MANIFEST_SIZE] = make_manifest(records, args.generation)
    for payload, record in zip(payloads, records):
        if payload is None:
            continue
        start = DATA_BASE + record["offset"]
        end = start + len(payload)
        if end > MANIFEST_SECONDARY:
            raise ValueError(f"{record['slot']} exceeds flash image")
        image[start:end] = payload
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    sidecar = output.with_suffix(output.suffix + ".json")
    sidecar.write_text(json.dumps({"image": str(output), "slots": records},
                                  ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {output} ({len(image)} bytes, {total_length} bytes of font data)")
    print(f"wrote {sidecar}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    check = sub.add_parser("check", help="validate all three files")
    check.add_argument("--input-dir", default=str(default_input_dir()))
    check.add_argument("--allow-missing", action="store_true")
    check.set_defaults(func=command_check)

    inspect = sub.add_parser("inspect", help="validate one file and print its metadata")
    inspect.add_argument("slot", choices=sorted(SLOTS))
    inspect.add_argument("file")
    inspect.set_defaults(func=command_inspect)

    pack = sub.add_parser("pack", help="build a 512 KiB dynamically packed Flash image")
    pack.add_argument("--input-dir", default=str(default_input_dir()))
    pack.add_argument("--output", required=True)
    pack.add_argument("--generation", type=int, default=1)
    pack.add_argument("--allow-missing", action="store_true")
    pack.set_defaults(func=command_pack)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return args.func(args)
    except (OSError, ValueError, struct.error) as exc:
        print(f"font_store_tool: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
