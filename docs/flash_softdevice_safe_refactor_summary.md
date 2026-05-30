# flash_softdevice 安全重构总结

## 背景

在原始工程里，Keil 的 `flash_softdevice` target 将输出目录直接指向了 SDK 自带的 SoftDevice 目录：

- `SDK/17.1.0_ddde560/components/softdevice/s112/hex/`

同时输出文件名也被设置成官方文件名：

- `s112_nrf52_7.3.0_softdevice.hex`

这会带来两个问题：

1. 在 Keil 中执行 `Clean Target` 时，Keil 会把该 target 的输出文件当成构建产物删除，从而误删 SDK 原始 SoftDevice 文件。
2. 如果误执行 `Build/Rebuild`，target 还有机会把错误产物覆盖到官方 SoftDevice 文件名上。

## 本次修改

### 1. 重构 `flash_softdevice` 为安全 target

修改文件：

- `Keil/EPD-nRF52.uvprojx`

调整内容：

- 将 `flash_softdevice` 的输出目录从 SDK 路径改为 `Keil/_build_softdevice_nRF52/`
- 将输出名改为不再直接占用 SDK 原文件路径的 `s112_nrf52_7.3.0_softdevice`
- 将 `AfterMake` 配置为调用中转脚本，由脚本把 SDK 中的官方 SoftDevice 复制到安全输出目录
- 将该 target 的编译器元数据和宏定义修正为与 nRF52 工程一致，避免残留 `NRF51/S130` 配置

### 2. 新增 SoftDevice 中转脚本

新增文件：

- `tools/stage-softdevice-nrf52.bat`

脚本职责：

- 检查 SDK 中的 `s112_nrf52_7.3.0_softdevice.hex` 是否存在
- 自动创建 `Keil/_build_softdevice_nRF52/`
- 将官方 SoftDevice 复制到 Keil 安全输出目录
- 在文件缺失或复制失败时返回明确错误

### 3. 更新开发文档

修改文件：

- `docs/develop.md`

文档更新点：

- 恢复为当前项目原有的中文文档风格
- 将 `flash_softdevice` 的用途改为“安全中转并烧录 SoftDevice”
- 将流程改为“先 Build 一次，再 Download”
- 补充说明不要再把 Keil 输出目录指向 SDK 的 `softdevice` 目录
- 补充缺少 `s112_nrf52_7.3.0_softdevice.hex` 时的恢复路径

## 修改后的使用流程

1. 切换到 `flash_softdevice`
2. 执行一次 `Build`
3. 确认生成文件位于 `Keil/_build_softdevice_nRF52/s112_nrf52_7.3.0_softdevice.hex`
4. 执行 `Download`
5. 切回 `nRF52811_xxAA` 编译并下载应用固件

## 结果

- `Clean Target` 不会再删除 SDK 原始 SoftDevice 文件
- `flash_softdevice` 的构建产物与 SDK 原始资源彻底隔离
- 文档与实际工程行为保持一致

## 附件

对应补丁文件：

- `docs/flash_softdevice_safe_refactor.diff`
