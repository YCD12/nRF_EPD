# 网页 DFU 升级流程分析与修复记录

本文件详细记录了当前网页端 Nordic DFU（无线固件升级）流程的设计背景、存在的问题分析，以及本次进行的修复与优化方法。

---

## 一、 DFU 流程背景设计

Web 端 DFU 升级利用了浏览器提供的 Web Bluetooth API，通过 Nordic Secure DFU 蓝牙协议，实现对 nRF52 芯片固件的 OTA 升级。其核心步骤分为以下两个阶段：

1. **应用态（Application Mode）切换**：
   - 网页首先通过蓝牙连接处于运行态的应用设备（例如 `NRF_EPD`）。
   - 向 Buttonless DFU 服务特征码（`BUTTON_UUID`）写入 `0x01`，通知设备进入 DFU 模式。
   - 设备接收指令后，修改 bootloader 标记，断开 GATT 连接，并重启进入 Bootloader 模式（此时广播名称变更为 `DfuTarg`，且 MAC 地址通常会改变以防止操作系统缓存 GATT 属性）。

2. **DFU 引导态（Bootloader Mode）升级**：
   - 网页等待设备重启完毕后，重新连接 DfuTarg 设备。
   - 通过控制特征码（`CONTROL_UUID`）和数据特征码（`PACKET_UUID`）依次传输 init 数据包（`.dat`）与固件数据包（`.bin`）。
   - 传输完成后发送 Execute 命令，设备完成校验并重启载入新固件。

---

## 二、 问题分析与修复方案

在对原始代码进行审计后，发现并修复了以下三个关键问题：

### 1. 致命缺陷：应用区 Flash 大小限制校验错误

