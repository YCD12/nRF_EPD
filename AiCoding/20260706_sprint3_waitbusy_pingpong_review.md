# Sprint 3 代码审查报告：WaitBusy 深度睡眠 & Ping-Pong 双槽防砖

> 审查时间：2026-07-06
> 审查范围：EPD/EPD_driver.c、EPD/epd_ext_resources.c、EPD/epd_ext_resources.h
> 审查深度：high effort，3 个正确性角度 + 5 个补充角度，独立 verify pass
> 对应 PRD：`lunar_db_prd_and_review.md` Sprint 3

---

## 一、问题清单总览

| 编号 | 严重度 | 状态 | 文件 | 问题摘要 |
|---|---|---|---|---|
| R4 | **严重** | CONFIRMED | EPD/EPD_driver.c:406 | `sd_app_evt_wait()` 导致运行时全刷死循环（已回退 delay(1)） |
| R1 | **高** | CONFIRMED | EPD/EPD_driver.c:409 | `nrf_gpio_cfg_sense_set` 无效——GPIOTE 中断从未在 EPD 操作期间使能 |
| R2 | **中** | CONFIRMED | EPD/epd_ext_resources.c:370 | backup manifest erase-then-write 非原子，掉电窗口导致 backup 静默永久损坏 |
| R3 | **低** | PLAUSIBLE | EPD/epd_ext_resources.c:205 | primary SPI 读取失败静默降级到 backup，日志语义模糊 |

---

## 二、R4 — `sd_app_evt_wait()` 导致运行时全刷死循环 ✅ 已修复

### 问题现象（线上日志）

```
14:23:46⇓[EPD]: busy timeout!
14:24:06⇓[EPD]: busy timeout!
14:24:07⇓[EPD]: BUSY timeout during full refresh (clock path), aborting
14:24:08⇓[EPD]: busy timeout!
14:24:08⇓[EPD]: === Using PARTIAL refresh for clock mode ===
14:24:08⇓[EPD]: *** Performing FULL refresh (mode switch or periodic) ***
...（每分钟循环一次）
```

### 根本原因

Sprint 3 将 EPD_WaitBusy 的 `delay(1)` 替换为 `sd_app_evt_wait()`。这与 commit `8a7a259`（"fix: 恢复 EPD_WaitBusy 轮询检测，修复低功耗休眠导致的忙超时异常"）之前已被回退的历史方案完全相同。

失效机制（两步连锁）：

1. **超时判定在 delay(1) 最后一次迭代后、BUSY 引脚再确认之前**：原代码结构为 "sleep → 检查 timer → return false"，若 EPD 在最后一次 sleep 期间释放了 BUSY，我们在 return false 前不会再读引脚。
2. **更根本的原因（历史回归）**：`sd_app_evt_wait()` 每次唤醒粒度 = BLE 连接间隔（通常 30–400ms）。对于 `WaitBusy(200)` / `WaitBusy(100)` 这类短超时调用（UC8179 PON/TSC 命令的 BUSY 等待），单次睡眠时长可能已超过 timeout 值，导致 EPD 操作被中途 abort。

一旦某次 WaitBusy 提前返回 false，EPD 硬件被遗留在 BUSY=LOW 状态。后续所有 WaitBusy 调用立即超时，形成死循环：
```
WaitBusy 超时 → abort full refresh → force_full_refresh 保持 true
→ 下次 timer tick → 再次全刷 → 再次超时 → ...
```

### 影响范围

- 设备连续全刷，无法显示时间，功耗激增
- `last_full_clear` 永远不更新，每次 timer tick 均触发全刷
- 与 commit `8a7a259` 记录的问题完全一致（历史已证）

### 修复方案（已实施）

**方案**：回退 `delay(1)` 轮询，同时追加 "误判防护"。

```c
while (digitalRead(EPD_BUSY_PIN) == value) {
    delay(1);

    uint32_t now_ticks = app_timer_cnt_get();
    if (app_timer_cnt_diff_compute(now_ticks, start_ticks) >= timeout_ticks) {
        if (digitalRead(EPD_BUSY_PIN) != value) {
            break;  // BUSY 在最后一次 delay 期间释放，非真超时
        }
        NRF_LOG_WARNING("[EPD]: busy timeout!\n");
        return false;
    }
    ...
}
```

