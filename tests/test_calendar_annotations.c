#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "calendar_annotations.h"

static uint8_t blob[CALENDAR_ANNOTATIONS_MAX_BYTES];

static void write_u16_le(uint8_t* dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t* dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void set_record(uint8_t* records, uint16_t index, uint16_t year, uint8_t month, uint8_t day,
                       uint8_t flags, const char* label)
{
    uint8_t* record = records + (size_t)index * CALENDAR_ANNOTATIONS_RECORD_SIZE;
    size_t length = strlen(label);

    assert(length <= CALENDAR_ANNOTATIONS_LABEL_MAX);
    memset(record, 0, CALENDAR_ANNOTATIONS_RECORD_SIZE);
    write_u16_le(record, year);
    record[2] = month;
    record[3] = day;
    record[4] = flags;
    record[5] = (uint8_t)length;
    memcpy(record + 6, label, length);
}

static size_t make_blob(uint8_t* out, uint16_t count)
{
    memset(out, 0, CALENDAR_ANNOTATIONS_HEADER_SIZE);
    write_u32_le(out, CALENDAR_ANNOTATIONS_MAGIC);
    out[4] = CALENDAR_ANNOTATIONS_VERSION;
    out[5] = CALENDAR_ANNOTATIONS_RECORD_SIZE;
    write_u16_le(out + 6, count);
    return CALENDAR_ANNOTATIONS_HEADER_SIZE + (size_t)count * CALENDAR_ANNOTATIONS_RECORD_SIZE;
}

static void test_lookup_replaces_festival_and_empty_label_is_mark_only(void)
{
    calendar_annotation_t annotation;
    size_t length = make_blob(blob, 3);

    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 8, 2, 0x01, "休");
    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 1, 2026, 8, 5,
               (uint8_t)(0x01U | (2U << 1) | (2U << 3)), "生日");
    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 2, 2027, 1, 1, 0x01, "");

    calendar_annotations_reset();
    assert(calendar_annotations_load(blob, length));

    assert(calendar_annotations_get(2026, 8, 5, &annotation));
    assert(annotation.present);
    assert(calendar_annotation_replaces_label(&annotation));
    assert(calendar_annotation_draws_dot(&annotation));
    assert(strcmp(annotation.label, "生日") == 0);
    assert(annotation.font_size == CALENDAR_ANNOTATION_FONT_LARGE);
    assert(annotation.color == CALENDAR_ANNOTATION_COLOR_RED);

    assert(calendar_annotations_get(2026, 8, 2, &annotation));
    assert(annotation.font_size == CALENDAR_ANNOTATION_FONT_FOLLOW);
    assert(annotation.color == CALENDAR_ANNOTATION_COLOR_BLACK);
    assert(annotation.position == CALENDAR_ANNOTATION_POS_BELOW);

    assert(calendar_annotations_get(2027, 1, 1, &annotation));
    assert(annotation.present);
    assert(!calendar_annotation_replaces_label(&annotation));
    assert(annotation.label[0] == '\0');

    assert(!calendar_annotations_get(2026, 8, 3, &annotation));
}

static void test_position_corner_uses_same_label_limit(void)
{
    calendar_annotation_t annotation;
    size_t length = make_blob(blob, 2);

    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 9, 8,
               (uint8_t)(0x01U | CALENDAR_ANNOTATION_POS_CORNER_FLAG), "休班");
    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 1, 2026, 9, 9,
               (uint8_t)(0x01U | CALENDAR_ANNOTATION_POS_CORNER_FLAG), "元旦节");
    calendar_annotations_reset();
    assert(calendar_annotations_load(blob, length));
    assert(calendar_annotations_get(2026, 9, 8, &annotation));
    assert(annotation.position == CALENDAR_ANNOTATION_POS_CORNER);
    assert(calendar_annotation_draws_corner(&annotation));
    assert(!calendar_annotation_replaces_label(&annotation));
    assert(!calendar_annotation_draws_dot(&annotation));
    assert(strcmp(annotation.label, "休班") == 0);
    assert(calendar_annotations_get(2026, 9, 9, &annotation));
    assert(strcmp(annotation.label, "元旦节") == 0);
    assert(calendar_annotation_label_has_ascii_letter("A"));
    assert(!calendar_annotation_label_has_ascii_letter("休"));

    length = make_blob(blob, 1);
    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 9, 10,
               (uint8_t)(0x01U | CALENDAR_ANNOTATION_POS_CORNER_FLAG), "123456789");
    blob[CALENDAR_ANNOTATIONS_HEADER_SIZE + 5U] = CALENDAR_ANNOTATIONS_LABEL_MAX + 1U;
    calendar_annotations_reset();
    assert(!calendar_annotations_load(blob, length));
}

