# P5：Secure DFU 与外置资源发布闸门

## 结论

P5 已实现可重复的 Nordic Secure DFU 发布与审计工具。发布公钥的唯一源码是 `SDK/17.1.0_ddde560/dfu/dfu_public_key.c`，并由 `Keil/DFU-nRF52.uvprojx` 编入 Bootloader。发布工具会优先使用该 Keil 工程生成的 HEX；在它尚未生成时才回退到 `tools/bootloader` 的历史预编译 HEX。

这一结论不涉及外置 Flash：外置 Flash 仍只保存单份资源和用户数据，外置固件槽数量为 0。官方 Secure DFU 的应用、settings、Bootloader 和 SoftDevice 都位于内部 Flash。

## 已实现的发布闸门

[`tools/dfu_release.py`](../tools/dfu_release.py) 是唯一的发布入口，`make-ota-nrf52.sh/.bat` 和 `build.bat ota` 都要求显式提供新旧 DFU 版本。工具会检查：

- 应用完整落在 `[0x19000,0x27000)`，不会占用保留的内部 FDS 区 `[0x27000,0x29000)`；
- Bootloader 位于 `[0x29000,0x2E000)`，UICR 的 Bootloader 指针位于 `0x10001014`；
- `NRF_DFU_REQUIRE_SIGNED_APP_UPDATE=1`、降级保护开启、`NRF_DFU_APP_DATA_AREA_SIZE=8192`；
- 使用 `nrfutil` 生成 ECDSA P-256 签名的应用 ZIP，版本必须大于上一发布版本；
- 生成并审计两页 Nordic settings（`0x2E000`、`0x2F000`）以及完整的 SoftDevice + Bootloader + 应用镜像；
- 签名私钥推导出的公钥与源码一致，并且源码公钥逐字节存在于将要合并的 Bootloader HEX；
- 可选的 `DFU_RESOURCES_DIR`/`--resources-dir` 校验分区表、日历、UI、字体、配置、标注六个 ERS 包的 ABI、分区 ID、CRC、提交标志和 SHA-256，并把清单写入 release JSON。

配置和标注包在清单中标为个人数据。新设备可以配套默认包；已有设备必须先导出 `EPD-MIG1`，再用 `resource_pack.py --migration-bundle` 生成专属包，不能用默认配置覆盖用户数据。

## 当前阻断项

`tools/priv.pem` 推导的公钥与 `SDK/17.1.0_ddde560/dfu/dfu_public_key.c` 一致，说明当前源码和私钥是一对。当前工作区未生成 `Keil/_build_DFU52/bl_nrf52811_xxaa_s112.hex`；回退使用的历史预编译 `tools/bootloader/bl_nrf52811_xxaa_s112.hex` 没有该 64 B 公钥，发布工具因此拒绝：

```
DFU release rejected: bootloader HEX does not contain the expected compiled public key
```

这说明该历史 HEX 由另一把公钥构建。不能仅修改源码公钥、关闭签名校验或跳过检查；那会生成格式正确但不能被 Bootloader 认证的包。

## 恢复为可发布状态的两条路径

1. 在 Keil 中编译 `Keil/DFU-nRF52.uvprojx`，生成 `Keil/_build_DFU52/bl_nrf52811_xxaa_s112.hex`；工具会自动选中它并验证 SDK 公钥。
2. 对已刷入历史 Bootloader 的设备，仍需找回其旧私钥，或通过 SWD/产线刷入新的 Bootloader、SoftDevice、settings 和应用。之后普通应用 OTA 可继续走官方 Secure DFU。

第二条路径会替换设备 Bootloader，不能经由当前 OTA 完成；应在隔离样机上先验证。没有旧私钥且没有 SWD 的既有设备，无法安全地切换到新密钥。

## DFU A/B 边界

当前 Nordic 配置为 `NRF_DFU_FORCE_DUAL_BANK_APP_UPDATES=0`、`NRF_DFU_SINGLE_BANK_APP_UPDATES=0`：空间足够时使用双 bank，不足时允许单 bank。56 KiB 应用区不能保证保留两份同等大小应用，所以不能承诺断电后自动回滚旧应用；单 bank 中断后应重新进入官方 DFU 恢复。外置 Flash 不参与固件暂存，也没有外置 A/B。

## 验证

- `python -m unittest tests.test_dfu_release.py`：发布地址、HEX 校验、资源包、单调版本、保留区和当前密钥错配均有覆盖。
- `dfu_release.py` 已实际运行到 Bootloader 公钥检查并按预期拒绝。
- 尚未在真机执行 SWD Bootloader 重刷、签名拒绝、单 bank 断电恢复或资源缺失修复测试。
