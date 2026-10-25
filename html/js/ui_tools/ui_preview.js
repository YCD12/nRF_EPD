(function (root) {
    'use strict';
    const node = typeof module !== 'undefined' && module.exports;
    const model = node ? require('./ui_model.js') : root.EPDUIModel;
    const fontsAPI = () => node ? require('../font_builder.js') : root.EPDWebFonts;
    const cache = new WeakMap(), trunc = Math.trunc;
    function fontMaps(fonts) { return fonts.map((f, i) => { if (!f)
        return null; if (!cache.has(f))
        cache.set(f, fontsAPI().glyphs(f, i)); return cache.get(f); }); }
    const signed = n => n > 127 ? n - 256 : n;
    const days = (y, m) => new Date(Date.UTC(y, m, 0)).getUTCDate();
    function lunar(cal, y, m, d) {
        const year = y < cal.year_min || y > cal.year_max ? cal.year_min : y, base = cal.solar_new_year[0];
        let i = year - base + 1, code = year * 512 + m * 32 + d;
        if (cal.solar_new_year[i] > code && i > 1)
            --i;
        const newYear = cal.solar_new_year[i], ny = newYear >> 9, nm = newYear >> 5 & 15, nd = newYear & 31;
        let offset = Math.round((Date.UTC(year, m - 1, d) - Date.UTC(ny, nm - 1, nd)) / 86400000) + 1, month = 1;
        const bits = cal.lunar_month_days[i], leap = bits >> 13 & 15;
        for (let n = 0; n < 13; n++) {
            const count = (bits >> (12 - n) & 1) ? 30 : 29;
            if (offset <= count)
                break;
            offset -= count;
            month++;
        }
        const isLeap = leap && month === leap + 1 ? 1 : 0;
        if (leap && month > leap)
            --month;
        return { year: base + i - 1, month, day: offset, leap: isLeap };
    }
    function jieqiDate(cal, y, m, d) {
        const year = y < cal.year_min || y > cal.year_max ? cal.year_min : y, index = (m - 1) * 2 + (d >= 15 ? 1 : 0);
        const bit = cal.jieqi_year_bits[(year - (cal.year_min - 1)) * 3 + Math.floor(index / 8)] & (128 >> (index % 8));
        return { index, day: cal.jieqi_base_days[index] + (bit ? ([1, 11, 18, 21].includes(index) && y < 2044 ? 1 : -1) : 0) };
    }
    function jieqi(cal, y, m, d) {
        let q = jieqiDate(cal, y, m, d), count = q.day - d, index = q.index;
        if (count < 0) {
            index++;
            if (d < 15)
                count = jieqiDate(cal, y, m, 15).day - d;
            else
                count = days(y, m) - d + jieqiDate(cal, m === 12 ? y + 1 : y, m === 12 ? 1 : m + 1, 1).day;
        }
        return { index: index % 24, days: count };
    }
    function festival(cal, y, m, d, w, l) {
        let item = cal.lunar_festivals.find(f => f.month === l.month && f.day === l.day);
        if (item)
            return item.name;
        if (l.month === 12 && l.day === 29) {
            const next = new Date(Date.UTC(y, m - 1, d + 1)), n = lunar(cal, next.getUTCFullYear(), next.getUTCMonth() + 1, next.getUTCDate());
            if (n.month === 1 && n.day === 1)
                return cal.special_festival_names[0];
        }
        if (m === 5 && w === 0 && d >= 8 && d <= 14)
            return cal.special_festival_names[1];
        if (m === 6 && w === 0 && d >= 15 && d <= 21)
            return cal.special_festival_names[2];
        if (m === 11 && w === 4 && d >= 22 && d <= 28)
            return cal.special_festival_names[3];
        item = cal.solar_festivals.find(f => f.month === m && f.day === d);
        if (item)
            return item.name;
        const jq = jieqiDate(cal, y, m, d);
        return jq.day === d ? (jq.index === 6 ? cal.special_festival_names[4] : cal.jieqi_names[jq.index]) : null;
    }
    function selection(todo, now) {
        const lead = (todo.leadMinutes ?? 10) * 60, retain = (todo.retainMinutes ?? 5) * 60;
        const phase = item => item.resident ? 'resident' : now < item.start ? 'upcoming' : now < item.end ? 'active' : 'expired';
        const sorted = (todo.items || []).filter(i => !i.completed && (i.resident || (now >= Math.max(0, i.start - lead) && now < i.end + retain)))
            .sort((a, b) => Number(b.resident) - Number(a.resident) || a.start - b.start || a.end - b.end || a.id - b.id);
        const start = Math.floor(now / 86400) * 86400;
        return { items: sorted.slice(0, todo.limit || 5).map(i => ({ ...i, phase: phase(i) })), daily: (todo.items || []).filter(i => !i.completed && (i.resident || Math.max(0, i.start - lead) < start + 86400 && i.end + retain > start)).length };
    }
    function render(ui, cal, fonts, runtime = {}, todo = {}, annotations = []) {
        // Decode what will actually be sent, so preview cannot accidentally render
        // fields omitted by the binary encoder or rely on unsaved CSS geometry.
        ui = model.decode(model.encode(ui));
        const maps = fontMaps(fonts), pixels = new Uint8Array(800 * 480).fill(1), date = new Date((runtime.timestamp ?? Math.floor(Date.now() / 1000)) * 1000);
        const year = date.getUTCFullYear(), month = date.getUTCMonth() + 1, day = date.getUTCDate(), l = lunar(cal, year, month, day), jq = jieqi(cal, year, month, day);
        const chosen = selection(todo, Math.floor(date.getTime() / 1000));
        let clip = [0, 0, 800, 480], slot = 0, fg = 0, bg = 1, transparent = true;
        function pixel(x, y, color) { x = trunc(x); y = trunc(y); if (x >= clip[0] && x < clip[2] && y >= clip[1] && y < clip[3] && x >= 0 && x < 800 && y >= 0 && y < 480)
            pixels[y * 800 + x] = color; }
        function rect(x, y, w, h, color) { for (let iy = 0; iy < h; iy++)
            for (let ix = 0; ix < w; ix++)
                pixel(x + ix, y + iy, color); }
        function outline(x, y, w, h, color) { rect(x, y, w, 1, color); rect(x, y + h - 1, w, 1, color); rect(x, y, 1, h, color); rect(x + w - 1, y, 1, h, color); }
        function circleHelper(cx, cy, r, corners, color, filled = false, delta = 0) {
            let f = 1 - r, dx = 1, dy = -2 * r, x = 0, y = r, px = x, py = y;
            delta++;
            while (x < y) {
                if (f >= 0) {
                    y--;
                    dy += 2;
                    f += dy;
                }
                x++;
                dx += 2;
                f += dx;
                if (filled) {
                    if (x < y + 1) {
                        if (corners & 1)
                            rect(cx + x, cy - y, 1, 2 * y + delta, color);
                        if (corners & 2)
                            rect(cx - x, cy - y, 1, 2 * y + delta, color);
                    }
                    if (y !== py) {
                        if (corners & 1)
                            rect(cx + py, cy - px, 1, 2 * px + delta, color);
                        if (corners & 2)
                            rect(cx - py, cy - px, 1, 2 * px + delta, color);
                        py = y;
                    }
                    px = x;
                }
                else {
                    if (corners & 4) {
                        pixel(cx + x, cy + y, color);
                        pixel(cx + y, cy + x, color);
                    }
                    if (corners & 2) {
                        pixel(cx + x, cy - y, color);
                        pixel(cx + y, cy - x, color);
                    }
                    if (corners & 8) {
                        pixel(cx - y, cy + x, color);
                        pixel(cx - x, cy + y, color);
                    }
                    if (corners & 1) {
                        pixel(cx - y, cy - x, color);
                        pixel(cx - x, cy - y, color);
                    }
                }
            }
        }
        function circle(cx, cy, r, color, filled = true) { if (filled) {
            rect(cx, cy - r, 1, 2 * r + 1, color);
            circleHelper(cx, cy, r, 3, color, true);
        }
        else {
            pixel(cx, cy + r, color);
            pixel(cx, cy - r, color);
            pixel(cx + r, cy, color);
            pixel(cx - r, cy, color);
            circleHelper(cx, cy, r, 15, color);
        } }
        function round(x, y, w, h, r, color, filled = true) { r = Math.min(r, trunc(Math.min(w, h) / 2)); if (filled) {
            rect(x + r, y, w - 2 * r, h, color);
            circleHelper(x + w - r - 1, y + r, r, 1, color, true, h - 2 * r - 1);
            circleHelper(x + r, y + r, r, 2, color, true, h - 2 * r - 1);
        }
        else {
            rect(x + r, y, w - 2 * r, 1, color);
            rect(x + r, y + h - 1, w - 2 * r, 1, color);
            rect(x, y + r, 1, h - 2 * r, color);
            rect(x + w - 1, y + r, 1, h - 2 * r, color);
            circleHelper(x + r, y + r, r, 1, color);
            circleHelper(x + w - r - 1, y + r, r, 2, color);
            circleHelper(x + w - r - 1, y + h - r - 1, r, 4, color);
            circleHelper(x + r, y + h - r - 1, r, 8, color);
        } }
        function metrics() { const f = fonts[slot]; return f ? { width: f[9], height: f[10], x: signed(f[11]), y: signed(f[12]), ascent: signed(f[13]), descent: signed(f[14]), advance: slot === 3 && [9, 12, 16, 20, 24, 32].some(size => { const p = Math.ceil(size * 4 / 3); return f[9] === p + Math.ceil(p / 4) && f[10] === p + Math.ceil(p / 3); }) ? signed(f[13]) + 1 : f[9] + 1 } : { width: 12, height: 12, x: 0, y: -2, ascent: 8, descent: -2, advance: 13 }; }
        function glyph(char) { return maps[slot]?.get(char.codePointAt(0)); }
        function advance(char) { const g = glyph(char); return g ? g.advance : char === ' ' ? 0 : metrics().advance; }
        function width(text, extent = false) { let x = 0, right = 0, left = 0, last = null; for (const char of text) {
            const g = glyph(char), m = metrics(), edge = g ? (g.width ? g.x + g.width : 0) : char === ' ' ? 0 : m.x + m.width;
            left = Math.min(left, x + (g ? g.x : m.x));
            right = Math.max(right, x + edge);
            x += advance(char);
            if (g)
                last = g;
        } return extent ? { right, left } : x + (last && last.width ? last.width + last.x - last.advance : 0); }
        function text(str, x, y) { for (const char of str) {
            const g = glyph(char), m = metrics();
            if (g) {
                for (let row = 0; row < g.height; row++)
                    for (let col = 0; col < g.width; col++) {
                        if (g.pixels[row * g.width + col])
                            pixel(x + g.x + col, y - g.y - g.height + row, fg);
                        else if (!transparent)
                            pixel(x + g.x + col, y - g.y - g.height + row, bg);
                    }
            }
            else if (char !== ' ') {
                outline(x + m.x, y - m.ascent, m.width, m.height, fg);
                for (let i = 1; i < Math.min(m.width, m.height) - 1; i++) {
                    pixel(x + m.x + i, y - m.ascent + i, fg);
                    pixel(x + m.x + m.width - 1 - i, y - m.ascent + i, fg);
                }
            }
            x += advance(char);
        } return x; }
        function binding(name, s) { const ly = l.year; switch (name) {
            case 'year': return String(year);
            case 'month': return String(month);
            case 'day': return String(day);
            case 'month2': return String(month).padStart(2, '0');
            case 'day2': return String(day).padStart(2, '0');
            case 'weekday': return cal.weekday_names[date.getUTCDay()];
            case 'lunar_month': return cal.leap_names[l.leap] + cal.month_names[l.month];
            case 'lunar_day': return cal.date_names[l.day];
            case 'stem': return cal.stem_names[ly % 10];
            case 'branch': return cal.branch_names[ly % 12];
            case 'zodiac': return cal.zodiac_names[ly % 12];
            case 'week': {
                const d = new Date(Date.UTC(year, month - 1, day));
                d.setUTCDate(day + 3 - (d.getUTCDay() + 6) % 7);
                return String(1 + Math.floor((d - new Date(Date.UTC(d.getUTCFullYear(), 0, 1))) / 604800000));
            }
            case 'temperature': return String(runtime.temperature ?? 25);
            case 'voltage': {
                    const factor=10**s.voltage_decimals;
                    const value=Math.floor(((runtime.voltage_mv??3000)*factor+500)/1000);
                    return String(Math.floor(value/factor))+(s.voltage_decimals?'.'+String(value%factor).padStart(s.voltage_decimals,'0'):'');
                }
            case 'ssid': return runtime.ssid ?? 'EPD-1234';
            case 'name_tail': return [...(runtime.ssid ?? 'EPD-1234')].slice(-s.name_tail || Infinity).join('');
            case 'jieqi': return cal.jieqi_names[jq.index];
            case 'jieqi_days': return String(jq.days);
            case 'todo_count': return String(chosen.items.length);
            case 'todo_daily': return String(chosen.daily);
            default: return '';
        } }
        function rich(c) { const chunks = []; let role = 0, color = c.foreground; for (const part of c.text[0].split(/(\{[^{}]+\})/)) {
            if (/^\{f[0-2]\}$/.test(part))
                role = Number(part[2]);
            else if (/^\{c[0-2]\}$/.test(part))
                color = Number(part[2]);
            else if (part)
                chunks.push({ slot: c.fonts[role], color, text: part.startsWith('{') ? binding(part.slice(1, -1), c.style) : part });
        } let w = 0; for (const item of chunks) {
            slot = item.slot;
            w += [...item.text].reduce((sum, c) => sum + advance(c), 0);
        } let x = c.x + (c.align === 1 ? trunc((c.width - w) / 2) : c.align === 2 ? c.width - w : 0); for (const item of chunks) {
            slot = item.slot;
            fg = item.color;
            bg = c.background;
            transparent = true;
            x = text(item.text, x, c.y + c.style.baseline);
        } }
        function clock(c) { const s = c.style, g = model.clockGeometry(c), scale = c.value0; const digits = [trunc(date.getUTCHours() / 10), date.getUTCHours() % 10, trunc(date.getUTCMinutes() / 10), date.getUTCMinutes() % 10]; for (let n = 0; n < 4; n++)
            for (let seg = 0; seg < 7; seg++) {
                const p = `segment_${seg}_`, x = c.x + g.x + (n < 2 ? n * g.step : g.minute + (n - 2) * g.step) + s[p + 'x_scale'] * scale + s[p + 'x_bias'], y = c.y + g.y + s[p + 'y_scale'] * scale + s[p + 'y_bias'], color = s['digit_' + digits[n]] & (1 << seg) ? c.foreground : c.background;
                for (let i = 0; i < scale; i++) {
                    if (s[p + 'horizontal']) {
                        rect(x + scale - i, y + i, s.horizontal_length * scale + 2 * i, 1, color);
                        rect(x + i, y + scale + i, s.horizontal_length * scale + 2 * (scale - i), 1, color);
                    }
                    else {
                        rect(x + i, y + scale - i, 1, s.vertical_length * scale + 2 * i, color);
                        rect(x + scale + i, y + i, 1, s.vertical_length * scale + 2 * (scale - i), color);
                    }
                }
            } for (const key of ['top', 'bottom'])
            rect(c.x + g.x + g.block + s.hour_gap * scale, c.y + g.y + trunc(s['colon_' + key] * scale / 2) + s['colon_' + key + '_bias'], s.colon_width * scale, s.colon_height * scale, c.foreground); }
        function week(c) { const s = c.style, cw = trunc(c.width / 7); slot = c.fonts[0]; const m = metrics(), baseline = trunc((c.height - m.ascent + m.descent) / 2) + m.ascent + s.baseline_shift, tw = width(cal.weekday_names[0]); for (let i = 0; i < 7; i++) {
            const d = ((runtime.week_start ?? 0) + i) % 7, b = (d === 0 || d === 6) ? s.weekend_bg : s.weekday_bg;
            rect(c.x + i * cw, c.y, i === 6 ? cw + c.width % 7 : cw, c.height, b);
            fg = c.foreground;
            bg = b;
            transparent = true;
            text(cal.weekday_names[d], c.x + trunc((cw - tw) / 2) + i * cw, c.y + baseline);
        } }
        function grid(c) {
            const s = c.style, first = new Date(Date.UTC(year, month - 1, 1)).getUTCDay(), offset = (first - (runtime.week_start ?? 0) + 7) % 7, count = days(year, month), rows = 1 + trunc((count - (7 - offset) + 6) / 7), cw = trunc(c.width / 7), ch = trunc(c.height / rows);
            if (s.grid_dot > 0) {
                for (let row = 1; row < rows; row++)
                    for (let x = 0; x < 7 * cw; x += s.grid_dot + s.grid_gap)
                        rect(c.x + x, c.y + row * ch, Math.min(s.grid_dot, 7 * cw - x), 1, s.grid_color);
                for (let col = 1; col < 7; col++)
                    for (let y = 0; y < rows * ch; y += s.grid_dot + s.grid_gap)
                        rect(c.x + col * cw, c.y + y, 1, Math.min(s.grid_dot, rows * ch - y), s.grid_color);
            }
            for (let d = 1; d <= count; d++) {
                const index = d - 1, col = (offset + index) % 7, row = trunc((offset + index) / 7), w = (first + index) % 7, cx = c.x + col * cw, cy = c.y + row * ch, half = s.date_half, bx = cx + trunc((cw - 2 * half) / 2), by = cy + trunc((ch - 2 * half) / 2) + s.cell_y, today = d === day;
                if (today)
                    circle(bx + half, by + half + s.today_y, s.today_radius, c.accent);
                slot = c.fonts[0];
                fg = today ? s.today_text : (w === 0 || w === 6) ? s.weekend_color : c.foreground;
                bg = today ? c.accent : c.background;
                transparent = false;
                let m = metrics();
                const dateBaseline = by - half + m.ascent - m.descent;
                text(String(d), bx + trunc((2 * half - width(String(d))) / 2), dateBaseline);
                const ld = lunar(cal, year, month, d), fest = festival(cal, year, month, d, w, ld), key = `${year}-${String(month).padStart(2, '0')}-${String(d).padStart(2, '0')}`, anno = annotations.find(a => a.date === key), label = anno?.label ?? anno?.text ?? '', corner = anno && anno.position === 1 && label, replace = anno && anno.position !== 1 && label;
                const holiday = year === cal.holiday_year ? cal.holidays.find(h => h.month === month && h.day === d) : null;
                let annoSlot = (anno?.fontSize === 9 || s.ascii_small && /[a-z]/i.test(label)) ? s.annotation_small_font : s.annotation_large_font;
                let subtitle = replace ? label : fest || (ld.day === 1 ? cal.leap_names[ld.leap] + cal.month_names[ld.month] : cal.date_names[ld.day]);
                slot = replace ? annoSlot : c.fonts[1];
                if (replace)
                    fg = anno.color ?? 0;
                else if (fest && !today)
                    fg = s.festival_color;
                bg = today ? c.accent : c.background;
                transparent = true;
                m = metrics();
                text(subtitle, bx + trunc((2 * half - width(subtitle)) / 2), dateBaseline + m.ascent - m.descent + s.label_gap);
                if (corner) {
                    slot = annoSlot;
                    fg = anno.color ?? 0;
                    bg = c.background;
                    m = metrics();
                    text(label, Math.max(cx + s.corner_pad, cx + cw - s.corner_pad - width(label)), cy + s.corner_pad + m.ascent);
                }
                if (holiday) {
                    let tx, ty, hx, hy;
                    if (corner) {
                        tx = cx + s.holiday_pad;
                        ty = cy + ch - s.holiday_pad;
                        hx = tx + s.holiday_radius;
                        hy = ty + s.holiday_baseline;
                    }
                    else {
                        tx = bx + s.holiday_text_x;
                        ty = by + s.holiday_text_y;
                        hx = bx + s.holiday_circle_x;
                        hy = by + s.holiday_circle_y;
                    }
                    if (today) {
                        circle(hx, hy, s.holiday_radius, c.background);
                        circle(hx, hy, s.holiday_radius, c.accent, false);
                    }
                    slot = c.fonts[2];
                    fg = holiday.work ? s.work_color : s.rest_color;
                    bg = c.background;
                    transparent = true;
                    text(c.text[holiday.work ? 0 : 1], tx, ty);
                }
                if (anno && !corner) {
                    const mx = cx + (holiday ? s.dot_pad : cw - s.dot_pad), my = cy + s.dot_y;
                    if (today)
                        circle(mx, my, s.dot_radius + s.dot_border, c.background);
                    circle(mx, my, s.dot_radius, c.accent);
                }
            }
        }
        function todoPane(c) {
            const s = c.style;
            slot = todo.fontSlot ?? (todo.fontSize > 12 || todo.fontBold || todo.fontItalic ? 3 : todo.fontSize === 9 ? 0 : 1);
            if (!fonts[slot])
                slot = todo.fontSize === 9 ? 0 : 1;
            fg = s.text_color;
            bg = c.background;
            transparent = true;
            const m = metrics(), extended = todo.fontSize > 12 || todo.fontBold || todo.fontItalic || m.height > 16;
            if (!chosen.items.length) {
                slot = c.fonts[0];
                text(c.text[0], c.x, c.y + s.empty_baseline);
                return;
            }
            const height = trunc(c.height / (todo.limit || 5)), first = extended ? m.ascent + s.text_top : s.baseline, step = extended ? m.height + s.line_gap : s.line_step, desc = Math.max(m.height - m.ascent, -m.y), two = s.max_lines > 1 && height >= (extended ? 2 * m.height + s.two_line_padding : s.legacy_two_line + desc), x = c.x + s.text_inset + (extended ? Math.max(0, -m.x) : 0), available = c.x + c.width - x;
            function line(str, last) { if (width(str, true).right <= available)
                return [str, '']; let dots = last ? c.text[2] : ''; while (dots && width(dots, true).right > available)
                dots = [...dots].slice(1).join(''); let part = ''; for (const char of str) {
                if (width(part + char + dots, true).right > available)
                    break;
                part += char;
            } return [part + dots, str.slice(part.length)]; }
            chosen.items.forEach((item, row) => { const y = c.y + row * height; clip = [c.x, y, c.x + c.width, y + height]; const markerY = y + (extended ? s.text_top + trunc((m.height - s.marker_size) / 2) : s.marker_y), color = item.phase === 'active' ? s.active_color : item.phase === 'expired' ? s.expired_color : c.foreground; if (item.resident && s.resident_fill)
                rect(c.x, markerY, s.marker_size, s.marker_size, color);
            else
                outline(c.x, markerY, s.marker_size, s.marker_size, color); let str = item.text; for (let n = 0; n < (two ? 2 : 1); n++) {
                const [shown, rest] = line(str, !two || n === 1), baseline = y + first + n * step;
                text(shown, x, baseline);
                if (item.phase === 'expired' && s.strike) {
                    const e = width(shown, true);
                    rect(Math.max(c.x, x + e.left), baseline - trunc(m.ascent / s.strike_divisor), Math.min(c.x + c.width, x + e.right) - Math.max(c.x, x + e.left), s.strike_width, s.text_color);
                }
                str = rest;
                if (!str)
                    break;
            } });
        }
        for (const c of ui.components) {
            if (!(c.modes & (1 << ((runtime.mode ?? 4) - 1))) || (c.flags & 1) || (c.flags & 2) && !(year === 2025 && month === 1) || (c.flags & 4) && !jq.days || (c.flags & 8) && jq.days)
                continue;
            clip = [c.x, c.y, c.x + c.width, c.y + c.height];
            slot = c.fonts[0];
            fg = c.foreground;
            bg = c.background;
            transparent = true;
            switch (c.kind) {
                case 1:
                    rich(c);
                    break;
                case 2:
                    round(c.x, c.y, c.width, c.height, c.style.radius, c.foreground);
                    for (let n = 0; n < c.style.border; n++)
                        round(c.x + n, c.y + n, c.width - 2 * n, c.height - 2 * n, c.style.radius, c.accent, false);
                    break;
                case 3:
                    grid(c);
                    break;
                case 4:
                    clock(c);
                    break;
                case 5:
                    week(c);
                    break;
                case 6:
                    todoPane(c);
                    break;
            }
        }
        return pixels;
    }
    function draw(canvas, ...args) { const pixels = render(...args), ctx = canvas.getContext('2d'); canvas.width = 800; canvas.height = 480; const image = ctx.createImageData(800, 480); for (let i = 0; i < pixels.length; i++) {
        const color = pixels[i], at = i * 4;
        image.data[at] = color ? 255 : 0;
        image.data[at + 1] = color === 1 ? 255 : 0;
        image.data[at + 2] = color === 1 ? 255 : 0;
        image.data[at + 3] = 255;
    } ctx.putImageData(image, 0, 0); return pixels; }
    const api = { render, draw, lunar, jieqi, festival, selection };
    if (node)
        module.exports = api;
    else
        root.EPDUIRenderer = api;
})(globalThis);
