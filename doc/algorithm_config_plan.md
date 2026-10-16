# 算法参数与配置归属方案

> **需求来源**：用户 2026-10-12 提问——「算法需要的各种配置项，比如 roi 区域，fx / fy 之类，是否需要单独的配置文件？因为不同的算法版本，需要的配置可能不同，这个需求是否合理？」
> **结论**：**需求本身合理，但落点不是「单独配置文件」，而是「按算法名命名空间化」**；并且 **`fx` / `fy` / `cx` / `cy` 根本不该进入算法**——它们是相机内参，算法只需要输出**像素坐标 + 深度**，XY 由下游 `CoordTransform::PixelToRobot` 用真实内参反投影。
> **状态**：⏳ 部分实施。**C2 已落地（TR-124，2026-10-13）**；**§十 的 5 项待裁定已于 2026-10-13 全部按「建议」列裁定**（见 §十）；**新增 §十一「算法接口契约（输入 / 输出）」与 §十二「承载位置与构建集成」**——算法同事**暂无法交付**，现阶段算法**由我方自行实现**（路线 = 移植老项目 `CPuffLocator` 深度法，来源见 `implementation_roadmap.md` §9.4），**但将来仍由同事以「独立程序」形态重写** ⇒ 输入 / 输出格式须**先冻结**，换实现只替换一个适配器，下游零改动。本文只定 **schema、归属、接口契约与承载方式**（§十一 / §十二），**不含深度法算法本体的移植步骤**（那一篇另开，见 `depth_puff_algorithm_plan.md`，索引见 `implementation_roadmap.md` §9.6）。
> **关联**：`config_ownership_plan.md`（分文件的触发条件 §6.1 与代价 §6.3，**本文 §八 以它为前置**）、`doc/config.md`（schema 权威落点，本文 §七 要求同步）、`implementation_roadmap.md` §九（④ 的工期与待定项）、`hand_eye_calibration_plan.md`（§5.1 C1 与本文 §六 C2 **是同一处改动**，两篇须合并执行）、`depth_puff_algorithm_plan.md`（**本文 §十一 契约的实现篇**——移植步骤与参数初值表）。

## 一、问题与结论

**问题**：不同算法实现需要的配置键不同（`SimAlgo` 只要 `confidenceThreshold`，深度法还要 ROI / `heightBandMm` / `nearestPercentile` 等十项），把它们的并集塞进 `vision.*` 会持续膨胀。是否应该给算法配置单独开文件？

**结论分三层**：

| # | 结论 | 一句话理由 |
| :--- | :--- | :--- |
| 1 | **「按算法名命名空间化」应做** | 真正的问题是**调用方知道算法私有键名**（`HardwareManager.cpp:161-163` 手工拼 JSON），不是「配置放在哪个文件」。命名空间化能用零基础设施成本解决它 |
| 2 | **「单独配置文件」暂不做** | 触发条件（`config_ownership_plan.md` §6.1）未满足；分文件的代价（同文 §6.3）与收益不成比例。**「一个算法一个文件」尤其不做** |
| 3 | **内参不进算法，也不进算法配置** | 算法只输出**像素 + 深度**；XY 由下游 `PixelToRobot(u, v, z)` 用真实内参反投影（`CoordTransform.cpp:62-76`，**已实现、全仓无调用者**）。内参的唯一所有者是相机 |
| 4 | **接口契约须先冻结**（2026-10-13 补） | 算法同事暂无法交付 ⇒ 现阶段由我方实现（移植 `CPuffLocator` 深度法），**但将来由同事以「独立程序」形态重写** ⇒ 输入 / 输出格式必须先定死，换实现只替换一个**适配器**，`SequenceWorker` / 视觉页 / `CoordTransform` **零改动**（§十一） |

⚠ **结论 3 是本方案最容易被写反的一处**：直觉上会认为「算法要出 `x/y`，所以要把 `fx/fy` 喂给算法」。本项目**已经为此准备好了正确通路**——`PixelToRobot` 就是「像素 + 深度 → 基座坐标（内参 + 手眼两步）」，只是没人调用它。走它，算法就完全不必知道内参。

## 二、参数的三层承载

算法用到的参数分三层，**每层的生命周期与所有者不同**，混淆是当前问题的根源。

| 层 | 载体 | 生命周期 | 所有者 | 现状 |
| :--- | :--- | :--- | :--- | :--- |
| **L1 全局配置** | `config/config.json` | 随部署 / 随产品与光照 | 现场与产品 | `vision.*` 4 键；**无算法命名空间** |
| **L2 参数快照** | `AlgorithmParams`（值语义） | 每次检测前取一次 | `IPuffAlgorithm` 子类 | 只有 3 个字段；**无算法私有量** |
| **L3 调用入参** | `CameraFrame` + 动作级参数 | 每次调用 | 调用方 | `ActionData.threshold` / `visionType` / `templateName` **均为死键**（见 §3.2） |

**约定（沿用 `IPuffAlgorithm.h:21-33` 的既有设计）**：

- **L1 只能在主线程读**——`ConfigManager` 的 `json` 读写非线程安全，且 worker 线程绝不读 `ConfigManager`（`SequenceWorker.cpp:106` 的同名约定）。
- **L2 是「主线程读 L1 → 打包成值语义结构 → 交给任意线程」的桥**。`Detect(frame, params)` 因此是**纯函数**。
- **L3 由调用方组装**，算法不回头读任何全局状态。
- ⚠ **相机内参不属于上述任何一层**：它由相机在运行期给出，**只在调用点使用**（§3.3、§六 C2）。

## 三、现状核实

### 3.1 算法配置链路：调用方硬编码算法私有键名

| 环节 | 位置 | 现状 |
| :--- | :--- | :--- |
| 选型 | `HardwareManager.cpp:153-154` | 读 `simulation.algorithmType`（现值 `"SimAlgo"`），经 `PuffAlgorithmFactory::Instance().Create(algoType)` 建实例 |
| **喂配置** | `HardwareManager.cpp:161-163` | ⚠ **手工拼字符串** `"{\"confidenceThreshold\":" + … + "}"` 后调 `LoadConfig()` |
| 接收 | `IPuffAlgorithm.h:49` | `virtual bool LoadConfig(const std::string& jsonConfig) = 0;` —— **收的就是一段 JSON 字符串** |
| 解析 | `SimAlgo.cpp:190-211` | 只解析 `confidenceThreshold`，其余键忽略 |
| 兜底 | `SimAlgo.cpp:213-218` | `EffectiveThreshold()` 实时读 `vision.confidenceThreshold`，键缺失时回落 `LoadConfig` 解析值 |

