(function (root) {
    'use strict';
    const node = typeof module !== 'undefined' && module.exports;
    const data = node ? require('./ui_data.js') : root.EPDUIData;
    const encoder = new TextEncoder(), decoder = new TextDecoder('utf-8', { fatal: true });
    const fail = message => { throw new Error(message); };
    const clone = value => JSON.parse(JSON.stringify(value));
    const view = bytes => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const defaults = () => clone(data.defaults);
    function integer(n, min, max, name) {
        if (!Number.isInteger(n) || n < min || n > max)
            fail(`${name} 应为 ${min}–${max} 的整数`);
    }
    function compile(text) {
        if (typeof text !== 'string' || /[\x00-\x1f\x7f{}]/.test(text.replace(/\{[^{}]+\}/g, '')) ||
            [...text].some(c => c.codePointAt(0) > 65535 || /[\p{Cs}\p{Cf}]/u.test(c)))
            fail('UI 文本或模板无效');
        const bytes = [];
        for (const part of text.split(/(\{[^{}]+\})/)) {
            if (!part.startsWith('{'))
                bytes.push(...encoder.encode(part));
            else {
                const token = part.slice(1, -1), id = data.schema.tokens.indexOf(token);
                if (id >= 0)
                    bytes.push(1, id + 1);
                else if (/^f[0-2]$/.test(token))
                    bytes.push(2, Number(token[1]));
                else if (/^c[0-2]$/.test(token))
                    bytes.push(3, Number(token[1]));
                else
                    fail(`未知模板字段 {${token}}`);
            }
        }
        if (bytes.length > 254)
            fail('单条 UI 文本最多 254 字节');
        return Uint8Array.from([...bytes, 0]);
    }
    function decompile(bytes, count) {
        const strings = [];
        let at = 0;
        while (at < bytes.length) {
            let text = '';
            while (at < bytes.length && bytes[at]) {
                const op = bytes[at++];
                if (op < 4) {
                    if (at >= bytes.length)
                        fail('UI 模板操作数缺失');
                    const n = bytes[at++];
                    if (op === 1 && n >= 1 && n <= data.schema.tokens.length)
                        text += `{${data.schema.tokens[n - 1]}}`;
                    else if (op === 2 && n <= 2)
                        text += `{f${n}}`;
                    else if (op === 3 && n <= 2)
                        text += `{c${n}}`;
                    else
                        fail('UI 模板操作数越界');
                }
                else {
                    const start = at - 1;
                    while (at < bytes.length && bytes[at] >= 4)
                        at++;
                    text += decoder.decode(bytes.subarray(start, at));
                }
            }
            if (at === bytes.length)
                fail('UI 文本缺少结束符');
            ++at;
            compile(text);
            strings.push(text);
        }
        if (strings.length !== count)
            fail('UI 文本数量不匹配');
        return strings;
    }
    function clockGeometry(c) {
        const s = c.style, scale = c.value0, step = s.digit_step_scale * scale + s.digit_step_bias;
        const block = c.value1 * step - s.block_trim * scale;
        const height = s.height_scale * scale + s.height_bias;
        const x = Math.trunc((c.width - 2 * block - s.span_gap * scale) / 2) + s.offset_x;
        const y = Math.trunc((c.height - height) / 2) + s.offset_y;
        return { x, y, step, block, height, minute: block + (s.hour_gap + s.minute_gap) * scale };
    }
    function validate(c) {
        const keys = ['id', 'modes', 'kind', 'flags', 'fonts', 'align', 'x', 'y', 'width', 'height', 'value0', 'value1',
            'foreground', 'background', 'accent', 'style', 'text'];
        if (!c || Object.keys(c).some(key => !keys.includes(key)) || keys.some(key => c[key] === undefined))
            fail('UI 组件字段缺失或包含不支持的字段');
        integer(c.id, 1, 255, '组件 ID');
        integer(c.modes, 1, 31, '模式位图');
        integer(c.kind, 1, 6, '组件类型');
        integer(c.flags, 0, 15, '显隐标记');
        if (c.kind !== 1 && c.align)
            fail('仅文字组件支持对齐');
        if (c.kind !== 4 && (c.value0 || c.value1))
            fail('仅时钟组件使用 value0/value1');
        if ((c.flags & 12) === 12)
            fail('节气显隐条件冲突');
        integer(c.align, 0, 2, '对齐');
        if (!Array.isArray(c.fonts) || c.fonts.length !== 3)
            fail('组件应有三个字体引用');
        c.fonts.forEach(slot => { if (slot !== 255)
            integer(slot, 0, 3, '字体槽位'); });
        ['foreground', 'background', 'accent'].forEach(key => integer(c[key], 0, 2, key));
        ['x', 'y', 'width', 'height', 'value0', 'value1'].forEach(key => integer(c[key], -32768, 32767, key));
        if (c.x < 0 || c.y < 0 || c.width < 1 || c.height < 1 || c.x + c.width > 800 || c.y + c.height > 480)
            fail(`组件 ${c.id} 超出 800×480 画布`);
        const spec = data.schema.styles[c.kind];
        if (!c.style || Object.keys(c.style).length !== spec.length || spec.some(([key]) => !(key in c.style)))
            fail(`组件 ${c.id} 的样式字段不匹配`);
        spec.forEach(([key, min, max]) => integer(c.style[key], min, max, key));
        const textCount = c.kind === 1 ? 1 : c.kind === 3 ? 2 : c.kind === 6 ? 3 : 0;
        if (!Array.isArray(c.text) || c.text.length !== textCount)
            fail(`组件 ${c.id} 应有 ${textCount} 条文本`);
        c.text.forEach(text => {
            compile(text);
            if (c.kind !== 1 && /[{}]/.test(text))
                fail('控件标签应为纯文本；数据模板请使用文字组件');
            let role = 0;
            for (const part of text.split(/(\{[^{}]+\})/)) {
                if (/^\{f[0-2]\}$/.test(part))
                    role = Number(part[2]);
                else if (part && !/^\{c[0-2]\}$/.test(part) && c.fonts[role] === 255)
                    fail('UI 文本引用了空字体');
            }
        });
        if ([1, 3, 5, 6].includes(c.kind) && c.fonts[0] === 255)
            fail('缺少组件主字体');
        if (c.kind === 3 && c.fonts.some(slot => slot === 255))
            fail('月历缺少字体');
        if (c.kind === 4) {
            if (c.value0 < 2 || c.value0 > 10 || c.value0 % 2 || c.value1 !== 2 || c.x % 8 || c.width % 8 || c.height > 208)
                fail('时钟画布必须字节对齐，缩放为 2–10 偶数，时分各两位，高度不超过 208');
            const g = clockGeometry(c), s = c.style, scale = c.value0;
            const rectangles = [];
            for (let n = 0; n < 4; n++)
                for (let seg = 0; seg < 7; seg++) {
                    const prefix = `segment_${seg}_`, horizontal = s[prefix + 'horizontal'];
                    rectangles.push([g.x + (n < 2 ? n * g.step : g.minute + (n - 2) * g.step) + s[prefix + 'x_scale'] * scale + s[prefix + 'x_bias'],
                        g.y + s[prefix + 'y_scale'] * scale + s[prefix + 'y_bias'],
                        horizontal ? (s.horizontal_length + 2) * scale : 2 * scale,
                        horizontal ? 2 * scale : (s.vertical_length + 2) * scale]);
                }
            for (const key of ['top', 'bottom'])
                rectangles.push([g.x + g.block + s.hour_gap * scale,
                    g.y + Math.floor(s['colon_' + key] * scale / 2) + s['colon_' + key + '_bias'], s.colon_width * scale, s.colon_height * scale]);
            if (g.x < 0 || g.y < 0 || rectangles.some(([x, y, w, h]) => x < 0 || y < 0 || x + w > c.width || y + h > c.height))
                fail('时钟字形或冒号超出局刷画布');
        }
    }
    function migrate(model) {
        if (model.version === 2)
            return model;
        if (Object.keys(model).some(k => !['version', 'components'].includes(k)) || model.version !== undefined && model.version !== 1)
            fail('不支持的旧版布局字段');
        if (!Array.isArray(model.components) || model.components.length !== 19)
            fail('旧版布局必须含 19 个组件');
        const next = defaults();
        const legacyModes = [1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3];
        const legacyKinds = [1, 1, 3, 2, 2, 2, 1, 1, 3, 1, 2, 4, 2, 1, 1, 2, 4, 2, 1];
        for (const [index, old] of model.components.entries()) {
            if (Object.keys(old).some(k => !['id', 'mode', 'kind', 'flags', 'fonts', 'x', 'y', 'width', 'height', 'value0', 'value1'].includes(k)) ||
                old.id !== index + 1 || old.mode !== legacyModes[index] || old.kind !== legacyKinds[index] || old.flags !== 0)
                fail('旧版组件字段、ID、模式或类型无效');
            const c = next.components.find(item => item.id === old.id);
            if (!c || ![1, 2, 3].includes(old.mode))
                fail('旧版布局组件无效');
            for (const key of ['x', 'width', 'height', 'value0', 'value1'])
                c[key] = old[key];
            c.fonts = old.fonts.slice();
            c.y = old.y;
            if (c.kind === 1) {
                c.y = Math.max(0, old.y - 24);
                c.height = Math.min(480 - c.y, Math.max(30, old.height));
                c.style.baseline = old.y - c.y;
                c.value0 = c.value1 = 0;
            }
        }
        const byID = id => next.components.find(c => c.id === id), old = id => model.components[id - 1];
        const place = (id, x, baseline, right, font) => {
            const c = byID(id);
            c.x = x;
            c.y = Math.max(0, baseline - c.style.baseline);
            c.style.baseline = baseline - c.y;
            c.height = Math.min(480 - c.y, Math.max(c.height, c.style.baseline + 6));
            if (right !== null) {
                c.width = Math.max(1, right - c.x);
                c.align = 2;
            }
            c.width = Math.min(c.width, 800 - c.x);
            if (font !== undefined)
                c.fonts[0] = font;
        };
        const cal = old(1), split = old(10), full = old(15), right = old(7), footer = old(14), fullFooter = old(19);
        place(1, cal.x, cal.y - 2, null);
        place(24, cal.x + 220, cal.y - 14, null, cal.fonts[2]);
        place(25, cal.x + 220, cal.y, null, cal.fonts[2]);
        place(26, 610, cal.value0 + 9, 800 - cal.value1, cal.fonts[2]);
        place(27, 610, cal.y, 802 - cal.value1, cal.fonts[2]);
        place(28, right.x, right.value0 + 9, right.value1, right.fonts[0]);
        place(29, split.x, split.value0, null, split.fonts[1]);
        place(30, Math.max(split.x, split.value1 - 147), split.value0, split.value1, split.fonts[0]);
        byID(30).fonts[1] = split.fonts[1];
        place(31, footer.x, footer.value0, null, footer.fonts[0]);
        place(32, footer.x + Math.max(0, footer.width - 177), footer.y, footer.x + footer.width, footer.fonts[0]);
        place(33, byID(32).x, footer.value0, footer.x + footer.width, footer.fonts[0]);
        place(45, byID(32).x, footer.y, footer.x + footer.width, footer.fonts[0]);
        place(34, full.x, full.value0, null, full.fonts[2]);
        place(35, full.value1, full.value0, null, full.fonts[2]);
        place(36, Math.max(full.x, full.x + full.width - 150), full.y - 2, full.x + full.width, full.fonts[2]);
        place(37, byID(36).x, full.value0, full.x + full.width, full.fonts[2]);
        place(38, fullFooter.x, fullFooter.value0, null, fullFooter.fonts[0]);
        for (const [id, baseline] of [[39, fullFooter.y], [40, fullFooter.value0], [46, fullFooter.y]])
            place(id, Math.max(fullFooter.x, fullFooter.value1 - 220), baseline, fullFooter.value1, fullFooter.fonts[0]);
        return next;
    }
    function encode(input) {
        const model = migrate(input);
        if (Object.keys(model).some(key => !['version', 'components'].includes(key)) || model.version !== 2 ||
            !Array.isArray(model.components) || !model.components.length || model.components.length > 64)
            fail('UIR2 应含 1–64 个组件');
        const seen = new Set(), records = [], chunks = [];
        let at = 16 + model.components.length * 32;
        for (const c of model.components) {
            validate(c);
            if (seen.has(c.id))
                fail('组件 ID 重复');
            seen.add(c.id);
            const fields = data.schema.styles[c.kind], params = new Uint8Array(fields.length * 2);
            fields.forEach(([key], i) => view(params).setInt16(i * 2, c.style[key], true));
            const text = Uint8Array.from(c.text.flatMap(s => [...compile(s)]));
            const raw = new Uint8Array(32), v = view(raw);
            raw.set([c.modes, c.id, c.kind, c.flags, ...c.fonts, c.align]);
            ['x', 'y', 'width', 'height', 'value0', 'value1'].forEach((key, i) => v.setInt16(8 + i * 2, c[key], true));
            raw.set([c.foreground, c.background, c.accent, 0], 20);
            v.setUint16(24, at, true);
            raw[26] = fields.length;
            raw[27] = c.text.length;
            v.setUint16(28, at + params.length, true);
            v.setUint16(30, text.length, true);
            records.push(raw);
            chunks.push(params, text);
            at += params.length + text.length;
        }
        if (at > 65504)
            fail('UI 资源超出分区容量');
        const bytes = new Uint8Array(at), v = view(bytes);
        v.setUint32(0, 0x32524955, true);
        [2, records.length, 800, 480, 32, 0].forEach((n, i) => v.setUint16(4 + i * 2, n, true));
        at = 16;
        for (const raw of [...records, ...chunks]) {
            bytes.set(raw, at);
            at += raw.length;
        }
        return bytes;
    }
    function decode(bytes) {
        const v = view(bytes), count = bytes.length >= 16 ? v.getUint16(6, true) : 0;
        if (bytes.length < 16 || v.getUint32(0, true) !== 0x32524955 || count < 1 || count > 64 ||
            [2, 800, 480, 32, 0].some((n, i) => v.getUint16([4, 8, 10, 12, 14][i], true) !== n) || bytes.length < 16 + count * 32)
            fail('不支持的 UIR2 资源');
        const components = [];
        let tail = 16 + count * 32;
        for (let i = 0; i < count; i++) {
            const at = 16 + i * 32, kind = bytes[at + 2], fields = data.schema.styles[kind];
            if (!fields || bytes[at + 23] || bytes[at + 26] !== fields.length || v.getUint16(at + 24, true) !== tail)
                fail('UI 样式偏移或长度无效');
            const end = tail + fields.length * 2, textAt = v.getUint16(at + 28, true), textEnd = textAt + v.getUint16(at + 30, true);
            if (end !== textAt || textEnd > bytes.length)
                fail('UI 文本越界');
            const c = { modes: bytes[at], id: bytes[at + 1], kind, flags: bytes[at + 3], fonts: Array.from(bytes.subarray(at + 4, at + 7)), align: bytes[at + 7] };
            ['x', 'y', 'width', 'height', 'value0', 'value1'].forEach((key, j) => { c[key] = v.getInt16(at + 8 + j * 2, true); });
            ['foreground', 'background', 'accent'].forEach((key, j) => { c[key] = bytes[at + 20 + j]; });
            c.style = Object.fromEntries(fields.map(([key], j) => [key, v.getInt16(tail + j * 2, true)]));
            c.text = decompile(bytes.subarray(textAt, textEnd), bytes[at + 27]);
            components.push(c);
            tail = textEnd;
        }
        if (tail !== bytes.length)
            fail('UI 资源含尾随数据');
        const model = { version: 2, components };
        encode(model);
        return model;
    }
    function todoCapacity(model, fontHeight, extended = true) {
        const panes = migrate(model).components.filter(c => c.kind === 6 && !(c.flags & 1));
        if (!panes.length)
            return 8;
        return Math.max(0, Math.min(8, ...panes.map(c => Math.floor(c.height / (fontHeight + c.style.row_padding)))));
    }
    const api = { defaults, schema: data.schema, compile, decompile, encode, decode, migrate, validate, clockGeometry, todoCapacity };
    if (node)
        module.exports = api;
    else
        root.EPDUIModel = api;
})(globalThis);
