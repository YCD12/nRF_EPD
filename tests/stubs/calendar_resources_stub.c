#include "calendar_resources.h"
#include <string.h>

static const char month_names[13][7] = {"----","正月","二月","三月","四月","五月","六月","七月","八月","九月","十月","冬月","腊月"};
static const char leap_names[2][4] = {" ","闰"};
static const char date_names[31][7] = {"----","初一","初二","初三","初四","初五","初六","初七","初八","初九","初十","十一","十二","十三","十四","十五","十六","十七","十八","十九","二十","廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"};
static const char weekday_names[7][4] = {"日","一","二","三","四","五","六"};
static const char zodiac_names[12][4] = {"猴","鸡","狗","猪","鼠","牛","虎","兔","龙","蛇","马","羊"};
static const char stem_names[10][4] = {"庚","辛","壬","癸","甲","乙","丙","丁","戊","己"};
static const char branch_names[12][4] = {"申","酉","戌","亥","子","丑","寅","卯","辰","巳","午","未"};
static const char jieqi_names[24][7] = {"小寒","大寒","立春","雨水","惊蛰","春分","清明","谷雨","立夏","小满","芒种","夏至","小暑","大暑","立秋","处暑","白露","秋分","寒露","霜降","立冬","小雪","大雪","冬至"};
static const uint32_t month_days[5] = {2024,0x96C,0xD4AE,0x149C,0x1A4C};
static const uint32_t new_year[5] = {2024,0xFD04A,0xFD23D,0xFD451,0xFD646};
static const uint8_t jieqi_bits[12] = {0x0F,0xEF,0xDB,0xBE,0xA6,0x99,0x9C,0xA2,0x98,0x80,0,0x18};
static const uint8_t jieqi_days[24] = {6,20,4,19,6,21,5,20,6,21,6,21,7,23,8,23,8,23,8,24,8,22,7,22};
typedef struct { uint8_t month, day; char name[10]; } festival_t;
static const festival_t solar[] = {{1,1,"元旦节"},{2,14,"情人节"},{3,8,"妇女节"},{3,12,"植树节"},{4,1,"愚人节"},{5,1,"劳动节"},{5,4,"青年节"},{6,1,"儿童节"},{7,1,"建党节"},{8,1,"建军节"},{9,10,"教师节"},{10,1,"国庆节"},{11,1,"万圣节"},{12,24,"平安夜"},{12,25,"圣诞节"}};
static const festival_t lunar[] = {{1,1,"春节"},{1,15,"元宵节"},{2,2,"龙抬头"},{5,5,"端午节"},{7,7,"七夕节"},{7,15,"中元节"},{8,15,"中秋节"},{9,9,"重阳节"},{10,1,"寒衣节"},{12,8,"腊八节"},{12,30,"除夕"}};
static const uint16_t holidays[39] = {0x0101,0x0102,0x0103,0x1104,0x120E,0x020F,0x0210,0x0211,0x0212,0x0213,0x0214,0x0215,0x0216,0x0217,0x121C,0x0404,0x0405,0x0406,0x0501,0x0502,0x0503,0x0504,0x0505,0x1509,0x0613,0x0614,0x0615,0x0919,0x091A,0x091B,0x1914,0x0A01,0x0A02,0x0A03,0x0A04,0x0A05,0x0A06,0x0A07,0x1A0A};
static const char special_names[5][10] = {"除夕","母亲节","父亲节","感恩节","清明节"};

bool calendar_resources_init(void) { return true; }
bool calendar_resources_retry_init(void) { return true; }
bool calendar_resources_ready(void) { return true; }
uint16_t calendar_resources_year_min(void) { return 2025; }
uint16_t calendar_resources_year_max(void) { return 2027; }
uint16_t calendar_resources_holiday_year(void) { return 2026; }
bool calendar_resource_read(calendar_resource_section_t s, uint16_t i, void *d, size_t n)
{ (void)s; (void)i; (void)d; (void)n; return false; }
bool calendar_resource_text(calendar_resource_section_t s, uint16_t i, char *d, size_t n)
{
    const void *src = NULL; size_t width = 0, count = 0;
    switch (s) {
    case CAL_RES_MONTH_NAMES: src=month_names;width=7;count=13;break;
    case CAL_RES_LEAP_NAMES: src=leap_names;width=4;count=2;break;
    case CAL_RES_DATE_NAMES: src=date_names;width=7;count=31;break;
    case CAL_RES_WEEKDAY_NAMES: src=weekday_names;width=4;count=7;break;
    case CAL_RES_ZODIAC_NAMES: src=zodiac_names;width=4;count=12;break;
    case CAL_RES_STEM_NAMES: src=stem_names;width=4;count=10;break;
    case CAL_RES_BRANCH_NAMES: src=branch_names;width=4;count=12;break;
    case CAL_RES_JIEQI_NAMES: src=jieqi_names;width=7;count=24;break;
    case CAL_RES_SPECIAL_FESTIVAL_NAMES: src=special_names;width=10;count=5;break;
    default:return false;
    }
    if (i>=count || n<width) return false;
    memcpy(d,(const uint8_t*)src+i*width,width); return true;
}
bool calendar_resource_u32(calendar_resource_section_t s, uint16_t i, uint32_t *v)
{
    const uint32_t *p = s==CAL_RES_LUNAR_MONTH_DAYS?month_days:s==CAL_RES_SOLAR_NEW_YEAR?new_year:NULL;
    if (!p || !v || i>=5) return false;
    *v=p[i];
    return true;
}
bool calendar_resource_u8(calendar_resource_section_t s, uint16_t i, uint8_t *v)
{
    const uint8_t *p=s==CAL_RES_JIEQI_YEAR_BITS?jieqi_bits:s==CAL_RES_JIEQI_BASE_DAYS?jieqi_days:NULL;
    size_t count=s==CAL_RES_JIEQI_YEAR_BITS?12:24;
    if(!p||!v||i>=count)return false;
    *v=p[i];
    return true;
}
bool calendar_resource_festival(calendar_resource_section_t s, uint8_t m, uint8_t d, char *n, size_t c)
{
    const festival_t *p=s==CAL_RES_SOLAR_FESTIVALS?solar:s==CAL_RES_LUNAR_FESTIVALS?lunar:NULL;
    size_t count=s==CAL_RES_SOLAR_FESTIVALS?15:11;
    if(!p||!n||c<10)return false;
    for(size_t i=0;i<count;i++)if(p[i].month==m&&p[i].day==d){memcpy(n,p[i].name,10);return true;}
    return false;
}
bool calendar_resource_holiday(uint16_t y, uint8_t m, uint8_t d, bool *w)
{
    if(y!=2026||!w)return false;
    for(size_t i=0;i<39;i++)if(((holidays[i]>>8)&15)==m&&(holidays[i]&255)==d){*w=(holidays[i]>>12)!=0;return true;}
    return false;
}
