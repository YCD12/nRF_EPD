"""Compile real service handlers with host hardware shims; no SDK/BLE emulator.

Extraction keeps the production control flow in the test while replacing only
hardware and service layout. Generated files live beside the other host tests.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"(?:static )?(?:void|bool|size_t|uint16_t|uint32_t|rs_result_t) " + name + r"\([^;]+?\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


source = (ROOT / 'EPD/EPD_service.c').read_text(encoding='utf-8')
retry_reset = ('static volatile uint8_t s_display_retry_step;\n' +
               function(source, 'epd_display_retry_reset') + '\n')
handlers = retry_reset + '\n'.join(function(source, name) for name in (
    'epd_append_char', 'epd_append_uint', 'epd_make_prefixed_uint',
    'epd_make_mtu_string',
    'epd_send_prefixed_uint', 'epd_send_mtu', 'epd_send_time', 'epd_send_voltage',
    'on_connect', 'on_disconnect', 'cmd_init', 'epd_init_process',
    'epd_connection_process', 'cmd_reset_transfer', 'epd_config_pins_valid',
    'epd_config_validate', 'epd_config_apply', 'cmd_set_pins',
    'epd_process_next_command', 'resource_notification_send', 'epd_todo_font_valid', 'epd_resource_event',
    # S-1/S-3: 图片模式主循环入口 + 0xA0/0xA1 单槽重发
    'epd_bulk_reply_send', 'crc16_compute', 'send_block_response',
    'send_transfer_status', 'epd_bulk_reply_process',
    'epd_ensure_display', 'epd_update_display_mode', 'epd_switch_from_clock_mode',
    'epd_enter_picture_mode', 'epd_picture_entry_process', 'cmd_write_block'))
start = source.index('finish_refresh:') + len('finish_refresh:')
end = source.index('\n}\n', start)
finish = 'static void finish_refresh(ble_epd_t *p_epd) {\n' + '''
    const epd_model_t *epd = p_epd->epd;
    display_mode_t mode = p_epd->config.display_mode;
    bool critical_battery = false;
    bool refresh_succeeded = true;
''' + source[start:end] + '\n}\n'
pin = function((ROOT / 'EPD/EPD_driver.c').read_text(encoding='utf-8'), 'pinMode')
fixture = (ROOT / 'tests/service_lifecycle_fixture.c').read_text(encoding='utf-8')
out = Path(os.environ.get('HOST_TEST_OUT', ROOT / '_build/host_tests'))
out.mkdir(parents=True, exist_ok=True)
generated = out / 'service_lifecycle_generated.c'
generated.write_text(fixture.replace('/* PRODUCTION_HANDLERS */', handlers + finish + pin), encoding='utf-8')
exe = out / ('test_service_lifecycle.exe' if os.name == 'nt' else 'test_service_lifecycle')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-Itests/font_transfer_stubs', '-IEPD', '-IGUI', str(generated),
                'EPD/epd_rle.c', '-o', str(exe)],
               cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)

service_header = (ROOT / 'EPD/EPD_service.h').read_text(encoding='utf-8')
service_types = '\n'.join(re.search(
    r'typedef struct \{[^{}]+\} ' + name + r';', service_header).group(0)
    for name in ('image_transfer_ctx_t', 'ble_epd_t'))
init_handlers = re.search(r'^#define EPD_CFG_52811 .+$', source, re.MULTILINE).group(0)
init_handlers += '\n' + function(
    (ROOT / 'EPD/calendar_annotations_tx.c').read_text(encoding='utf-8'),
    'calendar_annotations_tx_cancel')
init_handlers += '\n' + '\n'.join(function(source, name) for name in (
    'epd_append_char', 'epd_append_uint', 'epd_make_prefixed_uint',
    'epd_make_mtu_string',
    'epd_config_pins_valid', 'epd_config_validate', 'epd_config_defaults', 'ble_epd_init',
    'ble_epd_notification_try_send', 'ble_epd_string_send',
    'resource_notification_send', 'epd_control_notifications_process'))
fixture = (ROOT / 'tests/service_init_notify_fixture.c').read_text(encoding='utf-8')
generated = out / 'service_init_notify_generated.c'
generated.write_text(fixture.replace('/* PRODUCTION_SERVICE_TYPES */', service_types)
                     .replace('/* PRODUCTION_HANDLERS */', init_handlers), encoding='utf-8')
exe = out / ('test_service_init_notify.exe' if os.name == 'nt' else 'test_service_init_notify')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-Itests/font_transfer_stubs', '-IEPD',
                str(generated), '-o', str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)

config_handlers = re.search(r'^#define EPD_CFG_52811 .+$', source, re.MULTILINE).group(0) + '\n'
config_handlers += retry_reset + '\n'.join(function(source, name) for name in (
    'epd_config_pins_valid', 'epd_config_validate', 'epd_config_defaults', 'epd_config_apply',
    'epd_resource_idle', 'epd_config_restore', 'epd_update_display_mode', 'cmd_set_week_start',
    'cmd_set_pins', 'cmd_set_config', 'epd_todo_font_valid', 'epd_resource_event'))
fixture = (ROOT / 'tests/service_config_fixture.c').read_text(encoding='utf-8')
generated = out / 'service_config_generated.c'
generated.write_text(fixture.replace('/* PRODUCTION_SERVICE_TYPES */', service_types)
                     .replace('/* PRODUCTION_HANDLERS */', config_handlers), encoding='utf-8')
exe = out / ('test_service_config.exe' if os.name == 'nt' else 'test_service_config')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-Itests/font_transfer_stubs', '-IEPD', '-IGUI',
                str(generated), '-o', str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)

refresh_handlers = ('static volatile uint32_t s_display_retry_at;\n' + retry_reset +
                    '\n'.join(function(source, name) for name in (
    'epd_display_retry_failed',
    'epd_parse_time_payload', 'epd_display_request', 'epd_display_complete',
    'epd_update_display_mode', 'epd_switch_from_clock_mode', 'epd_command_ack',
    'epd_todo_font_valid', 'cmd_set_mode', 'ble_epd_on_timer', 'epd_gui_update',
    'epd_battery_process')))
refresh_fixture = (ROOT / 'tests/connected_refresh_fixture.c').read_text(encoding='utf-8')
generated = out / 'connected_refresh_generated.c'
generated.write_text(refresh_fixture.replace('/* PRODUCTION_HANDLERS */', refresh_handlers), encoding='utf-8')
exe = out / ('test_connected_refresh.exe' if os.name == 'nt' else 'test_connected_refresh')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-Itests/font_transfer_stubs', '-IEPD', '-IGUI',
                str(generated), 'EPD/epd_refresh_policy.c', '-o', str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)

display = function(source, 'epd_command_ack') + '\n' + function(source, 'cmd_display')
fixture = (ROOT / 'tests/display_completion_fixture.c').read_text(encoding='utf-8')
generated = out / 'display_completion_generated.c'
generated.write_text(fixture.replace('/* PRODUCTION_HANDLERS */', display), encoding='utf-8')
exe = out / ('test_display_completion.exe' if os.name == 'nt' else 'test_display_completion')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-IEPD', str(generated), '-o', str(exe)],
               cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)

ingress = function(source, 'epd_command_ack') + '\n' + function(source, 'epd_service_on_write')
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
    'advertising_start', 'gpiote_disable_wakeup', 'GPIOTE_IRQHandler',
    'setup_wakeup_pin', 'disconnect_for_run', 'on_adv_evt'))
radio_fixture = (ROOT / 'tests/offline_radio_fixture.c').read_text(encoding='utf-8')
generated = out / 'offline_radio_generated.c'
generated.write_text(radio_fixture.replace('/* PRODUCTION_HANDLERS */', radio_handlers), encoding='utf-8')
exe = out / ('test_offline_radio.exe' if os.name == 'nt' else 'test_offline_radio')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-IEPD', '-IGUI', str(generated), '-o', str(exe)],
               cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)
