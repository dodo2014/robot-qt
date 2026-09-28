# 多相机支持与相机选择解耦方案

> **需求来源**：用户 2026-09-24 提出——「现在 `config.json` 中手动配置启动的相机类型，只有一个相机；后面要支持多个不同型号的相机（USB 连接 / IP 连接），相机的选择要与配置解耦」，并追问四件事：视觉页如何手动切换型号、启动时是否同时扫描加载所有相机、识别动作是否绑定相机与算法、多相机检测结果是否都推送到自动运行界面。
> **本文性质**：**⏳ 方案（未实施）**。给出设计、复杂度评估与对当前架构的影响，**不含改动清单的逐行落点**（实施前另出）。
> **前置阅读**：`doc/camera_integration_plan.md`（相机 HAL 接入的既有事实，本文在其上扩展）、`doc/vision_test_page_redesign_plan.md`（视觉页现行规格）、`doc/config_ownership_plan.md`（配置归属约定）。
> **结论先行**：分两阶段。**阶段 A「多设备清单 + 单活动相机」复杂度中等、风险低，建议本期做**；**阶段 B「多相机并发取流」复杂度是 A 的数倍，不建议现在做**，但阶段 A 会把它的地基（帧溯源、外参归属）打好。

## 一、五个问题的直接回答

| # | 问题 | 结论 | 依据 |
| :--- | :--- | :--- | :--- |
| 1 | 相机选择如何与配置解耦 | 配置从「**一个相机类型**」升为「**一组相机设备清单**」；「当前用哪台」降为**运行时状态**（UI 切换，持久化到 `ui.activeCameraId` 作为「上次选择」记忆，不是功能开关） | §三 |
| 2 | 视觉页如何手动切换型号 | 控制栏顶部新增「**相机设备**」下拉（列配置清单 + 在线状态），切换即 `SetActiveCamera(id)`；原「型号 + SN」两个控件降级为**该设备的编辑属性**。⚠ 切换要求**无其他持有者**（自动运行中置灰） | §五.1 |
| 3 | 启动时是否同时扫描加载所有相机 | **枚举全部、实例化全部、只开流活动相机**。前两步廉价且必要（否则 UI 看不到可选设备），第三步有真实硬件代价（USB 带宽 / 设备独占） | §四 |
| 4 | 识别动作是否绑定相机与算法 | **要绑定，但走「显式绑定 + 默认回落」**：新增 `cameraId` / `algorithmName`，**空 = 回落活动相机 / 全局默认算法**，默认值惰性不落盘（与 P5 `sourceActionId` 同范式） | §六 |
| 5 | 多相机检测结果是否都推送到自动运行页 | **不全部推送，只推「本次运行实际用到的」**。自动运行页是生产观察通道，观察对象是当前方案的执行过程；未被引用的相机推画面零信息量、只增渲染负担 | §五.2 |

## 二、现状核实（单相机假设的全部落点）

> 全部经源码核实，**这是改造范围的边界**——凡下表未列之处，说明不依赖单相机假设。

| # | 位置 | 现状 | 多相机下的冲突 |
| :--- | :--- | :--- | :--- |
| S1 | `HardwareManager.h` 的 `std::unique_ptr<ICamera> camera_` | 单实例 | 只能持有一台 |
| S2 | `HardwareManager::camera()` 访问器 | 返回唯一相机 | 无法表达「哪一台」 |
| S3 | `HardwareManager.h` 的 `std::unique_ptr<IPuffAlgorithm> algorithm_` | 单实例 | 算法与相机无法配对 |
| S4 | `CameraManager.h` 的 `camera_` / `cameraThread_` / `cameraWorker_` | 单份线程 + 单 worker | 无法并发取流 |
| S5 | `HardwareManager.h` 的 `std::set<std::string> cameraHolders_` | 持有者集合描述**全局唯一流** | 语义是「某台相机的流」才正确 |
| S6 | `SequenceWorker.cpp` 的 `hw.camera()` → `CaptureFrame()` | 直接取唯一相机 | 动作无法指定相机 |
| S7 | `CameraFrame`（`ICamera.h`） | **无来源标识** | 订阅者无法区分帧来自哪台 |
| S8 | `SequenceWorker::DetectionResult` | `{frame, results, threshold}`，**无来源标识** | 自动运行页无法归因 |
| S9 | `VisionTestPage::latestFrame_` | 单槽缓存 | 多路会互相覆盖 |
| S10 | `ConfigPage.cpp` 的 `bindLineEdit(snEdit, "communication.camera.sn")` | 只有 SN 一个相机键的写入口 | 无设备清单编辑能力 |

