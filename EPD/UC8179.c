#include "UC8179.h"
#include "nrf_log.h"

extern void app_feed_wdt(void);

#define UC8179_ROW_CHUNK_SIZE 64U
#define UC8179_WDT_FEED_ROWS  8U
#define UC8179_LUT_SIZE       42U
#define UC8179_MIN_PARTIAL_W  16U
#define UC8179_MIN_PARTIAL_H  2U

/* 本屏使用的 UC8179c C0.6 寄存器值 */
#define UC8179_PSR_KWR_OTP    0x0FU
#define UC8179_PSR_KW_OTP     0x1FU
#define UC8179_PSR_KW_REG     0x3FU
#define UC8179_CDI_KWR        0x13U
#define UC8179_CDI_KW_FULL    0x93U
/* BDZ=1：边框高阻。N2OCP=1 与真机验证的 A9 一致。
 * DDX 仍为 00（0xA1/0xA9 仅 N2OCP 不同）；不要设 DDX=01。 */
#define UC8179_CDI_KW_DIFF    0xA9U
#define UC8179_CDI_INTERVAL   0x07U
#define UC8179_PARTIAL_VCOM   0x22U

static bool m_uc8179_busy_timeout;

bool UC8179_HadBusyTimeout(void) { return m_uc8179_busy_timeout; }
void UC8179_ClearBusyTimeout(void) { m_uc8179_busy_timeout = false; }



/* BUSY_N：忙时为低，就绪为高 */
static bool UC8179_WaitBusy(uint16_t timeout) {
    bool ready = EPD_WaitBusy(LOW, timeout);
    if (!ready) {
        m_uc8179_busy_timeout = true;
    }
    return ready;
}

static bool UC8179_PowerOn(void) /* PON，等忙 */
{
    EPD_WriteCmd(UC81xx_PON);
    return UC8179_WaitBusy(1000);
}

