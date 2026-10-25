**设备端未提交改动与 Flash 优化复核（2026-10-17）**

**后续已实施：** 用户确认后已应用局部优化、条件时钟修复、几何/资源读取及队列投递合并。最终应用 56,340 B，较本篇基线减少 704 B；实施范围、验证与产物见 [实施记录](device-flash-implementation-20261017.md)。以下保留实施前的评审快照和隔离试验数据。

结论：可以继续合并重复实现。三项局部优化在独立副本中的生产 ARM 构建，合计使应用从 **57,044 B 降为 56,484 B，减少 560 B**；56 KiB 应用区余量由 **300 B 增为 860 B**。这能改善余量，但不足以恢复 4 KiB 的工程预留目标。进一步优化应围绕时钟几何、资源读取和服务调度，不应削减已有显示功能或资源校验。

本次为评审及隔离验证，**没有把候选改动应用到生产源码，没有改变暂存区、提交、merge 或刷机**。已生成包含生成器修改的可审阅补丁：[flash-candidates.patch](../output/device-flash-review-20261017/flash-candidates.patch)。`git apply --check` 通过。补丁包含下述三个体积优化，不包含条件时钟显示问题的修复。

**范围与实际基线**

检查时 `dev`、`HEAD`、`origin/develop` 都指向 `ee0379c`，没有正在解决的 merge 冲突；这里的“未 merge”按暂存区相对 HEAD 的未提交变更处理，没有访问远端确认最新提交。`EPD/`、`GUI/` 共 21 个变更文件，`main.c` 本身无差异，但一并检查了启动、BLE、资源激活、定时刷新和休眠链路。

使用项目 GCC 9.3.1，默认 `-Os -flto --gc-sections`，固定 800×480 屏幕、外置字体、关闭日志。HEAD 从 Git 导出设备端源码后，使用相同 SDK、构建文件和参数重新构建。所有输出单独放在 `output/device-flash-review-20261017/`，没有覆盖 `_build/`。

| 构建 | 应用 BIN | 分区剩余 | 静态 RAM（data+bss） |
| --- | ---: | ---: | ---: |
| HEAD | 55,780 B | 1,564 B | 7,464 B |
| 当前暂存代码 | 57,044 B | 300 B | 9,512 B |
| 三项优化组合副本 | 56,484 B | 860 B | 9,504 B |

本批改动相对 HEAD 增加 1,264 B Flash 和 2,048 B 静态 RAM；不能直接沿用旧文档中其他版本的基线。当前 `m_components` 占 2,048 B RAM，不能把缩小它的效果直接算成 Flash 收益。组合构建仍保留 2,048 B 栈，静态 RAM 加栈为 11,552 B，低于 15,656 B RAM 预算；这不代表已测量实际栈峰值。

**功能与结构判断**

当前资源化方向应保留：网页生成外置字体、日历、UI、标注和待办；设备保留时间、温度、电压等运行时数据，以及历法、待办调度、字体解码、分页绘制和局刷执行。UIR2 已把日历、分屏时钟、全屏时钟、待办日历和全屏待办汇入 `DrawResourceScene()`，无需再分拆成五套硬编码页面。图片直传仍为独立路径。

`main.c` 启动顺序为服务/SPI 初始化、资源加载、广播与首次刷新；主循环负责服务处理、SDK 事件调度和低功耗。建议继续保持 main 的系统入口职责，GUI 负责绘制，EPD service 负责事务和刷新生命周期；合并业务公共实现比把三个目录拼成大文件更有价值。

**可以优先合并的三项优化**

| 优先级 | 具体合并 | 单项实测减少 | 组合时必须保留的行为 |
| --- | --- | ---: | --- |
| 1 | UI 参数范围表按重复上下界建立索引 | 256 B | 全部字段原有范围、参数顺序和包格式 |
| 2 | 去掉 GUI 字体回调的第二套裁剪状态，统一使用 GFX clip | 208 B | 组件、待办行、负 bearing、删除线和分页边界 |
| 3 | UC8179 OLD/NEW 窗口写入共用现有逐行传输函数 | 96 B | DTM1/DTM2 顺序、64 B 分块和每 8 行喂狗 |

1. **范围表复用。** `GUI/ui_schema.h` 的 118 组 `int16_t` 上下界实际只有 27 种。原表 472 B，改成 118 B 索引加 108 B 范围字典，共 226 B；246 B 是数据层差值，实际链接减少 256 B。`GUI/ui_resources.c` 通过索引查表，校验规则完全一致。必须同步修改 `tools/ui_resource_codec.py`，否则下次生成会覆盖手改头文件。候选补丁已经同步生成器，生成检查通过，网页 schema 和默认二进制载荷保持不变。

2. **裁剪统一。** `GUI/GUI.c:550` 的 `m_todo_clip` 和 `todo_clip_line()`、`GUI/ui_render.h:325` 的回调安装，与 `GUI/Adafruit_GFX.c:488` 新增的逐像素裁剪重复。候选删除前一套包装，用 GFX 的同一矩形维护组件边界和当前待办行；删除线也读取该矩形。静态 RAM 减少 8 B。该改动会让部分屏外字形扫描线继续进入逐像素拒绝路径，虽然像素一致，最坏裁剪场景的设备 CPU 时间仍需测量；如果有明显性能退化，应把扫描线预裁剪统一放到 GFX，而不是恢复两份状态。

3. **UC8179 逐行发送统一。** `EPD/UC8179.c:102` 的 `UC8179_Write_Packed_Rows()` 只有两个调用，stride 恒等于 `w/8`；与 `:313` 的 `UC8179_Write_Image_Rows()` 重复。调用入口已校验 OLD/NEW 非空，因此可直接复用现有发送器。没有合并或改变初始化命令、波形、面板刷新序列和超时策略。

