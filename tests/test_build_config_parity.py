"""Guard production build-configuration parity between the GNU and Keil builds.

Both entry points ship the same 56 KiB nRF52811 application image, so they must
select the same production feature set. Historically the Keil application target
still enabled RTT logging and SystemView (which also changed main.c's idle loop
through USE_SYSVIEW and kept the CPU awake), and both build files defined
DEVELOP_IN_NRF52840 even though production hardware is a custom nRF52811 board,
not an image running on the nRF52840 development kit.

The GNU LTO exceptions are checked too: with GCC 9.3.1 an IRQ handler that the
linker keeps with -Wl,-u is internalized when its file is compiled with LTO and
the vector then resolves to the startup file's weak Default_Handler. Only the
files defining forced handlers may stay -fno-lto.

Run with: python tests/test_build_config_parity.py
"""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
KEIL = ROOT / 'Keil/EPD-nRF52.uvprojx'
KEIL_FLASH = ROOT / 'Keil/flash_softdevice.uvprojx'
MAKEFILE = ROOT / 'Makefile.nRF52_linux'
MAIN_C = ROOT / 'main.c'

HANDLER_LTO_EXCEPTIONS = (
    'drv_rtc.c',
    'nrf_sdh.c',
    'nrfx_wdt.c',
    'nrfx_spi.c',
    'nrfx_spim.c',
)
DFU_FILES_WITHOUT_EXCEPTION = (
    'ble_dfu.c',
    'ble_dfu_unbonded.c',
    'nrf_dfu_svci.c',
)


def _parse_keil_targets(path):
    root = ET.parse(path).getroot()
    targets = {}
    for target in root.findall('./Targets/Target'):
        name = target.findtext('./TargetName')
        cads = target.find('./TargetOption/TargetArmAds/Cads/VariousControls')
        aads = target.find('./TargetOption/TargetArmAds/Aads/VariousControls')
        ldads = target.find('./TargetOption/TargetArmAds/LDads')
        after_make = target.find('./TargetOption/TargetCommonOption/AfterMake')
        groups = [g.text for g in target.findall('./Groups/Group/GroupName')]
        files = [f.text for f in target.findall('./Groups/Group/Files/File/FileName')]
        raw_files = [f.text for f in target.findall('./Groups/Group/Files/File/FilePath')]
        targets[name] = {
            'define': cads.findtext('Define') or '',
            'cads_misc': cads.findtext('MiscControls') or '',
            'asm_define': aads.findtext('Define') or '',
            'asm_misc': aads.findtext('MiscControls') or '',
            'groups': groups,
            'files': files,
            'raw_files': raw_files,
            'uAC6': target.findtext('./uAC6') or '',
            'pCCUsed': target.findtext('./pCCUsed') or '',
            'v6Lto': target.findtext('./TargetOption/TargetArmAds/Cads/v6Lto') or '',
            'scatter': ldads.findtext('ScatterFile') or '',
            'umfTarg': ldads.findtext('umfTarg') or '',
            'ld_misc': ldads.findtext('Misc') or '',
            'after_run1': after_make.findtext('RunUserProg1') if after_make is not None else '',
            'after_prog1': after_make.findtext('UserProg1Name') if after_make is not None else '',
        }
    return targets


def keil_targets():
    """App and SoftDevice targets live in separate single-target projects.

    They were split apart because uVision merged the two targets of the old
    two-target project back into both lists when an old session saved it, which
    produced duplicate _1.o objects and AC6 errors on app_error_handler_keil.c.
    """
    targets = _parse_keil_targets(KEIL)
    if KEIL_FLASH.is_file():
        targets.update(_parse_keil_targets(KEIL_FLASH))
    return targets


