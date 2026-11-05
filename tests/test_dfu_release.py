import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('dfu_release', ROOT / 'tools' / 'dfu_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
DEFAULT_APPLICATION_VERSION = release.default_application_version()
VERSIONED_STEM = f'test-v{DEFAULT_APPLICATION_VERSION:#x}'
OTA_ZIP = f'{VERSIONED_STEM}-ota.zip'
SETTINGS_HEX = f'{VERSIONED_STEM}-settings.hex'
FULL_HEX = f'{VERSIONED_STEM}-full.hex'
RELEASE_JSON = f'{VERSIONED_STEM}-release.json'


def record(address, kind, data=b''):
    raw = bytes([len(data), address >> 8, address & 0xff, kind]) + data
    return ':' + (raw + bytes([(-sum(raw)) & 0xff])).hex().upper() + '\n'


def write_hex(path, data, start=0x29000, uicr=True, vectors=True):
    if vectors:
        data = struct.pack('<II', 0x20006000, (start + 8) | 1) + data
    text = record(0, 4, (start >> 16).to_bytes(2, 'big'))
    for offset in range(0, len(data), 16):
        text += record((start + offset) & 0xffff, 0, data[offset:offset + 16])
    if uicr:
        text += record(0, 4, b'\x10\x00')
        text += record(0x1014, 0, struct.pack('<II', 0x29000, 0x2E000))
    path.write_text(text + record(0, 1), encoding='ascii')


def descriptor(reserved=8192, flags=3):
    return struct.pack('<8I', release.BOOTLOADER_LAYOUT_MAGIC, 1, 0x19000,
                       0x29000, reserved, 0x2E000, 0x2F000, flags)


def build_fixture(root, application_version=None):
    """Mock external tools while exercising the real HEX/ZIP/manifest audits."""
    if application_version is None:
        application_version = release.default_application_version()
    app, boot, sd, key = (root / name for name in ('app.hex', 'boot.hex', 'sd.hex', 'key.pem'))
    write_hex(app, b'abcd', start=release.APP_START, uicr=False)
    public_key = bytes(range(64))
    write_hex(boot, descriptor() + public_key)
    sd_data = bytearray(0x3018)
    struct.pack_into('<I', sd_data, 0x3008, release.APP_START)
    struct.pack_into('<H', sd_data, 0x300C, release.SOFTDEVICE_ID)
    struct.pack_into('<II', sd_data, 0x3010, 112, 7003000)
    write_hex(sd, sd_data, start=0, uicr=False, vectors=False)
    key.write_text('mock private key', encoding='ascii')
    args = argparse.Namespace(application=app, bootloader=boot, softdevice=sd, key=key,
                              bootloader_config=ROOT / 'SDK/17.1.0_ddde560/dfu/sdk_config.h',
                              output=root / 'release', name='test', resources_dir=None,
                              nrfutil='mock-nrfutil', mergehex='mock-mergehex',
                              application_version=application_version)
    state = {'failure': None, 'commands': []}

    def tool(command):
        state['commands'].append(command)
        if command[1:3] == ['keys', 'display']:
            return ', '.join(f'0x{value:02x}' for value in public_key)
        if command[1:3] == ['pkg', 'display']:
            phase = 'package_display'
            version = release.resolve_application_version(args)
            if state['failure'] == 'version_audit':
                version += 1
            text = (f'fw_version: 0x{version:X}\nhw_version 0x34\n'
                    f'app_size: {len(release.application_binary(app))}\n'
                    'signature_type: ECDSA_P256_SHA256\nsd_req: 0x126\n')
            if state['failure'] == 'metadata_audit':
                text = text.replace('0x34', '0x33')
        else:
            target = Path(command[-1])
            # Generator outputs must be isolated underneath the release directory.
            assert target.parent.parent == args.output.resolve()
            assert target.parent != args.output.resolve()
            if command[1:3] == ['pkg', 'generate']:
                phase = 'package_generate'
                binary = release.application_binary(app)
                if state['failure'] == 'package_bytes_audit':
                    binary = binary[:-1] + bytes([binary[-1] ^ 1])
                manifest = {'manifest': {'application': {'bin_file': 'app.bin', 'dat_file': 'app.dat'}}}
                with zipfile.ZipFile(target, 'w') as archive:
                    archive.writestr('manifest.json', json.dumps(manifest))
                    archive.writestr('app.bin', binary)
                    archive.writestr('app.dat', b'' if state['failure'] == 'package_audit' else b'init')
            elif command[1:3] == ['settings', 'generate']:
                phase = 'settings_generate'
                checksum = struct.pack('<I', zlib.crc32(app.read_bytes()))
                text = record(0, 4, b'\x00\x02') + record(0xE000, 0, checksum)
                if state['failure'] != 'settings_audit':
                    text += record(0xF000, 0, checksum)
                target.write_text(text + record(0, 1), encoding='ascii')
            else:
                assert command[0] == args.mergehex and command[1] == '-m'
                phase = 'merge'
                paths = command[2:command.index('-o')]
                if state['failure'] == 'full_missing_app_audit':
                    paths = [path for path in paths if Path(path) != app]
                text = ''.join(Path(path).read_text(encoding='ascii').replace(record(0, 1), '')
                               for path in paths)
                if state['failure'] == 'full_audit':
                    text += record(0, 4, b'\x00\x02') + record(0x8000, 0, b'reserved')
                if state['failure'] == 'full_extra_audit':
                    text += record(0, 4, b'\x10\x00') + record(0x1080, 0, b'unexpected')
                if state['failure'] == 'full_bytes_audit':
                    changed = release.image_memory(app)
                    changed[release.APP_START + 8] ^= 1
                    app_text = app.read_text(encoding='ascii').replace(record(0, 1), '')
                    replacement = record(0, 4, (release.APP_START >> 16).to_bytes(2, 'big'))
                    replacement += record(release.APP_START & 0xFFFF, 0,
                                          bytes(changed[address] for address in sorted(changed)))
                    text = text.replace(app_text, replacement)
                target.write_text(text + record(0, 1), encoding='ascii')
            text = ''
        if state['failure'] == phase:
            # Real tools can leave a partially or completely written output on failure.
            raise subprocess.CalledProcessError(2, command, stderr='injected tool failure')
        return text

    return args, tool, state


class DfuReleaseTests(unittest.TestCase):
    def existing_release_fixture(self, root):
        args, tool, state = build_fixture(root)
        with mock.patch.object(release, 'run', side_effect=tool):
            release.build_release(args)
        previous = {path.name: path.read_bytes() for path in args.output.iterdir()}
        self.assertEqual(len(previous), 4)
        # The new application changes all image hashes, so overwriting an old artifact is detectable.
        write_hex(args.application, b'WXYZ', start=release.APP_START, uicr=False)
        state['commands'].clear()
        return args, tool, state, previous

    def assert_previous_release(self, output, previous):
        self.assertEqual({path.name: path.read_bytes() for path in output.iterdir()}, previous)

    def test_release_generation_failures_keep_all_previous_artifacts(self):
        for failure in ('package_generate', 'settings_generate', 'merge', 'package_display'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                args, tool, state, previous = self.existing_release_fixture(Path(directory))
                state['failure'] = failure
                with mock.patch.object(release, 'run', side_effect=tool):
                    with self.assertRaises(subprocess.CalledProcessError):
                        release.build_release(args)
                self.assert_previous_release(args.output, previous)

    def test_release_audit_failures_keep_all_previous_artifacts(self):
        for failure in ('settings_audit', 'full_audit', 'package_audit', 'metadata_audit',
                        'version_audit', 'package_bytes_audit', 'full_missing_app_audit',
                        'full_extra_audit', 'full_bytes_audit'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                args, tool, state, previous = self.existing_release_fixture(Path(directory))
                state['failure'] = failure
                with mock.patch.object(release, 'run', side_effect=tool):
                    with self.assertRaises(release.ReleaseError):
                        release.build_release(args)
                self.assert_previous_release(args.output, previous)

    def test_sparse_unaligned_application_matches_word_padded_dfu_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            args, tool, state = build_fixture(Path(directory))
            write_hex(args.application, b'abcde', start=release.APP_START, uicr=False)
            text = args.application.read_text(encoding='ascii').replace(record(0, 1), '')
            args.application.write_text(text + record(0x9020, 0, b'x') + record(0, 1), encoding='ascii')
            expected = struct.pack('<II', 0x20006000, release.APP_START + 9)
            expected += b'abcde' + b'\xff' * 19 + b'x' + b'\xff' * 3
            self.assertEqual(release.application_binary(args.application), expected)
            with mock.patch.object(release, 'run', side_effect=tool):
                result = release.build_release(args)
            self.assertEqual(result['package']['size'], 36)
            self.assertEqual(result['application']['occupied_bytes'], 14)

    def test_invalid_zip_is_reported_as_release_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad.zip'
            path.write_bytes(b'not a ZIP')
            with self.assertRaisesRegex(release.ReleaseError, 'manifest'):
                release.validate_package_layout(path)

    def test_successful_release_replaces_audited_artifacts_and_manifest_last(self):
        with tempfile.TemporaryDirectory() as directory:
            args, tool, state, previous = self.existing_release_fixture(Path(directory))
            published = []
            original_replace = Path.replace

            def replace(path, destination):
                published.append(Path(destination).name)
                self.assertEqual((args.output / RELEASE_JSON).read_bytes(),
                                 previous[RELEASE_JSON])
                if path.name == RELEASE_JSON:
                    manifest = json.loads(path.read_text(encoding='utf-8'))
                    for section in ('package', 'settings', 'full_hex'):
                        artifact = manifest[section]
                        self.assertEqual(release.sha256(args.output / artifact['path']), artifact['sha256'])
                return original_replace(path, destination)

            with mock.patch.object(release, 'run', side_effect=tool), \
                    mock.patch.object(Path, 'replace', autospec=True, side_effect=replace):
                result = release.build_release(args)
            self.assertEqual(published, [OTA_ZIP, SETTINGS_HEX, FULL_HEX, RELEASE_JSON])
            self.assertEqual(json.loads((args.output / RELEASE_JSON).read_text(encoding='utf-8')), result)
            self.assertEqual({path.name for path in args.output.iterdir()}, set(previous))
            for name, contents in previous.items():
                self.assertNotEqual((args.output / name).read_bytes(), contents)

    def test_publication_io_failure_restores_previous_release(self):
        for failed_name in (OTA_ZIP, SETTINGS_HEX, FULL_HEX, RELEASE_JSON):
            with self.subTest(failed_name=failed_name), tempfile.TemporaryDirectory() as directory:
                args, tool, state, previous = self.existing_release_fixture(Path(directory))
                original_replace = Path.replace

                def replace(path, destination):
                    if path.name == failed_name:
                        raise OSError('injected publication failure')
                    return original_replace(path, destination)

                with mock.patch.object(release, 'run', side_effect=tool), \
                        mock.patch.object(Path, 'replace', autospec=True, side_effect=replace):
                    with self.assertRaisesRegex(OSError, 'injected publication failure'):
                        release.build_release(args)
                self.assert_previous_release(args.output, previous)

    def test_failed_first_publication_removes_new_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            args, tool, state = build_fixture(Path(directory))
            original_replace = Path.replace

            def replace(path, destination):
                if path.name == FULL_HEX:
                    raise OSError('injected publication failure')
                return original_replace(path, destination)

            with mock.patch.object(release, 'run', side_effect=tool), \
                    mock.patch.object(Path, 'replace', autospec=True, side_effect=replace):
                with self.assertRaisesRegex(OSError, 'injected publication failure'):
                    release.build_release(args)
            self.assertEqual(list(args.output.iterdir()), [])

    def test_failed_rollback_retains_the_old_artifact_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            args, tool, state, previous = self.existing_release_fixture(Path(directory))
            original_replace = Path.replace

            def replace(path, destination):
                if path.name == RELEASE_JSON or path.name == '1':
                    raise OSError('injected publication and settings restore failure')
                return original_replace(path, destination)

            with mock.patch.object(release, 'run', side_effect=tool), \
                    mock.patch.object(Path, 'replace', autospec=True, side_effect=replace):
                with self.assertRaisesRegex(release.ReleaseError, 'retained backups:'):
                    release.build_release(args)
            backups = list(args.output.glob('.dfu-release-backup-*'))
            self.assertEqual(len(backups), 1)
            self.assertEqual((backups[0] / '1').read_bytes(), previous[SETTINGS_HEX])
            self.assertEqual((args.output / RELEASE_JSON).read_bytes(), previous[RELEASE_JSON])

    def test_release_name_cannot_escape_staging(self):
        for name in ('', '.', '..', '../test', '..\\test', '/outside', 'C:test'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                args, tool, state, previous = self.existing_release_fixture(Path(directory))
                args.name = name
                with mock.patch.object(release, 'run', side_effect=tool):
                    with self.assertRaisesRegex(release.ReleaseError, 'filename'):
                        release.build_release(args)
                self.assertFalse(any(command[1:3] == ['pkg', 'generate'] for command in state['commands']))
                self.assert_previous_release(args.output, previous)

    def test_intel_hex_ranges_and_checksum(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'app.hex'
            path.write_text(record(0, 4, b'\x00\x01') + record(0x9000, 0, b'\x01\x02') +
                            record(0, 1), encoding='ascii')
            ranges = release.intel_hex_ranges(path)
            self.assertEqual(ranges, [(0x19000, 0x19002)])
            self.assertEqual(release.require_inside('app', ranges, 0x19000, release.APP_END)['occupied_bytes'], 2)
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
        self.assertEqual(release.APP_END - release.APP_START, 56 * 1024)
        self.assertEqual(release.FDS_END - release.FDS_START, 8192)

    def test_eight_k_bootloader_is_default_and_legacy_hex_is_not_used(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = root / 'tools/bootloader/bl_nrf52811_xxaa_s112.hex'
            legacy.parent.mkdir(parents=True)
            write_hex(legacy, descriptor(8192))
            self.assertEqual(release.default_bootloader(root),
                             root / 'tools/bootloader/bl_nrf52811_xxaa_s112_8k.hex')

    def test_compiled_bootloader_reservation_is_checked_even_for_small_app(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app, boot = root / 'app.hex', root / 'boot.hex'
            write_hex(app, b'\x01\x02\x03\x04', start=0x19000, uicr=False)
            config = ROOT / 'SDK/17.1.0_ddde560/dfu/sdk_config.h'
            # Source configuration cannot make a compiled 4 KiB image compatible.
            for invalid in (descriptor(4096), descriptor(8192, flags=1), b'old image'):
                write_hex(boot, invalid)
                with self.assertRaises(release.ReleaseError):
                    release.audit_application_bootloader(app, boot, config)
            write_hex(boot, descriptor())
            audit = release.audit_application_bootloader(app, boot, config)
            self.assertEqual(audit['bootloader_layout']['app_data_bytes'], 8192)
            self.assertEqual(audit['application']['occupied_bytes'], 12)

    def test_images_need_valid_vectors_at_their_fixed_origins(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'app.hex'
            write_hex(app, b'abcd', start=release.APP_START, uicr=False)
            release.validate_image_vectors('app', app, release.APP_START, release.APP_END)
            for stack, reset in ((0, release.APP_START + 9),
                                 (0x20008000, release.APP_START + 9),
                                 (0x20006004, release.APP_START + 9),
                                 (0x20006000, release.APP_START + 8),
                                 (0x20006000, release.APP_END + 1),
                                 (0x20006000, release.APP_START + 0x101)):
                write_hex(app, struct.pack('<II', stack, reset) + b'abcd',
                          start=release.APP_START, uicr=False, vectors=False)
                with self.assertRaises(release.ReleaseError):
                    release.validate_image_vectors('app', app, release.APP_START, release.APP_END)
            write_hex(app, b'abcd', start=release.APP_START + 4, uicr=False)
            with self.assertRaises(release.ReleaseError):
                release.validate_image_vectors('app', app, release.APP_START, release.APP_END)

    def test_conflicting_application_records_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'app.hex'
            write_hex(app, b'abcd', start=release.APP_START, uicr=False)
            app.write_text(app.read_text(encoding='ascii').replace(record(0, 1),
                           record(0x9008, 0, b'\xff') + record(0, 1)), encoding='ascii')
            with self.assertRaisesRegex(release.ReleaseError, 'conflicting'):
                release.validate_image_vectors('app', app, release.APP_START, release.APP_END)

    def test_softdevice_must_match_the_binary_info_used_by_dfu(self):
        with tempfile.TemporaryDirectory() as directory:
            sd = Path(directory) / 'softdevice.hex'
            for size, firmware_id, variant, version, valid in (
                    (0x19000, 0x126, 112, 7003000, True),
                    (0x18000, 0x126, 112, 7003000, False),
                    (0x19000, 0x125, 112, 7003000, False),
                    (0x19000, 0x126, 113, 7003000, False),
                    (0x19000, 0x126, 112, 7002000, False)):
                data = bytearray(0x3018)
                struct.pack_into('<I', data, 0x3008, size)
                struct.pack_into('<H', data, 0x300C, firmware_id)
                struct.pack_into('<II', data, 0x3010, variant, version)
                write_hex(sd, data, start=0, uicr=False, vectors=False)
                if valid:
                    self.assertEqual(release.audit_softdevice(sd)['firmware_id'], 0x126)
                else:
                    with self.assertRaises(release.ReleaseError):
                        release.audit_softdevice(sd)
            write_hex(sd, b'abcd', start=0, uicr=False, vectors=False)
            with self.assertRaises(release.ReleaseError):
                release.audit_softdevice(sd)

    def test_layout_descriptor_must_be_complete_and_unique(self):
        with tempfile.TemporaryDirectory() as directory:
            boot = Path(directory) / 'boot.hex'
            for invalid in (descriptor()[:16], descriptor() + descriptor()):
                write_hex(boot, invalid)
                with self.assertRaises(release.ReleaseError):
                    release.bootloader_layout(boot)
            write_hex(boot, descriptor())
            text = boot.read_text(encoding='ascii')
            text = text.replace(record(0, 1), record(0, 4, b'\x00\x02') +
                                record(0x9008, 0, b'\xff') + record(0, 1))
            boot.write_text(text, encoding='ascii')
            with self.assertRaisesRegex(release.ReleaseError, 'conflicting'):
                release.bootloader_layout(boot)

    def test_uicr_words_must_point_to_the_matching_bootloader_and_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app, boot = root / 'app.hex', root / 'boot.hex'
            write_hex(app, b'abcd', start=0x19000, uicr=False)
            config = ROOT / 'SDK/17.1.0_ddde560/dfu/sdk_config.h'
            for bad_uicr in (b'', struct.pack('<I', 0x29000),
                             struct.pack('<II', 0x28000, 0x2E000),
                             struct.pack('<II', 0x29000, 0x2F000)):
                write_hex(boot, descriptor(), uicr=False)
                text = boot.read_text(encoding='ascii').replace(record(0, 1),
                        record(0, 4, b'\x10\x00') + record(0x1014, 0, bad_uicr) + record(0, 1))
                boot.write_text(text, encoding='ascii')
                with self.assertRaises(release.ReleaseError):
                    release.audit_application_bootloader(app, boot, config)
            write_hex(boot, descriptor())
            text = boot.read_text(encoding='ascii').replace(record(0, 1),
                    record(0x1014, 0, b'\xff') + record(0, 1))
            boot.write_text(text, encoding='ascii')
            with self.assertRaisesRegex(release.ReleaseError, 'conflicting'):
                release.audit_application_bootloader(app, boot, config)

    def test_missing_bootloader_flash_is_a_release_error(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app, boot = root / 'app.hex', root / 'boot.hex'
            write_hex(app, b'abcd', start=0x19000, uicr=False)
            boot.write_text(record(0, 4, b'\x10\x00') +
                            record(0x1014, 0, struct.pack('<II', 0x29000, 0x2E000)) +
                            record(0, 1), encoding='ascii')
            with self.assertRaises(release.ReleaseError):
                release.audit_application_bootloader(app, boot,
                        ROOT / 'SDK/17.1.0_ddde560/dfu/sdk_config.h')

    def test_signing_key_is_matched_to_the_compiled_image(self):
        with tempfile.TemporaryDirectory() as directory:
            boot = Path(directory) / 'boot.hex'
            public_key = bytes(range(64))
            write_hex(boot, descriptor() + public_key)
            with mock.patch.object(release, 'run', return_value=', '.join(
                                   f'0x{value:02x}' for value in public_key)):
                release.validate_bootloader_signing_key('nrfutil', Path('private.pem'), boot)
            for invalid_key in (bytes(range(1, 65)), public_key[:-1]):
                with mock.patch.object(release, 'run', return_value=', '.join(
                                       f'0x{value:02x}' for value in invalid_key)):
                    with self.assertRaises(release.ReleaseError):
                        release.validate_bootloader_signing_key('nrfutil', Path('private.pem'), boot)

    def test_app_stops_before_both_retained_pages(self):
        ranges = [(0x19000, 0x27000)]
        self.assertEqual(release.require_inside('app', ranges, release.APP_START,
                         release.APP_END)['occupied_bytes'], 57344)
        with self.assertRaises(release.ReleaseError):
            release.require_inside('app', [(0x19000, 0x27001)], release.APP_START, release.APP_END)

    def test_development_full_image_paths_also_audit_compiled_layout(self):
        for name in ('build.bat', 'Makefile.nRF52_linux'):
            source = (ROOT / name).read_text(encoding='utf-8')
            self.assertIn('bl_nrf52811_xxaa_s112_8k.hex', source)
            self.assertIn('--audit-only', source)

    def test_generated_keil_bootloader_is_preferred(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = root / 'Keil/_build_DFU52/bl_nrf52811_xxaa_s112.hex'
            generated.parent.mkdir(parents=True)
            generated.write_text(':00000001FF\n', encoding='ascii')
            self.assertEqual(release.default_bootloader(root), generated)

    def test_generated_gcc_bootloader_is_used_before_bundled_image(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = root / 'output/bootloader-8k/bl_nrf52811_xxaa_s112_8k.hex'
            generated.parent.mkdir(parents=True)
            write_hex(generated, descriptor())
            self.assertEqual(release.default_bootloader(root), generated)

    def test_release_version_defaults_to_app_version_and_allows_override(self):
        header = (ROOT / 'EPD/service/EPD_service.h').read_text(encoding='utf-8')
        app_version = int(re.search(r'^#define\s+APP_VERSION\s+(0x[0-9A-Fa-f]+)',
                                    header, re.MULTILINE).group(1), 0)
        self.assertEqual(release.default_application_version(), app_version)
        self.assertGreater(app_version, 0x1C)  # above historical vendor DFU releases
        with mock.patch.dict(os.environ, {'DFU_APPLICATION_VERSION': '0x2B'}):
            self.assertEqual(release.resolve_application_version(None), 0x2B)
        explicit = argparse.Namespace(application_version=0x2C)
        with mock.patch.dict(os.environ, {'DFU_APPLICATION_VERSION': '0x2B'}):
            self.assertEqual(release.resolve_application_version(explicit), 0x2C)
        with mock.patch.dict(os.environ, {'DFU_APPLICATION_VERSION': 'not-a-number'}):
            with self.assertRaises(release.ReleaseError):
                release.resolve_application_version(None)

    def test_release_artifact_names_include_application_version(self):
        with tempfile.TemporaryDirectory() as directory:
            args, tool, state = build_fixture(Path(directory), application_version=0x2A)
            with mock.patch.object(release, 'run', side_effect=tool):
                result = release.build_release(args)
            tag = f'v{0x2A:#x}'
            self.assertEqual({path.name for path in args.output.iterdir()},
                             {f'test-{tag}-ota.zip', f'test-{tag}-settings.hex',
                              f'test-{tag}-full.hex', f'test-{tag}-release.json'})
            self.assertTrue(result['package']['path'].endswith(f'-{tag}-ota.zip'))
            self.assertTrue(result['full_hex']['path'].endswith(f'-{tag}-full.hex'))
            keil = (ROOT / 'tools/keil-postbuild.bat').read_text(encoding='utf-8')
            build = (ROOT / 'build.bat').read_text(encoding='utf-8')
            self.assertIn('-v%DFU_VERSION%-full.hex', keil)
            self.assertIn('-v%DFU_VERSION%-ota.zip', keil)
            self.assertIn('--print-application-version', build)
    def test_release_package_and_settings_use_resolved_application_version(self):
        with tempfile.TemporaryDirectory() as directory:
            args, tool, state = build_fixture(Path(directory), application_version=0x2A)
            with mock.patch.object(release, 'run', side_effect=tool):
                result = release.build_release(args)
            self.assertEqual(result['application_version'], 0x2A)
            package = next(command for command in state['commands']
                           if command[1:3] == ['pkg', 'generate'])
            self.assertEqual(package[package.index('--application-version') + 1], str(0x2A))
            settings = next(command for command in state['commands']
                            if command[1:3] == ['settings', 'generate'])
            self.assertEqual(settings[settings.index('--application-version') + 1], str(0x2A))

    def test_release_wrappers_keep_default_version_and_optional_override(self):
        shell = (ROOT / 'tools/make-ota-nrf52.sh').read_text(encoding='utf-8')
        batch = (ROOT / 'tools/make-ota-nrf52.bat').read_text(encoding='utf-8')
        build = (ROOT / 'build.bat').read_text(encoding='utf-8')
        release_source = (ROOT / 'tools/dfu_release.py').read_text(encoding='utf-8')
        for text in (shell, batch):
            self.assertNotIn('DFU_APPLICATION_VERSION=', text)  # wrappers need no setup
            self.assertIn('dfu_release.py', text)
        self.assertIn('DFU_APPLICATION_VERSION', release_source)
        self.assertIn('--application-version', release_source)
        self.assertIn('--print-application-version', release_source)
        self.assertIn('--print-application-version', build)
        self.assertIn('DFU_APPLICATION_VERSION', build)
        self.assertIn('DFU_RESOURCES_DIR', shell)
        self.assertIn('NRFUTIL', shell)
        self.assertIn('DFU_RESOURCES_DIR', batch)
    def test_reserved_range_overlap_is_rejected(self):
        for start, end in ((0x26FFF, 0x27001), (0x27000, 0x28000), (0x28000, 0x29000)):
            with self.subTest(start=start), self.assertRaises(release.ReleaseError):
                release.require_no_overlap('image', [(start, end)], release.FDS_START, release.FDS_END)
        self.assertIsNone(release.require_no_overlap('image', [(0x19000, 0x27000)],
                          release.FDS_START, release.FDS_END))

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
            self.assertEqual(bundle['packages']['todo.ers']['partition_id'], 9)
            self.assertIn('todo.ers', bundle['personal_record_packages'])
            (root / 'todo.ers').unlink()
            legacy = release.external_resource_packages(root)
            self.assertTrue(legacy['bundled'])
            self.assertNotIn('todo.ers', legacy['packages'])
            self.assertEqual(len(legacy['packages']), 6)
            (root / 'fonts.ers').write_bytes(b'broken')
            with self.assertRaises(release.ReleaseError):
                release.external_resource_packages(root)


if __name__ == '__main__':
    unittest.main()
