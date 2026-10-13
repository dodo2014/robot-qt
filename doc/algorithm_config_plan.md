# 算法参数与配置归属方案

> **需求来源**：用户 2026-10-12 提问——「算法需要的各种配置项，比如 roi 区域，fx / fy 之类，是否需要单独的配置文件？因为不同的算法版本，需要的配置可能不同，这个需求是否合理？」
> **结论**：**需求本身合理，但落点不是「单独配置文件」，而是「按算法名命名空间化」**；并且 **`fx` / `fy` / `cx` / `cy` 根本不该进入算法**——它们是相机内参，算法只需要输出**像素坐标 + 深度**，XY 由下游 `CoordTransform::PixelToRobot` 用真实内参反投影。
> **状态**：⏳ 计划中（未实施）。本文只定 **schema 与归属**，**不含深度法算法本体的移植步骤**（那一篇另开，见 `implementation_roadmap.md` §9.6）。
> **关联**：`config_ownership_plan.md`（分文件的触发条件 §6.1 与代价 §6.3，**本文 §八 以它为前置**）、`doc/config.md`（schema 权威落点，本文 §七 要求同步）、`implementation_roadmap.md` §九（④ 的工期与待定项）、`hand_eye_calibration_plan.md`（§5.1 C1 与本文 §六 C2 **是同一处改动**，两篇须合并执行）。

## 一、问题与结论

**问题**：不同算法实现需要的配置键不同（`SimAlgo` 只要 `confidenceThreshold`，深度法还要 ROI / `heightBandMm` / `nearestPercentile` 等十项），把它们的并集塞进 `vision.*` 会持续膨胀。是否应该给算法配置单独开文件？

**结论分三层**：

| # | 结论 | 一句话理由 |
| :--- | :--- | :--- |
| 1 | **「按算法名命名空间化」应做** | 真正的问题是**调用方知道算法私有键名**（`HardwareManager.cpp:161-163` 手工拼 JSON），不是「配置放在哪个文件」。命名空间化能用零基础设施成本解决它 |
| 2 | **「单独配置文件」暂不做** | 触发条件（`config_ownership_plan.md` §6.1）未满足；分文件的代价（同文 §6.3）与收益不成比例。**「一个算法一个文件」尤其不做** |
| 3 | **内参不进算法，也不进算法配置** | 算法只输出**像素 + 深度**；XY 由下游 `PixelToRobot(u, v, z)` 用真实内参反投影（`CoordTransform.cpp:62-76`，**已实现、全仓无调用者**）。内参的唯一所有者是相机 |

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
| **C1** | 调用方改为**透传整棵子树** | `HardwareManager.cpp:161-163` | 由手工拼 `{"confidenceThreshold": …}` 改为取 `vision.algorithms.<algoType>` 整块 `.dump()` 后喂 `LoadConfig()`；节点缺失时传 `{}`（**不是失败**，见 §十 待裁定 1） |
| **C2** | **识别分支改走 `PixelToRobot`** | `SequenceWorker.cpp:1035` | 由 `CameraToRobot(best.x, best.y, best.z)` 改为「`InMainThread` 取 `camera->GetIntrinsics()` → `coord.SetCameraIntrinsics(fx, fy, cx, cy)` → `coord.PixelToRobot(best.pixelU, best.pixelV, best.z)`」。⚠ **与 `hand_eye_calibration_plan.md` §5.1 C1 是同一处改动**，两篇须合并执行、不要做两遍 |
| **C3** | `AlgorithmParams` 加**算法私有调参量** | `IPuffAlgorithm.h:28-33` | 深度法所需 8 项 + `roi`。⚠ **不加内参字段**（§3.3）。⚠ 只加**当前已注册实现**的量；这是接口结构体，改它要走全清重建 |
| **C4** | `SnapshotParams()` 补齐新字段 | `SimAlgo.cpp:21-30` | 私有量从 `vision.algorithms.<名>` 读，通用量维持现读法 |
| **C5** | `LoadConfig` 解析自己的整棵子树 | `SimAlgo.cpp:190-211` | 从「只认 `confidenceThreshold`」改为「认整棵子树」，兜底值语义不变 |
| **C6** | `vision.templatePath` 迁到算法节点 | `config.json` + `doc/config.md:137,143` | 见 §七 M1 |
| **C7** | 动作级覆盖落地（**可选，属 C6 欠账**） | `SequenceWorker.cpp:1007,1011,1023` | 让 `ActionData.threshold` 真正覆盖 `algoParams.confidenceThreshold`，并消除 §3.2 的语义倒挂。**与本方案正交，可另开** |
| **C8** | `PuffResult.x/y` 的去留 | `IPuffAlgorithm.h:10-11`；`VisionTestPage` 结果表 | 见 §十 待裁定 2。**不实施 C2 则本项无意义** |

