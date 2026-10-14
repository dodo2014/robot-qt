# 视觉检测页（`VisionTestPage`）重构方案

> **需求来源**：用户 2026-09-17 提供的架构指令（由 Gemini 整理）第二份——「重构视觉检测专业调试页面」。
> **本文性质**：**🟡 已实施（2026-09-20，台账 TR-100）**。§十 的 6 项未决项已由用户裁定「**全部按方案推荐**」并落地（见该节末列）；§九 的「编译 / Sim 冒烟」判据已满足，**真机三项仍待人工执行**。文中现状事实均带 `file:line`，是实施前的快照，可用源码复核。
> **关联文档**：`doc/camera_integration_plan.md`（相机接入，本页依赖其 S2 设备枚举与 §2.1 持有者模型）、`doc/vision_guide_action_plan.md`（视觉引导动作，与本页共享 `PuffResult` 语义）、`doc/archive/ui_interlock_plan.md`（TR-075 互锁，本页 §6.2 对其有一处修正）。
> **补记说明**：本文是 2026-09-17 会话中**遗漏项的补齐**——该需求此前只被压缩进 `camera_integration_plan.md` 的 S5 一节（约 8 行），原始需求里的**控件规格基本丢失**。本文是它的完整落点。

## 一、需求整理（控件规格）

### 1.1 整体布局

```
┌──────────────┬───────────────────────────────────────────┐
│              │   ┌─────────────┐  ┌─────────────┐        │
│  左栏        │   │  RGB 彩色   │  │  深度 伪彩  │        │
│  320 px      │   └─────────────┘  └─────────────┘        │
│  QScrollArea │   ┌─────────────┐  ┌─────────────┐        │
│  （可滚动）  │   │  Overlay    │  │ 数据 + 日志 │        │
│              │   └─────────────┘  └─────────────┘        │
└──────────────┴───────────────────────────────────────────┘
```

- 左栏**固定 320 px 宽**，内容超出时垂直滚动（`QScrollArea`）。
- 右侧 **2×2 四宫格**，`QGridLayout`，四格等分拉伸，随窗口自适应。
- 画面区背景 `#0b0d0f`（与现有画面底色一致，见 `VisionTestPage::SetupUI` 的画面区样式），细边框分隔。

### 1.2 左栏 · 「相机控制」组

| 控件 | 类型 | 说明 |
| :--- | :--- | :--- |
| 刷新设备 | `QPushButton` | 触发设备枚举，填充下方 SN 下拉 |
| 序列号 (SN) | `QComboBox` | **从枚举结果填充**，不再手填 |
| 彩色流 · 分辨率 | `QComboBox` | 按 SDK 枚举结果填充 |
| 彩色流 · 格式 | `QComboBox` | 同上 |
| 彩色流 · 帧率 | `QComboBox` | 同上 |
| 深度流 · 分辨率 | `QComboBox` | 同上 |
| 深度流 · 格式 | `QComboBox` | 同上 |
| 深度流 · 帧率 | `QComboBox` | 同上 |
| D2C 对齐（软件） | `QCheckBox` | 勾选 = 走 `ob::Align` 逐帧滤波 |
| 开始采集 | `QPushButton` | — |
| 停止采集 | `QPushButton` | — |

**明确移除**：曝光控制（自动曝光 / 曝光值 1~10000）——本版固定自动曝光，见 `camera_integration_plan.md` 决策 4。

### 1.3 左栏 · 「算法测试」组

| 控件 | 类型 | 说明 |
| :--- | :--- | :--- |
| 算法引擎 | `QComboBox` | 从 `PuffAlgorithmFactory` 注册表填充 |
| 模板配置路径 | `QLineEdit`（只读） | **可选填**；留空则用算法内置默认值 |
| 浏览… | `QPushButton` | `QFileDialog` 选 json |
| 保存 RGB | `QCheckBox` | — |
| 保存 Depth | `QCheckBox` | — |
| 保存 Overlay | `QCheckBox` | — |
| 单次检测 | `QPushButton` | — |
| 连续检测 | `QPushButton`（可选中） | — |
| 加载本地图片 | `QPushButton` | 离线图检测 |

**「算法参数不上面」**：需求明确要求算法参数不上 UI，一律由配置驱动。

### 1.4 右侧四宫格

| 格位 | 内容 |
| :--- | :--- |
| 左上 | RGB 彩色画面 |
| 右上 | 深度伪彩画面 |
| 左下 | Overlay 叠加图（识别框） |
| 右下 | 检测结果表 + 运行日志 |

三格画面**同时显示**——这与现状的根本差别：现有页面是**单画面 + 视图切换按钮**（`rgbBtn_` / `depthBtn_`，`:389-402`），一次只能看一路。

### 1.5 需求未提及但现状存在、需处置的功能

| 功能 | 现状位置 | 需求覆盖情况 |
| :--- | :--- | :--- |
| 相机型号切换（Sim / Orbbec） | `cameraTypeCombo_` `:196-198` | **未提及**——但必须保留（M5：切型号需重建实例） |
| 相机状态指示灯 | `cameraStateDot_` / `cameraStateText_` `:217-226` | 未提及 |
| 检测参数（置信度 / 深度 Z min / Z max） | `confSpin_` / `zminSpin_` / `zmaxSpin_` `:327-346` | **被「算法参数不上面」覆盖**——但三者是直写 `ConfigManager` 的 |
| 打开 / 关闭相机 | `:230-240` | 未提及（被「开始采集 / 停止采集」取代？见 V4） |
| 保存截图 | `:420-424` | 被三路存图开关取代 |
| 状态文本 | `statusLabel_` `:358-361` | 未提及（日志区可承接） |
| FPS / 时间戳 / 分辨率 | `infoLabel_` `:414-416` | 未提及 |
| 识别框显示开关 | `overlayBtn_` `:404-410` | 未提及（Overlay 独立成格后语义变化） |

