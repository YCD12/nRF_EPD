# 逻辑分析仪 trace vs epd_driver.c 代码对比报告

**Trace**: decoder--260615-172202.txt (506182字节, 288.6s = 4.8分钟)
**代码**: ESP32C3/esp32c3_7_5_zrx/epd_driver.c (逆向工程)
**芯片**: UC8179 7.5寸 800×480

---

## 总体流程

```
上电(≈16.8s) → 整刷一次 → [深度睡眠 23.5s] → 
→ 局刷#1(≈1.6s) → [睡眠 57.5s] → 
→ 局刷#2(≈1.6s) → [睡眠 57.5s] → 
→ 局刷#3(≈1.6s) → [睡眠 57.5s] → 
→ 局刷#4(≈1.6s) → [睡眠 57.5s] → 
→ 局刷#5(≈1.6s) → 结束
```

**局刷间隔**: 每个局刷之间睡眠约57.5s ≈ 1分钟 → 符合"局刷时钟5分钟"

---

## 1. LUT 数据对比  ✅ 完全匹配

### 局刷 LUT (表2 - lut_vcom2/lut_ww2/lut_bw2/lut_wb2/lut_bb2)

| 表 | 波形 | T1 | T2 | T3 | T4 | 重复 | Trace ✅ | Code ✅ |
|----|------|-----|-----|-----|-----|------|---------|---------|
| LUTC | 0x00 | 15 | 15 | 0 | 0 | 1 | ✅ | ✅ |
| LUTWW | 0x10 | 15 | 15 | 0 | 0 | 1 | ✅ | ✅ |
| LUTBW | 0x10 | 15 | 15 | 0 | 0 | 1 | ✅ | ✅ |
| LUTWB | 0x80 | 15 | 15 | 0 | 0 | **2** | ✅ | ✅ |
| LUTBB | 0x80 | 15 | 15 | 0 | 0 | 1 | ✅ | ✅ |

额外字节(bytes 6-11): `84 0F 01 0F 01 02`(LUTC除外) 也完全一致。

**结论**: LUT数据完全正确。

---

## 2. 整刷初始化对比 ❌ 代码缺失 PSR=0x0F 且结构与 trace 不符

### Trace 整刷序列 (Phase 1, 24B init + DTM1 + DRF):

```
PWR(01): 07 07 3F 3F        → 电源设置
PSR(00): 0F                  → 面板设置: KW模式, 使用OTP LUT! ← ⚠️ 
TRES(61): 03 20 01 E0        → 800x480
0x15: 00                     → 双SPI禁用
CDI(50): 11 07               → VCOM和数据间隔
TCON(60): 22                 → 时序控制
GSST(65): 00 00 00 00        → 位置偏移
--- 之后直接 DTM1(10) + 96003B 图像数据 ---
--- 然后 2.7s 后直接 DRF(12) ---
--- 然后 18s 后 POF(02) + DSLP(07 A5) ---
```

### epd_driver.c epd_show_full() 代码序列:

```
PWR(01): 07 07 3F 3F          ✅
PSR(00): 0F                   ✅ (代码 init 中 0x0F)
TRES(61): 03 20 01 E0         ✅
0x15: 00                      ✅
CDI(50): 11 07                ✅ (代码中 0x11, 0x07) 注: trace只有11 07, 代码同样
TCON(60): 22                  ✅
GSST(65): 00 00 00 00         ✅
PON(04)                       ❌ TRACE没有!
DTM1(10): image data          ✅
PTOUT(92)                     ❌ TRACE没有!
DTM2(13): FF all              ❌ TRACE没有!
PON(04)                       ❌ TRACE没有!
DRF(12)                       ✅ TRACE有
```

### 🔴 差异1: 整刷没有PON, PTOUT, DTM2

Trace中的整刷仅发送: 寄存器(24B) → DTM1 → DRF → POF → DSLP
代码发送: 寄存器 → PON → DTM1 → PTOUT → DTM2 → PON → DRF

分析: Trace中**没有PON**。UC8179可能在RESET后默认电源已开启(或PWR命令间接启动了电源)。DTM2(红色层)也没有写入, 但实际屏幕是BWR类型——如果DTM2未写入, 红色层RAM内容为随机值。

### 🔴 差异2: PSR=0x0F vs 代码的寄存器PSR设置

Trace整刷新用 `PSR=0x0F` (使用OTP LUT), 代码在 `init` 中同样使用0x0F, 但代码在 `init_partial` 中使用了 `PSR=0x82` 和 `PSR=0x00`。

### 🔴 差异3: 没有单独的0x15和CDI数据排列

trace整刷 init 把 `15 00` 连在一起, 代码也用 `0x15, 0x00` ✅

---

## 3. 局刷初始化对比 ❌ 代码序列与 trace 不同

### Trace 局刷初始化 (336B, Phase 6):

```
PWR(01): 07 07 3F 3F         电源设置
VDCS(82): 00                 VCM_DC = 0x00
BTST(06): 27 27 2F 17        软启动
PSR(00): 3F                  面板: 使用寄存器LUT, KW模式
TRES(61): 03 20 01 E0        800x480
0x15: 00                     双SPI禁用
CDI(50): 11 07               VCOM和数据间隔
TCON(60): 22                 时序控制
GSST(65): 00 00 00 00        位置偏移
LUTC(20)~LUTBB(24): 5×60B    LUT加载
```

### epd_driver.c epd_init_partial() 代码序列:

```
RESET → PWR(07 07 3F 3F) → PSR(82) → BTST(27 27 2F 17) → PSR(00) → PLL(3F)
→ TRES → 0x15(00) → CDI(11 07) → TCON(22) → GSST → LUTs
```

