#include "calendar_resources.h"
#include "Lunar.h"
#include "resource_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t payload[1600];
static uint32_t payload_length;
static bool mount_ok = true;
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;i++) p[i]=v>>(8*i); }

rs_result_t resource_store_mount(void) { return mount_ok ? RS_OK : RS_NOT_FOUND; }
rs_result_t resource_store_open(uint16_t id, uint32_t *length)
{ if (id != 5 || !length) return RS_NOT_FOUND; *length = payload_length; return RS_OK; }
rs_result_t resource_store_read(uint16_t id, uint32_t offset, void *data, size_t length)
{
    if (id != 5 || !data || offset > payload_length || length > payload_length - offset)
        return RS_INVALID;
    memcpy(data, payload + offset, length);
    return RS_OK;
}
uint32_t resource_get_u32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }

static void make_package(void)
{
    static const uint16_t sizes[16] = {7,4,7,4,4,4,4,7,4,4,1,1,12,12,2,10};
    static const uint16_t counts[16] = {13,2,31,7,12,10,12,24,5,5,12,24,15,11,39,5};
    memset(payload, 0, sizeof(payload));
    put32(payload, 0x314C4143); put16(payload+4,1); put16(payload+6,16);
    put16(payload+8,2025); put16(payload+10,2027); put16(payload+12,2026);
    uint32_t offset = 16 + 16 * 16;
    for (unsigned i=0;i<16;i++) {
        uint8_t *e = payload + 16 + i * 16;
        put16(e,i+1); put16(e+2,sizes[i]); put16(e+4,counts[i]);
        put32(e+8,offset); put32(e+12,(uint32_t)sizes[i]*counts[i]);
        offset += (uint32_t)sizes[i]*counts[i];
    }
    payload_length = offset;
    uint32_t month_offset = resource_get_u32(payload + 16 + 8);
    memcpy(payload + month_offset, "fallback", 7);
    memcpy(payload + month_offset + 3 * 7, "三月", 7);
    uint32_t u32_offset = resource_get_u32(payload + 16 + 8 * 16 + 8);
    const uint32_t month_days[5] = {2024,0x96C,0xD4AE,0x149C,0x1A4C};
    for (unsigned i=0;i<5;i++) put32(payload + u32_offset + i*4, month_days[i]);
    uint32_t new_year_offset = resource_get_u32(payload + 16 + 9 * 16 + 8);
    const uint32_t new_year[5] = {2024,0xFD04A,0xFD23D,0xFD451,0xFD646};
    for (unsigned i=0;i<5;i++) put32(payload + new_year_offset + i*4, new_year[i]);
    uint32_t bits_offset = resource_get_u32(payload + 16 + 10 * 16 + 8);
    const uint8_t bits[12] = {0x0F,0xEF,0xDB,0xBE,0xA6,0x99,0x9C,0xA2,0x98,0x80,0,0x18};
    memcpy(payload + bits_offset, bits, sizeof(bits));
    uint32_t days_offset = resource_get_u32(payload + 16 + 11 * 16 + 8);
    const uint8_t days[24] = {6,20,4,19,6,21,5,20,6,21,6,21,7,23,8,23,8,23,8,24,8,22,7,22};
    memcpy(payload + days_offset, days, sizeof(days));
    uint32_t fest_offset = resource_get_u32(payload + 16 + 12 * 16 + 8);
    payload[fest_offset] = 10; payload[fest_offset+1] = 1;
    memcpy(payload + fest_offset + 2, "国庆节", 10);
    uint32_t holiday_offset = resource_get_u32(payload + 16 + 14 * 16 + 8);
    put16(payload + holiday_offset, 0x1A0A);
    uint32_t special_offset = resource_get_u32(payload + 16 + 15 * 16 + 8);
    memcpy(payload + special_offset + CAL_SPECIAL_QINGMING * 10, "清明节", 10);
}

int main(void)
{
    make_package();
    calendar_resources_host_reset();
    assert(calendar_resources_init());
    assert(calendar_resources_year_min() == 2025);
    assert(calendar_resources_year_max() == 2027);
    assert(calendar_resources_holiday_year() == 2026);
    char text[10];
    assert(calendar_resource_text(CAL_RES_MONTH_NAMES, 3, text, sizeof(text)));
    assert(!strcmp(text, "三月"));
    assert(!calendar_resource_text(CAL_RES_MONTH_NAMES, 13, text, sizeof(text)));
    uint32_t value;
    assert(calendar_resource_u32(CAL_RES_LUNAR_MONTH_DAYS, 2, &value));
    assert(value == 0xD4AE);
    assert(calendar_resource_festival(CAL_RES_SOLAR_FESTIVALS, 10, 1, text, sizeof(text)));
    assert(!strcmp(text, "国庆节"));
    bool work = false;
    assert(calendar_resource_holiday(2026, 10, 10, &work) && work);
    assert(!calendar_resource_holiday(2025, 10, 10, &work));
    assert(calendar_resource_text(CAL_RES_SPECIAL_FESTIVAL_NAMES,
                                  CAL_SPECIAL_QINGMING, text, sizeof(text)));
    assert(!strcmp(text, "清明节"));
    struct Lunar_Date lunar;
    LUNAR_SolarToLunar(&lunar, 2026, 2, 17);
    assert(lunar.Year == 2026 && lunar.Month == 1 && lunar.Date == 1);
    uint8_t jq_day;
    assert(GetJieQi(2026, 2, 1, &jq_day) && jq_day == 4);
    /* Annual holiday data and variable record counts must update without firmware. */
    put16(payload + 12, 2027);
    uint8_t *holiday_entry = payload + 16 + (CAL_RES_HOLIDAYS - 1U) * 16;
    put16(holiday_entry + 4, 38);
    put32(holiday_entry + 12, 76);
    calendar_resources_host_reset();
    assert(calendar_resources_init());
    assert(calendar_resources_holiday_year() == 2027);
    assert(!calendar_resource_holiday(2026, 10, 10, &work));
    calendar_resources_host_reset();
    payload[16 + 2] = 8; /* wrong item size */
    assert(!calendar_resources_init());
    calendar_resources_host_reset();
    mount_ok = false;
    assert(!calendar_resources_init());
    puts("calendar resources: schema, ranges, text, festival and holiday lookup OK");
    return 0;
}
