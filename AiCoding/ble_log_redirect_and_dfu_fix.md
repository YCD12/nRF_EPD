# nRF52811 BLE 日志重定向与 DFU 固件裁剪修复说明书

本说明文档整理了关于 nRF52811 + SoftDevice S112 平台下，为了实现“通过蓝牙重定向输出 `NRF_LOG` 日志至网页端”功能所进行的设计、遇到的致命 Bug（DFU 升级后不工作）的根因分析、以及为了关闭 LTO 避开该 Bug 所进行的高强度固件体积裁剪和安全优化。

---

## 一、 背景与功能设计

### 1. 痛点与需求
在没有硬件调试器（RTT/UART）的场景下，分析 E-Paper 显示状态或进行现场 Debug 非常困难。因此，需要设计一种将系统所有 `NRF_LOG` 日志重定向并通过 BLE（蓝牙）发送给网页端（Web Bluetooth API）显示的功能。

### 2. BLE 重定向设计方案
*   **重定向入口 (nrf_log.h)**：通过定义 `BLE_LOG_REDIRECT_ALL_NRF_LOG` 宏，在编译前将标准的 `NRF_LOG_INFO`、`NRF_LOG_WARNING`、`NRF_LOG_ERROR` 和十六进制打印 `NRF_LOG_HEXDUMP_*` 用我们自定义的 `ble_log` 和 `ble_log_hexdump` 覆盖重写。
*   **轻量化字符串格式化**：由于标准 `vsnprintf` 会引入大量的标准库开销，在 `main.c` 中实现了一个轻量级的 `mini_vsnprintf` 和 `long_to_str`，支持 `%d`、`%u`、`%x`、`%X`、`%s`、`%c` 以及修饰符 `%ld`/`%lu`/`%lx`，可覆盖全部的 NRF_LOG 格式化场景。
*   **BLE 传输通道**：格式化后的日志直接通过 EPD 服务的 Notification 传输，调用 `ble_epd_string_send` 发送，实现无线日志实时查阅。

---

## 二、 致命 Bug 分析：DFU 升级后不工作

### 1. 故障现象
启用蓝牙日志重定向后，由于代码体积增加，编译时 Flash 溢出。为了缩减体积，开启了 GCC 编译器的 `-flto` (Link-Time Optimization, 链接时间优化)。
然而，固件升级后出现：**设备无蓝牙广播，屏幕完全不刷新，系统卡死或无响应**。

### 2. 根因分析
Nordic SDK 采用了特殊的段注册机制（Section Variables）来注册各种系统观察者（Observer）。
以蓝牙协议栈事件为例，观察者宏的展开逻辑类似：
```c
#define NRF_SDH_BLE_OBSERVER(_name, _prio, _handler, _context) \
    STATIC_ASSERT(...) \
    const nrf_sdh_ble_evt_observer_t _name \
    __attribute__((section(".sdh_ble_observers"))) \
    __attribute__((used)) = ...
```
在正常的编译与链接期，链接脚本会在 `.sdh_ble_observers` 段中自动排列这些结构体，并通过全局段起始/结束符号（`__start_sdh_ble_observers` / `__stop_sdh_ble_observers`）在运行时遍历和分发蓝牙事件。

当开启 **`-flto`（链接时间优化）** 时，GCC 会进行全程序流图分析。由于这些观察者结构体在代码中**没有被显式地直接引用**（它们仅通过链接器段隐式连接），GCC 的 LTO 机制会判定它们为死代码（Dead Code），从而在最终阶段**直接将其剔除（Discard）**。
导致的结果是：
*   `.sdh_ble_observers` 段变为空（大小为 0）。
*   蓝牙协议栈事件（如连接、断开、广播超时、配对）无法分发到应用程序的回调函数中。
*   设备启动后，蓝牙协议栈实际上瘫痪，导致**无蓝牙广播，DFU 服务失效，屏幕定时刷新无法触发**。

### 3. 根本解法
**彻底禁用 `-flto`，保证静态链接段的完整性**。
通过 `arm-none-eabi-objdump -t` 导出符号验证，禁用 LTO 后，`.sdh_ble_observers` 中的符号如 `m_ble_observer`、`m_epd_obs`、`m_gatt_obs`、`m_dfus_obs` 等核心观察者均被正确保留，蓝牙功能随之恢复正常。

---

## 三、 Flash 深度裁剪与优化（无 LTO 约束下）

