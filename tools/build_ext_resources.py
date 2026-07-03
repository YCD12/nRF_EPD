#!/usr/bin/env python3
import base64
import binascii
import datetime
import json
import pathlib
import re
import struct


ROOT = pathlib.Path(__file__).resolve().parents[1]
DATA_START = 0x01000
DATA_END = 0x7CFFF
STAGING_MANIFEST = 0x7E000
# Active and staging manifest share one sector. Firmware COMMIT validates
# this sector and activates it in RAM; it does not copy manifest sectors.
MAIN_MANIFEST = STAGING_MANIFEST
SCRATCH = 0x7F000
MAGIC = 0x45505253
VERSION = 1
MAX_ENTRIES = 24
INCLUDE_EXTERNAL_LUT_RESOURCES = False

RES_TYPE_FONT = 1
RES_TYPE_LUNAR = 2
RES_TYPE_HOLIDAY = 3
RES_TYPE_UI = 4
RES_TYPE_LUT = 5
RES_TYPE_LUNAR_DB = 6

# Lunar Calendar Database (LCDB, resource type 6). See
# AiCoding/lunar_calendar_database_design_and_migration.md.
LUNAR_DB_ENTRY = 1
LUNAR_DB_MAGIC = 0x4C434442  # "LCDB"
LUNAR_DB_VERSION = 1
LUNAR_DB_RECORD_SIZE = 12
# Years with a dedicated data block. Firmware degrades other years to the first.
# 含 2025：设备默认时间戳为 2025-01-01，未校时开机需正确显示 2025 农历。
LUNAR_DB_YEARS = [2025, 2026, 2027]

FONT_IDS = {
    "u8g2_font_wqy9_t_lunar": 1,
    "u8g2_font_wqy12_t_lunar": 2,
    "u8g2_font_helvB14_tn": 3,
    "u8g2_font_helvB18_tn": 4,
}

LUNAR_IDS = {
    "lunar_month_names": 1,
    "lunar_leap_names": 2,
    "lunar_date_names": 3,
    "lunar_day_names": 4,
    "lunar_zodiac_names": 5,
    "lunar_stem_names": 6,
    "lunar_branch_names": 7,
    "lunar_jieqi_names": 8,
    "lunar_month_days": 9,
    "lunar_solar_1_1": 10,
    "lunar_jieqi_year_bits": 11,
    "lunar_jieqi_base_days": 12,
}

HOLIDAY_IDS = {
    "solar_festivals": 1,
    "lunar_festivals": 2,
    "holiday_adjustments": 3,
}

UI_IDS = {
    "ui_bundle": 1,
}

# UC8179 partial-refresh LUT tiers. Must match firmware UC8179.c T1/T2 values.
# Each tier resource is 6 tables x 42 bytes = 252 bytes, ordered as:
#   LUTC, LUTWW, LUTKW, LUTWK, LUTKK, LUTBD.
# Each 42-byte table is [waveform, T1, T2, 0, 0, repeat=1, 0*36].
LUT_TIER_TIMINGS = [
    (56, 22),  # tier 0  < 7C
    (52, 20),  # tier 1  7-9C
    (48, 18),  # tier 2  10-12C
    (44, 16),  # tier 3  13-15C
    (48, 18),  # tier 4  16-18C
    (50, 18),  # tier 5  19-21C
    (42, 14),  # tier 6  22-24C
    (36, 12),  # tier 7  25-27C
    (30, 10),  # tier 8  28-30C
    (20, 6),   # tier 9  31-33C
    (16, 5),   # tier 10 >= 34C
]
LUT_TABLE_WAVEFORMS = [0x00, 0x20, 0x58, 0xA8, 0x00, 0x00]  # LUTC,WW,KW,WK,KK,BD
LUT_TABLE_SIZE = 42
LUT_TABLES_PER_TIER = 6

UI_MAGIC = 0x314C4955
UI_VERSION = 1
UI_SCHEMA_MAJOR = 1
UI_SCHEMA_MINOR = 0
UI_COLOR_DEPTH_BWR = 2
UI_HEADER_SIZE = 28
UI_MODE_SIZE = 16
UI_COMPONENT_SIZE = 24

UI_COMPONENTS = {
    "week_header": 2,
    "month_grid": 3,
    "time_7seg": 4,
    "battery": 5,
    "line": 6,
    "low_battery_tip": 10,
    "time_sync_tip": 11,
    "text_field": 12,
}

UI_FLAG_PARTIAL_ONLY = 0x01
UI_TEXT_FLAG_RIGHT_ALIGN = 0x02
UI_TEXT_FLAG_ZERO_PAD = 0x04
UI_TEXT_FLAG_WITH_ZODIAC = 0x08
UI_TEXT_FLAG_WEEK_LEADING_SPACE = 0x10


def synthesize_mode_partial(mode):
    """P2: derive mode partial rect from PARTIAL_ONLY component union (pack-time)."""
    explicit = list(mode.get("partial", [0, 0, 0, 0]))
    partial_comps = [
        c for c in mode.get("components", [])
        if (c.get("flags", 0) & UI_FLAG_PARTIAL_ONLY) and c.get("w", 0) > 0 and c.get("h", 0) > 0
    ]
    if not partial_comps:
        return explicit

    min_x = min(c.get("x", 0) for c in partial_comps)
    min_y = min(c.get("y", 0) for c in partial_comps)
    max_x = max(c.get("x", 0) + c.get("w", 0) for c in partial_comps)
    max_y = max(c.get("y", 0) + c.get("h", 0) for c in partial_comps)
    if max_x <= min_x or max_y <= min_y:
        return explicit
    return [min_x, min_y, max_x - min_x, max_y - min_y]

