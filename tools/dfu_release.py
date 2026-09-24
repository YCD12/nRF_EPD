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
import zipfile
import zlib
from pathlib import Path

APP_START = 0x19000
APP_END = 0x27000
FDS_START = 0x27000
FDS_END = 0x29000
BOOTLOADER_START = 0x29000
SETTINGS_BACKUP = 0x2E000
SETTINGS = 0x2F000
UICR_BOOTLOADER_RECORD_START = 0x10001014
UICR_BOOTLOADER_RECORD_END = 0x1000101C
SOFTDEVICE_ID = 0x126
HARDWARE_VERSION = 52
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
}


class ReleaseError(ValueError):
    pass


def default_bootloader(workspace: Path | None = None) -> Path:
    """Prefer a locally built bootloader HEX when present."""
    workspace = ROOT if workspace is None else workspace
    generated = workspace / 'Keil/_build_DFU52/bl_nrf52811_xxaa_s112.hex'
    if generated.is_file():
        return generated
    return workspace / 'tools/bootloader/bl_nrf52811_xxaa_s112.hex'


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def intel_hex_ranges(path: Path) -> list[tuple[int, int]]:
    base = 0
    ranges: list[tuple[int, int]] = []
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
                ranges.append((base + offset, base + offset + length))
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
    if not ranges:
        raise ReleaseError(f'{path}: no data records')
    return ranges


def range_summary(ranges: list[tuple[int, int]]) -> dict[str, int]:
    return {'start': min(start for start, _ in ranges),
            'end_exclusive': max(end for _, end in ranges),
            'occupied_bytes': sum(end - start for start, end in ranges)}


def require_inside(name: str, ranges: list[tuple[int, int]], start: int, end: int) -> dict[str, int]:
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


def run(command: list[str]) -> str:
    completed = subprocess.run(command, check=True, text=True, capture_output=True)
    return completed.stdout + completed.stderr


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


def validate_package_layout(package: Path) -> None:
    with zipfile.ZipFile(package) as archive:
        try:
            manifest = json.loads(archive.read('manifest.json'))['manifest']
            application = manifest['application']
            binary = archive.read(application['bin_file'])
            init = archive.read(application['dat_file'])
        except (KeyError, zipfile.BadZipFile, json.JSONDecodeError) as error:
            raise ReleaseError('DFU ZIP must contain one complete application manifest') from error
    if not binary or not init:
        raise ReleaseError('DFU ZIP contains empty application or init packet')


def external_resource_packages(directory: Path | None) -> dict[str, object]:
    """Audit the single-copy resource packages paired with a firmware release.

    Config and annotations are listed separately because a field-device migration
    must generate those two packages from its exported user data, not overwrite
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


def build_release(args: argparse.Namespace) -> dict[str, object]:
    app = args.application.resolve()
    bootloader = args.bootloader.resolve()
    softdevice = args.softdevice.resolve()
    private_key = args.key.resolve()
    bootloader_config = args.bootloader_config.resolve()
    for path in (app, bootloader, softdevice, private_key, bootloader_config):
        if not path.is_file():
            raise ReleaseError(f'missing required file: {path}')
    resources = external_resource_packages(args.resources_dir)

    app_range = require_inside('application', intel_hex_ranges(app), APP_START, APP_END)
    bootloader_ranges = intel_hex_ranges(bootloader)
    boot_flash_ranges = [(start, end) for start, end in bootloader_ranges if end <= 0x10000000]
    boot_uicr_ranges = [(start, end) for start, end in bootloader_ranges if start >= 0x10000000]
    if len(boot_flash_ranges) + len(boot_uicr_ranges) != len(bootloader_ranges):
        raise ReleaseError('bootloader image mixes unsupported flash and UICR addresses')
    boot_range = require_inside('bootloader', boot_flash_ranges, BOOTLOADER_START, SETTINGS_BACKUP)
    require_inside('bootloader UICR', boot_uicr_ranges, UICR_BOOTLOADER_RECORD_START,
                   UICR_BOOTLOADER_RECORD_END)
    signed = config_value(bootloader_config, 'NRF_DFU_REQUIRE_SIGNED_APP_UPDATE')
    downgrade = config_value(bootloader_config, 'NRF_DFU_APP_DOWNGRADE_PREVENTION')
    force_dual = config_value(bootloader_config, 'NRF_DFU_FORCE_DUAL_BANK_APP_UPDATES')
    single_bank = config_value(bootloader_config, 'NRF_DFU_SINGLE_BANK_APP_UPDATES')
    reserved = config_value(bootloader_config, 'NRF_DFU_APP_DATA_AREA_SIZE')
    if signed != 1 or downgrade != 1 or reserved != FDS_END - FDS_START:
        raise ReleaseError('bootloader DFU security or retained-data configuration is not supported')
    if force_dual:
        raise ReleaseError('force-dual-bank bootloader cannot accept this 56 KiB application layout')

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    stem = args.name
    package = output / f'{stem}-ota.zip'
    settings = output / f'{stem}-settings.hex'
    full = output / f'{stem}-full.hex'
    package.unlink(missing_ok=True)
    settings.unlink(missing_ok=True)
    full.unlink(missing_ok=True)

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
    validate_package_layout(package)
    info = package_info(args.nrfutil, package)
    if (info['hardware'], info['softdevice'], info['size']) != (
            HARDWARE_VERSION, SOFTDEVICE_ID, app_range['occupied_bytes']):
        raise ReleaseError('DFU package metadata does not match application image')
    result = {
        'release_abi': 1,
        'application_version': info['version'],
        'application': app_range,
        'internal_fds_reserved': {'start': FDS_START, 'end_exclusive': FDS_END,
                                  'bytes': FDS_END - FDS_START, 'changed': False},
        'bootloader': boot_range,
        'bootloader_uicr': range_summary(boot_uicr_ranges),
        'secure_dfu': {'signed_app_required': bool(signed),
                       'downgrade_prevention': bool(downgrade),
                       'force_dual_bank': bool(force_dual),
                       'single_bank_configured': bool(single_bank),
                       'effective_update_mode': 'dual-bank-if-space-else-single-bank',
                       'external_firmware_slots': 0},
        'external_resources': resources,
        'package': {'path': package.name, 'sha256': sha256(package), **info},
        'settings': {'path': settings.name, 'sha256': sha256(settings), **settings_range},
        'full_hex': {'path': full.name, 'sha256': sha256(full),
                     'occupied_bytes': sum(end - start for start, end in full_ranges)},
    }
    (output / f'{stem}-release.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--application', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
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
    try:
        result = build_release(args)
    except (ReleaseError, OSError, subprocess.CalledProcessError) as error:
        print(f'DFU release rejected: {error}', file=sys.stderr)
        raise SystemExit(1)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
