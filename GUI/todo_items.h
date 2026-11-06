#ifndef TODO_ITEMS_H
#define TODO_ITEMS_H

#include <stdbool.h>
#include <stdint.h>

/* TODO v1 payload inside the normal ERS package in partition 9.
 * All multibyte fields are little endian. Timestamps use the device's local
 * wall-clock epoch, matching browser datetime-local values.
 *
 * Header (16): magic:u32, version:u8, record_size:u8, count:u16,
 *              display_limit:u8, font_size:u8, reserved[6]=0.
 * Custom-font v2 replaces reserved bytes with slot=3, UTF-8 family length,
 * font_key:u32 and appends that family after the final record. Typography v3
 * uses the same header, accepts an empty family for the browser default, and
 * appends a style byte (bold=1, italic=2) followed by the family bytes. Plain
 * 9/12 pt retains v1/v2; v3 is used for larger sizes or a non-default style.
 * Timing v4 appends style:u8, lead_minutes:u16, retain_minutes:u16 and then
 * the optional family. Header slot=0/key=0/family_length=0 uses a plain base
 * font; slot=3/key!=0 uses a custom or generated font. Older formats retain
 * zero lead and retention so an update preserves their scheduling behavior.
 * Record (80): id:u32, start:u32, end:u32, flags:u8, text_length:u8,
 *              UTF-8 text[64] (unused bytes zero), reserved[2]=0.
 * A permanent record has start=end=0. Other records require end>start.
 * Completed records are retained in storage and omitted from the display.
 */
#define TODO_ITEMS_MAGIC             0x4F444F54UL
#define TODO_ITEMS_VERSION           1U
#define TODO_ITEMS_CUSTOM_VERSION    2U
#define TODO_ITEMS_TYPOGRAPHY_VERSION 3U
#define TODO_ITEMS_TIMING_VERSION     4U
#define TODO_ITEMS_TIMING_TAIL_SIZE   5U
#define TODO_ITEMS_MAX_TIMING_MINUTES 1440U
#define TODO_ITEMS_DEFAULT_LEAD_MINUTES 10U
#define TODO_ITEMS_DEFAULT_RETAIN_MINUTES 5U
#define TODO_ITEMS_HEADER_SIZE       16U
#define TODO_ITEMS_RECORD_SIZE       80U
#define TODO_ITEMS_MAX_RECORDS       32U
#define TODO_ITEMS_TEXT_MAX          64U
#define TODO_ITEMS_MAX_VISIBLE       8U
#define TODO_ITEMS_DEFAULT_LIMIT     5U
#define TODO_ITEMS_DEFAULT_FONT_SIZE 12U
#define TODO_ITEMS_MAX_BYTES \
    (TODO_ITEMS_HEADER_SIZE + TODO_ITEMS_MAX_RECORDS * TODO_ITEMS_RECORD_SIZE + TODO_ITEMS_TEXT_MAX + TODO_ITEMS_TIMING_TAIL_SIZE)

#define TODO_FONT_BOLD   0x01U
#define TODO_FONT_ITALIC 0x02U

#define TODO_ITEM_PERMANENT 0x01U
#define TODO_ITEM_RESIDENT TODO_ITEM_PERMANENT
#define TODO_ITEM_COMPLETED 0x02U

typedef enum {
    TODO_PHASE_HIDDEN = 0,
    TODO_PHASE_UPCOMING,
    TODO_PHASE_ACTIVE,
    TODO_PHASE_EXPIRED,
    TODO_PHASE_RESIDENT,
} todo_phase_t;

typedef struct {
    uint32_t id, start, end;
    uint8_t flags;
    char text[TODO_ITEMS_TEXT_MAX + 1U];
} todo_item_t;

typedef struct {
    uint8_t count;
    uint8_t active_count; /* All visible-window items, including permanent. */
    uint8_t queued_count; /* Visible-window items behind the display limit. */
    uint8_t daily_count; /* Uncompleted items overlapping today, including permanent. */
    uint8_t index[TODO_ITEMS_MAX_VISIBLE];
    uint8_t phase[TODO_ITEMS_MAX_VISIBLE];
    uint32_t id[TODO_ITEMS_MAX_VISIBLE];
} todo_selection_t;

/* Main context only; the caller mounts the resource store first. A missing
 * partition or package installs an empty list successfully. Invalid data or
 * I/O errors return false and hide the complete list until a successful init.
 * Only scheduling metadata is retained in RAM; item text is read on demand.
 */
bool todo_items_init(void);
void todo_items_reset(void);
bool todo_items_ready(void);
uint8_t todo_items_count(void);
uint8_t todo_items_font_size(void);
uint8_t todo_items_font_slot(void);
uint32_t todo_items_font_key(void);
uint8_t todo_items_font_style(void);
bool todo_items_extended_font(void);
uint16_t todo_items_lead_minutes(void);
uint16_t todo_items_retain_minutes(void);
/* A permanent font dependency error waits for an explicit resource/mode reload. */
void todo_items_suspend_display(void);
uint8_t todo_items_display_limit(void);
bool todo_items_read(uint8_t index, todo_item_t *item);

/* Pure RAM APIs, safe for the display scheduling timer. Permanent items come
 * first, then records sort by start, end, id. Upcoming items enter at
 * max(0, start-lead); active items change to expired at end and disappear at
 * end+retention. Selection handles both forward and backward clock changes.
 * Nothing reads Flash or commits a display snapshot.
 */
todo_phase_t todo_items_phase(uint8_t index, uint32_t now);
/* Count all unfinished items whose expanded visibility window overlaps the local day at now,
 * plus permanent items. Includes later triggers and earlier deadlines today;
 * the visible-item limit does not affect this total.
 */
uint8_t todo_items_daily_count(uint32_t now);
bool todo_items_select(uint32_t now, todo_selection_t *selection);
bool todo_items_selection_changed(uint32_t now);
void todo_items_invalidate(void);
/* Main context, after the screen refresh succeeds. Pass the same timestamp
 * used to render: a failed refresh must leave the previous snapshot untouched.
 */
void todo_items_successful_refresh(uint32_t now);

#endif
