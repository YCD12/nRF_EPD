#include "EPD_driver.h"
#include "nrf_log.h"

// =============================================================================
// 温度分段参数定义 - 每3度一档,共11档
// Temperature tier parameters - 11 tiers with 3°C intervals
// =============================================================================

// T3 and T4 must always be 0 for partial refresh!
// T3和T4在局部刷新时必须始终为0!
#define T3_PARTIAL  0  // MUST BE 0 for partial refresh // 局刷时必须为0
#define T4_PARTIAL  0  // MUST BE 0 for partial refresh // 局刷时必须为0

// LUT参数表 - 基于线性插值,从极冷到很热
// 温度越低,T1/T2越大(驱动时间越长); 温度越高,T1/T2越小(驱动时间越短)
// T1: 主驱动阶段  T2: 扩展稳定阶段

// Tier 0: < 7°C (极冷/Very Cold)
#define T1_TIER_0  56
#define T2_TIER_0  22

// Tier 1: 7-9°C (很冷/Cold)
#define T1_TIER_1  52
#define T2_TIER_1  20

// Tier 2: 10-12°C (冷/Chilly)
#define T1_TIER_2  48
#define T2_TIER_2  18

// Tier 3: 13-15°C (偏冷/Cool)
#define T1_TIER_3  44
#define T2_TIER_3  16

// Tier 4: 16-18°C (凉爽/Mild Cool)
#define T1_TIER_4  48   // 增加驱动时间(原40)
#define T2_TIER_4  18   // 增加驱动时间(原14)

// Tier 5: 19-21°C (微凉/Mild - 关键区间,解决20°C残影问题)
// 大幅增加驱动时间以解决黑→白转换不完全的问题
#define T1_TIER_5  50   // 大幅增加(原36),确保黑色完全擦除
#define T2_TIER_5  18   // 大幅增加(原12),延长稳定时间

// Tier 6: 22-24°C (适中/Moderate)
#define T1_TIER_6  42   // 增加驱动时间(原32)
#define T2_TIER_6  14   // 增加驱动时间(原10)

// Tier 7: 25-27°C (偏暖/Warm)
#define T1_TIER_7  36   // 增加驱动时间(原28)
#define T2_TIER_7  12   // 增加驱动时间(原8)

// Tier 8: 28-30°C (暖/Hot)
#define T1_TIER_8  30   // 增加驱动时间(原24)
#define T2_TIER_8  10   // 增加驱动时间(原7)

// Tier 9: 31-33°C (热/Very Hot)
#define T1_TIER_9  20
#define T2_TIER_9  6

// Tier 10: >= 34°C (很热/Extreme Hot)
#define T1_TIER_10  16
#define T2_TIER_10  5

// Temperature range enumeration - 11 tiers
// 温度范围枚举 - 11档
typedef enum {
    TEMP_TIER_0,   // < 7°C   极冷
    TEMP_TIER_1,   // 7-9°C   很冷
    TEMP_TIER_2,   // 10-12°C 冷
    TEMP_TIER_3,   // 13-15°C 偏冷
    TEMP_TIER_4,   // 16-18°C 凉爽
    TEMP_TIER_5,   // 19-21°C 微凉 (20°C关键区间)
    TEMP_TIER_6,   // 22-24°C 适中
    TEMP_TIER_7,   // 25-27°C 偏暖
    TEMP_TIER_8,   // 28-30°C 暖
    TEMP_TIER_9,   // 31-33°C 热
    TEMP_TIER_10   // >= 34°C 很热
} temp_range_t;

// LUT tables must be 42 bytes (6 data + 36 padding zeros)
// LUT表必须为42字节(6字节数据 + 36字节填充零)
// Each temperature range has its own set of LUT tables
// 每个温度范围都有自己的一组LUT表

/*
 * =============================================================================
 * LUT (Look-Up Table / 查找表) 详细说明
 * =============================================================================
 *
 * LUT是电子纸刷新的核心,它定义了像素如何从一种颜色过渡到另一种颜色。
 *
 * LUT表格式说明:
 * - 每个LUT表有42字节
 * - 前6字节是有效数据: [波形类型, T1, T2, T3, T4, 重复次数]
 * - 后36字节为填充零
 *
 * 波形类型 (第一个字节) 说明:
 * - 0x00: 保持不变(不刷新)
 * - 0x20: 白→白 (LUTWW)
 * - 0x58: 黑→白 (LUTKW/LUTBW) - 先拉低电压再推高
 * - 0xA8: 白→黑 (LUTWK/LUTWB) - 先拉高电压再推低
 *
 * 时序参数说明:
 * - T1: 第一阶段驱动时间(主要电荷平衡阶段)
 * - T2: 第二阶段驱动时间(扩展稳定阶段)
 * - T3: 第三阶段驱动时间(局刷时必须为0!)
 * - T4: 第四阶段驱动时间(局刷时必须为0!)
 *
 * 各LUT表用途:
 * - LUTC:  通用/清除LUT(不刷新)
 * - LUTWW: 白色→白色 过渡(保持白色)
 * - LUTKW/LUTBW: 黑色→白色 过渡(黑变白)
 * - LUTWK/LUTWB: 白色→黑色 过渡(白变黑)
 * - LUTKK/LUTBB: 黑色→黑色 过渡(保持黑色)
 * - LUTBD: 边框LUT
 *
 * 温度对墨水的影响:
 * - 低温: 墨水粘度高,响应慢,需要更长驱动时间
 * - 高温: 墨水粘度低,响应快,可以使用较短驱动时间
 * =============================================================================
 */

// =============================================================================
// 宏定义: 生成LUT表数组 (避免重复代码)
// Macro: Generate LUT table array (avoid repetitive code)
// =============================================================================
#define DEFINE_LUT_SET(tier) \
static const uint8_t lut_LUTC_tier##tier[42] = { \
    0x00, T1_TIER_##tier, T2_TIER_##tier, T3_PARTIAL, T4_PARTIAL, 1, \
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 \
}; \
static const uint8_t lut_LUTWW_tier##tier[42] = { \
    0x20, T1_TIER_##tier, T2_TIER_##tier, T3_PARTIAL, T4_PARTIAL, 1, \
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 \
}; \
static const uint8_t lut_LUTKW_tier##tier[42] = { \
    0x58, T1_TIER_##tier, T2_TIER_##tier, T3_PARTIAL, T4_PARTIAL, 1, \
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 \
}; \
static const uint8_t lut_LUTWK_tier##tier[42] = { \
    0xA8, T1_TIER_##tier, T2_TIER_##tier, T3_PARTIAL, T4_PARTIAL, 1, \
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 \
}; \
static const uint8_t lut_LUTKK_tier##tier[42] = { \
    0x00, T1_TIER_##tier, T2_TIER_##tier, T3_PARTIAL, T4_PARTIAL, 1, \
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 \
}; \
static const uint8_t lut_LUTBD_tier##tier[42] = { \
    0x00, T1_TIER_##tier, T2_TIER_##tier, T3_PARTIAL, T4_PARTIAL, 1, \
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 \
};