**已具备、可直接复用的能力**（无需新增）：

- `ICamera` 已有 `EnumerateDevices()` / `SupportsDeviceEnumeration()` / `GetStreamProfiles()` / `Configure(CameraStreamConfig)` / `GetLinkState()`——**均为非纯虚默认实现**，`SimCamera` 零改动。
- `CameraDeviceInfo{id, name, connection}` 已存在；`OrbbecCamera` 已实现 `enableNetDeviceEnumeration(true)`，**IP 连接的能力入口已在**。
- `HardwareManager::PollCamera()` 的 500 ms 降频边沿检测 + `cameraStateChanged(CameraLinkState)` 广播。
- `OrbbecCamera` 内部 L1/L2/L3 分级恢复（含 TR-111 的 `wantStreaming_` 意图位与 `InterruptibleSleep()`）——**这是切换相机时能干净中断旧实例恢复循环的基础**。
- `IPuffAlgorithm::Detect(frame, params)` 已是**纯函数**（参数由调用方传入），多算法切换无全局状态风险。
- `SimCamera` / `SimAlgo` 无可变全局状态，多实例安全。

## 三、解耦模型：三层分离

需求「相机选择与配置解耦」拆成三层，**每层只有一个事实源**：

| 层 | 内容 | 事实源 | 变更时机 |
| :--- | :--- | :--- | :--- |
| **设备清单** | 有哪些相机、各是什么型号 / 连接方式 / 地址 / 流参数 / 外参 | `config.json` 的 `cameras.list` | 增删改设备时（低频，配置页） |
| **活动相机** | 当前用哪一台 | 运行时状态（`HardwareManager::activeCameraId_`） | 用户在视觉页切换（高频，会话内） |
| **动作绑定** | 某个识别动作用哪台相机 / 哪个算法 | `process.json` 的 `cameras` 引用字段（空 = 回落活动相机） | 编辑工艺方案时 |

**关键判断（三条，均为「刻意不做」）**：

1. **`ICamera` 接口不变**。多相机是**管理层的编排问题**，不是**设备的职责问题**。`ICamera` 只描述「一台相机」，加多相机方法会污染接口并让每个驱动实现都背上无关责任——这与 `camera_integration_plan.md` 决策 1「`ICamera` 不重写」一脉相承。
2. **`frameReady` / `detectionReady` 信号签名不变**。来源标识靠**载荷字段**携带（`CameraFrame` 加 `cameraId`），而不是加信号参数。理由：① 帧已经是 `shared_ptr` 传递的载体，加字段**零调用点改动**；② 这与 TR-116 给 `CameraFrame` 加 `depthWidth`/`depthHeight` 是同一种做法，已有先例。
3. **不引入「相机角色 (role)」概念**。`activeId` + 动作绑定已完整表达「谁在用」，再加 `primary`/`secondary` 会形成第二套「谁在用」的语义，属冗余。

## 四、启动扫描策略

分三级，**只有最后一级有硬件代价**：

| 级别 | 时机 | 内容 | 代价 | 是否全做 |
| :--- | :--- | :--- | :--- | :--- |
| L1 枚举 | 启动 + 手动「刷新设备」 | 对**每种已注册驱动类型**各枚举一次在线设备 | 低（SDK `queryDevices`） | ✅ 全部 |
| L2 实例化 | 启动（按 `cameras.list`） | 每个配置项 `CameraFactory::Create` + `Configure(流参数)` | 低（构造对象，**不开流**） | ✅ 全部 |
| L3 开流 | **仅活动相机**（懒加载） | `Open` + `StartStream` | **高**（USB 带宽 / 设备独占） | ❌ 只做活动相机 |

