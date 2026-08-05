# UC8179 官方 OLD/NEW 局刷重构记录

- 日期：2026-08-05
- 开发分支：`dev`
- 修改基线：`af6c860`
- 对照分支：`feature`，UC8179 相关提交 `e08ce12`
- 控制器：UC8179c C0.6
- 面板：800 × 480 BWR，局刷阶段使用 KW 黑白模式
- 官方依据：`docs/datasheets/UC8179.pdf`、`docs/datasheets/UC8179.txt`
- 状态：代码重构、主机回归、ARM 固件编译及 GUI golden test 已通过；实屏波形效果仍需验证

## 1. 修改目的

本次修改解决原 `dev` 局刷实现中的四类问题：

1. OLD/NEW 数据虽然部分路径写入顺序正确，但注释、遗留接口和伪造 OLD 数据的路径互相矛盾，容易再次把 DTM1、DTM2 用反。
2. PWR、CDI、VDCS、LUT 等配置与 UC8179c C0.6 的寄存器格式不完全一致，且全刷、局刷在多个函数中重复配置。
3. 原时钟差分只更新变化数字，状态来源和物理刷新结果没有形成严格事务，失败后可能失去可信的 OLD 帧。
4. 同一 UC8179 ID 同时注册“全刷型号”和“局刷型号”，但运行时实际始终选中第一个，形成不可达的重复实现。

重构后的基本原则是：寄存器语义和命令约束以官方 datasheet 为准；`feature` 分支仅作为 GDEY075Z08 面板波形、VCOM 和掉电保护时间的经验参考，不能覆盖官方 DTM 定义。

## 2. 官方 OLD/NEW 定义

官方手册对两个数据传输命令的定义如下：

| 工作模式 | DTM1，命令 `0x10` | DTM2，命令 `0x13` |
| --- | --- | --- |
| KW 黑白模式 | OLD，刷新前画面 | NEW，目标画面 |
| KWR 黑白红模式 | K/W，黑白平面 | RED，红色平面 |

依据位置：

- `docs/datasheets/UC8179.txt:3240`：DTM1 在 KW 模式写 OLD，在 KWR 模式写 K/W。
- `docs/datasheets/UC8179.txt:3408`：DTM2 在 KW 模式写 NEW，在 KWR 模式写 RED。
- datasheet 的 DTM1、DTM2 命令章节分别对应 `0x10`、`0x13`。

因此，“DTM1 是 OLD 还是黑白层”取决于 PSR 的 KW/R 模式，不能脱离模式单独判断。本次局刷固定使用 `PSR=0x3F`，即 KW + register LUT，所以唯一正确的差分写入是：

```text
DTM1 (0x10) <- 完整 OLD 位图
DTM2 (0x13) <- 完整 NEW 位图
```

`feature` 分支的单色快速刷新路径曾使用 DTM2=PREVIOUS、DTM1=CURRENT。该顺序与本地 UC8179c C0.6 datasheet 的 KW 定义相反，本次没有照搬；只复用了其中的六组完整 LUT 和面板相关参数。

## 3. 寄存器调整

### 3.1 实际值发生变化的寄存器

| 寄存器 | 原 `dev` 配置 | 当前配置 | 调整原因 |
| --- | --- | --- | --- |
| PWR `0x01` | 局刷使用 `07 07 3F 3F`，只有 4 个参数；全刷未统一写 PWR | `07 17 3F 3F 03`，5 个参数 | C0.6 的 PWR 是 5 参数寄存器；第 2 参数 bit4 `VCOM_SLEW` 固定为 1，并补回第 5 参数。全刷、局刷共用同一配置函数。 |
| CDI `0x50`，BWR 全刷 | 单字节 `57` | `13 07` | 统一为 KWR OTP 全刷配置，并始终发送 data interval 第 2 字节。 |
| CDI `0x50`，BW 全刷 | 单字节 `97` | `93 07` | 使用完整两字节格式；保留 BW 全刷数据极性配置。 |
| CDI `0x50`，KW 差分局刷 | 初始化为 `11 07`，实际差分写入/刷新前又改为 `A9 07` | `A1 07` | 统一使用显式 OLD/NEW 的 `DDX=01`；把 `N2OCP`（bit3）从 1 改为 0，禁止刷新后自动把 NEW 复制到 OLD。软件只在物理刷新成功后推进 OLD 状态。 |
| VDCS `0x82`，局刷 | `22` | `30` | 采用 `feature`/GDEY075Z08 参考实现的面板 VCOM 值；这是面板经验参数，不是 datasheet 对所有面板的强制值。 |
| LUT `0x20`～`0x25` | 每表声明 42 字节，但只有首个 6 字节状态被显式初始化，其余全为 0；使用实验时序 `T2_PART=0x2A` | 六张完整 42 字节表，每表为 7 个状态 × 6 字节 | 按 datasheet 的 KW register-LUT 结构写入完整 LUTC、LUTWW、LUTKW、LUTWK、LUTKK 和 border LUT；表值来自 `feature` 的 GDEY075Z08 方案。 |

