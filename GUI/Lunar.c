#include "Lunar.h"

#include "gui_porting_hal.h"

#define LUNAR_YEAR_BASE         2024U
#define LUNAR_YEAR_COUNT        6U
#define LUNAR_NAME_MONTH_SIZE   7U
#define LUNAR_NAME_LEAP_SIZE    4U
#define LUNAR_NAME_DATE_SIZE    7U
#define LUNAR_NAME_DAY_SIZE     4U
#define LUNAR_NAME_ZODIAC_SIZE  4U
#define LUNAR_NAME_STEM_SIZE    4U
#define LUNAR_NAME_BRANCH_SIZE  4U
#define LUNAR_NAME_JIEQI_SIZE   7U

int is_leap(int yr); /* 前置声明，实现见本文件下方时间转换区 */

/*******************************************************************************
 *          农历数据库 (LCDB, Type 6) 查询引擎 - Lunar DB Query Engine
 * ----------------------------------------------------------------------------
 * MCU 端只做"查表不计算"。首次查询时从外置 Flash 读取一次 16B 主头部与年份
 * 偏移表并缓存于 RAM，后续按日索引直接读取 12B 记录。详见
 * AiCoding/lunar_calendar_database_design_and_migration.md 第 4 章。
 ******************************************************************************/
#if EPD_SUPPORT_LUNAR
#define LUNAR_DB_MAX_YEARS      8U
#define LUNAR_DB_YEAR_HEADER_SZ 16U

static bool     s_db_inited = false;   /* 是否已尝试初始化（幂等） */
static bool     s_db_valid = false;    /* 数据库是否有效可用 */
static uint8_t  s_db_year_count = 0;   /* 数据库包含的年份数 */
static uint16_t s_db_start_year = 0;   /* 起始年份 */
static uint8_t  s_db_record_size = 4; /* 单日记录大小 */
static uint32_t s_db_year_offsets[LUNAR_DB_MAX_YEARS]; /* 各年份块相对资源起始的偏移 */

/* 单日记录缓存（优化1）：LCDB 为只读，按日期缓存永久有效，无需失效。
 * 月历渲染循环中同一天会被 LUNAR_SolarToLunar/GetFestival/调休连续查询，
 * 命中缓存后每日读盘由 3 次降为 1 次。 */
static bool          s_rec_cache_valid = false;
static uint16_t      s_rec_cache_year = 0;
static uint8_t       s_rec_cache_month = 0;
static uint8_t       s_rec_cache_day = 0;
static daily_record_t s_rec_cache_value;

/* 计算公历日期在当年的第几天 (0-based)，用于定位每日记录。 */
static uint16_t lunar_db_day_of_year(uint16_t year, uint8_t month, uint8_t day) {
    static const uint16_t cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    uint16_t doy = cum[(month - 1U) % 12U] + (uint16_t)(day - 1U);
    if (month > 2U && is_leap((int)year)) {
        doy += 1U;
    }
    return doy;
}

/* 将请求年份平稳降级到数据库覆盖范围内，返回有效年份。 */
static uint16_t lunar_db_effective_year(uint16_t year) {
    if (year < s_db_start_year || year >= (uint16_t)(s_db_start_year + s_db_year_count)) {
        return s_db_start_year;
    }
    return year;
}

void lunar_db_reset(void) {
    /* 资源热更新后调用：清除初始化缓存与单日缓存，下次查询自动重新读取新数据库，
     * 避免"更新资源后必须重启设备才生效"（见 7.10）。 */
    s_db_inited = false;
    s_db_valid = false;
    s_rec_cache_valid = false;
}

