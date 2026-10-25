// Generate both Flash payloads and expected web pixels from the editable model.
const fs = require('node:fs'), path = require('node:path');
const defaults = require('../html/js/ui_tools/resource_defaults.js');
const ui = require('../html/js/ui_tools/ui_model.js'), render = require('../html/js/ui_tools/ui_preview.js');
const todo = require('../html/js/todo_calendar.js');
const annotationVM = { TextEncoder, TextDecoder, Uint8Array, DataView, console };
require('node:vm').runInNewContext(fs.readFileSync('html/js/calendar_annotations.js', 'utf8') + '\nglobalThis.api=CalendarAnnotations;', annotationVM);
const annotations = annotationVM.api;
const glyphs = require('../html/js/font_builder.js');
const dir = process.argv[2];
fs.mkdirSync(dir, { recursive: true });
const fonts = ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn'].map(n => new Uint8Array(fs.readFileSync(`html/resources/fonts/u8g2_font_${n}.bin`)));
const now = 1789347600;
const tasks = { items: [{ id: 1, text: 'Read a long UTF8 title 中文', resident: true, completed: false, start: 0, end: 0 },
        { id: 2, text: 'Active', resident: false, completed: false, start: now - 30, end: now + 60 },
        { id: 3, text: 'Expired title wraps across two lines for validation', resident: false, completed: false, start: now - 600, end: now - 10 },
        { id: 4, text: 'Upcoming', resident: false, completed: false, start: now + 60, end: now + 600 }], limit: 5, fontSize: 12, leadMinutes: 10, retainMinutes: 5 };
const annos = [{ date: '2026-09-14', label: '会', fontSize: 12, color: 1, position: 0 },
    { date: '2026-09-25', label: 'A1', fontSize: 9, color: 0, position: 1 },
    { date: '2026-09-20', label: '', fontSize: 0, color: 2, position: 0 }];
const cases = [];
function add(name, mode, layout = ui.defaults(), config = { items: [], limit: 5, fontSize: 12 }, extra = {}) {
    cases.push({ name, mode, layout, config, timestamp: now, week_start: 1, temperature: -12, voltage_mv: 3100, ssid: 'NRF_EPD_LONG_0123456789', ...extra });
}
for (let mode = 1; mode <= 5; mode++)
    add('default-' + mode, mode);
for (const mode of [4, 5])
    add('tasks-' + mode, mode, ui.defaults(), tasks, { annotations: annos });
const moved = ui.defaults();
Object.assign(moved.components.find(c => c.id === 23), { x: 440, y: 10, width: 180, align: 1, foreground: 2 });
moved.components.find(c => c.id === 22).text = ['待办 {todo_count}/{todo_daily}'];
for (const c of moved.components.filter(c => c.kind === 6))
    Object.assign(c.style, { marker_size: 11, active_color: 0, expired_color: 2, text_inset: 20, strike_width: 2, line_gap: 7 });
for (const c of moved.components.filter(c => c.kind === 2)) {
    c.height = Math.min(c.height + 3, 480 - c.y);
    c.foreground = 2;
}
for (const mode of [4, 5])
    add('resource-edit-' + mode, mode, moved, tasks, { annotations: annos });
const cal = ui.defaults();
for (const c of cal.components.filter(c => c.kind === 3))
    Object.assign(c.style, { today_radius: 19, today_y: 2, label_gap: 8, grid_dot: 2, grid_gap: 7, weekend_color: 0, corner_pad: 7, holiday_text_x: 28, holiday_circle_x: 34, dot_radius: 4, dot_pad: 11, ascii_small: 0 });
for (const mode of [1, 2, 4])
    add('calendar-style-' + mode, mode, cal, tasks, { annotations: annos });
const clock = ui.defaults();
for (const c of clock.components.filter(c => c.kind === 4)) {
    Object.assign(c.style, { horizontal_length: 4, vertical_length: 6, offset_x: -2, offset_y: 2, colon_width: 1, colon_height: 1, digit_1: 0x7f });
}
for (const mode of [2, 3])
    add('clock-shape-' + mode, mode, clock, tasks);
for (const mode of [2, 3])
    add('jieqi-' + mode, mode, ui.defaults(), undefined, { timestamp: Math.floor(Date.UTC(2026, 8, 23, 9) / 1000) });
for (const mode of [1, 3])
    add('sync-' + mode, mode, ui.defaults(), undefined, { timestamp: Math.floor(Date.UTC(2025, 0, 1, 9) / 1000) });
add('six-rows', 4, ui.defaults(), { ...tasks, limit: 8, fontSize: 9 }, { annotations: annos, week_start: 6, temperature: -128 });
const clipped = ui.defaults();
Object.assign(clipped.components.find(c => c.id === 20), { x: 70, y: 90, width: 180, height: 330 });
add('narrow-pane', 4, clipped, tasks, { annotations: annos });
const hidden = ui.defaults();
hidden.components.find(c => c.id === 23).flags = 1;
add('hidden-temp', 5, hidden, tasks);
add('voltage-rounding',1,ui.defaults(),undefined,{voltage_mv:3150});
const holidayBackground=ui.defaults();holidayBackground.components.find(c=>c.id===3).background=0;
add('holiday-background',1,holidayBackground,undefined,{timestamp:Math.floor(Date.UTC(2026,8,25,9)/1000),annotations:annos});
(async () => {
    const styled = { ...tasks, fontSize: 20, fontBold: true, fontItalic: true, limit: 6 };
    const requests = todo.fontRequests.call({ ...todo, ...styled });
    const custom = (await glyphs.extendFonts(fonts, requests, { rasterize: async () => ({ width: 5, height: 15, x: -2, y: -2, advance: 8, pixels: new Uint8Array(75).fill(1) }) })).fonts;
    add('custom-font', 4, ui.defaults(), styled, { fonts: custom, annotations: annos });
    const large = fonts.map(f => f.slice());
    large[0].set([30, 38, 252, 248, 30, 248, 30, 248], 9);
    add('external-metrics', 1, ui.defaults(), undefined, { fonts: large });
    const manifests = [];
    for (const c of cases) {
        const folder = path.join(dir, c.name);
        fs.mkdirSync(folder, { recursive: true });
        const activeFonts = c.fonts || fonts;
        activeFonts.forEach((font, i) => fs.writeFileSync(path.join(folder, `font${i}.bin`), font));
        fs.writeFileSync(path.join(folder, 'ui.bin'), ui.encode(c.layout));
        fs.writeFileSync(path.join(folder, 'todo.bin'), todo.pack(c.config));
        const annoAPI = { ...annotations, annotations: (c.annotations || []).slice().sort((a, b) => a.date.localeCompare(b.date)) };
        fs.writeFileSync(path.join(folder, 'annotations.bin'), annotations.pack.call(annoAPI));
        fs.writeFileSync(path.join(folder, 'web.pixels'), render.render(c.layout, defaults.calendar(), activeFonts, c, c.config, c.annotations || []));
        manifests.push({ name: c.name, mode: c.mode, timestamp: c.timestamp, week: c.week_start, temperature: c.temperature, ssid: c.ssid, voltage: c.voltage_mv });
    }
    fs.writeFileSync(path.join(dir, 'cases.json'), JSON.stringify(manifests));
})().catch(error => { console.error(error); process.exitCode = 1; });
