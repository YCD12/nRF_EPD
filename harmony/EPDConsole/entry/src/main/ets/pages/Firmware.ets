import { picker } from '@kit.CoreFileKit';
import { promptAction, router, window } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';
import { DfuClient, DfuScanResult, EPD_SERVICE_UUID, DFU_SERVICE_UUID, DFU_ADV_NAME } from '../common/DfuClient';
import { BleClient } from '../common/BleClient';
import { FirmwarePackage, loadFirmwarePackage } from '../common/FirmwarePackage';

@Entry
@Component
struct Firmware {
  @State busy: boolean = false;
  @State status: string = '请选择固件包';
  @State progress: number = 0;
  @State logs: string[] = [];
  @State info: string = '';
  @State pkgReady: boolean = false;
  @State errorMsg: string = '';
  @State warnMsg: string = '';
  @State deviceInfo: string = '';
  @State inBootloader: boolean = false;
  private pkg: FirmwarePackage | null = null;
  private client: DfuClient = new DfuClient();

  aboutToAppear(): void {
    window.getLastWindow(getContext(this)).then((w: window.Window) => {
      w.setWindowKeepScreenOn(true);
    }).catch((e: Error) => {
    });
    const code: string = AppStorage.get<string>('driverCode') ?? '-';
    const size: string = AppStorage.get<string>('deviceSize') ?? '-';
    const ver: number = AppStorage.get<number>('appVersion') ?? 0;
    this.deviceInfo = '设备：驱动 ' + code + ' / ' + size + ' / 当前固件 0x' + ver.toString(16);
  }

  aboutToDisappear(): void {
    window.getLastWindow(getContext(this)).then((w: window.Window) => {
      w.setWindowKeepScreenOn(false);
    }).catch((e: Error) => {
    });
  }

  private log(m: string): void {
    console.info('FW: ' + m);
    const next: string[] = this.logs.concat([m]);
    this.logs = next.length > 120 ? next.slice(next.length - 120) : next;
  }

  private delay(ms: number): Promise<void> {
    return new Promise<void>((resolve: () => void) => {
      setTimeout(() => {
        resolve();
      }, ms);
    });
  }

  private async pickPackage(): Promise<void> {
    if (this.busy) {
      return;
    }
    try {
      const ctx0 = getContext(this) as common.UIAbilityContext;
      const p = new picker.DocumentViewPicker(ctx0);
      const opts: picker.DocumentSelectOptions = {
        maxSelectNumber: 1,
        fileSuffixFilters: ['.zip']
      };
      const uris = await p.select(opts);
      if (uris.length === 0) {
        return;
      }
      const ctx = getContext(this) as common.UIAbilityContext;
      this.status = '正在解析固件包...';
      this.pkgReady = false;
      const pkg = await loadFirmwarePackage(ctx, uris[0]);
      this.pkg = pkg;
      this.pkgReady = true;
      this.info = '包：' + pkg.fileName + '\n固件 0x' + pkg.init.fwVersion.toString(16) +
        ' / 硬件 ' + pkg.init.hwVersion.toString() + ' / SD 0x' + pkg.init.sdReq.toString(16) +
        ' / 镜像 ' + pkg.binSize.toString() + ' 字节';
      this.validate();
      this.status = this.errorMsg.length > 0 ? '包不可用' : '包已就绪';
    } catch (err) {
      this.pkgReady = false;
      this.errorMsg = '解析固件包失败: ' + (err as Error).message;
      this.status = '包不可用';
    }
  }