**为什么 L3 不能全做**（第一性原理）：

- 瓶颈不是 CPU 而是 **USB 带宽**。Gemini 336 在彩色 1280×720 MJPG + 深度 848×480 Y16 下已占用可观带宽，**两台同型号插在同一 USB 控制器上必然掉帧或开流失败**。这是硬件约束，软件无法绕开。
- 「全部开流」还意味着 N 条采集线程 + N 路 4.2 MB/帧 的广播，而当前只有视觉页一个消费者——纯浪费。

**切换相机的语义因此变得清晰**：不是「重建实例」，而是「**停旧流 → 换绑定 → 开新流**」。实例（L2）一直活着。

⚠ **切换时必须保证旧实例的恢复循环已退出**：`OrbbecCamera` 的 L1/L2/L3 恢复在实例内部，若旧实例仍在后台 `InterruptibleSleep()` 中，会与新实例争抢 USB。TR-111 引入的 `wantStreaming_` 意图位与可中断 sleep 正是为此准备——**切换路径必须走 `Close()` 且确认恢复循环已返回**。

## 五、UI 改造

### 5.1 视觉检测页

现状 UI 把两个概念混在一个下拉里：

| 控件 | 实际语义 | 数据源 |
| :--- | :--- | :--- |
| `cameraTypeCombo_` | **驱动型号**（`SimCamera` / `OrbbecCamera`） | `CameraFactory::Instance().AvailableTypes()` |
| `deviceSnCombo_` | **设备实例** | `hw.EnumerateCameras()` |

多相机后改为三层：

```
[相机设备 ▼]  cam_top  (在线 · USB)      ← 新增，切换即 SetActiveCamera(id)
[型号     ▼]  OrbbecCamera               ← 降级为「该设备的驱动类型」
[SN / 地址 ▼] CP9KB530005S               ← 降级为「该设备的地址」
[刷新设备]                               ← 保留
```

- **「相机设备」下拉**列 `cameras.list` + 每项的在线状态徽标（在线 / 未枚举到 / 已禁用）。切换它 = 切换活动相机。
- **型号 + SN** 退化为**该设备的编辑属性**。⚠ **切换型号必须重建实例**（`camera_integration_plan.md` 的 M5 已指出）——这是**危险操作**（停流 + 重建），需二次确认。
- **「新增设备」入口**：视觉页保留便捷入口，但设备清单的**权威编辑在配置页**（§5.3）。

**⚠ 互锁扩展（决策 10 的推广）**：现行决策 10 是「自动运行期间置灰相机开关 / 启停采集」。多相机后必须扩展为——**切换活动相机要求 `cameraHolders_` 中无其他持有者**。

理由：`cameraHolders_` 的语义是「**相机资源**的持有者」，不是「**某台相机**的持有者」。若自动运行正持有相机，此时切换活动相机，正在执行的识别动作会拿到**另一台相机**的帧——不报错、不崩溃，只是识别结果静默错误。这与 TR-111「消费 ≠ 持有」是同一类不对称缺陷。

视觉页自身持有 `VisionPage` 时切换是安全的：先 `ReleaseCamera("VisionPage")` → 切换 → 再 `AcquireCamera("VisionPage")`。

### 5.2 自动运行页

**不全部推送，只推「本次运行实际用到的」**：

- `DetectionResult` 加 `cameraId` / `algorithmName`，自动运行页按 `cameraId` 归因。
- 保持现有「识别动作跑完才发一次」的节流语义（**非每帧**）——这正是多相机下需要的粒度。
- 连续收到不同相机的结果时（P6 分组并发下可能），保留**最后到达的**并在信息条标注相机名。
- 保持 P3-4 的「不可见时只缓存、不产位图」优化不变。

**明确不做**：多路同屏（N×M 网格布局）。那是阶段 B，需要独立的新布局设计。

### 5.3 配置页

`ConfigPage.cpp` 目前只有 `communication.camera.sn` 一个相机键的写入口（S10）。多相机需要新增「**相机设备**」页签或区块，支持：

