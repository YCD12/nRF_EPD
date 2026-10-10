#!/usr/bin/env python3
"""Build and audit a Nordic Secure DFU application release.

The tool deliberately handles only the internal application image.  External
Flash resource packages are deployed separately and are never firmware slots.
It also makes the actual single-bank fallback visible in the release manifest.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

APP_START = 0x19000
APP_END = 0x27000
FDS_START = APP_END
FDS_END = 0x29000
BOOTLOADER_START = 0x29000
SETTINGS_BACKUP = 0x2E000
SETTINGS = 0x2F000
UICR_BOOTLOADER_RECORD_START = 0x10001014
UICR_BOOTLOADER_RECORD_END = 0x1000101C
SOFTDEVICE_ID = 0x126
HARDWARE_VERSION = 52
RAM_START = 0x20000000
RAM_END = 0x20006000
BOOTLOADER_LAYOUT_MAGIC = 0x31444C42  # BLD1
BOOTLOADER_LAYOUT_VERSION = 1
LEGACY_APP_END = 0x27000
ROOT = Path(__file__).resolve().parents[1]
RESOURCE_ABI = 1
RESOURCE_MAGIC = 0x31535245
RESOURCE_COMMIT = 0xA55AC33C
RESOURCE_STAMP_MAGIC = 0x54535245
RESOURCE_PACKAGES = {
    'partition-table.ers': 1,
    'calendar.ers': 5,
    'ui.ers': 6,
    'fonts.ers': 7,
}
PERSONAL_RECORD_PACKAGES = {
    'epd-config.ers': 2,
    'calendar-annotations.ers': 3,
    'todo.ers': 9,
}


class ReleaseError(ValueError):
    pass


def default_bootloader(workspace: Path | None = None) -> Path:
    """Prefer a locally built bootloader HEX when present."""
    workspace = ROOT if workspace is None else workspace
    generated = workspace / 'Keil/_build_DFU52/bl_nrf52811_xxaa_s112.hex'
    if generated.is_file():
        return generated
    generated = workspace / 'output/bootloader-8k/bl_nrf52811_xxaa_s112_8k.hex'
    if generated.is_file():
        return generated
    return workspace / 'tools/bootloader/bl_nrf52811_xxaa_s112_8k.hex'


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def intel_hex_records(path: Path) -> list[tuple[int, bytes]]:
    base = 0
    records: list[tuple[int, bytes]] = []
    for line_number, raw in enumerate(path.read_text(encoding='ascii').splitlines(), 1):
        if not raw:
            continue
        if not raw.startswith(':'):
            raise ReleaseError(f'{path}:{line_number}: invalid Intel HEX record')
        try:
            record = bytes.fromhex(raw[1:])
        except ValueError as error:
            raise ReleaseError(f'{path}:{line_number}: invalid hexadecimal data') from error
        if len(record) < 5 or len(record) != record[0] + 5 or sum(record) & 0xFF:
            raise ReleaseError(f'{path}:{line_number}: invalid Intel HEX checksum or length')
        length, offset, kind = record[0], (record[1] << 8) | record[2], record[3]
        data = record[4:4 + length]
        if kind == 0:
            if length:
                records.append((base + offset, data))
        elif kind == 2:
            if length != 2:
                raise ReleaseError(f'{path}:{line_number}: invalid segment address')
            base = int.from_bytes(data, 'big') << 4
        elif kind == 4:
            if length != 2:
                raise ReleaseError(f'{path}:{line_number}: invalid extended address')
            base = int.from_bytes(data, 'big') << 16
        elif kind not in (1, 3, 5):
            raise ReleaseError(f'{path}:{line_number}: unsupported Intel HEX record {kind}')
    if not records:
        raise ReleaseError(f'{path}: no data records')
    return records


def intel_hex_ranges(path: Path) -> list[tuple[int, int]]:
    return [(address, address + len(data)) for address, data in intel_hex_records(path)]


def image_memory(path: Path) -> dict[int, int]:
    memory: dict[int, int] = {}
    for address, data in intel_hex_records(path):
        for offset, value in enumerate(data):
            position = address + offset
            if position in memory and memory[position] != value:
                raise ReleaseError(f'{path}: conflicting Intel HEX data at 0x{position:X}')
            memory[position] = value
    return memory


def validate_image_vectors(name: str, path: Path, start: int, end: int) -> None:
    memory = image_memory(path)
    try:
        stack_pointer, reset_vector = struct.unpack('<II', bytes(memory[start + i] for i in range(8)))
    except KeyError as error:
        raise ReleaseError(f'{name}: missing complete vector table at 0x{start:X}') from error
    if not (RAM_START < stack_pointer <= RAM_END) or stack_pointer % 8:
        raise ReleaseError(f'{name}: initial stack pointer is outside nRF52811 RAM or unaligned')
    reset_address = reset_vector & ~1
    if (not reset_vector & 1 or not start <= reset_address < end or
            reset_address not in memory or reset_address + 1 not in memory):
        raise ReleaseError(f'{name}: reset vector does not point to Thumb code in its image')


def audit_softdevice(path: Path) -> dict[str, int]:
    """Match SDK nrf_sdm.h information fields to the S112 build and DFU sd_req."""
    summary = require_inside('SoftDevice', intel_hex_ranges(path), 0, APP_START)
    memory = image_memory(path)
    try:
        # MBR_SIZE (0x1000) + SOFTDEVICE_INFO_STRUCT_OFFSET (0x2000).
        size = struct.unpack('<I', bytes(memory[0x3008 + i] for i in range(4)))[0]
        firmware_id = struct.unpack('<H', bytes(memory[0x300C + i] for i in range(2)))[0]
        variant, version = struct.unpack('<II', bytes(memory[0x3010 + i] for i in range(8)))
    except KeyError as error:
        raise ReleaseError('SoftDevice is missing SDK information fields') from error
    if summary['start'] != 0 or (size, firmware_id, variant, version) != (
            APP_START, SOFTDEVICE_ID, 112, 7003000):
        raise ReleaseError('SoftDevice does not match S112 7.3.0, application origin or DFU sd_req')
    return {**summary, 'firmware_id': firmware_id, 'variant': variant, 'version': version}


def bootloader_layout(path: Path) -> dict[str, int]:
    """Read the retained layout descriptor from the compiled HEX, not its filename."""
    flash = bytearray(b'\xff' * (SETTINGS_BACKUP - BOOTLOADER_START))
    written: dict[int, int] = {}
    for address, data in intel_hex_records(path):
        for offset, value in enumerate(data):
            position = address + offset
            if BOOTLOADER_START <= position < SETTINGS_BACKUP:
                if position in written and written[position] != value:
                    raise ReleaseError('bootloader HEX contains conflicting data records')
                written[position] = value
                flash[position - BOOTLOADER_START] = value
    magic = struct.pack('<II', BOOTLOADER_LAYOUT_MAGIC, BOOTLOADER_LAYOUT_VERSION)
    locations = [i for i in range(len(flash) - 31) if flash[i:i + 8] == magic]
    if len(locations) != 1:
        raise ReleaseError('bootloader lacks one verified BLD1 layout descriptor; '
                           'rebuild the 8 KiB bootloader before packaging')
    offset = locations[0]
    if any(BOOTLOADER_START + offset + i not in written for i in range(32)):
        raise ReleaseError('bootloader layout descriptor is incomplete')
    values = struct.unpack_from('<8I', flash, offset)
    fields = ('magic', 'version', 'app_start', 'bootloader_start', 'app_data_bytes',
              'settings_backup', 'settings', 'security_flags')
    return dict(zip(fields, values))


def range_summary(ranges: list[tuple[int, int]]) -> dict[str, int]:
    return {'start': min(start for start, _ in ranges),
            'end_exclusive': max(end for _, end in ranges),
            'occupied_bytes': sum(end - start for start, end in ranges)}


def require_inside(name: str, ranges: list[tuple[int, int]], start: int, end: int) -> dict[str, int]:
    if not ranges:
        raise ReleaseError(f'{name}: no data records in the expected region')
    for item_start, item_end in ranges:
        if item_start < start or item_end > end:
            raise ReleaseError(f'{name} writes 0x{item_start:05X}-0x{item_end:05X}, outside '
                               f'allowed 0x{start:05X}-0x{end:05X}')
    return range_summary(ranges)


def require_no_overlap(name: str, ranges: list[tuple[int, int]], start: int, end: int) -> None:
    for item_start, item_end in ranges:
        if item_start < end and item_end > start:
            raise ReleaseError(f'{name} writes reserved range 0x{start:05X}-0x{end:05X}')


def config_value(path: Path, name: str) -> int:
    match = re.search(rf'^\s*#define\s+{re.escape(name)}\s+([^\s/]+)',
                      path.read_text(encoding='utf-8'), re.MULTILINE)
    if match is None:
        raise ReleaseError(f'{path}: missing {name}')
    return int(match.group(1), 0)


def validate_bootloader_uicr(path: Path) -> None:
    values: dict[int, int] = {}
    for address, data in intel_hex_records(path):
        for offset, value in enumerate(data):
            position = address + offset
            if UICR_BOOTLOADER_RECORD_START <= position < UICR_BOOTLOADER_RECORD_END:
                if position in values and values[position] != value:
                    raise ReleaseError('bootloader UICR contains conflicting data records')
                values[position] = value
    addresses = range(UICR_BOOTLOADER_RECORD_START, UICR_BOOTLOADER_RECORD_END)
    if any(position not in values for position in addresses):
        raise ReleaseError('bootloader UICR must contain both complete address words')
    if struct.unpack('<II', bytes(values[position] for position in addresses)) != (
            BOOTLOADER_START, SETTINGS_BACKUP):
        raise ReleaseError('bootloader UICR addresses do not match the release layout')


def run(command: list[str]) -> str:
    completed = subprocess.run(command, check=True, text=True, capture_output=True)
    return completed.stdout + completed.stderr


def validate_bootloader_signing_key(nrfutil: str, key: Path, bootloader: Path) -> None:
    public_code = run([nrfutil, 'keys', 'display', str(key), '--key', 'pk', '--format', 'code'])
    public_key = bytes(int(value, 16) for value in re.findall(r'\b0x([0-9a-fA-F]{2})\b', public_code))
    image = bytearray(b'\xff' * (SETTINGS_BACKUP - BOOTLOADER_START))
    for address, data in intel_hex_records(bootloader):
        if BOOTLOADER_START <= address and address + len(data) <= SETTINGS_BACKUP:
            image[address - BOOTLOADER_START:address - BOOTLOADER_START + len(data)] = data
    if len(public_key) != 64 or public_key not in image:
        raise ReleaseError('signing key does not match the public key in the compiled bootloader')


def package_info(nrfutil: str, package: Path) -> dict[str, int | str]:
    output = run([nrfutil, 'pkg', 'display', str(package)])
    required = {
        'version': r'fw_version:\s+0x([0-9a-fA-F]+)',
        'hardware': r'hw_version\s+0x([0-9a-fA-F]+)',
        'size': r'app_size:\s+(\d+)',
        'signature': r'signature_type:\s+([A-Z0-9_]+)',
        'softdevice': r'sd_req:\s+(0x[0-9a-fA-F]+)',
    }
    result: dict[str, int | str] = {}
    for key, expression in required.items():
        match = re.search(expression, output)
        if match is None:
            raise ReleaseError(f'cannot read {key} from nrfutil package output')
        value = match.group(1)
        if key == 'signature':
            result[key] = value
        elif key == 'size':
            result[key] = int(value, 10)
        else:
            result[key] = int(value, 16)
    if result['signature'] != 'ECDSA_P256_SHA256':
        raise ReleaseError('DFU package is not ECDSA P-256 signed')
    return result


def application_binary(path: Path) -> bytes:
    """Match nrfutil's HEX conversion: erased gaps and a word-aligned size."""
    memory = image_memory(path)
    size = (max(memory) + 1 - APP_START + 3) & ~3
    if min(memory) != APP_START or not 0 < size <= APP_END - APP_START:
        raise ReleaseError('application binary is outside the release application region')
    return bytes(memory.get(APP_START + offset, 0xFF) for offset in range(size))