// 生成11组LUT表 (Tier 0-10)
// Generate 11 LUT table sets (Tier 0-10)
DEFINE_LUT_SET(0)   // < 7°C   极冷
DEFINE_LUT_SET(1)   // 7-9°C   很冷
DEFINE_LUT_SET(2)   // 10-12°C 冷
DEFINE_LUT_SET(3)   // 13-15°C 偏冷
DEFINE_LUT_SET(4)   // 16-18°C 凉爽
DEFINE_LUT_SET(5)   // 19-21°C 微凉 (20°C关键区间)
DEFINE_LUT_SET(6)   // 22-24°C 适中
DEFINE_LUT_SET(7)   // 25-27°C 偏暖
DEFINE_LUT_SET(8)   // 28-30°C 暖
DEFINE_LUT_SET(9)   // 31-33°C 热
DEFINE_LUT_SET(10)  // >= 34°C 很热

// LUT table set structure for easy selection
// LUT表集合结构体,方便根据温度选择合适的LUT
typedef struct {
    const uint8_t *lutc;   // 通用LUT指针
    const uint8_t *lutww;  // 白→白 LUT指针
    const uint8_t *lutkw;  // 黑→白 LUT指针
    const uint8_t *lutwk;  // 白→黑 LUT指针
    const uint8_t *lutkk;  // 黑→黑 LUT指针
    const uint8_t *lutbd;  // 边框LUT指针
} lut_set_t;

// 宏定义: 生成LUT集合结构体
#define DEFINE_LUT_STRUCT(tier) \
static const lut_set_t lut_tier##tier = { \
    .lutc  = lut_LUTC_tier##tier, \
    .lutww = lut_LUTWW_tier##tier, \
    .lutkw = lut_LUTKW_tier##tier, \
    .lutwk = lut_LUTWK_tier##tier, \
    .lutkk = lut_LUTKK_tier##tier, \
    .lutbd = lut_LUTBD_tier##tier, \
};

// 生成11组LUT集合结构体
DEFINE_LUT_STRUCT(0)
DEFINE_LUT_STRUCT(1)
DEFINE_LUT_STRUCT(2)
DEFINE_LUT_STRUCT(3)
DEFINE_LUT_STRUCT(4)
DEFINE_LUT_STRUCT(5)
DEFINE_LUT_STRUCT(6)
DEFINE_LUT_STRUCT(7)
DEFINE_LUT_STRUCT(8)
DEFINE_LUT_STRUCT(9)
DEFINE_LUT_STRUCT(10)

/*
 * =============================================================================
 * 基础驱动函数
 * =============================================================================
 */

/**
 * @brief 等待UC8179忙信号结束
 * @param timeout 超时时间(毫秒)
 *
 * 功能说明:
 * UC8179芯片在执行命令时会将BUSY引脚拉高,表示芯片正忙。
 * 此函数等待BUSY信号变为低电平,表示芯片已准备好接收新命令。
 */
static void UC8179_WaitBusy(uint16_t timeout) { EPD_WaitBusy(LOW, timeout); }

/**
 * @brief UC8179电源开启
 *
 * 功能说明:
 * 发送PON(Power ON)命令启动电子纸的电源系统。
 * 这会启动内部的升压电路,产生驱动电子纸所需的高电压。
 */
static void UC8179_PowerOn(void) {
    EPD_WriteCmd(UC81xx_PON);  // 发送电源开启命令
    UC8179_WaitBusy(200);      // 等待电源稳定
}

/**
 * @brief UC8179电源关闭
 *
 * 功能说明:
 * 发送POF(Power OFF)命令关闭电子纸的电源系统。
 * 关闭电源可以大幅降低功耗,电子纸图像会保持显示。
 */
static void UC8179_PowerOff(void) {
    EPD_WriteCmd(UC81xx_POF);  // 发送电源关闭命令
    UC8179_WaitBusy(200);      // 等待电源完全关闭
}

/**
 * @brief 从UC8179读取温度
 * @param epd EPD设备模型指针
 * @return 返回温度值(摄氏度,有符号8位整数)
 *
 * 功能说明:
 * UC8179内置温度传感器,可用于根据环境温度调整刷新参数。
 * 温度会影响电子墨水的响应速度,低温时墨水更粘稠需要更长驱动时间。
 */
// Read temperature from driver chip
// 从驱动芯片读取温度
int8_t UC8179_Read_Temp(epd_model_t* epd) {
    EPD_WriteCmd(UC81xx_TSC);  // 发送温度传感器控制命令
    UC8179_WaitBusy(100);      // 等待温度转换完成
    return (int8_t)EPD_ReadByte();  // 读取温度值
}

/**
 * @brief 根据温度确定温度范围
 * @param temperature 当前温度(摄氏度)
 * @return 返回温度范围枚举值
 *
 * 功能说明:
 * 将连续的温度值映射到11个离散的温度范围,每3度一档,以便精确选择LUT参数。
 */
// Determine temperature range based on current temperature
// 根据当前温度确定温度范围 (11档,每3度一档)
static temp_range_t UC8179_Get_Temp_Range(int8_t temperature) {
    if (temperature < 7) {
        return TEMP_TIER_0;   // < 7°C   极冷
    } else if (temperature < 10) {
        return TEMP_TIER_1;   // 7-9°C   很冷
    } else if (temperature < 13) {
        return TEMP_TIER_2;   // 10-12°C 冷
    } else if (temperature < 16) {
        return TEMP_TIER_3;   // 13-15°C 偏冷
    } else if (temperature < 19) {
        return TEMP_TIER_4;   // 16-18°C 凉爽
    } else if (temperature < 22) {
        return TEMP_TIER_5;   // 19-21°C 微凉 (20°C关键区间)
    } else if (temperature < 25) {
        return TEMP_TIER_6;   // 22-24°C 适中
    } else if (temperature < 28) {
        return TEMP_TIER_7;   // 25-27°C 偏暖
    } else if (temperature < 31) {
        return TEMP_TIER_8;   // 28-30°C 暖
    } else if (temperature < 34) {
        return TEMP_TIER_9;   // 31-33°C 热
    } else {
        return TEMP_TIER_10;  // >= 34°C 很热
    }
}

