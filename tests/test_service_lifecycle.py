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
    match = re.search(r"(?:static )?(?:void|bool|size_t|uint32_t|rs_result_t) " + name + r"\([^;]+?\{", source)
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
    'epd_process_next_command', 'resource_notification_send', 'epd_resource_event'))
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

refresh_handlers = '\n'.join(function(source, name) for name in (
    'epd_display_request', 'epd_display_complete', 'epd_update_display_mode',
    'epd_switch_from_clock_mode', 'cmd_set_mode', 'ble_epd_on_timer', 'epd_gui_update'))
refresh_fixture = (ROOT / 'tests/connected_refresh_fixture.c').read_text(encoding='utf-8')
generated = out / 'connected_refresh_generated.c'
generated.write_text(refresh_fixture.replace('/* PRODUCTION_HANDLERS */', refresh_handlers), encoding='utf-8')
exe = out / ('test_connected_refresh.exe' if os.name == 'nt' else 'test_connected_refresh')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-Itests/font_transfer_stubs', '-IEPD', '-IGUI',
                str(generated), 'EPD/epd_refresh_policy.c', '-o', str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)

ingress = function(source, 'epd_service_on_write')
fixture = (ROOT / 'tests/command_ingress_fixture.c').read_text(encoding='utf-8')
generated = out / 'command_ingress_generated.c'
generated.write_text(fixture.replace('/* PRODUCTION_HANDLERS */', ingress), encoding='utf-8')
exe = out / ('test_command_ingress.exe' if os.name == 'nt' else 'test_command_ingress')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-Itests/font_transfer_stubs', '-IEPD',
                str(generated), '-o', str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)

radio_source = (ROOT / 'main.c').read_text(encoding='utf-8')
radio_handlers = '\n'.join(function(radio_source, name) for name in (
    'advertising_start', 'gpiote_evt_handler', 'setup_wakeup_pin', 'disconnect_for_run', 'on_adv_evt'))
radio_fixture = (ROOT / 'tests/offline_radio_fixture.c').read_text(encoding='utf-8')
generated = out / 'offline_radio_generated.c'
generated.write_text(radio_fixture.replace('/* PRODUCTION_HANDLERS */', radio_handlers), encoding='utf-8')
exe = out / ('test_offline_radio.exe' if os.name == 'nt' else 'test_offline_radio')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-IEPD', '-IGUI', str(generated), '-o', str(exe)],
               cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)
