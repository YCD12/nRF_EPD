#include "calendar_annotations.h"

#include <string.h>

#if CALENDAR_ANNOTATIONS_USE_NRF_CRITICAL
#include "app_util_platform.h"
#define ANNO_CRITICAL_ENTER() CRITICAL_REGION_ENTER()
#define ANNO_CRITICAL_EXIT()  CRITICAL_REGION_EXIT()
#else
#define ANNO_CRITICAL_ENTER() do { } while (0)
#define ANNO_CRITICAL_EXIT()  do { } while (0)
#endif

#define FLAG_PRESENT 0x01U
#define FONT_MODE_MASK 0x06U
#define FONT_MODE_SHIFT 1U
#define COLOR_MODE_MASK 0x18U
#define COLOR_MODE_SHIFT 3U
#define FLAG_POSITION_MASK CALENDAR_ANNOTATION_POS_CORNER_FLAG
#define FLAG_RESERVED_MASK 0xC0U

static uint32_t m_live_words[CALENDAR_ANNOTATIONS_MAX_BYTES / 4U];
static uint16_t m_live_len;
static uint16_t m_rx_off;
static uint16_t m_rx_total;
static volatile bool m_rx_active;
static volatile bool m_reload_pending;
static bool m_live_locked;
static calendar_annotations_reload_fn m_reload;

static uint8_t* live_bytes(void)
{
    return (uint8_t*)m_live_words;
}