UI_TEXT = {
    "date_ymd": 1,
    "lunar_date": 2,
    "temperature": 3,
    "lunar_year": 4,
    "week_number": 5,
    "jieqi": 6,
    "weekday": 7,
    "temp_ssid": 8,
    "ssid": 9,
}

LUNAR_MONTH_NAMES = ["----", "正月", "二月", "三月", "四月", "五月", "六月", "七月", "八月", "九月", "十月", "冬月", "腊月"]
LUNAR_LEAP_NAMES = [" ", "闰"]
LUNAR_DATE_NAMES = [
    "----", "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
]
LUNAR_DAY_NAMES = ["日", "一", "二", "三", "四", "五", "六"]
LUNAR_ZODIAC_NAMES = ["猴", "鸡", "狗", "猪", "鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊"]
LUNAR_STEM_NAMES = ["庚", "辛", "壬", "癸", "甲", "乙", "丙", "丁", "戊", "己"]
LUNAR_BRANCH_NAMES = ["申", "酉", "戌", "亥", "子", "丑", "寅", "卯", "辰", "巳", "午", "未"]
LUNAR_JIEQI_NAMES = [
    "小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明", "谷雨",
    "立夏", "小满", "芒种", "夏至", "小暑", "大暑", "立秋", "处暑",
    "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至",
]
LUNAR_MONTH_DAYS = [2024, 0x0000096C, 0x0000D4AE, 0x0000149C, 0x00001A4C, 0x0000BD26, 0x00001AA6]
LUNAR_SOLAR_1_1 = [2024, 0x000FD04A, 0x000FD23D, 0x000FD451, 0x000FD646, 0x000FD83A, 0x000FDA4D]
LUNAR_JIEQI_YEAR_BITS = bytes([
    0x0F, 0xEF, 0xDB,
    0xBE, 0xA6, 0x99,
    0x9C, 0xA2, 0x98,
    0x80, 0x00, 0x18,
    0x0F, 0xEF, 0xDB,
    0xBE, 0xA6, 0x99,
])
LUNAR_JIEQI_BASE_DAYS = bytes([
    6, 20, 4, 19, 6, 21,
    5, 20, 6, 21, 6, 21,
    7, 23, 8, 23, 8, 23,
    8, 24, 8, 22, 7, 22,
])

SOLAR_FESTIVALS = [
    (1, 1, "元旦节"), (2, 14, "情人节"), (3, 8, "妇女节"), (3, 12, "植树节"), (4, 1, "愚人节"),
    (5, 1, "劳动节"), (5, 4, "青年节"), (6, 1, "儿童节"), (7, 1, "建党节"), (8, 1, "建军节"),
    (9, 10, "教师节"), (10, 1, "国庆节"), (11, 1, "万圣节"), (12, 24, "平安夜"), (12, 25, "圣诞节"),
]
LUNAR_FESTIVALS = [
    (1, 1, "春节"), (1, 15, "元宵节"), (2, 2, "龙抬头"), (5, 5, "端午节"), (7, 7, "七夕节"),
    (7, 15, "中元节"), (8, 15, "中秋节"), (9, 9, "重阳节"), (10, 1, "寒衣节"), (12, 8, "腊八节"),
    (12, 30, "除夕"),
]
# 调休编码 (per-day, uint16): value = (month << 8) | day.
#   bit[15:12] (最高半字节) > 0 表示"班"(调休上班)，否则为"休"(法定休假)。
# 以年份为 Key 组织，支持多年份 LCDB 生成（见文档优化 17）。
HOLIDAY_YEAR = 2026
HOLIDAYS = {
    2026: [
        0x0101, 0x0102, 0x0103, 0x1104, 0x120E, 0x020F, 0x0210, 0x0211, 0x0212, 0x0213, 0x0214, 0x0215, 0x0216,
        0x0217, 0x121C, 0x0404, 0x0405, 0x0406, 0x0501, 0x0502, 0x0503, 0x0504, 0x0505, 0x1509, 0x0613, 0x0614,
        0x0615, 0x0919, 0x091A, 0x091B, 0x1914, 0x0A01, 0x0A02, 0x0A03, 0x0A04, 0x0A05, 0x0A06, 0x0A07, 0x1A0A,
    ],
    # 2027 国家法定调休尚未正式公布，先留空占位；公布后填入即可自动进入 LCDB。
    2027: [],
}


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def parse_c_string_payload(text):
    out = bytearray()
    i = 0
    while i < len(text):
        ch = text[i]
        if ch != "\\":
            out.append(ord(ch) & 0xFF)
            i += 1
            continue

        i += 1
        if i >= len(text):
            break
        esc = text[i]
        if esc in "01234567":
            digits = esc
            i += 1
            for _ in range(2):
                if i < len(text) and text[i] in "01234567":
                    digits += text[i]
                    i += 1
                else:
                    break
            out.append(int(digits, 8) & 0xFF)
            continue

        escapes = {
            "n": 10,
            "r": 13,
            "t": 9,
            "b": 8,
            "f": 12,
            "v": 11,
            "a": 7,
            "\\": 92,
            '"': 34,
            "'": 39,
            "?": 63,
            "0": 0,
        }
        out.append(escapes.get(esc, ord(esc)) & 0xFF)
        i += 1
    return bytes(out)