def validate_package_layout(package: Path, expected_binary: bytes | None = None) -> None:
    try:
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read('manifest.json'))['manifest']
            application = manifest['application']
            binary = archive.read(application['bin_file'])
            init = archive.read(application['dat_file'])
    except (KeyError, TypeError, zipfile.BadZipFile, json.JSONDecodeError, UnicodeDecodeError) as error:
        raise ReleaseError('DFU ZIP must contain one complete application manifest') from error
    if not binary or not init:
        raise ReleaseError('DFU ZIP contains empty application or init packet')
    if expected_binary is not None and binary != expected_binary:
        raise ReleaseError('DFU ZIP application bytes do not match the input application image')


def validate_merged_image(full: Path, components: tuple[Path, ...]) -> None:
    """Require the full HEX to contain precisely the audited input images."""
    expected: dict[int, int] = {}
    for component in components:
        for address, value in image_memory(component).items():
            if address in expected and expected[address] != value:
                raise ReleaseError(f'merge inputs conflict at 0x{address:X}')
            expected[address] = value
    actual = image_memory(full)
    for address, value in expected.items():
        if actual.get(address) != value:
            raise ReleaseError(f'merged full image is missing or changes input bytes at 0x{address:X}')
    unexpected = actual.keys() - expected.keys()
    if unexpected:
        raise ReleaseError(f'merged full image contains unexpected bytes at 0x{min(unexpected):X}')


