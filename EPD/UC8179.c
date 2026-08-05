#include "UC8179.h"
#include "nrf_log.h"

extern void app_feed_wdt(void);

#define UC8179_ROW_CHUNK_SIZE 64U
#define UC8179_WDT_FEED_ROWS  8U
#define UC8179_LUT_SIZE       42U
#define UC8179_MIN_PARTIAL_W  16U
#define UC8179_MIN_PARTIAL_H  2U

/* UC8179c C0.6 register values used by this panel. */
#define UC8179_PSR_KWR_OTP    0x0FU
#define UC8179_PSR_KW_OTP     0x1FU
#define UC8179_PSR_KW_REG     0x3FU
#define UC8179_CDI_KWR        0x13U
#define UC8179_CDI_KW_FULL    0x93U
#define UC8179_CDI_KW_DIFF    0xA1U
#define UC8179_CDI_INTERVAL   0x07U
#define UC8179_PARTIAL_VCOM   0x30U

static bool m_uc8179_busy_timeout;

bool UC8179_HadBusyTimeout(void) { return m_uc8179_busy_timeout; }
void UC8179_ClearBusyTimeout(void) { m_uc8179_busy_timeout = false; }



/* BUSY_N is low while busy and high when the controller is ready. */
static bool UC8179_WaitBusy(uint16_t timeout) {
    bool ready = EPD_WaitBusy(LOW, timeout);
    if (!ready) {
        m_uc8179_busy_timeout = true;
    }
    return ready;
}

static bool UC8179_PowerOn(void) {
    EPD_WriteCmd(UC81xx_PON);
    return UC8179_WaitBusy(1000);
}

static bool UC8179_PowerOff(void) {
    EPD_WriteCmd(UC81xx_POF);
    bool ready = UC8179_WaitBusy(1000);
    /* GDEY075Z08 reference keeps a 100 ms discharge guard after POF. */
    delay(100);
    return ready;
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
    (void)epd;
    EPD_WriteCmd(UC81xx_TSC);  // 发送温度传感器控制命令
    UC8179_WaitBusy(1000);      // 等待温度转换完成
    return (int8_t)EPD_ReadByte();  // 读取温度值
}

static void UC8179_SetPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
static void UC8179_Write_Image_Rows(const uint8_t* src, uint16_t wb, uint16_t h, uint8_t fill);

/*
 * Register waveform used by the feature-branch GDEY075Z08 implementation.
 * A 42-byte table is seven 6-byte waveform states, not "6 bytes + padding".
 * The four 2-bit fields in each state's first byte select voltage levels.
 */