def parse_fonts():
    source = (ROOT / "GUI" / "fonts.c").read_text(encoding="utf-8")
    pattern = re.compile(
        r"const\s+uint8_t\s+(\w+)\s*\[(\d+)\]\s+U8G2_FONT_SECTION\([^)]*\)\s*=",
        re.S,
    )
    found = {}
    for match in pattern.finditer(source):
        name = match.group(1)
        declared_len = match.group(2)
        start = match.end()
        in_string = False
        escaped = False
        end = start
        while end < len(source):
            ch = source[end]
            if in_string:
                if escaped:
                    escaped = False
                elif ch == "\\":
                    escaped = True
                elif ch == '"':
                    in_string = False
            else:
                if ch == '"':
                    in_string = True
                elif ch == ";":
                    break
            end += 1
        initializer = source[start:end]
        literals = re.findall(r'"((?:\\.|[^"\\])*)"', initializer, re.S)
        payload = b"".join(parse_c_string_payload(lit) for lit in literals)
        declared_len = int(declared_len)
        if len(payload) + 1 == declared_len:
            payload += b"\x00"
        if len(payload) != declared_len:
            raise ValueError(f"{name}: declared {declared_len}, parsed {len(payload)}")
        found[name] = payload

    missing = sorted(set(FONT_IDS) - set(found))
    if missing:
        raise ValueError(f"Missing font arrays: {', '.join(missing)}")
    return [(RES_TYPE_FONT, FONT_IDS[name], found[name], name) for name in FONT_IDS]


def fixed_strings(strings, item_size):
    out = bytearray()
    for text in strings:
        encoded = text.encode("utf-8")
        if len(encoded) >= item_size:
            raise ValueError(f"String too long for {item_size} bytes: {text}")
        out += encoded
        out += b"\x00" * (item_size - len(encoded))
    return bytes(out)


def pack_u32(values):
    return b"".join(struct.pack("<I", value) for value in values)


def pack_festivals(items):
    out = bytearray()
    for month, day, name in items:
        encoded = name.encode("utf-8")
        if len(encoded) >= 10:
            raise ValueError(f"Festival name too long: {name}")
        out += struct.pack("<BB", month, day)
        out += encoded + b"\x00" * (10 - len(encoded))
    return bytes(out)


def pack_holidays():
    values = HOLIDAYS.get(HOLIDAY_YEAR, [])
    out = bytearray(struct.pack("<HH", HOLIDAY_YEAR, len(values)))
    for value in values:
        out += struct.pack("<H", value)
    return bytes(out)


def parse_lunar_resources():
    return [
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_month_names"], fixed_strings(LUNAR_MONTH_NAMES, 7), "lunar_month_names"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_leap_names"], fixed_strings(LUNAR_LEAP_NAMES, 4), "lunar_leap_names"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_date_names"], fixed_strings(LUNAR_DATE_NAMES, 7), "lunar_date_names"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_day_names"], fixed_strings(LUNAR_DAY_NAMES, 4), "lunar_day_names"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_zodiac_names"], fixed_strings(LUNAR_ZODIAC_NAMES, 4), "lunar_zodiac_names"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_stem_names"], fixed_strings(LUNAR_STEM_NAMES, 4), "lunar_stem_names"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_branch_names"], fixed_strings(LUNAR_BRANCH_NAMES, 4), "lunar_branch_names"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_jieqi_names"], fixed_strings(LUNAR_JIEQI_NAMES, 7), "lunar_jieqi_names"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_month_days"], pack_u32(LUNAR_MONTH_DAYS), "lunar_month_days"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_solar_1_1"], pack_u32(LUNAR_SOLAR_1_1), "lunar_solar_1_1"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_jieqi_year_bits"], LUNAR_JIEQI_YEAR_BITS, "lunar_jieqi_year_bits"),
        (RES_TYPE_LUNAR, LUNAR_IDS["lunar_jieqi_base_days"], LUNAR_JIEQI_BASE_DAYS, "lunar_jieqi_base_days"),
    ]


def parse_holiday_resources():
    return [
        (RES_TYPE_HOLIDAY, HOLIDAY_IDS["solar_festivals"], pack_festivals(SOLAR_FESTIVALS), "solar_festivals"),
        (RES_TYPE_HOLIDAY, HOLIDAY_IDS["lunar_festivals"], pack_festivals(LUNAR_FESTIVALS), "lunar_festivals"),
        (RES_TYPE_HOLIDAY, HOLIDAY_IDS["holiday_adjustments"], pack_holidays(), "holiday_adjustments"),
    ]


# ---------------------------------------------------------------------------
# Lunar Calendar Database (LCDB) generation.
#
# The algorithms below are 1:1 ports of the firmware routines in GUI/Lunar.c
# (LUNAR_SolarToLunar / GetJieQi) and GUI/GUI.c (GetFestival). Keeping them
# byte-for-byte equivalent guarantees the pre-computed database renders exactly
# like the current on-device computation. The shared lookup tables
# (LUNAR_SOLAR_1_1 / LUNAR_MONTH_DAYS / LUNAR_JIEQI_*) are indexed as
# [base_marker(2024), y2024, y2025, ..., y2029], so year Y maps to index
# Y - 2024 + 1.
# ---------------------------------------------------------------------------

LUNAR_TABLE_BASE_YEAR = 2024
LUNAR_TABLE_YEAR_COUNT = 6


def _is_leap_year(year):
    if year % 100 == 0:
        return year % 400 == 0
    return year % 4 == 0