### 🔴 关键差异

| 项目 | Trace | 代码 | 谁对? |
|------|-------|------|-------|
| VDCS=0x00 | ✅ 有(CMD 0x82) | ❌ 没有 | **Trace对** - 实际固件发送了 |
| PSR=0x82 | ❌ 没有 | ✅ 有 | 不同版本 |
| PSR=0x3F | ✅ 有 | ❌ 没有 | **Trace对** - 实际固件用0x3F |
| PSR=0x00 | ❌ 没有 | ✅ 有 | 不同版本 |
| PLL=0x3F | ❌ 没有 | ✅ 有 | 不同版本 |

**解释**: 代码是 `PSR(82) → BTST → PSR(00) → PLL(3F)`, 但trace实际是 `VDCS(00) → BTST → PSR(3F)`, 完全不同。

---

## 4. 局刷写入+刷新对比 ❌ 代码缺失部分命令

### Trace 局刷写入 (Phase 7/8, 每个局刷周期):

```
VDCS(82): 22                  ← ⚠️ 代码没有!
CDI(50): A9 07                ← ⚠️ 代码: CDI(39 07)!
PTIN(91)                      ✅
PTL(90): x=440 xe=798 y=0 ye=293  ✅ 
DTM1(10): 40664B image data   ✅ (RAM1黑层)
--- 然后是 DTM2 ---
VDCS(82): 22                  ← ⚠️ 同上
CDI(50): A9 07                ← ⚠️ 同上
PTIN(91)                      ✅
PTL(90): 同坐标               ✅
DTM2(13): 40971B 0xFF为主     ✅ (RAM2颜色层,保持无红)
--- 然后是刷新 ---
FLG(71) × 1                   ← ⚠️ 代码没有!
DRF(12) + FLG(71)             ← ⚠️ 代码: PON+DRF
FLG(71) × 16 @100ms           ← 实际忙等
PTOUT(92): 02                 ✅ (代码没有参数)
DSLP(07): A5                  ✅
```

### epd_driver.c epd_write_image_partial() + epd_refresh_partial():

```
写图像: PTIN → PTL → DTM1(black) → [DTM2 if red] → PTOUT
刷新:   PON → DRF
```

### 🔴 关键差异

| 项目 | Trace | 代码 | 说明 |
|------|-------|------|------|
| VDCS=0x22 | ✅ 在写RAM前 | ❌ 没有 | **Trace对** |
| CDI=A9 07 | ✅ 在写RAM前 | CDI=39 07在refresh_partial | **不同** |
| 无PON | ❌ 没有PON | ✅ PON+DRF | 实际不需要PON? |
| PTOUT有参数02 | ✅ 92 02 | 92 (无参数) | **Trace对** |
| FLG预读 | ✅ 先FLG再DRF | ❌ 没有 | 忙检查 |
| 写+刷新合并在同一函数 | 合在一起(VDCS+CDI+PTIN+PTL+DTM+FLG+DRF+FLG×N+PTOUT+DSLP) | 分开为write_image和refresh | 架构不同 |

---

## 5. 局刷写入窗口坐标确认

```
PTL(90): 01 B8 03 1E 00 00 01 25 00
  x   = 0x01B8 = 440
  xe  = 0x031E = 798  
  y   = 0x0000 = 0
  ye  = 0x0125 = 293
  scan = 0x00
```

|x_aligned = 440 & 0xFFF8 = 440|
|计入字节对齐: ((x + w - 1) \| 7) = 798 → w = 798-440+1 = 359, 向上到byte边界 = 360px|
|字节宽度: 360/8 = 45B/行|
|行数: 293-0+1 = 294行|
|RAM大小: 45 × 294 = **13230B** (trace中DTM1却传了40664B, 包含SPI间隙补充)|

---

## 总结: 必须修改的代码

### ✅ LUT数据: 正确, 无需修改
5个LUT表(lut_vcom2~lut_bb2)与trace完全一致。

### ❌ epd_init_partial(): 需要修改
```
// 当前代码:
EPD_Write(UC81xx_PSR, 0x82);
EPD_Write(UC81xx_BTST, 0x27, 0x27, 0x2F, 0x17);
EPD_Write(UC81xx_PSR, 0x00);
EPD_Write(UC81xx_PLL, 0x3F);
EPD_Write(UC81xx_TRES, ...);
...

// Trace实际:
EPD_Write(UC81xx_VDCS, 0x00);     // ← 新增
EPD_Write(UC81xx_BTST, 0x27, 0x27, 0x2F, 0x17);
EPD_Write(UC81xx_PSR, 0x3F);       // ← 从 0x82→0x00→PLL 改为 直接 0x3F
EPD_Write(UC81xx_TRES, ...);       // 无PLL
...
```

### ❌ epd_write_image_partial(): 需要在PTIN前加VDCS+CDI
```
// 当前代码:
EPD_WriteCmd(UC81xx_PTIN);
EPD_Write(UC81xx_PTL, ...);

// Trace实际:
EPD_Write(UC81xx_VDCS, 0x22);     // ← 新增
EPD_Write(UC81xx_CDI, 0xA9, 0x07); // ← 新增 (值不同!)
EPD_WriteCmd(UC81xx_PTIN);
EPD_Write(UC81xx_PTL, ...);
```

### ❌ epd_refresh_partial(): CDI参数不同
```
// 当前代码:
EPD_Write(UC81xx_CDI, 0x39, 0x07);
// Trace在每次写RAM前用:
EPD_Write(UC81xx_CDI, 0xA9, 0x07);
```

### ❌ 无PON 
trace中整刷和局刷都不发送PON(0x04)命令。PON可能在RESET或PWR后被隐含触发, 或已在其他位置完成。
