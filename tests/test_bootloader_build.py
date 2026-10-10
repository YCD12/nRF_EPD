"""Exercise the real ARM bootloader build and its incremental dependencies.

Run separately from the toolchain-free host tests:
    python tests/test_bootloader_build.py
"""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = 'Bootloader/Makefile'
LINKER = 'Bootloader/gcc_bootloader.ld'


def read_hex(path):
    memory = {}
    base = 0
    for line in path.read_text(encoding='ascii').splitlines():
        record = bytes.fromhex(line[1:])
        if sum(record) & 255:
            raise AssertionError('bad Intel HEX checksum')
        size, kind = record[0], record[3]
        address = int.from_bytes(record[1:3], 'big')
        data = record[4:4 + size]
        if kind == 0:
            memory.update((base + address + i, value) for i, value in enumerate(data))
        elif kind == 2:
            base = int.from_bytes(data, 'big') << 4
        elif kind == 4:
            base = int.from_bytes(data, 'big') << 16
    return memory


class BootloaderBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name == 'nt':
            compiler = ROOT / 'tools/windows_tools/gcc/xpack-arm-none-eabi-gcc-9.3.1-1.3-win32-x64/bin/arm-none-eabi-gcc.exe'
            make = Path('D:/cygwin64/bin/make.exe')
            cls.make = str(make) if make.is_file() else shutil.which('make')
        else:
            compiler = ROOT / 'tools/gcc/bin/arm-none-eabi-gcc'
            cls.make = shutil.which('make')
        if not compiler.is_file() or not cls.make:
            raise unittest.SkipTest('the packaged ARM GCC compiler and GNU make are required')
        (ROOT / 'output').mkdir(exist_ok=True)
        cls.directory = tempfile.TemporaryDirectory(prefix='bootloader-build-test-', dir=ROOT / 'output')
        cls.out = (Path(cls.directory.name) / 'nested' / 'bootloader').relative_to(ROOT).as_posix()
        cls.prefix = ROOT / cls.out / 'bl_nrf52811_xxaa_s112_8k'
        cls.run_make()

    @classmethod
    def tearDownClass(cls):
        target = Path(cls.directory.name).resolve()
        if (ROOT / 'output').resolve() not in target.parents:
            raise AssertionError('temporary build output escaped the workspace')
        cls.directory.cleanup()

    @classmethod
    def run_make(cls, *arguments):
        command = [cls.make, '-f', MAKEFILE, 'OUTPUT_DIRECTORY=' + cls.out,
                   'PASS_LINKER_INPUT_VIA_FILE=0', '-j2', *arguments]
        process = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
        if process.returncode:
            raise AssertionError(process.stdout)
        return process.stdout

    def test_binary_contains_only_the_flash_payload_and_hex_keeps_uicr(self):
        memory = read_hex(self.prefix.with_suffix('.hex'))
        flash = {address: value for address, value in memory.items() if address < 0x10000000}
        self.assertEqual(min(flash), 0x29000)
        self.assertLessEqual(max(flash) + 1, 0x2e000)
        expected = bytes(flash.get(address, 255) for address in range(min(flash), max(flash) + 1))
        self.assertEqual(self.prefix.with_suffix('.bin').read_bytes(), expected)
        self.assertLessEqual(len(expected), 20 * 1024)
        self.assertEqual(bytes(memory[0x10001014 + i] for i in range(8)),
                         struct.pack('<II', 0x29000, 0x2e000))
        self.assertEqual(struct.unpack_from('<I', expected)[0], 0x20006000)
        layout_offset = expected.index(b'BLD1')
        self.assertEqual(struct.unpack_from('<8I', expected, layout_offset),
                         (0x31444c42, 1, 0x19000, 0x29000, 8192, 0x2e000, 0x2f000, 3))

    def test_unchanged_build_keeps_objects_and_link_output(self):
        stamp = self.prefix.with_suffix('.out').stat().st_mtime_ns
        output = self.run_make()
        self.assertNotIn('Compiling file:', output)
        self.assertNotIn('Assembling file:', output)
        self.assertNotIn('Linking target:', output)
        self.assertEqual(self.prefix.with_suffix('.out').stat().st_mtime_ns, stamp)

    def test_changing_the_bootloader_makefile_recompiles_and_relinks(self):
        output = self.run_make('-W', MAKEFILE)
        self.assertGreater(output.count('Compiling file:'), 60)
        self.assertIn('Assembling file: gcc_startup_nrf52811.S', output)
        self.assertIn('Linking target:', output)

    def test_changing_the_linker_script_relinks_without_recompiling(self):
        output = self.run_make('-W', LINKER)
        self.assertNotIn('Compiling file:', output)
        self.assertNotIn('Assembling file:', output)
        self.assertIn('Linking target:', output)

    def test_changing_effective_flags_recompiles_and_preserves_the_payload(self):
        previous = self.prefix.with_suffix('.hex').read_bytes()
        output = self.run_make('OPT=-Os -g3 -flto -fno-jump-tables -fstack-usage')
        self.assertGreater(output.count('Compiling file:'), 60)
        self.assertIn('Linking target:', output)
        self.assertEqual(self.prefix.with_suffix('.hex').read_bytes(), previous)
        self.run_make()
        self.assertEqual(self.prefix.with_suffix('.hex').read_bytes(), previous)


if __name__ == '__main__':
    unittest.main()
