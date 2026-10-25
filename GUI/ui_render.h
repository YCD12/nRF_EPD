/* Resource-driven widgets. Runtime values stay local; appearance and text
 * come exclusively from the validated UIR2 package. Included once by GUI.c. */
static uint16_t ui_color(uint8_t color) {
    return color == 1U ? GFX_WHITE : color == 2U ? GFX_RED : GFX_BLACK;
}

static void DrawClockFace(Adafruit_GFX *gfx, const tm_t *tm, const ui_component_t *c,
                          const int16_t *s, bool local) {
    ui_clock_layout_t layout;
    ui_resource_clock_layout(c, s, &layout);
    int scale = c->value0, step = layout.step, block = layout.block;
    int x = layout.x + (local ? 0 : c->x), y = layout.y + (local ? 0 : c->y);
    int minute = layout.minute;
    const uint8_t digits[] = {tm->tm_hour / 10, tm->tm_hour % 10, tm->tm_min / 10, tm->tm_min % 10};
    for (uint8_t n = 0; n < 4; n++)
        for (uint8_t seg = 0; seg < 7; seg++) {
            const int16_t *a = s + UI_CLOCK_SEGMENT_0_X_SCALE + 5U * seg;
            uint16_t color = ui_color(
                (s[UI_CLOCK_DIGIT_0 + digits[n]] & (1U << seg)) ? c->foreground : c->background);
            Draw7Segment(gfx,
                         x + (n < 2 ? n * step : minute + (n - 2) * step) + a[0] * scale + a[1],
                         y + a[2] * scale + a[3], scale, s[UI_CLOCK_HORIZONTAL_LENGTH] * scale,
                         s[UI_CLOCK_VERTICAL_LENGTH] * scale, a[4] != 0, color);
        }
    for (uint8_t i = 0; i < 2; i++)
        GFX_fillRect(gfx, x + block + s[UI_CLOCK_HOUR_GAP] * scale,
                     y + s[UI_CLOCK_COLON_TOP + 2U * i] * scale / 2 +
                         s[UI_CLOCK_COLON_TOP_BIAS + 2U * i],
                     s[UI_CLOCK_COLON_WIDTH] * scale, s[UI_CLOCK_COLON_HEIGHT] * scale,
                     ui_color(c->foreground));
}

