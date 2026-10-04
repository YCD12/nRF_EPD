#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "u8g2_font.h"

typedef struct {
    const uint8_t* bytes;
    size_t length;
    unsigned calls;
    size_t bytes_read;
    unsigned fail_reads;
} mock_font_t;

static int mock_read(const void* context, uint32_t offset, uint8_t* data, size_t length) {
    mock_font_t* mock = (mock_font_t*)context;

    assert(offset <= mock->length);
    assert(length <= mock->length - offset);
    assert(length <= U8G2_FONT_CACHE_SIZE);
    mock->calls++;
    if (mock->fail_reads != 0U) {
        mock->fail_reads--;
        return -1;
    }
    memcpy(data, mock->bytes + offset, length);
    mock->bytes_read += length;
    return 0;
}

static void make_font(uint8_t* font, size_t length, uint16_t upper_a_offset) {
    memset(font, 0, length);
    font[0] = 1; /* glyph count */
    font[2] = 1; /* background run length bits */
    font[3] = 1; /* foreground run length bits */
    font[4] = 3; /* glyph width bits */
    font[5] = 3; /* glyph height bits */
    font[6] = 3; /* x offset bits */
    font[7] = 3; /* y offset bits */
    font[8] = 4; /* delta x bits */
    font[9] = 8;
    font[10] = 8;
    font[13] = 8;
    font[17] = (uint8_t)(upper_a_offset >> 8);
    font[18] = (uint8_t)upper_a_offset;

    font[23U + upper_a_offset] = 'A';
    font[24U + upper_a_offset] = 4; /* entry size */
}

static void test_external_font_uses_one_block(void) {
    uint8_t font[64];
    mock_font_t mock;
    u8g2_font_source_t source;
    u8g2_font_t ctx;

    make_font(font, sizeof(font), 0);
    mock = (mock_font_t){font, sizeof(font), 0, 0, 0};
    source = (u8g2_font_source_t){NULL, sizeof(font), mock_read, &mock};
    memset(&ctx, 0, sizeof(ctx));

    u8g2_SetExternalFont(&ctx, &source);
    assert(mock.calls == 1);
    assert(mock.bytes_read == sizeof(font));
    u8g2_SetExternalFont(&ctx, &source);
    assert(mock.calls == 1); /* selecting the same source keeps the cache */
    assert(u8g2_IsGlyph(&ctx, 'A') == 1);
    assert(mock.calls == 1); /* parser stayed inside the cached block */
    assert(u8g2_GetGlyphWidth(&ctx, 'A') != 0);
    assert(mock.calls == 1);
}

static void test_external_font_refills_on_block_boundary(void) {
    uint8_t font[512];
    mock_font_t mock;
    u8g2_font_source_t source;
    u8g2_font_t ctx;

    make_font(font, sizeof(font), 237); /* glyph starts at offset 260 */
    mock = (mock_font_t){font, sizeof(font), 0, 0, 0};
    source = (u8g2_font_source_t){NULL, sizeof(font), mock_read, &mock};
    memset(&ctx, 0, sizeof(ctx));

    u8g2_SetExternalFont(&ctx, &source);
    assert(mock.calls == 1);
    assert(u8g2_IsGlyph(&ctx, 'A') == 1);
    assert(mock.calls == 2); /* header block + second cache block */
    assert(u8g2_IsGlyph(&ctx, 'A') == 1);
    assert(mock.calls == 2);
}

static void test_external_font_retries_transient_read_error(void) {
    uint8_t font[512];
    mock_font_t mock;
    u8g2_font_source_t source;
    u8g2_font_t ctx;

    make_font(font, sizeof(font), 237); /* glyph starts at offset 260 */
    mock = (mock_font_t){font, sizeof(font), 0, 0, 1};
    source = (u8g2_font_source_t){NULL, sizeof(font), mock_read, &mock};
    memset(&ctx, 0, sizeof(ctx));

    u8g2_SetExternalFont(&ctx, &source);
    assert(u8g2_IsGlyph(&ctx, 'A') == 1);
    assert(mock.calls == 4); /* failed header + header reload + glyph block */
}

static void test_external_font_retries_multiple_transient_errors(void) {
    uint8_t font[512];
    mock_font_t mock;
    u8g2_font_source_t source;
    u8g2_font_t ctx;

    make_font(font, sizeof(font), 237);
    mock = (mock_font_t){font, sizeof(font), 0, 0, 3};
    source = (u8g2_font_source_t){NULL, sizeof(font), mock_read, &mock};
    memset(&ctx, 0, sizeof(ctx));

    u8g2_SetExternalFont(&ctx, &source);
    assert(u8g2_IsGlyph(&ctx, 'A') == 1);
    assert(mock.calls == 6); /* failed header + header reload + glyph */
}

static void test_external_font_reloads_invalid_header_for_same_source(void) {
    uint8_t font[64];
    mock_font_t mock;
    u8g2_font_source_t source;
    u8g2_font_t ctx;

    make_font(font, sizeof(font), 0);
    mock = (mock_font_t){font, sizeof(font), 0, 0, 1};
    source = (u8g2_font_source_t){NULL, sizeof(font), mock_read, &mock};
    memset(&ctx, 0, sizeof(ctx));

    u8g2_SetExternalFont(&ctx, &source);
    u8g2_SetExternalFont(&ctx, &source);
    assert(u8g2_IsGlyph(&ctx, 'A') == 1U);
    assert(mock.calls == 3U); /* failed initial header + successful reload */
}

static void test_source_switch_and_revision(void) {
    uint8_t a[512], b[512];
    make_font(a, sizeof(a), 237);
    make_font(b, sizeof(b), 0);
    b[23] = 'B';
    mock_font_t ma = {a, sizeof(a), 0, 0, 0}, mb = {b, sizeof(b), 0, 0, 0};
    u8g2_font_source_t sa = {NULL, sizeof(a), mock_read, &ma, 1};
    u8g2_font_source_t sb = {NULL, sizeof(b), mock_read, &mb, 1};
    u8g2_font_t ctx = {0};
    u8g2_SetExternalFont(&ctx, &sa);
    assert(u8g2_IsGlyph(&ctx, 'A'));
    u8g2_SetExternalFont(&ctx, &sb);
    assert(!u8g2_IsGlyph(&ctx, 'A') && u8g2_IsGlyph(&ctx, 'B'));
    u8g2_SetExternalFont(&ctx, &sa);
    assert(u8g2_IsGlyph(&ctx, 'A'));
    /* Same reader/context/length can now contain a different resource. */
    a[260] = 'B'; ++sa.revision;
    u8g2_SetExternalFont(&ctx, &sa);
    assert(!u8g2_IsGlyph(&ctx, 'A') && u8g2_IsGlyph(&ctx, 'B'));
    /* Force source-slot eviction, then return to the other original font. */
    for (unsigned i = 0; i < 5; ++i) {
        ++sa.revision;
        u8g2_SetExternalFont(&ctx, &sa);
        assert(u8g2_IsGlyph(&ctx, 'B'));
    }
    u8g2_SetExternalFont(&ctx, &sb);
    assert(!u8g2_IsGlyph(&ctx, 'A') && u8g2_IsGlyph(&ctx, 'B'));
}

int main(void) {
    test_external_font_uses_one_block();
    test_external_font_refills_on_block_boundary();
    test_external_font_retries_transient_read_error();
    test_external_font_retries_multiple_transient_errors();
    test_external_font_reloads_invalid_header_for_same_source();
    test_source_switch_and_revision();
    puts("PASS: u8g2 external font block cache and retry");
    return 0;
}
