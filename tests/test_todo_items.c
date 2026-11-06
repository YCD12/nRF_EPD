#include "todo_items.h"
#include "resource_layout.h"
#include "resource_store.h"
#include "font_geometry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t payload[TODO_ITEMS_MAX_BYTES + 1U];
static uint32_t payload_length;
static rs_result_t open_result = RS_OK;
static int fail_read = -1;
static unsigned read_calls, open_calls;
static bool probe_loading;

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4U; i++) p[i] = (uint8_t)(value >> (i * 8U));
}

rs_result_t resource_store_open(uint16_t id, uint32_t *length)
{
    assert(id == 9U && id == RS_PART_TODO_ITEMS);
    open_calls++;
    *length = payload_length;
    return open_result;
}

rs_result_t resource_store_read(uint16_t id, uint32_t offset, void *data, size_t length)
{
    assert(id == 9U && id == RS_PART_TODO_ITEMS);
    unsigned call = read_calls++;
    if (probe_loading) {
        todo_selection_t selection;
        assert(!todo_items_ready() && todo_items_count() == 0U);
        assert(todo_items_daily_count(150U) == 0U);
        assert(!todo_items_select(150U, &selection) && selection.count == 0U);
        assert(selection.daily_count == 0U);
    }
    if ((int)call == fail_read) return RS_IO;
    if (offset > payload_length || length > payload_length - offset) return RS_INVALID;
    memcpy(data, payload + offset, length);
    return RS_OK;
}

static uint8_t *record(unsigned index)
{
    return payload + TODO_ITEMS_HEADER_SIZE + index * TODO_ITEMS_RECORD_SIZE;
}

static void make_payload(unsigned count, unsigned limit)
{
    memset(payload, 0, sizeof(payload));
    put32(payload, TODO_ITEMS_MAGIC);
    payload[4] = TODO_ITEMS_VERSION;
    payload[5] = TODO_ITEMS_RECORD_SIZE;
    put16(payload + 6U, (uint16_t)count);
    payload[8] = (uint8_t)limit;
    payload[9] = 12U;
    payload_length = TODO_ITEMS_HEADER_SIZE + count * TODO_ITEMS_RECORD_SIZE;
    read_calls = open_calls = 0U;
    fail_read = -1;
    open_result = RS_OK;
    todo_items_reset();
}

static void set_item(unsigned index, uint32_t id, uint32_t start, uint32_t end,
                     unsigned flags, const char *text)
{
    uint8_t *r = record(index);
    size_t length = strlen(text);
    assert(length > 0U && length <= TODO_ITEMS_TEXT_MAX);
    memset(r, 0, TODO_ITEMS_RECORD_SIZE);
    put32(r, id);
    put32(r + 4U, start);
    put32(r + 8U, end);
    r[12] = (uint8_t)flags;
    r[13] = (uint8_t)length;
    memcpy(r + 14U, text, length);
}

static todo_selection_t select_at(uint32_t now)
{
    todo_selection_t s;
    unsigned reads_before = read_calls, opens_before = open_calls;
    assert(todo_items_select(now, &s));
    assert(todo_items_daily_count(now) == s.daily_count);
    for (unsigned i = 0U; i < s.count; ++i)
        assert(todo_items_phase(s.index[i], now) == (todo_phase_t)s.phase[i]);
    (void)todo_items_selection_changed(now);
    assert(read_calls == reads_before && open_calls == opens_before);
    return s;
}

static void test_missing_empty_and_errors(void)
{
    todo_selection_t s;
    make_payload(0U, 5U);
    open_result = RS_NOT_FOUND;
    assert(todo_items_init());
    assert(todo_items_ready() && todo_items_count() == 0U);
    assert(todo_items_font_size() == 12U && todo_items_display_limit() == 5U);
    s = select_at(42U);
    assert(s.count == 0U && s.active_count == 0U && s.queued_count == 0U && s.daily_count == 0U);
    assert(read_calls == 0U);
    assert(todo_items_selection_changed(42U));
    todo_items_successful_refresh(42U);
    assert(!todo_items_selection_changed(0U));
    for (unsigned result = RS_INVALID; result <= RS_WRITE_PROTECTED; result++) {
        if (result == RS_NOT_FOUND) continue;
        open_result = (rs_result_t)result;
        assert(!todo_items_init() && !todo_items_ready() && todo_items_count() == 0U);
        assert(!todo_items_select(42U, &s) && s.count == 0U);
        assert(todo_items_daily_count(42U) == 0U && s.daily_count == 0U);
    }
    make_payload(0U, 8U);
    payload[9] = 9U;
    assert(todo_items_init() && todo_items_count() == 0U);
    assert(todo_items_font_size() == 9U && todo_items_display_limit() == 8U);
    assert(!todo_items_select(0U, NULL));
    assert(!todo_items_read(0U, NULL));
}