- 列出 `cameras.list`，增 / 删 / 复制设备
- 每项编辑：显示名、驱动类型（从工厂枚举）、连接方式、地址（SN / `ip:port`）、流参数、外参、启用开关
- 「设为活动相机」

⚠ **配置生效语义不变**：经 UI 改 → 即时生效；**直接编辑 `config.json` → 需重启**（`ConfigManager::getValue` 读内存 `root_`）。设备清单属结构性配置，改动需要重建实例，**建议统一要求重启**（比「部分即时生效」更不易误判）。

## 六、识别动作绑定

### 6.1 字段设计

`ActionData`（`ProcessManager.h`）新增两个字段：

| 字段 | 类型 | 语义 | 默认值 |
| :--- | :--- | :--- | :--- |
| `cameraId` | `QString` | 引用的相机设备 id | 空 = **回落活动相机** |
| `algorithmName` | `QString` | 引用的算法类型名 | 空 = **回落全局默认算法** |

**默认值惰性、不落盘**——与 P5 `sourceActionId`（默认 `0` = 未设置）的处理范式完全一致：默认值只在内存，不因读取而写盘，下次保存才落盘。

理由（第一性原理）：「用哪台相机看」在多相机下是**工艺语义**（顶视抓取 / 侧视测高），必须由动作决定，否则换相机就得改全局配置，等于没解耦；但强制绑定会让既有 `process.json`（当前 7 个节点全部无该字段）失效。

⚠ **`visionType` 字段的既有语义需先澄清**：现行 `process.json` 里识别动作的 `visionType` 是 `"CCD"`，而 `SequenceWorker` 只在日志里打印它（`视觉：采帧中 (%1)`），**不参与任何逻辑**。方案**不复用该字段承载相机引用**——避免「字段名与语义不符」的历史包袱，新字段名明确表达引用关系。

### 6.2 校验（沿用 P5 的两段式）

| 时机 | 规则 |
| :--- | :--- |
| 编辑期 | `cameraId` 非空时须指向 `cameras.list` 中**存在**的设备，否则提示并拒绝保存；下拉对**悬垂引用显式列出 `cam_x（已不存在）`**，不静默落到第一个合法项 |
| 执行期（必须） | `cameraId` 为空 → 回落活动相机；非空但设备不存在 / 未启用 → 写 `r.failReason` 并 `return false`，**不产生任何运动** |

### 6.3 ⚠ 外参必须跟着相机走（最易忽略的深坑）

`tcpCalibration.handEyeMatrix` 现在是**单份**，在 `SequenceWorker::ReloadFromConfig` 读一次设进 `impl_->coord`（`SequenceWorker.cpp:201-205`）。

**多相机若都参与定位，每台相机有各自的外参。** 若只切相机不切外参，识别坐标会静默落到**另一台相机的坐标系**里——不报错、不崩溃，只是抓歪。

处置（推荐前者）：

- **A（推荐）**：把 `handEyeMatrix` 挪进 `cameras.list[].handEyeMatrix`，`SequenceWorker` 按当前相机的 id 取对应外参。语义正确，且天然支持「每台相机独立标定」。
- **B**：明确「本期只支持单相机定位」，切换相机后**必须手工切换外参**，并在 UI 上强提示。

**建议采用 A**——外参与相机的绑定是物理事实，不该由操作者手工同步。

## 七、配置 schema

```jsonc
"cameras": {
    "list": [
        {
            "id": "cam_top",                    // 设备 id（方案内唯一，动作引用用）
            "name": "顶视相机",                  // 显示名
            "type": "OrbbecCamera",             // 驱动类型（工厂注册名）
            "transport": "usb",                 // "usb" / "ip" / "sim"
            "address": "CP9KB530005S",          // USB=SN；IP="192.168.1.20:8090"
            "enabled": true,                    // false = 不参与枚举与开流
            "stream": {                          // 与现行 communication.camera.* 同 schema
                "colorWidth": 1280, "colorHeight": 720, "colorFps": 15.0, "colorFormat": "MJPG",
                "depthWidth": 848, "depthHeight": 480, "depthFps": 15.0, "depthFormat": "Y16",
                "d2cAlign": "SW", "autoExposure": true
            },
            "handEyeMatrix": [1,0,0,12.3, 0,1,0,-5.7, 0,0,1,38.1, 0,0,0,1]
        }
    ]
},
"ui": {
    "activeCameraId": "cam_top"                  // 上次选择的相机（UI 偏好，非功能开关）
}
```