⚠ **问题不在「只有 1 个键」，而在「这个键名写在调用方里」**。`HardwareManager` 是通用的 HAL 门面，它现在**认识某个算法实现的私有键名**。每加一个算法参数就要改一次 `HardwareManager`——这条路径不能扩展。

⚠ **接口早已为「整块算法私有配置」留好位置**：`LoadConfig(const std::string&)` 收字符串而不是「路径」或「若干具名参数」，正是为了透传一整块。缺的只是让调用方把**整棵子树**原样传进去（§六 C1）。

### 3.2 动作级参数：三个死键

`ActionData`（`ProcessManager.h:51-54`）里有四个算法相关字段，实际生效情况：

| 字段 | 写入口 | 读点 | 生效状态 |
| :--- | :--- | :--- | :--- |
| `ActionData.threshold` | 动作编辑区 | `SequenceWorker.cpp:1007`（仅拼日志串） | **仅打印**——`:1011` 取的是 `SnapshotParams()`（读全局配置），动作级阈值**从不进入 `AlgorithmParams`** |
| `ActionData.visionType` | 动作编辑区 | `SequenceWorker.cpp:998`（仅拼日志串） | **仅打印** |
| `ActionData.templateName` | 动作编辑区 | **无** | **无消费点** |
| `ActionData.exposure` | 动作编辑区 | 挤出动作参数，与相机曝光无关 | 见 `doc/config.md:145` |

⚠ 另有一处**语义倒挂**：`SequenceWorker.cpp:1023` 写 `dr.threshold = algoParams.confidenceThreshold`——`DetectionResult.threshold`（用于渲染叠加框）取的是**全局**阈值，而日志（`:1007`）打的是**动作级**阈值。同一概念两个值，**调参时会直接看错**。

⚠ `config_ownership_plan.md` C6 已裁定「**动作级覆盖全局，全局作为默认值**」，但**该覆盖至今未落地**。本方案不改变这一裁定，只指出它仍欠账。

### 3.3 内参链路：全仓无通路，但**修法不是喂给算法**

| 项 | 事实 | 依据 |
| :--- | :--- | :--- |
| 算法侧无内参字段 | `AlgorithmParams` 只有 `confidenceThreshold` / `depthZMin` / `depthZMax` | `IPuffAlgorithm.h:28-33` |
| 算法用假内参 | `SimAlgo::Detect` 硬编码 `fx = fy = 520.0`、`cx = w/2`、`cy = h/2` | `SimAlgo.cpp:53-56` |
| 假内参**恰好等于**仿真相机内参 | `SimCamera` 的内参同为 `fx = fy = 520.0`、`cx/cy = w/2, h/2` | `SimCamera.cpp:27-30` |
| 相机能给出真内参 | `OrbbecCamera` 从彩色 profile 取 `fx/fy/cx/cy` + 畸变系数 | `OrbbecCamera.cpp:387-397` |
| **正确的下游通路已存在** | `CoordTransform::PixelToRobot(u, v, depthMm)`：像素 + 深度 → 内参反投影 → 手眼变换 → 基座坐标 | `CoordTransform.cpp:62-76` |
| **但没人调用** | `PixelToRobot` / `SetCameraIntrinsics` / `ICamera::GetIntrinsics()` 全仓无生产调用者 | 全仓 `grep`（2026-10-12） |
| 当前实际调用的是**另一半** | `SequenceWorker.cpp:1035` 走 `CameraToRobot(best.x, best.y, best.z)`——**消费算法用假内参算出的 `x/y`** | `SequenceWorker.cpp:1035` |

⇒ **仿真对得上、真机必错**，且错误的来源是「用假内参算出的 `x/y` 被当真值送进手眼变换」。

⚠ **所以修法是把 `:1035` 改成 `PixelToRobot(best.pixelU, best.pixelV, best.z)`，而不是把内参塞进 `AlgorithmParams`**。三条理由：

1. **内参的唯一所有者是相机**。喂给算法 ⇒ 算法成为第二个持有者 ⇒ 两处可能静默漂移。
2. **喂给算法要额外解决两个时序陷阱**（见 `hand_eye_calibration_plan.md` §5.1 C3 / C4）：内参在 `pipeline_->start(cfg)` **之后**才有值（启动时喂会得到 `fx = 0`），且**随流分辨率变**（`OrbbecCamera.cpp:396-397` 连 `width/height` 一起写入）⇒ 必须每次识别取。走 `PixelToRobot` 时这些约束**自动满足**——它本来就在每次识别时执行。
3. **与拟移植的算法天然吻合**：老项目 `CPuffLocator` 的输出就是 `center`（像素）+ `depthMm`，**全程不使用内参**。走 `PixelToRobot` 时移植**零额外成本**。

### 3.4 配置的读语义

- `ConfigManager::getValue` 读的是**内存 `root_`**，而 `root_` 只在启动时由 `load()` 填充 ⇒ **手改 `config.json` 必须重启**；经 UI 页 `set()` 则即时生效。
- `doc/config.md:144` 的 P4 裁定「**算法参数不上面**」⇒ 算法调参的工作流**就是**「手改 `config.json` + 重启」。所以算法参数需要一个**好找、有注释、不被其他域淹没**的位置。

## 四、归属判据：哪个键该放哪里

判据只有一条：**这个值由谁产出、随什么变化、谁有权修改。**