特别说明：原代码中的 `0x2A` 不是“使用 0x2A 寄存器/LUT”。它只是旧表第一个 6 字节状态里的 `T2_PART` 时序值。UC8179 的寄存器 LUT 命令仍然是 `0x20`～`0x25`，本次已删除“6 字节有效数据 + 自动补零”的实验表结构。

当前六张 LUT 的对应关系为：

| 命令 | datasheet 名称 | 当前表 |
| --- | --- | --- |
| `0x20` | LUTC | HOLD |
| `0x21` | LUTWW | HOLD |
| `0x22` | LUTKW，Black → White | KW |
| `0x23` | LUTWK，White → Black | WK |
| `0x24` | LUTKK | HOLD |
| `0x25` | Border LUT | 全 0 |

### 3.2 数值不变但配置方式已收敛的寄存器

| 寄存器 | 当前值/行为 | 本次变化 |
| --- | --- | --- |
| PSR `0x00` | BWR OTP 全刷 `0x0F`；BW OTP 全刷 `0x1F`；KW register-LUT 局刷 `0x3F` | 最终值不变，但局刷初始化直接进入 `0x3F`，全刷初始化和刷新都通过同一函数恢复正确模式，避免残留局刷状态。 |
| TRES `0x61` | 800 × 480 时仍为 `03 20 01 E0` | 从局刷中的硬编码改为由 `epd->width/height` 生成，并由全刷、局刷共享。 |
| DUSPI `0x15` | `00` | 用命名寄存器常量取代裸地址，并纳入统一时序配置。 |
| TCON `0x60` | `22` | 数值不变，纳入统一时序配置。 |
| VDCS `0x82`，全刷 | `00` | 每次全刷初始化和刷新前都明确恢复，防止局刷 `0x30` 泄漏到全刷。 |
| PTL `0x90` | 窗口坐标动态生成，`PT_SCAN=0` | 不再静默左移、扩展或裁剪请求；入口先严格校验，再发送调用者要求的精确窗口。 |

### 3.3 未加入的 feature 配置

`feature` 中使用过 `BTST 0x06 = 17 17 28 17`。当前 `dev` 继续依赖复位默认/现有升压配置，没有把该值直接合入。datasheet 定义了 BTST 格式，但不能据此证明该组面板参数对当前硬件必需；需在实屏上对启动稳定性、电流和低温表现做 A/B 验证后再决定。

## 4. LUT 来源与适用边界

datasheet 能确定以下内容：

- PSR 的 OTP/register LUT 选择和 KW/KWR 模式。
- KW 模式下 DTM1=OLD、DTM2=NEW。
- `0x20`～`0x25` 的 LUT 类型、每个状态的字段结构和 KW 使用 7 个状态。
- CDI 的 OLD/NEW、数据极性和 NEW-to-OLD copy 控制。

datasheet 不能替代面板厂给出一组对所有温度、批次都正确的波形。当前 LUT、`VDCS=0x30` 和 POF 后 100 ms 延时来自 `feature`/GDEY075Z08 的面板参考方案，属于“结构符合官方说明、参数需实屏确认”的配置。

## 5. 驱动结构和调用关系

### 5.1 全刷

```text
epd_gui_update
  -> epd_perform_full_refresh
     -> drv->init / UC8179_Init
        -> reset + BUSY
        -> ConfigureFull
           -> PWR + TRES + DUSPI + TCON
           -> PSR(OTP) + VDCS(0) + CDI(full)
     -> DrawGUI
        -> drv->write_image / UC8179_Write_Image
           -> DTM1=K/W，DTM2=RED（BWR）
     -> drv->refresh / UC8179_Refresh
        -> 再次 ConfigureFull，清除任何局刷寄存器残留
        -> PON -> DRF -> BUSY -> POF
```

全刷 DRF 不再额外设置一个“全屏 PTL”。图像写 RAM 时仍可用 PTIN/PTL 做窗口寻址，但真正显示刷新使用 OTP 全刷配置。

### 5.2 真 OLD/NEW 局刷

