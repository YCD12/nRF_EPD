"""Keep the current web command IDs aligned with the firmware protocol."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
firmware = (ROOT / "EPD/epd_protocol.h").read_text(encoding="utf-8")
web = (ROOT / "html/js/main.js").read_text(encoding="utf-8")
service = (ROOT / "EPD/EPD_service.h").read_text(encoding="utf-8")

fw_ids = {name: int(value, 16) for name, value in re.findall(
    r"EPD_CMD_(\w+)\s*=\s*(0x[0-9a-fA-F]+)", firmware)}
web_commands = web.split("const EpdCmd = {", 1)[1].split("};", 1)[0]
web_ids = {name: int(value, 16) for name, value in re.findall(
    r"^\s*(\w+):\s*(0x[0-9a-fA-F]+),", web_commands, re.MULTILINE)}

for name, value in web_ids.items():
    assert name in fw_ids, f"Unknown web command: {name}"
    assert value == fw_ids[name], f"{name}: web {value:#x}, firmware {fw_ids[name]:#x}"

expected = {"REFRESH": 0x03, "SLEEP": 0x04, "WRITE_BLOCK": 0x30,
            "QUERY_STATUS": 0x31, "RESET_TRANSFER": 0x32}
assert {name: fw_ids[name] for name in expected} == expected
assert all(name not in fw_ids and name not in web_ids for name in
           ("SEND_COMMAND", "SEND_DATA", "WRITE_IMAGE", "SEND_CMD", "WRITE_IMG"))

fw_version = int(re.search(r"#define APP_VERSION\s+(0x[0-9a-fA-F]+)", service).group(1), 16)
web_min = int(re.search(r"const MIN_FIRMWARE_VERSION\s*=\s*(0x[0-9a-fA-F]+)", web).group(1), 16)
assert fw_version == 0x3D and web_min == 0x38
todo_web = (ROOT / "html/js/todo_calendar.js").read_text(encoding="utf-8")
gui_header = (ROOT / "GUI/GUI.h").read_text(encoding="utf-8")
assert int(re.search(r"MIN_VERSION:\s*(0x[0-9a-fA-F]+)", todo_web).group(1), 16) == 0x3A
assert int(re.search(r"FULL_MIN_VERSION:\s*(0x[0-9a-fA-F]+)", todo_web).group(1), 16) == 0x3B
assert int(re.search(r"TYPOGRAPHY_MIN_VERSION:\s*(0x[0-9a-fA-F]+)", todo_web).group(1), 16) == 0x3C
assert int(re.search(r"TIMING_MIN_VERSION:\s*(0x[0-9a-fA-F]+)", todo_web).group(1), 16) == fw_version
assert re.search(r"MODE_TODO_CALENDAR\s*=\s*4\b", gui_header)
assert re.search(r"MODE_TODO\s*=\s*5\b", gui_header)
print("PASS: firmware/web command IDs and minimum version match")
