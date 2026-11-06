# P5：Secure DFU 与外置资源发布

`tools/dfu_release.py` 生成应用 OTA ZIP、Nordic settings HEX 和合并的 full HEX。发布仍使用 `tools/priv.pem` 签名，并检查应用、Bootloader、settings 与保留的内部 FDS 区域的地址范围、版本单调递增及资源包格式。不再比对私钥推导公钥与 Bootloader 源码或 HEX。

Bootloader 优先采用 `Keil/_build_DFU52/bl_nrf52811_xxaa_s112.hex`，不存在时使用 `tools/bootloader/bl_nrf52811_xxaa_s112.hex`。外置 Flash 仅存放分区资源和用户数据，没有固件槽；资源通过网页端单独烧录。

`DFU_RESOURCES_DIR` / `--resources-dir` 可附加并校验六个 ERS 包。空白设备可安装全部资源；已有分区表的设备更新日历、UI 和字体时保留配置与日期标注。

内部应用区为 `[0x19000,0x27000)`，FDS 保留区为 `[0x27000,0x29000)`。当前配置允许空间不足时使用单 bank 更新，不能保证断电后自动回滚旧应用。发布脚本生成文件，不会对设备执行刷写。
