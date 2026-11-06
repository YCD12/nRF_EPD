"""Generate the external resource ABI from the canonical layout; no device writes."""
import argparse
import importlib.util
import json
import struct
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYOUT = ROOT / 'docs/external-flash-layout-v1.draft.json'
_font_spec = importlib.util.spec_from_file_location('font_store_tool', ROOT / 'tools/font_store_tool.py')
font_store_tool = importlib.util.module_from_spec(_font_spec)
_font_spec.loader.exec_module(font_store_tool)
MAGIC, COMMIT, ABI = 0x31535245, 0xA55AC33C, 1
STAMP_MAGIC = 0x54535245  # ERST, appended after the payload and not written to Flash
# Written into each ERS header revision and stored on the device. Bump a module
# here when its resource contents change.
MODULE_VERSIONS = {
    1: 2,  # partition table, adds TODO without moving existing resources
    2: 1,  # epd config
    3: 1,  # calendar annotations
    5: 1,  # calendar
    6: 2,  # UIR2 display styles and templates
    7: 1,  # fonts
    9: 1,  # todo items
}
RECORD_SCHEMA, RECORD_HEADER_SIZE = 1, 16
CONFIG_RECORD_MAGIC, ANNOTATION_RECORD_MAGIC = 0x31435045, 0x31434E41
TYPES = {'partition_table': 1, 'config_record': 2, 'annotation_record': 3,
         'update_status': 4, 'calendar_package': 5, 'ui_package': 6,
         'font_package': 7, 'reserved': 8, 'todo_items': 9}


def load_layout(path=LAYOUT):
    layout = json.loads(Path(path).read_text(encoding='utf-8'))
    if (layout['capacity_bytes'] != 524288 or layout['erase_sector_bytes'] != 4096
            or layout['layout_version'] != ABI or layout['external_data_copies'] != 1
            or layout['external_firmware_slots']):
        raise ValueError('unsupported geometry, ABI or redundancy')
    parts, seen, end = layout['partitions'], set(), 0
    if not 1 <= len(parts) <= 32:
        raise ValueError('partition count must be 1..32')
    for p in parts:
        offset, size = int(p['offset'], 0), p['size_bytes']
        if (not 0 < p['id'] < 65536 or p['id'] in seen or offset != end
                or offset % 4096 or size <= 32 or size % 4096
                or offset + size > layout['capacity_bytes']):
            raise ValueError('invalid ID, alignment, range, overlap or gap')
        seen.add(p['id'])
        end = offset + size
        if p['type'] not in TYPES:
            raise ValueError('unknown partition type')
    if (parts[0]['id'], parts[0]['type'], parts[0]['size_bytes']) != (1, 'partition_table', 4096):
        raise ValueError('missing fixed table locator')
    if end != layout['capacity_bytes']:
        raise ValueError('layout must account for whole chip')
    return layout


def table_payload(layout):
    return b''.join(struct.pack('<HHIIHH', p['id'], TYPES[p['type']],
                     int(p['offset'], 0), p['size_bytes'], ABI, 0)
                    for p in layout['partitions'])


def package(part_id, payload, revision=1, built_at=None):
    if not payload or not 0 < part_id < 65536:
        raise ValueError('empty package or invalid ID')
    if built_at is None:
        built_at = int(time.time())
    header = struct.pack('<6I', MAGIC, ABI << 16 | part_id, revision,
                         len(payload), zlib.crc32(payload), 524288)
    image = header + struct.pack('<II', zlib.crc32(header), COMMIT) + payload
    stamp = struct.pack('<II', STAMP_MAGIC, built_at & 0xFFFFFFFF)
    return image + stamp + struct.pack('<I', zlib.crc32(stamp))


def record_payload(record_magic, data, tombstone=False):
    data = bytes(data)
    if len(data) > 0xffff or (tombstone and data):
        raise ValueError('record data is too large or tombstone is not empty')
    return struct.pack('<I4HI', record_magic, RECORD_SCHEMA, RECORD_HEADER_SIZE,
                       len(data), 1 if tombstone else 0, zlib.crc32(data)) + data


