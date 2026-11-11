# 官方 Secure DFU 实现调研与差异核对

调研对象：盒马/DYC EPD 墨水屏（nRF52811 + S112 7.3.0，SDK 17.1.0）固件自带的 Nordic Secure DFU。
事实依据：**设备厂商固件源码**（含 Nordic SDK 17.1.0 原始实现）与 **Nordic 官方 DFU 库**（Android / JS）。所有结论均给出文件与行号，未使用经验推断。

## 0. 结论先行：2900099 的根因

App 模式下的 DFU 特征 `8EC90003` 是 **write + indicate**，不是 notify：

- `SDK/17.1.0_ddde560/components/ble/ble_services/ble_dfu/ble_dfu_unbonded.c:207-222`
  `char_props.indicate = 1; char_props.write = 1; is_defered_write = true; ... write_access = SEC_OPEN`
- 写入授权前，固件强制检查 CCCD 是否为 **indication**：
  `ble_dfu.c:95-110` —— `ble_srv_is_indication_enabled()` 为假时返回
  `BLE_GATT_STATUS_ATTERR_CPS_CCCD_CONFIG_ERROR`，写入被拒。
- 现有实现调用的是 `setCharacteristicChangeNotification()`（CCCD=0x0001），因此固件拒绝写入，HarmonyOS 上报 2900099，设备不会重启进 bootloader。

Nordic 官方 Android 库同样使用 indication：
- `dfu_src/sources/no/nordicsemi/android/dfu/ButtonlessDfuWithoutBondSharingImpl.java:35-38` → `getResponseType() { return 2; }`（2 = indication）
- `ButtonlessDfuImpl.java:34-66` → `enableCCCD(ch, 2)`、写 `{1}`、等待响应、再断开

## 1. 进入 DFU / bootloader 的触发与广播

1. App 固件（S112）初始化的是 **buttonless** 服务：`main.c:276-279` → `ble_dfu_buttonless_init()`。
2. 客户端向 `8EC90003` 写入 1 字节 `0x01`（DFU_OP_ENTER_BOOTLOADER）：
   `ble_dfu_unbonded.c:126-148` → 置 `is_waiting_for_reset=true`，并发送 **indication** 响应 `0x20 0x01 0x01`。
3. 只有收到 indication 的 **HVC 确认** 后才真正复位进 bootloader：
   `ble_dfu.c:178-197`（HVC → `bootloader_start_prepare`）、`ble_dfu.c:262-284`（写 GPREGRET `BOOTLOADER_DFU_START` → `nrf_pwr_mgmt_shutdown(GOTO_DFU)`）。
4. Bootloader 广播名固定为 **DfuTarg**：`nrf_dfu_ble.c:940-949` + `dfu/sdk_config.h:1161-1163`。
5. 广播包内容：flags + 16-bit Service UUID **0xFE59** + Complete Local Name：
   `nrf_dfu_ble.c:181-215`；可连接、可扫描、非定向，interval=40（25 ms）：
   `nrf_dfu_ble.c:218-235`、`sdk_config.h:1167-1168`。
6. 非绑定模式下 bootloader 会把 BLE 地址 **+1**：`nrf_dfu_ble.c:900-915`。
   ⇒ 不能按升级前的 deviceId 直连，必须按 0xFE59 / DfuTarg 重新扫描。
7. 若设备已在 bootloader，会持续广播；连接断开后自动恢复广播：
   `nrf_dfu_ble.c:660-674`。

## 2. 连接与保持

- 连接建立后 bootloader 主动下发连接参数：`nrf_dfu_ble.c:643-658`。
- 参数值：min=max=12（15 ms），slave latency=0，supervision timeout=6000 ms：
  `dfu/sdk_config.h:1191-1205`。
- 非绑定变体特征权限全为 `SEC_OPEN`，**无需配对/绑定**：
  `ble_dfu_unbonded.c:219-221`。
- Bootloader 的 DFU Control Point 要求 **notification**（注意与 App 模式的 indication 相反）：
  `nrf_dfu_ble.c:245-265`；响应以 notification 发送：`nrf_dfu_ble.c:319-332`。
- MTU：bootloader 接受 MTU 交换，并把 payload 向下对齐到 4 字节：
  `nrf_dfu_ble.c:683-720`。

## 3. 固件包、分片、校验与写入时序

### 3.1 包格式
- nrfutil 6 包：`manifest.json` 引用 `.dat`（init packet）与 `.bin`（application image）。
  `pc-nrf-dfu-js/src/DfuUpdates.js` 与现有 `FirmwarePackage.ets` 一致。
- init packet 先传（object type 1，command object），再传 bin（object type 2，data object）：
  `DfuAbstractTransport.js:77-112`。

### 3.2 对象与分片
- `SELECT` 返回 `max_size, offset, crc`：
  `nrf_dfu_ble.c:432-438`（响应布局 12 字节）、`nrf_dfu_req_handler.c:389`。
- `max_size` = `DATA_OBJECT_MAX_SIZE`（nRF52 单页 4096）：
  `nrf_dfu_types.h:76-87`、`nrf_dfu_req_handler.c:389`。
- data object 的 size 必须 **页对齐**（4096），最后一个对象例外：
  `nrf_dfu_req_handler.c:417-425`。
- 一个对象内按 MTU 分片写入 DFU Packet 特征（Write Without Response）；
  官方 Android 每次写 `MTU-3` 字节：`SecureDfuImpl.java`（BaseCustomDfuImpl 的 `mPacketSize`），
  官方 JS 用 `this.mtu`：`DfuTransportPrn.js:260-275`。