def _lunar_bitint(data, length, shift):
    return (data & (((1 << length) - 1) << shift)) >> shift


def _solar_to_int(y, m, d):
    m = (m + 9) % 12
    y = y - m // 10
    return 365 * y + y // 4 - y // 100 + y // 400 + (m * 306 + 5) // 10 + (d - 1)


def solar_to_lunar(year, month, day):
    """Port of LUNAR_SolarToLunar. Returns (lunar_month, lunar_day, is_leap, lunar_year)."""
    year_min = LUNAR_TABLE_BASE_YEAR + 1
    year_max = LUNAR_TABLE_BASE_YEAR + LUNAR_TABLE_YEAR_COUNT - 1
    calc_year = year
    if calc_year < year_min or calc_year > year_max:
        calc_year = year_min
    year_index = calc_year - LUNAR_TABLE_BASE_YEAR + 1

    solar_data = (calc_year << 9) | (month << 5) | day
    if LUNAR_SOLAR_1_1[year_index] > solar_data and year_index > 1:
        year_index -= 1

    solar11 = LUNAR_SOLAR_1_1[year_index]
    y = _lunar_bitint(solar11, 12, 9)
    m = _lunar_bitint(solar11, 4, 5)
    d = _lunar_bitint(solar11, 5, 0)
    offset = _solar_to_int(calc_year, month, day) - _solar_to_int(y, m, d)

    days = LUNAR_MONTH_DAYS[year_index]
    leap = _lunar_bitint(days, 4, 13)
    lunar_year = LUNAR_TABLE_BASE_YEAR + year_index - 1
    lunar_month = 1
    offset += 1
    for i in range(13):
        dm = 30 if _lunar_bitint(days, 1, 12 - i) == 1 else 29
        if offset > dm:
            lunar_month += 1
            offset -= dm
        else:
            break

    is_leap = 0
    if leap != 0 and lunar_month > leap:
        if lunar_month == leap + 1:
            is_leap = 1
        lunar_month -= 1
    return lunar_month, offset, is_leap, lunar_year