## 二、现状清点

### 2.1 控件与槽函数全清单

| 控件 / 成员 | 定义位置 | 槽函数 | 行为 |
| :--- | :--- | :--- | :--- |
| `cameraTypeCombo_` | `.cpp:196-198` | — | 填充自 `CameraFactory::AvailableTypes()`（`:111-114`） |
| `deviceIdEdit_` | `.cpp:200-204` | — | 序列号**手填**，默认 `CAM-SIM-001` |
| `resolutionCombo_` | `.cpp:206-209` | — | 硬编码 `640x480` / `1280x720` |
| `fpsCombo_` | `.cpp:211-215` | — | 硬编码 `15/30/60`，默认 30 |
| `cameraStateDot_` / `cameraStateText_` | `.cpp:217-226` | `UpdateCameraState()` `:431-446` | 三态：未打开 / 已打开 / 采集中 |
| openBtn `openCamBtn` | `.cpp:230-234` | `OnOpenCamera` `:448-471` | `CameraOpen(w,h,fps)` |
| closeBtn `closeCamBtn` | `.cpp:236-240` | `OnCloseCamera` `:473-481` | `CameraClose()` |
| startBtn `startStreamBtn` | `.cpp:242-246` | `OnStartStream` `:483-508` | 未打开则先 `CameraOpen`，再 `StartCameraStream` |
| stopBtn `stopStreamBtn` | `.cpp:248-252` | `OnStopStream` `:510-518` | `StopCameraStream()` |
| `algoCombo_` | `.cpp:264-267` | — | 填充自 `PuffAlgorithmFactory::AvailableTypes()`（`:116-119`） |
| detectBtn `singleDetectBtn` | `.cpp:271-275` | `OnSingleDetect` `:530-538` | 对 `latestFrame_` 跑一次检测 |
| `continuousBtn_` `continuousBtn` | `.cpp:277-282` | `OnToggleContinuous` `:582-587` | 每帧 `frameReady` 都检测 |
| loadImgBtn `loadImgBtn` | `.cpp:284-288` | `OnLoadImage` `:668-700` | 离线图 → `CameraFrame` → 检测 |
| `resultTable_` | `.cpp:291-304` | `ShowResults` `:559-580` | **10 列**，最大高 190 px |
| `confSpin_` / `zminSpin_` / `zmaxSpin_` | `.cpp:327-346` | — | 直写 `vision.confidenceThreshold` / `depthZMin` / `depthZMax` |
| `exposureSpin_` | `.cpp:348-353` | — | 直写 `vision.exposure` |
| `statusLabel_` | `.cpp:358-361` | `SetStatus` `:718-722` | 单行状态 + `SPDLOG_INFO` |
| `previewLabel_` | `.cpp:373-384` | `RenderLatest` `:614-633` | **单画面**，按 `depthView_` 二选一 |
| `rgbBtn_` `rgbBtn` | `.cpp:389-395` | `OnShowRGB` `:596-603` | 切 `depthView_ = false` |
| `depthBtn_` `depthBtn` | `.cpp:397-402` | `OnShowDepth` `:605-612` | 切 `depthView_ = true` |
| `overlayBtn_` `overlayBtn` | `.cpp:404-410` | `OnToggleOverlay` `:589-594` | 控制是否画识别框 |
| `infoLabel_` | `.cpp:414-416` | `RenderLatest` `:628-632` | FPS / 时间戳 / 分辨率 |
| saveBtn `saveBtn` | `.cpp:420-424` | `OnSnapshot` `:651-666` | 存当前显示图到 `snapshots/` |
| `frameSaver_` + `saverThread_` | `.cpp:125-133` | `OnImageSaved` `:708` / `OnSaveError` `:713` | 独立线程异步存图 |
| `fpsTimer_` | `.cpp:135-137` | `OnFpsTick` `:702-706` | 每秒统计帧数 |
| `frameReady` 订阅 | `.cpp:123` | `OnFrameReady` `:520-528` | 缓存帧 + 渲染 + 可选检测 |

**对外契约只有一个**：`SetParamsLocked(bool)`（声明在 `VisionTestPage.h`），由 `MainWindow::ApplyModeState` 在模式切换时调用。**重构必须保持该签名不变**。

### 2.2 现存缺陷（重构时一并处理）

