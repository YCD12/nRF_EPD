#include "todo_items.h"
#include "ui_resources.h"

#include <stddef.h>
#include <string.h>

#include "resource_layout.h"
#include "resource_store.h"
#include "font_geometry.h"

/* No text buffer in persistent RAM: 416 bytes of scheduling metadata for the
 * maximum list. An individual 80-byte record lives on the main-loop stack.
 */
typedef struct { uint32_t id, start, end; } todo_metadata_t;
static todo_metadata_t m_items[TODO_ITEMS_MAX_RECORDS];
static uint8_t m_flags[TODO_ITEMS_MAX_RECORDS];
static uint8_t m_count;
static uint8_t m_limit = TODO_ITEMS_DEFAULT_LIMIT;
static uint8_t m_font_size = TODO_ITEMS_DEFAULT_FONT_SIZE;
static uint8_t m_font_slot = 1U;
static uint32_t m_font_key;
static uint8_t m_font_style;
static bool m_extended_font;
static uint16_t m_lead_minutes, m_retain_minutes;
static volatile bool m_ready;
static volatile bool m_snapshot_valid;
static todo_selection_t m_snapshot;

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool bytes_zero(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] != 0U) return false;
    }
    return true;
}

/* Match font_builder.js: printable BMP scalar values, excluding Unicode
 * Control and Format categories. Font glyphs cannot represent non-BMP text.
 */
static bool codepoint_displayable(uint16_t code)
{
    static const uint16_t excluded[][2] = {
        {0x0000U, 0x001FU}, {0x007FU, 0x009FU}, {0x00ADU, 0x00ADU},
        {0x0600U, 0x0605U}, {0x061CU, 0x061CU}, {0x06DDU, 0x06DDU},
        {0x070FU, 0x070FU}, {0x0890U, 0x0891U}, {0x08E2U, 0x08E2U},
        {0x180EU, 0x180EU}, {0x200BU, 0x200FU}, {0x202AU, 0x202EU},
        {0x2060U, 0x2064U}, {0x2066U, 0x206FU}, {0xD800U, 0xDFFFU},
        {0xFEFFU, 0xFEFFU}, {0xFFF9U, 0xFFFBU}, {0xFFFEU, 0xFFFFU},
    };
    for (unsigned i = 0U; i < sizeof(excluded) / sizeof(excluded[0]); i++) {
        if (code < excluded[i][0]) return true;
        if (code <= excluded[i][1]) return false;
    }
    return true;
}

static bool text_valid(const uint8_t *bytes, uint8_t length)
{
    uint8_t at = 0U;
    if (length == 0U || length > TODO_ITEMS_TEXT_MAX) return false;
    while (at < length) {
        uint8_t first = bytes[at++];
        uint16_t code;
        if (first < 0x80U) {
            code = first;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            uint8_t next;
            if (at >= length) return false;
            next = bytes[at++];
            if ((next & 0xC0U) != 0x80U) return false;
            code = (uint16_t)(((uint16_t)(first & 0x1FU) << 6) | (next & 0x3FU));
        } else if (first >= 0xE0U && first <= 0xEFU) {
            uint8_t second, third;
            if ((uint16_t)at + 1U >= length) return false;
            second = bytes[at++];
            third = bytes[at++];
            if ((second & 0xC0U) != 0x80U || (third & 0xC0U) != 0x80U ||
                (first == 0xE0U && second < 0xA0U) ||
                (first == 0xEDU && second >= 0xA0U)) return false;
            code = (uint16_t)(((uint16_t)(first & 0x0FU) << 12) |
                              ((uint16_t)(second & 0x3FU) << 6) | (third & 0x3FU));
        } else {
            return false;
        }
        if (!codepoint_displayable(code)) return false;
    }
    return true;
}