/**
 * @brief 根据温度选择合适的LUT集合
 * @param temperature 当前温度(摄氏度)
 * @return 返回对应温度范围的LUT集合指针
 *
 * 功能说明:
 * 根据环境温度自动选择最优的LUT参数集(11档),确保不同温度下都有良好的刷新效果。
 * - 低温时选择长脉冲LUT,补偿墨水响应慢的问题
 * - 高温时选择短脉冲LUT,加快刷新速度
 */
// Select appropriate LUT set based on temperature (11 tiers)
// 根据温度选择合适的LUT集合 (11档)
static const lut_set_t* UC8179_Select_LUT(int8_t temperature) {
    temp_range_t range = UC8179_Get_Temp_Range(temperature);

    switch (range) {
        case TEMP_TIER_0:
            NRF_LOG_INFO("[EPD]: Using Tier0 LUT (<7°C, T1=56, T2=22) temp=%d°C\n", temperature);
            return &lut_tier0;
        case TEMP_TIER_1:
            NRF_LOG_INFO("[EPD]: Using Tier1 LUT (7-9°C, T1=52, T2=20) temp=%d°C\n", temperature);
            return &lut_tier1;
        case TEMP_TIER_2:
            NRF_LOG_INFO("[EPD]: Using Tier2 LUT (10-12°C, T1=48, T2=18) temp=%d°C\n", temperature);
            return &lut_tier2;
        case TEMP_TIER_3:
            NRF_LOG_INFO("[EPD]: Using Tier3 LUT (13-15°C, T1=44, T2=16) temp=%d°C\n", temperature);
            return &lut_tier3;
        case TEMP_TIER_4:
            NRF_LOG_INFO("[EPD]: Using Tier4 LUT (16-18°C, T1=40, T2=14) temp=%d°C\n", temperature);
            return &lut_tier4;
        case TEMP_TIER_5:
            NRF_LOG_INFO("[EPD]: Using Tier5 LUT (19-21°C, T1=36, T2=12) temp=%d°C\n", temperature);
            return &lut_tier5;
        case TEMP_TIER_6:
            NRF_LOG_INFO("[EPD]: Using Tier6 LUT (22-24°C, T1=32, T2=10) temp=%d°C\n", temperature);
            return &lut_tier6;
        case TEMP_TIER_7:
            NRF_LOG_INFO("[EPD]: Using Tier7 LUT (25-27°C, T1=28, T2=8) temp=%d°C\n", temperature);
            return &lut_tier7;
        case TEMP_TIER_8:
            NRF_LOG_INFO("[EPD]: Using Tier8 LUT (28-30°C, T1=24, T2=7) temp=%d°C\n", temperature);
            return &lut_tier8;
        case TEMP_TIER_9:
            NRF_LOG_INFO("[EPD]: Using Tier9 LUT (31-33°C, T1=20, T2=6) temp=%d°C\n", temperature);
            return &lut_tier9;
        case TEMP_TIER_10:
        default:
            NRF_LOG_INFO("[EPD]: Using Tier10 LUT (>=34°C, T1=16, T2=5) temp=%d°C\n", temperature);
            return &lut_tier10;
    }
}

/**
 * @brief 设置局部RAM区域
 * @param epd EPD设备模型指针
 * @param x 起始X坐标
 * @param y 起始Y坐标
 * @param w 宽度
 * @param h 高度
 *
 * 功能说明:
 * 配置UC8179的局部刷新窗口。这个函数设置PTL(Partial Window)寄存器,
 * 告诉芯片只处理指定矩形区域内的像素数据。
 *
 * 注意:X坐标和宽度会被对齐到8像素边界(字节边界),
 * 因为每个字节包含8个像素。
 */
static void _setPartialRamArea(epd_model_t* epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
        uint16_t xe = (x + w - 1) | 0x0007;  // byte boundary inclusive (last byte) // 字节边界对齐(包含最后一个字节)
        uint16_t ye = y + h - 1;             // 结束Y坐标
        x &= 0xFFF8;                         // byte boundary // X起始地址对齐到8像素边界
        EPD_Write(UC81xx_PTL,                // partial window // 发送局部窗口命令
                  x / 256, x % 256,          // X起始地址(高字节,低字节)
                  xe / 256, xe % 256,        // X结束地址(高字节,低字节)
                  y / 256, y % 256,          // Y起始地址(高字节,低字节)
                  ye / 256, ye % 256,        // Y结束地址(高字节,低字节)
                  0x00);                     // 扫描模式: 正常
}

/*
 * =============================================================================
 * 全刷(Full Refresh)相关函数
 * =============================================================================
 *
 * 全刷说明:
 * - 全刷会刷新整个屏幕的所有像素
 * - 刷新过程中会有明显的黑白闪烁(用于消除残影)
 * - 刷新时间较长(通常几秒钟)
 * - 适用于显示全新内容或需要清除残影时
 * - 使用芯片OTP(一次性可编程存储器)中的默认LUT
 */

/**
 * @brief UC8179全屏刷新
 * @param epd EPD设备模型指针
 *
 * 功能说明:
 * 执行全屏刷新,将RAM中的图像数据显示到电子纸上。
 * 全刷会使用OTP中预设的LUT,产生黑白闪烁以获得最佳对比度。
 */
void UC8179_Refresh(epd_model_t* epd) {
    NRF_LOG_DEBUG("[EPD]: refresh begin\n");
    UC8179_PowerOn();  // 开启电源

    _setPartialRamArea(epd, 0, 0, epd->width, epd->height);  // 设置全屏区域

    EPD_WriteCmd(UC81xx_DRF);  // 发送显示刷新命令(Display Refresh)
    delay(100);                // 等待命令被接受
    UC8179_WaitBusy(30000);    // 等待刷新完成(最长30秒)

    UC8179_PowerOff();  // 关闭电源以节省功耗
    NRF_LOG_DEBUG("[EPD]: refresh end\n");
}

/**
 * @brief 导出UC8179的OTP数据(调试用)
 *
 * 功能说明:
 * 读取并输出UC8179内部OTP存储器的内容。
 * OTP中存储了芯片的默认配置和LUT数据。
 * 此函数主要用于调试和分析芯片配置。
 */