| # | 位置 | 缺陷 |
| :--- | :--- | :--- |
| D1 | `.cpp:462` | ~~`OnOpenCamera` 内无条件 `deviceIdEdit_->setText("CAM-SIM-001")`，**把用户填的 SN 覆盖掉**——SN 输入框形同虚设。已在 `camera_integration_plan.md` 记为 M2，属 S0 前置修复~~ **✅ 已于 P1（C2，2026-09-20）修复**：该行已删除，`deviceIdEdit_` 改为 `communication.camera.sn` 的真实写入口（构造期读入 + `editingFinished` 直写）。**P4 无需重复处理** |
| D2 | `.cpp:206-215` | 分辨率与帧率**硬编码**，接真机后选到不支持的组合会直接开流失败 |
| D3 | `.cpp:520-528` | `OnFrameReady` 每帧都 `RenderLatest()`（`QImage::copy` + `setPixmap` + `SmoothTransformation` 缩放）；重构后**三格同时刷新，开销 ×3** |
| D4 | `FrameSaver.cpp:47-52`（`queue_.push_back` 在 `:51`） | `SaveImage` 对 `queue_` 是**无界 `push_back`**，无上限保护。三路存图 + 连续检测下每帧 3 张（1280×720 `QImage` ≈ 3.5 MB）→ 队列无界增长至 OOM |
| D5 | `FrameSaver.cpp:54-84`（`while(true)` 在 `:62`，`emit imageSaved` 在 `:78`） | `ProcessQueue` 是 `while(true)` **无预算排空**：一次 tick 会把积压全部写完（单张 PNG 编码 + 落盘约 10~30 ms），积压 90 张时该线程连续阻塞数十秒；且**每张都 `emit imageSaved`**，UI 每秒收 90 个状态刷新信号 + spdlog 刷屏 |
| D6 | `.cpp:157-159` 注释 | TR-075 的「操作类控件保持可用」约定与相机流持有者模型冲突——见 §6.2 |
| D7 | `.cpp:596-612` | `OnShowRGB` / `OnShowDepth` 手写互斥（`setChecked` 两处），重构后视图切换按钮删除，这段逻辑整体废弃 |

## 三、需求评审：须修正项

| # | 需求原文 | 事实 / 问题 | 处理建议 |
| :--- | :--- | :--- | :--- |
| **V1** | 彩色流格式预置含 `Y16` | **`Y16` 是深度 / 红外格式，彩色流没有 Y16**；高分辨率彩色实际多为 MJPG（参考实现注释：「1280×720 不支持 RGB，优先使用 MJPG」） | 彩色格式列表**按 SDK 实际枚举结果**填充，不照抄预置项（同 `camera_integration_plan.md` E7） |
| **V2** | 分辨率预置 `640x480 / 848x480 / 960x540 / 1280x720 / 1920x1080` | 预置项是**假设**，该机型实际支持集未知；硬编码选到不支持的组合直接开流失败 | 从 `pipeline->getStreamProfileList(OB_SENSOR_COLOR/DEPTH)` **枚举后填充**（同 E8） |
| **V3** | 右下结果表列：`序号, X, Y, Z, R, 耗时ms`（6 列） | 现有表为 **10 列**（`序号/置信度/X/Y/Z/偏航/U/V/宽/高`，`.cpp:292-298`）。进算法阶段仍需 置信度 / U / V / 宽 / 高 做调参定位 | **保留 10 列 + 追加「耗时 ms」= 11 列**（同 E10） |
| **V4** | 相机控制组只有「开始采集 / 停止采集」 | 现状有**两对**语义：`CameraOpen`/`CameraClose`（设备）与 `StartCameraStream`/`StopCameraStream`（流）。`OnStartStream` 内部已自动补 `CameraOpen`（`:491-497`）→ 只留一对是合理的；但 `CameraClose` 将**无 UI 入口**，相机打开后不采集就一直占着设备 | **需裁定**：① 停止采集后自动 `CameraClose`（推荐，简单）；② 保留一个「关闭相机」按钮；③ 停止采集只停流、设备保持打开（切流参数时免重连，但拔线前必须先关） |
| **V5** | 「算法参数不上面」 | 现有 `confSpin_` / `zminSpin_` / `zmaxSpin_` 是**直写 `ConfigManager`** 的（`:329-346`），移除后这 3 个参数**只剩手改 `config.json`** 一条路。**更关键的是这三个参数的生效情况完全不同**（2026-09-18 核实）：`depthZMin` / `depthZMax` 由 `SimAlgo::Detect`（`SimAlgo.cpp:30-32`）**每次检测实时读**并在 `:92` 做深度筛选 → **生效**；`confidenceThreshold` 唯一真实消费者是 `VisionTestPage.cpp:645` 的叠加框绘制 → **仅影响显示**；`HardwareManager.cpp:183` 在 `Initialize` 时把它喂给 `algo->LoadConfig()`，而 **`SimAlgo::LoadConfig`（`SimAlgo.cpp:122-126`）是空实现**（只把 json 存入成员 `config_` 并打印字节数，从不解析）→ **该通路未生效** | **需裁定**：① 一并移除，调参走 `config.json`（最贴合需求原意）；② 保留但收进可折叠区；③ 迁到配置页。**无论选哪个，都应先处理「有一条看起来在工作、实际不工作的配置通路」**——要么让 `LoadConfig` 真正生效，要么删掉该通路并说明本版不生效 |
| **V6** | 右下格内容「数据 + 日志」 | **「日志」在本项目没有 UI 落点**——日志走 spdlog 落 `log/creampuff_YYYY-MM-DD.log`，页面里只有 `statusLabel_` 单行文本。日志的来源与范围需求未定义 | **需裁定**（建议方案见 §7.3）：① 页面级环形缓冲（`SetStatus` + 检测耗时 + 帧率，上限 N 行）；② 接 spdlog Qt sink 全量回显（需改 logger 配置，跨线程）；③ 只给「打开日志目录」按钮 |
| **V7** | 四宫格右下放 11 列表格 | 2×2 等分后右下格宽约 460 px（1280 窗口），11 列 × 56 px = 616 px → **必然横向滚动**，同时日志就没地方放 | 建议右下用 `QTabWidget`（页签「检测结果」/「运行日志」），或把日志做成横跨底部的独立条。见 §5.2 |
| **V8** | 保存 RGB / Depth / Overlay 三开关 | **触发时机未定义**。若语义是「每帧都存」，连续检测 30 fps × 3 路 = 90 张/秒 → 秒级爆盘 + 命中 D4/D5 两个缺陷 | 建议语义：**仅「单次检测」时按开关落盘**；连续检测期间忽略自动存图，另留显式「保存当前帧」按钮。**需确认** |
| **V9** | 「刷新设备」按钮 | 在 `USE_ORBBEC=OFF` 或 `SimCamera` 下，枚举无意义（`ICamera` 现无枚举接口，`camera_integration_plan.md` S2 才加） | `SimCamera` 下按钮置灰 + 提示；枚举失败时 SN 下拉回落为可编辑输入 |
| **V10** | 「D2C 对齐（软件）」复选框 | 与 `config.json` 的 `d2cAlign`（`camera_integration_plan.md` §六）构成**双事实源** | 复选框状态**写回 config**（`ConfigManager::set`），开流时读 config——与现有 spinbox 范式一致 |
| **V11** | 左栏 320 px + `QScrollArea` | 现有面板 330 px 已容纳 4 组；新增彩色 / 深度两套流参数（各 3 个 combo = 6 行）后高度显著增加，`QScrollArea` 是必需的 | 采纳需求；320 px 宽度下 combo 与标签并排会挤，建议标签改为**行内前缀**或 `QFormLayout` 窄标签 |
| **V12** | — | 现有按钮均有 `objectName`（`openCamBtn` / `startStreamBtn` / `continuousBtn` / `rgbBtn` …），供 UI 定位 | 重构后 objectName **重新定义并登记**（§5.4）；已核实**全库无外部引用**（仅 `VisionTestPage.cpp` 自身），改名安全 |

