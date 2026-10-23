# 深度法泡芙定位算法：原理与实现

> **本文回答什么**：`DepthPuffAlgo` **现在怎么算**——流水线每一步的目的、判据、关键常量与失败回退，每条判据附实测依据与对应台账条目。
> **本文不回答什么**：为什么移植（`depth_puff_algorithm_plan.md`）、接口契约（`algorithm_config_plan.md` §十一）、参数 schema 与现行值（`config.md`）。
> **成稿**：2026-10-23。**状态**：🟢 现行——按 TR-151 收口后的代码逐函数核对（`src/HAL/algorithm/DepthPuffAlgo.{h,cpp}`）。**技术事实以源码为准**：本文与源码冲突时请修正本文。
> **关联**：`depth_puff_algorithm_plan.md`（移植方案与阶段状态）、`algorithm_config_plan.md` §十一（输入 / 输出契约）、`config.md`（`vision.algorithms.DepthPuffAlgo` 键与现行值）、`TEST_RECORD.md`（TR-126 / TR-127 / TR-131 / TR-132 / TR-135 / TR-144 / TR-145 / TR-147 / TR-148 / TR-149 / TR-151）。

## 一、算法定位

**一句话**：只吃深度图，把「最近的那一层曲面」切出来，当成泡芙的球顶，输出**像素 + 深度**。

| 做 | 不做 |
| :--- | :--- |
| 深度预处理 → 有效掩码 → 锚点扫描 → 种子生长 → 连通域分析 → 峰切分解合并 → 综合打分 | ❌ 不用颜色、不用模板、不用深度学习（RGB 入口仅预留，`Detect` 只读 `depthData`） |
| 输出球顶像素 `(pixelU, pixelV)` + 球顶深度 `z` + 包围框 + 面积 + 置信度 | ❌ 不做像素 → 基座坐标转换（`CoordTransform::PixelToRobot` 负责，TR-124） |
| 参数由调用方快照后**按值传入**，`Detect` 是纯函数 | ❌ 不持有相机内参、不读 `ConfigManager`、不写任何全局状态（除 `lastError_`，`mutex_` 保护） |

这套边界由 `algorithm_config_plan.md` §十一 冻结，目的是将来算法由第三方以独立程序重写时，下游零改动。

**与 `SimAlgo` 的区别**（`config.md` 已记，此处只作对照）：`SimAlgo` 是 RGB 色块匹配的**占位**实现，`confidence` 存面积覆盖率，鲁棒性无保证；`DepthPuffAlgo` 的 `confidence` 存三权重综合 `score`。两者**同名不同义**，同一个 `confidenceThreshold` 在两者上松紧不等价（TR-126 / M1）。

## 二、输入与输出

### 2.1 输入

| 项 | 来源 | 约束 |
| :--- | :--- | :--- |
| 深度图 | `CameraFrame::depthData` | 已是 `float` 毫米（`OrbbecCamera` 用 `getValueScale()` 转好）⇒ 无 `depthScaleToMm` |
| 深度图尺寸 | `depthWidth` / `depthHeight` | **0 回落** `width` / `height`；`depthData.size()` 不足即拒帧（TR-116，否则越界 `0xC0000005`） |
| 彩色图尺寸 | `width` / `height` | 只用于**输出像素归一**与 `centerScore`，算法不读 `colorData` |
| 参数 | `AlgorithmParams` 快照 | `SnapshotParams()` **只能在主线程调**；`Detect` 本身可在任意线程 |

### 2.2 输出（`PuffResult`，按球顶深度升序）

| 字段 | 语义 | 坐标系 |
| :--- | :--- | :--- |
| `pixelU` / `pixelV` | **球顶**面片质心 | 彩色系 |
| `z` | **球顶面片均值**深度 | mm |
| `bboxX` / `bboxY` / `width` / `height` | 连通域紧包围框，**独立给出左上角** | 彩色系 |
| `area` | 连通域有效像素数 | **深度系**（不随像素归一缩放） |
| `minDepthMm` | 连通域**原始**最小深度，仅供交叉校验 | mm |
| `confidence` | `= clamp(score)`，三权重综合分 | — |
| `x` / `y` / `yaw` | **恒 0**（弃用，保留字段） | — |

**三条硬不变量**（违反任一即抓错点）：

1. **`pixelU/pixelV` 与 `z` 必须指向同一个物理点。** 二者都取「球顶面片」——质心配球顶深度会让 `PixelToRobot` 反投影出一个**不在目标表面上**的点。真机实测质心与球顶相距约 **47 px（≈23 mm）**（TR-132）。
2. **`results.front()` = 最高目标。** 排序键是 `apexDepthMm` **升序**，`score` 只作并列次级键（TR-131 / TR-132）。夹爪自上方下探，最高者可达且不与其它目标干涉。
3. **像素在彩色系、`area` 在深度系。** D2C = `SW` 时两分辨率相同、比例映射退化为恒等；D2C 关闭时 `u/v/bbox` 乘 `cw/dw`、`ch/dh`，而 `area` 保持深度域原值（契约 §11.3）。