static void test_selection_and_expiry(void)
{
    todo_selection_t s;
    todo_item_t item;
    make_payload(9U, 3U);
    set_item(0U, 30U, 100U, 300U, 0U, "third");
    set_item(1U, 20U, 100U, 200U, 0U, "second");
    set_item(2U, 10U, 100U, 200U, 0U, "first");
    set_item(3U, 9U, 0U, 0U, TODO_ITEM_PERMANENT, "常驻");
    set_item(4U, 1U, 0U, 0U, TODO_ITEM_PERMANENT | TODO_ITEM_COMPLETED, "done");
    set_item(5U, 2U, 50U, 200U, TODO_ITEM_COMPLETED, "done too");
    set_item(6U, 99U, 0U, 90U, 0U, "old");
    set_item(7U, 100U, 300U, 400U, 0U, "future");
    set_item(8U, 8U, 0U, 0U, TODO_ITEM_PERMANENT, "always");
    assert(todo_items_init() && todo_items_count() == 9U);
    s = select_at(0U);
    assert(s.count == 3U && s.active_count == 3U && s.queued_count == 0U);
    assert(s.id[0] == 8U && s.id[1] == 9U && s.id[2] == 99U);
    s = select_at(90U);
    assert(s.count == 2U && s.active_count == 2U);
    s = select_at(99U);
    assert(s.count == 2U);
    s = select_at(100U);
    assert(s.count == 3U && s.active_count == 5U && s.queued_count == 2U);
    assert(s.id[0] == 8U && s.id[1] == 9U && s.id[2] == 10U && s.index[2] == 2U);
    assert(todo_items_read(s.index[1], &item));
    assert(item.id == 9U && item.flags == TODO_ITEM_PERMANENT && strcmp(item.text, "常驻") == 0);
    s = select_at(200U);
    assert(s.active_count == 3U && s.queued_count == 0U && s.id[2] == 30U);
    s = select_at(300U);
    assert(s.active_count == 3U && s.id[2] == 100U);
    s = select_at(400U);
    assert(s.count == 2U && s.active_count == 2U);
    s = select_at(UINT32_MAX);
    assert(s.count == 2U);
    memset(&item, 0xA5, sizeof(item));
    assert(!todo_items_read(9U, &item) && item.id == 0U && item.text[0] == '\0');
}

static void test_queue_progression_and_snapshot(void)
{
    todo_selection_t s;
    make_payload(4U, 1U);
    set_item(0U, 1U, 0U, 100U, 0U, "first");
    set_item(1U, 2U, 50U, 150U, 0U, "second");
    set_item(2U, 3U, 100U, 200U, 0U, "third");
    set_item(3U, 4U, 110U, 120U, 0U, "short");
    assert(todo_items_init());
    assert(todo_items_selection_changed(0U));
    /* Repeated comparisons without success must continue requesting refresh. */
    assert(todo_items_selection_changed(0U));
    todo_items_successful_refresh(0U);
    assert(!todo_items_selection_changed(49U));
    assert(!todo_items_selection_changed(50U)); /* Visible rows/header stay; hidden queue grows. */
    todo_items_successful_refresh(50U);
    assert(!todo_items_selection_changed(99U));
    assert(todo_items_selection_changed(100U));
    s = select_at(100U);
    assert(s.id[0] == 2U && s.active_count == 2U && s.queued_count == 1U);
    assert(todo_items_selection_changed(100U)); /* Failed display: no commit. */
    todo_items_successful_refresh(100U);
    assert(!todo_items_selection_changed(109U));
    assert(!todo_items_selection_changed(110U)); /* A later item enters only the hidden queue. */
    todo_items_successful_refresh(110U);
    assert(!todo_items_selection_changed(120U)); /* Hidden short item expires; pixels stay. */
    todo_items_successful_refresh(120U);
    assert(!todo_items_selection_changed(149U));
    assert(todo_items_selection_changed(150U));
    todo_items_successful_refresh(150U);
    assert(!todo_items_selection_changed(199U));
    assert(todo_items_selection_changed(200U));
    todo_items_successful_refresh(200U);
    assert(todo_items_selection_changed(UINT32_MAX)); /* A different day's header total. */
    todo_items_successful_refresh(UINT32_MAX);
    assert(!todo_items_selection_changed(UINT32_MAX));
    assert(todo_items_selection_changed(75U)); /* Backward clock jump revives items. */
    todo_items_successful_refresh(75U);
    assert(!todo_items_selection_changed(80U));
    todo_items_invalidate();
    assert(todo_items_selection_changed(80U));
    todo_items_successful_refresh(80U);
    assert(!todo_items_selection_changed(80U));
    /* Reload resets the successful snapshot, even with unchanged scheduling. */
    assert(todo_items_init() && todo_items_selection_changed(80U));
    todo_items_reset();
    assert(!todo_items_ready() && todo_items_selection_changed(80U));
    todo_items_successful_refresh(80U);
    assert(todo_items_selection_changed(80U));
}

static void test_capacity_and_sort(void)
{
    todo_selection_t s;
    make_payload(32U, 8U);
    for (unsigned i = 0U; i < 32U; i++) {
        set_item(i, 32U - i, 0U, 100U, 0U, "item");
    }
    assert(todo_items_init());
    s = select_at(0U);
    assert(s.count == 8U && s.active_count == 32U && s.queued_count == 24U && s.daily_count == 32U);
    for (unsigned i = 0U; i < 8U; i++) {
        assert(s.id[i] == i + 1U && s.index[i] == 31U - i);
    }
    make_payload(4U, 4U);
    set_item(0U, 1U, 10U, 50U, 0U, "late start");
    set_item(1U, 2U, 0U, 90U, 0U, "later end");
    set_item(2U, 3U, 0U, 80U, 0U, "first end");
    set_item(3U, 4U, 0U, 80U, 0U, "id tie break");
    assert(todo_items_init());
    s = select_at(10U);
    assert(s.id[0] == 3U && s.id[1] == 4U && s.id[2] == 2U && s.id[3] == 1U);
}

