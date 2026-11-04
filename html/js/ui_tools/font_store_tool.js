(function (root) {
  'use strict';
  const node = typeof module !== 'undefined' && module.exports;
  const builder = node ? require('../resource_builder.js') : root.EPDResourceBuilder;
  const store = node ? require('../resource_store.js') : root.EPDResourceStore;
  const bundled = node ? require('./default_fonts.js') : root.EPDDefaultFonts;
  const slots = ['WQY9', 'WQY12', 'HELVB18'];
  const files = ['u8g2_font_wqy9_t_lunar.bin', 'u8g2_font_wqy12_t_lunar.bin', 'u8g2_font_helvB18_tn.bin'];
  const uiText = '年月日星期周正月二月三月四月五月六月七月八月九月十月冬月腊月闰初一二三四五六七八九十廿' +
    '猴鸡狗猪鼠牛虎兔龙蛇马羊庚辛壬癸甲乙丙丁戊己申酉戌亥子丑寅卯辰巳午未' +
    '离还有天元旦节情人妇女植树愚人清明劳动青年儿童建党建军教师国庆万圣平安夜圣诞' +
    '春节元宵龙抬头端午七夕中元中秋重阳寒衣腊八除夕休班℃';
  const asciiText = Array.from({ length: 95 }, (_, i) => String.fromCharCode(32 + i)).join('');
  const requiredText = [uiText + asciiText, uiText, '-0123456789'];
  const word = (bytes, at) => bytes[at] * 256 + bytes[at + 1];
  const signed = value => value > 127 ? value - 256 : value;
  const cached = new WeakMap();
  let bundledCache = null;
  function inspect(bytes, slot) {
    const glyphs = builder.validateFont(bytes, slot);
    const header = {
      glyph_count: bytes[0], bbx_mode: bytes[1], bits_per_0: bytes[2], bits_per_1: bytes[3],
      bits_per_char_width: bytes[4], bits_per_char_height: bytes[5], bits_per_char_x: bytes[6],
      bits_per_char_y: bytes[7], bits_per_delta_x: bytes[8], max_char_width: bytes[9], max_char_height: bytes[10],
      x_offset: signed(bytes[11]), y_offset: signed(bytes[12]), ascent: signed(bytes[13]), descent: signed(bytes[14]),
      ascent_para: signed(bytes[15]), descent_para: signed(bytes[16]), start_upper_A: word(bytes, 17),
      start_lower_a: word(bytes, 19), start_unicode: word(bytes, 21)
    };
    return { header, glyphs };
  }
  function checkSlot(slot, bytes) {
    if (typeof slot === 'string') slot = slots.indexOf(slot.toUpperCase());
    const { header, glyphs } = inspect(bytes, slot);
    if (header.bbx_mode !== 0) throw new Error('字模必须使用兼容的 u8g2 边界框模式');
    const missing = [...new Set(requiredText[slot] || '')].filter(char => !glyphs.has(char.codePointAt(0)));
    if (missing.length) throw new Error(`字体 ${slots[slot]} 缺少界面必需字符：${missing.join('')}`);
    return { slot: slots[slot] || 'TODO', file: files[slot] || 'todo-custom.bin', length: bytes.length,
      crc32: store.crc32(bytes), header, missing_ui_glyphs: [] };
  }
  function pack(fonts) {
    if (!Array.isArray(fonts) || ![3, 4].includes(fonts.length)) throw new Error('必须提供三个基础字体槽位和可选待办槽位');
    fonts.forEach((bytes, slot) => checkSlot(slot, bytes));
    return builder.encodeFonts(fonts);
  }
  async function loadDefault(fetcher = root.fetch) {
    // Classic scripts load from file://; fetch() cannot read the sibling BINs.
    if (root.location?.protocol === 'file:' || typeof fetcher !== 'function') {
      if (!bundledCache) bundledCache = files.map((file, slot) => {
        if (typeof bundled?.[file] !== 'string') throw new Error('网页随附字模缺失，请更新完整网页目录');
        const bytes = Uint8Array.from(root.atob(bundled[file]), char => char.charCodeAt(0));
        checkSlot(slot, bytes); return bytes;
      });
      return bundledCache.map(bytes => bytes.slice());
    }
    if (!cached.has(fetcher)) {
      const loading = Promise.all(files.map(async (file, slot) => {
        const response = await fetcher(`resources/fonts/${file}`, { cache: 'no-cache' });
        if (!response.ok) throw new Error(`无法读取基础字模 ${file}，请检查网页资源目录`);
        const bytes = new Uint8Array(await response.arrayBuffer());
        checkSlot(slot, bytes); return bytes;
      })).catch(error => { cached.delete(fetcher); throw error; });
      cached.set(fetcher, loading);
    }
    return (await cached.get(fetcher)).map(bytes => bytes.slice());
  }
  function requests(calendar, annotationText = '', ui) {
    let text = annotationText;
    const collect = value => {
      if (typeof value === 'string') text += value;
      else if (Array.isArray(value)) value.forEach(collect);
      else if (value && typeof value === 'object') Object.values(value).forEach(collect);
    };
    // Index zero is a firmware sentinel, not a displayed date/month. The
    // non-leap blank is also deliberately missing in the original fonts.
    const content = { ...calendar, month_names: calendar.month_names.slice(1),
      date_names: calendar.date_names.slice(1), leap_names: calendar.leap_names.filter((name, i) => i || name.trim()) };
    collect(content);
    const result = requiredText.map((fixed, slot) => ({ slot, text: fixed + (slot < 2 ? text : '') }));
    if (ui) {
      // Voltage, signed temperature, battery percentage, SSIDs and the SYNC
      // TIME URL use the text slot selected by each component, including
      // printable ASCII.
      for (const component of ui.components) {
        if (ui.version !== 2) throw new Error('请先迁移旧版布局');
        const add = (role, required) => result.push({slot:component.fonts[role],text:required});
        if (component.kind === 1) {
          let role=0;
          for (const part of component.text[0].split(/(\{[^{}]+\})/)) {
            if (/^\{f[0-2]\}$/.test(part)) role=Number(part[2]);
            else if (/^\{c[0-2]\}$/.test(part)) continue;
            else if (part.startsWith('{')) {
              const token=part.slice(1,-1);
              add(role,['year','month','day','month2','day2','week','todo_count','todo_daily','jieqi_days','battery'].includes(token)?'0123456789':
                token==='temperature'?'-0123456789':token==='voltage'?'0123456789.':
                ['ssid','name_tail'].includes(token)?asciiText:text);
            } else add(role,part);
          }
        } else if (component.kind===3) {
          add(0,'0123456789');add(1,text);add(2,component.text.join(''));
        } else if (component.kind===5) add(0,calendar.weekday_names.join(''));
        else if (component.kind===6) add(0,component.text.slice(0,2).join(''));
      }
    }
    return result;
  }
  const api = { inspect, checkSlot, pack, loadDefault, requests };
  if (node) module.exports = api;
  else root.EPDFontStoreTool = api;
})(globalThis);