class BuildConfigParity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.targets = keil_targets()
        cls.makefile = MAKEFILE.read_text(encoding='utf-8')
        cls.main_c = MAIN_C.read_text(encoding='utf-8')

    def test_keil_application_target_uses_release_configuration(self):
        app = self.targets['nRF52811_xxAA']
        for field in ('define', 'asm_define', 'asm_misc'):
            self.assertNotIn('USE_SYSVIEW', app[field], field)
            self.assertNotIn('NRF_LOG_ENABLED=1', app[field], field)
        self.assertIn('NRF_LOG_ENABLED=0', app['define'])
        # The app target is built with ARM Compiler 6, whose assembler options are
        # no longer passed through ARMCC's --cpreproc_opts asm_misc list.  The
        # product defines for the startup assembly live in asm_define instead.
        self.assertIn('NRF_LOG_ENABLED=0', app['asm_define'])
        # Debug-only source groups must not stay in the production app target.
        for name in ('nRF_Log', 'nRF_Segger_RTT', 'SystemView'):
            self.assertNotIn(name, app['groups'])
        for name in ('nrf_log_frontend.c', 'SEGGER_RTT.c', 'SEGGER_SYSVIEW.c'):
            self.assertNotIn(name, app['files'])

    def test_no_develop_in_nrf52840_in_either_build(self):
        for name, target in self.targets.items():
            for field in ('define', 'asm_define', 'asm_misc'):
                self.assertFalse(
                    re.search(r'\bDEVELOP_IN_NRF52840\b', target[field]),
                    f'{name}.{field} still enables DEVELOP_IN_NRF52840')
        # The Makefile mentions the define in explanatory comments; only an
        # actual CFLAGS/ASMFLAGS assignment changes the build.
        self.assertFalse(
            re.search(r'(?m)^(?:CFLAGS|ASMFLAGS) \+= -DDEVELOP_IN_NRF52840\b',
                      self.makefile),
            'Makefile.nRF52_linux still compiles with DEVELOP_IN_NRF52840')

    def test_internal_storage_disabled_in_both_builds(self):
        for name, target in self.targets.items():
            # ARMCC5 carried the asm preprocessor defines in asm_misc, while
            # AC6 uses asm_define; check the union so both target styles work.
            asm_blob = '\n'.join((target['asm_define'], target['asm_misc']))
            for field in ('define', 'asm_define'):
                self.assertIn('FDS_ENABLED=0', target[field], (name, field))
                self.assertIn('NRF_FSTORAGE_ENABLED=0', target[field], (name, field))
            self.assertIn('FDS_ENABLED=0', asm_blob, (name, 'asm'))
            self.assertIn('NRF_FSTORAGE_ENABLED=0', asm_blob, (name, 'asm'))
        self.assertIn('-DFDS_ENABLED=0 -DNRF_FSTORAGE_ENABLED=0', self.makefile)

    def test_keil_targets_have_no_duplicated_groups_or_files(self):
        """Guard against uVision merging both targets back into one file list.

        A corrupted project (historically caused by an old uVision session saving
        the project) makes the app target compile nrf_sdh/startup twice, which
        triggers L6235E and AC6 errors on app_error_handler_keil.c.
        """
        expected_groups = {
            'nRF52811_xxAA': [
                'App', 'EPD', 'GUI', 'nRF_BLE', 'nRF_DFU', 'nRF_Drivers',
                'nRF_Libraries', 'nRF_SoftDevice', 'Startup',
            ],
            'flash_softdevice': ['App'],
        }
        for name, target in self.targets.items():
            self.assertEqual(target['groups'], expected_groups[name], name)
            self.assertEqual(len(target['raw_files']), len(set(target['raw_files'])),
                             f'{name} has duplicated FilePath entries')
            if name == 'nRF52811_xxAA':
                self.assertNotIn('app_error_handler_keil.c',
                                 '\n'.join(target['raw_files']))
            else:
                self.assertNotIn('app_error_handler_gcc.c',
                                 '\n'.join(target['raw_files']))

    def test_keil_projects_are_single_target(self):
        """Split projects prevent uVision from merging target file lists.

        The SoftDevice staging project is optional; when present it must also be
        a single-target project.
        """
        self.assertEqual(len(_parse_keil_targets(KEIL)), 1)
        if KEIL_FLASH.is_file():
            self.assertEqual(len(_parse_keil_targets(KEIL_FLASH)), 1)

    def test_keil_application_target_uses_arm_compiler_6_with_lto(self):
        """The app target no longer fits with ARMCC5 (58.9 KiB > 56 KiB)."""
        app = self.targets['nRF52811_xxAA']
        self.assertEqual(app['uAC6'], '1')
        self.assertIn('V6.11', app['pCCUsed'])
        self.assertEqual(app['v6Lto'], '1')
        self.assertEqual(app['umfTarg'], '0')
        self.assertIn('EPD-nRF52-ac6.sct', app['scatter'])
        self.assertIn('ac6_gnu_symbols.c', app['files'])
        self.assertIn('app_error_handler_gcc.c', app['files'])
        self.assertTrue((ROOT / 'Keil/EPD-nRF52-ac6.sct').is_file())
        self.assertTrue((ROOT / 'Keil/ac6_gnu_symbols.c').is_file())
        # The app project's After Make hook generates the full HEX and OTA ZIP
        # from the Keil application HEX (same artifacts as build.bat build/ota).
        self.assertEqual(app['after_run1'], '1')
        self.assertIn('keil-postbuild.bat', app['after_prog1'])
        self.assertTrue((ROOT / 'tools/keil-postbuild.bat').is_file())
        self.assertTrue((ROOT / 'tools/windows_tools/nrfutil.exe').is_file())
        self.assertTrue((ROOT / 'tools/windows_tools/mergehex.exe').is_file())
        # The SoftDevice staging project is optional: it may be removed when
        # SoftDevice flashing is handled by build.bat flash_softdevice instead.
        if KEIL_FLASH.is_file():
            softdevice = self.targets['flash_softdevice']
            self.assertEqual(softdevice['uAC6'], '0')
            self.assertIn('stage-softdevice-nrf52.bat', softdevice['after_prog1'])

    def test_gcc_lto_exceptions_only_for_forced_irq_handlers(self):
        rules = set(re.findall(
            r'\$\(OUTPUT_DIRECTORY\)/nrf52811_xxaa/([\w.]+?\.c)\.o: CFLAGS \+= -fno-lto',
            self.makefile))
        self.assertEqual(rules, set(HANDLER_LTO_EXCEPTIONS))
        for name in DFU_FILES_WITHOUT_EXCEPTION:
            self.assertNotIn(name, rules)

    def test_direct_wakeup_gpio_replaces_nrfx_gpiote_driver(self):
        self.assertNotIn('nrfx_gpiote.c', self.makefile)
        self.assertNotIn('nrf_drv_gpiote', self.main_c)
        self.assertIn('GPIOTE_IRQHandler', self.main_c)
        self.assertIn('nrf_gpio_cfg_sense_set', self.main_c)


if __name__ == '__main__':
    unittest.main()