static void test_daily_total_and_day_boundaries(void)
{
    const uint32_t today = 10U * 86400U;
    const uint32_t tomorrow = today + 86400U;
    todo_selection_t s;
    make_payload(12U, 1U);
    set_item(0U, 1U, today + 10U, today + 20U, 0U, "expired today");
    set_item(1U, 2U, today + 80000U, today + 81000U, 0U, "later today");
    set_item(2U, 3U, today - 100U, today + 100U, 0U, "from yesterday");
    set_item(3U, 4U, tomorrow - 100U, tomorrow + 100U, 0U, "into tomorrow");
    set_item(4U, 5U, today - 100U, today, 0U, "ends at midnight");
    set_item(5U, 6U, tomorrow, tomorrow + 100U, 0U, "starts tomorrow");
    set_item(6U, 7U, 0U, 0U, TODO_ITEM_PERMANENT, "permanent");
    set_item(7U, 8U, 0U, 0U, TODO_ITEM_PERMANENT | TODO_ITEM_COMPLETED, "done permanent");
    set_item(8U, 9U, today, tomorrow, TODO_ITEM_COMPLETED, "done today");
    set_item(9U, 10U, today - 2U * 86400U, tomorrow + 86400U, 0U, "several days");
    set_item(10U, 11U, today, tomorrow, 0U, "whole today");
    set_item(11U, 12U, tomorrow + 1U, tomorrow + 2U, 0U, "later tomorrow");
    assert(todo_items_init());
    s = select_at(today);
    assert(s.daily_count == 7U && s.count == 1U && s.active_count == 4U);
    s = select_at(today + 40000U);
    assert(s.daily_count == 7U && s.count == 1U && s.active_count == 3U);
    s = select_at(tomorrow - 1U);
    assert(s.daily_count == 7U && s.count == 1U && s.active_count == 4U);
    s = select_at(today - 1U);
    assert(s.daily_count == 4U); /* Today-only items are absent from yesterday. */
    s = select_at(tomorrow);
    assert(s.daily_count == 5U); /* Exact end is excluded; exact start is included. */
    s = select_at(tomorrow + 200U);
    assert(s.daily_count == 5U); /* Today's earlier deadlines remain in its total. */

    /* A midnight total change must redraw the header even when the displayed
     * permanent item and active/queued counts remain identical.
     */
    make_payload(3U, 1U);
    set_item(0U, 1U, 0U, 0U, TODO_ITEM_PERMANENT, "permanent");
    set_item(1U, 2U, today + 10U, today + 20U, 0U, "expired today");
    set_item(2U, 3U, tomorrow + 10U, tomorrow + 20U, 0U, "tomorrow early");
    assert(todo_items_init());
    s = select_at(tomorrow - 1U);
    assert(s.count == 1U && s.active_count == 1U && s.daily_count == 2U);
    todo_items_successful_refresh(tomorrow - 1U);
    assert(!todo_items_selection_changed(tomorrow)); /* Equal totals need no repaint. */
    set_item(2U, 3U, tomorrow + 86400U, tomorrow + 86400U + 20U, 0U, "day after tomorrow");
    assert(todo_items_init());
    todo_items_successful_refresh(tomorrow - 1U);
    s = select_at(tomorrow);
    assert(s.count == 1U && s.active_count == 1U && s.daily_count == 1U);
    assert(todo_items_selection_changed(tomorrow));
    /* A failed display does not acknowledge that new header count. */
    assert(todo_items_selection_changed(tomorrow));
    todo_items_successful_refresh(tomorrow);
    assert(!todo_items_selection_changed(tomorrow + 1U));
    assert(todo_items_selection_changed(tomorrow - 1U)); /* Backward clock change. */

    /* The final partial uint32_t day must not overflow its upper boundary. */
    const uint32_t last_day = UINT32_MAX - UINT32_MAX % 86400U;
    make_payload(4U, 1U);
    set_item(0U, 1U, last_day, UINT32_MAX, 0U, "last day");
    set_item(1U, 2U, last_day - 1U, last_day, 0U, "prior day only");
    set_item(2U, 3U, last_day - 1U, last_day + 1U, 0U, "cross midnight");
    set_item(3U, 4U, 0U, 0U, TODO_ITEM_PERMANENT, "permanent");
    assert(todo_items_init());
    s = select_at(UINT32_MAX);
    assert(s.daily_count == 3U && s.count == 1U && s.active_count == 1U);
    assert(select_at(last_day).daily_count == 3U);
    assert(select_at(last_day - 1U).daily_count == 3U);
    assert(select_at(0U).daily_count == 1U);
    todo_items_reset();
    unsigned reads_before = read_calls, opens_before = open_calls;
    assert(todo_items_daily_count(UINT32_MAX) == 0U);
    assert(read_calls == reads_before && open_calls == opens_before);
}

static void good_one(void)
{
    make_payload(1U, 5U);
    set_item(0U, 1U, 0U, UINT32_MAX, 0U, "valid");
}

static void assert_invalid(void)
{
    todo_selection_t s;
    assert(!todo_items_init() && !todo_items_ready() && todo_items_count() == 0U);
    assert(!todo_items_select(1U, &s) && s.count == 0U && s.active_count == 0U);
}

