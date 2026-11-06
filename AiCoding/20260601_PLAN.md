# FM25Q04B 外置 Flash 资源迁移计划

## Summary
- 目标：把不常变化、占内置 Flash 的只读资源搬到 FM25Q04B，降低 nRF52811 应用 ROM 占用，保证 S112 + Bootloader + 应用 DFU 空间安全。
- 当前应用链接区为 `0x19000` 起、`0x17000` 长度，即 92KB；当前 map 显示 ROM 约 37.7KB，但 GUI/字体/LUT 继续增长会很快吃掉余量。
- 外置 Flash 使用 SPI1：CS=P0.12、MISO=P0.13、SCK=P0.14、MOSI=P0.15；EPD 当前运行配置使用 SPI0，不与外置 Flash 冲突，但需避免 `EPD_driver.c` 默认 P0.12/P0.13 在配置加载前被误初始化。

## Key Changes
- 完善 `fm25q04b.c/.h` 为通用驱动，不再只是自测：
  - 增加 `fm25q04b_init/deinit/probe/read/write/erase_sector/erase_range/read_jedec/read_status/wait_ready/self_test`。
  - 支持 24-bit 地址、512KB 边界检查、4KB sector erase、256B page program 自动跨页写。
  - 去掉生产路径里的大量 RTT 调试；保留可选 `FM25Q04B_ENABLE_SELF_TEST`。
  - 返回 `ret_code_t`，错误统一映射为 `NRF_SUCCESS/NRF_ERROR_*`。
- 新增外置资源层：
  - 采用 BLE 资源包写入外置 Flash。
  - Flash 布局：`0x00000-0x00FFF` 主 manifest，`0x01000-0x7DFFF` 资源数据区，`0x7E000-0x7EFFF` 临时/staging manifest，`0x7F000-0x7FFFF` 仅调试自测 scratch。
  - manifest 包含 magic、version、resource_count、entry 表、CRC32；entry 包含 type/id/offset/length/crc32。
  - 资源校验失败时直接报错，不做完整 GUI 回退。
- 优先剥离资源：
  - 第一优先级：`GUI/fonts.c` 的 4 个 u8g2 字体，约 9.8KB，是最直接收益。
  - 第二优先级：`EPD/UC8179.c` 的 11 档局刷 LUT，约 3KB；运行时按温度从外置 Flash 读 6 组 42B 到小 RAM buffer 后写寄存器。
  - 第三优先级：`GUI/Lunar.c` 的农历/节气字符串和表，体积较小但稳定，可作为后续资源项。
  - 不建议直接剥离 GUI 绘制代码、EPD 驱动代码、函数逻辑；外置 Flash 只能可靠承载数据，不承载可执行代码。
- 接入点：
  - 字体读取：给 u8g2 增加资源读取适配层，避免把整字库一次性搬进 RAM。
  - LUT 读取：替换 `UC8179_Select_LUT()` 返回内置指针的方式，改为返回 tier/type，由资源层读取对应 42B。
  - BLE 命令：新增资源包写入、manifest 提交、资源状态查询命令；图片传输逻辑保持现有路径。

## Test Plan
- 驱动测试：JEDEC ID、状态寄存器、跨页写、跨 sector 写、擦除校验、越界读写拒绝。
- 资源测试：manifest CRC 错误、entry CRC 错误、资源缺失、断电中断 staging manifest 不生效。
- 功能测试：日历/时钟字体渲染、UC8179 全刷/局刷、不同温度 tier 的 LUT 加载。
- 空间验证：Keil map 对比迁移前后 ROM，目标至少减少 12KB，最终应用 ROM 保持在链接区 92KB 内并留出增长余量。
- DFU 验证：普通应用 DFU 后外置 Flash 资源不丢失；资源包可通过 BLE 单独重写。

## Assumptions
- 采用 BLE 资源包作为外置 Flash 资源写入方式。
- 外置 Flash 资源校验失败时直接报错，不保留完整内置回退资源。
- FM25Q04B 按 4Mbit/512KB、4KB sector、256B page 处理。
- 外置 Flash 驱动使用 SPI1，EPD 继续使用 SPI0。