## 三、流水线总览

| # | 步骤 | 实现 | 一句话 | 失败即返回 |
| :- | :--- | :--- | :--- | :--- |
| 0 | 预处理 | `Detect` 开头 | 校验深度尺寸 → 零拷贝 `cv::Mat` 包装 → ROI 与图像求交 | `invalid depth dimensions` / `depth data smaller than …` |
| 1 | 有效掩码 | `BuildValidMask` | ROI 内「有限值且落在 `[depthZMin, depthZMax]`」的像素 | `no valid depth pixels in ROI` |
| 2 | 锚点扫描 | `InitAnchorSweep` / `NextAnchor` | 从近到远逐层抬高截断深度，产出**面积合格、不贴边、未被近端截断**的面片最低深度 | `no seeds in ROI` |
| 3 | 种子提取 | `BuildSeedsForAnchor` | 锚点上方 `kSeedBandMm` 带内的连通面片 = 一个目标顶点 | 换下一个锚点 |
| 4 | 区域生长 | `GrowSeedRegion` | 候选 = 深度 ≤ 种子顶点 + `heightBandMm`，取含种子的连通域 | 换下一个锚点 |
| 5 | 峰切分 | `FindMarkers` / `VoronoiSplit` | 一个连通域可能装着多个目标（`MORPH_CLOSE` 桥接）⇒ 按距离变换峰切开 | 回退区域级单目标 |
| 6 | 测量 | `MeasureRegion` / `ComputeApex` | 面积、质心、均值、标准差、球顶面片 + 三道硬过滤 | 丢弃该候选 |
| 7 | 打分排序 | `SelectRegions` 末尾 | `score` 三权重 + 按球顶深度升序 | — |
| 8 | 阈值过滤 | `Detect` 循环体 | `confidence < confidenceThreshold` 丢弃；全被滤掉则换下一个锚点 | `all candidates below confidenceThreshold` |

外层是一个**多锚点重试循环**：每个锚点各跑一遍「3 → 8」，取第一个能出过阈值目标的锚点（`kAnchorMaxAttempts = 8`）。

## 四、逐步原理

### 4.1 有效掩码：检测的前置条件，不是事后过滤

`BuildValidMask` 只做一件事：ROI 内、`std::isfinite(value)`、`zMin ≤ value ≤ zMax` 的像素置 255，同时统计有效像素数（为 0 直接拒帧）。

与 `SimAlgo` 的关键差异：`SimAlgo` 把超范围的**结果**丢弃，`DepthPuffAlgo` 把超范围的**像素**排除在连通域之外 ⇒ 背景（如仿真 `600 mm`）根本不参与运算。这也意味着 `depthZMin` / `depthZMax` 是**检测的前置条件**——设错不是「少检出几个」，而是「什么都检不出」。

⚠ `depthZMin` 同时是**近端截断边界**：比它更近的物体被切掉后只剩一层薄壳，其真实深度与真实面积都不可知。锚点扫描为此专门加了近端截断守卫（§4.2）。

### 4.2 锚点扫描：为什么必须从近到远逐层抬高

**问题的根**：`validMask` 里目标与它所在的承载面在 2D 上是**连成一片**的。直接对 `validMask` 做一次连通域，得到的永远是「贴着 ROI 边界、面积几十万像素」的巨域，`minArea` / `maxArea` / 边界过滤全过不去，一无所获。

**做法**：从 `lo` 起步，每次抬高截断深度 `cut`，对 `valid && depth ≤ cut` 做 8 连通域分析。随着 `cut` 抬高，目标会**在与背景合并之前**先长成一个面积合格的独立面片——那一刻的面片最低深度就是该目标的**顶点深度**，取它作**锚点**。

三道过滤（缺一不可）：

| 过滤 | 判据 | 挡什么 |
| :--- | :--- | :--- |
| 面积 | `CC_STAT_AREA ≥ max(kSeedMinPixels, minArea)` | 零星噪点、深度图边缘毛刺 |
| 边界 | 包围框四边都在 `edgeMargin` 之内 | 被 ROI 裁断的面片（散热器、背景残片都在边界侧） |
| 近端截断守卫 | `minDepth > depthZMin + kAnchorNearClipTolMm`（1 mm） | 被 `depthZMin` 切掉的薄壳——深度与面积都不可信 |

**起点与步长**（TR-149）：起点不是「ROI 内全局最小有效深度」，而是 `nearestPercentile` **分位**（代码内钳到 `[0.001, 0.005]`），步长**分级**——前 8 次 2 mm 精扫近处，之后转 10 mm 粗扫。总预算 `8 × 2 + 56 × 10 = 576 mm`。

