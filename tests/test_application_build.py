"""Exercise real ARM application builds and their incremental dependencies.

Run separately: python tests/test_application_build.py
The temporary outputs never replace release or hardware images.
"""
from pathlib import Path
import os
import json
import shutil
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from test_bootloader_build import read_hex


ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = 'Makefile.nRF52_linux'
LINKER = 'SDK/17.1.0_ddde560/gcc_nrf52.ld'


class ApplicationBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name == 'nt':
            cls.compiler = ROOT / 'tools/windows_tools/gcc/xpack-arm-none-eabi-gcc-9.3.1-1.3-win32-x64/bin/arm-none-eabi-gcc.exe'
            packaged_make = Path('D:/cygwin64/bin/make.exe')
            cls.make = str(packaged_make) if packaged_make.is_file() else shutil.which('make')
        else:
            cls.compiler = ROOT / 'tools/gcc/bin/arm-none-eabi-gcc'
            cls.make = shutil.which('make')
        if not cls.compiler.is_file() or not cls.make:
            raise unittest.SkipTest('packaged ARM GCC and GNU make are required')
        (ROOT / 'output').mkdir(exist_ok=True)
        cls.directory = tempfile.TemporaryDirectory(prefix='application-build-test-', dir=ROOT / 'output')
        cls.out = (Path(cls.directory.name) / 'nested' / 'application').relative_to(ROOT).as_posix()
        cls.prefix = ROOT / cls.out / 'nrf52811_xxaa'
        cls.run_make()
        cls.payload = cls.prefix.with_suffix('.bin').read_bytes()

    @classmethod
    def tearDownClass(cls):
        if (ROOT / 'output').resolve() not in Path(cls.directory.name).resolve().parents:
            raise AssertionError('temporary build output escaped the workspace')
        cls.directory.cleanup()

    @classmethod
    def run_make(cls, *arguments, target='nrf52811_xxaa', makefile=MAKEFILE):
        toolchain = cls.compiler.parent.relative_to(ROOT).as_posix() + '/'
        command = [cls.make, '-f', makefile, 'PROJ_DIR=.', 'GNU_INSTALL_ROOT=./' + toolchain,
                   'OUTPUT_DIRECTORY=' + cls.out, 'BUILD_PYTHON=' + Path(sys.executable).as_posix(),
                   'PASS_LINKER_INPUT_VIA_FILE=0', '-j2', target, *arguments]
        process = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, encoding='utf-8', errors='replace')
        if process.returncode:
            raise AssertionError(process.stdout)
        return process.stdout

    def test_production_flash_payload_matches_hex_and_partition(self):
        memory = read_hex(self.prefix.with_suffix('.hex'))
        self.assertEqual(min(memory), 0x19000)
        self.assertLessEqual(max(memory) + 1, 0x27000)
        self.assertLessEqual(len(self.payload), 56 * 1024)
        self.assertEqual(self.payload, bytes(memory.get(address, 255)
                         for address in range(min(memory), max(memory) + 1)))
        self.assertEqual(struct.unpack_from('<I', self.payload)[0], 0x20006000)
        self.assertIn(' / 57344 bytes', self.run_make())

    def test_project_flags_disable_internal_storage_without_changing_sdk_defaults(self):
        flags = shlex.split(self.prefix.with_suffix('.build-options').read_text().split('|')[1])
        command = [str(self.compiler), *flags, '-E', '-dM', '-x', 'c',
                   '-I', str(ROOT / 'SDK/17.1.0_ddde560'), '-']
        result = subprocess.run(command, input='#include "sdk_config.h"\n', cwd=ROOT,
                                text=True, capture_output=True, check=True)
        for name in ('FDS_ENABLED', 'NRF_FSTORAGE_ENABLED'):
            self.assertRegex(result.stdout, rf'(?m)^#define {name} 0$')
        self.assertRegex(result.stdout, r'(?m)^#define FDS_VIRTUAL_PAGES 2$')
        self.assertRegex(result.stdout, r'(?m)^#define FDS_VIRTUAL_PAGE_SIZE 1024$')
        # Keil has its own compiler settings, so migration must cover both
        # targets and C/assembler/preprocessor sources of project definitions.
        project = ET.parse(ROOT / 'Keil/EPD-nRF52.uvprojx')
        for target in project.findall('./Targets/Target'):
            for field in ('Cads', 'Aads'):
                defines = target.findtext('./TargetOption/TargetArmAds/' + field + '/VariousControls/Define')
                for name in ('FDS_ENABLED', 'NRF_FSTORAGE_ENABLED'):
                    self.assertRegex(defines, rf'\b{name}=0\b')
            misc = target.findtext('./TargetOption/TargetArmAds/Aads/VariousControls/MiscControls')
            for name in ('FDS_ENABLED', 'NRF_FSTORAGE_ENABLED'):
                self.assertIn('-D' + name + '=0,', misc)

    def test_makefile_changes_recompile_and_relink(self):
        output = self.run_make('-W', MAKEFILE)
        self.assertGreater(output.count('Compiling file:'), 60)
        self.assertIn('Assembling file: gcc_startup_nrf52811.S', output)
        self.assertIn('Linking target:', output)
        self.assertEqual(self.prefix.with_suffix('.bin').read_bytes(), self.payload)

    def test_linker_changes_only_relink(self):
        output = self.run_make('-W', LINKER)
        self.assertNotIn('Compiling file:', output)
        self.assertNotIn('Assembling file:', output)
        self.assertIn('Linking target:', output)
        self.assertEqual(self.prefix.with_suffix('.bin').read_bytes(), self.payload)

    def test_parameter_changes_recompile_and_restore_production_payload(self):
        try:
            output = self.run_make('GFX_FIXED_FONT_DIRECTION_0=0')
            self.assertGreater(output.count('Compiling file:'), 60)
            self.assertIn('Linking target:', output)
            self.assertNotEqual(self.prefix.with_suffix('.bin').read_bytes(), self.payload)
        finally:
            self.run_make()
        self.assertEqual(self.prefix.with_suffix('.bin').read_bytes(), self.payload)

    def test_debug_layout_updates_flags_sources_and_linker_then_restores(self):
        try:
            output = self.run_make('DEBUG_LAYOUT=1')
            self.assertGreater(output.count('Compiling file:'), 60)
            self.assertIn('Compiling file: SEGGER_RTT.c', output)
            self.assertIn('Linking target:', output)
            self.assertNotIn('tools/check_flash_budget.py', output)
            self.assertIn('gcc_nrf52_debug.ld', self.prefix.with_suffix('.build-options').read_text())
        finally:
            self.run_make()
        self.assertEqual(self.prefix.with_suffix('.bin').read_bytes(), self.payload)

    def test_unchanged_build_and_size_target_do_not_recompile(self):
        stamp = self.prefix.with_suffix('.out').stat().st_mtime_ns
        for target in ('nrf52811_xxaa', 'size', 'memory', self.prefix.relative_to(ROOT).with_suffix('.out').as_posix()):
            output = self.run_make(target=target)
            self.assertNotIn('Compiling file:', output)
            self.assertNotIn('Assembling file:', output)
            self.assertNotIn('Linking target:', output)
        self.assertEqual(self.prefix.with_suffix('.out').stat().st_mtime_ns, stamp)

    def test_parallel_rebuild_cleans_before_starting_the_build(self):
        # Execute the real rebuild/clean recipes, with a non-destructive RM and
        # a lightweight child-build probe. The child clean still invokes make.
        # This catches the former parallel `rebuild: clean build` dependency DAG.
        for entry in (MAKEFILE, 'Makefile.nRF52'):
            with self.subTest(makefile=entry):
                probe_dir = Path(self.directory.name) / entry.replace('.', '_')
                probe_dir.mkdir()
                script = probe_dir / 'schedule.py'
                script.write_text(
                    'from pathlib import Path\nimport json, subprocess, sys, time\n'
                    'root=Path(__file__).resolve().parent\n'
                    'def emit(name):\n'
                    '    (root/(name+".json")).write_text(json.dumps({"time_ns":time.time_ns()}))\n'
                    'mode=sys.argv[1]\n'
                    'if mode=="clean":\n'
                    '    emit("clean_start")\n'
                    '    limit=time.monotonic()+2\n'
                    '    while not (root/"build_start.json").exists() and time.monotonic()<limit:\n'
                    '        time.sleep(0.01)\n'
                    '    emit("clean_end")\n'
                    'elif mode=="child":\n'
                    '    if "clean" in sys.argv[2:]:\n'
                    '        raise SystemExit(subprocess.call(' + repr([self.make]) + '+sys.argv[2:]))\n'
                    '    emit("build_start")\n'
                    '    emit("build_end")\n'
                    'else:\n'
                    '    raise SystemExit(2)\n', encoding='utf-8')
                invocation = shlex.quote(Path(sys.executable).as_posix()) + ' ' + shlex.quote(script.relative_to(ROOT).as_posix())
                self.run_make('-o', 'firmware', 'RM=' + invocation + ' clean',
                              'MAKE=' + invocation + ' child', target='rebuild', makefile=entry)
                clean_end = json.loads((probe_dir / 'clean_end.json').read_text())['time_ns']
                build_start = json.loads((probe_dir / 'build_start.json').read_text())['time_ns']
                self.assertLess(clean_end, build_start)
                self.assertEqual(self.prefix.with_suffix('.bin').read_bytes(), self.payload)


if __name__ == '__main__':
    unittest.main()