static void test_corruption(void)
{
    const unsigned header_offsets[] = {0U, 4U, 5U, 10U, 15U};
    for (unsigned i = 0U; i < sizeof(header_offsets) / sizeof(header_offsets[0]); i++) {
        good_one(); payload[header_offsets[i]] ^= 1U; assert_invalid();
    }
    good_one(); payload[8] = 0U; assert_invalid();
    good_one(); payload[8] = 9U; assert_invalid();
    good_one(); payload[9] = 10U; assert_invalid();
    good_one(); put16(payload + 6U, 33U); assert_invalid();
    good_one(); payload_length--; assert_invalid();
    good_one(); payload_length++; assert_invalid();
    good_one(); payload_length = 15U; assert_invalid();
    good_one(); put32(record(0U), 0U); assert_invalid();
    good_one(); put32(record(0U) + 4U, UINT32_MAX); assert_invalid();
    good_one(); put32(record(0U) + 8U, 0U); assert_invalid();
    good_one(); record(0U)[12] = 4U; assert_invalid();
    good_one(); record(0U)[12] = TODO_ITEM_PERMANENT; assert_invalid();
    good_one(); record(0U)[13] = 0U; assert_invalid();
    good_one(); record(0U)[13] = 65U; assert_invalid();
    good_one(); record(0U)[77] = 1U; assert_invalid();
    good_one(); record(0U)[78] = 1U; assert_invalid();
    good_one(); record(0U)[79] = 1U; assert_invalid();
    make_payload(2U, 5U);
    set_item(0U, 1U, 0U, 100U, 0U, "valid first");
    set_item(1U, 1U, 1U, 100U, 0U, "duplicate");
    assert_invalid(); /* Cannot publish first record when the second is invalid. */
    good_one(); fail_read = 0; assert_invalid();
    good_one(); fail_read = 1; assert_invalid();
    make_payload(2U, 5U);
    set_item(0U, 1U, 0U, 100U, 0U, "valid first");
    set_item(1U, 2U, 0U, 100U, 0U, "valid second");
    fail_read = 2;
    assert_invalid();
}

static void test_utf8_and_deferred_read(void)
{
    static const uint8_t bad[][5] = {
        {1U, 0x00U}, {1U, 0x1FU}, {1U, 0x7FU}, {1U, 0x80U},
        {2U, 0xC0U, 0xAFU}, {2U, 0xC1U, 0xBFU}, {2U, 0xC2U, 0x20U},
        {1U, 0xC2U}, {2U, 0xC2U, 0x80U}, {2U, 0xC2U, 0xADU},
        {3U, 0xE0U, 0x80U, 0x80U}, {2U, 0xE4U, 0xB8U},
        {3U, 0xEDU, 0xA0U, 0x80U}, {3U, 0xEFU, 0xBFU, 0xBEU},
        {3U, 0xEFU, 0xBFU, 0xBFU}, {3U, 0xE2U, 0x80U, 0x8BU},
        {3U, 0xE2U, 0x80U, 0xAEU}, {3U, 0xEFU, 0xBBU, 0xBFU},
        {4U, 0xF0U, 0x9FU, 0x98U, 0x80U}, {1U, 0xFFU},
    };
    todo_item_t item;
    for (unsigned i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        good_one();
        memset(record(0U) + 14U, 0, TODO_ITEMS_TEXT_MAX);
        record(0U)[13] = bad[i][0];
        memcpy(record(0U) + 14U, bad[i] + 1U, bad[i][0]);
        assert_invalid();
    }
    good_one();
    set_item(0U, 1U, 0U, UINT32_MAX, 0U, "A 中文 café \xEE\x80\x80");
    assert(todo_items_init() && todo_items_read(0U, &item));
    assert(strcmp(item.text, "A 中文 café \xEE\x80\x80") == 0);
    good_one();
    memset(record(0U) + 14U, 'A', TODO_ITEMS_TEXT_MAX);
    record(0U)[13] = TODO_ITEMS_TEXT_MAX;
    assert(todo_items_init() && todo_items_read(0U, &item));
    assert(strlen(item.text) == TODO_ITEMS_TEXT_MAX && item.text[64] == '\0');
    /* Rendering errors never advance the successful-screen snapshot. */
    fail_read = (int)read_calls;
    assert(!todo_items_read(0U, &item) && item.id == 0U && item.text[0] == '\0');
    assert(todo_items_selection_changed(1U));
    fail_read = -1;
    assert(todo_items_read(0U, &item));
    todo_items_successful_refresh(1U);
    assert(!todo_items_selection_changed(2U));
    /* Reject a stale index after an out-of-band package replacement. */
    put32(record(0U), 2U);
    assert(!todo_items_read(0U, &item));
    put32(record(0U), 1U);
    put32(record(0U) + 4U, 3U);
    assert(!todo_items_read(0U, &item));
    put32(record(0U) + 4U, 0U);
    put32(record(0U) + 8U, 4U);
    assert(!todo_items_read(0U, &item));
    put32(record(0U) + 8U, UINT32_MAX);
    record(0U)[12] = TODO_ITEM_COMPLETED;
    assert(!todo_items_read(0U, &item));
}

