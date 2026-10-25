#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "EPD_config.h"
#include "calendar_annotations.h"
#include "calendar_annotations_store.h"
#include "persistent_record.h"
#include "resource_layout.h"
#include "fm25q04b.h"

#define CFG_MAGIC 0x31435045UL
#define ANNO_MAGIC 0x31434E41UL

static unsigned depth, saves[4], applied, loads;
static bool in_irq;
static bool clear_during_load;
static rs_result_t load_forced = RS_OK;
static uint32_t now = 1U;
static rs_result_t forced = RS_OK;
static struct {
    bool valid, tombstone;
    uint32_t magic, revision, crc;
    uint16_t length;
    uint8_t data[1040];
} records[4];

void test_critical_enter(void) { ++depth; }
void test_critical_exit(void) { assert(depth); --depth; }
void app_feed_wdt(void) {}
uint32_t app_uptime_seconds(void) { return now; }
ret_code_t fm25q04b_init(void) { return NRF_SUCCESS; }

uint32_t persistent_record_data_crc(const void *data, uint16_t length)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFU;
    for (uint16_t n = 0; n < length; ++n) {
        crc ^= p[n];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
    }
    return crc ^ 0xFFFFFFFFU;
}

rs_result_t persistent_record_load(uint16_t id, uint32_t magic, void *data,
                                   uint16_t capacity, persistent_record_info_t *info)
{
    assert(id < 4 && info != NULL);
    assert(!in_irq);
    ++loads;
    if (clear_during_load) {
        clear_during_load = false;
        in_irq = true;
        assert(!calendar_annotations_store_clear());
        assert(!calendar_annotations_store_maintenance_ready());
        in_irq = false;
    }
    if (load_forced != RS_OK) {
        if (load_forced == RS_IO && capacity) memset(data, 0xA5, capacity);
        return load_forced;
    }
    if (!records[id].valid || records[id].tombstone) return RS_NOT_FOUND;
    if (records[id].magic != magic || records[id].length > capacity) return RS_INCOMPATIBLE;
    memcpy(data, records[id].data, records[id].length);
    *info = (persistent_record_info_t){records[id].revision, records[id].crc,
                                       records[id].length};
    return RS_OK;
}

rs_result_t persistent_record_save(uint16_t id, uint32_t magic, const void *data,
                                   uint16_t length, bool tombstone,
                                   persistent_record_info_t *info)
{
    assert(!depth && id < 4 && info != NULL);
    ++saves[id];
    if (forced != RS_OK) {
        /* Fail after invalidation/erase, as a real single-copy save can. */
        if (forced != RS_BUSY) records[id].valid = false;
        return forced;
    }
    records[id].valid = true;
    records[id].tombstone = tombstone;
    records[id].magic = magic;
    records[id].revision++;
    records[id].length = length;
    records[id].crc = persistent_record_data_crc(data, length);
    if (length) memcpy(records[id].data, data, length);
    *info = (persistent_record_info_t){records[id].revision, records[id].crc, length};
    return RS_OK;
}

static void on_applied(void) { ++applied; }

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8U * i));
}

static uint16_t make_annotation(uint8_t *blob)
{
    memset(blob, 0, 32);
    put_u32(blob, CALENDAR_ANNOTATIONS_MAGIC);
    blob[4] = CALENDAR_ANNOTATIONS_VERSION;
    blob[5] = CALENDAR_ANNOTATIONS_RECORD_SIZE;
    put_u16(blob + 6, 1);
    put_u16(blob + 16, 2026);
    blob[18] = 9;
    blob[19] = 16;
    blob[20] = 1;
    blob[21] = 3;
    memcpy(blob + 22, "abc", 3);
    return 32;
}