| 参数 | 产出方 | 随什么变 | 归属 | 承载层 | 理由 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `fx` / `fy` / `cx` / `cy` / `k1` / `k2` / `p1` / `p2` | 相机 SDK / 标定 | 相机个体、**流分辨率** | **相机域** | **不进入算法**；只在 `SequenceWorker.cpp:1035` 调用点经 `SetCameraIntrinsics` 交给 `CoordTransform` | 算法成为第二个持有者 ⇒ 与相机实际值**静默漂移**；且喂入需额外解决启动时序与分辨率两个陷阱（§3.3） |
| `roi`（托盘感兴趣区域） | 调参期人定 | 安装与托盘布局 | **算法节点**（调参期）→ 稳定后可上提至产品域 | L1 `vision.algorithms.<名>` → L2 | 老项目 `puff_locator_params.json` 即为算法侧参数，有先例 |
| `confidenceThreshold` | 人定 | 产品与光照 | **`vision.*`（跨算法通用）** | L1 → L2 | 深度法与颜色法**都有**这个概念 ⇒ 不下沉，避免每算法重复声明 |
| `depthZMin` / `depthZMax` | 人定 | 安装高度与工作范围 | **`vision.*`（深度域通用）** | L1 → L2 | 任何消费深度图的算法都需要，属**深度域**而非某算法 |
| `nearestPercentile` / `heightBandMm` / `minArea` / `maxArea` / `targetArea` / `targetAreaTolerance` / `morphKernelSize` / `edgeMargin` | 人定 | **具体算法实现** | **算法节点** | L1 `vision.algorithms.<名>` → L2 | 只有深度法有这些概念；换算法即整体失效 |
| `templatePath` | 人定 | 具体算法实现 | **算法节点** | L1 `vision.algorithms.<名>` | ⚠ 现值在 `vision.templatePath`，**且无读点**（`doc/config.md:143`）⇒ 属错位，见 §七 M1 |
| 动作级 `threshold` / `visionType` / `templateName` | 工艺方案 | 单个动作 | **L3 调用入参** | `process.json` → 执行时组装 | 属动作而非全局；**当前未落地**（§3.2），本方案不改变 C6 裁定 |

## 五、目标 schema

```json
"vision": {
  "confidenceThreshold": 0.85,
  "depthZMin": 6.0,
  "depthZMax": 496.0,
  "algorithms": {
    "SimAlgo": {},
    "DepthPuffAlgo": {
      "nearestPercentile": 0.03,
      "heightBandMm": 35.0,
      "minArea": 120,
      "maxArea": 250000,
      "targetArea": 0,
      "targetAreaTolerance": 0,
      "morphKernelSize": 7,
      "edgeMargin": 8,
      "roi": { "x": 0, "y": 0, "w": 0, "h": 0 },
      "templatePath": ""
    }
  }
}
```

**设计要点**：

1. **`algorithms.<名>` 的键名 = `simulation.algorithmType` 的取值**——两者必须是同一套标识，不另立命名（避免「选型叫 A、配置找 B」的错位）。
2. **停用算法的节点保留、不删**——切回旧算法时参数还在，避免调参成果随一次切换丢失。
3. **`SimAlgo` 空对象是合法的**：它没有私有键，全部参数都在通用域。
4. **不设 `activeAlgorithm`**——选型唯一落点是 `simulation.algorithmType`（`doc/config.md:35` 已定义为「视觉算法实现名」），再加一个就是第二处真相。
5. **内参不在此出现**（§四）。

## 六、代码改动清单

| # | 改动 | 落点 | 说明 |
| :--- | :--- | :--- | :--- |
| **C1** | 调用方改为**透传整棵子树** ✅ **已实施 2026-10-14（TR-126）** | `HardwareManager.cpp:161-163` | 由手工拼 `{"confidenceThreshold": …}` 改为取 `vision.algorithms.<algoType>` 整块 `.dump()` 后喂 `LoadConfig()`；节点缺失时传 `{}`（**不是失败**，**已裁定 ①**，见 §十） |
| **C2** | **识别分支改走 `PixelToRobot`** ✅ **已实施 2026-10-13（TR-124）** | `SequenceWorker.cpp:1050-1055` | 由 `CameraToRobot(best.x, best.y, best.z)` 改为「`InMainThread` 取 `camera->GetIntrinsics()` → `coord.SetCameraIntrinsics(fx, fy, cx, cy)` → `coord.PixelToRobot(best.pixelU, best.pixelV, best.z)`」。⚠ **与 `hand_eye_calibration_plan.md` §5.1 C1 是同一处改动**——已合并执行一次。同批附带 C1b（日志补 `Vision calib: pixel(u,v) depth intr(fx,fy,cx,cy)` 读数行）与 C2'（`OrbbecCamera.cpp:407` 打印彩色内参），见该篇 §5.1 |
| **C3** | `AlgorithmParams` 加**算法私有调参量**；`PuffResult` 加 `area` / `minDepthMm` ✅ **已实施 2026-10-14（TR-126）**；`PuffResult` 再加 `bboxX` / `bboxY`、`AlgorithmParams` 再加 `apexPercentile` ✅ **已实施 2026-10-16（TR-132）** | `IPuffAlgorithm.h:28-33`、`:8-19` | 深度法所需 8 项 + `roi`。⚠ **字段清单与语义以 §十一 的接口契约为准**（`z` 定为「区域最高点」、`results.front()` = 最佳目标）。⚠ **不加内参字段**（§3.3、§十 裁定 5）。⚠ 只加**当前已注册实现**的量；这是接口结构体，改它要走全清重建 |
| **C4** | `SnapshotParams()` 补齐新字段 ✅ **已实施 2026-10-14（TR-126）** | `SimAlgo.cpp:21-30`、`DepthPuffAlgo.cpp:268-300` | 私有量从 `vision.algorithms.<名>` 读；**通用三键（`confidenceThreshold` / `depthZMin` / `depthZMax`）也按「节点优先、缺则回落 `vision.*`」读**（§十 裁定 3）。⚠ 原文「通用量维持现读法」与裁定 3 **冲突**，深度法初版照此实现 ⇒ 节点级同名覆盖键**静默失效**（**2026-10-14 TR-127 已修**，两算法现同一形态） |
| **C5** | `LoadConfig` 解析自己的整棵子树 ✅ **已实施 2026-10-14（TR-126）** | `SimAlgo.cpp:190-211` | 从「只认 `confidenceThreshold`」改为「认整棵子树」，兜底值语义不变 |
| **C6** | `vision.templatePath` 迁到算法节点 ⏳ **已移出本批（2026-10-14）** | `config.json` + `doc/config.md:137,143` | 见 §七 M1。⚠ **原文「且无读点」不成立**——`VisionTestPage.cpp:839` **读**、`:1313` **写**（视觉页「模板配置」的「浏览…」）。迁键须同批改这两处路径（还要跟随该页的算法选择状态）⇒ 属独立一处改动，**另开**；本批**只新增 `vision.algorithms` 节点、未删任何既有键** |
| **C7** | 动作级覆盖落地（**可选，属 C6 欠账**） | `SequenceWorker.cpp:1007,1011,1023` | 让 `ActionData.threshold` 真正覆盖 `algoParams.confidenceThreshold`，并消除 §3.2 的语义倒挂。**与本方案正交，可另开** |
| **C8** | `PuffResult.x/y` 的去留 | `IPuffAlgorithm.h:10-11`；`VisionTestPage.cpp:1268-1269` 结果表；`SequenceWorker.cpp:1061-1064` 日志 | **已裁定 ①**（见 §十）：字段**保留**、算法侧**不再填**（恒 0）；视觉页 X/Y 两列改由 `PixelToRobot` 实时算基座坐标（或改标注为「下游算」）；日志的 `cam(x,y,z)` 改为打印像素（`best.x/y` 已无意义）。**不实施 C2 则本项无意义**（C2 已落地 TR-124） |