> **为什么改起点**：实测 `depth_20261021135309_0`（球顶 586 mm），ROI 里 430~480 mm 有 300 余个零星噪声像素 ⇒ `lo = 430`，够到球顶要 78 次迭代 > 64 上限 ⇒ 锚点永远够不到球，报 `no seeds in ROI`。分位**只定起点**，不承担「锚点」职责。
> **为什么改步长**：泡芙球顶到承载面的深度跨度实测 ≥ 36 mm，10 mm 粗步长不会跨过整个目标。

**为什么不「首个合格面片即返回」**（TR-151）：TR-145 一找到合格面片就把锚点定死，于是**任何**比目标更近、又不贴 ROI 边界的物体都能否决整帧。实测 5 帧：机械臂末端侵入 ROI 右下角，深度 421~433 mm，被 `depthZMin = 430` 切成一层 3 mm 厚的薄壳（123~244 px > `minArea = 120`）；这层壳与 ROI 边界是否 8 连通只差 5~6 px，深度噪声一抖就断开 ⇒ 锚点落在 430 ⇒ 种子带 430~440 够不到球顶 584 ⇒ 只长出那块小面片（`score` 0.42~0.47 < 0.65）⇒ 报「未检出」。同一场景连拍 8 帧，5 帧漏检 / 3 帧正常，**差别只在这块薄壳有没有断开**（11 帧 100% 相关）。

⇒ 现行为：`NextAnchor` **逐个产出**全部合格锚点（同层面片按最低深度升序，跨层按 `kAnchorDedupMm = 2 mm` 去重），调用方对每个锚点各跑一遍「种子 → 生长 → 过滤」，取第一个出结果的。

**全扫描失败的兜底**：若 64 层扫完一个合格面片都没有，退回 TR-144 的全局分位路径（`BuildSeedsFromPercentile`，整帧只试一次），日志 `anchor=percentile`。这是**防御性路径**，正常场景不会走到。

### 4.3 种子：把「锚点」翻译成「某个目标的顶点」

`BuildSeedsForAnchor` 取 `valid && depth ≤ anchor + kSeedBandMm`（10 mm）的像素，做 3×3 椭圆**开运算**（只开不闭——闭会把相邻顶点桥接），再取连通域，面积 ≥ 20 px、包围框不贴 ROI 边界者即为种子；`seed.minDepthMm` = 该面片最低深度 = 该目标的顶点。

**为什么需要这一层**：TR-144 之前，顶层带厚是锚在**全 ROI 有效深度的分位**上的——目标只占画面一小块时，分位落在背景面上，顶层把背景一起圈进来。实测 `depth_20261020152105_0`：球顶 526 mm、背景 567 mm、3% 分位 = 561 mm ⇒ 上限 581 mm 覆盖背景 ⇒ 球与背景连成 **218487 px** 的连通域，绿框 579×406（球本身只有 91×102）。根因是基准随「目标占画面比例」漂移。

⇒ 锚点只用来定**种子**，每个种子代表一个目标顶点；此后每个目标**各自**以「种子顶点 + `heightBandMm`」定带，不再受其它目标或背景占画面比例的影响。

种子按 `minDepthMm` 升序处理——先长最近的那个。

### 4.4 生长：带厚锚在目标自己的顶点上

`GrowSeedRegion` 令候选 = `valid && depth ≤ seed.minDepthMm + max(1.0, heightBandMm)`，从种子质心做 8 连通 `floodFill`（填充值 128 与掩码的 0/255 区分），得到该目标所在连通域；随后在**该域紧包围框外扩核半径**内做开 + 闭（`morphKernelSize`，偶数自动 +1，≤ 1 跳过）。

- 带厚锚在顶点上 ⇒ 背景面在带外，不会被并进来。
- 形态学只在包围框内做：整图做会随种子数线性放大开销，空桌帧实测有 30 个种子。
- **闭运算的副作用**是本算法最大的坑：它会把**接触 / 重叠**的目标桥接成单域（§4.5）。

同一区域覆盖到的其它种子会被标记为「已消费」，避免重复计数；与已接受区域重叠过半的候选直接丢弃（平背景上多个种子长出同一片表面时会出现）。

### 4.5 峰切分：一个连通域 ≠ 一个目标

**现象**：三球挤在一起时闭运算把它们桥接成单连通域。实测 `depth_20261016153457_0`：合并域 `area = 18458`（≈ 三球之和）、`bbox = 160×177`；三球分开时各 `area ≈ 6458 / 5909 / 5025`、`bbox ≈ 96×92`。区域级只能产出 1 个目标 ⇒ 用户报障「三个纸团只检测出一个」。