def external_resource_packages(directory: Path | None) -> dict[str, object]:
    """Audit the single-copy resource packages paired with a firmware release.

    Config, annotations and TODO items are listed separately because a field-device migration
    must generate those packages from its exported user data, not overwrite
    it with the factory defaults used by a fresh-device bundle.
    """
    required = {
        'abi': RESOURCE_ABI,
        'single_copy': True,
        'firmware_slots': 0,
        'static_packages': list(RESOURCE_PACKAGES),
        'personal_record_packages': list(PERSONAL_RECORD_PACKAGES),
    }
    if directory is None:
        return {**required, 'bundled': False}
    directory = directory.resolve()
    if not directory.is_dir():
        raise ReleaseError(f'missing external resource directory: {directory}')
    packages: dict[str, dict[str, int | str]] = {}
    for name, partition_id in {**RESOURCE_PACKAGES, **PERSONAL_RECORD_PACKAGES}.items():
        path = directory / name
        if partition_id == 9 and not path.is_file():
            # TODO is optional in resource directories created before mode 4.
            continue
        if not path.is_file():
            raise ReleaseError(f'missing external resource package: {path}')
        data = path.read_bytes()
        if len(data) < 33:
            raise ReleaseError(f'{path}: resource package is too small')
        magic, abi_and_partition, revision, length, payload_crc, capacity, header_crc, commit = \
            struct.unpack_from('<8I', data)
        payload = data[32:32 + length]
        trailer = data[32 + length:]
        stamp_ok = not trailer
        if len(trailer) == 12:
            stamp_magic, _built_at, stamp_crc = struct.unpack('<3I', trailer)
            stamp_ok = stamp_magic == RESOURCE_STAMP_MAGIC and stamp_crc == zlib.crc32(trailer[:8])
        if (magic != RESOURCE_MAGIC or abi_and_partition != (RESOURCE_ABI << 16 | partition_id)
                or not revision or length != len(payload) or payload_crc != zlib.crc32(payload)
                or capacity != 524288 or header_crc != zlib.crc32(data[:24])
                or commit != RESOURCE_COMMIT or not stamp_ok):
            raise ReleaseError(f'{path}: invalid or incompatible ERS package')
        packages[name] = {'partition_id': partition_id, 'revision': revision,
                          'bytes': len(data), 'sha256': sha256(path)}
    return {**required, 'bundled': True, 'directory': str(directory), 'packages': packages}


