"""Compare web and production paged renderer using identical external resources."""
import json
import os
from pathlib import Path
import shlex
import shutil
import sys
import subprocess

ROOT=Path(__file__).resolve().parents[1]
OUT=Path(os.environ.get('HOST_TEST_OUT',ROOT/'_build/host_tests'))/'ui_render'
node=os.environ.get('HOST_NODE') or os.environ.get('NODE_BINARY') or shutil.which('node')
env=os.environ.copy()
if not node:
    print('SKIP: web/device pixel comparison requires Node.js (or HOST_NODE/NODE_BINARY).')
    sys.exit(0)
if node and node.lower().endswith('code.exe'):env['ELECTRON_RUN_AS_NODE']='1'
OUT.mkdir(parents=True,exist_ok=True)
node_out=str(OUT.resolve())
if sys.platform=='cygwin' and node.lower().endswith('.exe'):
    node_out=subprocess.check_output(['cygpath','-w',node_out],text=True).strip()
prepared=subprocess.run([node,'tests/ui_render_cases.cjs',node_out],cwd=ROOT,env=env,capture_output=True,text=True,encoding='utf-8')
if prepared.returncode:raise RuntimeError(prepared.stdout+prepared.stderr)
cc=shlex.split(os.environ.get('HOST_CC','gcc'))
sources=['GUI/GUI.c','GUI/todo_items.c','GUI/font_geometry.c','tests/stubs/todo_resources_stub.c',
    'GUI/Adafruit_GFX.c','GUI/u8g2_font.c','GUI/Lunar.c','GUI/calendar_annotations.c',
    'tests/stubs/calendar_resources_stub.c','tests/stubs/ui_resources_stub.c',
    'EPD/epd_clock_refresh.c','tests/test_ui_render_driver.c']
for fixed in (False,True):
    extra=['-DGFX_FIXED_BWR_PANEL=1','-DGFX_FIXED_FONT_DIRECTION_0=1',
           '-DGUI_FIXED_PANEL_WIDTH=800','-DGUI_FIXED_PANEL_HEIGHT=480'] if fixed else []
    exe=OUT/('fixed.exe' if fixed else 'generic.exe')
    subprocess.run(cc+['-Wall','-Werror','-std=c11','-IGUI','-IEPD','-Itests/stubs',
        '-Itools/test/host/shim',*extra,*sources,'-o',str(exe)],cwd=ROOT,check=True)
    cases=json.loads((OUT/'cases.json').read_text(encoding='utf-8'))
    for case in cases:
        folder=OUT/case['name'];target=folder/('device-fixed.pixels' if fixed else 'device.pixels')
        subprocess.run([str(exe.resolve()),str((folder/'ui.bin').resolve()),str((folder/'todo.bin').resolve()),
            str((folder/'annotations.bin').resolve()),str(folder.resolve()),str(target.resolve()),
            str(case['mode']),str(case['timestamp']),str(case['week']),str(case['temperature']),case['ssid'],str(case['voltage'])],cwd=ROOT,check=True)
        actual=target.read_bytes();expected=(folder/'web.pixels').read_bytes()
        if actual!=expected:
            mismatches=[i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b]
            raise AssertionError(f"{case['name']} fixed={fixed}: {len(mismatches)} differing pixels; "
                                 f"first x/y={mismatches[0]%800}/{mismatches[0]//800}")
    print(f'PASS: {len(cases)} web/device renders, fixed panel={fixed}')