"误判防护" 在 timeout 触发时先做一次引脚再确认：若 BUSY 已释放则正常退出，不计超时。这防止了 delay(1) 1ms 内恰好发生的边沿被漏检。

**为何 sd_app_evt_wait() 在此不可行**：

`sd_app_evt_wait()` 需要 GPIOTE_IRQn 在 NVIC 使能才能在 BUSY 边沿唤醒（详见 R1）。而在 EPD 操作期间 GPIOTE 中断未使能，CPU 只能靠 BLE/RTC 事件周期性唤醒（粒度 30–400ms）。此粒度：
- 超过 UC8179 短 WaitBusy（100/200ms）timeout 值 → 误判超时
- 不足以保证在 BUSY 释放后立即发送下一条 SPI 命令（UC8179 可能有内部命令间隔要求）

若要实现真正低功耗的精确 BUSY 等待，需要：将 BUSY 引脚通过 GPIOTE 通道接入，在等待期间使能 GPIOTE_IRQn，才能在边沿唤醒。当前硬件布局未做此设计，此 Sprint 3 子需求无法在不变更硬件的前提下安全实现。

---

## 三、R1 — `nrf_gpio_cfg_sense_set` 无效死代码

### 问题现象

`EPD_WaitBusy` 新实现：
```c
nrf_gpio_pin_sense_t wake_sense = (value == 0U)
    ? NRF_GPIO_PIN_SENSE_HIGH : NRF_GPIO_PIN_SENSE_LOW;
nrf_gpio_cfg_sense_set(EPD_BUSY_PIN, wake_sense);   // ← 无效

while (digitalRead(EPD_BUSY_PIN) == value) {
    (void)sd_app_evt_wait();
    ...
}

nrf_gpio_cfg_sense_set(EPD_BUSY_PIN, NRF_GPIO_PIN_NOSENSE);  // ← 无效
```

设计意图：通过 GPIO PORT 事件（BUSY 引脚边沿）唤醒 `sd_app_evt_wait()`，实现"仅在 BUSY 释放时唤醒"的精确低功耗等待。

### 根本原因

`nrf_gpio_cfg_sense_set()` 仅写 `NRF_GPIO->PIN_CNF[pin]` 的 SENSE 字段，**不会使能 GPIOTE 外设中断（GPIOTE_IRQn）**。

`sd_app_evt_wait()` 内部使用 WFE 指令，在 nRF52 上能唤醒 WFE 的条件：
1. SoftDevice 事件（BLE 无线事件、RTC 定时器）
2. NVIC 中有待处理的中断

GPIO PORT 事件（`NRF_GPIOTE->EVENTS_PORT`）要唤醒 WFE，**必须在 NVIC 层面使能 GPIOTE_IRQn**。而验证发现：

- 项目中 `nrf_drv_gpiote_init()` 仅在 `setup_wakeup_pin()`（`main.c:473`）中调用
- 该函数只在 `BLE_ADV_EVT_IDLE` 事件（广播超时 → 进入深度睡眠）时触发
- 唤醒后 `gpiote_evt_handler`（`main.c:459`）立刻调用 `nrf_drv_gpiote_uninit()`，再次禁用 GPIOTE 中断
- EPD 刷新（`epd_gui_update`）发生在 BLE 正常连接/广播期间，**此时 GPIOTE_IRQn 始终未使能**

### 实际影响

| 方面 | 影响 |
|---|---|
| **功能正确性** | 无影响——循环被 BLE/RTC 事件周期性唤醒，最终总能检测到 BUSY 释放 |
| **节电效果** | 部分实现——`sd_app_evt_wait()` 相比 `delay(1)` 仍有显著节电（CPU 在 BLE 事件间隙进 WFE 睡眠），但唤醒粒度退化为 BLE 连接间隔（10–500ms）而非引脚边沿（<1ms） |
| **唤醒延迟** | 最差增加一个 BLE 连接间隔的延迟，全刷 4 秒场景下可忽略 |
| **潜在副作用** | `nrf_gpio_cfg_sense_set(NOSENSE)` 只清 PIN_CNF.SENSE，**不清 GPIOTE LATCH 寄存器**。LATCH[BUSY_PIN] 置位后若未来某处启用 `nrfx_gpiote`，其 IRQ handler（`nrfx_gpiote.c:758`）会对 LATCH 置位的引脚取反 SENSE 配置，产生杂散行为 |