  private validate(): void {
    this.errorMsg = '';
    this.warnMsg = '';
    const pkg = this.pkg;
    if (pkg === null) {
      this.errorMsg = '请选择固件包';
      return;
    }
    if (pkg.init.hwVersion !== 0x34) {
      this.errorMsg = '硬件版本不匹配：包要求 0x' + pkg.init.hwVersion.toString(16) + '，本设备为 nRF52 (0x34)';
      return;
    }
    if (pkg.init.sdReq !== 0x126) {
      this.errorMsg = 'SoftDevice 不匹配：包要求 0x' + pkg.init.sdReq.toString(16) + '，本设备为 S112 7.3.0 (0x126)';
      return;
    }
    const size: string = AppStorage.get<string>('deviceSize') ?? '';
    if (size.length === 0) {
      this.errorMsg = '请先在“连接”页连接设备，识别型号/驱动后再升级';
      return;
    }
    const name: string = pkg.fileName;
    const is75: boolean = size.startsWith('7.5');
    if (name.includes('4.2') && is75) {
      this.errorMsg = '型号不匹配：当前设备是 7.5 寸，包名包含 4.2';
      return;
    }
    if (name.includes('7.5') && size.length > 0 && !is75) {
      this.errorMsg = '型号不匹配：当前设备是 ' + size.split('_')[0] + ' 寸，包名包含 7.5';
      return;
    }
    const cur: number = AppStorage.get<number>('appVersion') ?? 0;
    if (cur > 0 && pkg.init.fwVersion >= 0 && pkg.init.fwVersion < cur) {
      this.warnMsg = '注意：包内固件 0x' + pkg.init.fwVersion.toString(16) + ' 低于设备当前 0x' + cur.toString(16) + '，属于降级刷写。';
    }
    if (!name.includes('4.2') && !name.includes('7.5')) {
      this.warnMsg += ' 包内不含型号/驱动信息，无法从包内容校验；请确认这是 7.5 寸三色（驱动 07）的包。';
    }
  }

  private async start(): Promise<void> {
    const pkg = this.pkg;
    if (pkg === null || this.errorMsg.length > 0 || this.busy) {
      return;
    }
    const res = await promptAction.showDialog({
      title: '确认升级固件',
      message: '升级过程中请勿断电，确保 4 颗 CR2450 电量充足。\n刷错型号会导致花屏，包与型号不符时请取消。',
      buttons: [{ text: '取消', color: '#666666' }, { text: '开始升级', color: '#D9534F' }]
    });
    if (res.index !== 1) {
      return;
    }
    this.busy = true;
    this.progress = 0;
    this.logs = [];
    this.status = '准备升级...';
    const client = this.client;
    client.onLog = (m: string): void => {
      this.log(m);
    };
    client.onProgress = (pct: number, text: string): void => {
      this.progress = pct;
      this.status = text + ' ' + Math.round(pct * 100) + '%';
    };
    try {
      this.log('升级流程 DFU-DISCONNECT-WAIT：等待设备断开后扫描');
      const pre: DfuScanResult[] = await client.scan(DFU_SERVICE_UUID, 4000, DFU_ADV_NAME);
      this.inBootloader = pre.length > 0;
      if (this.inBootloader) {
        this.log('检测到设备已在 DFU 模式（' + pre[0].name + '）');
      } else {
        this.status = '扫描墨水屏...';
        const list: DfuScanResult[] = await client.scan(EPD_SERVICE_UUID, 70000);
        let target: DfuScanResult | null = null;
        for (let i: number = 0; i < list.length; i++) {
          if (list[i].name.indexOf('NRF_EPD') >= 0) {
            target = list[i];
            break;
          }
        }
        if (target === null) {
          throw new Error('未在 70 秒内扫描到 NRF_EPD。请确认设备已上电；相册模式下每 60 秒广播约 2 秒，可重试');
        }
        this.log('连接 ' + target.name + ' (' + target.deviceId + ')');
        await client.connect(target.deviceId);
        if (!client.hasButtonless()) {
          throw new Error('该固件未提供 BLE OTA（未找到 DFU 服务）');
        }
        this.log('按官方流程：不配对，开启 indication 后写入 0x01');
        await client.writeButtonless();
        this.status = '等待设备重启断开连接...';
        await client.waitForDeviceDisconnect();
        client.disconnect();
        this.log('等待 DfuTarg 广播...');
        await this.delay(800);
      }
      let dfu: DfuScanResult | null = null;
      for (let attempt: number = 0; attempt < 6 && dfu === null; attempt++) {
        this.status = '扫描 DFU 设备...';
        const list: DfuScanResult[] = await client.scan(DFU_SERVICE_UUID, 5000, DFU_ADV_NAME);
        this.log('DFU 扫描 ' + (attempt + 1) + '/6：发现 ' + list.length + ' 个候选');
        for (let i: number = 0; i < list.length; i++) {
          if (list[i].name.indexOf('Dfu') >= 0) {
            dfu = list[i];
            break;
          }
        }
        if (dfu === null && list.length > 0) {
          dfu = list[0];
        }
        if (dfu === null) {
          await this.delay(500);
        }
      }
      if (dfu === null) {
        throw new Error('未发现 DFU 设备（DfuTarg）。设备可能未成功进入 bootloader，请重试');
      }
      this.inBootloader = true;
      this.log('连接 DFU ' + dfu.name + ' (' + dfu.deviceId + ')');
      await client.connect(dfu.deviceId);
      if (!client.hasDfuService()) {
        throw new Error('DFU 服务发现失败（bootloader 未提供 0xFE59 控制点）');
      }
      await client.run(pkg.dat, pkg.bin);
      this.progress = 1;
      this.status = '传输完成，设备正在重启';
      this.log('传输完成，等待设备以应用固件重新广播');
      client.disconnect();
      this.inBootloader = false;
      let back: boolean = false;
      let appDeviceId: string = '';
      for (let round: number = 0; round < 2 && !back; round++) {
        await this.delay(1200);
        const apps: DfuScanResult[] = await client.scan(EPD_SERVICE_UUID, 70000);
        for (let i: number = 0; i < apps.length; i++) {
          if (apps[i].name.indexOf('NRF_EPD') >= 0) {
            back = true;
            appDeviceId = apps[i].deviceId;
            break;
          }
        }
      }
      if (back) {
        this.status = '设备已重新广播，正在验证连接...';
        const verify = new BleClient();
        verify.onLog = (m: string): void => {
          this.log('验证: ' + m);
        };
        try {
          await verify.connect(appDeviceId);
          if (verify.appVersion === pkg.init.fwVersion) {
            this.status = '升级完成：已连接，版本 0x' + verify.appVersion.toString(16);
            this.log('升级验证通过：连接成功，版本 0x' + verify.appVersion.toString(16));
          } else {
            this.status = '设备已连接，但版本 0x' + verify.appVersion.toString(16) +
              ' 与包内 0x' + pkg.init.fwVersion.toString(16) + ' 不一致';
            this.log('升级验证：版本不一致');
          }
          await verify.disconnectGracefully('升级验证完成');
        } catch (e) {
          this.status = '设备已重新广播，但连接验证失败：' + (e as Error).message;
          this.log('升级验证失败：' + (e as Error).message);
        }
      } else {
        this.status = '传输完成；未检测到设备重新广播，请手动重连确认';
        this.log('未在等待窗口内扫描到 NRF_EPD，请在连接页重新扫描');
      }
    } catch (err) {
      const msg: string = (err as Error).message;
      this.status = '升级失败: ' + msg;
      this.log('失败: ' + msg);
    } finally {
      this.busy = false;
      try {
        client.disconnect();
      } catch (e) {
      }
    }
  }