**验收判据**：

1. `HardwareManager` 中**不再出现任何算法私有键名字面量**；新增一个算法实现时 `HardwareManager` 零改动。
2. 识别动作日志 `[SequenceWorker] Vision target: cam(…) → robot(…)` 中的 `cam(x,y,z)` 与真机真实内参自洽（用同一靶点，`PixelToRobot` 结果与手算一致）。

## 七、须修正项

| # | 问题 | 处理 |
| :--- | :--- | :--- |
| **M1** | `vision.templatePath` 放在**通用域**却只对某个算法有意义。⚠ **原文写「至今无读点」，2026-10-14 核实更正**：`doc/config.md:143` 那句「不要误以为它已在工作」指的是**算法侧不消费**，但 **UI 侧确有读写点**——`VisionTestPage.cpp:839` 读、`:1313` 写（视觉页「模板配置」的「浏览…」）。**迁键会牵动 UI 页** ⇒ 已移出本批（见 §六 C6） | 迁入 `vision.algorithms.<名>.templatePath`，**并同批改 `VisionTestPage` 的读写路径**（须跟随该页的算法选择状态）。**只迁键不改读点，等于把死键换个地方继续死** |
| **M2** | `DetectionResult.threshold` 取全局值、日志打动作级值（§3.2） | 随 C7 一并消解；若不实施 C7，至少统一为「都取实际生效的那个值」 |
| **M3** | `LocateBest()`（`IPuffAlgorithm.h:47`）**全仓无生产调用点** | 保留（接口对称性）或标注「预留」；**不要**让新算法实现者误以为它被消费 |
| **M4** | 算法参数**无 UI 入口**且 P4 已裁定「算法参数不上面」 | 维持现状，但须在 `doc/config.md` 为算法节点补一段「手改 + 重启」的说明——否则新加入者会以为改完即生效 |

## 八、为什么不单独成文件

`config_ownership_plan.md` 已对「分文件」做过一次完整裁定，本方案**沿用其判据，不另立标准**。

**（1）触发条件未满足。** 该文 §6.1 定的两条是「**需要部署到第二台机器或第二个现场**」与「`config.json` 超过约 400 行 / 某域叶值超过 20 个」。现状 254 行，本方案新增约 12 个键，不越线。

**（2）分文件的代价是实打实的。** 该文 §6.3：

- `ConfigManager` 是**单文档模型**（单 `root_` + 单 `filePath_`，`ConfigManager.h:47-49`），承载多文件需先改造成多文档或按域复制多个管理器；
- 需新增**加载顺序与覆盖规则**（多文件同键谁赢）与**缺文件行为**（回落默认值还是报错）两套机制，现均无；
- ⚠ **新增配置文件必须同步改 `build_release.bat`**——该脚本 `:94-95` 只 `copy` `config.json` 与 `process.json`，漏了就是**部署包静默缺文件**（TR-117 同类的「功能静默消失」）。

**（3）收益已被命名空间化拿到。** 分文件想要的是「算法私有键不污染通用域」「换算法不用改共享 schema」——命名空间节点**两条都给**，且不需要动 `ConfigManager`、不需要定覆盖规则、不需要改打包脚本。

**（4）「一个算法一个文件」明确不做。** 文件数随算法实现数增长，每个都要进 `build_release.bat` 的拷贝清单，版本化的配置文件还要额外维护兼容矩阵。**这是把「配置项膨胀」换成「文件数与部署清单膨胀」，没有净收益。**

**（5）若将来真到分文件那天**，按该文 §6.2 的**生命周期**分法（`machine.json` / `site.json` / `vision.json` / `process.json`），算法配置应落进 **`vision.json`**（随产品与光照走、调参期频繁修改），**仍以命名空间节点形态存在**，而不是每个算法一个文件。本方案的 schema 与该分法**天然兼容**——届时只需把 `vision` 整段搬走。

## 九、落地顺序与台账

**建议顺序**（与 `implementation_roadmap.md` §9.2 的「接口 / 参数扩展 1~1.5 人天」是同一批工作）：

1. **M1 + C6**（迁 `templatePath`）—— 最小、可独立验证。
2. **C1 + C5**（命名空间 + 透传 + `LoadConfig` 认整棵子树）—— 解决调用方耦合，此时 `SimAlgo` 行为**逐字节不变**。
3. **C2**（识别分支改走 `PixelToRobot`）—— 修「仿真对、真机错」。⚠ **与 `hand_eye_calibration_plan.md` §5.1 C1 合并执行**；**✅ 已于 2026-10-13 提前实施**（TR-124）——原定「标定完成后再动」，因该改动不依赖相机、且相机就位后行为等价（`SimCamera` 内参与 `SimAlgo` 硬编码值逐项相同）而提前。实测真内参见 `hand_eye_calibration_plan.md` §五 前置项 1。
4. **C3 + C4**（私有调参量）—— 原定「随深度法算法本体落地，不提前」；**算法本体改由我方实现后与本批合并**（见下方 2026-10-13 段）。
5. **C8**（`PuffResult.x/y` 去留）—— **已裁定 ①**（§十）；实施依赖 C2（已落地），但与本批 schema 决策正交，**另开**。
6. **C7**（动作级覆盖）—— 独立可选项，欠账来自 C6。

