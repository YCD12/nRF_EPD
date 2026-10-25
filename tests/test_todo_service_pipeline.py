"""Exercise real resource replies, mode replies and sleeping TODO GUI workers."""
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"(?:static )?(?:void|bool|size_t|uint32_t|rs_result_t) " + name + r"\([^;]+?\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


source = (ROOT / 'EPD/EPD_service.c').read_text(encoding='utf-8')
retry_state = re.search(r'static volatile uint32_t s_display_retry_at;\s*'
                        r'static volatile uint8_t s_display_retry_step;', source).group(0)
handlers = retry_state + '\n' + '\n'.join(function(source, name) for name in (
    'epd_display_retry_reset', 'epd_display_retry_failed',
    'epd_display_request', 'epd_display_complete',
    'epd_perform_full_refresh', 'epd_gui_update',
    'epd_parse_time_payload', 'epd_config_pins_valid', 'epd_config_validate',
    'epd_config_apply', 'epd_update_display_mode', 'epd_switch_from_clock_mode',
    'epd_command_ack', 'epd_todo_font_valid', 'cmd_set_mode',
    'epd_append_char', 'epd_append_uint', 'epd_make_prefixed_uint',
    'epd_control_notifications_process', 'epd_resource_event', 'ble_epd_on_timer'))
fixture = (ROOT / 'tests/todo_service_pipeline_fixture.c').read_text(encoding='utf-8')
out = Path(os.environ.get('HOST_TEST_OUT', ROOT / '_build/host_tests'))
out.mkdir(parents=True, exist_ok=True)
generated = out / 'todo_service_pipeline_generated.c'
generated.write_text(fixture.replace('/* PRODUCTION_HANDLERS */', handlers), encoding='utf-8')
exe = out / ('test_todo_service_pipeline.exe' if os.name == 'nt' else 'test_todo_service_pipeline')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-Itests/font_transfer_stubs',
                '-Itests/stubs', '-IEPD', '-IGUI', str(generated),
                'EPD/resource_transfer.c', 'EPD/epd_refresh_policy.c',
                'tests/stubs/ui_resources_stub.c', 'GUI/todo_items.c', 'GUI/font_geometry.c', '-o', str(exe)], cwd=ROOT, check=True)
for mode in (4, 5):
    subprocess.run([str(exe.resolve()), str(mode)], check=True)
