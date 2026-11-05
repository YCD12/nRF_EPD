"""Compare production page culling with an independent, uncullled render.

Only the high-level page intersection predicate is removed from the reference;
GFX still rasterizes/clips every pixel into the usual small page buffer. Both
versions therefore use identical draw order and no separate expected layout.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get("HOST_TEST_OUT", "_build/host_tests")) / "gui_layout"
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / "GUI/GUI.c").read_text(encoding="utf-8")
source, count = re.subn(
    r"static bool gui_page_hits\([^)]*\)\s*\{[^}]*\}",
    "static bool gui_page_hits(const Adafruit_GFX *gfx, int32_t top, int32_t bottom)\n"
    "{ (void)gfx; (void)top; (void)bottom; return true; }",
    source,
    count=1,
)
assert count == 1, "Reference must disable exactly one page predicate"
exports = ("DrawGUI", "DrawGUI_ClockOnly", "GetTimeRefreshArea", "GetTimeRefreshAreaFromLocal",
           "GUI_TodoRenderValid", "GUI_RenderValid")
reference = OUT / "GUI_unculled.c"
reference.write_text("".join(f"#define {name} {name}_Unclipped\n" for name in exports) + source,
                     encoding="utf-8")
common = ["-DGUI_LAYOUT_REFERENCE", "-IGUI", '-I.', '-IEPD/hal', '-IEPD/driver', '-IEPD/board', '-IEPD/storage', '-IEPD/app', '-IEPD/service', "-Itests/stubs", "-Itools/test/host/shim",
          "GUI/GUI.c", str(reference), "GUI/Adafruit_GFX.c", "GUI/u8g2_font.c", "GUI/Lunar.c",
          "GUI/calendar_annotations.c", "GUI/todo_items.c", "GUI/font_geometry.c", "tests/stubs/todo_resources_stub.c",
          "tests/stubs/calendar_resources_stub.c",
          "tests/stubs/ui_resources_stub.c", "EPD/app/epd_clock_refresh.c", "tests/test_gui_render.c"]
cc = shlex.split(os.environ.get("HOST_CC", "gcc"))
flags = shlex.split(os.environ.get("HOST_CFLAGS", "-Wall -Werror -std=c11"))
for fixed in (False, True):
    options = (["-DGFX_FIXED_BWR_PANEL=1", "-DGFX_FIXED_FONT_DIRECTION_0=1",
                "-DGUI_FIXED_PANEL_WIDTH=800", "-DGUI_FIXED_PANEL_HEIGHT=480"] if fixed else [])
    binary = OUT / ("fixed.exe" if fixed else "generic.exe")
    subprocess.run(cc + flags + options + common + ["-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary.resolve())], cwd=ROOT, check=True)