**台账**：C2 单独落地记 **TR-124**（2026-10-13）；**C1 + C3 + C4 + C5 + 深度法算法本体（移植 `CPuffLocator`）合并落地记 TR-126（2026-10-14）**。**仍未落地**：**C6 / M1**（迁 `templatePath`——已确认会牵动 `VisionTestPage` 的读写点，见 §六 C6）、**C7**（动作级覆盖）、**C8**（`x/y` 去留）。落地时按当日台账最大号分配，并同步：`TEST_RECORD.md` → 本文状态 → `doc/config.md`（schema 权威落点）→ `doc/README.md` 状态列 → `AGENTS.md` 的 Config 节。

> **⚠ 2026-10-16 追加：TR-131 / TR-132 / TR-135 均属「算法内部改动」，本文**无 C 项变化**。** 三者分别：**TR-131** 节点级 `confidenceThreshold = 0.65`（**用既有私有键**，非新增键）+ 排序键改深度优先；**TR-132** `PuffResult` 加 `bboxX`/`bboxY`、`AlgorithmParams` 加 `apexPercentile`（**C3 的延伸**，已回填 §六 C3 行）；**TR-135** 一个连通域 → 多目标的解切分（距离变换峰 + 尺度自适应 NMS + Voronoi）——**刻意不设配置键**（判据全为相对量、尺度无关），故**本文与 `config.json` 的键表均无变化**。⇒ **若将来有人找「多目标切分的配置项」，答案是「没有，也不该有」**；理由与判据见 `depth_puff_algorithm_plan.md` §四（代码内硬编码量表）。

**2026-10-13 裁定后的实施计划**：§十 5 项已全部裁定（按「建议」列），且算法本体改由**我方**实现 ⇒ **下一批代码 = 第 1~4 步合并落地**：C1 + C5（命名空间 / 透传 / `LoadConfig` 认整棵子树）+ C6/M1（迁 `templatePath`）+ C3/C4（私有调参量，原写「随算法本体」——现在**算法本体就是这批**）+ **深度法算法本体（移植 `CPuffLocator`）**。**C7（动作级覆盖）与 C8（`x/y` 去留）另开**（§十 裁定 4；C8 虽已可做，但与本批 schema 决策正交）。移植段的**代码步骤与参数初值表另开一篇**（`implementation_roadmap.md` §9.6）。

**编译注意**：C3 改的是 `IPuffAlgorithm.h`（**头文件**）。本仓 ninja 依赖追踪在 MSVC 中文版下失效 ⇒ **改头文件必须 `-t clean` 全清重建**，否则会出现「编译期静默、运行期堆越界」或旧签名 `LNK2019`。

**2026-10-14 实施结果（TR-126）**：C1 + C3 + C4 + C5 与**深度法算法本体**（移植 `CPuffLocator` → `DepthPuffAlgo`）已合并落地。Debug + Release **全清重建**均 `[48/48] Linking CXX executable CreamPuffRobot.exe`、**零新增警告**（新增的 `DepthPuffAlgo.cpp` 自身零警告）。⚠ 两侧 POST_BUILD 均报 `FAILED: ... Unable to query qtpaths ... pipe:`——`windeployqt` 的沙箱限制（同 TR-117 起），**非编译 / 链接失败**。**未做**：Sim 冒烟（`simulation.cameraType = OrbbecCamera` 会抢真相机）与真机调参（阶段 3）。**C6 / M1 移出本批**（见 §六 C6）。移植步骤与参数初值表见 `depth_puff_algorithm_plan.md`。

## 十、已裁定项（2026-10-13）

> **裁定结果**：5 项**全部按「建议」列执行**。下表「裁定」列即生效结论，「落地影响」列写明它确定了下游哪一处。

| # | 议题 | 选项 | 建议（= 裁定） | 落地影响 |
| :--- | :--- | :--- | :--- | :--- |
| 1 | 算法节点缺失时的行为 | ① 传 `{}` 并让算法用内置默认值；② 启动时报错拒绝 | **①**——节点缺失是正常状态（新算法首次运行），报错会让人以为软件坏了 | **C1**：`vision.algorithms.<algoType>` 取不到时喂 `{}`，**不报错、不升为 WARN**（可留一条 INFO 便于排查） |
| 2 | `PuffResult.x/y` 的去留（C8） | ① 保留字段，算法侧**不再填**（恒 0），视觉页那两列改由 `PixelToRobot` 实时算并显示基座坐标；② 保留字段且算法侧继续用**真内参**填（即把内参喂进算法）；③ 删除字段 | **①**——与 §3.3 的结论一致。⚠ 选 ② 会重新引入「算法持有内参」的全部问题。③ 影响面大（接口 + 视觉页 + 日志格式），不建议 | **C8**：字段保留、算法侧恒 0；`VisionTestPage.cpp:1268-1269` 两列改由下游算；`SequenceWorker.cpp:1061-1064` 的 `cam(x,y,z)` 日志改打印像素 |
| 3 | 通用键与算法私有键的**覆盖**关系 | ① 算法节点优先，通用域作默认值；② 反之 | **①**——与 `config_ownership_plan.md` C6「动作级覆盖全局」同一逻辑，保持一致 | **C4 / C5**：`SnapshotParams()` 与 `LoadConfig` 先取算法节点，缺则回落通用域（`vision.confidenceThreshold` / `depthZMin` / `depthZMax`） |
| 4 | 是否同批落地 C7（动作级覆盖） | ① 同批；② 另开 | **②**——与本方案的 schema 决策正交，且要动 `SequenceWorker` 的执行分支，风险不同批 | **C7 移出本批**；本批**不动** `SequenceWorker` 的动作级参数分支 |
| 5 | 算法是否需要内参的**升级路径** | 若将来某个算法确实要做内部 3D 推理（点云配准等），内参怎么办 | 届时经 `AlgorithmParams` **注入**（不是「写进算法配置」），并沿用 §3.3 的时序约束。**本方案不预留字段**——避免为假想需求提前付接口成本 | **C3 不加内参字段**（与 §3.3 一致）；该升级路径写入 §十一 契约边界 |

## 十一、算法接口契约（输入 / 输出）

