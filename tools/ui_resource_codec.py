"""UIR2 codec. The web and firmware schema is generated from ui-schema.json."""
import json
from pathlib import Path
import re
import struct
import unicodedata

ROOT = Path(__file__).resolve().parents[1]
SCHEMA_PATH = ROOT / 'resources/ui-schema.json'
DEFAULT_PATH = ROOT / 'resources/ui-layout.json'

def schema():
    return json.loads(SCHEMA_PATH.read_text(encoding='utf-8'))

def default_ui():
    return json.loads(DEFAULT_PATH.read_text(encoding='utf-8'))

def template(text):
    if (not isinstance(text, str) or
            any(ord(c) > 65535 or unicodedata.category(c) in ('Cs', 'Cf') for c in text) or
            re.search(r'[\x00-\x1f\x7f{}]', re.sub(r'\{[^{}]+\}', '', text))):
        raise ValueError('invalid UI text or template')
    tokens = schema()['tokens']
    out = bytearray()
    for part in re.split(r'(\{[^{}]+\})', text):
        if part.startswith('{'):
            name = part[1:-1]
            if name in tokens:
                out.extend((1, tokens.index(name) + 1))
            elif re.fullmatch(r'f[0-2]', name):
                out.extend((2, int(name[1:])))
            elif re.fullmatch(r'c[0-2]', name):
                out.extend((3, int(name[1:])))
            else:
                raise ValueError(f'unknown UI template token: {name}')
        else:
            if any(ord(c) < 32 for c in part):
                raise ValueError('control characters are not allowed in UI text')
            out.extend(part.encode('utf-8'))
    return bytes(out) + b'\0'

def encode_ui(model):
    spec = schema()
    components = model['components']
    if set(model) != {'version','components'} or model.get('version') != 2 or not 1 <= len(components) <= 64:
        raise ValueError('UIR2 requires version 2 and 1..64 components')
    records, body = bytearray(), bytearray()
    start = 16 + len(components) * 32
    seen=set()
    def integer(value,low,high):
        if type(value) is not int or not low <= value <= high:
            raise ValueError(f'UI integer outside {low}..{high}: {value}')
    for c in components:
        if set(c) != {'id','modes','kind','flags','fonts','align','x','y','width','height',
                      'value0','value1','foreground','background','accent','style','text'}:
            raise ValueError('unknown or missing UI component fields')
        integer(c['id'],1,255);integer(c['modes'],1,31);integer(c['kind'],1,6)
        integer(c['flags'],0,15);integer(c['align'],0,2)
        if c['id'] in seen or c['flags'] & 12 == 12:
            raise ValueError('duplicate UI ID or conflicting visibility flags')
        seen.add(c['id'])
        if c['kind'] != 1 and c['align'] or c['kind'] != 4 and (c['value0'] or c['value1']):
            raise ValueError('alignment/value fields are not used by this kind')
        for name in ('x','y','width','height','value0','value1'):
            integer(c[name],-32768,32767)
        if (c['x']<0 or c['y']<0 or c['width']<1 or c['height']<1 or
                c['x']+c['width']>800 or c['y']+c['height']>480):
            raise ValueError('UI rectangle outside 800x480')
        if len(c['fonts']) != 3:
            raise ValueError('UI requires three font references')
        for slot in c['fonts']:
            if slot != 255: integer(slot,0,3)
        for name in ('foreground','background','accent'):integer(c[name],0,2)
        if c['kind'] in (1,3,5,6) and c['fonts'][0]==255 or c['kind']==3 and 255 in c['fonts']:
            raise ValueError('UI missing font')
        expected={1:1,3:2,6:3}.get(c['kind'],0)
        if len(c['text'])!=expected:
            raise ValueError('UI text count mismatch')
        for text in c['text']:
            if c['kind']!=1 and any(ch in text for ch in '{}'):
                raise ValueError('widget labels must be plain text')
            role=0
            for part in re.split(r'(\{[^{}]+\})',text):
                if re.fullmatch(r'\{f[0-2]\}',part):role=int(part[2])
                elif part and not re.fullmatch(r'\{c[0-2]\}',part) and c['fonts'][role]==255:
                    raise ValueError('UI text references an absent font')
            if len(template(text))>255:raise ValueError('UI text exceeds 254 bytes')
        fields = spec['styles'][str(c['kind'])]
        if set(c['style']) != {p[0] for p in fields}:
            raise ValueError('UI style fields do not match the schema')
        for key, minimum, maximum, _ in fields:
            integer(c['style'][key],minimum,maximum)
        if c['kind']==4:
            scale=c['value0'];s=c['style']
            if scale not in (2,4,6,8,10) or c['value1']!=2 or c['x']%8 or c['width']%8 or c['height']>208:
                raise ValueError('UI clock geometry/alignment invalid')
            step=s['digit_step_scale']*scale+s['digit_step_bias'];block=2*step-s['block_trim']*scale
            x=int((c['width']-2*block-s['span_gap']*scale)/2)+s['offset_x']
            y=int((c['height']-s['height_scale']*scale-s['height_bias'])/2)+s['offset_y']
            minute=block+(s['hour_gap']+s['minute_gap'])*scale
            boxes=[]
            for n in range(4):
                for seg in range(7):
                    key=f'segment_{seg}_';horizontal=s[key+'horizontal']
                    boxes.append((x+(n*step if n<2 else minute+(n-2)*step)+s[key+'x_scale']*scale+s[key+'x_bias'],
                        y+s[key+'y_scale']*scale+s[key+'y_bias'],
                        (s['horizontal_length']+2)*scale if horizontal else 2*scale,
                        2*scale if horizontal else (s['vertical_length']+2)*scale))
            for key in ('top','bottom'):
                boxes.append((x+block+s['hour_gap']*scale,y+s[f'colon_{key}']*scale//2+s[f'colon_{key}_bias'],
                              s['colon_width']*scale,s['colon_height']*scale))
            if x<0 or y<0 or any(bx<0 or by<0 or bx+w>c['width'] or by+h>c['height'] for bx,by,w,h in boxes):
                raise ValueError('UI clock shape outside its refresh canvas')
        params = struct.pack('<' + 'h' * len(fields), *(c['style'][p[0]] for p in fields))
        text = b''.join(template(t) for t in c['text'])
        records.extend(struct.pack('<8B6h4BHBBHH', c['modes'], c['id'], c['kind'], c['flags'],
            *c['fonts'], c['align'], c['x'], c['y'], c['width'], c['height'], c['value0'], c['value1'],
            c['foreground'], c['background'], c['accent'], 0, start + len(body), len(fields), len(c['text']),
            start + len(body) + len(params), len(text)))
        body.extend(params + text)
    result=struct.pack('<I6H', 0x32524955, 2, len(components), 800, 480, 32, 0) + records + body
    if len(result)>65504:raise ValueError('UI exceeds its external Flash partition')
    return result