void UC8179_Dump_OTP(void) {
    uint8_t data[128];

    UC8179_PowerOn();  // OTP读取需要电源开启
    EPD_Write(UC81xx_ROTP, 0x00);  // 发送读取OTP命令

    NRF_LOG_DEBUG("=== OTP BEGIN ===\n");
    for (int i = 0; i < 0xFFF; i += sizeof(data)) {
        EPD_ReadData(data, sizeof(data));  // 批量读取OTP数据
        NRF_LOG_HEXDUMP_DEBUG(data, sizeof(data));  // 十六进制输出
    }
    NRF_LOG_DEBUG("=== OTP END ===\n");

    UC8179_PowerOff();
}

/**
 * @brief UC8179全刷初始化
 * @param epd EPD设备模型指针
 *
 * 功能说明:
 * 执行UC8179的基本初始化,为全刷模式做准备。
 *
 * 初始化序列说明:
 * 1. 硬件复位 - 将芯片恢复到初始状态
 * 2. PSR(面板设置) - 配置显示模式和数据格式
 * 3. CDI(VCOM和数据间隔) - 配置显示时序和电压参数
 *
 * PSR寄存器值说明:
 * - 0x0F: BWR模式(黑白红三色),从OTP加载LUT
 * - 0x1F: BW模式(黑白双色),从OTP加载LUT
 *
 * CDI寄存器值说明:
 * - 0x77: BWR模式的VCOM和数据间隔设置
 * - 0x97: BW模式的VCOM和数据间隔设置
 */
void UC8179_Init(epd_model_t* epd) {
    EPD_Reset(HIGH, 10);  // 硬件复位,等待10ms

    //    UC8179_Dump_OTP();  // 调试用:导出OTP数据

    // PSR: Panel Setting / 面板设置
    // BWR模式使用0x0F, BW模式使用0x1F
    EPD_Write(UC81xx_PSR, epd->color == EPD_COLOR_BWR ? 0x0F : 0x1F);

    // CDI: VCOM and Data Interval Setting / VCOM和数据间隔设置
    // 这个设置影响显示对比度和刷新质量
    EPD_Write(UC81xx_CDI, epd->color == EPD_COLOR_BWR ? 0x77 : 0x97);
}

/*
 * =============================================================================
 * 局刷(Partial Refresh)相关函数
 * =============================================================================
 *
 * 局刷说明:
 * - 局刷只刷新屏幕的指定区域,其他区域保持不变
 * - 刷新过程无明显闪烁,视觉效果更好
 * - 刷新时间短(通常不到1秒)
 * - 适用于频繁更新的小区域(如时间显示、计数器等)
 * - 使用外部自定义LUT(存储在寄存器中)
 * - 注意:局刷只支持黑白显示,不支持红色!
 *
 * 局刷步骤(参考"局刷使用方法.txt"):
 * 步骤1: 加载OTP设置(Init_Partial)
 * 步骤2: 写入白色到两个RAM(Clear_Partial)
 * 步骤3: 刷新显示白屏(Clear_Partial中的refresh)
 * 步骤4: 加载外部LUT(Refresh_Partial)
 * 步骤5: 写入图像数据(Write_Image_Partial)
 * 步骤6: 执行刷新(Refresh_Partial)
 */

/**
 * @brief UC8179局刷模式初始化
 * @param epd EPD设备模型指针
 *
 * 功能说明:
 * 初始化UC8179进入局刷模式。这是局刷流程的第一步。
 *
 * 初始化序列详细说明:
 * 1. EPD_Reset: 硬件复位芯片
 * 2. PWR: 电源设置 - 配置升压电路电压
 *    - VGH=20V (栅极高电压)
 *    - VGL=-20V (栅极低电压)
 *    - VDH=15V (数据高电压)
 *    - VDL=-15V (数据低电压)
 * 3. PSR: 面板设置 - 0x0F表示KW模式,从OTP加载设置
 * 4. TRES: 分辨率设置 - 配置为800x480
 * 5. 0x15: 双SPI模式设置
 * 6. CDI: VCOM和数据间隔设置
 * 7. TCON: 栅极/源极时序控制
 * 8. PowerOn: 启动电源
 */
// Initialize UC8179 for partial refresh mode
// 为局刷模式初始化UC8179
// Step 1: Load OTP settings (_InitDisplay from reference code)
// 步骤1: 加载OTP设置(参考代码中的_InitDisplay)
void UC8179_Init_Partial(epd_model_t *epd)
{
    NRF_LOG_INFO("[EPD]: UC8179 partial init\n");

    // Important: Reset first!
    // 重要: 首先执行硬件复位!
    EPD_Reset(HIGH, 10);

    // PWR: Power Setting / 电源设置
    // 配置升压电路产生驱动电子纸所需的高电压
    // 参数: VGH=20V, VGL=-20V, VDH=15V, VDL=-15V
    EPD_Write(UC81xx_PWR, 0x07, 0x07, 0x3f, 0x3f);  // Power setting: VGH=20V, VGL=-20V, VDH=15V, VDL=-15V

    // PSR: Panel Setting / 面板设置
    // 0x0F = KW模式(黑白),从OTP加载LUT
    EPD_Write(UC81xx_PSR, 0x0f);  // Panel setting: KW mode, Load from OTP

    // TRES: Resolution Setting / 分辨率设置
    // 配置显示分辨率为800x480
    EPD_Write(UC81xx_TRES,
              epd->width >> 8,      // 宽度高字节
              epd->width & 0xFF,    // 宽度低字节
              epd->height >> 8,     // 高度高字节
              epd->height & 0xFF);  // Resolution: 800x480 // 分辨率设置完成

    // 0x15: Dual SPI mode / 双SPI模式
    // 0x00 = 禁用双SPI模式
    EPD_Write(0x15, 0x00);  // Dual SPI mode

    // CDI: VCOM and Data Interval Setting / VCOM和数据间隔设置
    EPD_Write(UC81xx_CDI, 0x11, 0x07);  // VCOM and Data interval

    // TCON: Gate/Source Timing Control / 栅极/源极时序控制
    // 控制显示驱动的时序参数
    EPD_Write(UC81xx_TCON, 0x22);  // TCON setting

    // 启动电源系统
    UC8179_PowerOn();
    NRF_LOG_INFO("[EPD]: UC8179 partial init done\n");
}

/**
 * @brief UC8179全刷清屏
 * @param epd EPD设备模型指针
 * @param refresh 是否执行刷新(true=刷新, false=仅写入RAM)
 *
 * 功能说明:
 * 清除显示RAM并可选地刷新屏幕为全白。
 * 使用全刷模式,会产生闪烁但能完全清除残影。
 */