> **缘起（2026-10-13）**：与算法同事对接的结果是**同事暂时没有时间完成算法编写**——现阶段算法**由我方自行实现**（路线 = 移植老项目 `CPuffLocator` 深度法，来源见 `implementation_roadmap.md` §9.4），**但将来仍由同事重写**，且其形态是**独立程序**（不在本仓内、不依赖本仓头文件）。
> **因此本章的定位**：把**输入 / 输出契约定死**。契约是双方**唯一的耦合面**——冻结它，将来换实现（换语言、换进程、换算法路线）只需替换一个**适配器**，`SequenceWorker` / `VisionTestPage` / `CoordTransform` 等下游**零改动**。
> ⚠ 本章只定**契约**（语言无关的数据语义）；移植的**代码步骤与参数初值表**属另一篇（`implementation_roadmap.md` §9.6）。

### 11.1 契约边界：算法做什么、不做什么

| 算法**做** | 算法**不做** |
| :--- | :--- |
| 读**一帧图像**（彩色 + 深度，值语义快照） | ❌ **不读** `ConfigManager`——参数由调用方在**主线程**快照后传入（§二 L2） |
| 在**图像域**定位目标 | ❌ **不持有相机内参**（§3.3 结论 3；§十 裁定 5） |
| 输出**图像域**结果：像素 + 深度 + 尺寸 + 置信度 | ❌ **不做坐标转换**——不碰 `CoordTransform` / 手眼矩阵 |
| 按参数**过滤**结果（置信度、深度范围） | ❌ 不访问相机 / 机械臂 / 网络；除只读日志外**无副作用** |

**唯一分界线**：算法交出的坐标一律是**像素 `(u, v)` + 深度 `mm`**；「像素 + 深度 → 基座坐标」由下游 `CoordTransform::PixelToRobot(u, v, depthMm)` 用**相机真实内参**完成（`SequenceWorker.cpp:1050-1055`，TR-124 已落地）。**这条线就是契约的核心**——越过它，就重新引入了 §3.3 列出的全部问题（内参双持有者、启动时序、分辨率漂移）。

### 11.2 输入契约

**（a）帧**（对应 `CameraFrame`，`ICamera.h:17-42`）

| 字段 | 类型 | 单位 / 取值 | 语义（冻结） |
| :--- | :--- | :--- | :--- |
| `colorData` | `uint8[]` | RGB888，长度 = `width × height × 3` | **彩色图**，通道序 **R, G, B**（`OrbbecCamera.cpp:584` 把任意格式归一化为 RGB888；`FrameConverter` 只认 RGB888） |
| `width` / `height` | `int` | px | **彩色**图尺寸——也是 `pixelU` / `pixelV` 的坐标系 |
| `depthData` | `float[]` | **mm** | 深度图；**`≤ 0` = 无效**（孔洞 / 无深度） |
| `depthWidth` / `depthHeight` | `int` | px | 深度图尺寸；**0 = 与彩色同尺寸**（`ICamera.h:23-37`）。⚠ 消费前必须按本字段（含 0 回落）校验 `depthData.size()`，否则 D2C 关闭时越界（TR-116） |
| `timestamp` | `int64` | 实现定义 | 帧时间戳，仅用于日志与去重 |
| `pointCloud` | `float[]` | — | 可选；**本阶段不用**（契约保留，不消费） |

**（b）参数快照**（对应 `AlgorithmParams`，`IPuffAlgorithm.h:28-33`）

| 分组 | 键 | 归属层 |
| :--- | :--- | :--- |
| **通用（跨算法）** | `confidenceThreshold` / `depthZMin` / `depthZMax` | `vision.*`（§四） |
| **算法私有** | `vision.algorithms.<algoType>.*`——如 `roi` / `nearestPercentile` / `heightBandMm` / `apexPercentile` / `minArea` / `maxArea` / `targetArea` / `targetAreaTolerance` / `morphKernelSize` / `edgeMargin` / `templatePath` | 算法节点（§四 / §五） |

⚠ **参数只以「快照」形式进入算法**：调用方在**主线程**调 `SnapshotParams()` 取一份值语义结构，算法**不回头读任何全局状态**（§二）。覆盖关系 = 算法节点优先、通用域作默认（§十 裁定 3）。

⚠ **`roi` / `edgeMargin` / `minArea` / `maxArea` 的坐标系 = 深度图**（算法在深度域做连通域），而 `pixelU` / `pixelV` 输出在**彩色系**（§11.3）。D2C = `SW` 时两分辨率相同、二者等价；D2C 关闭时填参数须按深度分辨率（`depth_puff_algorithm_plan.md` §七 M5）。

### 11.3 输出契约

输出 = `std::vector<PuffResult>`（`IPuffAlgorithm.h:8-19`；本次按 §十 裁定 2 与 C3 扩展）。

| 字段 | 类型 | 单位 | 语义（冻结） |
| :--- | :--- | :--- | :--- |
| `pixelU` / `pixelV` | `int` | px | 目标**球顶**在**彩色图**的像素坐标（原点左上，x 右、y 下）。⚠ **语义已定死为「球顶」**（TR-132，2026-10-16）——即连通域内最高点所在面片的质心，**不是**连通域质心：真机帧实测二者相距 **18~33 px**（球顶面片）／原始最高点与质心相距 **~47 px**（≈23 mm）。定死的理由 = 与 `z` 必须指向**同一个物理点**，否则 `PixelToRobot(u, v, z)` 反投影出一个**不在目标表面上**的点 |
| `z` | `double` | mm | 目标深度 = **球顶面片均值深度**。⚠ 语义 = 「区域内最高点」（`implementation_roadmap.md` §9.5 待定项 4 由此关闭），但取**面片均值**而非单像素极小值（TR-132）：深度传感器在轮廓处会产生 flying pixel 伪影——实测某球 `min = 513 mm` 而球顶面片均值 `= 522.6 mm`（差 9.6 mm 全属噪点），且**帧间抖动**由 ±3.0 mm 降到 ±1.0 mm |
| `bboxX` / `bboxY` | `int` | px | **新增**（TR-132）：包围框**左上角**（**彩色系**）。与 `width` / `height` 合起来即完整包围框。⚠ 独立给出原点而非「以某点为中心」：球顶 ≠ 框中心，只给「中心 + 尺寸」无法还原包围框 |
| `area` | `double` | px² | **新增**（C3）：目标像素面积，供调参 / 判定。⚠ **坐标系 = 深度图**（算法在深度域做连通域），与 `pixelU` / `pixelV` 的**彩色系**不同；D2C = `SW` 时两分辨率相同、二者等价（`depth_puff_algorithm_plan.md` §七 M2） |
| `minDepthMm` | `double` | mm | **新增**（C3）：连通域内**原始**最小深度。⚠ **TR-132 起与 `z` 不再恒等**——`z` 取球顶面片均值（抗噪），本字段保留原始极值 ⇒ 二者之差即该目标上的**深度噪点 / 尖点幅度**，供交叉校验与调参（原记「= 最高点深度，与 `z` 同源」已被本次改动取代） |
| `width` / `height` | `int` | px | 目标包围框尺寸（图像域）。⚠ 与 `bboxX` / `bboxY` 配对使用（TR-132） |
| `confidence` | `double` | 0~1 | 置信度，越大越可信 |
| `x` / `y` | `double` | — | ⚠ **弃用**（§十 裁定 2）：字段保留，**算法侧不再填**（恒 0）。基座坐标由下游 `PixelToRobot` 算 |
| `yaw` | `double` | ° | 图像平面内偏航角；**本项目无工艺意义**（`vision_guide_action_plan.md` §七），`r` 一律取动作的 `guideR` |

