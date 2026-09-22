# 外置资源迁移：P1 存储底座实施记录

## P0 前置规格

P0 已完成分区 ID、资源 ABI、单份存储约束和容量规划；其正式依据是[候选数据盘点](external-data-candidates-20260916.md)与[分区布局 JSON](external-flash-layout-v1.draft.json)。设备现有字体目录、外置实际占用和已部署 Bootloader 仍需在实机盘点中确认。

## P1 本次交付和边界

已实现分区 ABI、生成器、单份包存储、BLE 检查/维护通道及网页工具。**未完成八类业务数据迁移，配置和标注仍使用原 FDS，字体仍由正常版本按旧布局读取。** 本次没有烧录设备、生成签名 DFU 发布包或改动实际 Bootloader。

按用户要求固定保留：应用 `[0x19000,0x27000)` 56 KiB、内部 FDS `[0x27000,0x29000)` 8 KiB、Bootloader 起点 `0x29000`。链接脚本、FDS 配置、DFU 数据保留大小均未调整。

外置容量和八个分区仍采用 [布局 JSON](external-flash-layout-v1.draft.json)。每个分区只有一份，分区表也没有镜像。现阶段需要测试设备验证后再作为正式迁移布局发布。

## 实现文件

| 文件 | 用途 |
| --- | --- |
| `tools/resource_pack.py` | 校验布局，生成 C/网页常量、分区表包及存储测试用资源包 |
| `EPD/resource_layout.h`、`html/js/resource_layout.js` | 自动生成的分区 ID、偏移、大小；不要手工修改 |
| `EPD/resource_store.c/.h` | 分区表读取、ID 查找、包校验、范围检查、单份更新 |
| `EPD/resource_protocol.c/.h` | `0x26` 请求 / `0xA7` 响应，状态、导出、维护写入 |
| `EPD/resource_transfer.c/.h` | 主循环执行、通知重试、请求去重、连接代次和超时清理 |
| `html/resources.html`、`html/js/resource_store.js` | 独立维护页面及客户端，不替换现有业务页面 |

Makefile 和两个 Keil target 都已添加源码。Keil 默认使用正常版本，本轮只实际验证 GCC 构建。

## 两种构建

### 正常版本，默认 `RESOURCE_STORE_MAINTENANCE=0`

- 保留现有页面、旧字体上传及内部配置/标注存储。
- 新命令只开放状态和原始读取；所有资源写入请求返回 `RS_READ_ONLY`。
- 不自动写表、格式化、搬移或擦除旧数据。
- 字体初始化增加前缀检查，识别到新资源表或未知前缀后不启动旧分配器。

### 维护版本，`RESOURCE_STORE_MAINTENANCE=1`

- 保留 BLE、官方 DFU 入口和底层硬件驱动，停用旧字体初始化/上传及正常显示操作。
- 先显式进入 `MIG1` 维护会话，再允许写表和资源包。进入会话本身不擦除。
- 可在坏表或未安装表的情况下，重写固定地址处的分区表。
- 专供 P1 存储验证；尚未接入新资源消费者，因此不是业务功能完整的替代固件。
- 写入新表可能破坏旧字体目录；写资源可能覆盖旧字模。当前不能在旧设备上自动完成无损迁移。应先导出并校验，在测试设备上验证；正式转换器在 P4 提供。

**改成新布局后不要安装仍按 FST2/FST3 分配空间的旧固件。** 新表有效时前缀检查可以阻止旧路径，但这不是旧固件在所有坏表/掉电状态下的保护机制。维护版本从构建上停用该路径。

构建时为两个选项使用不同输出目录，避免复用选项不同的旧目标文件：

```sh
make -f Makefile.nRF52_linux nrf52811_xxaa RESOURCE_STORE_MAINTENANCE=0 OUTPUT_DIRECTORY=output/external-resources-p1/normal-build
make -f Makefile.nRF52_linux nrf52811_xxaa RESOURCE_STORE_MAINTENANCE=1 OUTPUT_DIRECTORY=output/external-resources-p1/maintenance-build
```

