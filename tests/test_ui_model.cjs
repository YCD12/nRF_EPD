const test = require('node:test'), assert = require('node:assert/strict'), fs = require('node:fs');
const ui = require('../html/js/ui_tools/ui_model.js');
const builder = require('../html/js/resource_builder.js');
const render = require('../html/js/ui_tools/ui_preview.js');
const defaults = require('../html/js/ui_tools/resource_defaults.js');
const fontAPI = require('../html/js/font_builder.js');
const fonts = () => ['wqy9_t_lunar', 'wqy12_t_lunar', 'helvB18_tn'].map(n => new Uint8Array(fs.readFileSync(`html/resources/fonts/u8g2_font_${n}.bin`)));
test('UIR2 round-trips all five modes, external styles, templates and drawing order', () => {
    const model = ui.defaults(), bytes = ui.encode(model);
    assert.deepEqual(ui.decode(bytes), model);
    assert.equal(model.components.length, 46);
    assert.deepEqual([...new Set(model.components.flatMap(c => [1, 2, 3, 4, 5].filter(mode => c.modes & (1 << (mode - 1)))))].sort(), [1, 2, 3, 4, 5]);
    for (const mutate of [m => m.components[22].x -= 50, m => m.components[19].style.active_color = 0,
        m => m.components[3].height = 6, m => m.components[22].text = ['{temperature}{f1} C'],
        m => m.components[2].style.dot_radius = 5, m => m.components[16].style.digit_1 = 127]) {
        const next = structuredClone(model);
        mutate(next);
        assert.notDeepEqual(ui.encode(next), bytes);
        assert.deepEqual(ui.decode(ui.encode(next)), next);
    }
});
test('unsupported JSON keys, invalid bounds, tokens, duplicate IDs and unsafe clock shapes fail before sending', () => {
    const edits = [m => m.components[0].color = 2, m => m.foo = 1, m => m.components[0].style.typo = 5,
        m => m.components[0].text = ['{unknown}'], m => m.components[0].text = ['bad\0text'], m => m.components[0].text = ['😀'],
        m => m.components[1].id = m.components[0].id, m => m.components[0].width = 800,
        m => m.components[22].text = ['{f2}missing'], m => m.components[11].style.horizontal_length = 20,
        m => m.components[11].x = 1, m => m.components[0].flags = 12, m => m.components[3].value0 = 1,
        m => m.components[3].align = 1, m => m.components[19].text[2] = '{year}'];
    for (const edit of edits) {
        const m = ui.defaults();
        edit(m);
        assert.throws(() => ui.encode(m));
    }
});
test('binary offsets, record sizes, trailing bytes and malformed template opcodes are checked', () => {
    const encoded = ui.encode(ui.defaults());
    for (const edit of [b => b[0] = 0, b => b[12] = 20, b => b[16 + 23] = 1, b => b[16 + 24] = 0, b => b[16 + 26]++,
        b => b[16 + 27]++, b => b[b[16 + 28] + 256 * b[16 + 29] + 1] = 255]) {
        const bytes = encoded.slice();
        edit(bytes);
        assert.throws(() => ui.decode(bytes));
    }
    assert.throws(() => ui.decode(encoded.subarray(0, encoded.length - 1)));
    assert.throws(() => ui.decode(Uint8Array.from([...encoded, 0])));
});
test('TODO capacity is computed from every visible resource pane and its row padding', () => {
    const m = ui.defaults();
    assert.equal(ui.todoCapacity(m, 58), 6);
    m.components.find(c => c.id === 20).height = 250;
    assert.equal(ui.todoCapacity(m, 58), 3);
    m.components.find(c => c.id === 20).flags = 1;
    assert.equal(ui.todoCapacity(m, 58), 6);
    m.components.find(c => c.id === 21).style.row_padding = 20;
    assert.equal(ui.todoCapacity(m, 58), 4);
    m.components.find(c => c.id === 21).flags = 1;
    assert.equal(ui.todoCapacity(m, 58), 8); // No visible pane constrains the stored preferences.
});
test('temperature movement changes pixels through resource bytes alone', () => {
    const m = ui.defaults(), f = fonts(), runtime = { mode: 5, timestamp: 1789347600, temperature: -128 };
    const before = render.render(m, defaults.calendar(), f, runtime);
    m.components.find(c => c.id === 23).x -= 80;
    const after = render.render(ui.decode(ui.encode(m)), defaults.calendar(), f, runtime);
    assert.notDeepEqual(after, before);
    for (let y = 50; y < 480; y++)
        assert.deepEqual(after.subarray(y * 800, (y + 1) * 800), before.subarray(y * 800, (y + 1) * 800));
});
test('base font resizing persists new metrics and redraws requested glyphs without changing other slots', async () => {
    const input = fonts(), seen = [];
    const next = await fontAPI.extendFonts(input, [{ slot: 0, text: '新' }], { rebuildSlots: [0], slotOptions: () => ({ pixelSize: 24 }),
        rasterize: async (code, slot, header, settings) => { seen.push(settings.pixelSize); return { width: 2, height: 4, x: 0, y: 0, advance: 3, pixels: new Uint8Array(8).fill(1) }; } });
    assert.equal(next.fonts[0][9], 30);
    assert.equal(next.fonts[0][10], 32);
    assert.deepEqual(next.fonts[1], input[1]);
    assert.deepEqual(next.fonts[2], input[2]);
    assert.ok(seen.every(n => n === 24));
    assert.deepEqual(builder.decodeFonts(builder.encodeFonts(next.fonts))[0], next.fonts[0]);
});

test('UIR1 binary and source migrate into editable UIR2 without dropping unknown fields', () => {
    const legacy=structuredClone(require('./fixtures/ui_v1.json'));
    legacy.components[9].y=100;legacy.components[9].value0=124;
    const bytes=new Uint8Array(396),v=new DataView(bytes.buffer);
    v.setUint32(0,0x31524955,true);[1,19,800,480,20,0].forEach((n,i)=>v.setUint16(4+i*2,n,true));
    legacy.components.forEach((c,i)=>{const at=16+i*20;bytes.set([c.mode,c.id,c.kind,c.flags,...c.fonts,0],at);
        ['x','y','width','height','value0','value1'].forEach((key,j)=>v.setInt16(at+8+j*2,c[key],true));});
    const migrated=builder.decodeUI(bytes);assert.equal(migrated.version,2);
    assert.deepEqual(ui.decode(ui.encode(legacy)),migrated);
    assert.equal(migrated.components.find(c=>c.id===10).y+migrated.components.find(c=>c.id===10).style.baseline,100);
    assert.equal(migrated.components.find(c=>c.id===29).y+migrated.components.find(c=>c.id===29).style.baseline,124);
    const store=require('../html/js/resource_store.js'),packer=require('../html/js/ui_tools/resource_pack.js');
    const project=packer.create({fonts:fonts()});
    const migratedProject=packer.validateBundle(project.packages.map(item=>item.pack.id===6?
        {name:item.name,bytes:builder.wrap(6,bytes,1,0)}:item));
    const uiPackage=migratedProject.packages.find(item=>item.pack.id===6);
    assert.equal(uiPackage.pack.revision,2);assert.deepEqual(builder.decodeUI(uiPackage.pack.payload),migrated);
    legacy.components[0].color=2;assert.throws(()=>ui.encode(legacy));
});