**三条必须写死的输出约定**：

1. **`results.front()` = 最佳目标**。这是**隐性契约**——`SequenceWorker.cpp:1034` 直接取 `results.front()` 作为抓取目标。算法**必须**把最佳结果排在首位（`SimAlgo` 现按连通域顺序 `push`、**未排序**，移植时须显式排序或保证顺序）。⚠ 此约定被破坏会**静默抓错目标**。
2. **空结果 = 未检出**，不是异常。`Detect` **不抛异常**，错误经 `GetLastError()` 以文本返回。
3. **过滤即丢弃**：不满足参数（置信度 / 深度范围）的目标**不出现在结果里**，不是「置 0 保留」（`SimAlgo.cpp:129-138`，TR-103 语义）。

### 11.4 序列化形态（语言无关）

> **为什么需要**：同事的代码是**独立程序**，无法直接吃 C++ 结构体 ⇒ 契约必须有一份**可跨进程 / 跨语言**的等价表达。**本节即该表达**，与 §11.2 / §11.3 一一对应。

**输入请求**（我方 → 算法）：

```json
{
  "protocolVersion": 1,
  "frame": {
    "width": 1280, "height": 720, "colorFormat": "RGB888",
    "colorPath": "C:/.../frame_000123_color.bin",
    "depthWidth": 1280, "depthHeight": 720, "depthFormat": "F32_MM",
    "depthPath": "C:/.../frame_000123_depth.bin",
    "timestamp": 1760000000000
  },
  "params": {
    "confidenceThreshold": 0.85,
    "depthZMin": 6.0, "depthZMax": 496.0,
    "nearestPercentile": 0.03, "heightBandMm": 35.0,
    "minArea": 120, "maxArea": 250000,
    "targetArea": 0, "targetAreaTolerance": 0,
    "morphKernelSize": 7, "edgeMargin": 8,
    "roi": { "x": 0, "y": 0, "w": 0, "h": 0 }
  }
}
```

**输出响应**（算法 → 我方）：

```json
{
  "protocolVersion": 1,
  "error": "",
  "results": [
    { "pixelU": 640, "pixelV": 336, "z": 412.5,
      "bboxX": 552, "bboxY": 300, "width": 180, "height": 175,
      "area": 24500, "minDepthMm": 405.0, "confidence": 0.93 }
  ]
}
```

**形态约定**：

- **图像走文件、不走 JSON**：一帧约 4.2 MB（`ICamera.h:44-57`），base64 进 JSON 会膨胀 ~33% 且吃内存 ⇒ 契约规定**像素数据落临时文件**（`.bin`，尺寸 / 格式由 JSON 给出），JSON 只带**路径 + 尺寸 + 格式**。
- **`params` 是 §五 schema 的展平**：通用域 + 算法私有域合并为一个扁平对象，键名与 `config.json` **逐字相同**（不另立命名）。
- **`error` 为空串 = 成功**；非空 = 失败原因（对应 `GetLastError()`）。
- **`results` 数组顺序即优先级**：第 0 个 = 最佳（§11.3 约定 1）。

### 11.5 承载与升级路径

| 承载形态 | 谁实现 | 我方代码 | 契约编码 |
| :--- | :--- | :--- | :--- |
| **进程内 · 静态链接**（现阶段） | 我方（`DepthPuffAlgo`，移植 `CPuffLocator`） | 新增算法类 + `REGISTER_PUFF_ALGORITHM` 注册 | C++ 结构体直传（§11.2 / §11.3） |
| **进程内 · 动态加载**（DLL） | 同事（导出 **C ABI**） | 新增**适配器**：`LoadLibrary` + `GetProcAddress` → 手工 `Register`（⚠ **不能靠注册宏**，见 §十二 12.3） | C ABI 头文件 + POD（⚠ **尚未成文**） |
| **进程外**（独立 exe） | 同事（独立程序） | 新增**适配器** `ExternalPuffAlgo`：取帧 → 落临时文件 → 序列化 §11.4 JSON → 调外部进程 → 解析响应 → 转回 `PuffResult` | §11.4 JSON |

⇒ **契约冻结的收益就体现在这里**：`simulation.algorithmType` 从 `DepthPuffAlgo` 换成 `DllPuffAlgo` / `ExternalPuffAlgo` 即可切换，**没有第二处要改**（这也是 §五 设计要点 1「键名 = `simulation.algorithmType`」的另一重意义）。三种承载的**语义契约完全相同**（§11.2 / §11.3），差别只在**编码**——这也是 §十二 只管「位置与构建」、不碰字段定义的原因。

⚠ **适配器对契约的前提要求**：`CameraFrame` / `AlgorithmParams` / `PuffResult` 三者都必须是**纯值语义**（无指针、无隐式全局状态），才能被完整序列化。§11.2 / §11.3 已满足；**将来新增字段时必须守住这一条**。

### 11.6 契约冻结清单与未决项

**冻结（改动须双方确认）**：

1. 彩色图语义：RGB888、**彩色坐标系**、`width` / `height` 指彩色。
2. 深度图语义：`float` mm、`depthWidth` / `depthHeight`（0 = 同彩色）、`≤ 0` = 无效。
3. 参数键名：通用域 3 键 + 算法私有键，与 `config.json` 逐字一致。
4. 输出字段名 + 单位 + 语义（§11.3 表），含 `front() = best` 与「过滤即丢弃」。
5. 边界：算法**不碰内参 / 手眼 / 坐标转换 / `ConfigManager`**。

