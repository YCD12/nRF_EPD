"""Run the production wall clock against a simulated 24-bit RTC.

This validates tick accounting and configured ppm arithmetic, not the physical
oscillator. Every sample gap stays below one RTC wrap, as required by main.c.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"static void " + name + r"\([^;]+?\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


source = (ROOT / "main.c").read_text(encoding="utf-8")
constants = source[source.index("#define APP_CLOCK_CALIBRATION_PPM"):
                   source.index("uint32_t get_reset_reason")]
ppm_definition = re.search(r"^#define APP_CLOCK_CALIBRATION_PPM[^\r\n]+", constants,
                           re.MULTILINE)
assert ppm_definition
clock = source[source.index("static volatile uint32_t m_uptime_seconds"):
               source.index("// reload the wdt channel")]
handlers = "\n".join(function(source, name) for name in (
    "clock_timer_timeout_handler", "application_timers_start"))
timestamp_state = re.search(r"static uint32_t\s+m_timestamp\s*=\s*\d+;", source)
assert timestamp_state
timer_interval = re.search(r"^#define CLOCK_TIMER_INTERVAL\s+[^\r\n]+", source,
                           re.MULTILINE)
assert timer_interval
fixture = (ROOT / "tests/clock_timekeeping_fixture.c").read_text(encoding="utf-8")
production = "\n".join((timestamp_state.group(0), constants,
                         timer_interval.group(0), clock, handlers))
out = Path(os.environ.get("HOST_TEST_OUT", ROOT / "_build/host_tests"))
out.mkdir(parents=True, exist_ok=True)
generated = out / "clock_timekeeping_generated.c"
def write_fixture(ppm):
    # The firmware uses one source macro; simulate editing it for each test case.
    configured = production if ppm is None else production.replace(
        ppm_definition.group(0), f"#define APP_CLOCK_CALIBRATION_PPM ({ppm})", 1)
    generated.write_text(fixture.replace("/* PRODUCTION_CLOCK */", configured), encoding="utf-8")

compiler = shlex.split(os.environ.get("HOST_CC", "gcc")) + [
    "-Wall", "-Werror", "-std=c11", "-I.", "-ISDK/17.1.0_ddde560"]


def run_case(prescaler, ppm):
    # None exercises the unchanged source configuration; other cases edit its macro.
    expected_ppm = -298 if ppm is None else ppm
    suffix = "default" if ppm is None else str(ppm)
    exe = out / (f"test_clock_timekeeping_{prescaler}_{suffix}" +
                 (".exe" if os.name == "nt" else ""))
    definitions = [f"-DAPP_TIMER_CONFIG_RTC_FREQUENCY={prescaler}",
                   f"-DEXPECTED_CALIBRATION_PPM={expected_ppm}"]
    write_fixture(ppm)
    subprocess.run(compiler + definitions + [str(generated), "-o", str(exe)],
                   cwd=ROOT, check=True)
    subprocess.run([str(exe.resolve())], check=True)


for prescaler in (0, 1, 31):
    for ppm in (None, 0, 298, -2000, 2000):
        run_case(prescaler, ppm)

for ppm in (-2001, 2001):
    write_fixture(ppm)
    rejected = subprocess.run(compiler + [f"-DEXPECTED_CALIBRATION_PPM={ppm}",
                              "-fsyntax-only", str(generated)],
                              cwd=ROOT, capture_output=True, text=True)
    assert rejected.returncode != 0, f"out-of-range calibration accepted: {ppm}"
    assert "APP_CLOCK_CALIBRATION_PPM must be between" in rejected.stderr, rejected.stderr
print("PASS: RC clock calibration rejects values outside +/-2000 ppm")