void UC8179_Clear(epd_model_t* epd, bool refresh) {
    // 计算RAM字节数: (宽度+7)/8 * 高度
    // 每个字节包含8个像素
    uint32_t ram_bytes = ((epd->width + 7) / 8) * epd->height;

    // DTM1: 黑色层RAM - 0xFF表示所有像素为白色
    EPD_FillRAM(UC81xx_DTM1, 0xFF, ram_bytes);
    // DTM2: 红色层RAM - 0xFF表示没有红色(BWR模式)
    EPD_FillRAM(UC81xx_DTM2, 0xFF, ram_bytes);

    // 如果需要刷新,执行全屏刷新
    if (refresh) UC8179_Refresh(epd);
}

/**
 * @brief UC8179局刷模式清屏
 * @param epd EPD设备模型指针
 * @param refresh 是否执行刷新(true=刷新, false=仅写入RAM)
 *
 * 功能说明:
 * 这是局刷流程的步骤2和步骤3。
 * 在局刷之前,需要先将屏幕清为全白背景。
 *
 * 步骤说明:
 * 1. 进入局部模式(PTIN)
 * 2. 设置刷新区域为全屏
 * 3. 向DTM1(黑色层)写入0xFF(白色)
 * 4. 向DTM2(颜色层)写入0x00(无红色)
 * 5. 退出局部模式(PTOUT)
 * 6. 可选执行刷新显示白屏
 *
 * 注意:调用此函数前应已调用UC8179_Init_Partial()
 */
// Clear screen for UC8179 using partial refresh
// UC8179局刷模式清屏
// This implements the "clear white" step for partial refresh (Step 2+3)
// 实现局刷的"清白"步骤(步骤2+3)
// NOTE: Caller should have already called UC8179_Init_Partial() once
// 注意: 调用者应已调用过UC8179_Init_Partial()
void UC8179_Clear_Partial(epd_model_t *epd, bool refresh)
{
    uint32_t ram_bytes = ((epd->width + 7) / 8) * epd->height;

    NRF_LOG_INFO("[EPD]: UC8179 partial clear\n");

    // Step 2: Write white to both RAMs
    // 步骤2: 向两个RAM写入白色数据
    EPD_WriteCmd(UC81xx_PTIN);  // partial in // 进入局部模式
    _setPartialRamArea(epd, 0, 0, epd->width, epd->height);  // 设置全屏区域

    // For EPD_COLOR_BWR screen: 0xFF = white in black layer (DTM1/0x10)
    // 对于BWR屏幕: 0xFF = 黑色层中的白色(DTM1/0x10)
    NRF_LOG_DEBUG("[EPD]: Writing 0xFF to RAM1 (black layer)\n");
    EPD_FillRAM(UC81xx_DTM1, 0xFF, ram_bytes);  // 黑色层填充白色

    // For EPD_COLOR_BWR screen: 0x00 = no red in color layer (DTM2/0x13)
    // 对于BWR屏幕: 0x00 = 颜色层中无红色(DTM2/0x13)
    // Reference code uses ~0xFF = 0x00 for white background
    // 参考代码使用 ~0xFF = 0x00 作为白色背景
    NRF_LOG_DEBUG("[EPD]: Writing 0x00 to RAM2 (color layer, no red)\n");
    EPD_FillRAM(UC81xx_DTM2, 0x00, ram_bytes);  // 颜色层填充零(无红色)

    EPD_WriteCmd(UC81xx_PTOUT);  // partial out // 退出局部模式
    delay(100);  // Wait for partial mode to fully exit // 等待局部模式完全退出
    UC8179_WaitBusy(100);  // Ensure display is ready // 确保显示器就绪

    // Step 3: Refresh to display white screen
    // 步骤3: 刷新以显示白屏
    if (refresh) {
        NRF_LOG_INFO("[EPD]: Refreshing after clear\n");
        EPD_WriteCmd(UC81xx_DRF);  // 发送刷新命令
        delay(100);
        UC8179_WaitBusy(30000);    // 等待刷新完成
    }

    NRF_LOG_INFO("[EPD]: UC8179 partial clear done\n");
}

/**
 * @brief UC8179局部刷新
 * @param epd EPD设备模型指针
 * @param x 刷新区域起始X坐标
 * @param y 刷新区域起始Y坐标
 * @param w 刷新区域宽度
 * @param h 刷新区域高度
 *
 * 功能说明:
 * 这是局刷流程的步骤4和步骤6。
 * 执行指定区域的局部刷新,无闪烁快速更新显示内容。
 *
 * 重要步骤说明:
 * 步骤4 - 加载外部LUT:
 *   - 根据当前温度选择合适的LUT参数
 *   - 将LUT数据写入芯片寄存器(覆盖OTP中的默认LUT)
 *   - 配置PSR为0x3F以使用寄存器中的LUT
 *
 * 步骤6 - 执行刷新:
 *   - 进入局部模式
 *   - 设置刷新区域
 *   - 发送刷新命令
 *   - 等待刷新完成
 *   - 退出局部模式
 *
 * LUT加载说明:
 * - LUTC(0x20):  通用/清除LUT
 * - LUTWW(0x21): 白→白 过渡LUT
 * - LUTBW(0x22): 黑→白 过渡LUT (实际是LUTKW)
 * - LUTWB(0x23): 白→黑 过渡LUT (实际是LUTWK)
 * - LUTBB(0x24): 黑→黑 过渡LUT (实际是LUTKK)
 * - LUTBD(0x25): 边框LUT
 *
 * 注意:
 * - 调用此函数前应已调用UC8179_Init_Partial()和UC8179_Write_Image_Partial()
 * - 局部刷新只支持黑白,不支持红色!
 */