static void DrawTodo(Adafruit_GFX *gfx, const gui_data_t *data, const ui_component_t *c,
                     const int16_t *s) {
    todo_selection_t selection;
    char message[UI_TEXT_BYTES];
    const gui_font_id_t font = (gui_font_id_t)todo_items_font_slot();
    const gui_font_metrics_t *metrics = GUI_GetFontMetrics(font);
    if (!ui_resource_text(c, 2, m_todo_ellipsis, sizeof(m_todo_ellipsis)))
        goto invalid;
    if (font == GUI_FONT_TODO &&
        !GUI_TodoFontMatches(todo_items_font_size(), todo_items_font_key()))
        todo_items_suspend_display();
    gui_set_font(gfx, font);
    GFX_setTextColor(gfx, ui_color(s[UI_TODO_TEXT_COLOR]), ui_color(c->background));
    if (!todo_items_select(data->timestamp, &selection)) {
        gui_set_font(gfx, (gui_font_id_t)c->font_primary);
        s_render_valid = false;
        if (ui_resource_text(c, 1, message, sizeof(message))) {
            const char *cursor = message;
            GFX_setCursor(gfx, c->x, c->y + s[UI_TODO_EMPTY_BASELINE]);
            DrawTodoText(gfx, &cursor, c->w, true, false, s);
        }
        return;
    }
    if (!selection.count) {
        gui_set_font(gfx, (gui_font_id_t)c->font_primary);
        if (ui_resource_text(c, 0, message, sizeof(message))) {
            const char *cursor = message;
            GFX_setCursor(gfx, c->x, c->y + s[UI_TODO_EMPTY_BASELINE]);
            DrawTodoText(gfx, &cursor, c->w, true, false, s);
        } else
            s_render_valid = false;
        return;
    }
    const int16_t row_height = c->h / todo_items_display_limit();
    if (!metrics || row_height < metrics->max_char_height + s[UI_TODO_ROW_PADDING])
        goto invalid;
    const bool extended = todo_items_extended_font() || (metrics && metrics->max_char_height > 16);
    int16_t text_x = c->x + s[UI_TODO_TEXT_INSET];
    int16_t first = s[UI_TODO_BASELINE], line = s[UI_TODO_LINE_STEP];
    bool two_lines = false;
    if (extended) {
        if (!metrics || row_height < metrics->max_char_height + s[UI_TODO_ROW_PADDING])
            goto invalid;
        if (metrics->x_offset < 0)
            text_x -= metrics->x_offset;
        first = metrics->ascent + s[UI_TODO_TEXT_TOP];
        line = metrics->max_char_height + s[UI_TODO_LINE_GAP];
        two_lines = row_height >= 2 * metrics->max_char_height + s[UI_TODO_TWO_LINE_PADDING];
    } else {
        int16_t descent = metrics ? metrics->max_char_height - metrics->ascent : 0;
        if (metrics && descent < -metrics->y_offset)
            descent = -metrics->y_offset;
        two_lines = row_height >= s[UI_TODO_LEGACY_TWO_LINE] + descent;
    }
    if (text_x >= c->x + c->w)
        goto invalid;
    two_lines = two_lines && s[UI_TODO_MAX_LINES] > 1;
    for (uint8_t row = 0; row < selection.count; row++) {
        int16_t y = c->y + row * row_height;
        if (!gui_page_hits(gfx, y, y + row_height))
            continue;
        gfx->clip_top = y;
        gfx->clip_bottom = y + row_height;
        todo_item_t item;
        if (!todo_items_read(selection.index[row], &item)) {
            s_render_valid = false;
            continue;
        }
        int16_t marker_y =
            y + (extended
                     ? s[UI_TODO_TEXT_TOP] + (metrics->max_char_height - s[UI_TODO_MARKER_SIZE]) / 2
                     : s[UI_TODO_MARKER_Y]);
        uint8_t color = selection.phase[row] == TODO_PHASE_ACTIVE    ? s[UI_TODO_ACTIVE_COLOR]
                        : selection.phase[row] == TODO_PHASE_EXPIRED ? s[UI_TODO_EXPIRED_COLOR]
                                                                     : c->foreground;
        if ((item.flags & TODO_ITEM_RESIDENT) && s[UI_TODO_RESIDENT_FILL])
            GFX_fillRect(gfx, c->x, marker_y, s[UI_TODO_MARKER_SIZE], s[UI_TODO_MARKER_SIZE],
                         ui_color(color));
        else
            GFX_drawRect(gfx, c->x, marker_y, s[UI_TODO_MARKER_SIZE], s[UI_TODO_MARKER_SIZE],
                         ui_color(color));
        const char *cursor = item.text;
        GFX_setCursor(gfx, text_x, y + first);
        bool strike = selection.phase[row] == TODO_PHASE_EXPIRED && s[UI_TODO_STRIKE];
        DrawTodoText(gfx, &cursor, c->x + c->w - text_x, !two_lines, strike, s);
        if (two_lines && *cursor) {
            GFX_setCursor(gfx, text_x, y + first + line);
            DrawTodoText(gfx, &cursor, c->x + c->w - text_x, true, strike, s);
        }
    }
    return;
invalid:
    todo_items_suspend_display();
    s_render_valid = false;
}