**向后兼容（关键）**：

- `communication.camera.*` **保留不删**，语义降级为「**默认设备模板**」。
- 启动时若 `cameras.list` 不存在或为空，但 `communication.camera.sn` 存在 → **内存中构造一台默认设备**（`id = "default"`，`type` 取 `simulation.cameraType`，流参数取 `communication.camera.*`），**惰性，不写盘**。
- `simulation.cameraType` 保留为「默认设备的驱动类型」，语义与 `simulation.motionCardType` / `servoType` 同族。
- **旧配置与旧 `process.json` 因此全部保持可用**，无需迁移脚本。

## 八、复杂度评估

### 阶段 A：多设备清单 + 单活动相机（**建议本期做**）

| 改动面 | 内容 | 量级 |
| :--- | :--- | :--- |
| 配置 | 新增 `cameras` 段 + 旧键迁移（惰性构造默认设备） | 1 处 schema + 1 个迁移函数 |
| HAL 核心 | `camera_` → `std::map<std::string, std::unique_ptr<ICamera>>` + `activeCameraId_`；**`camera()` 保留为薄封装返回活动相机** | `HardwareManager.h/.cpp` |
| HAL 核心 | `CameraManager` 结构**不变**（仍单线程单 worker），只增加「切换绑定」路径 | `CameraManager.h/.cpp` |
| HAL 核心 | `AcquireCamera` 语义明确为「**活动相机**的流」+ 切换前持有者检查 | `HardwareManager.cpp` |
| 帧溯源 | `CameraFrame` 加 `std::string cameraId`（或 `int cameraIndex` + 映射表） | `ICamera.h` |
| 结果溯源 | `DetectionResult` 加 `cameraId` / `algorithmName` | `SequenceWorker.h` |
| 动作绑定 | `ActionData` 加 `cameraId` / `algorithmName` + 编辑期下拉 + 执行期解析 | `ProcessManager.h/.cpp`、`ProcessPage.cpp` |
| 外参归属 | `handEyeMatrix` 挪进 `cameras[]`，按相机取 | `SequenceWorker.cpp`、`config.json` |
| UI | 视觉页加「相机设备」下拉 + 互锁扩展；配置页加设备清单编辑；自动运行页信息条加相机名 | `VisionTestPage`、`ConfigPage`、`AutoRunPage` |
| 文档 | `AGENTS.md` / `doc/config.md` / `doc/architecture.md` / `doc/process.md` 同步 | 4 篇 |

**复杂度：中**。约 10~14 个文件，但**绝大多数是「加字段 + 加一个解析函数」**，无算法级改动、无线程模型改动、无新布局设计。

**风险：低**。旧配置与旧 `process.json` 靠「默认回落」保持可用；`camera()` 薄封装让 5 处既有调用点零改动。

### 阶段 B：多相机并发取流（**不建议现在做**）

| 改动面 | 内容 |
| :--- | :--- |
| `CameraManager` | 单 session → 多 session（每相机一线程 + worker） |
| 持有者模型 | `set<string>` → `map<cameraId, set<owner>>`（**最深的一处改动**） |
| 检测调度 | 单 `detectThread_` + `detectBusy_` 背压模型 → 线程池 |
| UI | 多路同屏需要**新的布局设计**（非小改） |
| 资源调度 | 带宽 / 帧率优先级策略（谁降帧、谁让路） |

**复杂度：高**——是阶段 A 的数倍。**风险：中高**（USB 带宽、UI 负载、线程安全）。

## 九、对当前架构的影响