def validate_annotations(data):
    data = bytes(data)
    if len(data) < 16 or len(data) > 1040:
        raise ValueError('annotation blob length is invalid')
    magic, version, record_size, count = struct.unpack_from('<IBBH', data)
    if (magic, version, record_size) != (0x4F4E4E41, 1, 16) or count > 64 or \
            len(data) != 16 + count * 16 or any(data[8:16]):
        raise ValueError('annotation header is invalid')
    previous = -1
    for i in range(count):
        row = data[16 + i * 16:32 + i * 16]
        year, month, day, flags, label_len = struct.unpack_from('<H4B', row)
        key = year << 16 | month << 8 | day
        if (not flags & 1 or flags & 0xC0 or ((flags >> 1) & 3) > 2 or
                ((flags >> 3) & 3) > 2 or not 1 <= month <= 12 or
                not 1 <= day <= 31 or label_len > 9 or row[15] or key <= previous):
            raise ValueError(f'annotation record {i} is invalid or unsorted')
        previous = key
    return data


def default_record_data():
    config = bytes((0x14, 0x13, 0x06, 0x05, 0x04, 0x03, 0x02,
                    0x07, 0xFF, 0x12, 0x07, 0x02, 0x00, 0x00))
    annotations = struct.pack('<IBBH8x', 0x4F4E4E41, 1, 16, 0)
    return config, annotations


def todo_payload():
    """Empty TODO v1 package; five visible items and a 12-pixel font."""
    return struct.pack('<IBBHBB6x', 0x4F444F54, 1, 80, 0, 5, 12)


def migration_bundle(path):
    bundle = json.loads(Path(path).read_text(encoding='utf-8'))
    if bundle.get('format') != 'EPD-MIG1':
        raise ValueError('migration bundle format is not EPD-MIG1')
    try:
        config = bytes.fromhex(bundle['epd_config_hex'])
        annotations = bytes.fromhex(bundle['calendar_annotations_hex'])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError('migration bundle has invalid hex data') from error
    if len(config) != 14:
        raise ValueError('migration config must be 14 bytes')
    return config, validate_annotations(annotations)


def font_payload(input_dir):
    payloads, records = font_store_tool.load_all(Path(input_dir), False)
    directory_size = 8 + len(records) * 16
    offset = directory_size
    directory = bytearray(struct.pack('<IHH', 0x314B5046, 1, len(records)))
    body = bytearray()
    manifest_records = []
    for data, record in zip(payloads, records):
        if data is None:
            directory += struct.pack('<4I', 0, 0, 0, 0)
            manifest_records.append({**record, 'offset': 0})
            continue
        directory += struct.pack('<4I', offset, len(data), record['crc32'], 0)
        manifest_records.append({**record, 'offset': offset})
        body += data
        offset += len(data)
    return bytes(directory + body), manifest_records