**做法**：对区域掩码做欧氏距离变换，其**局部极大值** ≈ 各球的内切圆心，再用**尺度自适应 NMS** 去重，最后按「最近峰归属」（Voronoi）切子区域。

| 判据 | 值 | 含义 |
| :--- | :--- | :--- |
| 候选峰 | `dist > 0.25 × dist.max()` 且为 7×7 邻域内极大（容差 0.5 px） | 抗距离变换的「平台」抖动 |
| 平台归并 | 峰掩码按 8 连通域归并，每域取最强像素 | 一个圆盘顶面会产出一片等距像素 |
| NMS 间距 | `minSep = 1.0 × dist.max()` | 小于此距者视为同一球 |
| NMS 强度 | `minPeak = 0.55 × dist.max()` | 弱峰丢弃 |
| 切分成功判据 | 切出 **≥ 2** 个**合格**子目标（各自过 `minArea`） | 否则整块回退区域级单目标 |

**两个相对量为什么安全（可证，非拟合）**：理想圆盘上 `dist(p) = r − |p − c|`；`dist ≥ 0.55r` ⟺ 点落在圆心 `0.45r` 内 ⇒ 任意两个合格峰间距 ≤ `0.9r` < `minSep = r` ⇒ **单个圆球在几何上不可能产出 2 个峰**。实测 15 帧 / 20 个单球组件全部恰好 1 峰。

⇒ 判据刻意**不设配置键**：两者都相对 `dist.max()`（≈ 球半径）⇒ 换球径 / 换安装高度 / 换相机距离**无需重标定**；仅当目标**非球形**时才需改代码。切分失败时回退，单球行为与改造前逐位一致（当日 8 帧离线实测零过切，三子区域面积和 `6740 + 5499 + 6034` 恰等于组件面积 18273）。

⚠ 合并域各子目标的 `bbox` 是「**可见 footprint**」（重叠部分归更近的那个峰），不是球的完整外接框 ⇒ 叠加图上多框会有少量重叠，**属预期**。

### 4.6 测量与球顶

`MeasureRegion` 对一个区域掩码完成：有效像素计数、质心、均值、标准差、原始最小深度，然后调 `ComputeApex`。

**球顶怎么取**（TR-132）：把区域内有效深度排序，取第 `nth` 小的深度作面片上限，其中

```
nth = min(n − 1, max((n − 1) × apexPercentile, kApexMinPixels − 1))
```

面片内像素的**质心** ⇒ `pixelU/pixelV`，面片**均值** ⇒ `z`。`kApexMinPixels = 8` 是下限：`minArea = 120` 时 2% 只有 2 个像素，质心会被单点噪声主导。

> **为什么不用单像素极小值**：实测 `depth_20261016103952_0` 的球 #1 `min = 516 mm`，但该连通域 0.5% 分位 = 522 mm、`min + 3 mm` 带内只有 **7** 个像素（另一球同带有 818 个）⇒ 直接取 min 会让球顶偏 6 mm 且被 flying pixel 牵引。
> **为什么用比例而非绝对毫米带**：比例自适应连通域大小与深度梯度，换球径 / 换安装高度无需重标定。
> **默认 0.02 的标定依据**（真机 15 帧扫描）：`0.002` ⇒ `z` 帧间抖动 ±3.01 mm、球顶 ±8.4 px；`0.02` ⇒ ±1.00 mm、±3.6 px；`0.05` ⇒ ±0.69 mm、±3.2 px。取小更贴原始最高点但落在掩膜边缘（flying pixel 伪影），取大更稳但球顶沿球面下移。参照：连通域质心 = (1238.2±0.4, 400.0±1.1)，帧间极稳但**系统性偏低约 25 px（≈13 mm）**——这正是「质心 ≠ 球顶」的量化证据。

**三道硬过滤**（任一不过即丢弃该候选）：

| 过滤 | 判据 | 挡什么 |
| :--- | :--- | :--- |
| 有效像素 | `count ≥ max(10, minArea / 4)` | 过小的碎面片 |
| 边缘 | 质心落在 `[edgeMargin, 边长 − edgeMargin]` 内 | 贴边目标（位置不可信） |
| 几何上限 | `max(bbox.width, bbox.height) × apexDepthMm ≤ maxExtentProduct` | **近处大平面**（A4 纸叠、桌面残片） |

> **几何上限为什么对工作距离不敏感**：最长边(px) × 深度(mm) ≈ 物理尺寸 × `fx`——像素随距离缩小、深度随距离增大，乘积守恒。实测真机（`fx ≈ 1037`）：单球 ≤ `105 px × 530 ≈ 5.6 万`；三球合并域 `157 × 550 ≈ 8.6 万`；A4 纸叠（占 ROI 44% 的 567~573 mm 平面）切出的假目标 ≥ `305 × 568 ≈ 17.3 万` ⇒ 默认 **13 万**，两侧余量都够。`≤ 0` = 禁用（与 `targetArea = 0` 同风格）。

