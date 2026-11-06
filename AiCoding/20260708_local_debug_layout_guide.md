# nRF52811 本地调试免空间限制（84KB APP）内存布局指南

为了在本地调试期间能够无限制地开启 **SEGGER RTT Log** 和 **SEGGER SystemView** 进行实时性能分析，同时准备开发完整的**声明式通用规则引擎 (ECA)**，本指南提供了一种通过**屏蔽 Bootloader** 释放片内物理 Flash 空间的方案。

---

## 1. 内存空间布局对比

### 1.1 正常生产/OTA 布局 (56KB APP)
适用于线上 DFU 升级场景：
* **MBR**: `0x00000 - 0x01000` (4KB)
* **SoftDevice S112**: `0x01000 - 0x19000` (96KB)
* **Application (APP)**: `0x19000 - 0x27000` (56KB, `LENGTH = 0xE000`)
* **FDS (片内)**: `0x27000 - 0x29000` (8KB)
* **DFU Bootloader**: `0x29000 - 0x2E000` (20KB)
* **MBR Params**: `0x2E000 - 0x2F000` (4KB)
* **BL Settings**: `0x2F000 - 0x30000` (4KB)

### 1.2 本地调试布局 (84KB APP)
适用于 J-Link / J-Flash 本地整片擦写调试场景：
* **MBR**: `0x00000 - 0x01000` (4KB)
* **SoftDevice S112**: `0x01000 - 0x19000` (96KB)
* **Application (APP)**: `0x19000 - 0x2E000` (**84KB**, `LENGTH = 0x15000`)
* **FDS (自动平移)**: `0x2E000 - 0x30000` (8KB)
* **DFU Bootloader**: 无 (Disabled)

---

## 2. 方案实现原理

1. **自动平移 FDS 避让**：
   在 SDK 的 `fds.c` 中，FDS 结束地址通过运行时读取 `BOOTLOADER_ADDRESS` 来确定。如果通过 J-Link 擦除芯片使 UICR 的 Bootloader 寄存器（`0xFF8`）变为 `0xFFFFFFFF`，SDK 会将 Flash 终点识别为物理最大值 `0x30000` (192KB)。
   因此，FDS 会自动向后平移至最后的 `0x2E000` ~ `0x30000` 空间，为 APP 留出了从 `0x19000` 到 `0x2E000` 完整的 84KB 连续空间。
2. **免除了修改 SDK 源码的成本**：
   通过彻底擦除 UICR，我们不需要在 SDK 层的 `fds.c` 中 Hardcode 任何绝对地址，保持了第三方 SDK 的整洁与可维护性。

## 3. 编译脚本集成

项目通过分离链接脚本并集成 Makefile 条件判断，实现了生产与调试的动态切换，无需手动修改链接脚本。

### 3.1 调试用链接脚本
新建了专用链接脚本：[gcc_nrf52_debug.ld](file:///home/tal/Test/project/he-ma_n-rf_-epd/SDK/17.1.0_ddde560/gcc_nrf52_debug.ld#L11)，设置其 `LENGTH = 0x15000` (84KB)。  
原链接脚本：[gcc_nrf52.ld](file:///home/tal/Test/project/he-ma_n-rf_-epd/SDK/17.1.0_ddde560/gcc_nrf52.ld#L11) 还原为生产配置的 `LENGTH = 0xE000` (56KB)。

### 3.2 Makefile 支持
在 [Makefile.nRF52_linux](file:///home/tal/Test/project/he-ma_n-rf_-epd/Makefile.nRF52_linux#L36) 中，集成了 `DEBUG_LAYOUT=1` 变量。当传入此变量时，会自动加载调试用链接脚本：
```makefile
ifeq ($(DEBUG_LAYOUT),1)
$(OUTPUT_DIRECTORY)/nrf52811_xxaa.out: \
  LINKER_SCRIPT  := $(SDK_ROOT)/gcc_nrf52_debug.ld
else
$(OUTPUT_DIRECTORY)/nrf52811_xxaa.out: \
  LINKER_SCRIPT  := $(SDK_ROOT)/gcc_nrf52.ld
endif
```

---

## 4. 调试烧录脚本：`flash_debug.sh`

调试期间可直接运行 [flash_debug.sh](file:///home/tal/Test/project/he-ma_n-rf_-epd/flash_debug.sh) 进行一键编译与刷写。该脚本会自动：
1. 传入 `DEBUG_LAYOUT=1` 编译 84KB 调试版固件；
2. 擦除整颗芯片（包含 UICR，清除 `0xFF8`）；
3. 烧录 S112 SoftDevice；
4. 烧录 84KB Application；
5. 重置设备。

```bash
# 默认一键刷入调试包 (包含 SystemView 实时性能分析)
./flash_debug.sh

# 可选：关闭 SystemView 实时分析，仅进行 84KB 大空间调试
./flash_debug.sh USE_SYSVIEW=0
```

---

## 5. 生产烧录脚本：`flash.sh`

当不再需要进行大空间调试，恢复出厂 DFU 功能测试时，重新切回 `flash.sh` 即可：
```bash
# 一键编译标准 56KB 固件，并合并 SoftDevice + Bootloader + Settings 写入芯片
./flash.sh
```

---

## 6. Keil (uVision) 开发集成指南

如果在 Windows 环境下使用 Keil MDK 进行本地编译与 J-Link 调试，需要手动配置 Keil 工程选项以生效等价的 84KB 免空间限制调试布局：

### 6.1 修改 IROM1 大小 (IROM1 Size = 84KB)
Keil 使用魔术棒的 Target 配置控制编译边界：
1. 点击 Keil 工具栏的 **Options for Target**（魔术棒图标）。
2. 切换到 **Target** 选项卡。
3. 找到右下角的 **On-chip ROM Areas** 里的 **IROM1**，修改大小：
   * **Start Address**: 保持 `0x19000`
   * **Size**: 修改为 `0x15000`（十六进制，代表 84KB）
   *(若切回生产模式，将 Size 改为 `0xE000`，即 56KB)*。

### 6.2 启用“全片擦除” (Erase Full Chip)
为了在烧录时清空 UICR 启动跳转地址（从而自动屏蔽 DFU Bootloader，并使得 FDS 自动后移）：
1. 在 **Options for Target** 中，切换到 **Utilities** 选项卡。
2. 在 **Use Target Driver for Flash Programming** 右侧点击 **Settings** 按钮。
3. 切换到 **Flash Download** 选项卡。
4. 将 **Download Function** 由默认的 *Erase Sectors* 修改为 **Erase Full Chip**，点击确定保存。

### 6.3 启用 RTT / SystemView 宏与源码
1. **Define 宏定义**：切换到 **C/C++** 选项卡，在 **Define** 框末尾追加 `USE_SYSVIEW`。
2. **Include Paths 头文件引用**：添加 `..\SystemView`。
3. **添加源文件**：
   * 在 Keil Project 树中新建 Group：`SystemView`。
   * 将 [SystemView/](file:///home/tal/Test/project/he-ma_n-rf_-epd/SystemView) 目录下的 `SEGGER_SYSVIEW.c`、`SEGGER_SYSVIEW_Config_NoOS.c`、`SEGGER_RTT.c`、`SEGGER_RTT_printf.c` 添加进工程中编译。
   * 注意：将原本在 `nRF_Segger_RTT` Group 中的旧版 `SEGGER_RTT.c` 移除，避免符号重复定义。