### 修复方案

**方案 A（推荐）——直接删除两处 sense 调用**

`sd_app_evt_wait()` 本身已能节电，不需要 sense 配置辅助。删除后：
- 消除死代码和 LATCH 脏位副作用
- 节电效果仍然成立（相比原来的 `delay(1)` 显著改善）

```c
// 删除这两处（各一行）：
nrf_gpio_cfg_sense_set(EPD_BUSY_PIN, wake_sense);       // 删除
nrf_gpio_cfg_sense_set(EPD_BUSY_PIN, NRF_GPIO_PIN_NOSENSE);  // 删除（×2处）
```

**方案 B（可选，如需精确边沿唤醒）**

在 `EPD_WaitBusy` 进入前手动使能 GPIOTE 中断，退出后禁用：
```c
// 进入等待前
nrf_gpio_cfg_sense_set(EPD_BUSY_PIN, wake_sense);
NVIC_EnableIRQ(GPIOTE_IRQn);

while (...) { sd_app_evt_wait(); ... }

// 退出后
NVIC_DisableIRQ(GPIOTE_IRQn);
nrf_gpio_cfg_sense_set(EPD_BUSY_PIN, NRF_GPIO_PIN_NOSENSE);
// 清 LATCH：
NRF_GPIOTE->EVENTS_PORT = 0;
```

但此方案需评估与现有 `setup_wakeup_pin` / `nrf_drv_gpiote` 的互斥关系，建议先采用方案 A。

---

## 四、R2 — backup manifest erase-then-write 掉电窗口

### 问题现象

`epd_ext_resource_commit_staging()` 末尾：
```c
m_manifest = staging;
m_manifest_valid = true;   // ← primary 已生效

ret_code_t backup_ret = fm25q04b_erase_sector(EPD_EXT_RES_BACKUP_MANIFEST_ADDR);
if (backup_ret == NRF_SUCCESS) {
    backup_ret = fm25q04b_write(EPD_EXT_RES_BACKUP_MANIFEST_ADDR, &staging, sizeof(staging));
}
```

### 根本原因

`fm25q04b_erase_sector()` 成功（约 50–150ms）后、`fm25q04b_write()` 执行前，若发生掉电或看门狗复位，backup 扇区（0x7F000）被留在全 0xFF 状态。

### 影响路径

```
掉电于 erase 完成后
    │
    └─ 重启
           │
           ├─ reload() 读 primary (0x7E000) → 成功 → 正常启动
           │   （backup 全 0xFF，但 reload() 不会去读它）
           │
           └─ 此后某次 primary 损坏（BLE 传输中断、flash 磨损）
                   │
                   ├─ reload() 读 primary → 校验失败
                   ├─ fallback 读 backup (0x7F000) → magic=0xFFFFFFFF → 校验失败
                   └─ m_manifest_valid = false → 设备进入降级模式
                       ⚠ 无任何 log 提示 backup 已被静默损坏
```

### 实际影响

| 阶段 | 状态 |
|---|---|
| 掉电后首次重启 | 正常（primary 有效） |
| 下次 commit | 新 backup 写入成功，问题自动修复 |
| 掉电后 primary 再次损坏之前从未 commit | backup 无效，无 fallback，设备进降级模式 |

总体属于**低概率两重故障**（掉电时机精确 + primary 后续损坏），但防砖设计的核心假设被静默破坏，调试时无法感知。

### 修复方案

**方案 A（推荐）——启动时检测 backup 健康，损坏时打印告警**

在 `epd_ext_resources_reload()` 中，primary 成功加载后额外检测 backup：

```c
// primary 加载成功后追加（约 5 行）
if (m_manifest_valid) {
    epd_ext_resource_manifest_t bk;
    if (fm25q04b_read(EPD_EXT_RES_BACKUP_MANIFEST_ADDR, &bk, sizeof(bk)) != NRF_SUCCESS ||
        epd_ext_validate_manifest(&bk) != NRF_SUCCESS) {
        NRF_LOG_WARNING("[RES]: Backup manifest invalid or missing, will repair on next commit");
    }
}
```

效果：下次通过 BLE 日志即可感知 backup 退化，不影响正常启动路径性能（backup 校验只在 primary 有效时额外执行一次，且 `epd_ext_validate_manifest` 已有 per-entry CRC 读取——Flash 开销约 100ms，可在降低优先级或异步任务中执行）。