**未决（须与算法同事确认后再定稿）**：

- ⚠ **传输方式**（stdin / stdout 单次调用 · 常驻进程 + 本地 socket · 文件轮询）——影响 §11.4 的**调用编排**，不影响 payload 本身。
- ⚠ **`protocolVersion` 的兼容规则**：字段增删时谁向后兼容谁，目前仅占位。
- ⚠ **C ABI 头文件契约尚未成文**（仅当同事确定交 DLL 时必需）：与 §11.4 **同源、编码不同**，见 §十二 12.6。

## 十二、承载位置与构建集成（2026-10-14）

> **缘起**：澄清「同事的交付物（源码 / 静态库 / DLL / exe）放进本仓哪个目录」时暴露的构建与部署约束。
> **范围**：本章只讲「**放哪儿、怎么编、怎么部署**」。接口语义一律以 §11.2 / §11.3 为准，**本章不改任何字段**。
> **一句话结论**：exe **必然**是进程外；DLL 是**载体**，落在哪一侧取决于**导出接口的形态**——导出 C ABI 则属进程内（用不上 §11.4），导出 C++ 类则踩 ABI 雷区（§12.4）。

### 12.1 位置归属

| 交付物 | 建议位置 | 入库 | 理由 |
| :--- | :--- | :--- | :--- |
| 同事的**源码 / 静态库** | `src/HAL/algorithm/` | 是 | 属本仓源码，编进 HAL |
| 同事的 **DLL / exe** | `3rdparty/algorithm/` | 否 | 外部二进制产物，性质同 `3rdparty/orbbec/`（已整体 gitignore） |
| **适配器**（我方写） | `src/HAL/algorithm/` | 是 | 是本仓源码，必须随仓库走 |

⚠ **放 `algorithm/` ≠ 会被编译**（12.2）；⚠ **放 `algorithm/` 的二进制也不会进部署包**（12.5）。

### 12.2 CMake 集成

| # | 约束 | 依据 | 后果 |
| :--- | :--- | :--- | :--- |
| a | `HAL_SOURCES` 是**显式列举、非 GLOB** | `src/HAL/CMakeLists.txt:66-78` | 新增 `.cpp` **必须手工加进列表**，否则静默不编译 |
| b | HAL 是 `STATIC` 库 | `:117` | 若把含 `main()` 的源文件加进 `HAL_SOURCES` ⇒ 与主程序 `main` 符号重复（LNK2005） |
| c | HAL **不链接 OpenCV** | `:131-136`（只链 `Qt6::Core/Gui/Config/spdlog`） | 深度法 / 同事代码若用 OpenCV，须给 HAL 补 `${OpenCV_LIBS}` |
| d | 本仓**无任何动态加载机制** | `src/` 全仓无 `LoadLibrary` / `GetProcAddress` / `QLibrary` / `__declspec(dllexport)` | DLL 形态须**从零新增**加载器 |

### 12.3 注册机制：DLL 形态的专属陷阱

`HardwareFactory<T>::Instance()`（`HALFactory.h:24-28`）是**函数内静态局部变量**；`REGISTER_PUFF_ALGORITHM`（`:97-107`）靠匿名命名空间的静态对象在**模块加载时**注册。

⚠ **模板静态变量按 PE 模块各存一份** ⇒ 同事的 DLL 内若用该宏注册，写进的是 **DLL 自己那份注册表**；主 exe 的 `PuffAlgorithmFactory::Instance()`（`HardwareManager.cpp:154`）**看不到** ⇒ `Create()` 返回 `nullptr` ⇒ **静默回落到 `SimAlgo`**（用户以为在跑真算法）。

⇒ **DLL 必须导出 C ABI 创建函数**，由主 exe 侧 `LoadLibrary` + `GetProcAddress` 后**手工调用 `Register`**。⚠ 加载失败必须**显式播报**，不得静默降级（与 `USE_ORBBEC` 的 `message(STATUS/FATAL_ERROR)` 同一逻辑，TR-117 教训）。

### 12.4 ABI 约束

- 本仓未设 `CMAKE_MSVC_RUNTIME_LIBRARY` ⇒ 走 vcpkg 默认：**Debug `/MDd`、Release `/MD`**（两套 CRT）。
- ⚠ **同一 DLL 无法同时服务两版**——只要它跨边界传 C++ 类型（`std::string` / `std::vector<PuffResult>`），Debug 与 Release 的 STL 对象布局不同，混用即 UB（`_ITERATOR_DEBUG_LEVEL` 不匹配，典型表现是**编译通过、运行崩**）。而本仓**两版都要跑**（用户真机用 Debug，`build_release.bat` 出 Release）。
- ⇒ 导出面必须是 **C ABI + POD 结构体**；内存归属明确：**调用方分配**，或由 DLL 提供配套释放函数（跨 CRT `malloc` / `free` 会崩）。

### 12.5 部署

| # | 约束 | 依据 | 后果 |
| :--- | :--- | :--- | :--- |
| a | `copy /Y "%BUILD%\*.dll" "%DIST%\"` **只扫构建目录顶层** | `build_release.bat:86` | 放子目录的 DLL **不进部署包**（`extensions/` 是 `:92` 单独 xcopy 的特例） |
| b | 运行期 `LoadLibrary` **默认不搜子目录** | Windows 默认搜索序 | 须用绝对 / 相对路径，或 `SetDllDirectory` |
| c | 新增随包文件须同步打包脚本 | 同 `config.json` / `process.json`（`:94-95`） | 遗漏 = 部署包静默缺件 |

### 12.6 契约落点对照

§11.2 / §11.3 是**语义层**，与承载无关。三种承载各有一份**编码**：

| 承载 | 编码 | 定义落点 |
| :--- | :--- | :--- |
| 进程内 · 静态链接 | C++ 结构体直传 | §11.2 / §11.3（无需额外编码） |
| 进程内 · 动态加载 | **C ABI 头文件 + POD** | ⚠ **尚未成文**——同事确定交 DLL 时须补一份（与 §11.4 **同源、编码不同**） |
| 进程外 | JSON | §11.4 |

⚠ **三种承载的语义契约完全相同** ⇒ 换承载不改字段，只加一层编码 / 适配。