## 四、旧功能 → 新布局 映射表（防功能丢失）

> 重构最大的风险不是「新的没做」，而是「旧的悄悄没了」。下表逐项对账。

| # | 现有功能 | 新落点 | 处置 |
| :--- | :--- | :--- | :--- |
| 1 | 相机型号选择（Sim / Orbbec） | 「相机控制」组 · 相机型号 combo | **保留**（需求未提及但必需，M5） |
| 2 | 序列号手填 | 「相机控制」组 · SN **下拉框** | **改造**：手填 → 枚举填充；删 D1 覆盖行 |
| 3 | 分辨率（单一） | 彩色流 · 分辨率 + 深度流 · 分辨率 | **拆分 ×2**，改为枚举填充 |
| 4 | 帧率（单一） | 彩色流 · 帧率 + 深度流 · 帧率 | **拆分 ×2** |
| 5 | — | 彩色流 / 深度流 · 格式 | **新增** |
| 6 | — | D2C 对齐（软件）复选框 | **新增** |
| 7 | 打开 / 关闭相机 | 合并进「开始采集 / 停止采集」 | **合并**（待 V4 裁定关闭路径） |
| 8 | 开始采集 / 停止 | 「开始采集 / 停止采集」 | **保留**，内部改走持有者集合（§6.1） |
| 9 | 相机状态灯 | 「相机控制」组内保留 | **保留** |
| 10 | 单画面 + RGB / 深度切换按钮 | RGB 格 + Depth 格**同时显示** | **替换**：切换按钮删除（D7 逻辑废弃） |
| 11 | 识别框开关 `overlayBtn_` | Overlay 格标题栏复选框 | **迁移**：从底部按钮条移到格子标题 |
| 12 | FPS / 时间戳 / 分辨率 | 各格角落叠加 或 日志区 | **迁移**（建议：RGB 格右下角叠加，省纵向空间） |
| 13 | 保存截图 | 三路存图开关 + 显式「保存当前帧」 | **改造**（语义待 V8 确认） |
| 14 | 算法型号 combo | 「算法测试」组 | **保留** |
| 15 | — | 模板配置路径 + 浏览… | **新增**（可选填） |
| 16 | 检测参数 ×3（写 config） | 无 | **待 V5 裁定** |
| 17 | 曝光参数（写 config） | 无 | **删除**（决策 4） |
| 18 | 单次检测 | 「算法测试」组 | **保留**，加耗时统计 |
| 19 | 连续检测 | 「算法测试」组 | **保留** |
| 20 | 加载本地图片 | 「加载本地图片」 | **保留** |
| 21 | 10 列结果表 | 右下格 | **保留 10 列 + 耗时 ms** |
| 22 | 状态文本 `statusLabel_` | 日志区 / 面板底部 | **待 V6 裁定** |
| 23 | `FrameSaver` 异步存图线程 | 复用 | **保留 + 加固**（D4 / D5） |
| 24 | FPS 统计定时器 | 复用 | **保留** |
| 25 | `SetParamsLocked` 互锁 | 保留签名，改控件清单 | **保留 + 修正**（§6.2） |

## 五、实施规格

### 5.1 左栏结构

```
QScrollArea (setWidgetResizable(true), 固定宽 320)
└─ QWidget (content)
   ├─ MakeSection("相机控制", ...)
   │   ├─ 相机型号 / SN（+刷新设备）
   │   ├─ 彩色流：分辨率 / 格式 / 帧率
   │   ├─ 深度流：分辨率 / 格式 / 帧率
   │   ├─ [x] D2C 对齐（软件）
   │   ├─ ● 状态灯 + 文本
   │   └─ [开始采集] [停止采集]
   ├─ MakeSection("算法测试", ...)
   │   ├─ 算法引擎
   │   ├─ 模板配置 [路径只读] [浏览…]
   │   ├─ [x] 保存 RGB  [x] 保存 Depth  [x] 保存 Overlay
   │   └─ [单次检测] [连续检测] [加载本地图片]
   └─ statusLabel_（保留在面板底部，随滚动区滚动）
```