def calendar_payload(input_dir, calendar_data=None):
    input_dir = Path(input_dir)
    month_days = struct.pack('<5I', 2024, 0x0000096C, 0x0000D4AE, 0x0000149C, 0x00001A4C)
    solar_new_year = struct.pack('<5I', 2024, 0x000FD04A, 0x000FD23D, 0x000FD451, 0x000FD646)
    jieqi_bits = bytes((0x0F, 0xEF, 0xDB, 0xBE, 0xA6, 0x99,
                        0x9C, 0xA2, 0x98, 0x80, 0x00, 0x18))
    holidays = struct.pack('<39H',
        0x0101, 0x0102, 0x0103, 0x1104, 0x120E, 0x020F, 0x0210, 0x0211,
        0x0212, 0x0213, 0x0214, 0x0215, 0x0216, 0x0217, 0x121C, 0x0404,
        0x0405, 0x0406, 0x0501, 0x0502, 0x0503, 0x0504, 0x0505, 0x1509,
        0x0613, 0x0614, 0x0615, 0x0919, 0x091A, 0x091B, 0x1914, 0x0A01,
        0x0A02, 0x0A03, 0x0A04, 0x0A05, 0x0A06, 0x0A07, 0x1A0A)
    year_min, year_max, holiday_year = 2025, 2027, 2026
    if calendar_data is not None:
        data = json.loads(Path(calendar_data).read_text(encoding='utf-8'))
        year_min = data.get('year_min', year_min)
        year_max = data.get('year_max', year_max)
        holiday_year = data.get('holiday_year', holiday_year)
        if not (type(year_min) is int and type(year_max) is int and type(holiday_year) is int and
                1584 <= year_min <= year_max <= 4095 and 1583 <= holiday_year <= 4095):
            raise ValueError('calendar years are outside the CAL1 range')
        if (year_min, year_max) != (2025, 2027) and not all(
                key in data for key in ('lunar_month_days', 'solar_new_year', 'jieqi_year_bits')):
            raise ValueError('a different year range requires all three calculation tables')
        if holiday_year != 2026 and 'holidays' not in data:
            raise ValueError('a different holiday year requires its holiday table')
        def table(key, fmt, fallback):
            if key not in data:
                return fallback
            values = data[key]
            if not isinstance(values, list) or any(type(v) is not int for v in values):
                raise ValueError(f'{key}: expected an array of integers')
            try:
                return struct.pack(f'<{len(values)}{fmt}', *values)
            except struct.error as error:
                raise ValueError(f'{key}: integer out of range') from error
        month_days = table('lunar_month_days', 'I', month_days)
        solar_new_year = table('solar_new_year', 'I', solar_new_year)
        jieqi_bits = table('jieqi_year_bits', 'B', jieqi_bits)
        holidays = table('holidays', 'H', holidays)
    years = year_max - year_min + 1
    if (len(month_days) != (years + 2) * 4 or len(solar_new_year) != (years + 2) * 4 or
            len(jieqi_bits) != (years + 1) * 3 or
            struct.unpack_from('<I', month_days)[0] != year_min - 1 or
            struct.unpack_from('<I', solar_new_year)[0] != year_min - 1):
        raise ValueError('calendar table lengths/base years do not match the year range')
    special_names = b''.join(name.encode('utf-8').ljust(10, b'\0') for name in
                             ('除夕', '母亲节', '父亲节', '感恩节', '清明节'))
    specs = [
        ('month_names', 7, 13, 'lunar_month_names.bin'),
        ('leap_names', 4, 2, 'lunar_leap_names.bin'),
        ('date_names', 7, 31, 'lunar_date_names.bin'),
        ('weekday_names', 4, 7, 'lunar_day_names.bin'),
        ('zodiac_names', 4, 12, 'lunar_zodiac_names.bin'),
        ('stem_names', 4, 10, 'lunar_stem_names.bin'),
        ('branch_names', 4, 12, 'lunar_branch_names.bin'),
        ('jieqi_names', 7, 24, 'lunar_jieqi_names.bin'),
        ('lunar_month_days', 4, years + 2, month_days),
        ('solar_new_year', 4, years + 2, solar_new_year),
        ('jieqi_year_bits', 1, (years + 1) * 3, jieqi_bits),
        ('jieqi_base_days', 1, 24, 'lunar_jieqi_base_days.bin'),
        ('solar_festivals', 12, None, 'solar_festivals.bin'),
        ('lunar_festivals', 12, None, 'lunar_festivals.bin'),
        ('holidays', 2, None, holidays),
        ('special_festival_names', 10, 5, special_names),
    ]
    header_size = 16 + len(specs) * 16
    header = bytearray(struct.pack('<I6H', 0x314C4143, 1, len(specs), year_min, year_max, holiday_year, 0))
    body = bytearray()
    records = []
    for section, (name, item_size, count, source) in enumerate(specs, 1):
        data = (input_dir / source).read_bytes() if isinstance(source, str) else source
        if count is None:
            count = len(data) // item_size
        if count > 65535:
            raise ValueError(f'{name}: too many records')
        if len(data) != item_size * count:
            raise ValueError(f'{name}: {len(data)} bytes != {item_size} * {count}')
        offset = header_size + len(body)
        header += struct.pack('<4H2I', section, item_size, count, 0, offset, len(data))
        body += data
        records.append({'id': section, 'name': name, 'item_size': item_size,
                        'count': count, 'offset': offset, 'length': len(data),
                        'crc32': f'0x{zlib.crc32(data):08X}'})
    return bytes(header + body), records


