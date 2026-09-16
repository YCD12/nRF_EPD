#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fds.h"
#include "EPD_config.h"
#include "calendar_annotations.h"
#include "calendar_annotations_store.h"
#include "storage_retry.h"

static void (*handlers[4])(const fds_evt_t *);
static unsigned handler_count, depth, waits, writes, deletes, collections;
static uint32_t now=1, next_error, gc_error;
static bool installed, exists[2], gc_active;
static fds_record_t inflight;
static bool writing, deleting;
static uint8_t persisted[2][1040];
static fds_header_t headers[2];
void test_critical_enter(void) { depth++; }
void test_critical_exit(void) { assert(depth); depth--; }
void app_feed_wdt(void) {}
uint32_t app_uptime_seconds(void) { return now; }
static void emit(fds_evt_t evt) { for(unsigned i=0;i<handler_count;i++) handlers[i](&evt); }
uint32_t fds_register(void (*fn)(const fds_evt_t *)) { handlers[handler_count++]=fn; return 0; }
uint32_t fds_init(void) { return 0; }
uint32_t sd_app_evt_wait(void) {
    waits++; installed=true; emit((fds_evt_t){.id=FDS_EVT_INIT}); return 0;
}
uint32_t fds_record_find(uint16_t file,uint16_t key,fds_record_desc_t *desc,fds_find_token_t *token) {
    (void)key;(void)token; assert(installed && file<2);
    desc->record_id=file; return exists[file]?0:FDS_ERR_NOT_FOUND;
}
uint32_t fds_record_open(fds_record_desc_t *desc,fds_flash_record_t *record) {
    record->p_header=&headers[desc->record_id]; record->p_data=persisted[desc->record_id]; return 0;
}
uint32_t fds_record_close(fds_record_desc_t *desc) { (void)desc; return 0; }
uint32_t fds_record_write(fds_record_desc_t *desc,const fds_record_t *record) {
    (void)desc; assert(!depth); writes++;
    if(next_error) { uint32_t err=next_error;next_error=0;return err; }
    assert(!writing && !deleting && !gc_active);
    assert((uintptr_t)record->data.p_data % 4 == 0);
    inflight=*record; writing=true; return 0;
}
uint32_t fds_record_update(fds_record_desc_t *desc,const fds_record_t *record) { return fds_record_write(desc,record); }
uint32_t fds_record_delete(fds_record_desc_t *desc) {
    assert(!depth && !writing && !deleting); deleting=true; deletes++;
    inflight.file_id=desc->record_id; return 0;
}
uint32_t fds_gc(void) {
    assert(!depth); collections++;
    if(gc_error) { uint32_t err=gc_error;gc_error=0;return err; }
    gc_active=true; return 0;
}
static void complete(uint32_t result) {
    unsigned file=inflight.file_id;
    if(writing) {
        if(!result) {
            memcpy(persisted[file],inflight.data.p_data,inflight.data.length_words*4);
            headers[file].length_words=inflight.data.length_words;exists[file]=true;
        }
        writing=false;
        emit((fds_evt_t){.id=FDS_EVT_WRITE,.result=result,.write={.file_id=file,.record_key=1}});
    } else {
        assert(deleting);deleting=false;if(!result) exists[file]=false;
        emit((fds_evt_t){.id=FDS_EVT_DEL_RECORD,.result=result,.del={.file_id=file,.record_key=1}});
    }
}
static void process_config(void) { epd_storage_process(); epd_config_process(); }
int main(void) {
    epd_config_t cfg={0}; cfg.week_start=1;
    assert(epd_config_init(&cfg)==0 && waits==1);
    epd_config_write(&cfg);process_config();assert(writing);
    assert(inflight.data.length_words==4);
    const uint8_t *snap=inflight.data.p_data;
    assert(snap[14]==0 && snap[15]==0);
    cfg.week_start=5;epd_config_write(&cfg);
    assert(((const epd_config_t *)snap)->week_start==1);
    complete(0);process_config();assert(writing);
    assert(((const epd_config_t *)inflight.data.p_data)->week_start==5);
    complete(0);process_config();assert(!writing);
    next_error=FDS_ERR_NO_SPACE_IN_FLASH;cfg.week_start=6;
    epd_config_write(&cfg);process_config();process_config();
    gc_error=FDS_ERR_NO_SPACE_IN_QUEUES;process_config();assert(!gc_active);
    now++;process_config();assert(gc_active);
    gc_active=false;emit((fds_evt_t){.id=FDS_EVT_GC});process_config();assert(writing);
    complete(0);process_config();assert(persisted[0][12]==6);

    calendar_annotations_store_init();calendar_annotations_store_save();
    const uint8_t *blob;uint16_t len=calendar_annotations_blob(&blob);
    uint8_t copy[1040];memcpy(copy,blob,len);
    assert(calendar_annotations_rx_write(0,len,copy,len)==CALENDAR_ANNO_RX_ERR);
    calendar_annotations_store_process();assert(writing && inflight.file_id==1);
    assert(!calendar_annotations_store_clear());
    emit((fds_evt_t){.id=FDS_EVT_GC});
    emit((fds_evt_t){.id=FDS_EVT_DEL_RECORD,.del={.file_id=0,.record_key=1}});
    calendar_annotations_store_process();
    assert(calendar_annotations_rx_write(0,len,copy,len)==CALENDAR_ANNO_RX_ERR);
    complete(0);calendar_annotations_store_process();assert(writing);
    complete(0);calendar_annotations_store_process();assert(!calendar_annotations_store_busy());

    cfg.week_start=3;epd_config_write(&cfg);process_config();assert(writing);
    epd_config_clear(&cfg);assert(!epd_config_clear_complete());
    complete(0);process_config();assert(deleting && !epd_config_clear_complete());
    complete(FDS_ERR_BUSY);process_config();assert(!epd_config_clear_complete());
    now++;process_config();assert(deleting);
    complete(0);process_config();assert(epd_config_clear_complete() && !exists[0]);
    assert(writes>=6 && deletes==2 && collections==2);
    puts("PASS: async FDS init, immutable snapshots, shared GC retry, annotation lock and deferred delete");
}