| 维度 | 影响 | 说明 |
| :--- | :--- | :--- |
| 分层依赖 | **不变** | 多相机管理放在 `src/HAL/core/`，不引入新层 |
| `ICamera` 接口 | **不变** | 关键克制：多相机是管理层职责（§三 判断 1） |
| HAL 边界 | **不变** | 设备实现仍不读配置、不碰 Qt 信号 |
| 信号签名 | **不变** | `frameReady` / `detectionReady` 靠载荷字段携带来源（§三 判断 2） |
| 配置 schema | **扩展** | 新增 `cameras`；旧 `communication.camera.*` 保留为迁移源 |
| 线程模型 | 阶段 A **不变**；阶段 B 才改 | |
| UI 布局 | 视觉页加一个下拉、配置页加一个区块、自动运行页加一个标签 | **四宫格布局不变** |
| 手眼矩阵 | **需决策** | 单份 → 按相机（§6.3）。**这是本次唯一涉及「物理语义」的改动** |
| 运动学 / 坐标链路 | **不变** | `CameraToRobot` 的算法不变，只是外参来源从「全局」变「按相机」 |
| `IPuffAlgorithm` 接口 | **不变** | `Detect` 已是纯函数 |

**必须点出的四条风险**：

| 风险 | 影响 | 处置 |
| :--- | :--- | :--- |
| **外参与相机隐式耦合** | 切相机不切外参 ⇒ 识别坐标静默落到错误坐标系、抓歪，**不报错** | 外参挪进 `cameras[]`（§6.3 方案 A） |
| **TR-117 教训的推广** | 新驱动类型（如 `IpCamera`）若不 pin 在 CMake 配置里，CMakeCache 重建后**静默丢失** ⇒ 配置里有 `cam2` 但下拉框无该型号，**全程零报错** | 同步 `USE_*` 的**存在性自动探测**（TR-117 已建立范式） |
| **USB 带宽** | 多相机同控制器开流 ⇒ 掉帧 / 开流失败 | 这是**不同时开流（§四 L3）的硬理由**，非软件可绕 |
| **切换时旧实例恢复循环未退出** | 旧实例 L1/L2/L3 恢复与新实例争抢 USB | 切换走 `Close()` 并确认恢复循环已返回；复用 TR-111 的 `wantStreaming_` + `InterruptibleSleep()` |

## 十、明确不做（边界）

| 项 | 理由 |
| :--- | :--- |
| `ICamera` 接口加多相机方法 | 多相机是管理层编排，不是设备职责（§三 判断 1） |
| 多路同屏（N×M 网格） | 阶段 B；需要独立的新布局设计 |
| 多相机同时开流 | 硬件带宽约束（§四） |
| 「相机角色 (role)」字段 | 与 `activeId` + 动作绑定语义冗余（§三 判断 3） |
| 复用 `visionType` 承载相机引用 | 字段名与语义不符，新字段更清晰（§6.1） |
| 设备清单「部分即时生效」 | 结构性配置统一要求重启，比部分生效更不易误判（§5.3） |

## 十一、验收与台账

- 阶段 A 每步：**Debug + Release 双编译**（`EXITCODE=0`）+ **Sim 冒烟**（判据 = 当日日志含 `Initialize complete`）。
- **Sim 可验收**：设备清单增删改、活动相机切换、动作绑定与回落、外参按相机取、自动运行页归因。
- **真机必须**：多设备枚举（USB + IP）、切换后出图、切换后外参生效、旧配置迁移。
- 台账：实施后从 **TR-118** 起追加（当前最大 `TR-117`）。

## 十二、待用户裁定

| # | 事项 | 选项 |
| :--- | :--- | :--- |
| Q1 | 外参归属 | **A：挪进 `cameras[].handEyeMatrix`（推荐）** / B：保持单份 + 手工切换 |
| Q2 | 阶段 B 是否列入路线图 | 列入（留地基） / **不列入（本期只做 A）** |
| Q3 | 活动相机持久化位置 | **`ui.activeCameraId`（推荐）** / 不持久化（每次启动回落第一台） |
| Q4 | 设备清单编辑入口 | 配置页新增页签（推荐） / 并入现有「通信与连接」页签 |