def audit_application_bootloader(app: Path, bootloader: Path,
                                bootloader_config: Path) -> dict[str, object]:
    for path in (app, bootloader, bootloader_config):
        if not path.is_file():
            raise ReleaseError(f'missing required file: {path}')
    app_range = require_inside('application', intel_hex_ranges(app), APP_START, APP_END)
    validate_image_vectors('application', app, APP_START, APP_END)
    bootloader_ranges = intel_hex_ranges(bootloader)
    boot_flash_ranges = [(start, end) for start, end in bootloader_ranges if end <= 0x10000000]
    boot_uicr_ranges = [(start, end) for start, end in bootloader_ranges if start >= 0x10000000]
    if len(boot_flash_ranges) + len(boot_uicr_ranges) != len(bootloader_ranges):
        raise ReleaseError('bootloader image mixes unsupported flash and UICR addresses')
    boot_range = require_inside('bootloader', boot_flash_ranges, BOOTLOADER_START, SETTINGS_BACKUP)
    validate_image_vectors('bootloader', bootloader, BOOTLOADER_START, SETTINGS_BACKUP)
    require_inside('bootloader UICR', boot_uicr_ranges, UICR_BOOTLOADER_RECORD_START,
                   UICR_BOOTLOADER_RECORD_END)
    validate_bootloader_uicr(bootloader)
    signed = config_value(bootloader_config, 'NRF_DFU_REQUIRE_SIGNED_APP_UPDATE')
    downgrade = config_value(bootloader_config, 'NRF_DFU_APP_DOWNGRADE_PREVENTION')
    force_dual = config_value(bootloader_config, 'NRF_DFU_FORCE_DUAL_BANK_APP_UPDATES')
    single_bank = config_value(bootloader_config, 'NRF_DFU_SINGLE_BANK_APP_UPDATES')
    reserved = config_value(bootloader_config, 'NRF_DFU_APP_DATA_AREA_SIZE')
    if signed != 1 or downgrade != 1 or reserved != FDS_END - FDS_START:
        raise ReleaseError('bootloader DFU security or retained-data configuration is not supported')
    if force_dual:
        raise ReleaseError('force-dual-bank bootloader cannot accept this 56 KiB application layout')
    layout = bootloader_layout(bootloader)
    expected = {'magic': BOOTLOADER_LAYOUT_MAGIC, 'version': BOOTLOADER_LAYOUT_VERSION,
                'app_start': APP_START, 'bootloader_start': BOOTLOADER_START,
                'app_data_bytes': reserved, 'settings_backup': SETTINGS_BACKUP,
                'settings': SETTINGS,
                'security_flags': signed | (downgrade << 1) | (force_dual << 2) | (single_bank << 3)}
    if layout != expected:
        raise ReleaseError('compiled bootloader layout does not match the 8 KiB release configuration; '
                           'rebuild the bootloader')
    return {'application': app_range, 'bootloader': boot_range,
            'bootloader_uicr': range_summary(boot_uicr_ranges),
            'bootloader_layout': layout,
            'secure_dfu': {'signed_app_required': bool(signed),
                           'downgrade_prevention': bool(downgrade),
                           'force_dual_bank': bool(force_dual),
                           'single_bank_configured': bool(single_bank),
                           'effective_update_mode': ('single-bank' if single_bank else
                                                     'dual-bank-if-space-else-single-bank'),
                           'external_firmware_slots': 0}}


