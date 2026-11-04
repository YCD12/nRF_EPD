"""Verify that forced IRQ handlers survive LTO as strong code definitions.

With GCC 9.3.1 LTO, an IRQ handler that LDFLAGS keeps with `-Wl,-u` can be
internalized: the vector table then resolves to the startup file's weak
`Default_Handler` (`b .` infinite loop) and the first interrupt hangs the
device. This was measured on this project when nrfx_gpiote.c / drv_rtc.c /
nrfx_wdt.c / nrfx_spim.c / nrf_sdh.c were compiled with LTO.

Run after linking; exits non-zero when any forced handler is missing, weak, or
aliases Default_Handler.

Usage:
    python tools/check_irq_handlers.py --nm <arm-none-eabi-nm> \
        _build/nrf52811_xxaa.out
"""
import argparse
import subprocess
import sys

FORCED_HANDLERS = (
    'GPIOTE_IRQHandler',
    'RTC1_IRQHandler',
    'WDT_IRQHandler',
    'SPIM0_SPIS0_SPI0_IRQHandler',
    'TWIM0_TWIS0_TWI0_SPIM1_SPIS1_SPI1_IRQHandler',
    'SWI2_IRQHandler',
)

# A weak alias of Default_Handler is 2 bytes on Cortex-M (`b .`). Real handlers
# are at least a 4-byte branch/load, even for the SoftDevice event dispatcher.
MIN_STRONG_SIZE = 4


def parse_symbols(nm_output):
    """Return {name: (type, address, size)} from `nm --print-size --radix=d`."""
    symbols = {}
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            address = int(parts[0], 10)
        except ValueError:
            continue
        if len(parts) >= 4 and parts[1].isdigit():
            size = int(parts[1], 10)
            symbol_type, name = parts[2], parts[3]
        else:
            size = 0
            symbol_type, name = parts[1], parts[2]
        symbols[name] = (symbol_type, address, size)
    return symbols


def verify(nm_output, forced=FORCED_HANDLERS):
    """Return a list of human-readable errors (empty list means OK)."""
    symbols = parse_symbols(nm_output)
    default = symbols.get('Default_Handler')
    errors = []
    for name in forced:
        symbol = symbols.get(name)
        if symbol is None:
            errors.append(f'{name}: missing from the linked image')
            continue
        symbol_type, address, size = symbol
        if symbol_type != 'T':
            # W = weak alias, t = local, U = undefined
            errors.append(f'{name}: type {symbol_type} (expected strong T)')
        if size < MIN_STRONG_SIZE:
            errors.append(f'{name}: size {size} B (expected >= {MIN_STRONG_SIZE} B)')
        if default is not None and address == default[1]:
            errors.append(f'{name}: aliases Default_Handler at 0x{address:08x}')
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', help='linked ELF (.out) file')
    parser.add_argument('--nm', default='arm-none-eabi-nm',
                        help='path to arm-none-eabi-nm')
    args = parser.parse_args(argv)
    try:
        output = subprocess.run(
            [args.nm, '--print-size', '--radix=d', args.elf],
            check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        print(f'ERROR: cannot read symbols ({error})', file=sys.stderr)
        return 2
    errors = verify(output)
    if errors:
        print('ERROR: forced IRQ handlers are not strong definitions:',
              file=sys.stderr)
        for error in errors:
            print(f'  {error}', file=sys.stderr)
        print('This image would hang on the first interrupt. Check the '
              '-fno-lto exceptions and -Wl,-u list.', file=sys.stderr)
        return 1
    print(f'IRQ handlers: {len(FORCED_HANDLERS)} forced handlers are strong T')
    return 0


if __name__ == '__main__':
    sys.exit(main())