  build() {
    Column({ space: 10 }) {
      Row() {
        Button('返回').backgroundColor('#666666').onClick(() => {
          router.back();
        })
        Text('固件升级').fontSize(18).fontWeight(FontWeight.Bold).layoutWeight(1).textAlign(TextAlign.Center)
        Text(' ').width(58)
      }.width('100%')

      Text(this.deviceInfo).fontSize(12).fontColor('#666666').width('100%')

      Button('选择固件包 (.zip)')
        .width('100%')
        .enabled(!this.busy)
        .onClick(() => {
          this.pickPackage();
        })

      if (this.info.length > 0) {
        Text(this.info).fontSize(12).width('100%').fontColor('#333333')
      }

      if (this.errorMsg.length > 0) {
        Text('✕ ' + this.errorMsg).fontSize(12).fontColor('#C62828').width('100%')
      }
      if (this.warnMsg.length > 0) {
        Text('⚠ ' + this.warnMsg).fontSize(12).fontColor('#B26A00').width('100%')
      }

      Button(this.busy ? '升级中...' : '开始升级')
        .width('100%')
        .backgroundColor('#D9534F')
        .enabled(!this.busy && this.pkgReady && this.errorMsg.length === 0)
        .onClick(() => {
          this.start();
        })

      if (this.busy || this.progress > 0) {
        Progress({ value: this.progress * 100, total: 100, type: ProgressType.Linear })
          .width('100%')
      }
      Text(this.status).fontSize(13).fontColor('#2563EB').width('100%')

      Text('日志').fontSize(14).fontWeight(FontWeight.Bold).width('100%')
      List() {
        ForEach(this.logs, (line: string) => {
          ListItem() {
            Text(line).fontSize(11).fontColor('#333333').width('100%')
          }
        }, (line: string, idx: number) => idx.toString() + line)
      }
      .layoutWeight(1)
      .width('100%')
      .backgroundColor('#F5F5F5')
      .borderRadius(8)
    }
    .padding(12)
    .width('100%')
    .height('100%')
  }
}