static const uint8_t UC8179_LUT_HOLD[UC8179_LUT_SIZE] = {
    0x00, 0x03, 0x05, 0x00, 0x00, 0x02,
    0x00, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x00, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x00, 0x05, 0x05, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t UC8179_LUT_KW[UC8179_LUT_SIZE] = {
    0x20, 0x03, 0x05, 0x00, 0x00, 0x02,
    0x20, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x00, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x20, 0x05, 0x05, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t UC8179_LUT_WK[UC8179_LUT_SIZE] = {
    0x40, 0x03, 0x05, 0x00, 0x00, 0x02,
    0x40, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x00, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x40, 0x05, 0x05, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t UC8179_LUT_BORDER[UC8179_LUT_SIZE] = {0};

static void UC8179_Load_Partial_LUT(void) {
    static const uint8_t registers[6] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25};
    static const uint8_t* const luts[6] = {
        UC8179_LUT_HOLD,
        UC8179_LUT_HOLD,
        UC8179_LUT_KW,
        UC8179_LUT_WK,
        UC8179_LUT_HOLD,
        UC8179_LUT_BORDER,
    };

    for (uint8_t i = 0; i < 6U; i++) {
        EPD_WriteCmd(registers[i]);
        EPD_WriteData((uint8_t*)luts[i], UC8179_LUT_SIZE);
        app_feed_wdt();
    }
}

static bool UC8179_PartialAreaValid(const epd_model_t *epd, uint16_t x, uint16_t y,
                                    uint16_t w, uint16_t h) {
    return epd != NULL &&
           w >= UC8179_MIN_PARTIAL_W && h >= UC8179_MIN_PARTIAL_H &&
           (x & 7U) == 0U && (w & 7U) == 0U &&
           (uint32_t)x + w <= epd->width &&
           (uint32_t)y + h <= epd->height;
}

bool UC8179_Partial_Diff_Push(epd_model_t *epd, uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h,
                              const uint8_t *old_bm, const uint8_t *new_bm) {
    if (!UC8179_PartialAreaValid(epd, x, y, w, h) ||
        old_bm == NULL || new_bm == NULL) {
        return false;
    }

    uint16_t wb = w / 8U;
    EPD_WriteCmd(UC81xx_PTIN);
    UC8179_SetPartialRamArea(x, y, w, h);

    /* UC8179c KW mode: DTM1 is OLD and DTM2 is NEW. */
    EPD_WriteCmd(UC81xx_DTM1);
    UC8179_Write_Image_Rows(old_bm, wb, h, 0xFF);

    EPD_WriteCmd(UC81xx_DTM2);
    UC8179_Write_Image_Rows(new_bm, wb, h, 0xFF);

    EPD_WriteCmd(UC81xx_PTOUT);
    return true;
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
 * 注意:调用者必须提供字节对齐且满足 HRED>HRST、VRED>VRST 的区域;
 * 公共局刷入口会在发送命令前严格校验。
 */
static void UC8179_SetPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t xe = x + w - 1U;
    uint16_t ye = y + h - 1U;

    EPD_Write(UC81xx_PTL,
              x >> 8, x & 0xFF,
              xe >> 8, xe & 0xFF,
              y >> 8, y & 0xFF,
              ye >> 8, ye & 0xFF,
              0x00);  /* PT_SCAN=0: scan only inside the partial window. */
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

static void UC8179_ConfigurePowerAndTiming(const epd_model_t *epd) {
    /* PWR has five parameters on UC8179c C0.6.  Bit 4 of byte 2 is fixed 1. */
    EPD_Write(UC81xx_PWR, 0x07, 0x17, 0x3F, 0x3F, 0x03);
    EPD_Write(UC81xx_TRES,
              epd->width >> 8, epd->width & 0xFF,
              epd->height >> 8, epd->height & 0xFF);
    EPD_Write(UC81xx_DUSPI, 0x00);
    EPD_Write(UC81xx_TCON, 0x22);
}

static void UC8179_ConfigureFull(const epd_model_t *epd) {
    UC8179_ConfigurePowerAndTiming(epd);
    EPD_Write(UC81xx_PSR,
              epd->color == EPD_COLOR_BWR ? UC8179_PSR_KWR_OTP : UC8179_PSR_KW_OTP);
    EPD_Write(UC81xx_VDCS, 0x00);
    EPD_Write(UC81xx_CDI,
              epd->color == EPD_COLOR_BWR ? UC8179_CDI_KWR : UC8179_CDI_KW_FULL,
              UC8179_CDI_INTERVAL);
}

void UC8179_Refresh(epd_model_t* epd) {
    if (epd == NULL) return;

    NRF_LOG_DEBUG("[EPD]: refresh begin\n");

    /* A previous partial update leaves PSR in KW/register-LUT mode. */
    UC8179_ConfigureFull(epd);
    if (!UC8179_PowerOn()) {
        UC8179_PowerOff();
        return;
    }

    EPD_WriteCmd(UC81xx_DRF);
    delay(100);
    UC8179_WaitBusy(30000);
    UC8179_PowerOff();
    NRF_LOG_DEBUG("[EPD]: refresh end\n");
}

void UC8179_Init(epd_model_t* epd) {
    if (epd == NULL) return;

    EPD_Reset(HIGH, 10);
    if (!UC8179_WaitBusy(1000)) return;
    UC8179_ConfigureFull(epd);
}

void UC8179_Init_Partial(epd_model_t *epd)
{
    if (epd == NULL) return;

    NRF_LOG_INFO("[EPD]: UC8179 partial init\n");
    EPD_Reset(HIGH, 10);
    if (!UC8179_WaitBusy(1000)) return;

    UC8179_ConfigurePowerAndTiming(epd);
    /* REG=1, KW/R=1: register LUT with DTM1=OLD and DTM2=NEW. */
    EPD_Write(UC81xx_PSR, UC8179_PSR_KW_REG);
    EPD_Write(UC81xx_VDCS, UC8179_PARTIAL_VCOM);
    /* DDX=01 enables explicit OLD/NEW; N2OCP is intentionally disabled. */
    EPD_Write(UC81xx_CDI, UC8179_CDI_KW_DIFF, UC8179_CDI_INTERVAL);
    UC8179_Load_Partial_LUT();

    /* Keep high-voltage supplies off while OLD/NEW SRAM is populated. */
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
 * Refresh a previously populated OLD/NEW window.
 *
 * High voltage is enabled only for the display operation.  The order follows
 * the UC8179 flow: PON -> PTIN/PTL -> DRF -> PTOUT -> POF.
 */
bool UC8179_Refresh_Partial(epd_model_t *epd, uint16_t x, uint16_t y,
                             uint16_t w, uint16_t h) {
    if (!UC8179_PartialAreaValid(epd, x, y, w, h)) {
        NRF_LOG_ERROR("[EPD]: invalid partial refresh area (%u,%u,%u,%u)\n",
                      x, y, w, h);
        return false;
    }

    NRF_LOG_INFO("[EPD]: UC8179 partial refresh (%u,%u,%u,%u)\n", x, y, w, h);
    if (!UC8179_PowerOn()) {
        UC8179_PowerOff();
        return false;
    }

    EPD_WriteCmd(UC81xx_PTIN);
    UC8179_SetPartialRamArea(x, y, w, h);
    EPD_WriteCmd(UC81xx_DRF);
    delay(100);
    bool refreshed = UC8179_WaitBusy(30000);
    EPD_WriteCmd(UC81xx_PTOUT);
    bool powered_off = UC8179_PowerOff();

    return refreshed && powered_off;
}

static void UC8179_Write_Image_Rows(const uint8_t* src, uint16_t wb,
                                     uint16_t h, uint8_t fill) {
    uint8_t temp[UC8179_ROW_CHUNK_SIZE];

    for (uint16_t row = 0; row < h; row++) {
        uint16_t remaining = wb;
        uint16_t offset = 0;

        while (remaining > 0U) {
            uint8_t chunk = remaining > UC8179_ROW_CHUNK_SIZE ?
                            UC8179_ROW_CHUNK_SIZE : (uint8_t)remaining;

            if (src != NULL) {
                EPD_WriteData((uint8_t*)&src[(uint32_t)row * wb + offset], chunk);
            } else {
                for (uint8_t i = 0; i < chunk; i++) temp[i] = fill;
                EPD_WriteData(temp, chunk);
            }

            offset += chunk;
            remaining -= chunk;
        }

        if ((row % UC8179_WDT_FEED_ROWS) == 0U) app_feed_wdt();
    }
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
 *   - BWR模式: 0=红色, 1=无红色
 *   - BW模式: 直接使用black数据
 *
 * 注意:
 * - X坐标和宽度会自动对齐到8像素边界
 * - 如果图像超出屏幕范围,函数会直接返回
 */
void UC8179_Write_Image(epd_model_t* epd, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w,
                        uint16_t h) {
    if (epd == NULL || w == 0U || h == 0U || (x & 7U) != 0U) return;

    uint16_t wb = (w + 7) / 8;  // width bytes, bitmaps are padded // 宽度字节数,位图已填充
    w = wb * 8;                 // byte boundary // 宽度对齐到字节边界

    // 边界检查
    if ((uint32_t)x + w > epd->width || (uint32_t)y + h > epd->height) return;

    EPD_WriteCmd(UC81xx_PTIN);  // partial in // 进入局部模式
    UC8179_SetPartialRamArea(x, y, w, h);  // 设置写入区域

    // 如果是BWR模式,写入黑色层数据
    if (epd->color == EPD_COLOR_BWR) {
        EPD_WriteCmd(UC81xx_DTM1);  // 选择黑色层RAM
        UC8179_Write_Image_Rows(black, wb, h, 0xFF);  // 写入黑色数据或白色
    }

    // 写入颜色层数据
    EPD_WriteCmd(UC81xx_DTM2);  // 选择颜色层RAM
    if (epd->color == EPD_COLOR_BWR) {
        UC8179_Write_Image_Rows(color, wb, h, 0xFF);  // BWR模式:写入红色数据
    } else {
        UC8179_Write_Image_Rows(black, wb, h, 0xFF);  // BW模式:直接使用黑色数据
    }

    EPD_WriteCmd(UC81xx_PTOUT);  // partial out // 退出局部模式
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
    (void)epd;
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
 * UC8179局刷驱动接口
 *
 * 功能特点:
 * - 支持全刷和局部刷新
 * - 局刷无明显闪烁,速度快
 * - 局刷仅支持黑白显示
 * - 使用经面板验证的固定寄存器LUT
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
    .refresh_partial = UC8179_Refresh_Partial,      // 局刷刷新
    .diff_push = UC8179_Partial_Diff_Push,          // 差分帧写入 (pixel-level partial)
};

/*
 * =============================================================================
 * EPD型号定义
 * =============================================================================
 */

/**
 * UC8179 7.5寸 黑白红三色屏(局刷模式)
 *
 * 规格参数:
 * - 分辨率: 800x480
 * - 颜色: 黑白红三色(全刷), 仅黑白(局刷)
 * - 刷新模式: 全刷+局刷
 * - 全刷时间: 约3-5秒
 * - 局刷时间: <1秒
 * - LUT: 固定的GDEY075Z08寄存器波形,实际温区需真机验证
 * - 适用场景: 需要频繁更新小区域的应用(如时钟、传感器数据等)
 *
 * 使用建议:
 * 1. 首次显示或需要清除残影时使用全刷
 * 2. 频繁更新时使用局刷
 * 3. 每隔一段时间执行一次全刷以清除累积的残影
 */
// UC8179 800x480 Black/White/Red with Partial Refresh
// UC8179 800x480 黑白红三色屏(支持局刷)
epd_model_t epd_uc8179_750_Partial_bwr = {
    .id = EPD_UC8179_750_BWR,
    .color = EPD_COLOR_BWR,            // 黑白红三色
    .drv = &epd_drv_uc8179_partial,    // 局刷驱动
    .width = 800,
    .height = 480,
};