### 4.7 打分

```
score      = confidence × 0.60 + nearScore × 0.25 + areaScore × 0.15
confidence = areaScore × 0.45 + stabilityScore × 0.35 + centerScore × 0.20
nearScore  = 1 / (1 + max(0, 区域均值深度 − 锚点深度))
stabilityScore = 1 / (1 + 深度标准差 / max(1, heightBandMm))
centerScore    = 1 − 质心到画面中心距离 / 画面中心到角点距离
```

| 分项 | 语义 | 注意 |
| :--- | :--- | :--- |
| `areaScore` | `targetArea > 0` 时按容差打分；`= 0` 时退化为 `area / (minArea × 8)` | **现行 `targetArea = 0`** ⇒ 面积分退化，无法区分「一个泡芙」与「一堆泡芙」（M3） |
| `stabilityScore` | 区域深度越平越接近 1 | 仿真平盘无噪声 ⇒ 恒满分，**仿真验不出该项判别力** |
| `centerScore` | 越靠画面中心越高 | **与「最高」无关**——TR-131 之前按 `score` 排序，正因此把较低的球排到 `front()` |
| `nearScore` | 区域均值离锚点越近越高 | 基准是**本次尝试的锚点深度**，不是全帧最小值 |

⚠ 写进 `PuffResult.confidence` 的是 **`score` 的 clamp 值**，不是 `EstimateConfidence()` 的单独输出——后者只是 `score` 的一个输入项（移植时按老实现源码保真，`depth_puff_algorithm_plan.md` §三 已更正）。

### 4.8 排序与阈值

`stable_sort`：`apexDepthMm` 升序（小 = 高 = 优先），并列时 `score` 降序。随后按 `confidenceThreshold` 剔除，全被剔除则**换下一个锚点**而不是直接报未检出（TR-151 的配套语义）。

`LocateBest` 返回 `Detect().front()`，与主路径**同源**；当前全仓无调用者，保留为接口占位——语义仍须与 `Detect` 对齐，否则将来一旦接线就会引入第二套「最佳」定义。

## 五、参数

**默认值**列 = `AlgorithmParams` 结构体内置（`LoadConfig` 解析不到时也用它）；**现行值**列 = `config/config.json` 的 `vision.algorithms.DepthPuffAlgo`（2026-10-23 核对）。

⚠ **生效路径**：`vision.algorithms.*` 这些私有键**没有 UI 入口**，只能直接编辑 `config.json` ⇒ **改完必须重启**（`SnapshotParams` 读的是 `ConfigManager` 内存里的 `root_`）。配置页的「置信度阈值 / 深度范围」三个输入框写的是**通用域** `vision.*`，而本算法节点里已有同名键 ⇒ **节点优先，改配置页对深度法无效**；要调阈值只能改节点。

| 键 | 默认 | 现行 | 作用步骤 | 调整后果 |
| :--- | :--- | :--- | :--- | :--- |
| `confidenceThreshold` | 0.85 | **0.65**（节点级） | 8 | 松紧直接决定检出/漏检。0.65 的标定依据：真球 `score ∈ [0.697, 0.803]`、垃圾候选 ≤ 0.611 ⇒ 分界中点 ≈ 0.654 |
| `depthZMin` | 0.0 | **430** | 1 | 近端截断边界；调高会切出更多薄壳（§4.2 守卫兜底），调低会把近景干扰物纳入 |
| `depthZMax` | 500.0 | **700** | 1 | 远端物理边界；调低滤掉远处背景，调高把承载面拉进有效掩码 |
| `nearestPercentile` | 0.001 | 0.001 | 2 | 只定**扫描起点**；代码内钳到 `[0.001, 0.005]`，再大就会锚到背景面（TR-144） |
| `heightBandMm` | 35.0 | **20.0** | 4 | 顶层带厚。过薄会把球切成几块，过厚会把承载面并进来；代码内取 `max(1.0, 值)` |
| `apexPercentile` | 0.02 | 0.02 | 6 | **输出语义旋钮**：不改检出与否，只改「中心落在球的哪个位置」；钳到 `[0.005, 0.5]` |
| `minArea` | 120 | 120 | 2 / 4 / 5 / 6 | 连通域面积下限（深度系）；也是有效像素下限的除数（`/4`） |
| `maxArea` | 250000 | 250000 | 4 | 面积上限，排除「整片背景」这类非单目标 |
| `targetArea` | 0 | 0 | 6 | 目标面积；**0 = 禁用面积评分**（M3 未解） |
| `targetAreaTolerance` | 0 | 0 | 6 | 容差；0 时自动取 `max(1, targetArea / 2)` |
| `morphKernelSize` | 7 | 7 | 4 | 开 + 闭核；偶数自动 +1，≤ 1 跳过。**闭运算越大越容易桥接多目标** |
| `edgeMargin` | 8 | 8 | 2 / 3 / 6 | 边缘过滤距离（px）；锚点 / 种子按 ROI 判，质心按整图判 |
| `maxExtentProduct` | 130000 | 130000 | 6 | 几何上限 `px × mm`；**≤ 0 = 禁用**。换安装高度 / 球径须重核算 |
| `roi` | 全 0 = 全图 | `900, 70, 800×610` | 0 | 深度系 ROI；`w` 或 `h` ≤ 0 视为全图，与图像求交后使用 |