### 3.3 校验
- CRC32 是 **从固件起始累计** 的，不是单对象 CRC：
  `nrf_dfu_req_handler.c:447-463`（CREATE 时回滚到 `crc_last`）、`478-520`（逐包累计）、`524-532`（CRC 响应返回累计 offset/crc）。
- 客户端必须比较「累计 offset」与「前 offset 字节的累计 CRC」：
  `DfuAbstractTransport.js:179-198`。
- 每个对象写入完成后必须发 `CALCULATE_CHECKSUM(0x03)`，再发 `EXECUTE(0x04)`：
  `DfuAbstractTransport.js:155-172`、`nrf_dfu_req_handler.c:598-620`。
- 最后一个对象 execute 时做整包 postvalidate/activation：
  `nrf_dfu_req_handler.c:559-577`，随后写 settings 并复位（`on_dfu_complete`）。

## 4. 命令/响应、状态机、超时与重试

### 4.1 操作码（`nrf_dfu_req_handler.h` / `SecureDfuImpl.java:113-118`）
| 操作 | 请求 | 响应 |
|---|---|---|
| CREATE OBJECT | 0x01 + type + size(4, LE) | 0x60 0x01 result |
| SET PRN | 0x02 + prn(2, LE) | 0x60 0x02 result |
| CALC CHECKSUM | 0x03 | 0x60 0x03 result + offset(4) + crc(4) |
| EXECUTE | 0x04 | 0x60 0x04 result |
| SELECT | 0x06 + type | 0x60 0x06 result + max(4) + offset(4) + crc(4) |
| ABORT | 0x0C | 0x60 0x0C result |
| RESPONSE | — | 0x60 op result [ext] |

响应格式实现：`nrf_dfu_ble.c:319-450`。

### 4.2 状态机
`SELECT → CREATE → WRITE* → CHECKSUM → EXECUTE` 循环；
command object 先执行，bootloader 才允许 data object：
`nrf_dfu_req_handler.c:398-410`（无有效 init command 时 CREATE 返回 OPERATION_NOT_PERMITTED）。

### 4.3 超时与重试（官方）
- 单条响应读取超时 5 s：`DfuTransportPrn.js:142-147`。
- 每个分片失败后重试 5 次，并回退到分片边界重新 CREATE：
  `DfuAbstractTransport.js:199-217`。
- 断点续传：SELECT 的 offset>0 且前缀 CRC 匹配 → 从 offset 继续；
  offset 恰在对象边界时先补发一次 EXECUTE（幂等），再重新 SELECT：
  `DfuAbstractTransport.js:92-146`。
- CRC 不匹配视为不可安全续传，直接失败并提示（官方 panic）：
  `DfuAbstractTransport.js:132-139`。
- PRN=0 时不在数据阶段发通知，只对命令响应通知：
  `nrf_dfu_ble.c:397-410`。

## 5. 错误码与恢复

结果码（`nrf_dfu_req_handler.h:100-112`）：
0x01 成功；0x02 不支持的操作码；0x03 参数错误；0x04 资源不足；
0x05 对象无效（型号/签名/解析失败）；0x07 对象类型不支持；0x08 当前状态不允许；
0x0A 操作失败；0x0B 扩展错误（下一字节为扩展码）。

恢复路径：
- 传输中断后 bootloader 继续广播并可重连，已 EXECUTE 的对象进度保留在 settings：
  `nrf_dfu_ble.c:660-674`、`nrf_dfu_req_handler.c:559-590`。
- 续传依赖 SELECT 的 offset/crc；CRC 不匹配只能断电重启后整包重来。
- 厂商手册同样说明：升级偶尔失败时「抠电池重试」：
  `epd-tools/manual.txt` P94-P95、P120-P121。

## 6. 升级完成与回到 App

- 最后对象 execute 后：postvalidate → 写 settings（`on_dfu_complete`）→ 复位：
  `nrf_dfu_req_handler.c:559-577`。
- 复位后运行新 App，重新以 `NRF_EPD` / 服务 `62750001` 广播（App 固件行为）。
- 客户端应在传输完成后重新扫描 App 服务确认回归，而不是依赖 DFU 连接状态。

## 7. 现有实现差异与修正依据

| # | 现状 | 官方依据 | 修正 |
|---|---|---|---|
| 1 | buttonless 用 notify | `ble_dfu.c:95-110`、`ble_dfu_unbonded.c:207-222` | 改用 `setCharacteristicChangeIndication` |
| 2 | 写 0x01 后立即断开、不等待 | `ButtonlessDfuImpl.java:40-61` | 等待 `0x20 0x01 0x01` indication（断连可容忍）后再断开 |
| 3 | DFU 扫描无过滤/扫描全部 | `nrf_dfu_ble.c:181-215`、`900-915` | 按 0xFE59 + DfuTarg 扫描，不按旧地址连接 |
| 4 | 校验按「单对象 CRC/offset」 | `nrf_dfu_req_handler.c:478-532` | 改为累计 offset + 前缀累计 CRC |
| 5 | 无断点续传 | `DfuAbstractTransport.js:92-146` | 按 SELECT offset/crc 续传，边界补 EXECUTE |
| 6 | 分片失败即中止 | `DfuAbstractTransport.js:199-217` | 回退到分片边界重试（≤3 次） |
| 7 | 最后 EXECUTE 断连被当失败 | `nrf_dfu_req_handler.c:559-577` | 断连 + 进度 100% 视为成功，再扫描 App 验证 |
| 8 | 忽略扩展错误码 | `nrf_dfu_req_handler.h:100-112` | 解析 0x0B 的扩展码并展示 |
| 9 | 固定 20 字节分片 | `nrf_dfu_ble.c:683-720` | 使用协商 MTU-3（失败回退 20） |