static uint16_t read_u16_le(const uint8_t* value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint32_t read_u32_le(const uint8_t* value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static uint32_t record_key(const uint8_t* record)
{
    return ((uint32_t)read_u16_le(record) << 16) | ((uint32_t)record[2] << 8) | record[3];
}

static void abort_rx(void)
{
    m_rx_active = false;
    m_rx_off = 0;
    m_rx_total = 0;
}

static void decode_record(const uint8_t* record, calendar_annotation_t* annotation)
{
    uint8_t label_len = record[5];
    uint8_t font_mode = (uint8_t)((record[4] & FONT_MODE_MASK) >> FONT_MODE_SHIFT);
    uint8_t color_mode = (uint8_t)((record[4] & COLOR_MODE_MASK) >> COLOR_MODE_SHIFT);

    annotation->present = true;
    if (font_mode == 1U) {
        annotation->font_size = CALENDAR_ANNOTATION_FONT_SMALL;
    } else if (font_mode == 2U) {
        annotation->font_size = CALENDAR_ANNOTATION_FONT_LARGE;
    } else {
        annotation->font_size = CALENDAR_ANNOTATION_FONT_FOLLOW;
    }
    if (color_mode == 1U) {
        annotation->color = CALENDAR_ANNOTATION_COLOR_WHITE;
    } else if (color_mode == 2U) {
        annotation->color = CALENDAR_ANNOTATION_COLOR_RED;
    } else {
        annotation->color = CALENDAR_ANNOTATION_COLOR_BLACK;
    }
    annotation->position = ((record[4] & FLAG_POSITION_MASK) != 0U)
                               ? CALENDAR_ANNOTATION_POS_CORNER
                               : CALENDAR_ANNOTATION_POS_BELOW;
    memcpy(annotation->label, record + 6, label_len);
    annotation->label[label_len] = '\0';
}

static void install_empty(void)
{
    uint8_t* live = live_bytes();

    memset(m_live_words, 0, sizeof(m_live_words));
    live[0] = (uint8_t)CALENDAR_ANNOTATIONS_MAGIC;
    live[1] = (uint8_t)(CALENDAR_ANNOTATIONS_MAGIC >> 8);
    live[2] = (uint8_t)(CALENDAR_ANNOTATIONS_MAGIC >> 16);
    live[3] = (uint8_t)(CALENDAR_ANNOTATIONS_MAGIC >> 24);
    live[4] = CALENDAR_ANNOTATIONS_VERSION;
    live[5] = CALENDAR_ANNOTATIONS_RECORD_SIZE;
    m_live_len = CALENDAR_ANNOTATIONS_HEADER_SIZE;
}

static void restore_live(void)
{
    abort_rx();
    /* May run in a BLE IRQ: never invoke the external-store callback here. */
    m_reload_pending = true;
}

bool calendar_annotations_reload_pending(void) { return m_reload_pending; }

bool calendar_annotations_process_reload(void)
{
    if (!m_reload_pending || m_live_locked) return false;
    if (m_reload != NULL) {
        if (!m_reload()) return false;
    } else {
        install_empty();
    }
    m_reload_pending = false;
    return true;
}

static calendar_anno_rx_t fail_rx(bool started)
{
    if (!started) {
        return CALENDAR_ANNO_RX_ERR;
    }
    restore_live();
    return CALENDAR_ANNO_RX_RECOVERED;
}

static bool blob_valid(const uint8_t* data, size_t length)
{
    uint16_t count;
    size_t expected;
    uint32_t prev_key = 0;
    bool have_prev = false;
    uint16_t i;

    if (data == NULL || length < CALENDAR_ANNOTATIONS_HEADER_SIZE) {
        return false;
    }
    count = read_u16_le(data + 6);
    expected = CALENDAR_ANNOTATIONS_HEADER_SIZE +
               (size_t)count * CALENDAR_ANNOTATIONS_RECORD_SIZE;
    if (read_u32_le(data) != CALENDAR_ANNOTATIONS_MAGIC ||
        data[4] != CALENDAR_ANNOTATIONS_VERSION ||
        data[5] != CALENDAR_ANNOTATIONS_RECORD_SIZE ||
        count > CALENDAR_ANNOTATIONS_MAX_RECORDS ||
        expected > CALENDAR_ANNOTATIONS_MAX_BYTES || expected > length) {
        return false;
    }
    for (i = 8; i < CALENDAR_ANNOTATIONS_HEADER_SIZE; i++) {
        if (data[i] != 0U) {
            return false;
        }
    }
    for (i = 0; i < count; i++) {
        const uint8_t* record =
            data + CALENDAR_ANNOTATIONS_HEADER_SIZE + (size_t)i * CALENDAR_ANNOTATIONS_RECORD_SIZE;
        uint8_t flags = record[4];
        uint8_t label_len = record[5];
        uint8_t font_mode = (uint8_t)((flags & FONT_MODE_MASK) >> FONT_MODE_SHIFT);
        uint8_t color_mode = (uint8_t)((flags & COLOR_MODE_MASK) >> COLOR_MODE_SHIFT);
        uint32_t key = record_key(record);

        if ((flags & FLAG_PRESENT) == 0U || (flags & FLAG_RESERVED_MASK) != 0U ||
            font_mode > 2U || color_mode > 2U ||
            record[2] < 1U || record[2] > 12U || record[3] < 1U || record[3] > 31U ||
            label_len > CALENDAR_ANNOTATIONS_LABEL_MAX || record[15] != 0U) {
            return false;
        }
        if (have_prev && key <= prev_key) {
            return false;
        }
        prev_key = key;
        have_prev = true;
    }
    return true;
}

static bool copy_live(const uint8_t* data, size_t length)
{
    uint16_t count;
    size_t expected;

    if (!blob_valid(data, length)) {
        return false;
    }
    count = read_u16_le(data + 6);
    expected = CALENDAR_ANNOTATIONS_HEADER_SIZE +
               (size_t)count * CALENDAR_ANNOTATIONS_RECORD_SIZE;
    memset(m_live_words, 0, sizeof(m_live_words));
    memcpy(live_bytes(), data, expected);
    m_live_len = (uint16_t)expected;
    return true;
}

void calendar_annotations_reset(void)
{
    abort_rx();
    install_empty();
    m_reload_pending = false;
}

void calendar_annotations_set_reload(calendar_annotations_reload_fn reload)
{
    m_reload = reload;
}

void calendar_annotations_set_live_locked(bool locked)
{
    m_live_locked = locked;
}

void calendar_annotations_clear(void)
{
    if (m_live_locked) {
        return;
    }
    calendar_annotations_reset();
}

bool calendar_annotations_load(const uint8_t* data, size_t length)
{
    if (m_live_locked) {
        return false;
    }
    abort_rx();
    if (!copy_live(data, length)) {
        install_empty();
        return false;
    }
    return true;
}

uint16_t calendar_annotations_count(void)
{
    uint16_t count = 0U;
    ANNO_CRITICAL_ENTER();
    if (!m_rx_active && !m_reload_pending && m_live_len >= CALENDAR_ANNOTATIONS_HEADER_SIZE) {
        count = read_u16_le(live_bytes() + 6);
    }
    ANNO_CRITICAL_EXIT();
    return count;
}

uint16_t calendar_annotations_blob(const uint8_t** data)
{
    if (m_rx_active || m_reload_pending) {
        if (data != NULL) {
            *data = NULL;
        }
        return 0;
    }
    if (data != NULL) {
        *data = live_bytes();
    }
    return m_live_len;
}

bool calendar_annotations_get(uint16_t year, uint8_t month, uint8_t day,
                              calendar_annotation_t* annotation)
{
    uint16_t low = 0;
    uint16_t high;
    uint32_t want;
    bool found = false;

    if (annotation != NULL) {
        memset(annotation, 0, sizeof(*annotation));
    }
    if (annotation == NULL || month < 1U || month > 12U || day < 1U || day > 31U) {
        return false;
    }
    ANNO_CRITICAL_ENTER();
    if (!m_rx_active && !m_reload_pending && m_live_len >= CALENDAR_ANNOTATIONS_HEADER_SIZE) {
        high = read_u16_le(live_bytes() + 6);
        want = ((uint32_t)year << 16) | ((uint32_t)month << 8) | day;
        while (low < high) {
            uint16_t middle = (uint16_t)(low + (high - low) / 2U);
            const uint8_t* record = live_bytes() + CALENDAR_ANNOTATIONS_HEADER_SIZE +
                                    (size_t)middle * CALENDAR_ANNOTATIONS_RECORD_SIZE;
            uint32_t key = record_key(record);

            if (key == want) {
                decode_record(record, annotation);
                found = true;
                break;
            }
            if (key < want) low = (uint16_t)(middle + 1U);
            else high = middle;
        }
    }
    ANNO_CRITICAL_EXIT();
    return found;
}

uint8_t* calendar_annotations_load_buffer(uint16_t* capacity)
{
    if (capacity != NULL) *capacity = CALENDAR_ANNOTATIONS_MAX_BYTES;
    if (m_live_locked) return NULL;
    abort_rx();
    m_live_len = 0U;
    return live_bytes();
}

bool calendar_annotations_load_commit(uint16_t length)
{
    size_t expected;
    if (m_live_locked || !blob_valid(live_bytes(), length)) {
        install_empty();
        return false;
    }
    expected = CALENDAR_ANNOTATIONS_HEADER_SIZE +
               (size_t)read_u16_le(live_bytes() + 6) * CALENDAR_ANNOTATIONS_RECORD_SIZE;
    m_live_len = (uint16_t)expected;
    return true;
}

bool calendar_annotations_rx_active(void)
{
    return m_rx_active || m_reload_pending;
}

bool calendar_annotations_rx_cancel(void)
{
    if (!m_rx_active) return false;
    restore_live();
    return true;
}

calendar_anno_rx_t calendar_annotations_rx_write(uint16_t offset, uint16_t total,
                                                 const uint8_t* data, uint16_t length)
{
    if (m_live_locked || m_reload_pending) return CALENDAR_ANNO_RX_ERR;
    if (data == NULL && length != 0U) {
        return CALENDAR_ANNO_RX_ERR;
    }
    if (total < CALENDAR_ANNOTATIONS_HEADER_SIZE || total > CALENDAR_ANNOTATIONS_MAX_BYTES ||
        ((total - CALENDAR_ANNOTATIONS_HEADER_SIZE) % CALENDAR_ANNOTATIONS_RECORD_SIZE) != 0U) {
        return CALENDAR_ANNO_RX_ERR;
    }
    if (offset == 0U) {
        if (m_live_locked) {
            return CALENDAR_ANNO_RX_ERR;
        }
        m_rx_active = true;
        m_rx_off = 0;
        m_rx_total = total;
        memset(m_live_words, 0, sizeof(m_live_words));
        m_live_len = 0;
    } else if (!m_rx_active || offset != m_rx_off || total != m_rx_total) {
        return fail_rx(m_rx_active);
    }
    if ((uint32_t)offset + length > total) {
        return fail_rx(true);
    }
    if (length != 0U) {
        memcpy(live_bytes() + offset, data, length);
    }
    m_rx_off = (uint16_t)(offset + length);
    if (m_rx_off < total) {
        return CALENDAR_ANNO_RX_MORE;
    }
    m_rx_active = false;
    if (!blob_valid(live_bytes(), total)) {
        return fail_rx(true);
    }
    m_live_len = total;
    return CALENDAR_ANNO_RX_DONE;
}

uint16_t calendar_annotations_notify_chunk(uint16_t offset, uint8_t* out, uint16_t out_size)
{
    const uint8_t* blob = NULL;
    uint16_t total = calendar_annotations_blob(&blob);
    uint16_t room;
    uint16_t remain;

    if (out == NULL || blob == NULL || out_size <= CALENDAR_ANNO_NOTIFY_HDR || offset >= total) {
        return 0;
    }
    remain = (uint16_t)(total - offset);
    room = (uint16_t)(out_size - CALENDAR_ANNO_NOTIFY_HDR);
    if (room > remain) {
        room = remain;
    }
    out[0] = CALENDAR_ANNO_NOTIFY;
    out[1] = (uint8_t)offset;
    out[2] = (uint8_t)(offset >> 8);
    out[3] = (uint8_t)total;
    out[4] = (uint8_t)(total >> 8);
    memcpy(out + CALENDAR_ANNO_NOTIFY_HDR, blob + offset, room);
    return (uint16_t)(CALENDAR_ANNO_NOTIFY_HDR + room);
}
