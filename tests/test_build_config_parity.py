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


def keil_targets():
    root = ET.parse(KEIL).getroot()
    targets = {}
    for target in root.findall('./Targets/Target'):
        name = target.findtext('./TargetName')
        cads = target.find('./TargetOption/TargetArmAds/Cads/VariousControls')
        aads = target.find('./TargetOption/TargetArmAds/Aads/VariousControls')
        groups = [g.text for g in target.findall('./Groups/Group/GroupName')]
        files = [f.text for f in target.findall('./Groups/Group/Files/File/FileName')]
        targets[name] = {
            'define': cads.findtext('Define') or '',
            'cads_misc': cads.findtext('MiscControls') or '',
            'asm_define': aads.findtext('Define') or '',
            'asm_misc': aads.findtext('MiscControls') or '',
            'groups': groups,
            'files': files,
        }
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
        self.assertIn('-DNRF_LOG_ENABLED=0', app['asm_misc'])
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
            for field in ('define', 'asm_define', 'asm_misc'):
                self.assertIn('FDS_ENABLED=0', target[field], (name, field))
                self.assertIn('NRF_FSTORAGE_ENABLED=0', target[field], (name, field))
        self.assertIn('-DFDS_ENABLED=0 -DNRF_FSTORAGE_ENABLED=0', self.makefile)

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