static void test_config(void)
{
    epd_config_t cfg;
    uint8_t result;
    uint32_t revision, crc;
    assert(epd_config_init(&cfg) == 0);
    epd_config_read(&cfg);
    assert(epd_config_empty(&cfg));
    memset(&cfg, 0, sizeof(cfg));
    cfg.week_start = 1;
    epd_config_write(&cfg);
    epd_config_process();
    assert(saves[RS_PART_EPD_CONFIG] == 1);
    assert(epd_config_take_save_result(&result, &revision, &crc) && result == RS_OK && revision == 1);
    epd_config_write(&cfg);
    epd_config_process();
    assert(saves[RS_PART_EPD_CONFIG] == 1); /* unchanged: ACK without erase */
    assert(epd_config_take_save_result(&result, &revision, &crc) && result == RS_OK);

    cfg.week_start = 2;
    epd_config_write(&cfg);
    cfg.week_start = 3;
    epd_config_write(&cfg);
    epd_config_process();
    assert(records[RS_PART_EPD_CONFIG].data[12] == 3);
    forced = RS_IO;
    cfg.week_start = 4;
    epd_config_write(&cfg);
    for (unsigned i = 0; i < 4; ++i) { ++now; epd_config_process(); }
    assert(epd_config_take_save_result(&result, &revision, &crc) && result == RS_IO);
    forced = RS_OK;
    cfg.week_start = 3;
    assert(!epd_config_is_saved(&cfg));
    epd_config_write(&cfg);
    assert(epd_config_busy());
    ++now; epd_config_process();
    assert(epd_config_is_saved(&cfg));
    assert(epd_config_take_save_result(&result, &revision, &crc) && result == RS_OK);
    epd_config_t loaded;
    persistent_record_info_t info;
    assert(persistent_record_load(RS_PART_EPD_CONFIG, CFG_MAGIC, &loaded, sizeof(loaded), &info) == RS_OK);
    assert(!memcmp(&loaded, &cfg, sizeof(cfg)) && crc == info.data_crc);
    epd_config_clear(&cfg);
    ++now; epd_config_process();
    assert(epd_config_clear_complete() && records[RS_PART_EPD_CONFIG].tombstone);
}

static void test_annotations(void)
{
    uint8_t blob[32], result;
    uint32_t revision, crc;
    calendar_annotations_store_init();
    calendar_annotations_store_on_applied(on_applied);
    assert(calendar_annotations_count() == 0);
    calendar_annotations_store_save();
    assert(!calendar_annotations_store_maintenance_ready());
    calendar_annotations_store_process();
    assert(saves[RS_PART_CALENDAR_ANNOTATIONS] == 1);
    assert(calendar_annotations_store_take_save_result(&result, &revision, &crc) && result == RS_OK);

    uint16_t length = make_annotation(blob);
    assert(calendar_annotations_rx_write(0, length, blob, length) == CALENDAR_ANNO_RX_DONE);
    calendar_annotations_store_save();
    assert(!calendar_annotations_store_clear());
    calendar_annotations_store_process();
    assert(calendar_annotations_store_busy());
    calendar_annotations_store_process();
    assert(!calendar_annotations_store_busy() && calendar_annotations_count() == 0);
    assert(applied == 1 && records[RS_PART_CALENDAR_ANNOTATIONS].length == 16);

    assert(calendar_annotations_rx_write(0, length, blob, length) == CALENDAR_ANNO_RX_DONE);
    calendar_annotations_store_save();
    forced = RS_IO;
    for (unsigned i = 0; i < 4; ++i) { ++now; calendar_annotations_store_process(); }
    assert(calendar_annotations_store_take_save_result(&result, &revision, &crc) && result == RS_IO);
    assert(!records[RS_PART_CALENDAR_ANNOTATIONS].valid);
    forced = RS_OK;
    assert(calendar_annotations_store_clear());
    ++now; calendar_annotations_store_process();
    assert(calendar_annotations_store_take_save_result(&result, &revision, &crc) && result == RS_OK);
    uint8_t loaded[32];
    persistent_record_info_t info;
    assert(persistent_record_load(RS_PART_CALENDAR_ANNOTATIONS, ANNO_MAGIC, loaded, sizeof(loaded), &info) == RS_OK);
    assert(info.length == 16 && loaded[6] == 0);

    /* Invalid RX must not reload on the BLE IRQ stack, or expose partial data. */
    unsigned before = loads;
    uint8_t invalid[16] = {0};
    in_irq = true;
    assert(calendar_annotations_rx_write(0, sizeof(invalid), invalid, sizeof(invalid)) == CALENDAR_ANNO_RX_RECOVERED);
    in_irq = false;
    assert(loads == before && calendar_annotations_store_busy());
    assert(calendar_annotations_blob(NULL) == 0);
    load_forced = RS_BUSY;
    calendar_annotations_store_process();
    assert(calendar_annotations_store_busy() && calendar_annotations_blob(NULL) == 0);
    load_forced = RS_OK;
    ++now;
    calendar_annotations_store_process();
    assert(!calendar_annotations_store_busy());
    assert(calendar_annotations_blob(NULL) == 16 && loads == before + 2);
}

