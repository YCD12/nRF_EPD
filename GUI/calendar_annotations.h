#ifndef CALENDAR_ANNOTATIONS_H
#define CALENDAR_ANNOTATIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Device-owned calendar annotations. Persistence is supplied by the external
 * single-record store; this module owns the active RAM image.
 *
 * Binary layout, little endian:
 *   header (16 bytes): magic "ANNO", version, record_size, count,
 *                      reserved[8] must be zero (glyph tables are rejected)
 *   record (16 bytes): year:u16, month:u8, day:u8, flags:u8 (bit0=present,
 *                      bits1..2=font mode, bits3..4=label color,
 *                      bit5=1 右上角 / 0 日期下方；标签最多 9 字节),
 *                      label_len:u8, label:utf8[9], reserved:u8
 */
#define CALENDAR_ANNOTATIONS_MAGIC       0x4F4E4E41UL
#define CALENDAR_ANNOTATIONS_VERSION     1U
#define CALENDAR_ANNOTATIONS_HEADER_SIZE 16U
#define CALENDAR_ANNOTATIONS_RECORD_SIZE 16U
#define CALENDAR_ANNOTATIONS_MAX_RECORDS 64U
#define CALENDAR_ANNOTATIONS_LABEL_MAX   9U
#define CALENDAR_ANNOTATIONS_MAX_BYTES \
    (CALENDAR_ANNOTATIONS_HEADER_SIZE + \
     CALENDAR_ANNOTATIONS_MAX_RECORDS * CALENDAR_ANNOTATIONS_RECORD_SIZE)

#define CALENDAR_ANNOTATION_FONT_FOLLOW 0U
#define CALENDAR_ANNOTATION_FONT_SMALL  9U
#define CALENDAR_ANNOTATION_FONT_LARGE  12U

#define CALENDAR_ANNOTATION_COLOR_BLACK 0U
#define CALENDAR_ANNOTATION_COLOR_WHITE 1U
#define CALENDAR_ANNOTATION_COLOR_RED   2U

#define CALENDAR_ANNOTATION_POS_BELOW       0U
#define CALENDAR_ANNOTATION_POS_CORNER      1U
#define CALENDAR_ANNOTATION_POS_CORNER_FLAG 0x20U

#define CALENDAR_ANNO_OP_WRITE 0U
#define CALENDAR_ANNO_OP_CLEAR 1U
#define CALENDAR_ANNO_OP_GET   2U

#define CALENDAR_ANNO_NOTIFY     0xA2U
#define CALENDAR_ANNO_NOTIFY_HDR 5U

typedef struct {
    bool present;
    uint8_t font_size;
    uint8_t color;
    uint8_t position;
    char label[CALENDAR_ANNOTATIONS_LABEL_MAX + 1U];
} calendar_annotation_t;

typedef enum {
    CALENDAR_ANNO_RX_ERR = 0,
    CALENDAR_ANNO_RX_MORE,
    CALENDAR_ANNO_RX_DONE,
    CALENDAR_ANNO_RX_RECOVERED,
} calendar_anno_rx_t;

/* Return false to retry later (e.g. the external store is busy). */
typedef bool (*calendar_annotations_reload_fn)(void);

void calendar_annotations_reset(void);
void calendar_annotations_clear(void);
void calendar_annotations_set_reload(calendar_annotations_reload_fn reload);
/* Main loop only. Failed RX hides the live buffer until reload completes. */
bool calendar_annotations_process_reload(void);
bool calendar_annotations_reload_pending(void);
void calendar_annotations_set_live_locked(bool locked);
bool calendar_annotations_load(const uint8_t* data, size_t length);
/* Storage reload reads directly into the existing 1040-byte live buffer so
 * migration does not add a second large RAM copy. Call commit after reading. */
uint8_t* calendar_annotations_load_buffer(uint16_t* capacity);
bool calendar_annotations_load_commit(uint16_t length);
bool calendar_annotations_get(uint16_t year, uint8_t month, uint8_t day,
                              calendar_annotation_t* annotation);
calendar_anno_rx_t calendar_annotations_rx_write(uint16_t offset, uint16_t total,
                                                 const uint8_t* data, uint16_t length);
bool calendar_annotations_rx_active(void);
bool calendar_annotations_rx_cancel(void);
uint16_t calendar_annotations_count(void);
uint16_t calendar_annotations_blob(const uint8_t** data);
uint16_t calendar_annotations_notify_chunk(uint16_t offset, uint8_t* out, uint16_t out_size);

static inline bool calendar_annotation_has_text(const calendar_annotation_t* annotation)
{
    return annotation != NULL && annotation->present && annotation->label[0] != '\0';
}

static inline bool calendar_annotation_replaces_label(const calendar_annotation_t* annotation)
{
    return calendar_annotation_has_text(annotation) &&
           annotation->position == CALENDAR_ANNOTATION_POS_BELOW;
}

static inline bool calendar_annotation_draws_corner(const calendar_annotation_t* annotation)
{
    return calendar_annotation_has_text(annotation) &&
           annotation->position == CALENDAR_ANNOTATION_POS_CORNER;
}

/* 日期下方（有字或空标签）打红点；右上角有字时不打，避免叠在字上。 */
static inline bool calendar_annotation_draws_dot(const calendar_annotation_t* annotation)
{
    return annotation != NULL && annotation->present &&
           !calendar_annotation_draws_corner(annotation);
}

/* wqy12 农历字库只有数字 ASCII；字母必须走已在 Flash 里的 wqy9。 */
static inline bool calendar_annotation_label_has_ascii_letter(const char* label)
{
    const unsigned char* text = (const unsigned char*)label;

    if (text == NULL) {
        return false;
    }
    while (*text != '\0') {
        if ((*text >= (unsigned char)'A' && *text <= (unsigned char)'Z') ||
            (*text >= (unsigned char)'a' && *text <= (unsigned char)'z')) {
            return true;
        }
        text++;
    }
    return false;
}

#endif