**复用现有 helper**：`MakeSection()`（`.cpp:61-79`）、`MakeLabel()`（`:81-86`）、`MakeCombo()`（`:88-102`）、`SetButtonActionStyle()` / `SetButtonCheckedStyle()`（`:38-59`）——全部可原样沿用，深色主题与圆角风格零改动。

**注意**：`QScrollArea` 内的 `QGroupBox` 若设固定宽会与滚动条打架，宽度应交给内容布局；`panel->setFixedWidth(320)`（现 `:185`）改为设在 `QScrollArea` 上。

### 5.2 右侧四宫格

```cpp
auto* grid = new QGridLayout();
grid->setSpacing(8);
grid->addWidget(rgbTile,     0, 0);
grid->addWidget(depthTile,   0, 1);
grid->addWidget(overlayTile, 1, 0);
grid->addWidget(dataTile,    1, 1);
grid->setRowStretch(0, 1);
grid->setRowStretch(1, 1);
grid->setColumnStretch(0, 1);
grid->setColumnStretch(1, 1);
```

**右下格（`dataTile`）两个候选**：

| 候选 | 结构 | 优点 | 缺点 |
| :--- | :--- | :--- | :--- |
| **A（推荐）** | `QTabWidget`：「检测结果」= 11 列表格；「运行日志」= `QPlainTextEdit`（只读） | 表格独占整格宽度，日志需要时才切过去；改动最小 | 日志与结果不能同屏对照 |
| B | 上表 + 下日志（`QSplitter`） | 同屏对照 | 格高被切两半，表格只剩 3~4 行可见 |
| C | 四宫格 + **底部独立日志条**（横跨右区，固定高 ~90 px） | 日志常驻、表格独占右下格 | 偏离需求原文的 2×2 描述 |

**建议采纳 A**，理由：需求写「数据 + 日志」两件事，`QTabWidget` 是 Qt 里表达「同一格两件事」的标准做法，且不改变 2×2 的等分结构。B / C 作为备选留档。

### 5.3 画面格子 helper（三格复用）

三格结构完全同构（标题 + 画面 + 角落信息），应抽一个私有 helper，避免三份重复代码：

```cpp
// VisionTestPage.cpp 匿名 namespace
struct PreviewTile {
    QWidget* frame = nullptr;   // 含边框的容器
    QLabel*  view  = nullptr;   // 画面
    QLabel*  info  = nullptr;   // 右下角叠加信息（FPS / 时间戳 / 分辨率）
};
PreviewTile MakePreviewTile(const QString& title, bool withToggle = false, QCheckBox** toggleOut = nullptr);
```

- `view` 的样式沿用现有 `previewLabel_` 的 QSS（`background: #0b0d0f; border-radius: 12px; border: 1px solid #3a424e;`，`:377-382`）。
- `info` 用半透明背景叠在 `view` 右下角（`QLabel` 绝对定位 或 `QGridLayout` 叠层），替代现有独立的 `infoLabel_`。
- Overlay 格的 `withToggle` = true，复选框标题「显示识别框」，默认勾选（对应现 `overlayBtn_->setChecked(true)` `:407`）。

### 5.4 `objectName` 清单（重构后）

| objectName | 控件 | 备注 |
| :--- | :--- | :--- |
| `refreshDeviceBtn` | 刷新设备 | 新增 |
| `cameraTypeCombo` | 相机型号 | 沿用语义 |
| `deviceSnCombo` | SN 下拉 | **原 `deviceIdEdit_` 改名** |
| `colorResCombo` / `colorFormatCombo` / `colorFpsCombo` | 彩色流三件套 | 新增 |
| `depthResCombo` / `depthFormatCombo` / `depthFpsCombo` | 深度流三件套 | 新增 |
| `d2cAlignCheck` | D2C 对齐 | 新增 |
| `startStreamBtn` / `stopStreamBtn` | 采集启停 | **沿用原名** |
| `algoCombo` | 算法引擎 | 沿用语义 |
| `templatePathEdit` / `browseTemplateBtn` | 模板配置 | 新增 |
| `saveRgbCheck` / `saveDepthCheck` / `saveOverlayCheck` | 存图三开关 | 新增 |
| `singleDetectBtn` / `continuousBtn` / `loadImgBtn` | 检测三按钮 | **沿用原名** |
| `rgbView` / `depthView` / `overlayView` | 三个画面 | 新增 |
| `overlayToggleCheck` | 识别框开关 | **原 `overlayBtn` 迁移** |
| `resultTable` | 结果表 | 沿用语义 |
| `logView` | 日志视图 | 新增（V6 定后） |
| `saveFrameBtn` | 保存当前帧 | **原 `saveBtn` 迁移** |
| ~~`openCamBtn`~~ / ~~`closeCamBtn`~~ | — | **删除**（V4 裁定后） |
| ~~`rgbBtn`~~ / ~~`depthBtn`~~ / ~~`overlayBtn`~~ | — | **删除**（视图切换按钮废弃） |

## 六、状态与互锁

### 6.1 相机流持有者集合

