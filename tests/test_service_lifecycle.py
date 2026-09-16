"""Compile real service handlers with host hardware shims; no SDK/BLE emulator.

Extraction keeps the production control flow in the test while replacing only
hardware and service layout. Generated files live beside the other host tests.
"""
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    import re
    match = re.search(r"(?:static )?(?:void|bool|size_t) " + name + r"\([^;]+?\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


source = (ROOT / 'EPD/EPD_service.c').read_text(encoding='utf-8')
handlers = '\n'.join(function(source, name) for name in (
    'epd_append_char', 'epd_append_uint', 'epd_make_prefixed_uint',
    'epd_send_prefixed_uint', 'epd_send_mtu', 'epd_send_time', 'epd_send_voltage',
    'on_connect', 'on_disconnect', 'cmd_init', 'epd_init_process',
    'epd_connection_process', 'cmd_reset_transfer', 'epd_config_pins_valid', 'cmd_set_pins',
    'epd_process_next_command'))
start = source.index('finish_refresh:') + len('finish_refresh:')
end = source.index('\n}\n', start)
finish = 'static void finish_refresh(ble_epd_t *p_epd) {\n' + '''
    const epd_model_t *epd = p_epd->epd;
    display_mode_t mode = p_epd->config.display_mode;
    bool critical_battery = false;
''' + source[start:end] + '\n}\n'
pin = function((ROOT / 'EPD/EPD_driver.c').read_text(encoding='utf-8'), 'pinMode')
fixture = (ROOT / 'tests/service_lifecycle_fixture.c').read_text(encoding='utf-8')
out = Path(os.environ.get('HOST_TEST_OUT', ROOT / '_build/host_tests'))
out.mkdir(parents=True, exist_ok=True)
generated = out / 'service_lifecycle_generated.c'
generated.write_text(fixture.replace('/* PRODUCTION_HANDLERS */', handlers + finish + pin), encoding='utf-8')
exe = out / ('test_service_lifecycle.exe' if os.name == 'nt' else 'test_service_lifecycle')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-Itests/font_transfer_stubs', '-IEPD', '-IGUI', str(generated), '-o', str(exe)],
               cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)