def ui_payload(source=None):
    import importlib.util
    spec = importlib.util.spec_from_file_location('ui_resource_codec', ROOT / 'tools/ui_resource_codec.py')
    codec = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(codec)
    default_ui, encode_ui = codec.default_ui, codec.encode_ui
    model = json.loads(Path(source).read_text(encoding="utf-8")) if source else default_ui()
    return encode_ui(model), model['components']


def generate(layout, check=False):
    lines = ['/* Generated by tools/resource_pack.py; edit the layout JSON. */',
             '#ifndef RESOURCE_LAYOUT_H', '#define RESOURCE_LAYOUT_H',
             '#define RS_CAPACITY 524288UL', '#define RS_SECTOR_SIZE 4096UL']
    for p in layout['partitions']:
        name = p['name'].upper()
        lines += [f'#define RS_PART_{name} {p["id"]}U',
                  f'#define RS_OFFSET_{name} {p["offset"]}UL',
                  f'#define RS_SIZE_{name} {p["size_bytes"]}UL']
    lines += ['#endif', '']
    header = '\n'.join(lines)
    path = ROOT / 'EPD/storage/resource_layout.h'
    if check:
        if not path.exists() or path.read_text(encoding='utf-8') != header:
            raise ValueError(f'stale generated file: {path}')
    else:
        with path.open('w', encoding='utf-8', newline='\n') as stream:
            stream.write(header)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--layout', type=Path, default=LAYOUT)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--output', type=Path, help='write table payload and committed package')
    parser.add_argument('--resource-id', type=int, help='wrap an opaque resource payload (P1 storage tests)')
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--revision', type=int, default=1)
    parser.add_argument('--fonts', action='store_true', help='build validated font partition package')
    parser.add_argument('--font-dir', type=Path, default=ROOT / 'tools/dist')
    parser.add_argument('--calendar', action='store_true', help='build calendar resource package')
    parser.add_argument('--calendar-dir', type=Path, default=ROOT / 'tools/dist')
    parser.add_argument('--calendar-data', type=Path, help='JSON overrides for calendar years and calculation/holiday tables')
    parser.add_argument('--ui-layout', type=Path, help='UIR2 source JSON exported by the web editor')
    parser.add_argument('--ui', action='store_true', help='build fixed-component UI layout package')
    parser.add_argument('--todo', action='store_true', help='build empty TODO items with factory display settings')
    parser.add_argument('--records', action='store_true', help='build default config and empty annotation records')
    parser.add_argument('--config-record', type=Path, help='14-byte legacy epd_config_t export')
    parser.add_argument('--annotations-record', type=Path, help='legacy ANNO blob export')
    parser.add_argument('--migration-bundle', type=Path, help='EPD-MIG1 JSON exported by the legacy web UI')
    args = parser.parse_args()
    built_at = int(time.time())
    layout = load_layout(args.layout)
    generate(layout, args.check)
    if args.payload or args.resource_id:
        if not args.payload or not args.resource_id or not args.output:
            parser.error('--payload, --resource-id and --output are required together')
        part = next((p for p in layout['partitions'] if p['id'] == args.resource_id), None)
        if part is None or part['type'] in ('partition_table', 'reserved', 'update_status'):
            parser.error('not a writable resource partition')
        payload = args.payload.read_bytes()
        if not payload or len(payload) > part['size_bytes'] - 32 or not 0 <= args.revision <= 0xffffffff:
            parser.error('payload or revision is out of range')
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / f'resource-{args.resource_id}.ers').write_bytes(
            package(args.resource_id, payload, args.revision, built_at))
    if args.fonts:
        if not args.output:
            parser.error('--fonts requires --output')
        payload, records = font_payload(args.font_dir)
        fonts = next(p for p in layout['partitions'] if p['id'] == 7)
        if len(payload) > fonts['size_bytes'] - 32:
            parser.error('font package exceeds its partition')
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'fonts.ers').write_bytes(package(7, payload, MODULE_VERSIONS[7], built_at))
        (args.output / 'fonts.json').write_text(json.dumps({
            'format': 'FPK1', 'revision': args.revision,
            'payload_bytes': len(payload), 'slots': records,
        }, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.calendar:
        if not args.output:
            parser.error('--calendar requires --output')
        try:
            payload, records = calendar_payload(args.calendar_dir, args.calendar_data)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            parser.error(f'invalid calendar data: {error}')
        year_min, year_max, holiday_year = struct.unpack_from('<3H', payload, 8)
        calendar = next(p for p in layout['partitions'] if p['id'] == 5)
        if len(payload) > calendar['size_bytes'] - 32:
            parser.error('calendar package exceeds its partition')
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'calendar.ers').write_bytes(package(5, payload, MODULE_VERSIONS[5], built_at))
        (args.output / 'calendar.json').write_text(json.dumps({
            'format': 'CAL1', 'revision': args.revision,
            'year_min': year_min, 'year_max': year_max, 'holiday_year': holiday_year,
            'payload_bytes': len(payload), 'sections': records,
        }, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.ui:
        if not args.output:
            parser.error('--ui requires --output')
        payload, records = ui_payload(args.ui_layout)
        ui = next(p for p in layout['partitions'] if p['id'] == 6)
        if len(payload) > ui['size_bytes'] - 32:
            parser.error('UI package exceeds its partition')
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'ui.ers').write_bytes(package(6, payload, MODULE_VERSIONS[6], built_at))
        (args.output / 'ui.json').write_text(json.dumps({
            'format': 'UIR2', 'revision': MODULE_VERSIONS[6],
            'screen_width': 800, 'screen_height': 480, 'version': 2,
            'payload_bytes': len(payload), 'components': records,
        }, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.todo:
        if not args.output:
            parser.error('--todo requires --output')
        payload = todo_payload()
        todo = next((p for p in layout['partitions'] if p['id'] == 9), None)
        if todo is None or todo['type'] != 'todo_items' or len(payload) > todo['size_bytes'] - 32:
            parser.error('missing or incompatible TODO partition')
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'todo.ers').write_bytes(package(9, payload, MODULE_VERSIONS[9], built_at))
        (args.output / 'todo.json').write_text(json.dumps({
            'format': 'TODO', 'revision': MODULE_VERSIONS[9],
            'payload_bytes': len(payload), 'record_size': 80,
            'count': 0, 'display_limit': 5, 'font_size': 12,
        }, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.records or args.config_record or args.annotations_record or args.migration_bundle:
        if not args.output:
            parser.error('record generation requires --output')
        if args.migration_bundle and (args.config_record or args.annotations_record):
            parser.error('--migration-bundle cannot be combined with individual record files')
        default_config, default_annotations = default_record_data()
        if args.migration_bundle:
            try:
                config, annotations = migration_bundle(args.migration_bundle)
            except ValueError as error:
                parser.error(str(error))
        else:
            config = args.config_record.read_bytes() if args.config_record else default_config
            annotations = args.annotations_record.read_bytes() if args.annotations_record else default_annotations
        if len(config) != 14:
            parser.error('config record must be the 14-byte epd_config_t image')
        try:
            annotations = validate_annotations(annotations)
        except ValueError as error:
            parser.error(str(error))
        args.output.mkdir(parents=True, exist_ok=True)
        config_payload = record_payload(CONFIG_RECORD_MAGIC, config)
        annotation_payload = record_payload(ANNOTATION_RECORD_MAGIC, annotations)
        (args.output / 'epd-config.ers').write_bytes(package(2, config_payload, MODULE_VERSIONS[2], built_at))
        (args.output / 'calendar-annotations.ers').write_bytes(
            package(3, annotation_payload, MODULE_VERSIONS[3], built_at))
        (args.output / 'records.json').write_text(json.dumps({
            'format': 'REC1', 'revision': args.revision,
            'epd_config': {'bytes': len(config), 'crc32': f'0x{zlib.crc32(config):08X}'},
            'calendar_annotations': {'bytes': len(annotations),
                                     'records': struct.unpack_from('<H', annotations, 6)[0],
                                     'crc32': f'0x{zlib.crc32(annotations):08X}'},
        }, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        payload = table_payload(layout)
        (args.output / 'partition-table.payload.bin').write_bytes(payload)
        (args.output / 'partition-table.ers').write_bytes(package(1, payload, MODULE_VERSIONS[1], built_at))
    print('Resource layout OK: single copy, 512 KiB, sector aligned')
    if args.output:
        print(f'Build timestamp: {time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(built_at))}')


if __name__ == '__main__':
    main()