static void ui_binding(uint8_t token, char *out, uint16_t size, const tm_t *tm,
                       const struct Lunar_Date *lunar, const gui_data_t *data,
                       const int16_t *style) {
    const char *text = NULL;
    int number = 0;
    uint8_t padding = 1;
    switch (token) {
    case UI_TOKEN_YEAR:
        number = tm->tm_year + YEAR0;
        break;
    case UI_TOKEN_MONTH:
        number = tm->tm_mon + 1;
        break;
    case UI_TOKEN_MONTH2:
        number = tm->tm_mon + 1;
        padding = 2;
        break;
    case UI_TOKEN_DAY:
        number = tm->tm_mday;
        break;
    case UI_TOKEN_DAY2:
        number = tm->tm_mday;
        padding = 2;
        break;
    case UI_TOKEN_WEEKDAY:
        text = LUNAR_WeekdayName(tm->tm_wday);
        break;
    case UI_TOKEN_LUNAR_MONTH:
        format_lunar_month(out, LUNAR_LeapName(lunar->IsLeap), LUNAR_MonthName(lunar->Month));
        return;
    case UI_TOKEN_LUNAR_DAY:
        text = LUNAR_DateName(lunar->Date);
        break;
    case UI_TOKEN_STEM:
        text = LUNAR_StemName(LUNAR_GetStem(lunar));
        break;
    case UI_TOKEN_BRANCH:
        text = LUNAR_BranchName(LUNAR_GetBranch(lunar));
        break;
    case UI_TOKEN_ZODIAC:
        text = LUNAR_ZodiacName(LUNAR_GetZodiac(lunar));
        break;
    case UI_TOKEN_WEEK:
        number = GetWeekOfYear(tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday);
        break;
    case UI_TOKEN_TEMPERATURE:
        number = data->temperature;
        break;
    case UI_TOKEN_VOLTAGE: {
        uint16_t factor = 1U;
        uint8_t decimals = style[UI_TEXT_VOLTAGE_DECIMALS];
        for (uint8_t i = 0; i < decimals; i++)
            factor *= 10U;
        unsigned value = ((uint32_t)data->voltage_mv * factor + 500U) / 1000U;
        size_t pos = gui_format_int(out, size, value / factor, 1);
        if (decimals) {
            out[pos++] = '.';
            gui_format_int(out + pos, size - pos, value % factor, decimals);
        }
        return;
    }
    case UI_TOKEN_SSID:
        text = data->ssid;
        break;
    case UI_TOKEN_NAME_TAIL: {
        text = data->ssid;
        unsigned count = 0;
        for (const uint8_t *p = (const uint8_t *)text; *p; p++)
            if ((*p & 0xc0U) != 0x80U)
                count++;
        unsigned keep = style[UI_TEXT_NAME_TAIL];
        while (*text && count > keep) {
            ++text;
            while (((uint8_t)*text & 0xc0U) == 0x80U)
                ++text;
            --count;
        }
        break;
    }
    case UI_TOKEN_JIEQI:
    case UI_TOKEN_JIEQI_DAYS: {
        uint8_t days = 0, jq = GetJieQiStr(tm->tm_year + YEAR0, tm->tm_mon + 1, tm->tm_mday, &days);
        if (token == UI_TOKEN_JIEQI)
            text = LUNAR_JieQiName(jq % 24);
        else
            number = days;
        break;
    }
    case UI_TOKEN_TODO_COUNT:
    case UI_TOKEN_TODO_DAILY: {
        todo_selection_t selection;
        if (!todo_items_select(data->timestamp, &selection)) {
            s_render_valid = false;
            out[0] = 0;
            return;
        }
        number = token == UI_TOKEN_TODO_COUNT ? selection.count : selection.daily_count;
        break;
    }
    default:
        break;
    }
    if (text)
        clock_text_append(out, size, 0, text);
    else
        gui_format_int(out, size, number, padding);
}

/* Run the same template twice: first measure advances with its font switches,
 * then draw at the configured anchor. No sensor/time-dependent sample widths. */
