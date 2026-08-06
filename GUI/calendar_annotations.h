#ifndef CALENDAR_ANNOTATIONS_H
#define CALENDAR_ANNOTATIONS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Device-owned calendar annotation resource (type 7, entry 1).
 *
 * Binary layout, little endian:
 *   header (16 bytes): magic "ANNO", version, record_size, count,
 *                      glyph_format, glyph_record_size, glyph_count,
 *                      bitmap_data_offset
 *   record (16 bytes): year:u16, month:u8, day:u8, flags:u8 (bit0=present,
 *                      bits1..2=font mode, bits3..4=label color), label_len:u8,
 *                      label:utf8[9], reserved:u8
 *   optional glyph record (12 bytes): codepoint:u16, pixel_size:u8,
 *                      width:u8, height:u8, advance:u8, x/y offset:i8,
 *                      bitmap_len:u8, reserved:u8, bitmap_offset:u16
 *
 * The resource deliberately stays separate from LCDB: LCDB is generated
 * system data, while annotations are user data that must survive a system
 * resource package update.
 */
#define CALENDAR_ANNOTATIONS_MAGIC          0x4F4E4E41UL
#define CALENDAR_ANNOTATIONS_VERSION        1U
#define CALENDAR_ANNOTATIONS_HEADER_SIZE    16U
#define CALENDAR_ANNOTATIONS_RECORD_SIZE    16U
#define CALENDAR_ANNOTATIONS_MAX_RECORDS    128U
#define CALENDAR_ANNOTATIONS_LABEL_MAX      9U
#define CALENDAR_ANNOTATIONS_GLYPH_RECORD_SIZE 12U
#define CALENDAR_ANNOTATIONS_GLYPH_FORMAT   1U
#define CALENDAR_ANNOTATIONS_MAX_GLYPHS     768U
#define CALENDAR_ANNOTATIONS_GLYPH_MAX_DIM  16U
#define CALENDAR_ANNOTATIONS_GLYPH_BITMAP_MAX 32U

/* Record flag bits 1..2 select an optional label size.  Zero deliberately
 * means "follow the calendar" so every resource written by older webpages
 * keeps the original behaviour. */
#define CALENDAR_ANNOTATION_FONT_FOLLOW      0U
#define CALENDAR_ANNOTATION_FONT_SMALL       9U
#define CALENDAR_ANNOTATION_FONT_LARGE       12U

/* Zero is intentionally black: all existing annotation records have zeroed
 * reserved flag bits and therefore retain a readable default on upgrade. */
#define CALENDAR_ANNOTATION_COLOR_BLACK       0U
#define CALENDAR_ANNOTATION_COLOR_WHITE       1U
#define CALENDAR_ANNOTATION_COLOR_RED         2U

typedef struct {
    bool present;
    uint8_t font_size;
    uint8_t color;
    char label[CALENDAR_ANNOTATIONS_LABEL_MAX + 1U];
} calendar_annotation_t;

typedef struct {
    uint8_t width;
    uint8_t height;
    uint8_t advance;
    int8_t x_offset;
    int8_t y_offset;
    uint8_t bitmap_length;
    uint8_t bitmap[CALENDAR_ANNOTATIONS_GLYPH_BITMAP_MAX];
} calendar_annotation_glyph_t;

/* Drop the lightweight resource metadata cache after RES_COMMIT. */
void calendar_annotations_reset(void);

/* Returns true only when the requested date has a valid user annotation. */
bool calendar_annotations_get(uint16_t year, uint8_t month, uint8_t day,
                              calendar_annotation_t* annotation);

/* Read a browser-generated monochrome fallback glyph for one calendar font. */
bool calendar_annotations_get_glyph(uint16_t codepoint, uint8_t pixel_size,
                                    calendar_annotation_glyph_t* glyph);

#endif