static void test_rejects_glyph_table_and_oversize(void)
{
    calendar_annotation_t annotation;
    size_t length = make_blob(blob, 1);

    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 1, 1, 0x01, "A");
    blob[8] = 1U; /* glyph format from the old external-flash payload */
    calendar_annotations_reset();
    assert(!calendar_annotations_load(blob, length));
    assert(!calendar_annotations_get(2026, 1, 1, &annotation));

    {
        uint8_t oversized[CALENDAR_ANNOTATIONS_MAX_BYTES + CALENDAR_ANNOTATIONS_RECORD_SIZE];
        size_t too_big = make_blob(oversized, CALENDAR_ANNOTATIONS_MAX_RECORDS + 1U);
        calendar_annotations_reset();
        assert(!calendar_annotations_load(oversized, too_big));
    }
}

static void test_rejects_unsorted_and_bad_label(void)
{
    size_t length = make_blob(blob, 2);

    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 8, 5, 0x01, "b");
    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 1, 2026, 8, 2, 0x01, "a");
    calendar_annotations_reset();
    assert(!calendar_annotations_load(blob, length));

    length = make_blob(blob, 1);
    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 8, 2, 0x01, "ok");
    blob[CALENDAR_ANNOTATIONS_HEADER_SIZE + 5U] = CALENDAR_ANNOTATIONS_LABEL_MAX + 1U;
    calendar_annotations_reset();
    assert(!calendar_annotations_load(blob, length));
}

static void test_load_accepts_fds_word_padding(void)
{
    calendar_annotation_t annotation;
    uint8_t padded[CALENDAR_ANNOTATIONS_HEADER_SIZE + CALENDAR_ANNOTATIONS_RECORD_SIZE + 4];
    size_t length = make_blob(padded, 1);

    set_record(padded + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 3, 3, 0x01, "休");
    padded[length] = 0;
    padded[length + 1] = 0;
    calendar_annotations_reset();
    assert(calendar_annotations_load(padded, length + 2U));
    assert(calendar_annotations_get(2026, 3, 3, &annotation));
    assert(strcmp(annotation.label, "休") == 0);
    assert(calendar_annotations_blob(NULL) == (uint16_t)length);
}

static void test_rx_chunks_install_and_clear(void)
{
    calendar_annotation_t annotation;
    size_t length = make_blob(blob, 1);
    uint8_t first[CALENDAR_ANNOTATIONS_HEADER_SIZE + 8];
    calendar_anno_rx_t rx;

    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 9, 8, 0x01, "班");
    memcpy(first, blob, sizeof(first));

    calendar_annotations_reset();
    rx = calendar_annotations_rx_write(0, (uint16_t)length, first, sizeof(first));
    assert(rx == CALENDAR_ANNO_RX_MORE);
    assert(!calendar_annotations_get(2026, 9, 8, &annotation));

    rx = calendar_annotations_rx_write((uint16_t)sizeof(first), (uint16_t)length,
                                       blob + sizeof(first), (uint16_t)(length - sizeof(first)));
    assert(rx == CALENDAR_ANNO_RX_DONE);
    assert(calendar_annotations_get(2026, 9, 8, &annotation));
    assert(strcmp(annotation.label, "班") == 0);

    calendar_annotations_clear();
    assert(!calendar_annotations_get(2026, 9, 8, &annotation));
    assert(calendar_annotations_count() == 0);
}

static void test_notify_chunks_reassemble_live_blob(void)
{
    uint8_t first[32];
    uint8_t second[32];
    uint8_t rebuilt[64];
    const uint8_t* live = NULL;
    uint16_t total;
    uint16_t n;
    size_t length = make_blob(blob, 1);

    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 9, 8, 0x01, "班");
    calendar_annotations_reset();
    assert(calendar_annotations_load(blob, length));
    total = calendar_annotations_blob(&live);
    assert(total == (uint16_t)length);

    n = calendar_annotations_notify_chunk(0, first, 20);
    assert(n == 20);
    assert(first[0] == CALENDAR_ANNO_NOTIFY);
    assert((uint16_t)(first[1] | (first[2] << 8)) == 0);
    assert((uint16_t)(first[3] | (first[4] << 8)) == total);

    n = calendar_annotations_notify_chunk(15, second, 32);
    assert(n == 5U + (total - 15U));
    assert((uint16_t)(second[1] | (second[2] << 8)) == 15);

    memset(rebuilt, 0x5A, sizeof(rebuilt));
    memcpy(rebuilt, first + 5, 15);
    memcpy(rebuilt + 15, second + 5, total - 15);
    assert(memcmp(rebuilt, live, total) == 0);
    assert(calendar_annotations_notify_chunk(total, first, 20) == 0);
}