// Perform partial refresh for UC8179
// 执行UC8179局部刷新
// Step 4: Load external LUT, Step 6: Refresh
// 步骤4: 加载外部LUT, 步骤6: 刷新
// This is called AFTER writing image data
// 在写入图像数据后调用此函数
// NOTE: Caller should have already called UC8179_Init_Partial() once
// 注意: 调用者应已调用过UC8179_Init_Partial()
void UC8179_Refresh_Partial(epd_model_t *epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    NRF_LOG_INFO("[EPD]: UC8179 partial refresh (%d,%d,%d,%d)\n", x, y, w, h);

    // Make sure coordinates are aligned
    // 确保坐标对齐
    int16_t w1 = w;
    int16_t h1 = h;
    int16_t x1 = x;
    int16_t y1 = y;

    // 边界检查:确保刷新区域在屏幕范围内
    w1 = x1 + w1 < (int16_t)epd->width ? w1 : (int16_t)epd->width - x1;
    h1 = y1 + h1 < (int16_t)epd->height ? h1 : (int16_t)epd->height - y1;
    if ((w1 <= 0) || (h1 <= 0)) {
        NRF_LOG_ERROR("[EPD]: Invalid refresh area!\n");
        return;
    }

    // Make x1, w1 multiple of 8
    // 将x1和w1调整为8的倍数(字节对齐)
    w1 += x1 % 8;
    if (w1 % 8 > 0) w1 += 8 - w1 % 8;
    x1 -= x1 % 8;

    NRF_LOG_DEBUG("[EPD]: Adjusted area: (%d,%d,%d,%d)\n", x1, y1, w1, h1);

    // Read current temperature and select appropriate LUT
    // 读取当前温度并选择合适的LUT
    int8_t temperature = UC8179_Read_Temp(epd);
    const lut_set_t *lut = UC8179_Select_LUT(temperature);

    // Step 4: Load external LUT tables based on temperature
    // 步骤4: 根据温度加载外部LUT表
    NRF_LOG_DEBUG("[EPD]: Loading temperature-adaptive partial LUT\n");

    // 写入各个LUT表到相应的寄存器
    EPD_WriteCmd(UC81xx_LUTC);   // LUTC寄存器(0x20)
    EPD_WriteData((uint8_t *)lut->lutc, 42);
    EPD_WriteCmd(UC81xx_LUTWW);  // LUTWW寄存器(0x21)
    EPD_WriteData((uint8_t *)lut->lutww, 42);
    EPD_WriteCmd(UC81xx_LUTBW);  // LUTBW寄存器(0x22) - 黑→白
    EPD_WriteData((uint8_t *)lut->lutkw, 42);
    EPD_WriteCmd(UC81xx_LUTWB);  // LUTWB寄存器(0x23) - 白→黑
    EPD_WriteData((uint8_t *)lut->lutwk, 42);
    EPD_WriteCmd(UC81xx_LUTBB);  // LUTBB寄存器(0x24) - 黑→黑
    EPD_WriteData((uint8_t *)lut->lutkk, 42);
    EPD_WriteCmd(0x25);          // LUTBD寄存器(0x25) - 边框
    EPD_WriteData((uint8_t *)lut->lutbd, 42);

    // Configure panel for external LUT
    // 配置面板使用外部LUT
    NRF_LOG_DEBUG("[EPD]: Configuring panel for external LUT\n");

    // PSR=0x3F: 使用寄存器中的LUT而非OTP中的默认LUT
    EPD_Write(UC81xx_PSR, 0x3F);  // Use LUT from registers // 使用寄存器中的LUT
    EPD_Write(UC81xx_PWR, 0x07, 0x07, 0x3f, 0x3f);  // 电源设置
    EPD_Write(UC81xx_TRES, 0x03, 0x20, 0x01, 0xE0);  // 800x480 分辨率
    EPD_Write(0x15, 0x00);  // 双SPI模式禁用
    EPD_Write(UC81xx_TCON, 0x22);  // 时序控制
    EPD_Write(UC81xx_VDCS, 0x31);  // VCM_DC Setting // VCM直流设置
    EPD_Write(UC81xx_CDI, 0x39, 0x07);  // VCOM and data interval for partial // 局刷的VCOM和数据间隔

    UC8179_PowerOn();  // 启动电源

    // Set partial area and refresh
    // 设置局部区域并刷新
    NRF_LOG_DEBUG("[EPD]: Setting partial area and refreshing\n");
    EPD_WriteCmd(UC81xx_PTIN);  // partial in // 进入局部模式
    _setPartialRamArea(epd, x1, y1, w1, h1);  // 设置刷新区域

    // Step 6: Refresh
    // 步骤6: 执行刷新
    EPD_WriteCmd(UC81xx_DRF);  // 发送刷新命令
    delay(100);
    UC8179_WaitBusy(30000);    // 等待刷新完成

    EPD_WriteCmd(UC81xx_PTOUT);  // partial out // 退出局部模式

    NRF_LOG_INFO("[EPD]: UC8179 partial refresh done\n");
}

/**
 * @brief 全刷模式写入图像
 * @param epd EPD设备模型指针
 * @param black 黑色层图像数据指针(NULL表示全白)
 * @param color 颜色层图像数据指针(NULL表示无红色)
 * @param x 图像起始X坐标
 * @param y 图像起始Y坐标
 * @param w 图像宽度
 * @param h 图像高度
 *
 * 功能说明:
 * 将图像数据写入UC8179的显示RAM,用于全刷模式。
 *
 * 数据格式说明:
 * - 每个字节包含8个像素(1位每像素)
 * - 黑色层(DTM1/0x10): 0=黑色, 1=白色
 * - 颜色层(DTM2/0x13):
 *   - BWR模式: 0=无红色, 1=有红色
 *   - BW模式: 直接使用black数据
 *
 * 注意:
 * - X坐标和宽度会自动对齐到8像素边界
 * - 如果图像超出屏幕范围,函数会直接返回
 */
void UC8179_Write_Image(epd_model_t* epd, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w,
                        uint16_t h) {
    uint16_t wb = (w + 7) / 8;  // width bytes, bitmaps are padded // 宽度字节数,位图已填充
    x -= x % 8;                 // byte boundary // X坐标对齐到字节边界
    w = wb * 8;                 // byte boundary // 宽度对齐到字节边界

    // 边界检查
    if (x + w > epd->width || y + h > epd->height) return;

    EPD_WriteCmd(UC81xx_PTIN);  // partial in // 进入局部模式
    _setPartialRamArea(epd, x, y, w, h);  // 设置写入区域

    // 如果是BWR模式,写入黑色层数据
    if (epd->color == EPD_COLOR_BWR) {
        EPD_WriteCmd(UC81xx_DTM1);  // 选择黑色层RAM
        for (uint16_t i = 0; i < h; i++) {
            for (uint16_t j = 0; j < w / 8; j++)
                EPD_WriteByte(black ? black[j + i * wb] : 0xFF);  // 写入黑色数据或白色
        }
    }

    // 写入颜色层数据
    EPD_WriteCmd(UC81xx_DTM2);  // 选择颜色层RAM
    for (uint16_t i = 0; i < h; i++) {
        for (uint16_t j = 0; j < w / 8; j++) {
            if (epd->color == EPD_COLOR_BWR)
                EPD_WriteByte(color ? color[j + i * wb] : 0xFF);  // BWR模式:写入红色数据
            else
                EPD_WriteByte(black[j + i * wb]);  // BW模式:直接使用黑色数据
        }
    }

    EPD_WriteCmd(UC81xx_PTOUT);  // partial out // 退出局部模式
}