**通用三键的覆盖顺序**（`confidenceThreshold` / `depthZMin` / `depthZMax`）：**算法节点优先 → 通用域 `vision.*` → 结构体默认**。顺序写反会**静默失效**——通用域三键恒存在于 `config.json`，若先读通用域，节点里的同名覆盖键永远读不到（TR-127）。

⚠ **整数键必须写成整数**：`SnapshotParams` 的整数量走 `json::get<int>()`，`120.0` 会抛异常并被吞掉 ⇒ 静默回落到默认值。受影响的键：`minArea` / `maxArea` / `targetArea` / `targetAreaTolerance` / `morphKernelSize` / `edgeMargin` / `roi.*`。

### 5.1 换场景的调参顺序

按此顺序改，**每次只改一项**并重启，看 `[DepthPuffAlgo] attempt#…` 行的对应字段（§7.2）：

| 顺序 | 键 | 判据 | 改错的表现 |
| :- | :--- | :--- | :--- |
| 1 | `depthZMin` / `depthZMax` | 区间要同时覆盖球顶与承载面（真机实测球顶约 530 mm、桌面约 567 mm） | `no valid depth pixels in ROI`；或背景把目标淹成巨域 |
| 2 | `roi` | 只框托盘，**不要框到机械臂末端 / 线缆** | `no seeds in ROI`；或 `attempt#` 反复换锚点才出结果 |
| 3 | `heightBandMm` | 从 20 起，看 `area` 是否接近单球面积 | 过薄 ⇒ 一个球被切成几块（`candidate(s)` 多但 `area` 小）；过厚 ⇒ `area` 暴涨（承载面并入） |
| 4 | `minArea` | 取第 3 步量到的单球面积的**下限**（120 是保守值） | 过大会漏掉远处小球，过小会放进噪点面片 |
| 5 | `confidenceThreshold` | 两批帧分别取「真球 `score` 下限」与「垃圾候选 `score` 上限」的中点（TR-131 的做法） | 过严 ⇒ `all candidates below confidenceThreshold`；过松 ⇒ 假目标进结果 |
| 6 | `apexPercentile` | 最后调；只影响输出点落在球的哪个位置，不影响检出 | 取太小 ⇒ `z` 帧间抖动变大（flying pixel） |

## 六、硬编码常量

以下量**不可配**，改了要改代码（`DepthPuffAlgo.cpp` 匿名命名空间顶部集中定义）。判据标注「可证」者不依赖实测标定，换场景无需重算。

| 分组 | 常量 | 值 | 含义 |
| :--- | :--- | :--- | :--- |
| 锚点 | `kAnchorStepMm` | 2.0 mm | 细步长 |
| 锚点 | `kAnchorFineIters` | 8 | 前 8 次用细步长 |
| 锚点 | `kAnchorCoarseStepMm` | 10.0 mm | 之后的粗步长 |
| 锚点 | `kAnchorMaxIters` | 64 | 迭代上限（总行程 576 mm） |
| 锚点 | `kAnchorMaxAttempts` | 8 | 单帧最多尝试的锚点数 |
| 锚点 | `kAnchorNearClipTolMm` | 1.0 mm | 近端截断守卫容差 |
| 锚点 | `kAnchorDedupMm` | 2.0 mm | 跨层锚点去重容差 |
| 锚点 | `kNearestPercentileMin` / `Max` | 0.001 / 0.005 | `nearestPercentile` 钳位 |
| 种子 | `kSeedBandMm` | 10.0 mm | 种子带厚（同桌面泡芙顶点差 ≤ 9 mm、背景低 ≥ 19 mm） |
| 种子 | `kSeedMinPixels` | 20 px | 种子最小面积（滤单像素噪点） |
| 种子 | `kSeedMorphKernel` | 3 px | 种子开运算核（只开不闭） |
| 生长 | `kMinHeightBandMm` | 1.0 mm | `heightBandMm` 下限 |
| 切分 | `kSplitSepRatio` | 1.0 | NMS 间距 = 此值 × `dist.max()`（可证，尺度无关） |
| 切分 | `kSplitPeakRatio` | 0.55 | NMS 强度 = 此值 × `dist.max()`（可证，尺度无关） |
| 切分 | `kSplitLocalKernel` | 7 px | 局部极大比较核 |
| 切分 | `kSplitLocalEps` | 0.5 px | 局部极大容差（抗平台抖动） |
| 切分 | `kSplitPlateauFloorRatio` | 0.25 | 候选峰预筛强度 |
| 切分 | `kSplitMinMarkers` | 2 | 触发切分 / 判定成功的最低峰数 |
| 球顶 | `kApexPercentileMin` / `Max` | 0.005 / 0.5 | `apexPercentile` 钳位 |
| 球顶 | `kApexMinPixels` | 8 px | 球顶面片最少像素 |
| 测量 | `kMinValidPixelsFloor` / `Div` | 10 / 4 | 有效像素下限 = `max(10, minArea/4)` |
| 打分 | `kScoreConfWeight` / `Near` / `Area` | 0.60 / 0.25 / 0.15 | `score` 三权重 |
| 打分 | `kConfAreaWeight` / `Stability` / `Center` | 0.45 / 0.35 / 0.20 | `confidence` 三权重 |
| 打分 | `kAreaScoreScale` | 8 | 面积评分退化分支的除数 |