int lunar_db_init(void) {
    uint8_t header[16];
    uint32_t magic;

    if (s_db_inited) {
        return s_db_valid ? 0 : -1;
    }
    s_db_inited = true;
    s_db_valid = false;

    if (res_hal_read(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, 0, header, sizeof(header)) !=
        NRF_SUCCESS) {
        return -2;
    }

    magic = (uint32_t)header[0] | ((uint32_t)header[1] << 8) | ((uint32_t)header[2] << 16) |
            ((uint32_t)header[3] << 24);
    if (magic != EPD_EXT_LUNAR_DB_MAGIC) {
        return -3;
    }

    s_db_year_count = header[5];
    s_db_start_year = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
    s_db_record_size = header[8];
    if (s_db_record_size == 0) {
        s_db_record_size = 4;
    }
    if (s_db_year_count == 0) {
        return -4;
    }
    if (s_db_year_count > LUNAR_DB_MAX_YEARS) {
        s_db_year_count = LUNAR_DB_MAX_YEARS;
    }

    /* 年份偏移表紧随 16B 主头部，按小端 uint32 存储，与 MCU 字节序一致，直接读入。 */
    if (res_hal_read(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, 16, s_db_year_offsets,
                              (size_t)s_db_year_count * 4U) != NRF_SUCCESS) {
        return -5;
    }

    s_db_valid = true;
    return 0;
}

/* 读取指定(降级后)年份块的 16B 年份头部，输出块基址/总天数/稀疏表数量/稀疏表偏移/字符串池偏移。 */
static int lunar_db_read_year_header(uint16_t eff_year, uint32_t* block_offset, uint16_t* total_days,
                                     uint16_t* sparse_count, uint32_t* sparse_table_offset,
                                     uint32_t* string_pool_offset) {
    uint8_t yh[16];
    uint8_t idx = (uint8_t)(eff_year - s_db_start_year);
    uint32_t base = s_db_year_offsets[idx];

    if (res_hal_read(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, base, yh, 16) !=
        NRF_SUCCESS) {
        return -1;
    }
    if (block_offset) *block_offset = base;
    if (total_days) *total_days = (uint16_t)yh[2] | ((uint16_t)yh[3] << 8);
    if (sparse_count) *sparse_count = (uint16_t)yh[4] | ((uint16_t)yh[5] << 8);
    if (sparse_table_offset) {
        *sparse_table_offset = (uint32_t)yh[6] | ((uint32_t)yh[7] << 8) |
                               ((uint32_t)yh[8] << 16) | ((uint32_t)yh[9] << 24);
    }
    if (string_pool_offset) {
        *string_pool_offset = (uint32_t)yh[10] | ((uint32_t)yh[11] << 8) |
                              ((uint32_t)yh[12] << 16) | ((uint32_t)yh[13] << 24);
    }
    return 0;
}