static void test_reload_failure_and_reboot_recovery(void)
{
    todo_selection_t selection;
    todo_item_t item;
    make_payload(3U, 2U);
    set_item(0U, 1U, 100U, 200U, 0U, "scheduled");
    set_item(1U, 2U, 0U, 0U, TODO_ITEM_RESIDENT, "permanent");
    set_item(2U, 3U, 0U, 0U, TODO_ITEM_RESIDENT | TODO_ITEM_COMPLETED, "done");
    assert(todo_items_init());
    todo_items_successful_refresh(150U);
    assert(!todo_items_selection_changed(150U));
    /* Reloading hides the entire previous view before any header/record reads,
     * including a timer interruption after some new metadata has been read. */
    probe_loading = true;
    fail_read = (int)read_calls + 2;
    assert(!todo_items_init() && !todo_items_ready());
    assert(todo_items_count() == 0U && !todo_items_read(0U, &item));
    todo_items_successful_refresh(150U);
    assert(todo_items_selection_changed(150U));
    fail_read = -1;
    assert(todo_items_init());
    probe_loading = false;
    assert(todo_items_count() == 3U && todo_items_selection_changed(150U));
    selection = select_at(150U);
    assert(selection.count == 2U && selection.id[0] == 2U && selection.id[1] == 1U);
    todo_items_successful_refresh(150U);
    /* A reboot retains only persisted records: expired/completed items stay
     * hidden when selection is recomputed using the corrected current time. */
    todo_items_reset();
    assert(todo_items_init() && todo_items_selection_changed(250U));
    selection = select_at(250U);
    assert(selection.count == 1U && selection.active_count == 1U && selection.id[0] == 2U);
    /* An uncommitted package after interrupted erase/write is an empty list;
     * restoring a valid package reloads without retaining that empty snapshot. */
    open_result = RS_NOT_FOUND;
    assert(todo_items_init() && todo_items_count() == 0U);
    todo_items_successful_refresh(250U);
    assert(!todo_items_selection_changed(250U));
    open_result = RS_OK;
    assert(todo_items_init() && todo_items_count() == 3U && todo_items_selection_changed(250U));
}

static void test_custom_font_configuration(void)
{
    const char *family = "SimSun";
    for (unsigned size = 9U; size <= 12U; size += 3U) {
        make_payload(1U, 5U);
        set_item(0U, 1U, 0U, 0U, TODO_ITEM_PERMANENT, "A");
        payload[4] = TODO_ITEMS_CUSTOM_VERSION; payload[9] = (uint8_t)size;
        payload[10] = 3U; payload[11] = (uint8_t)strlen(family);
        put32(payload + 12U, 0x12345678UL);
        memcpy(payload + payload_length, family, strlen(family)); payload_length += strlen(family);
        assert(todo_items_init() && todo_items_font_slot() == 3U);
        assert(todo_items_font_size() == size && todo_items_font_key() == 0x12345678UL);
        assert(!todo_items_extended_font() && todo_items_font_style() == 0U);
        assert(todo_items_lead_minutes() == 0U && todo_items_retain_minutes() == 0U);
        todo_items_suspend_display(); assert(!todo_items_ready());
        assert(todo_items_init() && todo_items_ready());
        payload[10] = 2U; assert(!todo_items_init()); payload[10] = 3U;
        put32(payload + 12U, 0U); assert(!todo_items_init()); put32(payload + 12U, 1U);
        payload[payload_length - 1U] = 0U; assert(!todo_items_init());
    }
    make_payload(0U, 5U); payload[9] = 9U;
    assert(todo_items_init() && todo_items_font_slot() == 0U && todo_items_font_key() == 0U);
}

static void make_typography_payload(uint8_t size, uint8_t style, const char *family)
{
    const u8g2_font_placeholder_t *metrics = todo_font_metrics_for_size(size);
    unsigned limit = metrics == NULL ? 1U : 384U / ((unsigned)metrics->max_char_height + 6U);
    make_payload(1U, limit < TODO_ITEMS_MAX_VISIBLE ? limit : TODO_ITEMS_MAX_VISIBLE);
    set_item(0U, 1U, 0U, 0U, TODO_ITEM_PERMANENT, "larger body");
    payload[4] = TODO_ITEMS_TYPOGRAPHY_VERSION; payload[9] = size;
    payload[10] = 3U; payload[11] = (uint8_t)strlen(family);
    put32(payload + 12U, 0xA1B2C3D4UL);
    payload[payload_length++] = style;
    memcpy(payload + payload_length, family, strlen(family));
    payload_length += (uint32_t)strlen(family);
}

static void test_extended_typography_configuration(void)
{
    for (unsigned i = 0U; i < TODO_FONT_SIZE_COUNT; ++i) {
        for (unsigned style = 0U; style <= (TODO_FONT_BOLD | TODO_FONT_ITALIC); ++style) {
            make_typography_payload(todo_font_sizes[i], (uint8_t)style, style & 1U ? "SimSun" : "");
            if (style == 0U && todo_font_sizes[i] <= 12U) { assert_invalid(); continue; }
            assert(todo_items_init() && todo_items_extended_font());
            assert(todo_items_font_slot() == 3U && todo_items_font_size() == todo_font_sizes[i]);
            assert(todo_items_font_style() == style && todo_items_font_key() == 0xA1B2C3D4UL);
            assert(select_at(1U).count == 1U);
        }
    }
    make_typography_payload(32U, TODO_FONT_BOLD | TODO_FONT_ITALIC, "");
    assert(payload[8] == 6U && todo_items_init());
    payload[8] = 7U; assert_invalid();
    make_typography_payload(32U, 4U, ""); assert_invalid();
    make_typography_payload(10U, 0U, ""); assert_invalid();
    make_typography_payload(24U, 0U, "SimSun");
    payload[payload_length - 1U] = 0U; assert_invalid();
    make_typography_payload(24U, 0U, "");
    payload_length--; assert_invalid();
    make_typography_payload(24U, 0U, "");
    payload[payload_length++] = 0U; assert_invalid();
    make_typography_payload(24U, 0U, "");
    fail_read = 1; assert_invalid();
    make_typography_payload(24U, TODO_FONT_ITALIC, "SimSun");
    fail_read = 2; assert_invalid();
    assert(todo_items_font_style() == 0U && !todo_items_extended_font());
    char family[TODO_ITEMS_TEXT_MAX + 1U];
    memset(family, 'A', TODO_ITEMS_TEXT_MAX); family[TODO_ITEMS_TEXT_MAX] = '\0';
    make_typography_payload(16U, TODO_FONT_BOLD, family);
    assert(todo_items_init() && todo_items_font_style() == TODO_FONT_BOLD);
    good_one(); assert(todo_items_init());
    assert(todo_items_font_style() == 0U && !todo_items_extended_font());
}

