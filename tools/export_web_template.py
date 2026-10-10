"""Export a reproducible browser template from the canonical ERS generators."""
import argparse
import base64
import json
from pathlib import Path
import resource_pack as pack

ROOT = Path(__file__).resolve().parents[1]


def template():
    config, annotations = pack.default_record_data()
    rows = [
        (1, 'partition-table', pack.table_payload(pack.load_layout())),
        (5, 'calendar', pack.calendar_payload(ROOT / 'tools/dist')[0]),
        (6, 'ui', pack.ui_payload()[0]),
        (7, 'fonts', pack.font_payload(ROOT / 'tools/dist')[0]),
        (2, 'epd-config', pack.record_payload(pack.CONFIG_RECORD_MAGIC, config)),
        (3, 'calendar-annotations', pack.record_payload(pack.ANNOTATION_RECORD_MAGIC, annotations)),
        (9, 'todo', pack.todo_payload()),
    ]
    return json.dumps({'format': 'EPD-RESOURCE-PROJECT-1', 'files': [
        {'name': name + '.ers', 'base64': base64.b64encode(
            pack.package(part, payload, pack.MODULE_VERSIONS[part], 0)).decode('ascii')}
        for part, name, payload in rows]}, ensure_ascii=False, indent=2) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    path = ROOT / 'html/resources/default-project.json'
    content = template()
    if args.check:
        if path.read_text(encoding='utf-8') != content:
            raise SystemExit('Browser template is stale; run tools/export_web_template.py')
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8', newline='\n')
    print('Browser resource template OK')