static bool record_valid(const uint8_t *raw)
{
    uint32_t start = get_u32(raw + 4U), end = get_u32(raw + 8U);
    uint8_t flags = raw[12], length = raw[13];
    if (get_u32(raw) == 0U || (flags & ~(TODO_ITEM_PERMANENT | TODO_ITEM_COMPLETED)) != 0U ||
        !text_valid(raw + 14U, length)) return false;
    if ((flags & TODO_ITEM_PERMANENT) != 0U) {
        if (start != 0U || end != 0U) return false;
    } else if (end <= start) {
        return false;
    }
    return bytes_zero(raw + 14U + length, TODO_ITEMS_RECORD_SIZE - 14U - length);
}

void todo_items_invalidate(void) { m_snapshot_valid = false; }

void todo_items_reset(void)
{
    /* Hide the list before clearing any metadata visible to the timer. */
    m_ready = false;
    todo_items_invalidate();
    m_count = 0U;
    m_limit = TODO_ITEMS_DEFAULT_LIMIT;
    m_font_size = TODO_ITEMS_DEFAULT_FONT_SIZE;
    m_font_slot = 1U; m_font_key = 0U;
    m_font_style = 0U; m_extended_font = false;
    m_lead_minutes = 0U; m_retain_minutes = 0U;
    memset(m_items, 0, sizeof(m_items));
    memset(m_flags, 0, sizeof(m_flags));
}

bool todo_items_init(void)
{
    uint8_t header[TODO_ITEMS_HEADER_SIZE];
    uint8_t raw[TODO_ITEMS_RECORD_SIZE];
    uint16_t count;
    uint32_t length;
    uint8_t style = 0U;
    uint16_t lead = 0U, retain = 0U;
    rs_result_t result;

    todo_items_reset();
    result = resource_store_open(RS_PART_TODO_ITEMS, &length);
    if (result == RS_NOT_FOUND) {
        m_ready = true;
        return true;
    }
    if (result != RS_OK || length < TODO_ITEMS_HEADER_SIZE ||
        resource_store_read(RS_PART_TODO_ITEMS, 0U, header, sizeof(header)) != RS_OK)
        return false;
    count = get_u16(header + 6U);
    uint8_t version = header[4];
    bool timing = version == TODO_ITEMS_TIMING_VERSION;
    bool typography = version == TODO_ITEMS_TYPOGRAPHY_VERSION;
    bool custom = header[10] == 3U;
    uint8_t tail_size = timing ? TODO_ITEMS_TIMING_TAIL_SIZE : (typography ? 1U : 0U);
    const u8g2_font_placeholder_t *metrics = todo_font_metrics_for_size(header[9]);
    uint32_t tail = TODO_ITEMS_HEADER_SIZE + (uint32_t)count * TODO_ITEMS_RECORD_SIZE;
    if (get_u32(header) != TODO_ITEMS_MAGIC || version < TODO_ITEMS_VERSION || version > TODO_ITEMS_TIMING_VERSION ||
        header[5] != TODO_ITEMS_RECORD_SIZE || count > TODO_ITEMS_MAX_RECORDS ||
        header[8] < 1U || header[8] > TODO_ITEMS_MAX_VISIBLE ||
        header[11] > TODO_ITEMS_TEXT_MAX ||
        (version == TODO_ITEMS_VERSION && !bytes_zero(header + 10U, 6U)) ||
        (!timing && version != TODO_ITEMS_VERSION && !custom) ||
        (!custom && !bytes_zero(header + 10U, 6U)) ||
        (custom && get_u32(header + 12U) == 0U) ||
        length != tail + header[11] + tail_size)
        return false;
    if (tail_size != 0U) {
        if (resource_store_read(RS_PART_TODO_ITEMS, tail, raw, tail_size) != RS_OK ||
            (raw[0] & ~(TODO_FONT_BOLD | TODO_FONT_ITALIC)) != 0U) return false;
        style = raw[0];
        tail += tail_size;
        if (timing) {
            lead = get_u16(raw + 1U); retain = get_u16(raw + 3U);
            if (lead > TODO_ITEMS_MAX_TIMING_MINUTES || retain > TODO_ITEMS_MAX_TIMING_MINUTES) return false;
        }
    }
    bool extended = header[9] > 12U || style != 0U;
    if ((typography && !extended) ||
        (extended ? ((!typography && !timing) || !custom || metrics == NULL ||
                     header[8] > ui_resources_todo_capacity((uint8_t)metrics->max_char_height, true)) :
                    (header[9] != 9U && header[9] != 12U)) ||
        (custom && !extended && header[11] == 0U)) return false;
    if (custom && header[11] != 0U &&
        (resource_store_read(RS_PART_TODO_ITEMS, tail, raw, header[11]) != RS_OK ||
         !text_valid(raw, header[11]))) return false;
    for (uint16_t i = 0U; i < count; i++) {
        if (resource_store_read(RS_PART_TODO_ITEMS,
                TODO_ITEMS_HEADER_SIZE + (uint32_t)i * TODO_ITEMS_RECORD_SIZE,
                raw, sizeof(raw)) != RS_OK || !record_valid(raw)) return false;
        uint32_t id = get_u32(raw);
        for (uint16_t j = 0U; j < i; j++) {
            if (m_items[j].id == id) return false;
        }
        m_items[i].id = id;
        m_items[i].start = get_u32(raw + 4U);
        m_items[i].end = get_u32(raw + 8U);
        m_flags[i] = raw[12];
    }
    m_limit = header[8];
    m_font_size = header[9];
    m_font_slot = custom ? 3U : (m_font_size == 9U ? 0U : 1U);
    m_font_key = custom ? get_u32(header + 12U) : 0U;
    m_extended_font = extended;
    m_font_style = style;
    m_lead_minutes = lead; m_retain_minutes = retain;
    m_count = (uint8_t)count;
    /* A corrupt record never publishes an earlier, partially read list. */
    m_ready = true;
    return true;
}