static void make_timing_payload(unsigned count, unsigned limit, uint8_t size, uint8_t style,
                                const char *family, uint16_t lead, uint16_t retain)
{
    make_payload(count, limit);
    payload[4] = TODO_ITEMS_TIMING_VERSION; payload[9] = size;
    bool custom = size > 12U || style != 0U || family[0] != '\0';
    payload[10] = custom ? 3U : 0U; payload[11] = (uint8_t)strlen(family);
    put32(payload + 12U, custom ? 0xA1B2C3D4UL : 0U);
    payload[payload_length] = style;
    put16(payload + payload_length + 1U, lead);
    put16(payload + payload_length + 3U, retain);
    payload_length += TODO_ITEMS_TIMING_TAIL_SIZE;
    memcpy(payload + payload_length, family, strlen(family));
    payload_length += (uint32_t)strlen(family);
}

static void test_timing_format_and_compatibility(void)
{
    for (unsigned i = 0U; i < TODO_FONT_SIZE_COUNT; ++i) {
        for (unsigned style = 0U; style < 4U; ++style) {
            make_timing_payload(0U, 5U, todo_font_sizes[i], (uint8_t)style, "", 10U, 5U);
            assert(todo_items_init() && todo_items_ready());
            bool extended = todo_font_sizes[i] > 12U || style != 0U;
            assert(todo_items_extended_font() == extended && todo_items_font_style() == style);
            assert(todo_items_font_slot() == (extended ? 3U : (i == 0U ? 0U : 1U)));
            assert(todo_items_lead_minutes() == 10U && todo_items_retain_minutes() == 5U);
        }
    }
    make_timing_payload(0U, 8U, 12U, 0U, "SimSun", 1440U, 1440U);
    assert(todo_items_init() && todo_items_font_slot() == 3U && !todo_items_extended_font());
    assert(todo_items_lead_minutes() == 1440U && todo_items_retain_minutes() == 1440U);
    make_timing_payload(0U, 6U, 32U, 3U, "", 0U, 0U);
    assert(todo_items_init() && todo_items_extended_font());
    payload[8] = 7U; assert_invalid();
    make_timing_payload(0U, 5U, 12U, 0U, "", 1441U, 0U); assert_invalid();
    make_timing_payload(0U, 5U, 12U, 0U, "", 0U, 1441U); assert_invalid();
    make_timing_payload(0U, 5U, 12U, 4U, "", 10U, 5U); assert_invalid();
    make_timing_payload(0U, 5U, 12U, 0U, "", 10U, 5U);
    payload[10] = 3U; put32(payload + 12U, 1U); assert_invalid(); /* Plain custom needs a family. */
    make_timing_payload(0U, 5U, 12U, 0U, "", 10U, 5U);
    payload[11] = 1U; payload[payload_length++] = 'A'; assert_invalid(); /* Base needs zero font metadata. */
    make_timing_payload(0U, 5U, 12U, 0U, "", 10U, 5U);
    put32(payload + 12U, 1U); assert_invalid();
    make_timing_payload(0U, 5U, 16U, 0U, "", 10U, 5U);
    payload[10] = 0U; put32(payload + 12U, 0U); assert_invalid(); /* Large needs generated glyphs. */
    make_timing_payload(0U, 5U, 12U, 0U, "", 10U, 5U);
    payload[10] = 2U; assert_invalid();
    make_timing_payload(0U, 5U, 12U, 0U, "", 10U, 5U);
    payload_length--; assert_invalid();
    make_timing_payload(0U, 5U, 12U, 0U, "", 10U, 5U);
    payload[payload_length++] = 0U; assert_invalid();
    make_timing_payload(0U, 5U, 12U, 0U, "SimSun", 10U, 5U);
    payload[payload_length - 1U] = 0U; assert_invalid();
    for (unsigned fail = 0U; fail < 4U; ++fail) {
        make_timing_payload(1U, 5U, 12U, 0U, "SimSun", 10U, 5U);
        set_item(0U, 1U, 0U, 0U, TODO_ITEM_PERMANENT, "item");
        fail_read = (int)fail; assert_invalid();
        assert(todo_items_lead_minutes() == 0U && todo_items_retain_minutes() == 0U);
    }
    char family[TODO_ITEMS_TEXT_MAX + 1U];
    memset(family, 'A', TODO_ITEMS_TEXT_MAX); family[TODO_ITEMS_TEXT_MAX] = '\0';
    make_timing_payload(32U, 8U, 12U, 0U, family, 1440U, 1440U);
    for (unsigned i = 0U; i < 32U; ++i)
        set_item(i, i + 1U, 0U, 0U, TODO_ITEM_PERMANENT, "full list");
    assert(payload_length == TODO_ITEMS_MAX_BYTES && todo_items_init());
    assert(select_at(0U).active_count == 32U);
    good_one(); assert(todo_items_init());
    assert(todo_items_lead_minutes() == 0U && todo_items_retain_minutes() == 0U);
    assert(todo_items_phase(0U, UINT32_MAX) == TODO_PHASE_HIDDEN);
    make_typography_payload(12U, TODO_FONT_BOLD, "SimSun");
    assert(todo_items_init() && todo_items_lead_minutes() == 0U && todo_items_retain_minutes() == 0U);
}