## 七、失败模式与日志

### 7.1 `GetLastError()` 的六个值

| 错误串 | 触发点 | 常见原因 | 处置 |
| :--- | :--- | :--- | :--- |
| `invalid depth dimensions` | 0 步 | 深度宽高 ≤ 0 且彩色也 ≤ 0 | 检查相机帧 |
| `depth data smaller than depthWidth x depthHeight` | 0 步 | D2C 尺寸与缓冲不匹配 | 查 `depthWidth/depthHeight`（TR-116） |
| `no valid depth pixels in ROI` | 1 步 | **ROI 落在图像外**、`depthZMin/Max` 与场景深度不相交 | **换环境第一查项**：仿真 640×480 与真机 1920×1080 的 ROI 不可通用 |
| `no seeds in ROI` | 2 步 | 扫描 64 层无合格面片、兜底分位也长不出种子 | 收紧 `roi`、核对 `depthZMin`、查 ROI 内是否有大面积近景遮挡 |
| `no candidate passed filtering` | 4 / 6 步 | 全部被面积 / 边缘 / 几何上限过滤 | 看 `maxExtentProduct` 是否过紧、`minArea` 是否过大 |
| `all candidates below confidenceThreshold` | 8 步 | 候选全部低于阈值 | 阈值或光照/深度质量；换锚点已自动尝试 |

### 7.2 每次尝试的信息行

```
[DepthPuffAlgo] attempt#2/8 3 seed(s) → 2 candidate(s); best area=6458 meanDepth=547.2mm
  topDepth=516.0mm apexDepth=522.6mm apex=(1241,375)[168px] score=0.741;
  nearestDepth=530.4mm anchor=component zRange=[430.0,700.0]
```

（上面是**格式示例**，字段含义见下表；数值随帧变化，勿当标定基准。）

| 字段 | 读法 |
| :--- | :--- |
| `attempt#n/8` | 第 n 个锚点（达到 8 仍未出目标会额外 WARN） |
| `seed(s)` / `candidate(s)` | 本次锚点长出的种子数 / 通过的候选数 |
| `area` | `front()` 的面积（深度系 px²） |
| `meanDepth` | 区域均值——调参时对照穹顶高度用 |
| `topDepth` | **原始最小深度**（保留旧名，兼容既有 grep 配方） |
| `apexDepth` | **球顶面片均值 = 真正写进 `z` 的值**；与 `topDepth` 之差 = 该连通域的噪点幅度 |
| `apex` / `[Npx]` | 球顶像素（深度系）与面片像素数；`[0px]` 表示退化兜底 |
| `score` | 写进 `confidence` 的值 |
| `anchor=` | 锚点来源：`component` = 逐层扫描，`percentile` = 全局分位兜底 |
| `zRange` | 本次 `depthZMin` / `depthZMax` |

另有三条 `SPDLOG_INFO`：切分成功（`seed #N area=… → M sub-target(s)`）、阈值过滤（`filtered X of Y detection(s) → 换下一个锚点`）、锚点达上限（`锚点尝试达上限 8 仍未出目标`）。

## 八、仿真与真机的差异

| 项 | 仿真 | 真机 |
| :--- | :--- | :--- |
| 深度场景 | `SimCamera` 背景 `600 mm`、目标 `55` / `105 mm`、半径 30 / 20 px、`640×480`、**平盘无噪声** | 真实穹顶 + flying pixel 伪影 + 深度噪声 |
| `stabilityScore` | 恒满分 | 有判别力 |
| 检出个数 | 取决于 `zRange` 与 `heightBandMm`（TR-126 推演：只有最高层进带 ⇒ 1 个） | 1 / 2 / 3 个均实测出现 |
| 像素对应 | D2C 恒等 | D2C = `SW` 时恒等，关闭时按比例映射 |