def generate(check=False):
    spec, defaults = schema(), default_ui()
    js = ('// Generated by tools/ui_resource_codec.py; edit resources/ui-*.json.\n'
          '(function(root){const data=' + json.dumps({'schema': spec, 'defaults': defaults}, ensure_ascii=False,
          separators=(',', ':')) + ';if(typeof module!=="undefined"&&module.exports)module.exports=data;'
          'else root.EPDUIData=data;})(globalThis);\n')
    header = ['/* Generated by tools/ui_resource_codec.py. */', '#ifndef UI_SCHEMA_H', '#define UI_SCHEMA_H']
    for i, name in enumerate(spec['tokens'], 1):
        header.append(f'#define UI_TOKEN_{name.upper()} {i}U')
    header.append(f'#define UI_TOKEN_COUNT {len(spec["tokens"])}U')
    for kind, fields in spec['styles'].items():
        prefix = spec['kinds'][str(kind)].upper()
        for i, (key, _, _, _) in enumerate(fields):
            header.append(f'#define UI_{prefix}_{key.upper()} {i}U')
        header.append(f'#define UI_{prefix}_PARAMS {len(fields)}U')
    header.extend(['#ifdef UI_SCHEMA_VALIDATION', 'static const uint8_t ui_style_counts[] = {0,' +
                   ','.join(str(len(f)) for f in spec['styles'].values()) + '};'])
    bounds = [(p[1], p[2]) for fields in spec['styles'].values() for p in fields]
    unique_bounds = list(dict.fromkeys(bounds))
    if len(unique_bounds) > 256:
        raise ValueError('UI validation range dictionary exceeds uint8_t indices')
    bound_indices = {pair: i for i, pair in enumerate(unique_bounds)}
    header.append('static const uint8_t ui_style_bound_index[] = {' +
                  ','.join(str(bound_indices[pair]) for pair in bounds) + '};')
    header.append('static const int16_t ui_style_bounds[][2] = {' +
                  ','.join('{' + f'{lo},{hi}' + '}' for lo, hi in unique_bounds) + '};')
    header.extend(['#endif', '#endif', ''])
    payload = encode_ui(defaults)
    fixture = ('/* Generated by tools/ui_resource_codec.py from the production UI source. */\n'
               'static const unsigned char ui_default_payload[] = {\n' +
               '\n'.join('    ' + ','.join(str(n) for n in payload[i:i+24]) + ','
                         for i in range(0,len(payload),24)) + '\n};\n')
    for path, content in [(ROOT / 'html/js/ui_tools/ui_data.js', js),
                          (ROOT / 'GUI/ui_schema.h', '\n'.join(header)),
                          (ROOT / 'tests/stubs/ui_default_payload.h', fixture)]:
        if check:
            if not path.exists() or path.read_text(encoding='utf-8') != content:
                raise ValueError(f'{path}: regenerate UI schema')
        else:
            path.write_text(content, encoding='utf-8', newline='\n')

if __name__ == '__main__':
    import sys
    generate('--check' in sys.argv)