Windows/Cygwin 环境沿用工程的 `PROJ_DIR`、`SDK_ROOT`、`GNU_INSTALL_ROOT` 绝对 Windows 路径及 `PASS_LINKER_INPUT_VIA_FILE=0` 设置。

## 资源包 ABI 1

全部为显式小端序，不直接序列化 C struct。包头固定 32 B：

| 偏移 | 长度 | 内容 |
| --- | ---: | --- |
| 0 | 4 | `ERS1` magic，`0x31535245` |
| 4 | 4 | 高 16 位 ABI=1，低 16 位分区 ID |
| 8 | 4 | 资源 revision |
| 12 | 4 | 载荷长度，不含包头 |
| 16 | 4 | 载荷 CRC32/IEEE |
| 20 | 4 | 芯片容量，当前 524288 |
| 24 | 4 | 前 24 B 的 CRC32/IEEE |
| 28 | 4 | 最后写入的有效标记 `0xA55AC33C` |

分区表本身也是一个包，固定 ID=1、位置=0、分区容量=4096。表的载荷由最多 32 个 16 B 表项组成：`id:u16, type:u16, offset:u32, size:u32, schema:u16, flags:u16`。当前 schema=1、flags=0；表项数量由载荷长度推导。类型数字 1～8 对应布局中的八种类型。

读取表时检查包头、容量、CRC、扇区对齐、地址范围、分区 ID 唯一性、分区之间无重叠及固定表位置。未来资源 ID 可以扩展，但当前 BLE 协议只支持 1～255。

`resource_store_open()` 完整检查载荷 CRC；消费者应先成功 open，再使用 `resource_store_read()` 做范围内读取。P1 校验到存储包这一层，字体结构、日期范围、UI 组件及资源之间的版本依赖检查在 P2/P3 的消费者中实现。**通用包提交成功不等于其业务内容已经可显示。**

## 单份更新语义

1. 校验请求和当前分区表，阻止越界或只读分区写入。
2. 将旧有效标记写为无效并读回确认，再擦除首扇区。
3. 顺序写入载荷，按需擦除后续扇区。
4. 完整读回校验载荷；表包额外校验表项结构。
5. 写入并读回包头，最后写有效标记并读回确认。

失败时不恢复旧数据，也不寻找备用包。重启后只接受有效标记、CRC 和格式均通过的包；否则需要重新上传。写操作结果不确定时，当前事务停止继续提交。

顺序写入允许重传已经落盘且内容相同的小块；传输层缓存最近完整请求和响应，重复 BEGIN/COMMIT 不再次执行。客户端按请求/响应逐包推进，最大包长 20 B，适配默认 ATT MTU。

传输进度目前保存在 RAM，并可通过状态查询；断连、60 秒无活动或复位后从头重传。外置 4 KiB 更新状态分区暂不落盘使用，避免每个小包擦写引起磨损。本阶段没有断点续传承诺，也没有在该分区保存副本。

BLE 接收回调只复制请求。Flash 擦写及通知发送均由主循环执行；旧字体更新/屏幕刷新期间等待空闲。通知队列满时保留回复，断连代次变化时丢弃旧会话和回复。

## BLE 协议

请求前缀：`26 op token`；响应前缀：`A7 op token result`，所有多字节字段为小端序。

| op | 请求体 | 响应追加内容 |
| --- | --- | --- |
| 0 状态 | 无 | 写入能力:u8、ABI:u8、活动 ID:u8、维护会话:u8、已收长度:u32、容量:u32 |
| 1 原始读取 | 地址:u32、长度:u8（1～16） | 原始字节，用于旧布局导出和检查 |
| 2 BEGIN | ID:u8、载荷长度:u32、CRC:u32、revision:u32 | 无 |
| 3 DATA | 载荷偏移:u32、数据:1～13 B | 无 |
| 4 COMMIT | 无 | 无 |
| 5 ABORT | 无 | 无；仅取消 RAM 事务，不恢复旧数据 |
| 6 ENTER | ASCII `MIG1` | 无 |