本页的「开始采集 / 停止采集」不再直接调 `StartCameraStream` / `StopCameraStream`（现 `.cpp:499` / `:514`），改为：

| 本页动作 | 调用 |
| :--- | :--- |
| 开始采集 | `HardwareManager::AcquireCamera("VisionPage")` |
| 停止采集 | `HardwareManager::ReleaseCamera("VisionPage")` |

模型与三类故障场景见 `camera_integration_plan.md` §2.1。要点：**视觉页释放时若自动运行仍持有，则不真停**，状态栏提示「自动运行仍在使用相机」。

### 6.2 `SetParamsLocked` 控件清单（对 TR-075 的修正）

P4 前实现锁定 9 个参数控件，注释声明「相机开关、采集、检测、视图切换、截图等操作类控件保持可用」。**该约定必须改**：自动运行期间视觉页的采集启停会与自动运行争相机。**已按本表落地**：`VisionTestPage::SetParamsLocked` 的 `params` 列表（相机型号 / SN / 刷新设备、彩色流与深度流 6 件套、D2C 对齐、算法引擎 / 模板配置）统一置灰；**采集启停按钮不在此列表**——它们由 `UpdateButtons()` 按 `locked_` 统一算。

| 控件 | 自动运行期间 | 变化 |
| :--- | :--- | :--- |
| 相机型号 / SN / 刷新设备 | **置灰** | 原已置灰 |
| 彩色流 / 深度流 6 件套 | **置灰** | 新增置灰 |
| D2C 对齐 | **置灰** | 新增置灰 |
| **开始采集** | **置灰** | **⚠ 由「可用」改为置灰**（本次修正核心） |
| **停止采集** | **可用**（仅在本页持流时） | ⚠ **2026-09-23 再修正**（用例 11.26，TR-110）：放行——只解除本页登记，不影响自动运行 |
| 算法引擎 / 模板配置 | **置灰** | 原已置灰 |
| 存图三开关 | 可用 | 只读性质 |
| 单次检测 / 连续检测 | **可用** | 保留（只读观察） |
| 加载本地图片 | **可用** | 保留 |
| 识别框开关 | **可用** | 保留 |
| 保存当前帧 | **可用** | 保留 |
| 三个画面（纯显示） | 可用 | — |

> **⚠ 2026-09-23 修正（用例 11.26，TR-110）**：本节原写「自动运行期间若视觉页能点『停止采集』，**会打断正在执行的识别动作（拿不到帧）**」——**该立论已被代码证伪**。① `AutoRunPage.cpp:502` 自动运行页启动方案时**自己独立** `AcquireCamera("AutoRun")`、只在 `Idle` / `Fault` 释放（`:791`），并不依赖视觉页持流；② `SequenceWorker.cpp:981` 取帧是直接 `hw.camera()->CaptureFrame()`，**根本不经过视觉页**。故 `ReleaseCamera("VisionPage")` 只解除本页那一份登记，`AutoRun` 的持有仍在、流不中断、识别动作不受影响。
> 
> **裁定**：**「停止采集」放行**（`UpdateButtons()` 去掉 `!locked_` 门禁），点后如实提示「已停止本页采集；其他流程仍在使用相机，流未中断」；**「开始采集」仍置灰**——自动运行已持流，本页无需（也不应）重复 acquire。
> 
> 副产物：修正前 `VisionTestPage.cpp` 里那条「已停止本页采集…」分支在自动模式下**不可达**（按钮恒置灰，且 `MainWindow::OnModeToggled` 禁止 Running / Paused 期间切模式），近乎死代码；放行后它成为正常路径。

**签名不变**：`void SetParamsLocked(bool locked)`，`MainWindow::ApplyModeState` 的调用点无需改动。

### 6.3 按钮可用性状态机

| 状态 | 开始采集 | 停止采集 | 单次检测 | 连续检测 | 刷新设备 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 相机未打开 | 可用（内部自动 Open） | 置灰 | 置灰 | 置灰 | 可用 |
| 已打开未采集 | 可用 | 置灰 | 可用（若 `hasFrame_`） | 可用 | 可用 |
| 采集中 | 置灰 | 可用 | 可用 | 可用 | 置灰 |
| 自动运行中 | 置灰 | **可用**（本页持流时） | 可用 | 可用 | 置灰 |
| 断线恢复中 | 置灰 | 可用 | 置灰 | 置灰 | 置灰 |

## 七、数据流与渲染

### 7.1 帧流

```
OrbbecCamera / SimCamera
   ↓ CaptureFrame()（拉模型，CameraCaptureWorker 每 tick）
HardwareManager::frameReady(CameraFrame)
   ├─ VisionTestPage::OnFrameReady ─→ latestFrame_ ─┬─ RGB 格
   │                                                 ├─ Depth 格
   │                                                 └─ Overlay 格（叠 lastResults_）
   └─ AutoRunPage（**S6 后移除**，改订阅 detectionReady）
```

### 7.2 三格渲染与节流

- 三格同时刷新，`QImage::copy` + `SmoothTransformation` 缩放的开销是现状的 3 倍（D3）。
- **节流方案**：单 `QTimer`（如 33 ms ≈ 30 fps 或 66 ms ≈ 15 fps），`OnFrameReady` 只更新 `latestFrame_` 与计数，定时器到点统一刷三格。相比现状「每帧同步刷」，可把 UI 线程占用与帧率解耦。
- 缩放策略：`SmoothTransformation` 在 1280×720 → 460×280 下开销明显；格子尺寸固定时建议改用 `FastTransformation`，或在窗口尺寸变化时才做 `Smooth` 重算。
- `hasFrame_` 为假时三格显示占位文本（如「RGB 画面」），沿用现有 `previewLabel_` 的占位文案风格（`:373`）。

