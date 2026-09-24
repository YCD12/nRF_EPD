import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('dfu_release', ROOT / 'tools' / 'dfu_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def record(address, kind, data=b''):
    raw = bytes([len(data), address >> 8, address & 0xff, kind]) + data
    return ':' + (raw + bytes([(-sum(raw)) & 0xff])).hex().upper() + '\n'


class DfuReleaseTests(unittest.TestCase):
    def test_intel_hex_ranges_and_checksum(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'app.hex'
            path.write_text(record(0, 4, b'\x00\x01') + record(0x9000, 0, b'\x01\x02') +
                            record(0, 1), encoding='ascii')
            ranges = release.intel_hex_ranges(path)
            self.assertEqual(ranges, [(0x19000, 0x19002)])
            self.assertEqual(release.require_inside('app', ranges, 0x19000, 0x27000)['occupied_bytes'], 2)
            with self.assertRaises(release.ReleaseError):
                release.require_inside('app', ranges, 0x19001, 0x27000)

    def test_invalid_hex(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad.hex'
            path.write_text(':020000040001F8\n:02900000010200\n', encoding='ascii')
            with self.assertRaises(release.ReleaseError):
                release.intel_hex_ranges(path)
    def test_release_configuration_is_explicit_and_safe(self):
        config = ROOT / 'SDK/17.1.0_ddde560/dfu/sdk_config.h'
        self.assertEqual(release.config_value(config, 'NRF_DFU_REQUIRE_SIGNED_APP_UPDATE'), 1)
        self.assertEqual(release.config_value(config, 'NRF_DFU_APP_DOWNGRADE_PREVENTION'), 1)
        self.assertEqual(release.config_value(config, 'NRF_DFU_FORCE_DUAL_BANK_APP_UPDATES'), 0)
        self.assertEqual(release.config_value(config, 'NRF_DFU_APP_DATA_AREA_SIZE'), 8192)

    def test_generated_keil_bootloader_is_preferred(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = root / 'Keil/_build_DFU52/bl_nrf52811_xxaa_s112.hex'
            generated.parent.mkdir(parents=True)
            generated.write_text(':00000001FF\n', encoding='ascii')
            self.assertEqual(release.default_bootloader(root), generated)

    def test_release_wrappers_do_not_gate_on_dfu_versions(self):
        shell = (ROOT / 'tools/make-ota-nrf52.sh').read_text(encoding='utf-8')
        batch = (ROOT / 'tools/make-ota-nrf52.bat').read_text(encoding='utf-8')
        release_source = (ROOT / 'tools/dfu_release.py').read_text(encoding='utf-8')
        for text in (shell, batch, release_source):
            self.assertNotIn('DFU_APPLICATION_VERSION', text)
            self.assertNotIn('DFU_PREVIOUS_APPLICATION_VERSION', text)
            self.assertNotIn('previous-version', text)
            self.assertNotIn('previous_version', text)
        self.assertIn('DFU_RESOURCES_DIR', shell)
        self.assertIn('NRFUTIL', shell)
        self.assertNotIn('FW_VER=', shell)
        self.assertIn('DFU_RESOURCES_DIR', batch)
        self.assertNotIn('set "fw_ver=', batch.lower())

    def test_reserved_range_overlap_is_rejected(self):
        with self.assertRaises(release.ReleaseError):
            release.require_no_overlap('image', [(0x26FFF, 0x27001)], 0x27000, 0x29000)
        self.assertIsNone(release.require_no_overlap('image', [(0x19000, 0x27000)], 0x27000, 0x29000))

    def test_external_resource_bundle_requires_valid_single_copy_packages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, partition_id in {**release.RESOURCE_PACKAGES,
                                       **release.PERSONAL_RECORD_PACKAGES}.items():
                payload = bytes((partition_id, 0xA5))
                head = struct.pack('<6I', release.RESOURCE_MAGIC,
                                   release.RESOURCE_ABI << 16 | partition_id, 1, len(payload),
                                   zlib.crc32(payload), 524288)
                (root / name).write_bytes(head + struct.pack('<II', zlib.crc32(head),
                                      release.RESOURCE_COMMIT) + payload)
            bundle = release.external_resource_packages(root)
            self.assertTrue(bundle['bundled'])
            self.assertEqual(bundle['packages']['fonts.ers']['partition_id'], 7)
            (root / 'fonts.ers').write_bytes(b'broken')
            with self.assertRaises(release.ReleaseError):
                release.external_resource_packages(root)


if __name__ == '__main__':
    unittest.main()
