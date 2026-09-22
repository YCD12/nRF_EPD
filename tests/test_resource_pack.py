import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('resource_pack', ROOT / 'tools/resource_pack.py')
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


class ResourcePackTests(unittest.TestCase):
    def test_canonical_layout_and_generated_files(self):
        layout = pack.load_layout()
        pack.generate(layout, check=True)
        self.assertEqual(sum(p['size_bytes'] for p in layout['partitions']), 524288)
        self.assertEqual(len(pack.table_payload(layout)), 128)

    def test_wire_header(self):
        payload = pack.table_payload(pack.load_layout())
        image = pack.package(1, payload, 19)
        words = struct.unpack('<8I', image[:32])
        self.assertEqual(words[:4], (pack.MAGIC, 0x10001, 19, 128))
        self.assertEqual(words[4], zlib.crc32(payload))
        self.assertEqual(words[6], zlib.crc32(image[:24]))
        self.assertEqual(words[7], pack.COMMIT)

    def test_font_payload_uses_validated_three_slot_directory(self):
        payload, records = pack.font_payload(ROOT / 'tools/dist')
        magic, version, count = struct.unpack_from('<IHH', payload)
        self.assertEqual((magic, version, count), (0x314B5046, 1, 3))
        previous_end = 56
        for slot, record in enumerate(records):
            offset, length, checksum, flags = struct.unpack_from('<4I', payload, 8 + slot * 16)
            self.assertEqual(offset, previous_end)
            self.assertEqual((length, checksum, flags),
                             (record['length'], record['crc32'], 0))
            self.assertEqual(record['offset'], offset)
            self.assertEqual(zlib.crc32(payload[offset:offset + length]), checksum)
            previous_end += length
        self.assertEqual(previous_end, len(payload))

    def test_calendar_payload_matches_current_source_window(self):
        payload, records = pack.calendar_payload(ROOT / 'tools/dist')
        self.assertEqual(struct.unpack_from('<I6H', payload),
                         (0x314C4143, 1, 16, 2025, 2027, 2026, 0))
        self.assertEqual(len(records), 16)
        by_name = {r['name']: r for r in records}
        self.assertEqual(by_name['lunar_month_days']['length'], 20)
        self.assertEqual(by_name['solar_new_year']['length'], 20)
        self.assertEqual(by_name['jieqi_year_bits']['length'], 12)
        self.assertEqual(by_name['holidays']['length'], 78)
        self.assertEqual(by_name['special_festival_names']['length'], 50)
        for r in records:
            self.assertLessEqual(r['offset'] + r['length'], len(payload))

    def test_calendar_payload_accepts_annual_data_override(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'calendar.json'
            path.write_text(json.dumps({
                'holiday_year': 2027,
                'holidays': [0x0101, 0x1102],
            }), encoding='utf-8')
            payload, records = pack.calendar_payload(ROOT / 'tools/dist', path)
        self.assertEqual(struct.unpack_from('<H', payload, 12)[0], 2027)
        holidays = next(record for record in records if record['name'] == 'holidays')
        self.assertEqual((holidays['count'], holidays['length']), (2, 4))

    def test_calendar_year_range_requires_matching_calculation_tables(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'calendar.json'
            path.write_text(json.dumps({'year_min': 2026, 'year_max': 2028}), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'calculation tables'):
                pack.calendar_payload(ROOT / 'tools/dist', path)

    def test_ui_payload_is_bounded_fixed_component_schema(self):
        payload, records = pack.ui_payload()
        self.assertEqual(struct.unpack_from('<I6H', payload),
                         (0x31524955, 1, 19, 800, 480, 20, 0))
        self.assertEqual(len(payload), 16 + 19 * 20)
        self.assertEqual([r['id'] for r in records], list(range(1, 20)))
        for i, record in enumerate(records):
            raw = struct.unpack_from('<8B6h', payload, 16 + i * 20)
            self.assertEqual(raw[1], record['id'])
            self.assertGreater(raw[10], 0)
            self.assertGreater(raw[11], 0)

    def test_single_record_payload_and_legacy_validation(self):
        config, annotations = pack.default_record_data()
        self.assertEqual(len(config), 14)
        self.assertEqual(pack.validate_annotations(annotations), annotations)
        payload = pack.record_payload(pack.CONFIG_RECORD_MAGIC, config)
        self.assertEqual(struct.unpack_from('<I4HI', payload),
                         (pack.CONFIG_RECORD_MAGIC, 1, 16, 14, 0, zlib.crc32(config)))
        self.assertEqual(payload[16:], config)
        with self.assertRaises(ValueError):
            pack.validate_annotations(annotations + b'\0')
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'migration.json'
            path.write_text(json.dumps({'format': 'EPD-MIG1',
                                        'epd_config_hex': config.hex(),
                                        'calendar_annotations_hex': annotations.hex()}),
                            encoding='utf-8')
            self.assertEqual(pack.migration_bundle(path), (config, annotations))

    def test_reject_bad_layout_before_generating(self):
        good = pack.load_layout()
        for field, value in [('offset', '0x0'), ('size_bytes', 4097), ('id', 1), ('size_bytes', 524288)]:
            with self.subTest(field=field, value=value), tempfile.TemporaryDirectory() as temp:
                bad = copy.deepcopy(good)
                bad['partitions'][1][field] = value
                path = Path(temp) / 'bad.json'
                path.write_text(json.dumps(bad), encoding='utf-8')
                with self.assertRaises(ValueError):
                    pack.load_layout(path)


if __name__ == '__main__':
    unittest.main()
