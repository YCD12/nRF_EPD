**设备端 Flash 优化实施（2026-10-17）**

已按 [前期评审](device-flash-review-20261017.md) 落实优化并修复条件时钟局刷问题。应用 BIN 从 **57,044 B 降至 56,340 B，减少 704 B（约 1.23%）**；56 KiB 应用区余量由 **300 B 增为 1,004 B**。静态 RAM 最终保持 **9,512 B**。

改动在原有暂存代码之上，保留原暂存区，没有提交 Git、刷写设备或生成签名发布包。UIR2/字体/资源协议和内外 Flash 分区没有改变，已有资源不需要为本轮优化重新生成。

**已落地的改动**

1. **UI 校验范围表去重。** 118 组范围用 27 组共享上下界和索引表示，全部字段范围保持一致；同步修改 `tools/ui_resource_codec.py` 与生成头文件。网页 schema 和默认载荷未变，生成器检查通过。新增测试把生成表展开后逐项对照 JSON schema。
2. **UC8179 传输循环合并。** OLD/NEW 窗口复用 `UC8179_Write_Image_Rows()`，保留 DTM1/DTM2 命令顺序、64 B 分块和喂狗频率。
3. **GUI 裁剪统一。** 删除独立的 `m_todo_clip` 与字体回调包装，组件、待办行和删除线统一使用 GFX clip。保留负 bearing、双行、省略、斜体与分页处理。
4. **条件时钟局刷修复。** `ui_resources_clock_geom()` 对带条件标志的时钟禁用局刷，让服务走完整绘制。这样不会把完整场景未绘制的旧时钟误作 OLD。分屏/全屏、同步条件及两种节气条件均有新增回归：旧代码先复现失败，修复后通过；服务级测试确认不会进入 OLD/NEW 渲染。普通时钟继续允许局刷。
5. **时钟几何与参数读取统一。** 初始化验证、局刷几何和显示共用 `ui_resource_clock_layout()`；初始化验证复用已读参数，场景将 style 直接交给时钟/待办控件，减少重复 SPI 读取。ClockOnly 独立路径继续读取自身参数。本项主要改善结构和读取次数，未单独带来 Flash 收益。
6. **资源验证入口合并。** 新增 `resource_store_open_info()`，一次查分区、读头并验证完整 payload 后返回 revision/length/CRC。字体、UI、持久记录与协议的 verified metadata 查询改用该入口。原 `resource_store_info()` 仍只验证头，原 `resource_store_open()` 保留兼容接口。CRC、长度、并发写入拒绝和失败缓存失效语义保留。
7. **队列投递公共逻辑合并。** 新增 `EPD/epd_work_queue.h`，命令与显示队列共用上下文投递、队列满后的 scheduled 回滚。保留两类业务队列、临界区、IRQ 请求、ACK 背压及 SDK scheduler 的执行顺序。

`main.c` 无需改写：本轮调度优化落在它调用的服务层。没有把业务 worker 直接搬进主循环。直接调用会涉及“每轮处理几个命令、何时可以休眠、ACK 阻塞时怎样避免空转”的行为变化；保留既有执行顺序的公共投递合并已经获得实测收益，且由现有并发测试覆盖。

**逐阶段生产构建**

使用项目 GCC 9.3.1、`-Os -flto --gc-sections`，固定 800×480 屏幕、外置字体、日志关闭。所有阶段在独立输出目录构建，未覆盖原 `_build/`。

| 阶段 | 应用 BIN | 相对上阶段 | 剩余 Flash |
| --- | ---: | ---: | ---: |
| 优化前 | 57,044 B | — | 300 B |
| 范围表、裁剪、UC8179 合并 | 56,484 B | −560 B | 860 B |
| 条件时钟修复 | 56,484 B | 0 B | 860 B |
| 时钟几何与参数读取统一 | 56,484 B | 0 B | 860 B |
| 资源读取/验证合并 | 56,372 B | −112 B | 972 B |
| 队列投递合并，最终版本 | **56,340 B** | **−32 B** | **1,004 B** |

最终 ELF `size`：text 56,128 B、data 212 B、bss 9,300 B。最初裁剪合并减少 8 B 静态 RAM，随后几何重构的链接结果增加 8 B，最终与优化前一致；不把阶段性节省算作最终收益。静态 RAM 加 2,048 B 栈预留共 11,560 B，15,656 B RAM 预算还余 4,096 B，实际栈峰值未测。

**验证结果**

- `tests/run_host_tests.sh` 返回 `HOST_OK`：34 组 C 主机回归及可运行的 Python 跨层检查通过，覆盖资源存储、CRC/掉电模拟、字体、普通/固定 GFX、UC8179、服务生命周期、热加载及待办调度。
- 新增 verified metadata 回归确认：比独立 info+open 读取次数更少；热缓存也不能绕过 payload CRC；I/O/CRC/BUSY 失败清零输出，失败后不保留旧读取缓存。
- 最后变更的队列代码单独重跑命令队列和异步队列测试，通过 IRQ 所有权、重复投递、调度队列满重试与请求合并检查。服务/待办流水线检查覆盖 ACK 优先、超时重试、资源擦除恢复和跨日。
- 对最终源码，复用前次保存的 25 组真实外置资源及网页参考像素，普通/固定配置共 **50 次逐像素一致**。本次环境没有 Node，主机脚本明确跳过动态浏览器字体 ABI、重新生成网页像素样本及 JS 测试；没有据此声称完成浏览器全量验证。
- 资源工具 14 项 Python 测试通过；UI 生成器 `--check`、`git diff --check` 通过。最终重建的 BIN 与最后阶段构建 SHA-256 一致。
- 原暂存区二进制 diff 前后 SHA-256 一致，原有暂存内容没有被覆盖。

物理屏幕耗时、BLE 空口压力、OTA 安装和实际栈峰值仍需硬件验证。裁剪统一后，屏外字形扫描线由 GFX 的逐像素边界检查拒绝；像素已验证，不能将主机正确性测试等同于实机耗时测量。

**产物**

- [最终应用 HEX](../output/device-flash-implementation-20261017/final/nrf52811_xxaa.hex)、[BIN](../output/device-flash-implementation-20261017/final/nrf52811_xxaa.bin)：仅应用，不含 SoftDevice/Bootloader/settings，不是签名 OTA 包。
- [各阶段测量](../output/device-flash-implementation-20261017/measurements.json)、[最终大小与哈希](../output/device-flash-implementation-20261017/final/size.json)。
- [主机回归日志](../output/device-flash-implementation-20261017/host.log)、[像素核对日志](../output/device-flash-implementation-20261017/pixels.log)、[像素核对脚本](../output/device-flash-implementation-20261017/verify_pixels.py)。

最终 BIN SHA-256：`8f02b6f74c1af1ac5775df51fcd0434483a8665d301d3b7b649d3905295b7578`。