static void test_config_read_recovery(void)
{
    epd_config_t saved = {0}, loaded, requested;
    persistent_record_info_t info;
    saved.week_start = 6;
    assert(persistent_record_save(2, CFG_MAGIC, &saved, sizeof(saved), false, &info) == RS_OK);
    epd_config_init(&loaded);
    unsigned writes = saves[2];
    load_forced = RS_IO;
    epd_config_read(&loaded);
    assert(epd_config_empty(&loaded) && epd_config_read_pending());
    assert(!epd_config_is_saved(&loaded) && !epd_config_busy());
    unsigned reads = loads;
    for (unsigned i = 0; i < 30; ++i) {
        assert(!epd_config_retry_read(&loaded));
        epd_config_process();
    }
    assert(loads == reads && saves[2] == writes);
    for (unsigned i = 0; i < 3; ++i) {
        ++now;
        assert(!epd_config_retry_read(&loaded) && epd_config_read_pending());
        epd_config_process();
    }
    assert(loads == reads + 3U && saves[2] == writes && records[2].valid);
    assert(!memcmp(records[2].data, &saved, sizeof(saved)));
    load_forced = RS_OK;
    ++now;
    assert(epd_config_retry_read(&loaded) && !epd_config_read_pending());
    assert(!memcmp(&loaded, &saved, sizeof(saved)) && epd_config_is_saved(&loaded));

    /* Explicit settings and reset supersede a delayed boot read. */
    load_forced = RS_IO;
    epd_config_read(&loaded);
    requested = saved; requested.week_start = 1;
    epd_config_write(&requested);
    assert(!epd_config_read_pending());
    load_forced = RS_OK;
    ++now;
    epd_config_process();
    assert(!epd_config_retry_read(&loaded) && epd_config_is_saved(&requested));
    assert(!memcmp(records[2].data, &requested, sizeof(requested)));
    load_forced = RS_IO;
    epd_config_read(&loaded);
    epd_config_clear(&loaded);
    assert(!epd_config_read_pending());
    load_forced = RS_OK;
    ++now;
    epd_config_process();
    assert(records[2].tombstone && epd_config_clear_complete());
    assert(!epd_config_retry_read(&loaded));

    /* A confirmed missing record finishes recovery so service can initialize
     * defaults; deadlines also work over uptime wrap. */
    epd_config_init(&loaded);
    now = UINT32_MAX;
    load_forced = RS_BUSY;
    epd_config_read(&loaded);
    assert(epd_config_read_pending() && !epd_config_retry_read(&loaded));
    load_forced = RS_OK;
    now = 0;
    assert(epd_config_retry_read(&loaded) && epd_config_empty(&loaded));
    puts("PASS: config transient reads retry without writes; explicit settings/reset supersede recovery");
}