static bool UC8179_PowerOff(void) /* POF + 100 ms 放电 */
{
    EPD_WriteCmd(UC81xx_POF);
    bool ready = UC8179_WaitBusy(1000);
    /* 参考 GDEY075Z08：POF 后保留 100 ms 放电间隔 */
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
// 从驱动芯片读取温度
int8_t UC8179_Read_Temp(const epd_model_t * epd) {
    (void)epd;
    EPD_WriteCmd(UC81xx_TSC);  // 发送温度传感器控制命令
    UC8179_WaitBusy(1000);  // 等待温度转换完成
    return (int8_t)EPD_ReadByte();  // 读取温度值
}

static void UC8179_SetPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
static void UC8179_Write_Image_Rows(const uint8_t* src, uint16_t wb, uint16_t h, uint8_t fill);

/*
 * 上一版固件真机验证过的局刷 LUT。
 * 一组 6 字节状态，其余为 0：T1=1, T2=0x2A, T3=0x02, repeat=1。
 * WK 用 0x10（不是 GDEY 的 0x40 极性）。边框用 HOLD。
 */
static void UC8179_Load_Partial_LUT(void)
{
    uint8_t lut[UC8179_LUT_SIZE] = {0, 1, 0x2A, 2, 0, 1};
    for (uint8_t i = 0; i < 6U; i++) {
        lut[0] = i == 2U ? 0x20U : i == 3U ? 0x10U : 0U;
        EPD_WriteCmd(UC81xx_LUTC + i);
        EPD_WriteData(lut, sizeof(lut));
        app_feed_wdt();
    }
}

/* 局刷窗：最小 16x2，x/w 8 对齐，不越界。 */
static bool UC8179_PartialAreaValid(const epd_model_t *epd, uint16_t x, uint16_t y,
                                    uint16_t w, uint16_t h) {
    return epd != NULL &&
           w >= UC8179_MIN_PARTIAL_W && h >= UC8179_MIN_PARTIAL_H &&
           (x & 7U) == 0U && (w & 7U) == 0U &&
           (uint32_t)x + w <= epd->width &&
           (uint32_t)y + h <= epd->height;
}

/* 按 src_stride 把窗口位图写入当前 DTM 流。 */
static void UC8179_Write_Packed_Rows(const uint8_t *src, uint16_t src_stride,
                                     uint16_t w, uint16_t h) {
    uint16_t wb = w / 8U;

    for (uint16_t row = 0; row < h; row++) {
        const uint8_t *row_src = src + (uint32_t)row * src_stride;
        uint16_t remaining = wb;
        uint16_t offset = 0U;

        while (remaining > 0U) {
            uint8_t chunk = remaining > UC8179_ROW_CHUNK_SIZE
                                ? UC8179_ROW_CHUNK_SIZE
                                : (uint8_t)remaining;
            EPD_WriteData((uint8_t *)&row_src[offset], chunk);
            offset += chunk;
            remaining -= chunk;
        }

        if ((row % UC8179_WDT_FEED_ROWS) == 0U) app_feed_wdt();
    }
}

/* 同一 PTL 窗口写入打包的 OLD(DTM1) 与 NEW(DTM2)。 */
bool UC8179_Partial_Diff_Push(const epd_model_t *epd, uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h,
                              const uint8_t *old_bm, const uint8_t *new_bm) {
    if (!UC8179_PartialAreaValid(epd, x, y, w, h) ||
        old_bm == NULL || new_bm == NULL) {
        return false;
    }

    uint16_t wb = w / 8U;
    EPD_WriteCmd(UC81xx_PTIN);
    UC8179_SetPartialRamArea(x, y, w, h);
    EPD_WriteCmd(UC81xx_DTM1);
    UC8179_Write_Packed_Rows(old_bm, wb, w, h);
    EPD_WriteCmd(UC81xx_DTM2);
    UC8179_Write_Packed_Rows(new_bm, wb, w, h);
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
              0x00);  /* PT_SCAN=0：只扫描局刷窗口内 */
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

static void UC8179_ConfigurePowerAndTiming(const epd_model_t *epd) /* PWR/TRES/TCON */
{
    /* UC8179c C0.6 的 PWR 有 5 个参数。第 2 字节 bit4 固定为 1。 */
    EPD_Write(UC81xx_PWR, 0x07, 0x17, 0x3F, 0x3F, 0x03);
    EPD_Write(UC81xx_TRES,
              epd->width >> 8, epd->width & 0xFF,
              epd->height >> 8, epd->height & 0xFF);
    EPD_Write(UC81xx_DUSPI, 0x00);
    EPD_Write(UC81xx_TCON, 0x22);
}

static void UC8179_ConfigureFull(const epd_model_t *epd) /* OTP LUT 全刷 PSR/CDI */
{
    UC8179_ConfigurePowerAndTiming(epd);
    EPD_Write(UC81xx_PSR,
              epd->color == EPD_COLOR_BWR ? UC8179_PSR_KWR_OTP : UC8179_PSR_KW_OTP);
    EPD_Write(UC81xx_VDCS, 0x00);
    EPD_Write(UC81xx_CDI,
              epd->color == EPD_COLOR_BWR ? UC8179_CDI_KWR : UC8179_CDI_KW_FULL,
              UC8179_CDI_INTERVAL);
}

/* OTP 全刷：PON → DRF → POF。 */
void UC8179_Refresh(const epd_model_t * epd) {
    if (epd == NULL) return;

    NRF_LOG_DEBUG("[EPD]: refresh begin\n");

    /* 上次局刷会把 PSR 留在 KW/寄存器 LUT 模式 */
    UC8179_ConfigureFull(epd);
    if (!UC8179_PowerOn()) {
        UC8179_PowerOff();
        return;
    }

    EPD_WriteCmd(UC81xx_DRF);
    delay(100);
    if (!UC8179_WaitBusy(30000) && EPD_BusyAbortRequested()) {
        UC8179_PowerOff();
        return;
    }
    UC8179_PowerOff();
    NRF_LOG_DEBUG("[EPD]: refresh end\n");
}

void UC8179_Init(const epd_model_t * epd) /* 复位后进 OTP 全刷模式 */
{
    if (epd == NULL) return;

    EPD_Reset(HIGH, 10);
    if (!UC8179_WaitBusy(1000)) return;
    UC8179_ConfigureFull(epd);
}

void UC8179_Init_Partial(const epd_model_t *epd)
{
    if (epd == NULL) return;

    NRF_LOG_INFO("[EPD]: UC8179 partial configure\n");
    /* 从 POF/DSLP 唤醒，不经过全刷 OTP 模式。
     * OLD/NEW 由软件提供，复位 SRAM 可以接受。 */
    EPD_Reset(HIGH, 10);
    if (!UC8179_WaitBusy(1000)) return;

    UC8179_ConfigurePowerAndTiming(epd);
    /* REG=1, KW/R=1：寄存器 LUT，DTM1=OLD，DTM2=NEW */
    EPD_Write(UC81xx_PSR, UC8179_PSR_KW_REG);
    EPD_Write(UC81xx_VDCS, UC8179_PARTIAL_VCOM);
    EPD_Write(UC81xx_CDI, UC8179_CDI_KW_DIFF, UC8179_CDI_INTERVAL);
    UC8179_Load_Partial_LUT();

    /* 写入 OLD/NEW SRAM 期间保持高压电源关闭 */
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
void UC8179_Clear(const epd_model_t * epd, bool refresh) {
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
 * 刷新已写入的 OLD/NEW 窗口。
 *
 * 仅在显示操作时开高压。顺序按 UC8179：PON -> PTIN/PTL -> DRF -> PTOUT -> POF。
 */
bool UC8179_Refresh_Partial(const epd_model_t *epd, uint16_t x, uint16_t y,
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

/* 逐行写入；src 为空则填 fill。 */
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
 * - BWR模式: 0=红色, 1=无红色
 * - BW模式: 直接使用black数据
 *
 * 注意:
 * - X坐标和宽度会自动对齐到8像素边界
 * - 如果图像超出屏幕范围,函数会直接返回
 */
void UC8179_Write_Image(const epd_model_t * epd, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w,
                        uint16_t h) {
    if (EPD_BusyAbortRequested()) return;
    if (epd == NULL || w == 0U || h == 0U || (x & 7U) != 0U) return;

    uint16_t wb = (w + 7) / 8;  // width bytes, bitmaps are padded // 宽度字节数,位图已填充
    w = wb * 8;  // byte boundary // 宽度对齐到字节边界

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
void UC8179_Write_Ram(const epd_model_t * epd, uint8_t cfg, uint8_t* data, uint8_t len) {
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
void UC8179_Sleep(const epd_model_t * epd) {
    (void)epd;
    UC8179_PowerOff();  // 关闭电源
    delay(100);  // 等待电源稳定关闭
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
static const epd_driver_t epd_drv_uc8179_partial = {
    .ic = EPD_DRIVER_IC_UC8179,
    .init = UC8179_Init,  // 全刷初始化
    .clear = UC8179_Clear,  // 全刷清屏
    .write_image = UC8179_Write_Image,  // 全刷写入图像
    .write_ram = UC8179_Write_Ram,  // 写入RAM数据
    .refresh = UC8179_Refresh,  // 全刷刷新
    .sleep = UC8179_Sleep,  // 进入睡眠
    .read_temp = UC8179_Read_Temp,  // 读取温度
    // 局刷功能支持
    .init_partial = UC8179_Init_Partial,  // 局刷初始化
    .refresh_partial = UC8179_Refresh_Partial,  // 局刷刷新
    .diff_push = UC8179_Partial_Diff_Push,  // 同一窗口写入 OLD+NEW
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
// UC8179 800x480 黑白红三色屏(支持局刷)
const epd_model_t epd_uc8179_750_Partial_bwr = {
    .id = EPD_UC8179_750_BWR,
    .color = EPD_COLOR_BWR,  // 黑白红三色
    .drv = &epd_drv_uc8179_partial,  // 局刷驱动
    .width = 800,
    .height = 480,
};