def _get_jieqi_date(year, month, mday):
    """Port of GetJieQi: solar-term day-of-month for the half-month, or 0."""
    if month == 0 or month > 12:
        return 0
    year_min = LUNAR_TABLE_BASE_YEAR + 1
    year_max = LUNAR_TABLE_BASE_YEAR + LUNAR_TABLE_YEAR_COUNT - 1
    calc_year = year
    if calc_year < year_min or calc_year > year_max:
        calc_year = year_min

    jq = (month - 1) * 2
    if mday >= 15:
        jq += 1
    bak1 = LUNAR_JIEQI_YEAR_BITS[(calc_year - LUNAR_TABLE_BASE_YEAR) * 3 + jq // 8]
    value = ((bak1 << (jq % 8)) & 0x80)
    jqdate = LUNAR_JIEQI_BASE_DAYS[jq]
    if jqdate == 0:
        return 0
    if value != 0:
        if jq in (1, 11, 18, 21) and year < 2044:
            jqdate += 1
        else:
            jqdate -= 1
    return jqdate


def get_solar_term(year, month, day):
    """Return 1..24 (index into LUNAR_JIEQI_NAMES + 1) when the date is a solar term, else 0."""
    jqdate = _get_jieqi_date(year, month, day)
    if jqdate != 0 and jqdate == day:
        jq = (month - 1) * 2 + (1 if day >= 15 else 0)
        return jq + 1
    return 0


def get_festival(year, month, day, dow_sun0, lunar):
    """Port of GUI.c GetFestival priority order, excluding solar terms
    (which are stored separately in daily_record.solar_term)."""
    lunar_month, lunar_day, _is_leap, _ly = lunar

    for m, d, name in LUNAR_FESTIVALS:
        if lunar_month == m and lunar_day == d:
            return name

    # 除夕：农历 12/29 且次日为正月初一（12/30 已在上面命中）。
    if lunar_month == 12 and lunar_day == 29:
        nxt = datetime.date(year, month, day) + datetime.timedelta(days=1)
        n_month, n_day, _nl, _nly = solar_to_lunar(nxt.year, nxt.month, nxt.day)
        if n_month == 1 and n_day == 1:
            return "除夕"

    if month == 5 and dow_sun0 == 0 and 8 <= day <= 14:
        return "母亲节"
    if month == 6 and dow_sun0 == 0 and 15 <= day <= 21:
        return "父亲节"
    if month == 11 and dow_sun0 == 4 and 22 <= day <= 28:
        return "感恩节"

    for m, d, name in SOLAR_FESTIVALS:
        if month == m and day == d:
            return name
    return None


def _holiday_type(year, month, day):
    """0: 普通; 1: 休; 2: 班. Matches GUI.c decode of holiday_adjustments."""
    for value in HOLIDAYS.get(year, []):
        if ((value >> 8) & 0xF) == month and (value & 0xFF) == day:
            return 2 if ((value >> 12) & 0xF) > 0 else 1
    return 0


def _build_lunar_db_year_block(year):
    total_days = 366 if _is_leap_year(year) else 365

    string_pool = bytearray(b"\x00")  # index 0 reserved as empty string
    pool_index = {}

    def intern(text):
        if text in pool_index:
            return pool_index[text]
        offset = len(string_pool)
        if offset > 0xFFFE:
            raise ValueError(f"LCDB string pool overflow for year {year}")
        string_pool.extend(text.encode("utf-8") + b"\x00")
        pool_index[text] = offset
        return offset

    records = bytearray()
    day0 = datetime.date(year, 1, 1)
    for doy in range(total_days):
        cur = day0 + datetime.timedelta(days=doy)
        month, day = cur.month, cur.day
        lunar = solar_to_lunar(year, month, day)
        lunar_month, lunar_day, is_leap, lunar_year = lunar
        dow_sun0 = (cur.weekday() + 1) % 7  # Python Mon=0 -> Sun=0 convention

        solar_term = get_solar_term(year, month, day)
        festival = get_festival(year, month, day, dow_sun0, lunar)
        festival_offset = intern(festival) if festival else 0xFFFF
        holiday_type = _holiday_type(year, month, day)
        # 干支年基于农历年，且沿用固件既有名词数组语义 (index % 10 / % 12)。
        jiazi_year = lunar_year % 60
        lunar_month_byte = (lunar_month & 0x7F) | (0x80 if is_leap else 0x00)

        records += struct.pack(
            "<BBBBBBBBHH",
            lunar_month_byte,  # lunar_month (bit7 = leap)
            lunar_day,         # lunar_day
            jiazi_year,        # jiazi_year (0-59)
            0,                 # jiazi_month (reserved)
            0,                 # jiazi_day (reserved)
            solar_term,        # solar_term (0 或 1-24)
            holiday_type,      # holiday_type (0/1/2)
            0,                 # reserved
            festival_offset,   # festival_offset (0xFFFF = none)
            0xFFFF,            # special_label (0xFFFF = none)
        )

    string_pool_offset = 8 + len(records)  # relative to year block start
    header = struct.pack("<HHI", year, total_days, string_pool_offset)
    return header + bytes(records) + bytes(string_pool)


def parse_lunar_db_resources():
    years = LUNAR_DB_YEARS
    year_count = len(years)

    blocks = [_build_lunar_db_year_block(y) for y in years]

    header_size = 16
    offset_table_size = year_count * 4
    offsets = []
    cursor = header_size + offset_table_size
    for block in blocks:
        offsets.append(cursor)
        cursor += len(block)

    out = bytearray()
    out += struct.pack("<IBBHB", LUNAR_DB_MAGIC, LUNAR_DB_VERSION, year_count, years[0], LUNAR_DB_RECORD_SIZE)
    out += b"\x00" * 7  # reserved, pads master header to 16 bytes
    for offset in offsets:
        out += struct.pack("<I", offset)
    for block in blocks:
        out += block

    return [(RES_TYPE_LUNAR_DB, LUNAR_DB_ENTRY, bytes(out), "lunar_db")]


def default_ui_layout():
    return {
        "version": UI_VERSION,
        "schema": {"major": UI_SCHEMA_MAJOR, "minor": UI_SCHEMA_MINOR},
        "screen": {"width": 800, "height": 480, "colorDepth": UI_COLOR_DEPTH_BWR},
        "modes": [
            {
                "id": 1,
                "name": "calendar",
                "updatePolicy": 1,
                "partial": [0, 0, 0, 0],
                "components": [
                    {"type": "text_field", "dataId": UI_TEXT["date_ymd"], "x": 10, "y": 36, "w": 260, "h": 24},
                    {"type": "text_field", "dataId": UI_TEXT["lunar_year"], "x": 205, "y": 24, "w": 140, "h": 14, "fontId": 9, "flags": UI_TEXT_FLAG_WITH_ZODIAC},
                    {"type": "text_field", "dataId": UI_TEXT["lunar_date"], "x": 205, "y": 38, "w": 90, "h": 14, "fontId": 9},
                    {"type": "text_field", "dataId": UI_TEXT["week_number"], "x": 295, "y": 38, "w": 70, "h": 14, "fontId": 9, "flags": UI_TEXT_FLAG_WEEK_LEADING_SPACE},
                    {"type": "battery", "x": 788, "y": 16, "w": 20, "h": 12},
                    {"type": "text_field", "dataId": UI_TEXT["ssid"], "x": 560, "y": 38, "w": 230, "h": 12, "fontId": 9, "flags": UI_TEXT_FLAG_RIGHT_ALIGN},
                    {"type": "week_header", "x": 10, "y": 44, "w": 780, "h": 32},
                    {"type": "month_grid", "x": 10, "y": 84, "w": 780, "h": 386},
                    {"type": "time_sync_tip", "x": 0, "y": 0, "w": 800, "h": 480},
                ],
            },
            {
                "id": 2,
                "name": "clock_calendar",
                "updatePolicy": 2,
                "components": [
                    {"type": "line", "x": 15, "y": 45, "w": 770, "h": 2, "fg": 0},
                    {"type": "line", "x": 15, "y": 435, "w": 770, "h": 2, "fg": 0},
                    {"type": "line", "x": 390, "y": 70, "w": 1, "h": 350, "fg": 0},
                    {"type": "battery", "x": 788, "y": 77, "w": 20, "h": 12},
                    {"type": "text_field", "dataId": UI_TEXT["ssid"], "x": 400, "y": 86, "w": 230, "h": 12, "fontId": 9},
                    {"type": "week_header", "x": 397, "y": 96, "w": 398, "h": 32},
                    {"type": "month_grid", "x": 397, "y": 136, "w": 398, "h": 284},
                    {"type": "text_field", "dataId": UI_TEXT["date_ymd"], "x": 15, "y": 92, "w": 362, "h": 24},
                    {"type": "text_field", "dataId": UI_TEXT["lunar_date"], "x": 15, "y": 115, "w": 160, "h": 16, "fontId": 12},
                    {"type": "text_field", "dataId": UI_TEXT["temperature"], "x": 15, "y": 115, "w": 362, "h": 24, "flags": UI_TEXT_FLAG_RIGHT_ALIGN},
                    {"type": "line", "x": 15, "y": 128, "w": 362, "h": 1, "fg": 0},
                    {"type": "time_7seg", "x": 56, "y": 182, "w": 272, "h": 124, "p0": 6, "p1": 2},
                    {"type": "line", "x": 15, "y": 360, "w": 362, "h": 1, "fg": 0},
                    {"type": "text_field", "dataId": UI_TEXT["lunar_year"], "x": 15, "y": 380, "w": 160, "h": 16},
                    {"type": "text_field", "dataId": UI_TEXT["week_number"], "x": 15, "y": 404, "w": 160, "h": 16},
                    {"type": "text_field", "dataId": UI_TEXT["jieqi"], "x": 15, "y": 380, "w": 362, "h": 40, "p0": 404},
                    {"type": "time_7seg", "x": 56, "y": 182, "w": 272, "h": 124, "p0": 6, "p1": 2, "flags": UI_FLAG_PARTIAL_ONLY},
                ],
            },
            {
                "id": 3,
                "name": "clock",
                "updatePolicy": 2,
                "partial": [0, 0, 0, 0],
                "components": [
                    {"type": "text_field", "dataId": UI_TEXT["date_ymd"], "x": 100, "y": 36, "w": 220, "h": 24, "flags": UI_TEXT_FLAG_ZERO_PAD},
                    {"type": "text_field", "dataId": UI_TEXT["weekday"], "x": 100, "y": 58, "w": 38, "h": 12, "fontId": 9},
                    {"type": "text_field", "dataId": UI_TEXT["lunar_date"], "x": 138, "y": 58, "w": 160, "h": 12, "fontId": 9},
                    {"type": "battery", "x": 700, "y": 25, "w": 20, "h": 12},
                    {"type": "text_field", "dataId": UI_TEXT["temp_ssid"], "x": 100, "y": 58, "w": 600, "h": 12, "fontId": 9},
                    {"type": "line", "x": 90, "y": 68, "w": 620, "h": 1, "fg": 0},
                    {"type": "time_7seg", "x": 166, "y": 139, "w": 468, "h": 202, "p0": 10, "p1": 2},
                    {"type": "line", "x": 90, "y": 412, "w": 620, "h": 1, "fg": 0},
                    {"type": "text_field", "dataId": UI_TEXT["lunar_year"], "x": 100, "y": 442, "w": 220, "h": 16, "flags": UI_TEXT_FLAG_WITH_ZODIAC},
                    {"type": "text_field", "dataId": UI_TEXT["week_number"], "x": 100, "y": 462, "w": 160, "h": 16, "flags": UI_TEXT_FLAG_WEEK_LEADING_SPACE},
                    {"type": "text_field", "dataId": UI_TEXT["jieqi"], "x": 100, "y": 442, "w": 600, "h": 38, "p0": 462},
                    {"type": "time_sync_tip", "x": 0, "y": 0, "w": 800, "h": 480},
                ],
            },
            {
                "id": 250,
                "name": "low_battery",
                "updatePolicy": 0,
                "partial": [0, 0, 0, 0],
                "components": [
                    {"type": "low_battery_tip", "x": 0, "y": 0, "w": 800, "h": 480},
                ],
            },
        ],
    }


def pack_ui_bundle(layout):
    components = []
    mode_records = []

    for mode in layout["modes"]:
        start = len(components)
        for comp in mode["components"]:
            components.append(comp)
        mode_records.append({
            "id": mode["id"],
            "updatePolicy": mode.get("updatePolicy", 0),
            "componentStart": start,
            "componentCount": len(mode["components"]),
            "partial": synthesize_mode_partial(mode),
            "flags": mode.get("flags", 0),
        })

    mode_offset = UI_HEADER_SIZE
    component_offset = mode_offset + len(mode_records) * UI_MODE_SIZE
    string_offset = component_offset + len(components) * UI_COMPONENT_SIZE
    string_table = b"\x00"
    schema = layout.get("schema", {})
    schema_major = schema.get("major", UI_SCHEMA_MAJOR)
    schema_minor = schema.get("minor", UI_SCHEMA_MINOR)
    color_depth = layout.get("screen", {}).get("colorDepth", UI_COLOR_DEPTH_BWR)

    out = bytearray()
    out += struct.pack(
        "<IHHHBBHHHHBBBBI",
        UI_MAGIC,
        UI_VERSION,
        layout["screen"]["width"],
        layout["screen"]["height"],
        len(mode_records),
        len(components),
        mode_offset,
        component_offset,
        string_offset,
        len(string_table),
        schema_major,
        schema_minor,
        color_depth,
        0,
        0,
    )

    for mode in mode_records:
        px, py, pw, ph = mode["partial"]
        out += struct.pack(
            "<BBBBHHHHHH",
            mode["id"],
            mode["updatePolicy"],
            mode["componentStart"],
            mode["componentCount"],
            px,
            py,
            pw,
            ph,
            mode["flags"],
            0,
        )

    for comp in components:
        out += struct.pack(
            "<BBBBhhhhHHhhhH",
            UI_COMPONENTS[comp["type"]],
            comp.get("dataId", 0),
            comp.get("fontId", 0),
            comp.get("flags", 0),
            comp.get("x", 0),
            comp.get("y", 0),
            comp.get("w", 0),
            comp.get("h", 0),
            comp.get("fg", 0),
            comp.get("bg", 1),
            comp.get("p0", 0),
            comp.get("p1", 0),
            comp.get("p2", 0),
            comp.get("textOffset", 0),
        )

    out += string_table
    return bytes(out)


def parse_ui_resources():
    layout = default_ui_layout()
    return [(RES_TYPE_UI, UI_IDS["ui_bundle"], pack_ui_bundle(layout), "ui_bundle")]


def pack_lut_tier(tier_index):
    t1, t2 = LUT_TIER_TIMINGS[tier_index]
    out = bytearray()
    for waveform in LUT_TABLE_WAVEFORMS:
        table = bytearray(LUT_TABLE_SIZE)
        table[0] = waveform
        table[1] = t1
        table[2] = t2
        table[3] = 0  # T3 must be 0 for partial refresh
        table[4] = 0  # T4 must be 0 for partial refresh
        table[5] = 1  # repeat count
        out += table
    return bytes(out)


def parse_lut_resources():
    return [
        (RES_TYPE_LUT, tier, pack_lut_tier(tier), f"uc8179_lut_tier{tier}")
        for tier in range(len(LUT_TIER_TIMINGS))
    ]


def align4(value):
    return (value + 3) & ~3


def build_manifest(entries):
    manifest = bytearray()
    manifest += struct.pack("<IHH", MAGIC, VERSION, len(entries))
    for entry in entries:
        manifest += struct.pack("<HHIII", entry["type"], entry["id"],
                                entry["offset"], entry["length"], entry["crc32"])
    for _ in range(MAX_ENTRIES - len(entries)):
        manifest += struct.pack("<HHIII", 0, 0, 0, 0, 0)
    manifest += struct.pack("<I", 0)
    manifest_crc = crc32(manifest)
    struct.pack_into("<I", manifest, len(manifest) - 4, manifest_crc)
    return bytes(manifest), manifest_crc


def get_u8g2_font_supported_chars(font_bytes):
    start_pos_unicode = int.from_bytes(font_bytes[21:23], byteorder='big')
    supported = set()
    idx = 23
    while idx + 1 < len(font_bytes):
        glyph_size = font_bytes[idx + 1]
        if glyph_size == 0:
            break
        encoding = font_bytes[idx]
        supported.add(chr(encoding))
        idx += glyph_size
    lookup_table_start = 23 + start_pos_unicode
    if lookup_table_start < len(font_bytes):
        first_subblock_rel_offset = int.from_bytes(font_bytes[lookup_table_start:lookup_table_start+2], byteorder='big')
        num_entries = first_subblock_rel_offset // 4
        current_subblock_offset = lookup_table_start
        for i in range(num_entries):
            entry_offset = lookup_table_start + i * 4
            rel_offset = int.from_bytes(font_bytes[entry_offset:entry_offset+2], byteorder='big')
            current_subblock_offset += rel_offset
            idx = current_subblock_offset
            while idx + 2 < len(font_bytes):
                e_val = int.from_bytes(font_bytes[idx:idx+2], byteorder='big')
                if e_val == 0:
                    break
                glyph_size = font_bytes[idx + 2]
                if glyph_size == 0:
                    break
                supported.add(chr(e_val))
                idx += glyph_size
    return supported


def validate_font_characters(resources, ui_layout):
    fonts = {}
    for res_type, res_id, payload, name in resources:
        if res_type == RES_TYPE_FONT:
            fonts[name] = get_u8g2_font_supported_chars(payload)

    # Base sets of characters that must be supported
    required_chinese = set()
    for _, _, name in SOLAR_FESTIVALS:
        required_chinese.update(name)
    for _, _, name in LUNAR_FESTIVALS:
        required_chinese.update(name)

    SOLAR_TERMS = [
        "小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明", "谷雨",
        "立夏", "小满", "芒种", "夏至", "小暑", "大暑", "立秋", "处暑",
        "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至"
    ]
    for term in SOLAR_TERMS:
        required_chinese.update(term)

    # 1. wqy9 required characters (used for all small UI text elements)
    wqy9_required = set(required_chinese)
    # Fixed Chinese calendar strings/stems/branches/zodiacs (excluding '卅' as it is never used)
    wqy9_required.update("闰月年周星期日一二三四五六初十甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥鼠牛虎兔龙蛇马羊猴鸡狗猪℃[]")
    wqy9_ascii = set("0123456789-:. ")

    # 2. wqy12 required characters (used for large grid days, sync screen etc.)
    wqy12_required = set(required_chinese)
    wqy12_required.update("闰月年周星期日一二三四五六初十甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥鼠牛虎兔龙蛇马羊猴鸡狗猪℃离还有天")
    wqy12_ascii = set("0123456789")

    # Traverse ui_layout for any future static text values
    def find_static_texts(obj):
        texts = []
        if isinstance(obj, dict):
            for k, v in obj.items():
                if k == "text" and isinstance(v, str):
                    texts.append(v)
                else:
                    texts.extend(find_static_texts(v))
        elif isinstance(obj, list):
            for item in obj:
                texts.extend(find_static_texts(item))
        return texts

    for text in find_static_texts(ui_layout):
        wqy9_required.update(text)
        wqy12_required.update(text)

    # Font: wqy9
    if "u8g2_font_wqy9_t_lunar" in fonts:
        supported = fonts["u8g2_font_wqy9_t_lunar"]
        missing_cn = sorted(list(wqy9_required - supported))
        if missing_cn:
            raise ValueError(f"Font validation failed: 'u8g2_font_wqy9_t_lunar' is missing required characters: {''.join(missing_cn)}")
        missing_ascii = sorted(list(wqy9_ascii - supported))
        if missing_ascii:
            raise ValueError(f"Font validation failed: 'u8g2_font_wqy9_t_lunar' is missing required ASCII characters: {''.join(missing_ascii)}")

    # Font: wqy12
    if "u8g2_font_wqy12_t_lunar" in fonts:
        supported = fonts["u8g2_font_wqy12_t_lunar"]
        missing_cn = sorted(list(wqy12_required - supported))
        if missing_cn:
            raise ValueError(f"Font validation failed: 'u8g2_font_wqy12_t_lunar' is missing required characters: {''.join(missing_cn)}")
        missing_ascii = sorted(list(wqy12_ascii - supported))
        if missing_ascii:
            raise ValueError(f"Font validation failed: 'u8g2_font_wqy12_t_lunar' is missing required ASCII characters: {''.join(missing_ascii)}")

    print("Font character validation passed! All required characters are supported by Chinese fonts.")


def main():
    ui_layout = default_ui_layout()
    resources = parse_fonts() + parse_lunar_resources() + parse_holiday_resources() + parse_lunar_db_resources() + [
        (RES_TYPE_UI, UI_IDS["ui_bundle"], pack_ui_bundle(ui_layout), "ui_bundle")
    ]
    if INCLUDE_EXTERNAL_LUT_RESOURCES:
        resources += parse_lut_resources()

    # Validate that the fonts contain all required Chinese and ASCII characters
    validate_font_characters(resources, ui_layout)

    if len(resources) > MAX_ENTRIES:
        raise ValueError(f"Too many resources: {len(resources)} > {MAX_ENTRIES}")

    data = bytearray()
    entries = []
    address = DATA_START
    for res_type, res_id, payload, name in resources:
        address = DATA_START + align4(len(data))
        padding = address - (DATA_START + len(data))
        if padding:
            data += b"\xFF" * padding
        if address + len(payload) - 1 > DATA_END:
            raise ValueError("External resource data exceeds FM25Q04B resource area")
        data += payload
        entries.append({
            "type": res_type,
            "id": res_id,
            "offset": address,
            "length": len(payload),
            "crc32": crc32(payload),
            "name": name,
        })

    manifest, manifest_crc = build_manifest(entries)
    html_js = ROOT / "html" / "js"
    html_js.mkdir(parents=True, exist_ok=True)

    bundle = f"""// Generated by tools/build_ext_resources.py. Do not edit by hand.
(function () {{
  window.EPD_RESOURCE_BUNDLE = {{
    version: {VERSION},
    magic: {MAGIC},
    manifestAddress: 0x{MAIN_MANIFEST:05X},
    dataStart: 0x{DATA_START:05X},
    dataEnd: 0x{DATA_END:05X},
    stagingManifestAddress: 0x{STAGING_MANIFEST:05X},
    scratchAddress: 0x{SCRATCH:05X},
    manifestSize: {len(manifest)},
    manifestCrc32: 0x{manifest_crc:08X},
    dataBase64: "{base64.b64encode(bytes(data)).decode("ascii")}",
    manifestBase64: "{base64.b64encode(manifest).decode("ascii")}",
    uiLayout: {json.dumps(ui_layout, ensure_ascii=False, separators=(",", ":"))},
    entries: [
{chr(10).join(f'      {{ type: {e["type"]}, id: {e["id"]}, offset: 0x{e["offset"]:05X}, length: {e["length"]}, crc32: 0x{e["crc32"]:08X}, name: "{e["name"]}" }},' for e in entries)}
    ]
  }};
}})();
"""
    (html_js / "resource_bundle.js").write_text(bundle, encoding="utf-8")

    # Write independent partition files and manifest to tools/dist
    dist_dir = ROOT / "tools" / "dist"
    dist_dir.mkdir(parents=True, exist_ok=True)

    # Save the manifest binary
    (dist_dir / "manifest.bin").write_bytes(manifest)

    # Save each independent resource payload
    for res_type, res_id, payload, name in resources:
        (dist_dir / f"{name}.bin").write_bytes(payload)

    # Save a JSON manifest file for incremental DFU tooling
    json_manifest = {
        "magic": "EPDR",
        "version": VERSION,
        "manifestCrc32": f"0x{manifest_crc:08X}",
        "entries": [
            {
                "type": e["type"],
                "id": e["id"],
                "name": e["name"],
                "offset": e["offset"],
                "length": e["length"],
                "crc32": f"0x{e['crc32']:08X}"
            }
            for e in entries
        ]
    }
    (dist_dir / "manifest.json").write_text(
        json.dumps(json_manifest, indent=2, ensure_ascii=False),
        encoding="utf-8"
    )

    image = bytearray([0xFF] * (SCRATCH - DATA_START))
    image[0:len(data)] = data
    main_manifest_offset = MAIN_MANIFEST - DATA_START
    image[main_manifest_offset:main_manifest_offset + len(manifest)] = manifest
    if STAGING_MANIFEST != MAIN_MANIFEST:
        staging_manifest_offset = STAGING_MANIFEST - DATA_START
        image[staging_manifest_offset:staging_manifest_offset + len(manifest)] = manifest
    (ROOT / "tools" / "epd_resources.bin").write_bytes(image)

    print(f"resources={len(entries)} data={len(data)} manifest_crc=0x{manifest_crc:08X}")


if __name__ == "__main__":
    main()