def publish_release_artifacts(staging: Path, output: Path, names: tuple[str, ...]) -> None:
    """Publish an audited set, with its manifest last and rollback on I/O errors.

    Each replace is atomic for one file; this is not an atomic multi-file release.
    Keep backups if an I/O failure also prevents restoring the previous release.
    """
    backups = Path(tempfile.mkdtemp(prefix='.dfu-release-backup-', dir=output))
    previous: dict[str, Path | None] = {}
    published: list[str] = []
    keep_backups = False
    try:
        for index, name in enumerate(names):
            destination = output / name
            backup = backups / str(index) if destination.exists() else None
            if backup is not None:
                shutil.copy2(destination, backup)
            previous[name] = backup
        for name in names:
            (staging / name).replace(output / name)
            published.append(name)
    except OSError as error:
        rollback_errors = []
        for name in reversed(published):
            try:
                backup = previous[name]
                if backup is None:
                    (output / name).unlink(missing_ok=True)
                else:
                    backup.replace(output / name)
            except OSError as restore_error:
                rollback_errors.append(str(restore_error))
        if rollback_errors:
            keep_backups = True
            raise ReleaseError(f'cannot restore previous release; retained backups: {backups}; '
                               + '; '.join(rollback_errors)) from error
        raise
    finally:
        if not keep_backups:
            shutil.rmtree(backups, ignore_errors=True)