**方案 B（更严格）——commit 时先写后擦**

改为先写入 backup 新内容的第一页（利用 flash"只能从 1 写 0"特性标记进行中状态），再执行完整 erase+write，最后通过魔数验证原子切换。实现较复杂，仅在工业级可靠性需求下考虑。

---

## 五、R3 — primary 读取失败静默降级，日志语义模糊

### 问题现象

```c
ret = fm25q04b_read(EPD_EXT_RES_MAIN_MANIFEST_ADDR, &m_manifest, sizeof(m_manifest));
if (ret == NRF_SUCCESS) {
    ret = epd_ext_validate_manifest(&m_manifest);
}
if (ret != NRF_SUCCESS) {
    NRF_LOG_WARNING("Primary manifest invalid (%u), trying backup", ret);
    // 不管是 SPI 硬件错误还是数据校验失败，都走同一条日志
    ...
}
```

### 根本原因

`fm25q04b_read()` 的非 NRF_SUCCESS 返回（如 `NRF_ERROR_BUSY`）与 `epd_ext_validate_manifest()` 的 `NRF_ERROR_INVALID_DATA`，在代码层面合并走同一 fallback 路径，日志统一描述为"invalid"，无法区分 SPI 硬件故障与数据损坏。

### 实际影响

- 当前 SPI 为单线程独占实例，`fm25q04b_read` 几乎不返回 NRF_ERROR_BUSY，**实际触发概率极低**
- 主要影响是**故障排查时的可观测性**——flash 磨损导致偶发读取错误时，日志无法区分是硬件还是数据问题
- 功能正确性无影响（backup fallback 逻辑本身正确）

### 修复方案

区分 SPI 读取错误和校验失败，各自打印独立日志：

```c
ret_code_t read_ret  = fm25q04b_read(EPD_EXT_RES_MAIN_MANIFEST_ADDR, &m_manifest, sizeof(m_manifest));
ret_code_t valid_ret = (read_ret == NRF_SUCCESS) ? epd_ext_validate_manifest(&m_manifest) : read_ret;

if (valid_ret != NRF_SUCCESS) {
    if (read_ret != NRF_SUCCESS) {
        NRF_LOG_WARNING("[RES]: Primary manifest SPI read failed (%u), trying backup", read_ret);
    } else {
        NRF_LOG_WARNING("[RES]: Primary manifest CRC/format invalid (%u), trying backup", valid_ret);
    }
    ...
}
```

---

## 六、修复优先级与工作量估算

| 编号 | 优先级 | 工作量 | 状态 |
|---|---|---|---|
| R4 | **立即回退** | 10 分钟 | ✅ 已实施（delay(1) + 误判防护）|
| R1 | **立即修复** | 10 分钟 | ✅ 已实施 |
| R2 | **建议修复** | 30 分钟 | ✅ 已实施 |
| R3 | **可选优化** | 15 分钟 | ✅ 已实施 |

---

## 七、已实施修复（2026-07-06）

### R1 — 删除 `nrf_gpio_cfg_sense_set` 死代码（EPD/EPD_driver.c）

移除 3 行无效代码，并同步更新函数注释：

```c
// 删除：函数入口处的 wake_sense 变量声明及 sense 配置（共 5 行注释+代码）
-    // 配置 sense：PIN 从 busy 电平跳变至相反电平时产生 PORT 事件，唤醒 sd_app_evt_wait
-    nrf_gpio_pin_sense_t wake_sense = (value == 0U)
-        ? NRF_GPIO_PIN_SENSE_HIGH
-        : NRF_GPIO_PIN_SENSE_LOW;
-    nrf_gpio_cfg_sense_set(EPD_BUSY_PIN, wake_sense);

// 删除：超时退出路径的 sense 清除（1 行）
-            nrf_gpio_cfg_sense_set(EPD_BUSY_PIN, NRF_GPIO_PIN_NOSENSE);

// 删除：正常退出路径的 sense 清除（1 行）
-    nrf_gpio_cfg_sense_set(EPD_BUSY_PIN, NRF_GPIO_PIN_NOSENSE);
```

函数注释由原来的"配置 BUSY 引脚 PORT sense 在释放边沿唤醒"改为：
> "循环内调用 sd_app_evt_wait() 进入 System ON 睡眠，由 BLE/RTC 事件周期性唤醒后复检引脚（EPD 操作期间 GPIOTE_IRQn 未使能，无需配置 sense）"