由于禁用了 LTO，固件体积瞬间突破了 nRF52811 对应用层限制的 **56KB (57,344 字节)**。
为了既保证蓝牙正常工作，又能把固件塞进 56KB 空间，我们实施了高强度的代码与常量精简：

### 1. 调试级日志剔除
在重定向配置中，将 `NRF_LOG_DEBUG` 和 `NRF_LOG_HEXDUMP_DEBUG` 宏强制定义为空：
```c
#define NRF_LOG_DEBUG(...)           do { } while(0)
#define NRF_LOG_HEXDUMP_DEBUG(p, l)  do { } while(0)
```
这在编译期完全移除了底层耗费空间的各种调试级别字符串（如 `UC8179` 驱动层及 SPI 读写底层的 verbose 打印），**直接释放了约 1,840 字节**。

### 2. DFU 与核心层日志精简
将核心逻辑中冗长的只读字符串进行了极限缩短，避免占用 Flash 的 `.rodata` 段：
*   `main.c` 里的 DFU 状态说明精简：
    *   `"Device is preparing to enter bootloader mode."` $\rightarrow$ `"Prep DFU"`
    *   `"Device will enter bootloader mode."` $\rightarrow$ `"Enter DFU"`
    *   `"Request to enter bootloader mode failed asynchroneously."` $\rightarrow$ `"DFU fail async"`
    *   `"Request to send a response to client failed."` $\rightarrow$ `"Resp fail"`
    *   `"Unknown event from ble_dfu_buttonless."` $\rightarrow$ `"DFU unknown evt"`
*   `EPD_service.c` 里的频繁模式切换日志精简：
    *   `"[EPD]: Switching from clock mode to PICTURE mode for clear"` $\rightarrow$ `"Switch mode: clear"`
    *   同理，精简了 cmd/data/img/block 相关的模式转换提示信息。
*   通过精简字符串，**进一步节省了近 400 字节**。

### 3. 栈资源优化
在 RAM 仅 15KB 的 nRF52811 上，为了防止在 EPD 高级 GUI 刷新等深层函数调用链中发生栈溢出：
*   将 `long_to_str` 转换所需的栈缓存由 `buf[32]` 减小到 `buf[12]`。
*   将 `ble_log` 格式化缓存从 `buf[128]` 精简为 `buf[80]`。
*   将 `ble_log_hexdump` 格式化缓存从 `buf[128]` 精简为 `buf[64]`。
*   共节省约 **132 字节栈深度**。

### 4. 关键 Bug 修复：防 Dangling-else 与重入
*   **防重入机制**：`ble_log` 内部增加了基于 `static volatile bool in_ble_log` 的防重入互斥锁。如果在 SoftDevice 处于中断上下文中触发了重定向日志，该锁能有效阻止嵌套发起 SoftDevice 的 SVC 呼叫（如 `sd_ble_gatts_hvx`），规避了系统的硬 fault 风险。
*   **防 Dangling-else**：重定向宏包裹由 `if (1) { ... }` 规范化重构为 `do { ... } while(0)`，确保在各种无括号 `if-else` 条件控制流分支下语义完全正确。

---

## 四、 编译与大小现状

在还原 `gcc_nrf52.ld` 硬限制：`FLASH (rx) : ORIGIN = 0x19000, LENGTH = 0xe000` (56KB Limit) 后，执行 `make -f Makefile.nRF52_linux rebuild` 的产物指标如下：

*   **.text (代码及只读常量段)**：56,372 字节
*   **.data (初始化数据段)**：440 字节
*   **Flash 物理占用总计**：**56,812 字节** 
*   **当前安全冗余空间**：**532 字节**

### 总结
目前固件在**关闭 LTO** 的保障下，通过代码重构和精简，成功降落在 56KB 警戒线以内。蓝牙观察者表完好无损，DFU 升级安全顺畅，红蓝双色墨水屏以及时钟模式刷新均能够完全恢复正常工作。

## 五、 BLE 日志重定向 与 RTT View 调试切换指南

为了确保团队中不同开发者使用不同工具链（GCC / Keil）以及调试方法时互不干扰，以下是切换这两种调试日志模式的具体操作步骤指南：

### 1. 模式一：BLE 蓝牙日志重定向模式（默认发布/DFU 模式）