static uint8_t g_reload_blob[CALENDAR_ANNOTATIONS_MAX_BYTES];
static size_t g_reload_len;
static unsigned g_reload_calls;

static void test_reload_from_snapshot(void)
{
    g_reload_calls++;
    (void)calendar_annotations_load(g_reload_blob, g_reload_len);
}

static void test_rx_rejects_gap_and_keeps_previous(void)
{
    calendar_annotation_t annotation;
    size_t length = make_blob(blob, 1);

    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 2, 2, 0x01, "元旦节");
    calendar_annotations_reset();
    assert(calendar_annotations_load(blob, length));

    assert(calendar_annotations_rx_write(16, 32, blob, 8) == CALENDAR_ANNO_RX_ERR);
    assert(calendar_annotations_get(2026, 2, 2, &annotation));
    assert(strcmp(annotation.label, "元旦节") == 0);
}

static void test_rx_failed_after_start_reloads_previous(void)
{
    calendar_annotation_t annotation;
    uint8_t first[CALENDAR_ANNOTATIONS_HEADER_SIZE + 8];
    size_t new_len;

    g_reload_len = make_blob(g_reload_blob, 1);
    set_record(g_reload_blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 2, 2, 0x01, "元旦节");
    g_reload_calls = 0;

    calendar_annotations_reset();
    calendar_annotations_set_reload(test_reload_from_snapshot);
    assert(calendar_annotations_load(g_reload_blob, g_reload_len));

    new_len = make_blob(blob, 1);
    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 9, 8, 0x01, "班");
    memcpy(first, blob, sizeof(first));

    assert(calendar_annotations_rx_write(0, (uint16_t)new_len, first, sizeof(first)) ==
           CALENDAR_ANNO_RX_MORE);
    assert(!calendar_annotations_get(2026, 2, 2, &annotation));
    assert(!calendar_annotations_get(2026, 9, 8, &annotation));

    assert(calendar_annotations_rx_write(16, (uint16_t)new_len, blob, 8) ==
           CALENDAR_ANNO_RX_RECOVERED);
    assert(g_reload_calls >= 1U);
    assert(calendar_annotations_get(2026, 2, 2, &annotation));
    assert(strcmp(annotation.label, "元旦节") == 0);
    calendar_annotations_set_reload(NULL);
}

static void test_rx_offset0_rejected_when_locked(void)
{
    calendar_annotation_t annotation;
    size_t length = make_blob(blob, 1);

    set_record(blob + CALENDAR_ANNOTATIONS_HEADER_SIZE, 0, 2026, 2, 2, 0x01, "元旦节");
    calendar_annotations_reset();
    assert(calendar_annotations_load(blob, length));

    calendar_annotations_set_live_locked(true);
    assert(calendar_annotations_rx_write(0, (uint16_t)length, blob, 16) == CALENDAR_ANNO_RX_ERR);
    assert(calendar_annotations_get(2026, 2, 2, &annotation));
    assert(strcmp(annotation.label, "元旦节") == 0);

    calendar_annotations_set_live_locked(false);
    assert(calendar_annotations_rx_write(0, (uint16_t)length, blob, (uint16_t)length) ==
           CALENDAR_ANNO_RX_DONE);
}

int main(void)
{
    test_lookup_replaces_festival_and_empty_label_is_mark_only();
    test_position_corner_uses_same_label_limit();
    test_rejects_glyph_table_and_oversize();
    test_rejects_unsorted_and_bad_label();
    test_load_accepts_fds_word_padding();
    test_rx_chunks_install_and_clear();
    test_notify_chunks_reassemble_live_blob();
    test_rx_rejects_gap_and_keeps_previous();
    test_rx_failed_after_start_reloads_previous();
    test_rx_offset0_rejected_when_locked();
    puts("PASS: calendar annotations");
    return 0;
}