/**
 * @brief 局刷模式写入图像
 * @param epd EPD设备模型指针
 * @param black 黑色层图像数据指针(NULL表示全白)
 * @param color 颜色层图像数据指针(局刷中未使用)
 * @param x 图像起始X坐标
 * @param y 图像起始Y坐标
 * @param w 图像宽度
 * @param h 图像高度
 *
 * 功能说明:
 * 这是局刷流程的步骤5。
 * 将图像数据写入UC8179的RAM,用于局部刷新模式。
 *
 * 关键区别 - 全刷 vs 局刷:
 * ┌────────────────┬─────────────────┬─────────────────┐
 * │     项目       │     全刷        │     局刷        │
 * ├────────────────┼─────────────────┼─────────────────┤
 * │ 刷新范围       │ 整个屏幕        │ 指定区域        │
 * │ 刷新时间       │ 3-5秒           │ <1秒            │
 * │ 闪烁情况       │ 明显黑白闪烁    │ 无明显闪烁      │
 * │ 残影清除       │ 完全清除        │ 可能有轻微残影  │
 * │ 颜色支持       │ 黑白红三色      │ 仅黑白          │
 * │ LUT来源        │ OTP默认         │ 外部寄存器      │
 * │ RAM1写入       │ black数据       │ black数据       │
 * │ RAM2写入       │ red数据         │ ~black数据      │
 * │ 适用场景       │ 全新图像/清残影 │ 频繁小区域更新  │
 * └────────────────┴─────────────────┴─────────────────┘
 *
 * 局刷数据写入方法(参考"局刷使用方法.txt" 第141-143行):
 * - RAM1(0x10/DTM1): 写入 black 数据
 * - RAM2(0x13/DTM2): 写入 ~black 数据(取反)
 *
 * 为什么RAM2要写入反转数据?
 * 局刷模式下,芯片通过比较RAM1和RAM2的差异来决定哪些像素需要改变:
 * - RAM1和RAM2相同的位 → 像素不变(节省刷新时间)
 * - RAM1和RAM2不同的位 → 像素需要改变
 * 写入反转数据确保所有black区域都会被正确更新。
 *
 * 注意:
 * - 局刷只支持黑白,不支持红色!
 * - 调用此函数前应已调用UC8179_Init_Partial()
 * - 写入后需要调用UC8179_Refresh_Partial()才能显示
 */
// Write image for UC8179 partial refresh
// UC8179局刷模式写入图像
// Step 5: Write black data to old RAM (0x10) and ~black to new RAM (0x13)
// 步骤5: 向旧RAM(0x10)写入black数据,向新RAM(0x13)写入~black数据
// This function is called by DrawGUI via the buffer_callback
// 此函数由DrawGUI通过buffer_callback调用
void UC8179_Write_Image_Partial(epd_model_t *epd, uint8_t *black, uint8_t *color, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t wb = (w + 7) / 8; // width bytes, bitmaps are padded // 宽度字节数,位图已填充
    x -= x % 8;                // byte boundary // X坐标对齐到字节边界
    w = wb * 8;                // byte boundary // 宽度对齐到字节边界

    // 边界检查
    if (x + w > epd->width || y + h > epd->height) {
        NRF_LOG_WARNING("[EPD]: Image out of bounds! (%d,%d,%d,%d)\n", x, y, w, h);
        return;
    }

    NRF_LOG_INFO("[EPD]: UC8179 write image partial at (%d,%d) size (%d,%d)\n", x, y, w, h);

    EPD_WriteCmd(UC81xx_PTIN); // partial in // 进入局部模式
    _setPartialRamArea(epd, x, y, w, h);  // 设置写入区域

    // Step 5: Write black data to RAM1 (0x10) and ~black to RAM2 (0x13)
    // 步骤5: 向RAM1(0x10)写入black数据,向RAM2(0x13)写入~black数据
    // Reference: 局刷使用方法.txt line 141-143
    // 参考: 局刷使用方法.txt 第141-143行
    // "0x10 写black 数据, 0x13 写~black 数据"
    // NOTE: Partial refresh only supports black/white, no red color!
    // 注意: 局刷仅支持黑白,不支持红色!

    // Write black data to RAM1 (0x10) - directly write GFX data
    // 向RAM1(0x10)写入黑色数据 - 直接写入GFX数据
    NRF_LOG_DEBUG("[EPD]: Writing black data to RAM1 (0x10)\n");
    EPD_WriteCmd(UC81xx_DTM1);  // 选择RAM1(黑色层)
    for (uint16_t i = 0; i < h; i++)
    {
        for (uint16_t j = 0; j < wb; j++)
            EPD_WriteByte(black ? black[j + i * wb] : 0xFF);  // 0xFF = white // 0xFF = 白色
    }

    // Write ~black data to RAM2 (0x13) - inverted GFX data
    // 向RAM2(0x13)写入~black数据 - 反转的GFX数据
    NRF_LOG_DEBUG("[EPD]: Writing ~black data to RAM2 (0x13)\n");
    EPD_WriteCmd(UC81xx_DTM2);  // 选择RAM2(颜色层)
    for (uint16_t i = 0; i < h; i++)
    {
        for (uint16_t j = 0; j < wb; j++)
            EPD_WriteByte(black ? ~black[j + i * wb] : 0x00);  // inverted // 反转数据
    }

    EPD_WriteCmd(UC81xx_PTOUT); // partial out // 退出局部模式
    NRF_LOG_DEBUG("[EPD]: UC8179 write image partial done\n");
}

/**
 * @brief 写入RAM数据(通用接口)
 * @param epd EPD设备模型指针
 * @param cfg 配置字节(高4位:是否开始, 低4位:黑色或颜色层)
 * @param data 数据指针
 * @param len 数据长度
 *
 * 功能说明:
 * 这是一个通用的RAM写入接口,用于分块传输大量数据。
 *
 * cfg参数说明:
 * - bit[7:4]=0x00: 表示数据块的开始,需要发送命令选择RAM
 * - bit[7:4]!=0x00: 表示数据块的继续,不发送命令
 * - bit[3:0]=0x0F: 选择黑色层(DTM1)
 * - bit[3:0]!=0x0F: 选择颜色层(DTM2)
 */
void UC8179_Write_Ram(epd_model_t* epd, uint8_t cfg, uint8_t* data, uint8_t len) {
    bool begin = (cfg >> 4) == 0x00;  // 检查是否是数据块开始
    bool black = (cfg & 0x0F) == 0x0F;  // 检查是否是黑色层

    if (begin) {
        if (epd->color == EPD_COLOR_BWR)
            EPD_WriteCmd(black ? UC81xx_DTM1 : UC81xx_DTM2);  // BWR模式:选择相应的RAM
        else
            EPD_WriteCmd(UC81xx_DTM2);  // BW模式:总是选择DTM2
    }
    EPD_WriteData(data, len);  // 写入数据
}