```text
epd_gui_update
  -> drv->init_partial / UC8179_Init_Partial
     -> reset + BUSY
     -> PWR/TRES/DUSPI/TCON
     -> PSR=0x3F + VDCS=0x30 + CDI=A1 07
     -> 写入 0x20..0x25 六张 42-byte LUT
     -> 保持高压关闭
  -> epd_clock_diff_try_partial
     -> 从 UI render cache 找唯一的普通 TIME_7SEG 组件
     -> 渲染完整 OLD HH:MM 和完整 NEW HH:MM
     -> 按共享 GFX buffer 容量分 tile
     -> 每个 tile 调用 diff_push
        -> PTIN -> PTL
        -> DTM1 <- OLD tile
        -> DTM2 <- NEW tile
        -> PTOUT
     -> 所有 tile 写完后只调用一次 refresh_partial
        -> PON -> PTIN -> PTL -> DRF -> BUSY -> PTOUT -> POF
     -> 仅刷新成功后把软件缓存推进到 NEW 时间
```

高压只覆盖实际 DRF 阶段。初始化、LUT 装载和 OLD/NEW SRAM 写入阶段均不 PON。PON 失败也会执行 POF 收尾；POF 后保留 100 ms 放电保护，原实现为 500 ms。

### 5.3 局刷窗口约束

公共局刷入口现在同时要求：

- `x` 和 `w` 为 8 像素对齐；
- `w >= 16`，保证 datasheet 的 `HRED > HRST`；
- `h >= 2`，保证 `VRED > VRST`；
- 窗口完全位于面板范围内；
- `PT_SCAN=0`，只扫描窗口内部。

不合法窗口直接返回 `false`，不再由底层偷偷改变请求范围。调用方必须先完成对齐和边界计算。

## 6. 时钟差分状态调整

原状态只保存四个数字，新状态保存 `{mode, hour, minute, valid}`，并形成以下一致性约束：

- `MODE_CLOCK` 和 `MODE_CLOCK_CALENDAR` 都支持真差分局刷。
- 默认 `MODE_CLOCK_CALENDAR` 同时有普通时间组件和 `UI_FLAG_PARTIAL_ONLY` 镜像；查找时忽略镜像，否则会被误判成重复时间组件并永远回退全刷。
- 每次局刷渲染完整 OLD/NEW `HH:MM`，包括未变化数字和冒号，避免多个小窗口之间出现边界遗漏。
- 2560 字节共享 GFX buffer 被拆成 OLD/NEW 两个平面并按高度分 tile，不再假设整个区域能一次放入内存。
- 同一轮写完所有 tile 后只触发一次物理刷新。
- `diff_push` 或 `refresh_partial` 任一失败，状态不推进，并使调用方回退到统一全刷路径。
- 全刷成功且实际显示的是正常电压下的时钟页时才 seed 差分状态。
- 低电量提示页、资源缺失页、模式切换或失败路径会 invalidate OLD 状态。
- 默认 `MODE_CLOCK` 模板也包含日期/农历/星期/节气，所以跨天与 `MODE_CLOCK_CALENDAR` 一样强制全刷。

默认布局的内存分块结果：

| 模式 | 时间缩放 | 差分区域 | 每行字节 | 每 tile 行数 | tile 数 | OLD+NEW 最大占用 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `MODE_CLOCK_CALENDAR` | 6 | 288 × 140 | 36 | 35 | 4 | 2520 B |
| `MODE_CLOCK` | 10 | 480 × 220 | 60 | 21 | 11 | 2520 B |

两种默认布局均不超过 2560 B 共享缓冲区。

## 7. 服务层回退关系

全刷逻辑已收敛为 `epd_perform_full_refresh()` 一个入口。以下情况不会跳进某个旧分支或继续使用未知 OLD，而是自然落入该入口：

- 首次显示、强制全刷或到达维护全刷周期；
- 局刷初始化 BUSY 超时；
- UI 模板不满足差分条件；
- tile 写入失败、窗口无效或物理局刷失败；
- 低电量或非时钟模式。

全刷失败会设置 `force_full_refresh=true` 并使差分状态失效；只有全刷物理成功后才更新 `last_full_clear`、清除强制标志和 seed OLD 状态。

## 8. 删除的遗留路径

本次删除或收敛了以下不再需要的代码：

- 用 `~NEW` 伪造 OLD 的大窗口局刷路径；
- guarded invert、strong-BW 等实验分支及 `UC8179_Set_Partial_Strong_BW_Allowed()`；
- `clear_partial`、`write_image_partial` 两个旧 vtable 接口；
- 未使用的 OTP dump、native RAM 辅助函数；
- 两套相同 UC8179 model ID 的全刷/局刷驱动对象；当前只保留一套同时提供全刷和真差分局刷的 model；
- 服务层标签/goto 形式的局刷失败回退和重复全刷流程。

局刷 vtable 现在只保留：