static void test_phases_and_refresh_boundaries(void)
{
    make_timing_payload(4U, 2U, 12U, 0U, "", 1U, 1U);
    set_item(0U, 1U, 2000U, 2200U, 0U, "scheduled");
    set_item(1U, 2U, 0U, 0U, TODO_ITEM_PERMANENT, "permanent");
    set_item(2U, 3U, 2000U, 2200U, TODO_ITEM_COMPLETED, "done timed");
    set_item(3U, 4U, 0U, 0U, TODO_ITEM_PERMANENT | TODO_ITEM_COMPLETED, "done permanent");
    assert(todo_items_init());
    unsigned reads_before = read_calls, opens_before = open_calls;
    assert(todo_items_phase(0U, 1939U) == TODO_PHASE_HIDDEN);
    assert(todo_items_phase(0U, 1940U) == TODO_PHASE_UPCOMING);
    assert(todo_items_phase(0U, 1999U) == TODO_PHASE_UPCOMING);
    assert(todo_items_phase(0U, 2000U) == TODO_PHASE_ACTIVE);
    assert(todo_items_phase(0U, 2199U) == TODO_PHASE_ACTIVE);
    assert(todo_items_phase(0U, 2200U) == TODO_PHASE_EXPIRED);
    assert(todo_items_phase(0U, 2259U) == TODO_PHASE_EXPIRED);
    assert(todo_items_phase(0U, 2260U) == TODO_PHASE_HIDDEN);
    assert(todo_items_phase(1U, 0U) == TODO_PHASE_RESIDENT);
    assert(todo_items_phase(1U, UINT32_MAX) == TODO_PHASE_RESIDENT);
    assert(todo_items_phase(2U, 2000U) == TODO_PHASE_HIDDEN);
    assert(todo_items_phase(3U, 2000U) == TODO_PHASE_HIDDEN);
    assert(todo_items_phase(4U, 2000U) == TODO_PHASE_HIDDEN);
    todo_selection_t s = select_at(1940U);
    assert(s.count == 2U && s.active_count == 2U && s.daily_count == 2U);
    assert(s.id[0] == 2U && s.phase[0] == TODO_PHASE_RESIDENT);
    assert(s.id[1] == 1U && s.phase[1] == TODO_PHASE_UPCOMING);
    todo_items_successful_refresh(1940U);
    assert(!todo_items_selection_changed(1999U));
    assert(todo_items_selection_changed(2000U)); /* Same IDs/counts; only styling changes. */
    assert(todo_items_selection_changed(2000U)); /* Failed display must retry. */
    todo_items_successful_refresh(2000U);
    assert(!todo_items_selection_changed(2199U));
    assert(todo_items_selection_changed(2200U));
    todo_items_successful_refresh(2200U);
    assert(!todo_items_selection_changed(2259U));
    assert(todo_items_selection_changed(2260U));
    todo_items_successful_refresh(2260U);
    assert(!todo_items_selection_changed(2270U));
    assert(todo_items_selection_changed(2199U)); /* Backwards restores active styling. */
    todo_items_successful_refresh(2199U);
    assert(todo_items_selection_changed(1940U));
    todo_items_successful_refresh(1940U);
    assert(todo_items_selection_changed(1939U));
    assert(read_calls == reads_before && open_calls == opens_before);
    todo_items_reset();
    assert(todo_items_phase(0U, 2000U) == TODO_PHASE_HIDDEN);
}

static void test_phase_queue_and_zero_timing(void)
{
    make_timing_payload(3U, 1U, 12U, 0U, "", 1U, 1U);
    set_item(0U, 1U, 2000U, 2200U, 0U, "first");
    set_item(1U, 2U, 2100U, 2400U, 0U, "second");
    set_item(2U, 3U, 2200U, 2500U, 0U, "third");
    assert(todo_items_init());
    todo_selection_t s = select_at(2200U);
    assert(s.count == 1U && s.active_count == 3U && s.queued_count == 2U);
    assert(s.id[0] == 1U && s.phase[0] == TODO_PHASE_EXPIRED);
    s = select_at(2260U);
    assert(s.id[0] == 2U && s.phase[0] == TODO_PHASE_ACTIVE && s.active_count == 2U);
    record(1U)[12] = TODO_ITEM_COMPLETED;
    assert(todo_items_init());
    s = select_at(2260U); assert(s.id[0] == 3U && s.count == 1U);
    make_timing_payload(1U, 1U, 12U, 0U, "", 0U, 0U);
    set_item(0U, 1U, 100U, 200U, 0U, "zero windows");
    assert(todo_items_init());
    assert(todo_items_phase(0U, 99U) == TODO_PHASE_HIDDEN);
    assert(todo_items_phase(0U, 100U) == TODO_PHASE_ACTIVE);
    assert(todo_items_phase(0U, 199U) == TODO_PHASE_ACTIVE);
    assert(todo_items_phase(0U, 200U) == TODO_PHASE_HIDDEN);
}