**效果**：消除潜在 LATCH 脏位副作用，代码量减少 7 行，Flash text 净减少 32 字节。

---

### R2 — backup manifest 健康检测（EPD/epd_ext_resources.c `epd_ext_resources_reload()`）

primary 加载成功后，对 backup 做轻量结构校验（读 396B manifest 结构体，检查 magic / version / resource_count / struct CRC），**不读资源数据**，避免 init 时间翻倍：

```c
    m_manifest_valid = (ret == NRF_SUCCESS);

    if (m_manifest_valid) {
        epd_ext_resource_manifest_t bk;
        bool bk_ok = false;
        if (fm25q04b_read(EPD_EXT_RES_BACKUP_MANIFEST_ADDR, &bk, sizeof(bk)) == NRF_SUCCESS &&
            bk.magic == EPD_EXT_RES_MAGIC &&
            bk.version == EPD_EXT_RES_VERSION &&
            bk.resource_count <= EPD_EXT_RES_MAX_ENTRIES) {
            uint32_t stored_crc = bk.crc32;
            bk.crc32 = 0;
            uint32_t computed_crc = epd_ext_crc32_finalize(
                epd_ext_crc32_update(epd_ext_crc32_init(), &bk, sizeof(bk)));
            bk.crc32 = stored_crc;
            bk_ok = (computed_crc == stored_crc);
        }
        if (!bk_ok) {
            NRF_LOG_WARNING("[RES]: Backup manifest invalid or missing, will repair on next commit");
        }
    }
```

**效果**：掉电于 erase-write 窗口后，下次启动 BLE 日志立即可见 `"will repair on next commit"`，无需等到 primary 损坏才暴露问题。额外 Flash 读取约 1–2ms（仅 396B，无资源 CRC）。

---

### R3 — SPI / CRC 错误日志区分（EPD/epd_ext_resources.c `epd_ext_resources_reload()`）

将原来的单条 `"Primary manifest invalid"` 拆分为两条独立日志：

```c
-    ret = fm25q04b_read(EPD_EXT_RES_MAIN_MANIFEST_ADDR, &m_manifest, sizeof(m_manifest));
-    if (ret == NRF_SUCCESS) {
-        ret = epd_ext_validate_manifest(&m_manifest);
-    }
-    if (ret != NRF_SUCCESS) {
-        NRF_LOG_WARNING("Primary manifest invalid (%u), trying backup", ret);
+    ret_code_t read_ret = fm25q04b_read(EPD_EXT_RES_MAIN_MANIFEST_ADDR, &m_manifest, sizeof(m_manifest));
+    ret = (read_ret == NRF_SUCCESS) ? epd_ext_validate_manifest(&m_manifest) : read_ret;
+    if (ret != NRF_SUCCESS) {
+        if (read_ret != NRF_SUCCESS) {
+            NRF_LOG_WARNING("[RES]: Primary manifest SPI read failed (%u), trying backup", read_ret);
+        } else {
+            NRF_LOG_WARNING("[RES]: Primary manifest CRC/format invalid (%u), trying backup", ret);
+        }
```

**效果**：SPI 硬件故障（NRF_ERROR_BUSY 等）与 manifest 数据损坏（NRF_ERROR_INVALID_DATA）在日志中清晰区分，无需查表 error code。

---

### 构建验证

```
text    data    bss     dec
56636   448     6656    63740
```

- Flash upper limit：57344 字节
- 剩余：708 字节（修复前 676 字节，R1 删除死代码净节省 32 字节）
- 0 error，0 warning

---

## 八、不需要修复的项目

- **`staging` 栈占用 396 字节**：`epd_ext_resource_manifest_t staging` 为 commit_staging 原有代码，非本次改动引入，当前栈余量充足
- **backup write 耗时约 100ms**：`fm25q04b_erase_sector` 内部有 WDT feeding（每 ~16ms 一次），不触发 WDT 复位；BLE SoftDevice 在更高中断优先级运行，不受影响
- **`sd_app_evt_wait()` 无最小唤醒间隔**：原 `delay(1)` 的 1ms 最小间隔被移除，但 BLE 事件粒度（10ms+）足够，不构成实际问题