**验收判据**：

1. `HardwareManager` 中**不再出现任何算法私有键名字面量**；新增一个算法实现时 `HardwareManager` 零改动。
2. 识别动作日志 `[SequenceWorker] Vision target: cam(…) → robot(…)` 中的 `cam(x,y,z)` 与真机真实内参自洽（用同一靶点，`PixelToRobot` 结果与手算一致）。

## 七、须修正项

| # | 问题 | 处理 |
| :--- | :--- | :--- |
| **M1** | `vision.templatePath` 放在**通用域**却只对某个算法有意义，且**至今无读点**（`doc/config.md:143` 自述「不要误以为它已在工作」） | 迁入 `vision.algorithms.<名>.templatePath`。**迁键与补消费点应同批做**——只迁键不补读点，等于把死键换个地方继续死 |
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
3. **C2**（识别分支改走 `PixelToRobot`）—— 修「仿真对、真机错」。⚠ **与 `hand_eye_calibration_plan.md` §5.1 C1 合并执行**；用户 2026-10-12 已要求标定完成后再动，故本步**排在标定之后**。
4. **C3 + C4**（私有调参量）—— 随深度法算法本体落地，不提前。
5. **C8**（`PuffResult.x/y` 去留）—— 随 C2 一并定。
6. **C7**（动作级覆盖）—— 独立可选项，欠账来自 C6。

**台账**：本批**尚未产生代码改动**，不占 TR 编号。落地时按当日台账最大号分配（现为 TR-123），并同步：`TEST_RECORD.md` → 本文状态 → `doc/config.md`（schema 权威落点）→ `doc/README.md` 状态列 → `AGENTS.md` 的 Config 节。

**编译注意**：C3 改的是 `IPuffAlgorithm.h`（**头文件**）。本仓 ninja 依赖追踪在 MSVC 中文版下失效 ⇒ **改头文件必须 `-t clean` 全清重建**，否则会出现「编译期静默、运行期堆越界」或旧签名 `LNK2019`。

## 十、待裁定项

| # | 议题 | 选项 | 建议 |
| :--- | :--- | :--- | :--- |
| 1 | 算法节点缺失时的行为 | ① 传 `{}` 并让算法用内置默认值；② 启动时报错拒绝 | **①**——节点缺失是正常状态（新算法首次运行），报错会让人以为软件坏了 |
| 2 | `PuffResult.x/y` 的去留（C8） | ① 保留字段，算法侧**不再填**（恒 0），视觉页那两列改由 `PixelToRobot` 实时算并显示基座坐标；② 保留字段且算法侧继续用**真内参**填（即把内参喂进算法）；③ 删除字段 | **①**——与 §3.3 的结论一致。⚠ 选 ② 会重新引入「算法持有内参」的全部问题。③ 影响面大（接口 + 视觉页 + 日志格式），不建议 |
| 3 | 通用键与算法私有键的**覆盖**关系 | ① 算法节点优先，通用域作默认值；② 反之 | **①**——与 `config_ownership_plan.md` C6「动作级覆盖全局」同一逻辑，保持一致 |
| 4 | 是否同批落地 C7（动作级覆盖） | ① 同批；② 另开 | **②**——与本方案的 schema 决策正交，且要动 `SequenceWorker` 的执行分支，风险不同批 |
| 5 | 算法是否需要内参的**升级路径** | 若将来某个算法确实要做内部 3D 推理（点云配准等），内参怎么办 | 届时经 `AlgorithmParams` **注入**（不是「写进算法配置」），并沿用 §3.3 的时序约束。**本方案不预留字段**——避免为假想需求提前付接口成本 |
