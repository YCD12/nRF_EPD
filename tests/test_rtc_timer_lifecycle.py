"""Exercise SDK app_timer2 scheduling with the project's RTC keep-active disabled.

The timer engine and sorted list are extracted unchanged from production files.
Only the RTC registers, interrupt request FIFO, and logging/critical-section
primitives are host shims. This checks software scheduling, not oscillator drift.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "SDK/17.1.0_ddde560"


def function(source, name):
    match = re.search(r"^(?:static )?(?:inline )?[^\n;{}]+\b" + name +
                      r"\([^;]+?\{", source, re.MULTILINE)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


source = (SDK / "components/libraries/timer/app_timer2.c").read_text(encoding="utf-8")
sortlist = (SDK / "components/libraries/sortlist/nrf_sortlist.c").read_text(encoding="utf-8")
request_types = source[source.index("typedef enum"):
                       source.index("static app_timer_t * volatile mp_active_timer")]
functions = "\n".join(function(sortlist, name) for name in (
    "nrf_sortlist_add", "nrf_sortlist_pop", "nrf_sortlist_peek", "nrf_sortlist_remove"))
functions += "\n" + "\n".join(function(source, name) for name in (
    "get_now", "compare_func", "timer_expire", "rtc_schedule", "sortlist_pop",
    "sortlist_peek", "sorted_list_stop_all", "on_compare_evt", "rtc_update",
    "timer_req_process", "app_timer_start", "app_timer_stop",
    "app_timer_cnt_diff_compute", "app_timer_cnt_get"))
fixture = (ROOT / "tests/rtc_timer_lifecycle_fixture.c").read_text(encoding="utf-8")
out = Path(os.environ.get("HOST_TEST_OUT", ROOT / "_build/host_tests"))
out.mkdir(parents=True, exist_ok=True)
generated = out / "rtc_timer_lifecycle_generated.c"
generated.write_text(fixture.replace("/* PRODUCTION_REQUEST_TYPES */", request_types)
                     .replace("/* PRODUCTION_TIMER_ENGINE */", functions), encoding="utf-8")
exe = out / ("test_rtc_timer_lifecycle.exe" if os.name == "nt" else "test_rtc_timer_lifecycle")
subprocess.run(shlex.split(os.environ.get("HOST_CC", "gcc")) +
               ["-Wall", "-Werror", "-std=c11", "-I" + str(SDK),
                "-I" + str(SDK / "components/libraries/sortlist"),
                str(generated), "-o", str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)
