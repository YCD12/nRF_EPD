#include "Lunar.h"

/*******************************************************************************
 *                      农历字符串常量表 - Lunar String Constants
 * ----------------------------------------------------------------------------
 * 以下数组定义了农历日期显示所需的中文字符串
 ******************************************************************************/

/**
 * 农历月份名称表 - Lunar Month Names
 * 索引0为占位符，索引1-12对应正月到腊月
 * 注意：十一月称"冬月"，十二月称"腊月"
 */
const char Lunar_MonthString[13][7] = {"----", "正月", "二月", "三月", "四月", "五月", "六月",
                                       "七月", "八月", "九月", "十月", "冬月", "腊月"};

/**
 * 闰月标识 - Leap Month Indicator
 * 索引0: 空格（非闰月）, 索引1: "闰"（闰月）
 */
const char Lunar_MonthLeapString[2][4] = {" ", "闰"};

/**
 * 农历日期名称表 - Lunar Day Names
 * 索引0为占位符，索引1-30对应初一到三十
 * 农历日期命名规则:
 *   1-10日: 初一、初二、...、初十
 *   11-20日: 十一、十二、...、二十
 *   21-29日: 廿一、廿二、...、廿九 (廿=二十)
 *   30日: 三十
 */
const char Lunar_DateString[31][7] = {"----", "初一", "初二", "初三", "初四", "初五", "初六", "初七",
                                      "初八", "初九", "初十", "十一", "十二", "十三", "十四", "十五",
                                      "十六", "十七", "十八", "十九", "二十", "廿一", "廿二", "廿三",
                                      "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};

/**
 * 星期名称表 - Day of Week Names
 * 索引0-6对应星期日到星期六
 */
const char Lunar_DayString[7][4] = {"日", "一", "二", "三", "四", "五", "六"};

/**
 * 十二生肖表 - Chinese Zodiac Animals (12-year cycle)
 * 排列顺序从猴开始，是因为计算公式 year % 12 的结果对应关系:
 *   0:猴, 1:鸡, 2:狗, 3:猪, 4:鼠, 5:牛, 6:虎, 7:兔, 8:龙, 9:蛇, 10:马, 11:羊
 * 例如: 2024年(甲辰龙年) % 12 = 8，对应数组索引8即"龙"
 */
const char Lunar_ZodiacString[12][4] = {"猴", "鸡", "狗", "猪", "鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊"};

/**
 * 十天干表 - Ten Heavenly Stems (天干)
 * 天干是中国古代的计数符号，与地支配合使用
 * 排列顺序从庚开始，是因为计算公式 year % 10 的结果对应关系:
 *   0:庚, 1:辛, 2:壬, 3:癸, 4:甲, 5:乙, 6:丙, 7:丁, 8:戊, 9:己
 * 例如: 2024年 % 10 = 4，对应数组索引4即"甲"
 */
const char Lunar_StemStrig[10][4] = {"庚", "辛", "壬", "癸", "甲", "乙", "丙", "丁", "戊", "己"};

/**
 * 十二地支表 - Twelve Earthly Branches (地支)
 * 地支与天干配合组成六十甲子，用于纪年、纪月、纪日、纪时
 * 排列顺序从申开始，是因为计算公式 year % 12 的结果对应关系:
 *   0:申, 1:酉, 2:戌, 3:亥, 4:子, 5:丑, 6:寅, 7:卯, 8:辰, 9:巳, 10:午, 11:未
 * 例如: 2024年 % 12 = 8，对应数组索引8即"辰"，所以2024年是甲辰年
 *
 * 干支纪年说明:
 *   天干配地支可得60种组合（六十甲子），循环使用用于纪年
 *   如: 甲子、乙丑、丙寅、...、癸亥，然后又从甲子开始
 */
const char Lunar_BranchStrig[12][4] = {"申", "酉", "戌", "亥", "子", "丑", "寅", "卯", "辰", "巳", "午", "未"};

/*******************************************************************************
 *                    农历数据表 - Lunar Calendar Data Tables
 * ----------------------------------------------------------------------------
 * 农历转换核心数据表，采用压缩编码方式存储每年的农历信息
 *
 * 【重要】年份范围限制: 2024 ~ 2029 (共6年数据)
 *        有效显示范围: 2025 ~ 2029 (超出范围使用2025年数据)
 ******************************************************************************/

/**
 * 农历每月天数编码表 - Lunar Month Days Encoding
 * lunar_month_days[] 存储每年农历月份天数信息
 *
 * 数据格式 (32位压缩编码):
 *   Bit[16:13] - 闰月月份 (0表示无闰月，1-12表示闰几月)
 *   Bit[12:0]  - 每月天数标志 (1=30天大月, 0=29天小月)
 *                从高位到低位依次表示1月到12月（如有闰月则为13个月）
 *
 * 解码示例 (以2024年为例):
 *   0x0000096C = 0000 0000 0000 0000 0000 1001 0110 1100
 *   Bit[16:13] = 0000 = 0 → 无闰月
 *   Bit[12:0]  = 1001011011000 → 各月天数:
 *     正月30天, 二月29天, 三月29天, 四月30天, 五月29天, 六月30天,
 *     七月30天, 八月29天, 九月30天, 十月30天, 冬月29天, 腊月29天
 *
 * 【原理说明】
 * 农历月份有大月(30天)和小月(29天)之分，每年不同，需要查表确定
 * 闰月是为了协调农历年与回归年的差异而设置的额外月份
 */
/* 2024 ~ 2029  */
const uint32_t lunar_month_days[] = {
    2024,        /* 索引0: 基准年份标识 */
    0x0000096C,  /* 2024: 农历甲辰年（龙年）月份数据 */
    0x0000D4AE,  /* 2025: 农历乙巳年（蛇年）月份数据 */
    0x0000149C,  /* 2026: 农历丙午年（马年）月份数据 */
    0x00001A4C,  /* 2027: 农历丁未年（羊年）月份数据 */
    0x0000BD26,  /* 2028: 农历戊申年（猴年）月份数据 */
    0x00001AA6,  /* 2029: 农历己酉年（鸡年）月份数据 */
};

/**
 * 农历正月初一对应的公历日期编码表 - Solar Date of Lunar New Year
 * solar_1_1[] 存储每年农历正月初一对应的公历日期
 *
 * 数据格式 (32位压缩编码):
 *   Bit[20:9]  - 公历年份 (12位)
 *   Bit[8:5]   - 公历月份 (4位, 1-12)
 *   Bit[4:0]   - 公历日期 (5位, 1-31)
 *
 * 解码示例 (以2024年为例):
 *   0x000FD04A = 0000 0000 0000 1111 1101 0000 0100 1010
 *   Bit[20:9]  = 0111 1110 1000 = 2024 → 公历年
 *   Bit[8:5]   = 0010 = 2 → 公历2月
 *   Bit[4:0]   = 01010 = 10 → 公历10日
 *   即: 2024年农历正月初一 = 公历2024年2月10日
 *
 * 【原理说明】
 * 农历新年(春节)每年公历日期不同，通常在1月21日到2月20日之间
 * 通过查表获取春节日期，再结合月份天数表可推算任意农历日期
 */
/* 2024 ~ 2029  */
const uint32_t solar_1_1[] = {
    2024,        /* 索引0: 基准年份标识 */
    0x000FD04A,  /* 2024年春节: 公历2月10日 */
    0x000FD23D,  /* 2025年春节: 公历1月29日 */
    0x000FD451,  /* 2026年春节: 公历2月17日 */
    0x000FD646,  /* 2027年春节: 公历2月6日 */
    0x000FD83A,  /* 2028年春节: 公历1月26日 */
    0x000FDA4D,  /* 2029年春节: 公历2月13日 */
};

/*******************************************************************************
 *                        辅助函数 - Helper Functions
 ******************************************************************************/

/**
 * @brief 位域提取函数 - Bit Field Extraction
 * @param data 源数据
 * @param length 要提取的位数
 * @param shift 起始位置（从最低位开始计数）
 * @return 提取出的数值
 *
 * 功能说明:
 *   从32位数据中提取指定位置、指定长度的位域
 *   公式: (data & (((1 << length) - 1) << shift)) >> shift
 *
 * 示例: GetBitInt(0xABCD, 4, 8)
 *       提取从第8位开始的4位 = 0xB
 */
static uint32_t GetBitInt(uint32_t data, uint8_t length, uint8_t shift) {
    return (data & (((1 << length) - 1) << shift)) >> shift;
}

/**
 * @brief 公历日期转儒略日数 - Solar Date to Julian Day Number
 * @param y 公历年
 * @param m 公历月 (1-12)
 * @param d 公历日 (1-31)
 * @return 简化的儒略日数（用于日期差计算）
 *
 * WARNING: Dates before Oct. 1582 are inaccurate
 * 警告: 1582年10月之前的日期不准确（格里高利历改革前）
 *
 * 【算法说明】
 * 此函数使用简化的儒略日计算公式，主要用于计算两个日期之间的天数差
 * 公式原理:
 *   1. 将1月和2月视为上一年的13月和14月
 *   2. 计算从公元0年到指定日期的总天数
 *   3. 考虑闰年规则: 4年一闰，100年不闰，400年再闰
 */
// WARNING: Dates before Oct. 1582 are inaccurate
static uint16_t SolarToInt(uint16_t y, uint8_t m, uint8_t d) {
    m = (m + 9) % 12;           /* 将月份重新编号: 3月=0, 4月=1, ..., 2月=11 */
    y = y - m / 10;             /* 对于1月和2月，年份减1 */
    /* 计算总天数:
     * 365*y: 每年365天
     * y/4 - y/100 + y/400: 闰年修正
     * (m*306 + 5)/10: 月份内天数累计
     * (d - 1): 日期
     */
    return 365 * y + y / 4 - y / 100 + y / 400 + (m * 306 + 5) / 10 + (d - 1);
}

/*******************************************************************************
 *                   公历转农历核心函数 - Solar to Lunar Conversion
 ******************************************************************************/

/**
 * @brief 公历转农历 - Convert Solar Date to Lunar Date
 * @param lunar 输出参数，存储转换后的农历日期
 * @param solar_year 公历年份
 * @param solar_month 公历月份 (1-12)
 * @param solar_date 公历日期 (1-31)
 *
 * 【算法流程】
 * 1. 参数有效性检查
 * 2. 年份范围检查（超出2025-2029范围则使用2025年数据）
 * 3. 查找该公历日期对应的农历年（通过比较春节日期）
 * 4. 计算公历日期与春节的天数差
 * 5. 根据月份天数表逐月累减，确定农历月份和日期
 * 6. 处理闰月情况
 *
 * 【年份限制说明】
 * 有效年份范围: 2025 ~ 2029
 * 超出范围时使用2025年的农历数据，仅显示农历月日，年份不准确
 */
void LUNAR_SolarToLunar(struct Lunar_Date* lunar, uint16_t solar_year, uint8_t solar_month, uint8_t solar_date) {
    uint8_t i, lunarM, m, d, leap, dm;
    uint16_t year_index, lunarY, y, offset;
    uint32_t solar_data, solar11, days;

    /* 步骤1: 参数有效性检查 - Parameter Validation */
    if (solar_month < 1 || solar_month > 12 || solar_date < 1 || solar_date > 31) {
        lunar->Year = 0;
        lunar->Month = 0;
        lunar->Date = 0;
        lunar->IsLeap = 0;
        return;
    }

    /* 步骤2: 计算年份范围 - Calculate Year Range */
    uint16_t year_base = (uint16_t)solar_1_1[0];                                    /* 基准年份: 2024 */
    uint16_t year_count = (uint16_t)(sizeof(solar_1_1) / sizeof(uint32_t) - 1);    /* 数据条目数: 6 */
    uint16_t year_min = (uint16_t)(year_base + 1);                                  /* 有效起始年: 2025 */
    uint16_t year_max = (uint16_t)(year_base + year_count - 1);                    /* 有效结束年: 2029 */

    /* 步骤3: 年份范围限制处理 - Year Range Limitation */
    uint16_t calc_year = solar_year;
    if (calc_year < year_min || calc_year > year_max) {
        /* 只保留 2025~2029 的精确数据，其余年份按 2025 年的农历数据显示 */
        calc_year = year_min;
    }

    /* 步骤4: 计算数据表索引 - Calculate Table Index */
    year_index = (uint16_t)(calc_year - year_base + 1);  /* 索引 1~year_count（对应 2024~2029） */

    /* 步骤5: 判断公历日期是否在当年春节之前 - Check if Before Lunar New Year */
    /* 将公历日期编码为单一数值便于比较 */
    solar_data = ((uint32_t)calc_year << 9) | ((uint32_t)solar_month << 5) | ((uint32_t)solar_date);
    if (solar_1_1[year_index] > solar_data && year_index > 1) {
        /* 如果公历日期在该年春节之前，则属于上一个农历年 */
        year_index -= 1;
    }

    /* 步骤6: 获取春节日期并解码 - Get and Decode Lunar New Year Date */
    solar11 = solar_1_1[year_index];
    y = GetBitInt(solar11, 12, 9);    /* 提取春节所在公历年份 */
    m = GetBitInt(solar11, 4, 5);     /* 提取春节所在公历月份 */
    d = GetBitInt(solar11, 5, 0);     /* 提取春节所在公历日期 */

    /* 步骤7: 计算公历日期与春节的天数差 - Calculate Days Offset from New Year */
    offset = SolarToInt(calc_year, solar_month, solar_date) - SolarToInt(y, m, d);

    /* 步骤8: 获取该农历年的月份信息 - Get Lunar Month Information */
    days = lunar_month_days[year_index];
    leap = GetBitInt(days, 4, 13);    /* 提取闰月月份 (0=无闰月, 1-12=闰几月) */

    /* 步骤9: 计算农历年份 - Calculate Lunar Year */
    lunarY = (uint16_t)(year_base + year_index - 1);
    lunarM = 1;      /* 从正月开始 */
    offset += 1;     /* 偏移量加1（因为初一是第1天） */

    /* 步骤10: 逐月累减确定农历月份 - Determine Lunar Month by Subtracting */
    for (i = 0; i < 13; i++) {
        /* 获取当月天数: Bit[12-i]为1表示大月(30天)，为0表示小月(29天) */
        if (GetBitInt(days, 1, 12 - i) == 1) {
            dm = 30;    /* 大月30天 */
        } else {
            dm = 29;    /* 小月29天 */
        }
        if (offset > dm) {
            lunarM += 1;      /* 进入下一个月 */
            offset -= dm;     /* 减去当月天数 */
        } else {
            break;            /* 找到目标月份 */
        }
    }

    /* 步骤11: 处理闰月 - Handle Leap Month */
    lunar->IsLeap = 0;
    if (leap != 0 && lunarM > leap) {
        /* 如果存在闰月且当前月份序号大于闰月位置 */
        if (lunarM == leap + 1) {
            /* 正好是闰月 */
            lunar->IsLeap = 1;
        }
        /* 闰月后的月份序号需要减1（因为闰月与前一个月同名） */
        lunarM -= 1;
    }

    /* 步骤12: 设置返回值 - Set Return Values */
    lunar->Month = lunarM;      /* 农历月份 */
    lunar->Date = offset;       /* 农历日期 */
    lunar->Year = lunarY;       /* 农历年份 */
}

/*******************************************************************************
 *                   干支生肖计算函数 - Stem-Branch & Zodiac Calculation
 ******************************************************************************/

/**
 * @brief 获取生肖索引 - Get Chinese Zodiac Index
 * @param lunar 农历日期结构体
 * @return 生肖索引 (0-11)
 *
 * 【计算原理】
 * 十二生肖按固定顺序循环: 子鼠、丑牛、寅虎、卯兔、辰龙、巳蛇、午马、未羊、申猴、酉鸡、戌狗、亥猪
 * 由于数组从猴开始排列（与地支"申"对应），直接用年份对12取模即可
 *
 * 示例: 2024年(龙年) % 12 = 8 → 数组索引8 = "龙"
 */
uint8_t LUNAR_GetZodiac(const struct Lunar_Date* lunar) { return lunar->Year % 12; }

/**
 * @brief 获取天干索引 - Get Heavenly Stem Index
 * @param lunar 农历日期结构体
 * @return 天干索引 (0-9)
 *
 * 【计算原理】
 * 十天干按固定顺序循环: 甲、乙、丙、丁、戊、己、庚、辛、壬、癸
 * 由于数组从庚开始排列，直接用年份对10取模即可
 *
 * 示例: 2024年 % 10 = 4 → 数组索引4 = "甲"
 */
uint8_t LUNAR_GetStem(const struct Lunar_Date* lunar) { return lunar->Year % 10; }

/**
 * @brief 获取地支索引 - Get Earthly Branch Index
 * @param lunar 农历日期结构体
 * @return 地支索引 (0-11)
 *
 * 【计算原理】
 * 十二地支按固定顺序循环: 子、丑、寅、卯、辰、巳、午、未、申、酉、戌、亥
 * 由于数组从申开始排列，直接用年份对12取模即可
 *
 * 示例: 2024年 % 12 = 8 → 数组索引8 = "辰"
 * 配合天干"甲"，2024年即为"甲辰年"
 */
uint8_t LUNAR_GetBranch(const struct Lunar_Date* lunar) { return lunar->Year % 12; }

/*********************************************************************************************************
 **         以下为24节气计算相关程序 - Solar Terms (24 JieQi) Calculation
 **------------------------------------------------------------------------------------------------------
 ** 【节气概述】
 ** 二十四节气是中国古代订立的补充历法，用于指导农业生产
 ** 每月两个节气，全年24个，平均每15天一个
 ** 节气反映太阳在黄道上的位置，与公历日期基本固定（每年相差1-2天）
 ********************************************************************************************************/

/**
 * 节气年份范围定义 - Solar Term Year Range
 * 【重要】有效年份: 2024 ~ 2029
 *        超出范围时使用2025年的节气数据
 */
#define JIEQI_YEAR_MIN 2024
#define JIEQI_YEAR_MAX 2029

/**
 * 节气日期修正标志表 - Solar Term Date Correction Flags
 * YearMonthBit[] 存储每年24个节气的日期修正标志
 *
 * 数据格式:
 *   每年使用3个字节(24位)，每位对应一个节气
 *   位值为1表示需要修正（日期+1或-1）
 *   位值为0表示使用基准日期
 *
 * 排列顺序: 从最高位到最低位依次对应24个节气
 *   Byte0 Bit7: 小寒, Bit6: 大寒, Bit5: 立春, Bit4: 雨水, ...
 *   Byte1: 立夏~处暑
 *   Byte2: 白露~冬至
 *
 * 【原理说明】
 * 节气日期每年略有不同，但变化范围很小（通常只差1天）
 * 通过基准日期 + 修正值的方式，可以大大减少存储空间
 * 每年只需3字节即可存储24个节气的精确日期
 *
 * 有兴趣的朋友可按照上面给的原理添加其它年份的表格
 * 不是很清楚的朋友可给我发EMAIL
 */
static const uint8_t YearMonthBit[(JIEQI_YEAR_MAX - JIEQI_YEAR_MIN + 1) * 3] = {
    0x0F, 0xEF, 0xDB,  /* 2024年节气修正标志 */
    0xBE, 0xA6, 0x99,  /* 2025年节气修正标志 */
    0x9C, 0xA2, 0x98,  /* 2026年节气修正标志 */
    0x80, 0x00, 0x18,  /* 2027年节气修正标志 */
    0x0F, 0xEF, 0xDB,  /* 2028年节气修正标志 */
    0xBE, 0xA6, 0x99,  /* 2029年节气修正标志 */
};

/**
 * 节气基准日期表 - Solar Term Base Dates
 * days[] 存储每个节气的基准日期（通常是最常见的日期）
 *
 * 排列顺序: 按月份和半月排列
 *   索引0-1: 一月的两个节气（小寒、大寒）
 *   索引2-3: 二月的两个节气（立春、雨水）
 *   ...以此类推
 *
 * 【二十四节气】按时间顺序:
 *   春季: 立春、雨水、惊蛰、春分、清明、谷雨
 *   夏季: 立夏、小满、芒种、夏至、小暑、大暑
 *   秋季: 立秋、处暑、白露、秋分、寒露、霜降
 *   冬季: 立冬、小雪、大雪、冬至、小寒、大寒
 */
static const uint8_t days[24] = {
    6, 20, 4, 19, 6, 21,  /* 一月到三月的节气基本日期: 小寒6日, 大寒20日, 立春4日, 雨水19日, 惊蛰6日, 春分21日 */
    5, 20, 6, 21, 6, 21,  /* 四月到六月的节气基本日期: 清明5日, 谷雨20日, 立夏6日, 小满21日, 芒种6日, 夏至21日 */
    7, 23, 8, 23, 8, 23,  /* 七月到九月的节气基本日期: 小暑7日, 大暑23日, 立秋8日, 处暑23日, 白露8日, 秋分23日 */
    8, 24, 8, 22, 7, 22,  /* 十月到十二月的节气基本日期: 寒露8日, 霜降24日, 立冬8日, 小雪22日, 大雪7日, 冬至22日 */
};

/**
 * 二十四节气名称表 - Solar Term Names (24 JieQi)
 * 按公历月份顺序排列（从一月开始）
 *
 * 【节气含义说明】
 * 小寒: 天气渐寒，尚未大冷
 * 大寒: 一年中最寒冷的时期
 * 立春: 春季开始
 * 雨水: 降水开始增多
 * 惊蛰: 春雷始鸣，蛰虫惊醒
 * 春分: 昼夜平分，春季中点
 * 清明: 天气清朗明净
 * 谷雨: 雨生百谷
 * 立夏: 夏季开始
 * 小满: 麦类等作物籽粒开始饱满
 * 芒种: 有芒作物成熟，开始播种
 * 夏至: 白昼最长，夏季中点
 * 小暑: 天气炎热，尚未极热
 * 大暑: 一年中最热的时期
 * 立秋: 秋季开始
 * 处暑: 暑气消退
 * 白露: 天气转凉，露水凝白
 * 秋分: 昼夜平分，秋季中点
 * 寒露: 露水更凉，将要结冰
 * 霜降: 开始降霜
 * 立冬: 冬季开始
 * 小雪: 开始降雪，雪量较小
 * 大雪: 雪量增大
 * 冬至: 白昼最短，冬季中点
 */
const char JieQiStr[24][7] = {
    "小寒", "大寒", "立春", "雨水", "惊蛰", "春分",  /* 一月~三月 */
    "清明", "谷雨", "立夏", "小满", "芒种", "夏至",  /* 四月~六月 */
    "小暑", "大暑", "立秋", "处暑", "白露", "秋分",  /* 七月~九月 */
    "寒露", "霜降", "立冬", "小雪", "大雪", "冬至",  /* 十月~十二月 */
};

/**
 * 每月最大天数表 - Maximum Days per Month
 * 用于日期计算（2月按平年28天计算，闰年需特殊处理）
 */
const uint8_t MonthDayMax[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
};

/*********************************************************************************************************
 ** 函数名称: GetJieQi
 ** 功能描述: 获取指定日期所在半月的节气日期
 **          Get Solar Term Date for the Half-Month of Given Date
 ** 输入参数:
 **          myear   - 公历年份 (有效范围: 2025~2029)
 **          mmonth  - 公历月份 (1-12)
 **          mday    - 公历日期 (1-31)，用于判断上半月或下半月
 **          JQdate  - 输出参数，存储节气日期
 ** 输出返回:
 **          1 - 成功
 **          0 - 失败（月份参数无效）
 **
 ** 【算法说明】
 ** 1. 根据月份和日期确定节气序号（每月2个节气，上半月取第一个，下半月取第二个）
 ** 2. 从修正标志表中获取对应位的值
 ** 3. 用基准日期加上修正值得到实际日期
 **
 ** 【修正规则】
 ** - 对于节气索引1(大寒)、11(夏至)、18(寒露)、21(小雪)，且年份<2044时:
 **   修正值为1表示日期+1
 ** - 其他节气: 修正值为1表示日期-1
 **
 ** 作者: 赖皮 ★〓个人原创〓★
 ** 日期: 2007年02月08日
 **------------------------------------------------------------------------------------------------------
 ********************************************************************************************************/
uint8_t GetJieQi(uint16_t myear, uint8_t mmonth, uint8_t mday, uint8_t* JQdate) {
    uint8_t bak1, value, JQ;

    /* 参数检查 - Parameter Validation */
    if ((mmonth == 0) || (mmonth > 12)) return 0;

    /* 计算年份有效范围 - Calculate Valid Year Range */
    uint16_t year_base = JIEQI_YEAR_MIN;                         /* 表中起始年: 2024 */
    uint16_t year_count = (JIEQI_YEAR_MAX - JIEQI_YEAR_MIN + 1); /* 共6年数据: 2024~2029 */
    uint16_t year_min = (uint16_t)(year_base + 1);               /* 有效显示起始年: 2025 */
    uint16_t year_max = (uint16_t)(year_base + year_count - 1);  /* 有效显示结束年: 2029 */

    /* 年份范围限制 - Year Range Limitation */
    uint16_t calc_year = myear;
    if (calc_year < year_min || calc_year > year_max) {
        /* 只保留 2025~2029 的精确数据，其余年份按 2025 年的节气数据显示 */
        calc_year = year_min;
    }

    /* 重复检查（冗余但保留原代码逻辑） */
    if ((mmonth == 0) || (mmonth > 12)) return 0;

    /* 计算节气序号 - Calculate Solar Term Index */
    JQ = (mmonth - 1) * 2;  /* 获得节气顺序标号(0～23)，每月2个节气 */
    if (mday >= 15) JQ++;   /* 判断是否是下半月，下半月取第二个节气 */

    /* 获取修正标志 - Get Correction Flag */
    bak1 = YearMonthBit[(calc_year - year_base) * 3 + JQ / 8];  /* 获得节气日期修正值所在字节 */
    value = ((bak1 << (JQ % 8)) & 0x80);                        /* 获得节气日期修正标志位 */

    /* 计算实际日期 - Calculate Actual Date */
    *JQdate = days[JQ];    /* 获取基准日期 */
    if (value != 0) {
        /* 需要修正日期 */
        /* 判断年份,以决定节气相对值1代表+1,还是-1 */
        /* 特殊节气（大寒、夏至、寒露、小雪）在2044年之前，修正值1表示+1天 */
        if ((JQ == 1 || JQ == 11 || JQ == 18 || JQ == 21) && myear < 2044)
            (*JQdate)++;    /* 日期加1 */
        else
            (*JQdate)--;    /* 日期减1 */
    }
    return 1;
}

/*********************************************************************************************************
 ** 函数名称: GetJieQiStr
 ** 功能描述: 获取当前日期的节气信息及距离下一个节气的天数
 **          Get Solar Term Info and Days Until Next Term
 ** 输入参数:
 **          myear   - 公历年份 (有效范围: 2025~2029)
 **          mmonth  - 公历月份 (1-12)
 **          mday    - 公历日期 (1-31)
 **          day     - 输出参数，距离节气的天数
 ** 输出返回:
 **          0~23    - 节气索引（可用于索引JieQiStr数组）
 **          0xFF    - 失败
 **
 ** 【返回值说明】
 ** - 如果今天正好是节气日，day=0，返回当前节气索引
 ** - 如果今天不是节气日，day=距离下一个节气的天数，返回下一个节气索引
 **
 ** 示例: GetJieQiStr(2025, 2, 8, &day)
 **       如果2月8日不是节气日，返回下一个节气"雨水"的索引3，day=天数差
 **
 ** 作者: 赖皮 ★〓个人原创〓★
 ** 日期: 2007年02月08日
 **------------------------------------------------------------------------------------------------------
 ********************************************************************************************************/
uint8_t GetJieQiStr(uint16_t myear, uint8_t mmonth, uint8_t mday, uint8_t* day) {
    uint8_t JQdate, JQ, MaxDay;

    /* 获取当前半月的节气日期 - Get Solar Term Date */
    if (GetJieQi(myear, mmonth, mday, &JQdate) == 0) return 0xFF;

    /* 计算节气序号 - Calculate Solar Term Index */
    JQ = (mmonth - 1) * 2;  /* 获得节气顺序标号(0～23) */
    if (mday >= 15) JQ++;   /* 判断是否是下半月 */

    /* 情况1: 今天正好是节气日 - Today is a Solar Term Day */
    if (mday == JQdate)
    {
        *day = 0;
        return JQ;
    }

    /* 情况2: 今天不是节气日 - Today is NOT a Solar Term Day */
    /* 需要计算距离下一个节气的天数 */

    if (mday < JQdate)  /* 如果今天日期小于本半月的节气日期 */
    {
        /* 下一个节气就在本半月内 */
        mday = JQdate - mday;    /* 计算天数差 */
    }
    else  /* 如果今天日期大于本半月的节气日期（已过该节气） */
    {
        JQ++;    /* 指向下一个节气 */

        if (mday < 15) {
            /* 当前在上半月，下一个节气在下半月 */
            GetJieQi(myear, mmonth, 15, &JQdate);    /* 获取下半月节气日期 */
            mday = JQdate - mday;                    /* 计算天数差 */
        }
        else  /* 当前在下半月，下一个节气在下个月 - 需要翻月 */
        {
            MaxDay = MonthDayMax[mmonth - 1];    /* 获取当月最大天数 */

            /* 处理闰年2月 - Handle February in Leap Year */
            if (mmonth == 2)
            {
                /* 闰年判断: 能被4整除且(不能被100整除或能被400整除) */
                if ((myear % 4 == 0) && ((myear % 100 != 0) || (myear % 400 == 0))) MaxDay++;
            }

            /* 处理月份进位 - Handle Month Overflow */
            if (++mmonth == 13) mmonth = 1;    /* 12月之后是1月 */

            /* 获取下个月上半月的节气日期 */
            GetJieQi(myear, mmonth, 1, &JQdate);

            /* 计算天数差: 本月剩余天数 + 下月节气日期 */
            mday = MaxDay - mday + JQdate;
        }
    }
    *day = mday;    /* 输出距离天数 */
    return JQ;      /* 返回节气索引 */
}

/*******************************************************************************
 *                  时间转换相关常量 - Time Conversion Constants
 ******************************************************************************/

/**
 * 每年的总秒数 - Seconds per Year
 * SEC_PER_YR[0]: 平年秒数 = 365 * 24 * 3600 = 31,536,000
 * SEC_PER_YR[1]: 闰年秒数 = 366 * 24 * 3600 = 31,622,400
 */
uint32_t SEC_PER_YR[2] = {31536000, 31622400};

/**
 * 每月的总秒数 - Seconds per Month
 * SEC_PER_MT[0][]: 平年各月秒数
 * SEC_PER_MT[1][]: 闰年各月秒数（仅2月不同）
 *
 * 秒数计算: 天数 * 86400
 *   31天 = 2,678,400秒
 *   30天 = 2,592,000秒
 *   29天 = 2,505,600秒
 *   28天 = 2,419,200秒
 */
uint32_t SEC_PER_MT[2][12] = {
    {2678400, 2419200, 2678400, 2592000, 2678400, 2592000, 2678400, 2678400, 2592000, 2678400, 2592000, 2678400},  /* 平年 */
    {2678400, 2505600, 2678400, 2592000, 2678400, 2592000, 2678400, 2678400, 2592000, 2678400, 2592000, 2678400},  /* 闰年 */
};

#define SECOND_OF_DAY 86400  /* 一天的秒数 = 24 * 60 * 60 */

/*******************************************************************************
 *                        闰年判断函数 - Leap Year Check
 ******************************************************************************/

/**
 * @Name       : static int is_leap(int yr)
 * @Description: 判断是否为闰年 - Check if a Year is a Leap Year
 * @In         : yr - 待判断的年份
 * @Out        : 1:是闰年   0:非闰年
 *
 * 【闰年规则】Leap Year Rules:
 * "非整百年份：能被4整除的是闰年。"
 *   Non-century years: Divisible by 4 is a leap year.
 * "整百年份：能被400整除的是闰年。"
 *   Century years: Divisible by 400 is a leap year.
 *
 * 示例: 2024年是闰年(2024%4=0)，2100年不是(2100%100=0但2100%400!=0)，2000年是(2000%400=0)
 *
 * @Author     : Denis
 */
int is_leap(int yr) {
    if (0 == (yr % 100))
        return (yr % 400 == 0) ? 1 : 0;    /* 整百年份需能被400整除 */
    else
        return (yr % 4 == 0) ? 1 : 0;      /* 非整百年份能被4整除即可 */
}

/*******************************************************************************
 *                    星期计算函数 - Day of Week Calculation
 ******************************************************************************/

/**
 * @Name       : day_of_week_get
 * @Description: 根据输入的年月日计算当天为星期几
 *               Calculate Day of Week from Date (Using Tomohiko Sakamoto Algorithm)
 * @In         : month - 月份 (1-12)
 *               day   - 日期 (1-31)
 *               year  - 年份
 * @Out        : 星期几 (0=星期日, 1=星期一, ..., 6=星期六)
 *
 * 【算法说明】坂本智彦算法 (Tomohiko Sakamoto Algorithm)
 * 这是一种高效的星期计算公式，基于蔡勒公式的变体
 * t[]数组存储每月的修正值，用于补偿月份长度的不规则性
 *
 * 算法步骤:
 * 1. 如果是1月或2月，年份减1（将1月2月视为上一年的13月14月）
 * 2. 计算: (年 + 年/4 - 年/100 + 年/400 + 月修正值 + 日) % 7
 *
 * @Author     : Denis
 */
unsigned char day_of_week_get(unsigned char month, unsigned char day, unsigned short year) {
    /* Month should be a number 0 to 11, Day should be a number 1 to 31 */
    /* 月份修正表: 用于补偿每月天数差异对星期计算的影响 */
    static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    year -= (uint8_t)(month < 3);    /* 1月和2月，年份减1 */
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

/*******************************************************************************
 *                Unix时间戳转换函数 - Unix Timestamp Conversion
 ******************************************************************************/

/**
 * @brief Unix时间戳转时间结构体 - Convert Unix Timestamp to Time Structure
 * @param unix_time Unix时间戳（从1970年1月1日00:00:00 UTC起的秒数）
 * @param result 输出参数，存储转换后的时间
 *
 * 【算法流程】
 * 1. 逐年减去当年秒数，直到剩余秒数小于一年
 * 2. 逐月减去当月秒数，直到剩余秒数小于一月
 * 3. 计算日期 = 剩余秒数 / 86400 + 1
 * 4. 计算小时 = (剩余秒数 % 86400) / 3600
 * 5. 计算分钟 = (剩余秒数 % 3600) / 60
 * 6. 计算秒 = 剩余秒数 % 60
 * 7. 计算星期
 *
 * 注意: 结果中的tm_year存储的是相对于1900年的偏移量（减去YEAR0后）
 */
void transformTime(uint32_t unix_time, struct devtm* result) {
    int leapyr = 0;
    uint32_t ltime = unix_time;

    memset(result, 0, sizeof(struct devtm));
    result->tm_year = EPOCH_YR;    /* 从1970年开始计算 */

    /* 步骤1: 逐年减去秒数，确定年份 - Determine Year */
    while (1) {
        if (ltime < SEC_PER_YR[is_leap(result->tm_year)]) {
            break;    /* 剩余秒数不足一年，退出循环 */
        }
        ltime -= SEC_PER_YR[is_leap(result->tm_year)];
        ++(result->tm_year);
    }

    leapyr = is_leap(result->tm_year);    /* 记录当前年是否为闰年 */

    /* 步骤2: 逐月减去秒数，确定月份 - Determine Month */
    while (1) {
        if (ltime < SEC_PER_MT[leapyr][result->tm_mon]) break;
        ltime -= SEC_PER_MT[leapyr][result->tm_mon];
        ++(result->tm_mon);
    }

    /* 步骤3: 计算日期 - Calculate Day */
    result->tm_mday = ltime / SEC_PER_DY;
    ++(result->tm_mday);    /* 日期从1开始 */
    ltime = ltime % SEC_PER_DY;

    /* 步骤4: 计算小时 - Calculate Hour */
    result->tm_hour = ltime / SEC_PER_HR;
    ltime = ltime % SEC_PER_HR;

    /* 步骤5: 计算分钟和秒 - Calculate Minute and Second */
    result->tm_min = ltime / 60;
    result->tm_sec = ltime % 60;

    /* 步骤6: 计算星期几 - Calculate Day of Week */
    result->tm_wday = day_of_week_get(result->tm_mon + 1, result->tm_mday, result->tm_year);

    /*
     * The number of years since YEAR0"
     * 将年份转换为相对于1900年的偏移量（兼容标准C库的tm结构体）
     */
    result->tm_year -= YEAR0;
}

/**
 * 每月天数表 - Days per Month
 * 用于日期计算（2月按平年28天，闰年需特殊处理）
 */
uint8_t map[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

/**
 * @brief 获取指定月份的最后一天 - Get Last Day of Month
 * @param year 公历年份
 * @param month 月份索引 (0-11, 0表示1月)
 * @return 该月的最大日期
 *
 * 注意: 此函数的month参数是0-11，与其他函数的1-12不同
 */
uint8_t get_last_day(uint16_t year, uint8_t month) {
    if (month % 12 == 1) {    /* 2月 (索引1) */
        return map[month % 12] + is_leap(year);    /* 闰年2月29天 */
    }
    return map[month % 12];
}

/**
 * @brief 获取指定月份第一天是星期几 - Get Day of Week for First Day of Month
 * @param year 公历年份
 * @param month 公历月份 (1-12)
 * @return 星期几 (0=星期日, 1=星期一, ..., 6=星期六)
 */
uint8_t get_first_day_week(uint16_t year, uint8_t month) { return day_of_week_get(month, 1, year); }

/**
 * @brief 时间结构体转Unix时间戳 - Convert Time Structure to Unix Timestamp
 * @param result 时间结构体
 * @return Unix时间戳（从1970年1月1日00:00:00起的秒数）
 *
 * 【算法流程】
 * 1. 计算从1970年到指定年份的总天数（考虑闰年）
 * 2. 加上当年已过的完整月份天数
 * 3. 加上当月已过的天数
 * 4. 总天数 * 86400 + 小时秒数 + 分钟秒数 + 秒数
 *
 * 注意: 此函数的tm_year应为实际年份（如2025），与transformTime的输出格式不同
 */
uint32_t transformTimeStruct(struct devtm* result) {
    uint32_t Cyear = 0;

    /* 计算1970年到指定年份之间的闰年数量 */
    for (uint16_t i = 1970; i < result->tm_year; i++) {
        if (is_leap(i) == 1) Cyear++;
    }

    /* 计算总天数: 闰年天数 + 平年天数 + 当月天数 - 1（因为1日算第0天） */
    uint32_t CountDay =
        Cyear * (uint32_t)366 + (uint32_t)(result->tm_year - 1970 - Cyear) * (uint32_t)365 + result->tm_mday - 1;

    /* 加上当年已过完整月份的天数 */
    for (uint8_t i = 0; i < result->tm_mon - 1; i++) {
        CountDay += get_last_day(result->tm_year, i);
    }

    /* 计算总秒数: 天数*86400 + 时*3600 + 分*60 + 秒 */
    return (CountDay * SECOND_OF_DAY + (uint32_t)result->tm_sec + (uint32_t)result->tm_min * 60 +
            (uint32_t)result->tm_hour * 3600);
}

/**
 * @brief 获取指定月份的天数 - Get Number of Days in Month
 * @param year 年份 (仅使用低8位，会被隐式转换为uint8_t)
 * @param month 月份 (1-12)
 * @return 该月的天数
 *
 * 注意: year参数为uint8_t类型，只取年份的低8位
 *       因此闰年判断可能不准确（如2024会被视为224）
 *       建议在调用时确保year%4的结果正确
 */
uint8_t thisMonthMaxDays(uint8_t year, uint8_t month) {
    if (year % 4 == 0 && month == 2)
        return MonthDayMax[month - 1] + 1;    /* 闰年2月29天 */
    else
        return MonthDayMax[month - 1];
}