以上单项收益以当前代码为共同基线；组合构建独立验证为减少 560 B，而不是仅根据单项数字推算。

**合并前值得处理的功能一致性问题**

时钟组件自身设置 `SYNC_ONLY`、`JIEQI_AHEAD` 或 `JIEQI_TODAY` 时，全刷与局刷的可见性规则不一致：

- `GUI/ui_render.h:309` 的完整场景会判断上述条件，条件不满足时不画该组件。
- `GUI/ui_resources.c:244` 的 `partial_allowed` 只检查黑白颜色、隐藏位和模式，未排除时钟自身的条件标志。
- `EPD/epd_clock_refresh.c:219` 特殊判断的是覆盖在时钟上的同步提示；`GUI/GUI.c:793` 的 ClockOnly 路径直接绘制时钟。
- `EPD/EPD_service.c:413` 可在完整刷新成功后标记时钟 OLD 有效，即使该条件时钟实际上没有被画出。

例如把全屏时钟组件设为“仅未校时时显示”，在 2026 年完整刷新时它被隐藏；下一分钟仍可能进入局刷，把原应隐藏的时钟画出来，并用并不存在的旧数字生成 OLD。默认资源不使用这种时钟配置，因此常规默认像素回归不能覆盖它。

主机探针确认三种标志均能通过资源加载，且仍返回 `partial_allowed=true`，见 [probe_clock_visibility.c](../output/device-flash-review-20261017/probe_clock_visibility.c) 和 [日志](../output/device-flash-review-20261017/clock-visibility.log)。这是局刷资格判定的复现，未宣称已经实机复现面板残影。

建议先让带运行时条件的时钟保守回退完整绘制；后续把组件可见性集中为一个规则，并在条件发生变化时失效 OLD。仅在当前分钟判断“可见”还不够，也必须保证上一帧的 OLD 对应同一可见状态。

**第二阶段值得评估的合并，收益尚未实测**

| 位置 | 建议 | 约束 |
| --- | --- | --- |
| `GUI/ui_resources.c:74` 与 `GUI/ui_render.h:7` | 统一时钟 step/block/x/y/minute 几何计算，初始化验证、全刷和局刷共用 | 不能用完整扫描全部段形的验证函数替代每页轻量绘制；避免重复读外置参数 |
| `GUI/ui_render.h:303` 与 DrawClockFace/DrawTodo | 将场景已读取的 style 传给具体控件，避免再次读取 | ClockOnly 独立入口仍需读参数；减少 SPI 读取不等于一定减少 Flash |
| `EPD/resource_store.c:133/154` 及 font/UI/calendar 消费者 | 提供一次获取并验证 metadata/payload 的公共入口，减少 info+open 重复查分区/读头 | 保留包 CRC、长度、版本与热更新失效规则，不能把未验证 info 当成已验证 open |
| `main.c:1023`、`EPD/epd_command_queue.h:61`、`epd_display_queue.h:34` | 评估主循环直接驱动已有业务队列，减少再次包装 SDK scheduler 事件的状态 | ACK 必须先于慢刷新，保留 IRQ/主循环隔离、断连重试、DFU 和休眠；SDK 电源管理仍使用 scheduler，不能直接删库 |

当前 ELF 中 `DrawResourceScene` 5,936 B、`main` 5,904 B、`epd_gui_update` 2,976 B、`epd_execute_command` 2,120 B，是继续定位的入口。LTO 会把被调用函数内联归到调用者，这些大小不等于相应源码文件的独立成本，更不等于删除模块的净收益。

把 `ui_render.h` 中的实现整理为 `.c`/私有头文件可以改善维护，但该头目前只由 GUI.c 包含一次，不能据此声称会消除多份固件代码。清理未调用的旧布局 helper、内部字体条件分支和关闭的日志也应先看最终 ELF；当前构建已启用 LTO 和未使用节回收。恢复/缩减 8 KiB 保留区属于分区变更，不是本次代码优化，未涉及。

**验证与产物**

- HEAD、当前、三个单项和组合均以生产 ARM 配置构建，Flash 硬边界通过；大小及 SHA-256 见 [measurements.json](../output/device-flash-review-20261017/measurements.json)。
- 组合版 UIR2 加载/非法包/几何/容量测试、UC8179 命令契约测试通过。
- 当前版与组合版均执行普通和固定屏幕 GUI 回归，包括日历、待办字体/负 bearing、行裁剪、故障恢复、跨日和逐字节 OLD/NEW 条带；均通过。
- 复用已有的 25 组外置资源与网页像素样本，对当前版/组合版、普通/固定屏幕共进行 100 次逐像素核对，全部一致。本次没有重新运行 Node 生成这些样本，不能视为新跑了完整网页测试。脚本及结果见 [verify_candidates.py](../output/device-flash-review-20261017/verify_candidates.py)、[candidate-validation.log](../output/device-flash-review-20261017/candidate-validation.log)。
- 当前源码的 service lifecycle、resources reload、TODO service pipeline 和 TODO integration 测试通过，见 [current-host.log](../output/device-flash-review-20261017/current-host.log)。
- 候选生成器 `--check` 与补丁 `git apply --check` 通过；生成后的网页 schema、默认载荷未改变。

未验证实机显示耗时、掉电、BLE 空口压力、OTA 安装和栈峰值。隔离输出为分析构建，不是签名发布包。建议先合并范围表/UC8179 小改动，裁剪合并补测设备耗时，并单独修正条件时钟局刷资格；之后再以相同口径评估第二阶段结构合并。
