"""Tests for tools/check_irq_handlers.py (no ARM build required).

The checker guards the GCC 9.3.1 LTO trap where a `-Wl,-u`-forced IRQ handler
is internalized and the vector table falls back to Default_Handler. These tests
pin the acceptance/rejection rules on synthetic `nm --print-size` output.

Run with: python tests/test_irq_handler_check.py
"""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'check_irq_handlers', ROOT / 'tools/check_irq_handlers.py')
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)

STRONG_IMAGE = '\n'.join([
    '200 2 T Default_Handler',
    '154036 336 T GPIOTE_IRQHandler',
    '153780 20 T RTC1_IRQHandler',
    '156400 36 T WDT_IRQHandler',
    '156308 44 T SPIM0_SPIS0_SPI0_IRQHandler',
    '156352 48 T TWIM0_TWIS0_TWI0_SPIM1_SPIS1_SPI1_IRQHandler',
    '153604 4 T SWI2_IRQHandler',
])

BROKEN_IMAGE = '\n'.join([
    '200 2 T Default_Handler',
    '200 2 W GPIOTE_IRQHandler',
    '200 2 W RTC1_IRQHandler',
    '200 2 W WDT_IRQHandler',
    '200 2 W SPIM0_SPIS0_SPI0_IRQHandler',
    '200 2 W TWIM0_TWIS0_TWI0_SPIM1_SPIS1_SPI1_IRQHandler',
    '200 2 W SWI2_IRQHandler',
])


class IrqHandlerCheckTests(unittest.TestCase):
    def test_strong_handlers_pass(self):
        self.assertEqual(checker.verify(STRONG_IMAGE, forced=checker.FORCED_HANDLERS), [])

    def test_weak_aliases_are_rejected(self):
        errors = checker.verify(BROKEN_IMAGE, forced=checker.FORCED_HANDLERS)
        for name in checker.FORCED_HANDLERS:
            self.assertTrue(any(error.startswith(name + ':') for error in errors), name)
        self.assertGreaterEqual(len(errors), len(checker.FORCED_HANDLERS))

    def test_missing_handler_is_rejected(self):
        errors = checker.verify('200 2 T Default_Handler\n',
                                forced=('GPIOTE_IRQHandler',))
        self.assertEqual(errors, ['GPIOTE_IRQHandler: missing from the linked image'])

    def test_tiny_local_handler_is_rejected(self):
        image = ('200 2 T Default_Handler\n'
                 '153604 2 t SWI2_IRQHandler\n')
        errors = checker.verify(image, forced=('SWI2_IRQHandler',))
        self.assertIn('SWI2_IRQHandler: type t (expected strong T)', errors)
        self.assertIn('SWI2_IRQHandler: size 2 B (expected >= 4 B)', errors)


if __name__ == '__main__':
    unittest.main()