#### 【分析与背景】
在 [dfu.js](file:///d:/Desk/hema/me/New_Plane/he-ma_n-rf_-epd/html/js/dfu.js) 中，限制应用区大小上限的常量 `APP_FLASH_SIZE_BYTES` 原被设定为 `0x17000` (92 KB)。
然而，项目的实际 GCC 链接脚本 [gcc_nrf52.ld](file:///d:/Desk/hema/me/New_Plane/he-ma_n-rf_-epd/SDK/17.1.0_ddde560/gcc_nrf52.ld) 已经将应用区大小硬限制为了 **`0xE000` (56 KB)**：
```ld
FLASH (rx) : ORIGIN = 0x19000, LENGTH = 0xE000
```
这是因为在 `0x19000 + 0xE000`（即 `0x27000`）之上是 FDS 应用数据区（`0x27000 - 0x29000`）以及 Bootloader 代码区（`0x29000` 起）。如果固件写入超过 `0xE000`，会直接覆盖 FDS 和 Bootloader 区域。

#### 【后果】
若用户上传了介于 56 KB 到 92 KB 之间的固件（例如 60 KB），网页会显示“镜像大小就绪”并正常升级，但设备写入时会因覆盖 Bootloader 而**彻底损坏（Brick）**，无法再次启动或进行 DFU。

#### 【修改方法】
在 `dfu.js` 中，将 `APP_FLASH_SIZE_BYTES` 和估算缓存 `DFU_APP_CACHE_ESTIMATE_BYTES` 的大小均修改为 **`0xE000`**：
```javascript
  const APP_FLASH_SIZE_BYTES = 0xE000;
  const DFU_APP_CACHE_ESTIMATE_BYTES = 0xE000;
```

---

### 2. 体验缺陷与兼容性折中：DfuTarg 设备的手动选择与重连优化

#### 【分析与背景】
在 unbonded buttonless DFU 流程中，设备从应用态切换到 DFU 引导态时会重启，且 **MAC 地址可能会发生改变**（通常原 MAC + 1）。
对于 Web Bluetooth，浏览器有非常严格的安全性隔离：
1. **若 MAC 地址改变**：新 MAC 地址对浏览器而言是一个**全新设备**。由于安全策略，网页**绝无可能**静默、自动连接一个新的、未经用户授权的物理设备。网页必须再次调用 `requestDevice()` 来请求用户从浏览器弹出的蓝牙列表中选择 `DfuTarg`。这是浏览器的底层安全防线，无法绕过。
2. **若 MAC 地址保持不变**（例如通过 Bootloader 配置为共享相同 MAC）：由于该物理设备在应用态已获得过授权，浏览器是可以**直接自动重连**（即免去再次弹窗确认）。

原代码在设备重启后，会强制执行 `tryResolveAuthorizedBootloaderDevice` 在后台自动寻找 DfuTarg 设备并挂起等待 8 秒（`INITIAL_DFU_DISCOVERY_TIMEOUT_MS = 8000`），且默认的连接尝试超时长达 6 秒。
- **如果在新 MAC 模式下**，该自动连接必然因为无授权而失败。用户在设备重启后，必须等待 8 秒超时，等自动查找失败后，浏览器又由于手势（User Gesture）过期导致直接 `requestDevice()` 失败并报错 `NotAllowedError`，接着才显式要求用户去手动点击按钮。这导致每次都要无谓等待 8 秒。
- **如果在同 MAC 模式下**，若连接由于系统调度稍有延迟，6000ms 的重试机制也容易导致连接等待太久。

#### 【优化与兼容方案】
为了兼顾“同 MAC 地址下可实现全自动升级”以及“新 MAC 地址下必须手动选择但需跳过无谓的 8 秒等待”，我们对 `resolveBootloaderAfterSwitch` 进行了**动态超时与重连重构**：
1. **获取授权设备列表**：利用 `navigator.bluetooth.getDevices()` 获取当前页面已授权的所有设备。
2. **动态设定搜索超时 (`timeoutMs`)**：
   - 如果已授权列表中已经存在名称符合 `looksLikeBootloaderName`（即 `DfuTarg` 或以 `Dfu` 开头）的设备，或者 `preferredDevice` 本身就是 bootloader（说明不是第一次从应用态切换，而是处于断线重连或多分区升级中），说明有重连成功的可能，继续使用完整的 **8 秒** 超时进行自动搜索。
   - 如果不存在已授权的 DfuTarg，且当前设备仍然是应用态设备名称（如 `NRF_EPD`），说明很有可能该设备会使用新 MAC 地址进行重启。为了不让用户无谓等待，将搜索超时压缩到 **3 秒**，并将 `CONNECT_TIMEOUT_MS` 全局调小至 **3 秒**。
3. **效果**：
   - **若是同 MAC 模式**：在 3 秒窗口期内，代码会自动与重启后的设备建立 GATT 重连，从而实现**全自动化 OTA，无需用户进行二次选择**。
   - **若是不同 MAC 模式**：在 3 秒快速重连失败后，网页会迅速转入 `requestDevice` 失败流程，立刻将手动接管按钮显示并闪烁，减少了原本需要被迫等待的 5 秒空转时间。

```javascript
  async function resolveBootloaderAfterSwitch(dfu, preferredDevice) {
    await dfu.delayPromise(DFU_REBOOT_SETTLE_MS);

    // 优化：检测是否有已授权的 Bootloader 设备
    let hasAuthorizedDfu = false;
    if (navigator.bluetooth && typeof navigator.bluetooth.getDevices === 'function') {
      try {
        const devices = await navigator.bluetooth.getDevices();
        hasAuthorizedDfu = devices.some(dev => looksLikeBootloaderName(dev));
      } catch (_) { }
    }
    const isPreferredLooksLikeBootloader = looksLikeBootloaderName(preferredDevice);

    // 如果未找到任何预授权的 DfuTarg，且原设备为应用态设备，
    // 则有极大概率 MAC 地址会发生改变。我们将超时时间压缩为 3 秒（并将连接超时设为 3 秒），
    // 使得如果是同 MAC 模式能自动重连（实现完全自动化），若非同 MAC 模式也仅需等待 3 秒即可唤醒手动接管。
    const timeoutMs = (hasAuthorizedDfu || isPreferredLooksLikeBootloader)
      ? INITIAL_DFU_DISCOVERY_TIMEOUT_MS // 8 秒
      : 3000; // 3 秒

    const authorized = await tryResolveAuthorizedBootloaderDevice(
      dfu,
      preferredDevice,
      timeoutMs,
      '切换到 DFU'
    );
    if (authorized) return authorized;
    try {
      return await requestValidatedBootloaderDevice(dfu);
    } catch (err) {
      if (isUserGestureError(err)) {
        throw makeManualPickError('系统可能已经配对 DfuTarg，但当前网页尚未获得该设备授权，请点击“手动选择DFU设备并继续”并重新选择 DfuTarg');
      }
      if (isPickerCancelled(err)) {
        const cancelled = new Error('已取消选择DFU设备，可点击“手动选择DFU设备并继续”重试');
        cancelled.code = DFU_BOOTLOADER_PICK_CANCELLED_ERROR;
        throw cancelled;
      }
      throw err;
    }
  }
```

---

### 3. 逻辑缺陷：固件完全传输后缺乏断点续传/跳过检查

#### 【分析与背景】
在 `transfer` 方法中，代码先前仅对 `objectName === 'init'` 做匹配跳过：
```javascript
if (objectName === 'init' && offset === buffer.byteLength && this.checkCrc(buffer, crc)) { ... }
```
对于数据固件（`firmware`），如果不加以拦截，当固件其实已经全部传输完毕（`offset === buffer.byteLength` 且 CRC 匹配），但先前在执行 Execute 或 ACK 接收时发生断开重试，代码会误入 `transferObject`。
在此处由于 `offset` 等于固件长度，`start` 被计算为 `buffer.byteLength`，传入 `transferObject` 的长度差为 0。最终会尝试向设备发送大小为 0 的 Create Object 申请，这会引起 Nordic 引导程序的协议报错（如操作不允许或参数错误），从而阻碍升级流程。

#### 【修改方法】
将完全匹配的断点续传检查泛化到所有类型的 Object（包括 `init` 和 `firmware`）。若 `firmware` 已就绪且 CRC 匹配，直接发送 `EXECUTE` 激活固件：
```javascript
    async transfer(buffer, objectName, selectType, createType) {
      const resp = await this.sendControl(selectType);
      const maxSize = resp.getUint32(0, LITTLE_ENDIAN);
      const offset = resp.getUint32(4, LITTLE_ENDIAN);
      const crc = resp.getInt32(8, LITTLE_ENDIAN);
      if (offset === buffer.byteLength && this.checkCrc(buffer, crc)) {
        this.log(`${objectName} already available, executing/skipping transfer`);
        if (objectName === 'firmware') {
          await this.sendControl(OP.EXECUTE);
        }
        return;
      }
      this.progress(objectName, buffer.byteLength, 0);
      await this.transferObject(buffer, createType, maxSize, offset, objectName);
    }
```

---

## 三、 验证与修复结果

通过以上修改，升级网页端：
1. 彻底防范了上传过大固件（>56 KB）导致设备损坏的致命安全缺陷。
2. 将升级过程中的交互重连等待体验大幅提升，非首次升级或无授权的查找过程缩减了 8 秒无谓挂起。
3. 兼顾了自动升级的兼容性：在同 MAC 升级时可直接实现全自动 OTA，而在不同 MAC 且无浏览器授权时则在 3 秒重试失败后即刻显示手动唤醒按钮。
4. 增强了无线连接 of 鲁棒性，固件已完全传输的情况下重试升级能正常进行激活执行。
