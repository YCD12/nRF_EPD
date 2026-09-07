import {
  Callout,
  Card,
  CardBody,
  CardHeader,
  Code,
  CollapsibleSection,
  Divider,
  Grid,
  H1,
  H2,
  H3,
  Pill,
  Row,
  Stack,
  Stat,
  Table,
  Text,
  UsageBar,
  computeDAGLayout,
  useHostTheme,
} from "cursor/canvas";

const FILE_LINES = {
  epdService: 2576,
  gfx: 1560,
  gui: 1469,
  webPaint: 1771,
  webMain: 1513,
  main: 1025,
  lunar: 856,
  eptJs: 808,
  driver: 629,
  flash: 543,
  uc8179: 516,
  emulator: 298,
};

const APP_C_LINES =
  FILE_LINES.epdService +
  FILE_LINES.gui +
  FILE_LINES.main +
  FILE_LINES.gfx +
  FILE_LINES.lunar +
  FILE_LINES.driver +
  FILE_LINES.uc8179 +
  FILE_LINES.flash;

function ArchitectureDag() {
  const theme = useHostTheme();
  const layout = computeDAGLayout({
    direction: "horizontal",
    nodeWidth: 132,
    nodeHeight: 36,
    rankGap: 56,
    nodeGap: 18,
    padding: 8,
    nodes: [
      { id: "web" },
      { id: "timer" },
      { id: "svc" },
      { id: "gui" },
      { id: "gfx" },
      { id: "uc" },
    ],
    edges: [
      { from: "web", to: "svc" },
      { from: "timer", to: "svc" },
      { from: "svc", to: "gui" },
      { from: "gui", to: "gfx" },
      { from: "gfx", to: "uc" },
      { from: "svc", to: "uc" },
    ],
  });

  const labels: Record<string, string> = {
    web: "Web BLE / 传图",
    timer: "1Hz 软时钟",
    svc: "EPD_service",
    gui: "DrawGUI",
    gfx: "Adafruit_GFX",
    uc: "UC8179",
  };

  return (
    <svg
      width="100%"
      viewBox={`0 0 ${layout.width} ${layout.height}`}
      role="img"
      aria-label="固件数据流：Web 与定时器进入 EPD_service，再经 GUI / GFX 到 UC8179"
    >
      {layout.edges.map((edge) => (
        <line
          key={`${edge.from}-${edge.to}`}
          x1={edge.sourceX}
          y1={edge.sourceY}
          x2={edge.targetX}
          y2={edge.targetY}
          stroke={theme.stroke.secondary}
          strokeWidth={1}
        />
      ))}
      {layout.nodes.map((node) => (
        <g key={node.id}>
          <rect
            x={node.x}
            y={node.y}
            width={132}
            height={36}
            rx={4}
            fill={node.id === "svc" ? theme.fill.tertiary : theme.bg.elevated}
            stroke={node.id === "svc" ? theme.accent.primary : theme.stroke.primary}
          />
          <text
            x={node.x + 66}
            y={node.y + 23}
            textAnchor="middle"
            fill={theme.text.primary}
            fontSize={11}
          >
            {labels[node.id]}
          </text>
        </g>
      ))}
    </svg>
  );
}