### 7.3 检测结果与日志

**结果**：`RunDetection` 加 `QElapsedTimer` 计时，结果表第 11 列显示「耗时 ms」；同时把耗时写入日志。

**日志（V6 建议方案）**：页面级环形缓冲，**不接 spdlog sink**。

| 写入源 | 内容 |
| :--- | :--- |
| `SetStatus()` | 现有全部状态文本（复用，一处改、全页生效） |
| 检测完成 | `[检测] 目标 N 个，耗时 X.X ms` |
| 帧率异常 | `[帧率] X.X fps`（低于阈值时） |
| 相机状态边沿 | `[相机] 断开` / `[相机] 正在连接/恢复` / `[相机] 已恢复出图` / `[相机] 帧率异常`（来自 `cameraStateChanged`；`Unknown` 态不记日志） |

- 容器：`QPlainTextEdit`（只读、`setMaximumBlockCount(500)` 自带环形上限）。
- 理由：本页日志用途是「调参时回看」，不需要全量系统日志；全量日志仍在 `log/creampuff_YYYY-MM-DD.log`，面板底部另给一个「打开日志目录」按钮（`QDesktopServices::openUrl`）即可。
- **不接 spdlog sink** 的原因：需要改 `main.cpp` 的 logger 配置、跨线程 marshal、且会让本页与日志系统耦合——与「HAL / 页面不越界」的既有边界不符。

### 7.4 存图语义与加固（D4 / D5）

**触发语义（待 V8 确认）**：三开关 = 「**单次检测时自动存图**」，连续检测期间忽略。

**落盘格式（TR-125，2026-10-14 补）**：三路的**文件格式与画面对应关系**在此前未写明，导致 `saves/depth/` 误存了预览用的**伪彩图**（8-bit RGB，16-bit 深度查看器读到的全是 0）。约定如下——

| 开关 | 落盘内容 | 产出函数 | 说明 |
| :--- | :--- | :--- | :--- |
| 保存 RGB | 彩色原帧（8-bit RGB PNG） | `ColorToQImage` | 与预览格同源 |
| 保存 Depth | **深度原始值（16-bit 灰度 PNG，单位 mm）** | `DepthToQImage16` | **不是**预览格那张伪彩图；伪彩不可逆，还原不出 mm |
| 保存 Overlay | 彩色原帧 + 检测框 | `BuildOverlayImage` | 与预览格同源 |

**`FrameSaver` 加固（无论 V8 如何裁定都要做）**：

| 项 | 现状 | 加固 |
| :--- | :--- | :--- |
| 队列上限 | 无界（`FrameSaver.cpp:51`） | `kMaxQueueSize = 30`，超限丢**最旧**并记一条 `saveError` |
| 单 tick 预算 | `while(true)` 全排空（`:62`） | 每次 tick 最多处理 `kMaxPerTick = 3` 张，余量留到下 tick |
| 信号风暴 | 每张 `emit imageSaved`（`:78`） | 连续存图时**合并通知**（如每 tick 只发最后一次 / 或改发计数） |
| `Stop()` 阻塞 | 同步 `ProcessQueue()` 排空（`:40-45`） | `Stop()` 只置标志 + 丢弃队列，不做全量落盘 |

## 八、改动清单

| 文件 | 改动 |
| :--- | :--- |
| `src/UI/VisionTestPage.h` | 成员全量替换（§5.4）；删 `rgbBtn_` / `depthBtn_` / `overlayBtn_` / `previewLabel_` / `infoLabel_` / `resolutionCombo_` / `fpsCombo_` / `deviceIdEdit_`；新增三格成员、6 个流参数 combo、模板路径、3 个存图 check、日志视图；新增槽 `OnRefreshDevices` / `OnBrowseTemplate` / `OnRenderTick` / `OnOverlayToggle`；`SetParamsLocked` **签名不变** |
| `src/UI/VisionTestPage.cpp` | `CreateControlPanel()` 重写（`QScrollArea` + 两组）；`CreatePreviewArea()` → `CreateQuadArea()`（四宫格）；新增 `MakePreviewTile()`；`OnFrameReady` 改「只缓存 + 计数」，渲染移入 `OnRenderTick`；`RunDetection` 加计时 + 写日志；`RenderLatest()` 拆为 `RenderRgb()` / `RenderDepth()` / `RenderOverlay()`；`SetParamsLocked` 清单更新（§6.2）；删 `OnShowRGB` / `OnShowDepth`；删 `.cpp:462` 的 SN 覆盖行（D1）；`OnStartStream` / `OnStopStream` 改走 acquire / release（§6.1） |
| `src/HAL/camera/FrameSaver.h/.cpp` | 队列上限 + 单 tick 预算 + 通知合并（§7.4） |
| `src/HAL/interfaces/ICamera.h` | 加非纯虚 `EnumerateDevices()`（`camera_integration_plan.md` S2） |
| `src/HAL/core/HardwareManager.h/.cpp` | `AcquireCamera` / `ReleaseCamera` + 持有者集合（S6.2）；`EnumerateDevices` 透传 |
| `src/HAL/camera/OrbbecCamera.h/.cpp` | 枚举 + profile 列表暴露（供 UI 填 combo，S2 / S3） |
| `doc/archive/ui_interlock_plan.md` | §5 视觉页只读条款按 §6.2 修正（TR-075 的约定变更） |
| `doc/config.md` | `vision` 段同步（`camera_integration_plan.md` §六） |
| `AGENTS.md` | UI Pages 节补视觉页新布局；`TEST_RECORD.md` 追加 **TR-100** |

