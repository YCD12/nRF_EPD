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
web_min_definitions = re.findall(
    r"\bconst NEW_WEB_MIN_FIRMWARE_VERSION\s*=\s*(0x[0-9a-fA-F]+)", web)
assert len(web_min_definitions) == 1, "Define the new/legacy page boundary once"
web_min = int(web_min_definitions[0], 16)
assert fw_version == web_min == 0x21
assert not (ROOT / "html/js/firmware_policy.js").exists(), "Keep the policy in main.js"
production_sources = [(path, path.read_text(encoding="utf-8"))
                      for path in (ROOT / "html/js").rglob("*.js")]
assert sum(len(re.findall(r"\bconst NEW_WEB_MIN_FIRMWARE_VERSION\s*=", source))
           for _, source in production_sources) == 1
for path, source in production_sources:
    assert "firmware_policy.js" not in source, path
    assert not re.search(r"\b(?:MIN_FIRMWARE_VERSION|CONFIG_READBACK_MIN_FIRMWARE_VERSION|"
                         r"RESOURCE_PROTOCOL_MIN_FIRMWARE_VERSION|resourceProtocolMinVersion)\b", source), path
    assert not re.search(r"\bappVersion\s*(?:<|>|<=|>=)\s*0x[0-9a-fA-F]+", source), path
assert web_ids["SET_MODE"] == 0x22, "The SET_MODE opcode is independent of the page boundary"
todo_web = (ROOT / "html/js/todo_calendar.js").read_text(encoding="utf-8")
gui_header = (ROOT / "GUI/GUI.h").read_text(encoding="utf-8")
for name in ("MIN_VERSION", "FULL_MIN_VERSION", "TYPOGRAPHY_MIN_VERSION", "TIMING_MIN_VERSION"):
    assert re.search(r"get " + name + r"\(\)\s*\{\s*return firmwarePolicy.newWebMinVersion;", todo_web), name
assert re.search(r"MODE_TODO_CALENDAR\s*=\s*4\b", gui_header)
assert re.search(r"MODE_TODO\s*=\s*5\b", gui_header)
print("PASS: firmware/web command IDs and minimum version match")