def build_release(args: argparse.Namespace) -> dict[str, object]:
    app = args.application.resolve()
    bootloader = args.bootloader.resolve()
    softdevice = args.softdevice.resolve()
    private_key = args.key.resolve()
    bootloader_config = args.bootloader_config.resolve()
    audit = audit_application_bootloader(app, bootloader, bootloader_config)
    for path in (softdevice, private_key):
        if not path.is_file():
            raise ReleaseError(f'missing required file: {path}')
    audit['softdevice'] = audit_softdevice(softdevice)
    validate_bootloader_signing_key(args.nrfutil, private_key, bootloader)
    audit['secure_dfu']['signing_key_matches_bootloader'] = True
    resources = external_resource_packages(args.resources_dir)
    binary = application_binary(app)

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    stem = args.name
    if not stem or stem in ('.', '..') or '/' in stem or '\\' in stem or ':' in stem:
        raise ReleaseError('release name must be a filename without directory components')
    with tempfile.TemporaryDirectory(prefix='.dfu-release-', dir=output,
                                     ignore_cleanup_errors=True) as directory:
        staging = Path(directory)
        package = staging / f'{stem}-ota.zip'
        settings = staging / f'{stem}-settings.hex'
        full = staging / f'{stem}-full.hex'
        manifest = staging / f'{stem}-release.json'

        run([args.nrfutil, 'pkg', 'generate', '--application', str(app), '--key-file', str(private_key),
             '--hw-version', str(HARDWARE_VERSION), '--sd-req', hex(SOFTDEVICE_ID), '--sd-id',
             hex(SOFTDEVICE_ID), '--application-version', '1',
             '--app-boot-validation', 'VALIDATE_GENERATED_CRC', str(package)])
        run([args.nrfutil, 'settings', 'generate', '--family', 'NRF52810', '--application', str(app),
             '--softdevice', str(softdevice), '--application-version', '1',
             '--bootloader-version', '1', '--bl-settings-version', '2', '--key-file', str(private_key),
             '--app-boot-validation', 'VALIDATE_GENERATED_CRC', '--start-address', hex(SETTINGS),
             str(settings)])
        run([args.mergehex, '-m', str(softdevice), str(bootloader), str(app), str(settings), '-o', str(full)])

        settings_range = require_inside('DFU settings', intel_hex_ranges(settings), SETTINGS_BACKUP,
                                        SETTINGS + 0x1000)
        settings_records = intel_hex_ranges(settings)
        if not any(start <= SETTINGS_BACKUP < end for start, end in settings_records) or \
                not any(start <= SETTINGS < end for start, end in settings_records):
            raise ReleaseError('DFU settings image must contain both settings pages')
        full_ranges = intel_hex_ranges(full)
        full_flash_ranges = [(start, end) for start, end in full_ranges if end <= 0x10000000]
        require_no_overlap('merged full image', full_flash_ranges, FDS_START, FDS_END)
        validate_merged_image(full, (softdevice, bootloader, app, settings))
        validate_package_layout(package, binary)
        info = package_info(args.nrfutil, package)
        if (info['hardware'], info['softdevice'], info['size']) != (
                HARDWARE_VERSION, SOFTDEVICE_ID, len(binary)):
            raise ReleaseError('DFU package metadata does not match application image')
        result = {
            'release_abi': 2,
            'application_version': info['version'],
            **audit,
            'internal_fds_reserved': {'start': FDS_START, 'end_exclusive': FDS_END,
                                      'bytes': FDS_END - FDS_START, 'changed': False,
                                      'runtime_fds_enabled': False},
            'layout_migration': {'legacy_app_end': LEGACY_APP_END,
                                 'requires_matching_bootloader_for_expanded_app': False,
                                 'bootloader_install_method': 'SWD full HEX',
                                 'application_only_ota_updates_bootloader': False,
                                 'paired_bootloader_sha256': sha256(bootloader)},
            'external_resources': resources,
            'package': {'path': package.name, 'sha256': sha256(package), **info},
            'settings': {'path': settings.name, 'sha256': sha256(settings), **settings_range},
            'full_hex': {'path': full.name, 'sha256': sha256(full),
                         'occupied_bytes': sum(end - start for start, end in full_ranges)},
        }
        manifest.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        publish_release_artifacts(staging, output,
                                  (package.name, settings.name, full.name, manifest.name))
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--application', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--audit-only', action='store_true',
                        help='validate application and compiled bootloader layout without generating files')
    parser.add_argument('--name', default='nrf52811_xxaa')
    parser.add_argument('--key', type=Path, default=Path('tools/priv.pem'))
    parser.add_argument('--bootloader', type=Path, default=default_bootloader())
    parser.add_argument('--softdevice', type=Path,
                        default=Path('SDK/17.1.0_ddde560/components/softdevice/s112/hex/s112_nrf52_7.3.0_softdevice.hex'))
    parser.add_argument('--bootloader-config', type=Path,
                        default=Path('SDK/17.1.0_ddde560/dfu/sdk_config.h'))
    parser.add_argument('--resources-dir', type=Path,
                        help='directory containing audited ERS packages paired with this release')
    parser.add_argument('--nrfutil', default=os.environ.get('NRFUTIL') or shutil.which('nrfutil') or 'nrfutil')
    parser.add_argument('--mergehex', default=os.environ.get('MERGEHEX') or shutil.which('mergehex') or 'mergehex')
    args = parser.parse_args()
    if not args.audit_only and args.output is None:
        parser.error('--output is required unless --audit-only is used')
    try:
        if args.audit_only:
            result = audit_application_bootloader(args.application.resolve(), args.bootloader.resolve(),
                                                 args.bootloader_config.resolve())
            result['softdevice'] = audit_softdevice(args.softdevice.resolve())
        else:
            result = build_release(args)
    except (ReleaseError, OSError, subprocess.CalledProcessError) as error:
        print(f'DFU release rejected: {error}', file=sys.stderr)
        raise SystemExit(1)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