int lunar_db_get_record(uint16_t year, uint8_t month, uint8_t day, daily_record_t* out_record) {
    uint16_t eff_year;
    uint32_t base = 0;
    uint16_t total_days = 0;
    uint16_t sparse_count = 0;
    uint32_t sparse_table_offset = 0;
    uint16_t doy;
    uint32_t addr;
    uint32_t packed_val = 0;
    bool has_festival, has_special;

    if (out_record == NULL || month < 1 || month > 12 || day < 1 || day > 31) {
        return -1;
    }

    if (s_rec_cache_valid && s_rec_cache_year == year && s_rec_cache_month == month &&
        s_rec_cache_day == day) {
        *out_record = s_rec_cache_value;
        return 0;
    }

    if (!s_db_valid && lunar_db_init() != 0) {
        return -2;
    }

    eff_year = lunar_db_effective_year(year);
    if (lunar_db_read_year_header(eff_year, &base, &total_days, &sparse_count, &sparse_table_offset, NULL) != 0 || total_days == 0) {
        return -3;
    }

    doy = lunar_db_day_of_year(eff_year, month, day);
    if (doy >= total_days) {
        doy = (uint16_t)(total_days - 1U);
    }

    addr = base + LUNAR_DB_YEAR_HEADER_SZ + (uint32_t)doy * s_db_record_size;
    if (res_hal_read(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, addr, &packed_val, 4) != NRF_SUCCESS) {
        return -4;
    }

    // Bit shifting to unpack bitfield
    uint8_t l_mon = (uint8_t)(packed_val & 0x0FU);
    uint8_t is_leap_m = (uint8_t)((packed_val >> 4) & 0x01U);
    out_record->lunar_month = l_mon | (is_leap_m ? 0x80U : 0x00U);
    out_record->lunar_day = (uint8_t)((packed_val >> 5) & 0x1FU);
    out_record->jiazi_year = (uint8_t)((packed_val >> 10) & 0x3FU);
    out_record->solar_term = (uint8_t)((packed_val >> 16) & 0x1FU);
    out_record->holiday_type = (uint8_t)((packed_val >> 21) & 0x03U);
    out_record->reserved = 0;
    out_record->festival_offset = 0xFFFFU;
    out_record->special_label = 0xFFFFU;

    has_festival = (bool)((packed_val >> 23) & 0x01U);
    has_special = (bool)((packed_val >> 24) & 0x01U);

    if ((has_festival || has_special) && sparse_count > 0) {
        int low = 0;
        int high = (int)sparse_count - 1;
        while (low <= high) {
            int mid = low + (high - low) / 2;
            uint8_t entry_buf[6];
            uint32_t entry_addr = base + sparse_table_offset + (uint32_t)mid * 6U;
            if (res_hal_read(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, entry_addr, entry_buf, 6) == NRF_SUCCESS) {
                uint16_t entry_doy = (uint16_t)entry_buf[0] | ((uint16_t)entry_buf[1] << 8);
                if (entry_doy == doy) {
                    out_record->festival_offset = (uint16_t)entry_buf[2] | ((uint16_t)entry_buf[3] << 8);
                    out_record->special_label   = (uint16_t)entry_buf[4] | ((uint16_t)entry_buf[5] << 8);
                    break;
                } else if (entry_doy < doy) {
                    low = mid + 1;
                } else {
                    high = mid - 1;
                }
            } else {
                break;
            }
        }
    }

    s_rec_cache_year = year;
    s_rec_cache_month = month;
    s_rec_cache_day = day;
    s_rec_cache_value = *out_record;
    s_rec_cache_valid = true;
    return 0;
}

/*
 * 诚实降级支持：判断某公历年份是否在 LCDB 覆盖范围内（含数据库有效性）。
 * 返回 false 表示当前显示的是被替代/过期的农历数据，UI 应给出更新提示，
 * 而非静默呈现"看似正确、实则错误"的内容。
 */
bool lunar_db_year_supported(uint16_t year) {
    if (!s_db_valid && lunar_db_init() != 0) {
        return false;
    }
    return (year >= s_db_start_year) && (year < (uint16_t)(s_db_start_year + s_db_year_count));
}

const char* lunar_db_get_string(uint16_t year, uint16_t offset, char* buffer, size_t buffer_len) {
    uint16_t eff_year;
    uint32_t base = 0;
    uint32_t pool_offset = 0;
    uint32_t addr;
    size_t i = 0;

    if (buffer == NULL || buffer_len == 0) {
        return "";
    }
    buffer[0] = '\0';
    if (offset == 0xFFFF) {
        return buffer;
    }
    if (!s_db_valid && lunar_db_init() != 0) {
        return buffer;
    }

    eff_year = lunar_db_effective_year(year);
    if (lunar_db_read_year_header(eff_year, &base, NULL, NULL, NULL, &pool_offset) != 0) {
        return buffer;
    }

    /* 零拷贝流式读取：每次读 1 字节直至 NUL，RAM 开销 O(1)（见文档优化 6）。 */
    addr = base + pool_offset + offset;
    while (i < buffer_len - 1U) {
        uint8_t ch;
        if (res_hal_read(EPD_EXT_RES_TYPE_LUNAR_DB, EPD_EXT_LUNAR_DB_ENTRY, addr + i, &ch, 1) !=
            NRF_SUCCESS) {
            break;
        }
        if (ch == 0) {
            break;
        }
        buffer[i++] = (char)ch;
    }
    buffer[i] = '\0';
    return buffer;
}