⚠ **现行配置不能直接用于仿真**（2026-10-23 核对）：`roi = (900, 70, 800×610)` 在 `640×480` 的仿真图上与图像求交为**空矩形** ⇒ `no valid depth pixels in ROI`；且 `depthZMin = 430` 会把仿真的 `55` / `105 mm` 目标全部滤掉、只留 `600 mm` 背景。切回仿真前必须同步改 `roi` 与 `depthZMin/depthZMax`（TR-126 当初的推演用的是 `depthZMax = 496`、`nearestPercentile = 0.03`，**与现行值不同，勿照搬**）。

## 九、已知限制

| # | 限制 | 影响 | 现状 |
| :--- | :--- | :--- | :--- |
| 1 | `targetArea = 0` ⇒ 面积评分退化 | 无法区分「一个泡芙」与「一堆泡芙」 | M3 未解；真机调参阶段（阶段 3）定值 |
| 2 | 锚点预算固定 576 mm | 安装高度变化超出预算时够不到目标 | TR-149 分级步长已放宽；仍须按实际安装高度核对 |
| 3 | 峰切分判据对**非球形**目标不成立 | 切分可能过切或切不开 | 需改代码（`kSplit*`），不是配置项 |
| 4 | `depthZMin` 切出的近端薄壳 | 可能劫持锚点 | TR-151 近端守卫 + 多锚点重试已缓解 |
| 5 | 合并域子目标的 `bbox` 是可见 footprint | 叠加图多框有少量重叠 | 属预期 |
| 6 | 球顶 ≠ 包围框中心（约 47 px） | 画框必须用 `bboxX/bboxY` | TR-132 已把原点独立给出 |
| 7 | 无外观信息 | 与球等距的其它凸起会被当目标 | 几何上限只挡「大平面」，挡不住「近处小球」 |
| 8 | 阶段 3 真机调参未做 | `targetArea` / `roi` / `depthZMin` / `depthZMax` 仍是首轮值 | 见 `depth_puff_algorithm_plan.md` §五 阶段 3 |

## 十、代码地图

| 位置 | 内容 |
| :--- | :--- |
| `src/HAL/interfaces/IPuffAlgorithm.h` | `PuffResult` / `RoiRect` / `AlgorithmParams` / `IPuffAlgorithm`（纯接口，不引入 OpenCV） |
| `src/HAL/algorithm/DepthPuffAlgo.h` | 类声明 + 边界说明（不碰内参 / 不读配置 / 不做坐标转换） |
| `src/HAL/algorithm/DepthPuffAlgo.cpp` 匿名命名空间 | 常量块 → `Seed` / `Candidate` → 打分 → `BuildValidMask` → `AnchorSweep` 三件套 → `BuildSeedsForAnchor` / `BuildSeedsFromPercentile` → `GrowSeedRegion` → `FindMarkers` → `VoronoiSplit` → `ComputeApex` → `MeasureRegion` → `SelectRegions` |
| `DepthPuffAlgo::SnapshotParams` | 主线程读 `ConfigManager`，节点优先 |
| `DepthPuffAlgo::Detect` | 尺寸校验 → 掩码 → 锚点循环 → 归一 → 阈值过滤 |
| `DepthPuffAlgo::LoadConfig` | 解析整棵子树作**兜底**（节点缺失传 `{}`，不报错） |
| 消费侧 | `SequenceWorker` 识别分支：`SnapshotParams` + `Detect` 经 `InMainThread`，取 `front()` → `PixelToRobot(pixelU, pixelV, z)`（TR-124）；`VisionTestPage` 单次检测走同一入口 |

## 十一、台账索引

| 条目 | 一句话 | 影响本文 |
| :--- | :--- | :--- |
| TR-126 | 深度法本体移植 + 算法配置命名空间 | 全文基线 |
| TR-127 | 算法配置覆盖顺序（节点优先） | §五 覆盖顺序 |
| TR-131 | 「取最高目标」语义 + 节点级阈值 0.65 | §4.8、§五 |
| TR-132 | 输出语义定死为球顶（`apexPercentile`） | §4.6、§二 不变量 1 |
| TR-135 | 多目标合并解切分（峰 + NMS + Voronoi） | §4.5 |
| TR-144 | 顶层锚点改「逐目标种子」 | §4.3 |
| TR-145 | 锚点改连通域级（近→远逐层抬高） | §4.2 |
| TR-147 | 目标几何上限 | §4.6 |
| TR-148 | 几何上限配置化（`maxExtentProduct`） | §4.6、§五 |
| TR-149 | 锚点起点改分位 + 分级步长 | §4.2 |
| TR-151 | 锚点改「逐个尝试」+ 近端截断守卫 | §4.2、§4.8 |