```c
void (*init_partial)(epd_model_t *epd);
bool (*diff_push)(epd_model_t *epd, uint16_t x, uint16_t y,
                  uint16_t w, uint16_t h,
                  const uint8_t *old_bm, const uint8_t *new_bm);
bool (*refresh_partial)(epd_model_t *epd, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h);
```

两个会失败的操作返回 `bool`，使服务层能够基于真实结果决定是否推进状态或回退全刷。

## 9. 涉及文件

- `EPD/UC8179.c`：寄存器、LUT、DTM 数据顺序、电源生命周期、严格 PTL、统一全刷/局刷实现。
- `EPD/UC8179.h`：只暴露当前需要的局刷和 BUSY 状态接口。
- `EPD/EPD_driver.h`：明确 DTM 在 KW/KWR 两种模式下的语义，精简局刷 vtable。
- `EPD/EPD_driver.c`：只注册一套 UC8179 model。
- `EPD/EPD_service.c`：统一全刷入口、局刷失败回退、状态 seed/invalidate、两个时钟模式的跨天处理。
- `EPD/epd_clock_diff.c`、`EPD/epd_clock_diff.h`：完整 HH:MM OLD/NEW 渲染、tile 传输和事务式状态推进。
- `tests/uc8179_command_sequence_test.c`：UC8179 命令序列和寄存器契约测试。
- `tests/epd_clock_diff_test.c`：时钟差分布局、分块、状态失败语义测试。
- `tests/stubs/`、`tests/Makefile`、`tests/README.md`：主机测试桩和运行入口。

## 10. 验证结果

### 10.1 主机命令契约测试

```text
$ make -C tests clean test
PASS: UC8179 official partial-refresh command contract
PASS: clock differential tiling and OLD-state contract
```

覆盖范围包括：PWR 五参数、PSR、CDI 两参数、VDCS、TRES/DUSPI/TCON、六张 LUT 的长度和顺序、DTM1=OLD/DTM2=NEW、PTL、PON/DRF/PTOUT/POF、全刷恢复 VDCS、BUSY 失败收尾、非法边界、vtable、两种默认时钟布局、partial-only 镜像和刷新失败不推进状态。

### 10.2 nRF52 固件编译

```text
$ CCACHE_DISABLE=1 make -f Makefile.nRF52_linux build
PASS
text=49576, data=276, bss=7048, total=56900
Flash: 49852 / 57344，剩余 7492 B
RAM static: 7324 B；含 stack 预留共 9372 / 15656，剩余 6284 B
```

相对本次修改前的本地基线记录，Flash 余量增加约 976 B，RAM 余量增加 12 B。

### 10.3 GUI golden test

```text
$ make -f tools/test/Makefile.golden test
GUI golden: CRC32=0x8678EADB OK (800x480 calendar BWR)
```

### 10.4 差异格式检查

```text
$ git diff --check
PASS
```

## 11. 实屏验证清单

自动测试确认的是寄存器和命令契约，不能替代电子纸物理效果验证。合入或量产前至少检查：

1. 连续 100 次以上分钟更新的残影累积和功耗。
2. `xx:59 -> yy:00` 的小时跨越，以及 `23:59 -> 00:00` 的跨天全刷。
3. 黑→白擦除能力、白→黑浓度、冒号和未变化数字是否稳定。
4. 红色颗粒、窗口边界红线、水平串扰和 border 状态。
5. 低电量页、资源缺失页、恢复正常页之后的第一次刷新是否强制全刷。
6. `MODE_CLOCK`、`MODE_CLOCK_CALENDAR` 和其他模式来回切换后的 OLD 状态。
7. 低温、常温、高温下的刷新时间、残影和 BUSY 超时。
8. PON/POF 电流曲线和 POF 后 100 ms 是否足够放电。
9. 当前默认升压配置与 `BTST=17 17 28 17` 的 A/B 对比。
10. 不同面板批次是否需要按温度分档 LUT；当前实现使用固定 GDEY075Z08 波形。

## 12. 后续必须保持的约束

- KW 局刷永远是 DTM1=OLD、DTM2=NEW；KWR 全刷才是 DTM1=K/W、DTM2=RED。
- 不允许用 `~NEW`、全白或其他推导数据冒充未知 OLD。
- 软件 OLD 只在确认物理刷新成功后推进。
- SRAM 写入阶段不打开高压；PON/POF 必须围绕实际 DRF 成对出现。
- 局刷失败必须回退全刷，不能继续保留不可信差分状态。
- 每次全刷都要恢复 PSR、VDCS、CDI 和公共电源/时序寄存器。
- 任何新 LUT 都必须写满 datasheet 要求的表结构，并标明面板和温区验证来源。
- 旧分析文档若与本记录的 DTM 方向冲突，以 UC8179c C0.6 datasheet 和本记录为准。