在此模式下，系统所有的 `NRF_LOG_INFO`、`NRF_LOG_WARNING`、`NRF_LOG_ERROR` 以及 `NRF_LOG_HEXDUMP_*` 都会重定向到蓝牙通道发送。
*   **适用场景**：日常发布、空中 DFU 升级、非连接调试器调试（网页端无线看日志）。
*   **空间占用**：约 56.4KB (安全控制在 56KB 阈值内)。

#### 💻 GCC 工具链切换步骤：
1.  打开 `Makefile.nRF52_linux`，确保文件顶部的 `USE_RTT` 被设为 `0`（或保持缺省配置）：
    ```makefile
    USE_RTT ?= 0
    ```
2.  在终端执行全量重新构建：
    ```bash
    make -f Makefile.nRF52_linux rebuild
    ```
3.  系统会自动注入 `-DBLE_LOG_REDIRECT_ALL_NRF_LOG=1`（仅对应用层代码及 EPD 驱动有效），同时剔除全部 RTT 底层源文件编译，生成小于 56KB 的安全 DFU 固件。

#### 🛠️ Keil MDK 编译步骤：
*   **默认配置**：直接点击 Keil 的 Build 即可，默认即为 BLE 重定向模式（因为 `EPD_config.h` 中默认将 `BLE_LOG_REDIRECT_ALL_NRF_LOG` 设为了 `1`）。
*   **网页接收**：设备通过蓝牙与网页连接后，开启 EPD 服务的 Notification，即可在网页端接收实时运行日志。

---

### 2. 模式二：SEGGER RTT J-Link 调试模式

在此模式下，完全关闭蓝牙重定向功能，将日志打印恢复为原汁原味的 RTT J-Link 传输，可通过 J-Link RTT Viewer / RTT Client 实时查看。
*   **适用场景**：本地有线烧录调试、协议栈底层行为排查、断连瞬间排查。
*   **空间占用**：引入了 `fprintf` 格式化以及 RTT 缓冲区，体积会暴涨约 **4.2KB**，这会直接超出 56KB 的 Flash 物理上限导致 GCC 链接器报错。**所以该模式仅可用于本地有线烧录开发调试，不能用于生成最终发布的 DFU 固件**。

#### 💻 GCC 工具链切换步骤：
1.  打开 `Makefile.nRF52_linux`，将顶部的 `USE_RTT` 修改为 `1`：
    ```makefile
    USE_RTT ?= 1
    ```
    *(或者您也可以在命令行中通过传参直接编译：`make -f Makefile.nRF52_linux rebuild USE_RTT=1`)*
2.  为了能够顺利通过编译不被 56KB 空间限制拦截，需要临时将 `SDK/17.1.0_ddde560/gcc_nrf52.ld` 中的 Flash 长度增大：
    ```ld
    /* 临时将 LENGTH 改大（例如 0x12000），仅用于有线烧录调试 */
    FLASH (rx) : ORIGIN = 0x19000, LENGTH = 0x12000
    ```
3.  执行编译，并使用 J-Link 调试器将 `_build/nrf52811_xxaa.hex` 烧录进芯片。
4.  打开 `SEGGER RTT Viewer` 即可查看日志。

#### 🛠️ Keil MDK 编译步骤：
Keil 工程中默认已经打包编译了全部 RTT 源文件。当您在 Keil 中需要恢复 J-Link 打印时：
1.  进入 Keil 的项目配置：
    `Options for Target -> C/C++ -> Preprocessor Symbols -> Define`
2.  在预定义宏列表中添加：`BLE_LOG_REDIRECT_ALL_NRF_LOG=0`
3.  确保 `sdk_config.h` 中使能了原生 RTT（`NRF_LOG_ENABLED` 设为 `1`，`NRF_LOG_BACKEND_RTT_ENABLED` 设为 `1`）。
4.  点击编译并烧录，即可通过 RTT 正常读取日志。

---

### 3. 底层防错优化说明
*   **缺少分号容错**：在 SDK 部分源码（例如 `app_timer2.c` 的 191 行）中存在形如 `NRF_LOG_DEBUG("...")` 的不规范书写（末尾缺少分号）。为了在重定向状态下对其完美兼容，我们将 `nrf_log.h` 中的 `NRF_LOG_DEBUG(...)` 设为了完全的空宏定义（`#define NRF_LOG_DEBUG(...)`）。这样即使代码中漏写分号，在两种模式下均能正常通过 GCC 和 Keil 编译器，保障了代码的整洁与安全。