static void test_hidden_queue_does_not_change_display_snapshot(void)
{
    make_timing_payload(2U, 1U, 12U, 0U, "", 1U, 1U);
    set_item(0U, 1U, 0U, 0U, TODO_ITEM_RESIDENT, "pin");
    set_item(1U, 2U, 2000U, 2200U, 0U, "hidden");
    assert(todo_items_init());
    todo_items_successful_refresh(1939U);
    const uint32_t times[] = {1940U, 2000U, 2200U, 2260U};
    for (unsigned i = 0U; i < sizeof(times) / sizeof(times[0]); ++i) {
        todo_selection_t s = select_at(times[i]);
        assert(s.count == 1U && s.id[0] == 1U && s.phase[0] == TODO_PHASE_RESIDENT);
        assert(s.daily_count == 2U);
        assert(s.active_count == (i < 3U ? 2U : 1U));
        assert(s.queued_count == (i < 3U ? 1U : 0U));
        assert(!todo_items_selection_changed(times[i]));
    }
    /* The same visible resident still requires a new heading tomorrow. */
    assert(todo_items_selection_changed(86400U));
    todo_items_successful_refresh(86400U);
    assert(!todo_items_selection_changed(86401U));
}

static void test_phase_midnight_and_timestamp_limits(void)
{
    const uint32_t day = 10U * 86400U;
    make_timing_payload(4U, 8U, 12U, 0U, "", 1U, 1U);
    set_item(0U, 1U, day + 30U, day + 300U, 0U, "early yesterday");
    set_item(1U, 2U, day - 300U, day - 30U, 0U, "retained today");
    set_item(2U, 3U, day - 300U, day - 60U, 0U, "retention ends midnight");
    set_item(3U, 4U, day + 86400U + 60U, day + 86400U + 300U, 0U, "early starts tomorrow");
    assert(todo_items_init());
    assert(todo_items_phase(0U, day - 31U) == TODO_PHASE_HIDDEN);
    assert(todo_items_phase(0U, day - 30U) == TODO_PHASE_UPCOMING);
    assert(todo_items_phase(1U, day + 29U) == TODO_PHASE_EXPIRED);
    assert(todo_items_phase(1U, day + 30U) == TODO_PHASE_HIDDEN);
    assert(todo_items_phase(2U, day) == TODO_PHASE_HIDDEN);
    assert(todo_items_daily_count(day - 1U) == 3U);
    assert(todo_items_daily_count(day) == 2U); /* Exact midnight endpoints are excluded. */
    assert(todo_items_daily_count(day + 86400U) == 1U);
    todo_selection_t s = select_at(day);
    assert(s.active_count == 2U && s.active_count <= s.daily_count);

    make_timing_payload(2U, 2U, 12U, 0U, "", 1440U, 1440U);
    set_item(0U, 1U, 30U, 100U, 0U, "saturates epoch");
    set_item(1U, 2U, UINT32_MAX - 100U, UINT32_MAX, 0U, "retained beyond final tick");
    assert(todo_items_init());
    assert(todo_items_phase(0U, 0U) == TODO_PHASE_UPCOMING);
    assert(todo_items_phase(0U, 30U) == TODO_PHASE_ACTIVE);
    assert(todo_items_phase(0U, 100U) == TODO_PHASE_EXPIRED);
    assert(todo_items_phase(0U, 86499U) == TODO_PHASE_EXPIRED);
    assert(todo_items_phase(0U, 86500U) == TODO_PHASE_HIDDEN);
    assert(todo_items_phase(1U, UINT32_MAX - 86501U) == TODO_PHASE_HIDDEN);
    assert(todo_items_phase(1U, UINT32_MAX - 86500U) == TODO_PHASE_UPCOMING);
    assert(todo_items_phase(1U, UINT32_MAX - 100U) == TODO_PHASE_ACTIVE);
    assert(todo_items_phase(1U, UINT32_MAX) == TODO_PHASE_EXPIRED);
    s = select_at(UINT32_MAX);
    assert(s.count == 1U && s.phase[0] == TODO_PHASE_EXPIRED && s.daily_count == 1U);
    assert(todo_items_daily_count(86400U) == 1U); /* Grace crosses into the next day. */
    assert(todo_items_daily_count(2U * 86400U) == 0U);
    todo_items_successful_refresh(UINT32_MAX);
    assert(!todo_items_selection_changed(UINT32_MAX));
    assert(todo_items_selection_changed(UINT32_MAX - 1U));
}

int main(void)
{
    test_missing_empty_and_errors();
    test_selection_and_expiry();
    test_queue_progression_and_snapshot();
    test_capacity_and_sort();
    test_daily_total_and_day_boundaries();
    test_corruption();
    test_utf8_and_deferred_read();
    test_reload_failure_and_reboot_recovery();
    test_custom_font_configuration();
    test_extended_typography_configuration();
    test_timing_format_and_compatibility();
    test_phases_and_refresh_boundaries();
    test_phase_queue_and_zero_timing();
    test_hidden_queue_does_not_change_display_snapshot();
    test_phase_midnight_and_timestamp_limits();
    puts("PASS: TODO v1-v4, early/active/expired phases, daily totals, queue, uint32 bounds and refresh retry");
    return 0;
}