static const char* lunar_copy_fallback(const char* fallback, char* buffer, size_t buffer_len) {
    if (buffer != NULL && buffer_len > 0) {
        strncpy(buffer, fallback, buffer_len - 1);
        buffer[buffer_len - 1] = '\0';
        return buffer;
    }
    return fallback;
}

static const char* lunar_read_name(uint16_t id, uint8_t index, uint8_t count, uint8_t item_size,
                                   const char* fallback, char* buffer, size_t buffer_len) {
    ret_code_t ret;
    size_t read_len;

    if (buffer == NULL || buffer_len == 0 || index >= count) {
        return fallback;
    }

    read_len = (buffer_len - 1 < item_size) ? buffer_len - 1 : item_size;
    buffer[0] = '\0';
    ret = res_hal_read(EPD_EXT_RES_TYPE_LUNAR, id, (uint32_t)index * item_size, buffer, read_len);
    if (ret != NRF_SUCCESS) {
        return lunar_copy_fallback(fallback, buffer, buffer_len);
    }
    buffer[buffer_len - 1] = '\0';
    return buffer;
}

const char* LUNAR_GetMonthName(uint8_t index, char* buffer, size_t buffer_len) {
    return lunar_read_name(EPD_EXT_LUNAR_MONTH_NAMES, index, 13, LUNAR_NAME_MONTH_SIZE, "--", buffer, buffer_len);
}

const char* LUNAR_GetLeapName(uint8_t index, char* buffer, size_t buffer_len) {
    return lunar_read_name(EPD_EXT_LUNAR_LEAP_NAMES, index, 2, LUNAR_NAME_LEAP_SIZE, index ? "" : " ", buffer, buffer_len);
}

const char* LUNAR_GetDateName(uint8_t index, char* buffer, size_t buffer_len) {
    return lunar_read_name(EPD_EXT_LUNAR_DATE_NAMES, index, 31, LUNAR_NAME_DATE_SIZE, "--", buffer, buffer_len);
}

const char* LUNAR_GetDayName(uint8_t index, char* buffer, size_t buffer_len) {
    return lunar_read_name(EPD_EXT_LUNAR_DAY_NAMES, index, 7, LUNAR_NAME_DAY_SIZE, "--", buffer, buffer_len);
}

const char* LUNAR_GetZodiacName(uint8_t index, char* buffer, size_t buffer_len) {
    return lunar_read_name(EPD_EXT_LUNAR_ZODIAC_NAMES, index, 12, LUNAR_NAME_ZODIAC_SIZE, "--", buffer, buffer_len);
}

const char* LUNAR_GetStemName(uint8_t index, char* buffer, size_t buffer_len) {
    return lunar_read_name(EPD_EXT_LUNAR_STEM_NAMES, index, 10, LUNAR_NAME_STEM_SIZE, "--", buffer, buffer_len);
}

const char* LUNAR_GetBranchName(uint8_t index, char* buffer, size_t buffer_len) {
    return lunar_read_name(EPD_EXT_LUNAR_BRANCH_NAMES, index, 12, LUNAR_NAME_BRANCH_SIZE, "--", buffer, buffer_len);
}

const char* LUNAR_GetJieQiName(uint8_t index, char* buffer, size_t buffer_len) {
    return lunar_read_name(EPD_EXT_LUNAR_JIEQI_NAMES, index, 24, LUNAR_NAME_JIEQI_SIZE, "--", buffer, buffer_len);
}

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

/**
 * 闰月标识 - Leap Month Indicator
 * 索引0: 空格（非闰月）, 索引1: "闰"（闰月）
 */

/**
 * 农历日期名称表 - Lunar Day Names
 * 索引0为占位符，索引1-30对应初一到三十
 * 农历日期命名规则:
 *   1-10日: 初一、初二、...、初十
 *   11-20日: 十一、十二、...、二十
 *   21-29日: 廿一、廿二、...、廿九 (廿=二十)
 *   30日: 三十
 */