bool todo_items_ready(void) { return m_ready; }
void todo_items_suspend_display(void) { m_ready = false; todo_items_invalidate(); }
uint8_t todo_items_count(void) { return m_ready ? m_count : 0U; }
uint8_t todo_items_font_size(void) { return m_font_size; }
uint8_t todo_items_font_slot(void) { return m_font_slot; }
uint32_t todo_items_font_key(void) { return m_font_key; }
uint8_t todo_items_font_style(void) { return m_font_style; }
bool todo_items_extended_font(void) { return m_extended_font; }
uint16_t todo_items_lead_minutes(void) { return m_lead_minutes; }
uint16_t todo_items_retain_minutes(void) { return m_retain_minutes; }
uint8_t todo_items_display_limit(void) { return m_limit; }

bool todo_items_read(uint8_t index, todo_item_t *item)
{
    uint8_t raw[TODO_ITEMS_RECORD_SIZE];
    if (item == NULL) return false;
    memset(item, 0, sizeof(*item));
    if (!m_ready || index >= m_count ||
        resource_store_read(RS_PART_TODO_ITEMS,
            TODO_ITEMS_HEADER_SIZE + (uint32_t)index * TODO_ITEMS_RECORD_SIZE,
            raw, sizeof(raw)) != RS_OK || !record_valid(raw) ||
        get_u32(raw) != m_items[index].id || get_u32(raw + 4U) != m_items[index].start ||
        get_u32(raw + 8U) != m_items[index].end || raw[12] != m_flags[index])
        return false;
    item->id = m_items[index].id;
    item->start = m_items[index].start;
    item->end = m_items[index].end;
    item->flags = m_flags[index];
    memcpy(item->text, raw + 14U, raw[13]);
    return true;
}

static bool item_before(uint8_t a, uint8_t b)
{
    bool a_permanent = (m_flags[a] & TODO_ITEM_PERMANENT) != 0U;
    bool b_permanent = (m_flags[b] & TODO_ITEM_PERMANENT) != 0U;
    if (a_permanent != b_permanent) return a_permanent;
    if (m_items[a].start != m_items[b].start) return m_items[a].start < m_items[b].start;
    if (m_items[a].end != m_items[b].end) return m_items[a].end < m_items[b].end;
    return m_items[a].id < m_items[b].id;
}