状态值：0 成功、1 请求/格式错误、2 忙、3 CRC 错误、4 未找到、5 I/O 错误、6 ABI 不兼容、7 只读。状态查询的 result 表示表挂载结果；即使没有表，仍可导出和在维护版中重建表。包头/载荷的确认由 COMMIT 完成，DATA ACK 只表示底层写入返回成功。

## 工具用法

```sh
python tools/resource_pack.py --check
python tools/resource_pack.py --output output/external-resources-p1
# 仅封装测试载荷；不会把它变成可用的字体/日历/UI 格式：
python tools/resource_pack.py --resource-id 5 --payload path/to/test.bin --output output/external-resources-p1
```

输出 `partition-table.payload.bin`（128 B）与 `partition-table.ers`（160 B）。分区容量均包含 32 B 包头。

从已有网页服务打开 `html/resources.html`，连接设备后先读取状态。支持导出完整 512 KiB Flash；使用 16 B 小块，传输较慢。该导出是原始字节快照，不包括内部 FDS 数据，也不替代 P4 的完整配置/标注迁移器；导出期间不得并发修改外置数据。页面显示导出文件 CRC，实际旧字体目录及字模 CRC 仍需后续盘点验证。

上传前选择 `.ers` 包并确认已保存旧数据；客户端校验文件头和载荷 CRC，设备提交前再次读回校验。普通固件拒绝写入。表需要先上传，其余包随后按分区 ID 写入。旧资源完整导入、新消费者切换、UI 兼容检查尚未提供。

## 构建与验证结果

GCC 9.3.1、生产 56 KiB 链接区域、`-Os` + LTO：

| 构建 | Flash | 剩余 | 静态 RAM 段合计 |
| --- | ---: | ---: | ---: |
| 本次前基线 | 52,412 B | 4,932 B | 6,788 B |
| 正常版本 | 53,884 B | 3,460 B | 6,908 B |
| 维护版本 | 35,740 B | 21,604 B | 4,780 B |

正常版新增 1,472 B Flash、120 B 静态 RAM。RAM 段合计不含 4 B 对齐及 2,048 B 栈预留；正常版静态区与栈间空余 6,696 B。尚未测实机栈峰值。

正常版剩余低于 4 KiB 预警线，未超过硬上限；下一步应通过替换旧字体存储路径控制过渡代码，而不是再叠加一整套旧/新实现。维护版因停用显示与旧字体路径而变小，不能计为完整应用的优化收益。

- 全量通过：32 个 C 测试程序、实际服务函数提取集成测试、22 个 Node 测试、6 个 Python 单元测试。
- 新测试包括表/资源更新中断、部分擦除/编程/提交、CRC、坏表修复、重复/越界 ID、跨扇区写入及相邻分区保护、表挂载后损坏时拒绝擦写。
- BLE 传输测试覆盖队列满重试、重复 BEGIN 不重复擦除、断连取消、正常版本拒写、错误 token 和客户端 CRC/长度检查。
- 两份 HEX 校验和、地址范围和加载内容均与 BIN 一致，最大地址小于 `0x27000`。
- 生成文件一致性与 `git diff --check` 通过。
- 未做设备烧录、真实 BLE、SPI 掉电、页面浏览器交互或 DFU 实机验证。

产物位于 `output/external-resources-p1/{normal-build,maintenance-build}/`；测量、SHA-256 和链接符号位于 `output/external-resources-p1/result.json`，回归日志为同目录 `host-tests.log`。

## 后续实施

1. P0 尚需读取设备实际旧字体目录、字模大小/CRC，并核对实际 Bootloader；源码配置不能代替设备事实。
2. P2 先接入三套字体包和日历资源读取，提供格式验证、上传与损坏回退，再测净 Flash/RAM。
3. P3 外置 UI 参数，共享全刷/局刷坐标；资源改变后使 OLD/NEW 状态失效并先全刷。
4. P4 迁移配置/标注及正式导出/导入工具。即使以后不再使用 FDS，本次仍保留其 8 KiB 地址范围。
5. P5 核对官方 DFU 包版本并实机验证升级和中断恢复；不增加外置固件区。