/**
 * 星期名称表 - Day of Week Names
 * 索引0-6对应星期日到星期六
 */

/**
 * 十二生肖表 - Chinese Zodiac Animals (12-year cycle)
 * 排列顺序从猴开始，是因为计算公式 year % 12 的结果对应关系:
 *   0:猴, 1:鸡, 2:狗, 3:猪, 4:鼠, 5:牛, 6:虎, 7:兔, 8:龙, 9:蛇, 10:马, 11:羊
 * 例如: 2024年(甲辰龙年) % 12 = 8，对应数组索引8即"龙"
 */

/**
 * 十天干表 - Ten Heavenly Stems (天干)
 * 天干是中国古代的计数符号，与地支配合使用
 * 排列顺序从庚开始，是因为计算公式 year % 10 的结果对应关系:
 *   0:庚, 1:辛, 2:壬, 3:癸, 4:甲, 5:乙, 6:丙, 7:丁, 8:戊, 9:己
 * 例如: 2024年 % 10 = 4，对应数组索引4即"甲"
 */

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
/* 农历/节气换算已下放至打包端并固化为 LCDB 数据库，MCU 端不再做位域解码。 */

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
/*
 * 数据库模式下的公历转农历兼容接口：直接查询 LCDB 单日记录填充结构体。
 * 注意 lunar->Year 在此模式下临时充当六十甲子索引 (0-59)，供
 * LUNAR_GetStem/Branch/Zodiac 按 %10 / %12 取名（语义与旧实现一致）。
 */
void LUNAR_SolarToLunar(struct Lunar_Date* lunar, uint16_t solar_year, uint8_t solar_month, uint8_t solar_date) {
    daily_record_t rec;

    if (lunar == NULL) {
        return;
    }
    if (solar_month < 1 || solar_month > 12 || solar_date < 1 || solar_date > 31 ||
        lunar_db_get_record(solar_year, solar_month, solar_date, &rec) != 0) {
        lunar->Year = 0;
        lunar->Month = 0;
        lunar->Date = 0;
        lunar->IsLeap = 0;
        return;
    }

    lunar->IsLeap = (uint8_t)(rec.lunar_month >> 7);
    lunar->Month = (uint8_t)(rec.lunar_month & 0x7F);
    lunar->Date = rec.lunar_day;
    lunar->Year = rec.jiazi_year;
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
    uint8_t start, end, dd;

    if (mmonth == 0 || mmonth > 12 || JQdate == NULL) {
        return 0;
    }

    /* 每半月恰有一个节气：上半月扫描 1~14，下半月扫描 15~月末，命中即返回其公历日。 */
    if (mday >= 15) {
        start = 15;
        end = get_last_day(myear, (uint8_t)(mmonth - 1));
    } else {
        start = 1;
        end = 14;
    }

    for (dd = start; dd <= end; dd++) {
        daily_record_t rec;
        if (lunar_db_get_record(myear, mmonth, dd, &rec) == 0 && rec.solar_term != 0) {
            *JQdate = dd;
            return 1;
        }
    }
    return 0;
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
    uint16_t yy = myear;
    uint8_t mm = mmonth, dd = mday, k;

    /* 从当天起向后逐日扫描（跨月/跨年），首个带节气标志的记录即为下一个节气。
     * 节气间隔约 15 天，32 天窗口足以覆盖，返回其名称索引 (0-23) 与距离天数。 */
    for (k = 0; k < 32; k++) {
        daily_record_t rec;
        if (lunar_db_get_record(yy, mm, dd, &rec) == 0 && rec.solar_term != 0) {
            if (day != NULL) *day = k;
            return (uint8_t)(rec.solar_term - 1);
        }
        dd++;
        if (dd > get_last_day(yy, (uint8_t)(mm - 1))) {
            dd = 1;
            if (++mm > 12) {
                mm = 1;
                yy++;
            }
        }
    }
    if (day != NULL) *day = 0;
    return 0xFF;
}
#else