todo_phase_t todo_items_phase(uint8_t index, uint32_t now)
{
    if (!m_ready || index >= m_count || (m_flags[index] & TODO_ITEM_COMPLETED) != 0U)
        return TODO_PHASE_HIDDEN;
    if ((m_flags[index] & TODO_ITEM_PERMANENT) != 0U) return TODO_PHASE_RESIDENT;
    if (now >= m_items[index].end) {
        return now - m_items[index].end < (uint32_t)m_retain_minutes * 60U ?
            TODO_PHASE_EXPIRED : TODO_PHASE_HIDDEN;
    }
    if (now >= m_items[index].start) return TODO_PHASE_ACTIVE;
    return m_items[index].start - now <= (uint32_t)m_lead_minutes * 60U ?
        TODO_PHASE_UPCOMING : TODO_PHASE_HIDDEN;
}

uint8_t todo_items_daily_count(uint32_t now)
{
    uint8_t count = 0U;
    uint32_t day_start = now - now % 86400U;
    /* The last representable day ends beyond UINT32_MAX; do not wrap its end
     * back to the epoch and incorrectly exclude its scheduled items.
     */
    bool final_day = day_start > UINT32_MAX - 86400U;
    uint32_t day_end = final_day ? UINT32_MAX : day_start + 86400U;
    uint32_t lead = (uint32_t)m_lead_minutes * 60U;
    uint32_t retain = (uint32_t)m_retain_minutes * 60U;
    if (!m_ready) return 0U;
    for (uint8_t i = 0U; i < m_count; i++) {
        if ((m_flags[i] & TODO_ITEM_COMPLETED) != 0U) continue;
        uint32_t start = m_items[i].start > lead ? m_items[i].start - lead : 0U;
        if ((m_flags[i] & TODO_ITEM_PERMANENT) != 0U ||
            ((m_items[i].end > day_start || day_start - m_items[i].end < retain) &&
             (final_day || start < day_end))) count++;
    }
    return count;
}

bool todo_items_select(uint32_t now, todo_selection_t *selection)
{
    if (selection == NULL) return false;
    memset(selection, 0, sizeof(*selection));
    if (!m_ready) return false;
    selection->daily_count = todo_items_daily_count(now);
    for (uint8_t i = 0U; i < m_count; i++) {
        uint8_t insert;
        todo_phase_t phase = todo_items_phase(i, now);
        if (phase == TODO_PHASE_HIDDEN) continue;
        selection->active_count++;
        insert = selection->count;
        while (insert > 0U && item_before(i, selection->index[insert - 1U])) insert--;
        if (insert >= m_limit) continue;
        if (selection->count < m_limit) selection->count++;
        for (uint8_t j = (uint8_t)(selection->count - 1U); j > insert; j--) {
            selection->index[j] = selection->index[j - 1U];
            selection->id[j] = selection->id[j - 1U];
            selection->phase[j] = selection->phase[j - 1U];
        }
        selection->index[insert] = i;
        selection->id[insert] = m_items[i].id;
        selection->phase[insert] = (uint8_t)phase;
    }
    selection->queued_count = (uint8_t)(selection->active_count - selection->count);
    return true;
}

bool todo_items_selection_changed(uint32_t now)
{
    /* Selection clears every byte before inserting rows. Assert that this
     * representation contains no padding before comparing its empty slots.
     */
    typedef char selection_without_padding[
        sizeof(todo_selection_t) == 4U + 6U * TODO_ITEMS_MAX_VISIBLE ? 1 : -1];
    (void)sizeof(selection_without_padding);
    todo_selection_t selection;
    if (!todo_items_select(now, &selection) || !m_snapshot_valid) return true;
    /* The heading shows visible/daily counts only. A task entering or leaving
     * the hidden queue does not change pixels while the visible rows stay the
     * same, so do not spend a full panel refresh on these bookkeeping counts. */
    selection.active_count = m_snapshot.active_count;
    selection.queued_count = m_snapshot.queued_count;
    return memcmp(&selection, &m_snapshot, sizeof(selection)) != 0;
}

void todo_items_successful_refresh(uint32_t now)
{
    todo_selection_t selection;
    if (!todo_items_select(now, &selection)) return;
    m_snapshot_valid = false;
    m_snapshot = selection;
    m_snapshot_valid = true;
}