static void DrawResourceText(Adafruit_GFX *gfx, const ui_component_t *c, const tm_t *tm,
                             const struct Lunar_Date *lunar, const gui_data_t *data,
                             const int16_t *style) {
    char source[UI_TEXT_BYTES], value[96];
    if (!ui_resource_text(c, 0, source, sizeof(source))) {
        s_render_valid = false;
        return;
    }
    int16_t width = 0;
    for (uint8_t pass = 0; pass < 2; pass++) {
        gui_set_font(gfx, (gui_font_id_t)c->font_primary);
        GFX_setFontMode(gfx, 1);
        GFX_setTextColor(gfx, ui_color(c->foreground), ui_color(c->background));
        int16_t x = c->x + (c->align == 1 ? (c->w - width) / 2 : c->align == 2 ? c->w - width : 0);
        GFX_setCursor(gfx, pass ? x : 0, c->y + style[UI_TEXT_BASELINE]);
        uint8_t *p = (uint8_t *)source;
        while (*p) {
            uint8_t op = *p;
            if (op == 2 || op == 3) {
                uint8_t n = p[1];
                p += 2;
                if (op == 2) {
                    gui_set_font(gfx, (gui_font_id_t)(&c->font_primary)[n]);
                    GFX_setFontMode(gfx, 1);
                } else
                    GFX_setTextColor(gfx, ui_color(n), ui_color(c->background));
                continue;
            }
            char *text;
            uint8_t *end = NULL, saved = 0;
            if (op == 1) {
                ui_binding(p[1], value, sizeof(value), tm, lunar, data, style);
                text = value;
                p += 2;
            } else {
                text = (char *)p;
                while (*p >= 4)
                    ++p;
                end = p;
                saved = *end;
                *end = 0;
            }
            if (pass)
                GFX_puts(gfx, text);
            else {
                int16_t advance;
                todo_text_extent(gfx, text, NULL, &advance);
                gfx->tx += advance;
            }
            if (end)
                *end = saved;
        }
        if (!pass)
            width = gfx->tx;
    }
}

static void DrawResourceScene(Adafruit_GFX *gfx, const tm_t *tm, const struct Lunar_Date *lunar,
                              const gui_data_t *data) {
    if (data->mode < 1 || data->mode > 5)
        return;
    for (uint8_t i = 0; i < ui_resources_count(); i++) {
        const ui_component_t *c = ui_resource_at(i);
        int16_t style[UI_MAX_PARAMS];
        if (!(c->modes & (1U << (data->mode - 1U))) || (c->flags & UI_FLAG_HIDDEN) ||
            ((c->flags & UI_FLAG_SYNC_ONLY) && !(tm->tm_year + YEAR0 == 2025 && tm->tm_mon == 0)) ||
            !gui_page_hits(gfx, c->y, c->y + c->h))
            continue;
        if (c->flags & (UI_FLAG_JIEQI_AHEAD | UI_FLAG_JIEQI_TODAY)) {
            uint8_t jq_days = 0;
            GetJieQiStr(tm->tm_year + YEAR0, tm->tm_mon + 1, tm->tm_mday, &jq_days);
            if (((c->flags & UI_FLAG_JIEQI_AHEAD) && !jq_days) ||
                ((c->flags & UI_FLAG_JIEQI_TODAY) && jq_days))
                continue;
        }
        if (!ui_resource_params(c, style)) {
            s_render_valid = false;
            return;
        }
        gfx->clip_enabled = true;
        gfx->clip_left = c->x;
        gfx->clip_right = c->x + c->w;
        gfx->clip_top = c->y;
        gfx->clip_bottom = c->y + c->h;
        switch (c->kind) {
        case UI_KIND_TEXT:
            DrawResourceText(gfx, c, tm, lunar, data, style);
            break;
        case UI_KIND_LINE:
            GFX_fillRoundRect(gfx, c->x, c->y, c->w, c->h, style[UI_RECT_RADIUS],
                              ui_color(c->foreground));
            for (int n = 0; n < style[UI_RECT_BORDER]; n++)
                GFX_drawRoundRect(gfx, c->x + n, c->y + n, c->w - 2 * n, c->h - 2 * n,
                                  style[UI_RECT_RADIUS], ui_color(c->accent));
            break;
        case UI_KIND_WEEK:
            DrawWeekHeader(gfx, c, data, style);
            break;
        case UI_KIND_GRID:
            DrawMonthGrid(gfx, tm, data, c, style);
            break;
        case UI_KIND_CLOCK:
            DrawClockFace(gfx, tm, c, style, false);
            break;
        case UI_KIND_TODO:
            DrawTodo(gfx, data, c, style);
            break;
        default:
            break;
        }
        gfx->clip_enabled = false;
    }
}