void lunar_db_reset(void) {}
int lunar_db_init(void) { return -1; }
int lunar_db_get_record(uint16_t year, uint8_t month, uint8_t day, daily_record_t* out_record) {
    (void)year; (void)month; (void)day;
    if (out_record) memset(out_record, 0, sizeof(daily_record_t));
    return -1;
}
const char* lunar_db_get_string(uint16_t year, uint16_t offset, char* buffer, size_t buffer_len) {
    (void)year; (void)offset;
    if (buffer && buffer_len > 0) buffer[0] = '\0';
    return "";
}
bool lunar_db_year_supported(uint16_t year) {
    (void)year;
    return false;
}

const char* LUNAR_GetMonthName(uint8_t index, char* buffer, size_t buffer_len) {
    (void)index;
    if (buffer && buffer_len > 0) buffer[0] = '\0';
    return "";
}
const char* LUNAR_GetLeapName(uint8_t index, char* buffer, size_t buffer_len) {
    (void)index;
    if (buffer && buffer_len > 0) buffer[0] = '\0';
    return "";
}
const char* LUNAR_GetDateName(uint8_t index, char* buffer, size_t buffer_len) {
    (void)index;
    if (buffer && buffer_len > 0) buffer[0] = '\0';
    return "";
}
const char* LUNAR_GetDayName(uint8_t index, char* buffer, size_t buffer_len) {
    static const char* const days[] = {"日", "一", "二", "三", "四", "五", "六"};
    if (buffer && buffer_len > 0) {
        if (index < 7) {
            strncpy(buffer, days[index], buffer_len - 1);
            buffer[buffer_len - 1] = '\0';
        } else {
            buffer[0] = '\0';
        }
        return buffer;
    }
    return (index < 7) ? days[index] : "";
}
const char* LUNAR_GetZodiacName(uint8_t index, char* buffer, size_t buffer_len) {
    (void)index;
    if (buffer && buffer_len > 0) buffer[0] = '\0';
    return "";
}
const char* LUNAR_GetStemName(uint8_t index, char* buffer, size_t buffer_len) {
    (void)index;
    if (buffer && buffer_len > 0) buffer[0] = '\0';
    return "";
}
const char* LUNAR_GetBranchName(uint8_t index, char* buffer, size_t buffer_len) {
    (void)index;
    if (buffer && buffer_len > 0) buffer[0] = '\0';
    return "";
}
const char* LUNAR_GetJieQiName(uint8_t index, char* buffer, size_t buffer_len) {
    (void)index;
    if (buffer && buffer_len > 0) buffer[0] = '\0';
    return "";
}

void LUNAR_SolarToLunar(struct Lunar_Date* lunar, uint16_t solar_year, uint8_t solar_month, uint8_t solar_date) {
    (void)solar_year; (void)solar_month; (void)solar_date;
    if (lunar) {
        lunar->Year = 0;
        lunar->Month = 0;
        lunar->Date = 0;
        lunar->IsLeap = 0;
    }
}
uint8_t LUNAR_GetZodiac(const struct Lunar_Date* lunar) { (void)lunar; return 0; }
uint8_t LUNAR_GetStem(const struct Lunar_Date* lunar) { (void)lunar; return 0; }
uint8_t LUNAR_GetBranch(const struct Lunar_Date* lunar) { (void)lunar; return 0; }
uint8_t GetJieQiStr(uint16_t myear, uint8_t mmonth, uint8_t mday, uint8_t* day) {
    (void)myear; (void)mmonth; (void)mday;
    if (day) *day = 0;
    return 0xFF;
}
uint8_t GetJieQi(uint16_t myear, uint8_t mmonth, uint8_t mday, uint8_t* JQdate) {
    (void)myear; (void)mmonth; (void)mday;
    if (JQdate) *JQdate = 0;
    return 0;
}

#endif

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
 * 每月最大天数表 - Maximum Days per Month
 * 用于日期计算（2月按平年28天计算，闰年需特殊处理）
 */
const uint8_t MonthDayMax[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
};

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