/**
 * @brief 写入原生格式数据到RAM
 * @param epd EPD设备模型指针
 * @param cfg 配置字节
 * @param data 数据指针
 * @param len 数据长度
 *
 * 功能说明:
 * 写入原生格式数据,格式应为2pp(每像素2位)或更高。
 * 这个函数用于支持灰度或其他高位深度的显示模式。
 */
// Write native data to ram, format should be 2pp or above
// 写入原生数据到RAM,格式应为2pp或更高
void UC8179_Write_Ram_Native(epd_model_t* epd, uint8_t cfg, uint8_t* data, uint8_t len) {
    bool begin = (cfg >> 4) == 0x00;  // 检查是否是数据块开始
    bool black = (cfg & 0x0F) == 0x0F;  // 检查是否是黑色层

    if (begin && black) EPD_WriteCmd(UC81xx_DTM1);  // 仅在开始且为黑色层时发送命令
    EPD_WriteData(data, len);  // 写入数据
}

/**
 * @brief UC8179进入深度睡眠模式
 * @param epd EPD设备模型指针
 *
 * 功能说明:
 * 使UC8179进入深度睡眠模式以节省功耗。
 * 在睡眠模式下,芯片功耗降至最低,但显示内容会保持。
 *
 * 步骤:
 * 1. 关闭电源系统
 * 2. 等待电源完全关闭
 * 3. 发送深度睡眠命令(0xA5为魔术数)
 *
 * 注意:从深度睡眠唤醒需要重新初始化芯片
 */
void UC8179_Sleep(epd_model_t* epd) {
    UC8179_PowerOff();  // 关闭电源
    delay(100);         // 等待电源稳定关闭
    EPD_Write(UC81xx_DSLP, 0xA5);  // 进入深度睡眠,0xA5是确认码
}

/*
 * =============================================================================
 * 驱动接口定义
 * =============================================================================
 */

/**
 * UC8179全刷驱动接口
 *
 * 功能特点:
 * - 支持全屏刷新
 * - 支持黑白红三色显示(BWR模式)
 * - 使用OTP默认LUT
 * - 刷新时有明显闪烁,能完全清除残影
 */
static epd_driver_t epd_drv_uc8179 = {
    .ic = EPD_DRIVER_IC_UC8179,
    .init = UC8179_Init,              // 全刷初始化
    .clear = UC8179_Clear,            // 全刷清屏
    .write_image = UC8179_Write_Image,// 全刷写入图像
    .write_ram = UC8179_Write_Ram,    // 写入RAM数据
    .refresh = UC8179_Refresh,        // 全刷刷新
    .sleep = UC8179_Sleep,            // 进入睡眠
    .read_temp = UC8179_Read_Temp,    // 读取温度
};

/**
 * UC8179局刷驱动接口
 *
 * 功能特点:
 * - 支持全刷和局部刷新
 * - 局刷无明显闪烁,速度快
 * - 局刷仅支持黑白显示
 * - 使用温度自适应的外部LUT
 * - 适合频繁更新的应用场景
 */
static epd_driver_t epd_drv_uc8179_partial = {
    .ic = EPD_DRIVER_IC_UC8179,
    .init = UC8179_Init,              // 全刷初始化
    .clear = UC8179_Clear,            // 全刷清屏
    .write_image = UC8179_Write_Image,// 全刷写入图像
    .write_ram = UC8179_Write_Ram,    // 写入RAM数据
    .refresh = UC8179_Refresh,        // 全刷刷新
    .sleep = UC8179_Sleep,            // 进入睡眠
    .read_temp = UC8179_Read_Temp,    // 读取温度
    // Partial refresh support
    // 局刷功能支持
    .init_partial = UC8179_Init_Partial,            // 局刷初始化
    .clear_partial = UC8179_Clear_Partial,          // 局刷清屏
    .write_image_partial = UC8179_Write_Image_Partial,  // 局刷写入图像
    .refresh_partial = UC8179_Refresh_Partial,      // 局刷刷新
};

/*
 * =============================================================================
 * EPD型号定义
 * =============================================================================
 */

// // UC8179 800x480 Black/White
// // UC8179 800x480 黑白双色屏
// const epd_model_t epd_uc8179_750_bw = {
//     .id = EPD_UC8179_750_BW,
//     .color = EPD_COLOR_BW,
//     .drv = &epd_drv_uc8179,
//     .width = 800,
//     .height = 480,
// };

/**
 * UC8179 7.5寸 黑白红三色屏(全刷模式)
 *
 * 规格参数:
 * - 分辨率: 800x480
 * - 颜色: 黑白红三色
 * - 刷新模式: 仅全刷
 * - 刷新时间: 约3-5秒
 * - 适用场景: 需要红色显示或不频繁更新的应用
 */
// UC8179 800x480 Black/White/Red
// UC8179 800x480 黑白红三色屏
const epd_model_t epd_uc8179_750_bwr = {
    .id = EPD_UC8179_750_BWR,
    .color = EPD_COLOR_BWR,    // 黑白红三色
    .drv = &epd_drv_uc8179,    // 全刷驱动
    .width = 800,
    .height = 480,
};

/**
 * UC8179 7.5寸 黑白红三色屏(局刷模式)
 *
 * 规格参数:
 * - 分辨率: 800x480
 * - 颜色: 黑白红三色(全刷), 仅黑白(局刷)
 * - 刷新模式: 全刷+局刷
 * - 全刷时间: 约3-5秒
 * - 局刷时间: <1秒
 * - 温度自适应: 支持(-10°C ~ 50°C)
 * - 适用场景: 需要频繁更新小区域的应用(如时钟、传感器数据等)
 *
 * 使用建议:
 * 1. 首次显示或需要清除残影时使用全刷
 * 2. 频繁更新时使用局刷
 * 3. 每隔一段时间执行一次全刷以清除累积的残影
 */
// UC8179 800x480 Black/White/Red with Partial Refresh
// UC8179 800x480 黑白红三色屏(支持局刷)
const epd_model_t epd_uc8179_750_Partial_bwr = {
    .id = EPD_UC8179_750_BWR,
    .color = EPD_COLOR_BWR,            // 黑白红三色
    .drv = &epd_drv_uc8179_partial,    // 局刷驱动
    .width = 800,
    .height = 480,
};