static void test_annotation_read_recovery(void)
{
    uint8_t blob[32];
    persistent_record_info_t info;
    make_annotation(blob);
    assert(persistent_record_save(3, ANNO_MAGIC, blob, sizeof(blob), false, &info) == RS_OK);
    unsigned writes = saves[3];
    load_forced = RS_IO;
    calendar_annotations_store_init();
    assert(calendar_annotations_reload_pending() && calendar_annotations_store_busy());
    assert(calendar_annotations_store_maintenance_ready());
    assert(!calendar_annotations_store_valid() && calendar_annotations_blob(NULL) == 0);
    unsigned reads = loads;
    for (unsigned i = 0; i < 30; ++i) calendar_annotations_store_process();
    assert(loads == reads && saves[3] == writes);
    ++now;
    calendar_annotations_store_process();
    assert(loads == reads + 1U && calendar_annotations_reload_pending());
    assert(calendar_annotations_store_maintenance_ready());
    load_forced = RS_OK;
    ++now;
    calendar_annotations_store_process();
    assert(!calendar_annotations_store_busy() && calendar_annotations_store_valid());
    assert(calendar_annotations_count() == 1U && saves[3] == writes);

    /* An aborted upload hides its partial live buffer until the original
     * persisted annotation has been read successfully, including transient IO. */
    assert(calendar_annotations_rx_write(0, sizeof(blob), blob, 16) == CALENDAR_ANNO_RX_MORE);
    assert(!calendar_annotations_store_maintenance_ready());
    assert(calendar_annotations_rx_cancel());
    assert(calendar_annotations_store_maintenance_ready());
    load_forced = RS_IO;
    calendar_annotations_store_process();
    assert(calendar_annotations_reload_pending() && calendar_annotations_blob(NULL) == 0);
    assert(records[3].valid && records[3].length == sizeof(blob));
    load_forced = RS_OK;
    ++now;
    calendar_annotations_store_process();
    assert(calendar_annotations_count() == 1U && calendar_annotations_store_valid());
    assert(saves[3] == writes);

    /* Explicit clear supersedes a deferred read. It must not wait forever for
     * damaged storage to become readable before attempting the new request. */
    load_forced = RS_BUSY;
    calendar_annotations_store_init();
    reads = loads;
    assert(calendar_annotations_store_clear());
    assert(!calendar_annotations_reload_pending());
    assert(!calendar_annotations_store_maintenance_ready());
    ++now;
    calendar_annotations_store_process();
    assert(!calendar_annotations_store_busy() && calendar_annotations_count() == 0);
    assert(calendar_annotations_store_maintenance_ready());
    assert(loads == reads);
    assert(records[3].length == 16U && saves[3] == writes + 1U);

    /* Persistent read IO plus an explicit clear cannot starve resource repair.
     * If writing also fails, report failure after the existing finite retries. */
    load_forced = RS_IO;
    calendar_annotations_store_init();
    assert(calendar_annotations_reload_pending());
    reads = loads; writes = saves[3];
    assert(calendar_annotations_store_clear());
    assert(!calendar_annotations_reload_pending() && calendar_annotations_blob(NULL) == 16U);
    forced = RS_IO;
    uint8_t result;
    uint32_t revision, crc;
    for (unsigned i = 0; i < 4U; ++i) {
        ++now;
        calendar_annotations_store_process();
        assert(loads == reads && saves[3] == writes + i + 1U);
        if (i < 3U) {
            assert(!calendar_annotations_store_maintenance_ready());
            assert(!calendar_annotations_store_take_save_result(&result, &revision, &crc));
        }
    }
    assert(calendar_annotations_store_take_save_result(&result, &revision, &crc) && result == RS_IO);
    assert(!calendar_annotations_store_busy() && calendar_annotations_store_maintenance_ready());
    ++now;
    calendar_annotations_store_process();
    assert(loads == reads && saves[3] == writes + 4U);
    forced = RS_OK;

    /* A permanently invalid record settles on empty without a retry loop. */
    load_forced = RS_CRC;
    calendar_annotations_store_init();
    assert(!calendar_annotations_reload_pending() && !calendar_annotations_store_valid());
    reads = loads;
    ++now;
    calendar_annotations_store_process();
    assert(loads == reads);
    load_forced = RS_OK;
    puts("PASS: boot/aborted annotation read IO stays hidden and retries; explicit clear and invalid records settle");
}

static void test_annotation_clear_during_reload(void)
{
    uint8_t blob[32], result;
    uint32_t revision, crc;
    persistent_record_info_t info;
    make_annotation(blob);
    for (unsigned fail = 0; fail < 2U; ++fail) {
        assert(persistent_record_save(3, ANNO_MAGIC, blob, sizeof(blob), false, &info) == RS_OK);
        load_forced = fail ? RS_IO : RS_OK;
        clear_during_load = true;
        calendar_annotations_store_init();
        assert(!clear_during_load && calendar_annotations_store_busy());
        assert(!calendar_annotations_store_maintenance_ready());
        assert(calendar_annotations_reload_pending() == (fail != 0U));
        assert(calendar_annotations_count() == (fail ? 0U : 1U));
        unsigned reads = loads, writes = saves[3];
        forced = fail ? RS_IO : RS_OK;
        unsigned attempts = fail ? 4U : 1U;
        for (unsigned i = 0; i < attempts; ++i) {
            ++now;
            calendar_annotations_store_process();
            assert(loads == reads && saves[3] == writes + i + 1U);
        }
        assert(calendar_annotations_count() == 0U && !calendar_annotations_reload_pending());
        assert(!calendar_annotations_store_busy() && calendar_annotations_store_maintenance_ready());
        assert(records[3].valid == (fail == 0U));
        if (!fail) assert(records[3].length == 16U && records[3].data[6] == 0U);
        assert(calendar_annotations_store_take_save_result(&result, &revision, &crc) &&
               result == (fail ? RS_IO : RS_OK));
        ++now;
        calendar_annotations_store_process();
        assert(loads == reads && saves[3] == writes + attempts);
        forced = RS_OK;
    }
    load_forced = RS_OK;
    puts("PASS: IRQ clear during successful/failed read is deferred safely and supersedes retry");
}

int main(void)
{
    assert(CFG_MAGIC == 0x31435045UL && ANNO_MAGIC == 0x31434E41UL);
    test_config();
    test_annotations();
    test_config_read_recovery();
    test_annotation_read_recovery();
    test_annotation_clear_during_reload();
    puts("PASS: external config/annotation records, dedup, retry, tombstone and deferred clear");
    return 0;
}