export default function FirmwareLogicReview() {
  const theme = useHostTheme();

  return (
    <Stack gap={24} style={{ padding: 24, maxWidth: 1180 }}>
      <Stack gap={8}>
        <H1>固件逻辑结构分析</H1>
        <Text tone="secondary">
          当前是一次「外置 Flash UI 栈回退 + UC8179 真 OLD/NEW 局刷内联」的中间态。
          产品路径完整，真正的重复几乎全在 C 内部，不在 Web。
        </Text>
        <Text size="small" tone="tertiary">
          源码快照 · 2026-09-05 · 对照 main.c / GUI.c / EPD_service.c / html/js
        </Text>
      </Stack>

      <Grid columns={4} gap={12}>
        <Stat value="4" label="显示模式" tone="info" />
        <Stat value="2,576" label="EPD_service.c 行" tone="warning" />
        <Stat value="~140" label="月历格重复行" tone="warning" />
        <Stat value="12" label="时钟模式判断副本" />
      </Grid>

      <Callout tone="warning" title="优先收敛三处，不要重做 Web 日历">
        DrawMonthDays 与 DrawCalendar_Right 几乎逐行重复；7 段时钟几何写了三份；
        刷新决策散落在 ble_epd_on_timer / epd_gui_update / epd_clock_partial_try。
        html/ 是 BLE 控制台和传图编辑器，没有重画固件页面。
      </Callout>

      <H2>运行路径</H2>
      <ArchitectureDag />
      <Text size="small" tone="tertiary">
        数据流：Web BLE 与 1Hz 软时钟进入 EPD_service，再经 DrawGUI / GFX 到 UC8179。
        中间加粗的是编排层，协议、刷新策略、EPT1、时钟差分目前都堆在这里。
      </Text>

      <Grid columns="1.2fr 0.8fr" gap={16}>
        <Stack gap={10}>
          <H3>产品实际做什么</H3>
          <Table
            headers={["能力", "入口", "刷新"]}
            striped
            rows={[
              ["图片", "MODE_PICTURE", "BLE 写 RAM，无 DrawGUI"],
              ["全屏月历", "MODE_CALENDAR", "每天 00:00 全刷"],
              ["左时钟 + 右日历", "MODE_CLOCK_CALENDAR", "每分钟局刷 HH:MM"],
              ["全屏时钟", "MODE_CLOCK", "每分钟局刷，几何另一套"],
              ["低电量页", "DrawLowBatteryTip", "<= 2800 mV 替换 UI"],
              ["EPT1 传图", "0x34–0x3B", "PackBits + 实心填充"],
              ["无按钮 DFU", "buttonless DFU", "SoftDevice 关闭后 SYSTEMOFF"],
            ]}
          />
        </Stack>
        <Card>
          <CardHeader trailing={<Pill tone="warning" active size="sm">God file</Pill>}>
            EPD_service.c
          </CardHeader>
          <CardBody>
            <Stack gap={8}>
              <Text size="small">
                一个翻译单元同时拥有 GATT 协议、命令队列、EPT1 解码、电池保护、刷新策略、时钟 OLD/NEW。
              </Text>
              <Text size="small" tone="secondary">
                GUI.c 1469 行是第二热点：全屏与分屏各写一套月历和时钟。
              </Text>
              <Text size="small" tone="tertiary">
                Adafruit_GFX.c 1560 行是第三方绘图栈，不是重复逻辑。
              </Text>
            </Stack>
          </CardBody>
        </Card>
      </Grid>

      <H2>重复与漂移</H2>
      <Table
        headers={["问题", "位置", "代价", "建议"]}
        striped
        rowTone={["danger", "warning", "warning", "warning", "info", "neutral"]}
        rows={[
          [
            "月历格双实现",
            "DrawMonthDays / DrawCalendar_Right",
            "节日、调休、高亮要改两处",
            "抽 DrawMonthGrid(rect, options)",
          ],
          [
            "星期栏内联复制",
            "DrawWeekHeader vs Right L797",
            "周末配色易漂",
            "右侧直接复用 DrawWeekHeader",
          ],
          [
            "7 段几何三份",
            "DrawClock_Left / ClockOnly / GetTimeRefreshArea",
            "改字号必须三处同步",
            "clock_layout.h 单一常量",
          ],
          [
            "时钟上一分钟双状态",
            "GUI s_last_time_refresh_ts + service 缓存",
            "窗口缩小与 OLD 位图语义分裂",
            "抽出 epd_clock_refresh.c",
          ],
          [
            "刷新 if 链分散",
            "on_timer + gui_update + partial_try",
            "45s 消抖 / 跨天 / 夜间难单测",
            "epd_refresh_decide()",
          ],
          [
            "模式注释反了",
            "EPD_service.h SET_MODE",
            "写成 2=时钟、3=分屏",
            "与 GUI.h 枚举对齐",
          ],
        ]}
      />

      <H2>应用 C 行数分布</H2>
      <UsageBar
        total={APP_C_LINES}
        topLeftLabel="应用 C 源（不含 SDK / Web）"
        topRightLabel={`${APP_C_LINES.toLocaleString()} 行`}
        segments={[
          { id: "svc", value: FILE_LINES.epdService, color: "orange" },
          { id: "gfx", value: FILE_LINES.gfx, color: "blue" },
          { id: "gui", value: FILE_LINES.gui, color: "purple" },
          { id: "main", value: FILE_LINES.main, color: "green" },
          { id: "lunar", value: FILE_LINES.lunar, color: "yellow" },
          { id: "drv", value: FILE_LINES.driver, color: "pink" },
          { id: "uc", value: FILE_LINES.uc8179, color: "gray" },
          { id: "flash", value: FILE_LINES.flash, color: "red" },
        ]}
      />
      <Text size="small" tone="tertiary">
        段从左到右：EPD_service 2576、Adafruit_GFX 1560、GUI 1469、main 1025、Lunar 856、
        EPD_driver 629、UC8179 516、fm25q04b 543。fm25q04b 已编进固件但无调用方。
      </Text>

      <H2>更优拆法（行为不变）</H2>
      <Grid columns={3} gap={12}>
        <Card>
          <CardHeader trailing={<Pill tone="success" active size="sm">先做</Pill>}>
            抽出时钟刷新
          </CardHeader>
          <CardBody>
            <Text size="small">
              把 <Code>epd_clock_partial_try</Code>、OLD 缓存、capture/diff 回调迁到
              <Code>EPD/epd_clock_refresh.c</Code>。服务层只调 try()。恢复可单测性，不改刷屏语义。
            </Text>
          </CardBody>
        </Card>
        <Card>
          <CardHeader trailing={<Pill active size="sm">高收益</Pill>}>
            合并月历格
          </CardHeader>
          <CardBody>
            <Text size="small">
              全屏与分屏共用单元格循环；布局用 origin_x / width / bottom。
              节日和调休以后只改一处。
            </Text>
          </CardBody>
        </Card>
        <Card>
          <CardHeader trailing={<Pill tone="warning" active size="sm">可测</Pill>}>
            刷新策略函数
          </CardHeader>
          <CardBody>
            <Text size="small">
              <Code>epd_refresh_decide(ts, flags)</Code> 返回是否刷、全/局。
              用表驱动覆盖跨天、45s 消抖、低电、EPT 会话、1–5 点静默。
            </Text>
          </CardBody>
        </Card>
      </Grid>

      <H3>不要走的路</H3>
      <Callout tone="neutral" title="Web 不要再画一套日历">
        html/js 没有农历、节日、7 段数码管。Windows emulator.c 已经直接调 DrawGUI。
        需要预览时扩展模拟器支持 MODE_CLOCK_CALENDAR，或做主机 golden，不要在 JS 复刻 GUI.c。
      </Callout>

      <Divider />

      <CollapsibleSection title="协议、默认值和死代码" defaultOpen>
        <Stack gap={12}>
          <Table
            headers={["项", "固件", "Web / 模拟器", "影响"]}
            rows={[
              [
                "SET_MODE 0x22",
                "已实现",
                "EpdCmd 未列出，靠 SET_TIME 第 7 字节切模式",
                "控制面与头文件不同步",
              ],
              [
                "EPT 0x34–0x3B",
                "EPD_service.c",
                "只在 ept_transfer.js",
                "main.js 协议表不完整",
              ],
              [
                "week_start 默认",
                "0 = 周日",
                "DOM 默认 1 = 周一",
                "首次连接周列可能不一致",
              ],
              [
                "低电阈值",
                "EPD_driver.h 2800/2700",
                "main.js 再写一份 2800",
                "改阈值要两处",
              ],
              [
                "fm25q04b.c",
                "Makefile 仍链接 543 行",
                "资源系统已删",
                "占 Flash，无调用",
              ],
              [
                "emulator_reload_ui()",
                "无定义",
                "按 R 键会调用",
                "Win32 模拟器链接可能失败",
              ],
              [
                "lunar_db 注释",
                "ble_epd_on_timer L2521",
                "外置 DB 已删",
                "误导后续改动",
              ],
            ]}
          />
          <Text size="small" tone="secondary">
            分层上也有小耦合：EPD_service.h include GUI.h；GUI.c include EPD_driver.h
            只为电压宏；UC8179.c 用 extern app_feed_wdt。这些适合小头文件整理，不是功能问题。
          </Text>
        </Stack>
      </CollapsibleSection>

      <Stack gap={6} style={{ borderTop: `1px solid ${theme.stroke.tertiary}`, paddingTop: 12 }}>
        <Text size="small" tone="tertiary">
          建议落地顺序：clock_layout.h → 月历格合并 → epd_clock_refresh.c →
          epd_refresh_decide() → 删或隔离 fm25q04b / 修 emulator。每年节日表可再生成 calendar_data.c。
        </Text>
      </Stack>
    </Stack>
  );
}