## 九、验收标准

> **落地情况（2026-09-20，TR-100）**：前 7 行是 **Sim 侧**判据，均已具备执行条件并通过冒烟；第 8 行**真机三项仍待人工执行**；第 9 行**已满足**（Debug `[33/33]` / Release `[33/33]` 均 `EXITCODE=0`、零新警告；Sim 冒烟两次日志均含 `Initialize complete`）。

| 场景 | 判据 | 落地情况（2026-09-20） |
| :--- | :--- | :--- |
| Sim 模式（`SimCamera` + `SimAlgo`） | 四宫格全功能；三格同时刷新；结果表 11 列有值；连续检测开启后三格持续更新且 UI 不卡 | 🟢 冒烟通过（三格同刷、11 列有值）；连续检测下的长时间流畅度**建议人工再看一次** |
| 窗口自适应 | 拉大 / 拉小窗口，四格等分缩放，左栏 320 px 不变、内容可滚动 | 🟡 未实测（需人工拖动窗口） |
| 离线图检测 | 「加载本地图片」→ 三格显示该图 → 结果表出值 | 🟡 未实测（需人工点一次） |
| 存图 | 单次检测 + 三开关全开 → `saves/{rgb,depth,overlay}/` 各落一张；连续检测期间不落盘 | 🟡 未实测；`FrameSaver` 加固已静态核对 |
| `FrameSaver` 加固 | 连续快速点击「保存当前帧」50 次 → 队列不超上限、无 OOM、退出不卡顿 | 🟡 未实测（队列上限 30 / 单 tick 3 张已落地） |
| 互锁 | 自动运行期间：采集启停 / 流参数 / 算法选择全部置灰，检测与画面可用 | 🟢 代码已按决策 10 实现；**实际互锁表现建议随 P5 真机联调复核** |
| 持有者集合 | 自动运行中视觉页点「停止采集」→ 提示「自动运行仍在使用相机」，流不中断；自动运行结束后视觉页仍出图 | 🟡 未实测（逻辑已在 `OnStopStream` 实现） |
| 真机（须真机执行，Sim 不可替代） | 「刷新设备」列出真实 SN；彩色 1280×720 MJPG + 深度 848×480 Y16 开流成功；D2C 软对齐生效（叠加框与深度对齐）；插拔相机后状态与画面自动恢复 | 🔴 **待人工**（三项） |
| 编译 | **Debug + Release 双编译** `EXITCODE=0`；Sim 冒烟判据 = 当日日志含 `Initialize complete` | 🟢 **已满足** |

## 十、未决项（2026-09-20 已裁定并落地）

> **裁定来源**：用户 2026-09-20 答复「**全部按推荐**」——即下表「建议」列全部采纳，落地情况见末列。

| # | 问题 | 选项 | 建议 | 裁定 / 落地 |
| :--- | :--- | :--- | :--- | :--- |
| V4 | 停止采集后是否关闭相机 | 自动关闭 / 保留「关闭相机」按钮 / 只停流不关设备 | **自动关闭**（简单、不占设备） | ✅ **自动关闭**。`OnStopStream` → `ReleaseCamera("VisionPage")`；若 `CameraHolders()` 仍非空则只提示「其他流程仍在使用相机，流未中断」**不关设备**，否则关闭相机 |
| V5 | 检测参数（置信度 / Z min / Z max）何去何从 | 移除 / 折叠保留 / 迁配置页 | **移除**（贴合「算法参数不上面」）；注意改值需重启 | ✅ **移除**。三控件连同 `exposure` 一并从页面删除；参数改为「编辑 `config.json`」——`confidenceThreshold` / `depthZMin` / `depthZMax` 是**实时读取**，**无需重启**（方案此处「需重启」的旧前提已被 P1 修正） |
| V6 | 日志区的来源与范围 | 页面级环形缓冲 / spdlog Qt sink / 仅「打开日志目录」 | **页面级环形缓冲 + 打开日志目录按钮**（§7.3） | ✅ `QPlainTextEdit` + `setMaximumBlockCount(500)` +「打开日志目录」（`ConfigPaths::logDir()` + `QDesktopServices::openUrl`）；**未接 spdlog sink** |
| V7 | 右下格布局 | `QTabWidget` / 上下 `QSplitter` / 底部独立日志条 | **`QTabWidget`**（§5.2） | ✅ 右下格 `QTabWidget`（页签：结果表 / 页面日志） |
| V8 | 存图三开关的触发时机 | 每次检测都存 / 仅单次检测存 / 仅显式按钮 | **仅单次检测存** + 显式「保存当前帧」 | ✅ `RunDetection(frame, allowAutoSave)`——连续检测传 `false`（忽略三开关）；「保存当前帧」始终可用 |
| V9 | `SimCamera` 下「刷新设备」 | 置灰 / 隐藏 / 允许（返回空列表） | **置灰 + 提示** | ✅ `refreshDevBtn_->setEnabled(SupportsDeviceEnumeration())`；`SimCamera` 返回 `false` → **置灰**（空列表有歧义，故不靠「返回空」判断） |
