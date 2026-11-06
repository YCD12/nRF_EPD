/**
 * @file uc81xx.h
 * @brief UC81xx 系列命令码（仅面板驱动层使用）
 *
 * 从 hal/EPD_driver.h 下沉到 driver 层；服务层不再依赖具体驱动 IC 命令。
 */
#ifndef UC81XX_H
#define UC81XX_H

enum {
    UC81xx_PSR = 0x00,   /* Panel Setting */
    UC81xx_PWR = 0x01,   /* Power Setting */
    UC81xx_POF = 0x02,   /* Power OFF */
    UC81xx_PFS = 0x03,   /* Power OFF Sequence */
    UC81xx_PON = 0x04,   /* Power ON */
    UC81xx_PMES = 0x05,  /* Power ON Measure */
    UC81xx_BTST = 0x06,  /* Booster Soft Start */
    UC81xx_DSLP = 0x07,  /* Deep sleep */
    UC81xx_DTM1 = 0x10,  /* Display Start Transmission 1 */
    UC81xx_DSP = 0x11,   /* Data Stop */
    UC81xx_DRF = 0x12,   /* Display Refresh */
    UC81xx_DTM2 = 0x13,  /* Display Start Transmission 2 */
    UC81xx_DUSPI = 0x15, /* Dual SPI */
    UC81xx_LUTC = 0x20,
    UC81xx_LUTWW = 0x21,
    UC81xx_LUTBW = 0x22,
    UC81xx_LUTWB = 0x23,
    UC81xx_LUTBB = 0x24,
    UC81xx_PLL = 0x30,
    UC81xx_TSC = 0x40,
    UC81xx_TSE = 0x41,
    UC81xx_TSW = 0x42,
    UC81xx_TSR = 0x43,
    UC81xx_CDI = 0x50,
    UC81xx_LPD = 0x51,
    UC81xx_TCON = 0x60,
    UC81xx_TRES = 0x61,
    UC81xx_GSST = 0x65,
    UC81xx_REV = 0x70,
    UC81xx_FLG = 0x71,
    UC81xx_AMV = 0x80,
    UC81xx_VV = 0x81,
    UC81xx_VDCS = 0x82,
    UC81xx_PTL = 0x90,
    UC81xx_PTIN = 0x91,
    UC81xx_PTOUT = 0x92,
    UC81xx_PGM = 0xA0,
    UC81xx_APG = 0xA1,
    UC81xx_ROTP = 0xA2,
    UC81xx_CCSET = 0xE0,
    UC81xx_PWS = 0xE3,
    UC81xx_TSSET = 0xE5,
};

#endif /* UC81XX_H */
