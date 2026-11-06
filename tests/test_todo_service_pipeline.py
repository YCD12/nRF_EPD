"""Exercise real resource replies, mode replies and sleeping TODO GUI workers."""
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]

# Functions declared in EPD_service.h keep external linkage in generated fixtures.
PUBLIC_SERVICE_API = {'ble_epd_on_timer'}


# The split routes queue scheduling through two size-tuned helpers (see
# epd_service_internal.h); expand them back to the fixture-visible calls.
SCHEDULE_HELPERS = (
    ('epd_service_schedule_cmd(p_epd)',
     'epd_cmd_queue_schedule(&p_epd->cmd_queue, p_epd, epd_process_next_command)'),
    ('epd_service_schedule_display(p_epd)',
     'epd_display_queue_schedule(&p_epd->display_queue, p_epd, epd_gui_update)'),
)


def function(source, name):
    """Extract a production function and normalise it to `static` linkage."""
    match = re.search(r"(?:static )?(?:void|bool|size_t|uint32_t|rs_result_t) " + name + r"\([^;]+?\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    text = source[match.start():end]
    for helper, expanded in SCHEDULE_HELPERS:
        text = text.replace(helper, expanded)
    return text if (name in PUBLIC_SERVICE_API or text.startswith('static ')) else 'static ' + text


# Five service modules, concatenated in dependency (definition) order.
SERVICE_SOURCES = (
    'EPD/service/epd_service_control.c',
    'EPD/service/epd_service_transfer.c',
    'EPD/service/epd_service_display.c',
    'EPD/service/epd_service_cmd.c',
    'EPD/service/EPD_service.c',
)
source = '\n'.join((ROOT / path).read_text(encoding='utf-8') for path in SERVICE_SOURCES)
retry_state = re.search(r'static volatile uint32_t s_display_retry_at;\s*'
                        r'static volatile uint8_t s_display_retry_step;', source).group(0)
handlers = retry_state + '\n' + '\n'.join(function(source, name) for name in (
    'epd_display_retry_reset', 'epd_display_retry_failed',
    'epd_display_retry_scheduled', 'epd_display_retry_waiting',
    'epd_display_request', 'epd_display_complete',
    'epd_perform_full_refresh', 'epd_gui_update',
    'epd_parse_time_payload', 'epd_config_pins_valid', 'epd_config_validate',
    'epd_config_apply', 'epd_update_display_mode', 'epd_switch_from_clock_mode',
    'epd_command_ack', 'epd_todo_font_valid', 'cmd_set_mode',
    'epd_append_char', 'epd_append_uint', 'epd_make_prefixed_uint',
    'epd_make_mtu_string',  # initial_notify stage2 "mtu=<n> rle=1"
    'epd_control_notifications_process', 'epd_resource_event', 'ble_epd_on_timer'))
fixture = (ROOT / 'tests/todo_service_pipeline_fixture.c').read_text(encoding='utf-8')
out = Path(os.environ.get('HOST_TEST_OUT', ROOT / '_build/host_tests'))
out.mkdir(parents=True, exist_ok=True)
generated = out / 'todo_service_pipeline_generated.c'
generated.write_text(fixture.replace('/* PRODUCTION_HANDLERS */', handlers), encoding='utf-8')
exe = out / ('test_todo_service_pipeline.exe' if os.name == 'nt' else 'test_todo_service_pipeline')
subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) +
               ['-Wall', '-Werror', '-std=c11', '-Itests/font_transfer_stubs',
                '-Itests/stubs', '-I.', '-IEPD/hal', '-IEPD/driver', '-IEPD/board', '-IEPD/storage', '-IEPD/app', '-IEPD/service', '-IGUI', str(generated),
                'EPD/service/resource_transfer.c', 'EPD/storage/resource_crc.c',
                'EPD/app/epd_refresh_policy.c',
                'tests/stubs/ui_resources_stub.c', 'GUI/todo_items.c', 'GUI/font_geometry.c', '-o', str(exe)], cwd=ROOT, check=True)
for mode in (4, 5):
    subprocess.run([str(exe.resolve()), str(mode)], check=True)
