# 深度法泡芙定位算法移植方案

> **需求来源**：`implementation_roadmap.md` §9.6 要求「④ 的技术方案尚未成文……移植步骤、接口扩展、参数表须另开一篇」；用户 2026-10-13 指令「把移植步骤与参数初值表那篇补上」。
> **结论**：**移植优先于重写**——老项目 `D:/workspace/projects/robot-puff/Framework/VisionModule/` 的 `CPuffLocator` 是纯深度法实现，核心 6 个函数**零 MFC 依赖**（只用 OpenCV + std）。本文给出**符号映射、参数初值、分阶段步骤与验收判据**，落地形态为 `src/HAL/algorithm/DepthPuffAlgo.{h,cpp}`。
> **状态**：🟡 **阶段 0 + 阶段 1 已实施（2026-10-14，TR-126）**——`DepthPuffAlgo` 已落地并注册，C1 / C3 / C4 / C5 同批完成，Debug + Release 全清重建均链接成功、零新增警告。**阶段 2（Sim 冒烟）与阶段 3（真机调参）未做**——`simulation.cameraType = OrbbecCamera` 且真实相机在线，跑冒烟会**抢真相机**（`AGENTS.md` 冒烟纪律），待确认时机。⚠ **C6 / M1 已移出本批**——`vision.templatePath` 并非「无读点」，见 §七 M1。本文只定**怎么移植**；**接口契约**见 `algorithm_config_plan.md` §十一，**参数归属**见同篇 §四 / §五。
> **关联**：`algorithm_config_plan.md`（§十一 接口契约、§十 已裁定项、§六 C1/C3/C4/C5）、`implementation_roadmap.md` §9.2 / §9.4 / §9.5（工期、来源、既有约束）、`hand_eye_calibration_plan.md`（真机标定；本算法**不做**坐标转换）、`doc/config.md`（键的 schema 落点）。

## 一、目标与范围

**目标**：在本仓新增一个**深度图定位算法实现** `DepthPuffAlgo`，定位行为与老项目 `CPuffLocator` 一致，同时遵守 `algorithm_config_plan.md` §十一 的接口契约。

| 做 | 不做 |
| :--- | :--- |
| 移植深度法定位本体：深度预处理 → 最高层 `mask` → 连通域分析 → 综合打分 | ❌ **不做**相机坐标 → 基座坐标转换（老实现也没有；本仓由 `CoordTransform::PixelToRobot` 负责，TR-124 已接通） |
| 接入 `IPuffAlgorithm` + `REGISTER_PUFF_ALGORITHM` 注册 | ❌ **不做**真机标定与调参（属 `hand_eye_calibration_plan.md` 与本文 §七） |
| 按本仓 L2 约定，参数改为「主线程快照 → 值语义传入」 | ❌ **不引入**颜色 / 形状 / 深度学习辅助（老实现亦未用，RGB 入口仅预留） |

## 二、移植来源与可移植性核实

| 文件 | 规模 | 内容 |
| :--- | :--- | :--- |
| `include/PuffLocator.h` | 86 行 | `CPuffLocator` + `PuffLocateResult`（`found` / `center` / `boundingBox` / `depthMm` / `minDepthMm` / `area` / `confidence`） |
| `src/PuffLocator.cpp` | 407 行 | 纯深度流水线：`BuildDepthMmAndMask` → `BuildTopRegionMask` → `SelectBestRegion` → `CalculateAreaScore` / `EstimateConfidence` |
| `examples/puff_locator_params.json` | 16 键 | 参数已全部外置（10 项 + ROI + 2 项 demo 采集参数） |
| `交接文档.md` | 2026-06-17 | 调参清单（§十）、已知限制（§九）、参数含义（§四）、构建运行（§五 / §六） |

**逐条核实结论**（均已读源码）：

| # | 结论 | 依据 |
| :--- | :--- | :--- |
| 1 | 核心 6 个函数**零 MFC**，只用 OpenCV + std | `PuffLocator.cpp:138-385`——无 `CString` / `_T()` / `afx` 调用 |
| 2 | MFC 外壳只在**类骨架与错误串**上 | `VisionBase.h` include `<afxwin.h>`；`BOOL` / `CString` / `_T()` / `m_strLastError` / `m_strResult` |
| 3 | **本就是纯深度算法** | `Locate` 的 `color` 形参**匿名且完全未使用**（`PuffLocator.cpp:85`） |
| 4 | **相机是同一台** | 交接文档 §八 记 `Orbbec Gemini 336 [SN: CP9KB530005S]`，与本仓 `communication.camera.sn` 逐字一致 ⇒ 深度噪声 / 量程特性可直接参考 |
| 5 | 老实现**不做坐标转换** | 交接文档 §九.3 自述「没有做相机坐标到机器人坐标的转换」；输出仅 `center`（像素）+ `depthMm` |
| 6 | **从未在真实泡芙上标定** | 交接文档 §九.4：`targetArea` 当前为 0；§八 的验证场景是桌面杂物与手持 |

⇒ 结论 5 与 6 决定了本方案的两个边界：**移植段可立即开工**（不依赖相机），但**真机调参不可省**（§七）。

## 三、符号映射表

### 3.1 类与函数

| 老项目符号 | 本仓对应 | 处理 |
| :--- | :--- | :--- |
| `CVisionBase`（基类） | `IPuffAlgorithm` | 换基类（`afxwin.h` 整条依赖链随之下线） |
| `CPuffLocator` | `DepthPuffAlgo` | 改名；**类名 = `simulation.algorithmType` 取值**，也是 `vision.algorithms.<名>` 的节点名（`algorithm_config_plan.md` §五 设计要点 1） |
| `BOOL` / `FALSE` / `TRUE` | `bool` / `false` / `true` | MFC → 标准 |
| `CString` / `_T()` | `std::string` | MFC → 标准；错误串改走 `GetLastError()` |
| `m_strLastError` | `lastError_`（`mutex_` 保护） | 同 `SimAlgo.h:33-34` 的既有写法 |
| `Locate(color, depth, result)` | `Detect(frame, params)` | 签名归一；返回值由 `BOOL` 改为 `std::vector<PuffResult>` |
| `SetRoi` / `SetValidDepthRange` / `SetTopRegionParams` / `SetAreaRange` / `SetTargetArea` / `SetMorphKernelSize` / `SetEdgeMargin` | `AlgorithmParams` 字段 | **不保留 setter**——参数改由调用方快照后传入（本仓 L2 约定，`IPuffAlgorithm.h:21-27`） |
| `CaptureImage` / `ProcessImage` / `ProcessFrame` / `UpdateResultText` | —— | **不移植**：MFC 骨架 + demo 可视化 |
| `GetResult()`（人读文本） | —— | **不移植**（日志由本仓 `SPDLOG_*` 承担） |
| `m_lastColor` / `m_lastDepth` / `m_lastImage` / `m_lastCandidateMask` | —— | **不移植**：帧缓存 / 调试图由 `SequenceWorker` 与视觉页持有 |
| `RgbdFrame{color, depth, depthPreview}` | `CameraFrame` | `depthPreview` 丢弃（demo 伪彩可视化专用） |
| `CVisionFactory` + `CreatePuffLocator()` | `REGISTER_PUFF_ALGORITHM("DepthPuffAlgo", DepthPuffAlgo)` | 本仓走静态注册宏（`HALFactory.h:98-109`） |

### 3.2 结果字段

| 老 `PuffLocateResult` | 本仓 `PuffResult` | 说明 |
| :--- | :--- | :--- |
| `found` | —— | 本仓用**空 `vector`** 表达「未检出」（契约 §11.3 约定 2），不设标志位 |
| `center.x` / `center.y` | `pixelU` / `pixelV` | ⚠ **须做坐标系归一**（深度图 → 彩色图），见 §五 1.6 |
| `boundingBox.width` / `height` | `width` / `height` | 同样须归一 |
| `depthMm`（区域**均值**） | —— | **丢弃**——契约 §11.3 定 `z` 为「区域**最高点**」 |
| `minDepthMm`（区域最小深度） | `z` **与** `minDepthMm` | `z = minDepthMm`（最高点）；两字段同源，供交叉校验 |
| `area` | `area` | 单位 = **深度图**像素²（与 `pixelU/pixelV` 的彩色系不同，见 §七 M2） |
| `confidence` | `confidence` | ⚠ **以源码为准（2026-10-14 更正）**：老实现存入结果的是**三权重综合 `score` 的 clamp 值**（`confidence*0.60 + near*0.25 + area*0.15`，`PuffLocator.cpp:328-340`），**不是** `EstimateConfidence()` 的单独输出——后者只是 `score` 的一个输入项。本表原写「语义同：加权综合分（`EstimateConfidence` 的输出）」有误，移植按源码保真 |

## 四、参数初值表

**归属**列指向 `algorithm_config_plan.md` §四 / §五 的裁定：通用域 = `vision.*`，私有域 = `vision.algorithms.DepthPuffAlgo.*`。

**初值取「老项目 `examples/puff_locator_params.json`」**——那是经手持实测验证过的一组；⚠ `PuffLocator.h:75-85` 的成员默认值与 JSON **不一致**（`heightBandMm` 25 vs 35、`minArea` 80 vs 120、`maxArea` 200000 vs 250000、`morphKernelSize` 5 vs 7、`edgeMargin` 5 vs 8），**以 JSON 为准**，头文件默认值视为过期。

| 配置键 | 归属 | 类型 | 初值 | 单位 | 说明与调参方向 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `vision.depthZMin` | 通用域 | double | `6.0` | mm | 有效深度下限。老项目为 `minDepthMm = 100`；⚠ 本仓现值 `6.0` 是按**仿真背景 600 mm** 定的，**真机装高后必须重定**（`implementation_roadmap.md` §9.5 项 3） |
| `vision.depthZMax` | 通用域 | double | `496.0` | mm | 有效深度上限。老项目为 `maxDepthMm = 3000`；同上，真机重定 |
| `vision.confidenceThreshold` | 通用域 | double | `0.85` | 0~1 | 置信度过滤阈值。⚠ **老实现从不过滤**（只输出 `confidence`）；本仓契约要求消费该键（§七 M1 说明风险） |
| `…nearestPercentile` | 私有域 | double | `0.03` | — | 最近层分位数；代码内 `clamp(值, 0.001, 0.5)`。**调参重点** |
| `…heightBandMm` | 私有域 | double | `35.0` | mm | 最高层带厚；代码内实际用 `max(1.0, 值)`。**调参重点** |
| `…minArea` | 私有域 | int | `120` | px² | 连通域面积下限（滤噪声） |
| `…maxArea` | 私有域 | int | `250000` | px² | 连通域面积上限（排除非单目标） |
| `…targetArea` | 私有域 | int | `0` | px² | 目标泡芙面积；**0 = 禁用面积评分**。真机标定后设（§七） |
| `…targetAreaTolerance` | 私有域 | int | `0` | px² | 目标面积容差；**0 = 自动取 `max(1, targetArea/2)`** |
| `…morphKernelSize` | 私有域 | int | `7` | px | 形态学核（开 + 闭）；**偶数自动 +1**，`≤1` 时跳过形态学 |
| `…edgeMargin` | 私有域 | int | `8` | px | 边缘过滤距离：中心点落在 `[margin, 边长−margin]` 之外则丢弃 |
| `…roi` | 私有域 | object | `{x:0, y:0, w:0, h:0}` | px | 感兴趣区域；`w` 或 `h` ≤ 0 视为**全图**。⚠ 坐标系 = **深度图** |
| `…templatePath` | 私有域 | string | `""` | — | 深度法**不使用**模板；随 M1 迁键保留空串 |

**不移植的键**（属 demo 而非算法）：`framesToCapture`（60）、`frameDelayMs`（50）——老 demo 的采集帧数与帧间隔，本仓取帧由 `ICamera::TakeLatestFrame()` 负责（TR-123）。

**代码内硬编码、不可配的量**（如将来需调，须改代码）：

| 量 | 值 | 位置 |
| :--- | :--- | :--- |
| 最高层阈值 | `nearestDepthMm + max(1.0, heightBandMm)` | `BuildTopRegionMask` |
| 连通域最少有效像素 | `max(10, minArea / 4)` | `SelectBestRegion` |
| `score` 权重 | `confidence × 0.60 + nearScore × 0.25 + areaScore × 0.15` | `SelectBestRegion` |
| `confidence` 权重 | `areaScore × 0.45 + stabilityScore × 0.35 + centerScore × 0.20` | `EstimateConfidence` |
| `nearScore` | `1.0 / (1.0 + max(0.0, meanDepth − nearestDepthMm))` | `SelectBestRegion` |

## 五、移植步骤

### 阶段 0 — 前置（不依赖相机，可立即开工） ✅ **已实施（2026-10-14，TR-126）**

| # | 步骤 | 落点 | 说明 |
| :--- | :--- | :--- | :--- |
| 0.1 | **给 HAL 加 OpenCV 依赖** | `src/HAL/CMakeLists.txt` | ⚠ **HAL 当前不链接 OpenCV**（只链 `Qt6::Core` / `Qt6::Gui` / `Config` / `spdlog`；OpenCV 只在 `Core` 与主程序）。新算法要用 `cv::connectedComponentsWithStats` / `morphologyEx` ⇒ 须加 `target_link_libraries(HAL PRIVATE ${OpenCV_LIBS})`。运行时 DLL 已在 exe 目录（`CMakeLists.txt:204-220` 拷贝）且 `build_release.bat` 的 `copy *.dll` 会带走 ⇒ **部署无新增动作** |
| 0.2 | **扩展接口**（C3） | `src/HAL/interfaces/IPuffAlgorithm.h` | `AlgorithmParams` 加 8 项私有量 + `roi`（§四）；`PuffResult` 加 `area` / `minDepthMm`。⚠ **头文件** ⇒ 改完必须 `-t clean` 全清重建（ninja 依赖追踪在 MSVC 中文版下失效） |
| 0.3 | **调用方透传整棵子树**（C1） | `HardwareManager.cpp:161-163` | 由手工拼 `{"confidenceThreshold": …}` 改为 `cfg.getValue<nlohmann::json>("vision.algorithms." + algoType, nlohmann::json::object()).dump()`。节点缺失 ⇒ 得 `{}`，**不报错**（§十 裁定 1） |
| 0.4 | **`SimAlgo` 补齐解析**（C4 / C5） | `SimAlgo.cpp:190-211`、`:21-30` | 认整棵子树；私有键优先、通用域作默认（§十 裁定 3）。此步后 `SimAlgo` 行为**逐字节不变** |
| 0.5 | **配置落盘** | `config/config.json` | 加 `vision.algorithms.DepthPuffAlgo` 节点（§四 初值）+ `vision.algorithms.SimAlgo: {}`。⚠ 同步 `doc/config.md`（schema 权威落点） |

### 阶段 1 — 算法本体移植 ✅ **已实施（2026-10-14，TR-126）**

| # | 步骤 | 说明 |
| :--- | :--- | :--- |
| 1.1 | 新建 `src/HAL/algorithm/DepthPuffAlgo.{h,cpp}`，注册 `REGISTER_PUFF_ALGORITHM("DepthPuffAlgo", DepthPuffAlgo)` | 目录 / 命名与 `SimAlgo` 对齐 |
| 1.2 | 按 §三 映射表逐个替换符号 | MFC 外壳全下线 |
| 1.3 | **去掉 `m_dDepthScaleToMm`** | 本仓 `CameraFrame::depthData` **已是 `float` mm**（`ICamera.h:40`；`OrbbecCamera.cpp:725-732` 用 `getValueScale()` 转好），无需再缩放 |
| 1.4 | **深度图零拷贝包装** | `cv::Mat(dh, dw, CV_32FC1, const_cast<float*>(frame.depthData.data()))`——只读不改；⚠ 包装前**必须**按 `depthWidth/depthHeight`（含 0 回落）校验 `depthData.size()`（TR-116） |
| 1.5 | 删除 `depth.channels()` / `type()` 类型分支 | 输入已固定为单通道 `float` mm，`CV_16UC1` 分支不再可达 |
| 1.6 | ⚠ **坐标系归一：深度图 → 彩色图** | 算法在**深度域**做连通域 ⇒ `center` / `boundingBox` 都是**深度像素**。契约 §11.3 要求 `pixelU/pixelV` 在**彩色系**（因为 `PixelToRobot` 用的是 `OrbbecCamera` 的**彩色**内参）。故输出前按比例映射：`u_color = u_depth × width / depthWidth`、`v_color = v_depth × height / depthHeight`。**D2C = SW 时两分辨率相同，映射退化为恒等**（本仓 `config.json` 现为 `SW`）；D2C 关闭时必须生效，否则像素喂错内参 |
| 1.7 | **`SelectBestRegion` 改为遍历全部连通域** | 老实现只返回 `argmax` 一个（`PuffLocator.cpp:330-341`），而 `IPuffAlgorithm::Detect` 返回 `vector<PuffResult>`（`implementation_roadmap.md` §9.5 项 5）。改为**全部通过硬过滤的连通域**都产出结果 |
| 1.8 | ⚠ **按 `score` 降序排序** | 契约 §11.3 约定 1：`results.front()` = 最佳目标（`SequenceWorker.cpp:1034` 直接取 `front()`）。老实现靠「边扫边比」天然得到最优，改成 `vector` 后**必须显式排序**，否则静默抓错目标 |
| 1.9 | **`z` 取最高点** | `PuffResult.z = minDepthMm`（不是 `depthMm` 均值）——契约 §11.3。均值仍可留给日志 |
| 1.10 | `LoadConfig` 认整棵子树（C5） | 从 `SimAlgo` 的写法复制结构：解析出的值作**兜底默认值** |
| 1.11 | `SnapshotParams` 补齐（C4） | 私有键读 `vision.algorithms.DepthPuffAlgo.*`；通用域读 `vision.depthZMin` / `depthZMax` / `confidenceThreshold`。⚠ **主线程专用**（读 `ConfigManager`） |
| 1.12 | `Detect` 串行化 | 视觉页检测线程与 `SequenceWorker` 识别动作可能共用同一实例 ⇒ `lastError_` 用 `mutex_` 保护（照 `SimAlgo.cpp:36`） |
| 1.13 | 注册进 CMake | `src/HAL/CMakeLists.txt` 的 `HAL_SOURCES` / `HAL_HEADERS` |

### 阶段 2 — 仿真同构与通路验证 🟢 **基本完成**（2026-10-14，TR-126 / TR-127）

| # | 步骤 | 说明 |
| :--- | :--- | :--- |
| 2.1 | `simulation.algorithmType` → `DepthPuffAlgo` | 切换实现，零代码改动（契约 §11.5 的收益） |
| 2.2 | 验证「通路通」而非「算法准」 | `SimCamera` 深度是**平盘 + 无噪声**（`SimCamera.cpp:120`、`:140-161`），只验数据流 |
| 2.3 | 记录**预期行为变化** | 仿真下深度法**只检出 1 个**目标（颜色法为 2 个）——见 §六，**属预期、非缺陷**，验收判据须写明 |

> **进度（2026-10-14）**：2.1 已做（`algorithmType` 切 `DepthPuffAlgo` 启动正常）；2.2 的**通路**已验（`[DepthPuffAlgo] config loaded`、无 `not registered` 警告、无 `[SimAlgo]` 前缀，TR-127）；**2.3 的「仿真检出 1 个目标」与 A4 已由离线自测坐实**（TR-126，合成帧 `heightBandMm=35` ⇒ 1 个、`35→60` ⇒ 2 个）；**真机亦实测出结果**（视觉页「单次检测」发现 1 / 2 / 3 个目标）。⚠ 仍缺两项：`SequenceWorker`「识别」动作路径（B 档 5，本轮真机走的是视觉页单次检测）与真机调参（阶段 3）。

### 阶段 3 — 真机调参（待相机装好）

| # | 步骤 | 说明 |
| :--- | :--- | :--- |
| 3.1 | 在**最终安装高度**采一组真泡芙 RGB + 深度图 | `implementation_roadmap.md` §9.6「相机装好后第一件事」 |
| 3.2 | 按 `_mask.png` 量出单个泡芙面积 | 定 `targetArea` / `targetAreaTolerance` |
| 3.3 | 定 `roi`（托盘区域）与 `depthZMin` / `depthZMax` | 老项目用 `[100, 3000]`；本仓须按实际安装高度重定 |
| 3.4 | 复核 `confidenceThreshold` | ⚠ 见 §七 M1 |

### 阶段 4 — 文档与台账

落地后按 `algorithm_config_plan.md` §九 的同步清单走：`TEST_RECORD.md` → 本文状态 → `doc/config.md` → `doc/README.md` → `AGENTS.md` 的 Config / Current Phase 节。

**编译注意**：阶段 0.2 改的是头文件（`IPuffAlgorithm.h`），阶段 0.1 改的是 CMake ⇒ 两者都**必须 `-t clean` 全清重建**，否则会出现「编译期静默、运行期堆越界」或旧签名 `LNK2019`。

## 六、仿真同构与预期行为变化

`SimCamera` 的仿真场景（`SimVision.h:22-28`、`SimCamera.cpp:140-152`）：

| 元素 | 值 |
| :--- | :--- |
| 背景深度 | `600 mm` |
| 目标 1 | 半径 `30 px`、深度 `55 mm`、面积 ≈ `2827 px²` |
| 目标 2 | 半径 `20 px`、深度 `105 mm`、面积 ≈ `1257 px²` |
| 图像尺寸 | `640 × 480`（`SimVision::kWidth` / `kHeight`） |

**推演**（按 §四 初值）：

1. `depthZMin = 6` / `depthZMax = 496` ⇒ 背景 `600 mm` 被滤除，两个目标（`55` / `105 mm`）都有效 ⇒ 有效像素 ≈ `2827 + 1257 = 4084`。
2. `nearestPercentile = 0.03` ⇒ 第 `(4084 − 1) × 0.03 ≈ 122` 个最近值；因 `55 mm` 目标有 `2827` 个像素（远多于 `122`）⇒ `nearestDepthMm = 55 mm`。
3. 最高层阈值 = `55 + 35 = 90 mm` ⇒ 只有 `55 mm` 那个目标进 `topMask`，**`105 mm` 目标被排除**。
4. ⇒ 连通域只剩 1 个 ⇒ **只检出 1 个目标**。

⚠ **这是预期行为变化，不是缺陷**（`implementation_roadmap.md` §9.5 项 6 已预告）：颜色法（`SimAlgo`）能检出 2 个，深度法在仿真下检出 1 个。**用例文档与验收判据须按「1 个」写**，否则会被误判为回归。

⚠ 另需注意：仿真深度是**平盘而非穹顶、且无噪声** ⇒ `EstimateConfidence` 的 `stabilityScore`（深度标准差）在仿真下恒为满分，**仿真验不出该项的判别力**。

## 七、须修正项与风险

| # | 项 | 说明 | 处理 |
| :--- | :--- | :--- | :--- |
| **M1** | ⚠ **`confidenceThreshold = 0.85` 对深度法可能过严** | 两套算法的 `confidence` **语义不同**：`SimAlgo` 是**面积覆盖率**（`SimAlgo.cpp:146-147`，`coverage × 1.08`）；深度法存入结果的是**三权重综合 `score`**（`EstimateConfidence × 0.60 + nearScore × 0.25 + areaScore × 0.15`，⚠ **不是** `EstimateConfidence` 本身——见 §三 更正）。同一个 `0.85` 在深度法上可能把有效目标滤掉。⚠ 但**必须消费该键**——否则又是「看起来在工作、实际不工作」的死键 | 先按 `0.85` 落地并消费（**已落地 TR-126**）；⚠ **2026-10-14 真机实测把该风险坐实**：`filtered 8~11 of 10~12 detection(s)`，且两次 `best score = 0.833 < 0.85` ⇒ **全部滤掉**、视觉页报「未找到目标」⇒ **真机调参（阶段 3）第一优先处理**；处理手段＝在 `vision.algorithms.DepthPuffAlgo` 加私有覆盖键（按 §十 裁定 3 的「私有优先」语义；⚠ 该路径**已可用**——通用三键的覆盖顺序已由 **TR-127** 修正） |
| **M2** | `area` 的坐标系未在契约中写明 | 算法在**深度域**做连通域 ⇒ `area` 单位是**深度图**像素²，而 `pixelU/pixelV` / `width` / `height` 归一到**彩色系**。D2C = `SW` 时两者等价；D2C 关闭时不等价 | 在 `algorithm_config_plan.md` §11.3 补注 `area` 的坐标系（本次已同步） |
| **M3** | ⚠ **`targetArea = 0` ⇒ 面积评分退化** | 老实现从未在真实泡芙上标定（交接文档 §九.4）⇒ `CalculateAreaScore` 走「够大就满分」分支（`area / (minArea × 8)`），**无法区分「单个泡芙」与「一堆泡芙」**——而后者恰恰是抓取最需要避免的 | 真机标定后**必须**设 `targetArea` / `targetAreaTolerance`；在此之前不要相信面积维度 |
| **M4** | 老实现的**已知限制**随移植带入 | 交接文档 §九：① 非语义级识别，只是深度高点区域定位；② RGB 未参与评分；③ 过近物体有深度孔洞 / 噪声；④ demo 会多帧择优 | **如实保留**，在 `doc/algorithm_config_plan.md` §十一 契约边界内属正常；不承诺鲁棒性 |
| **M5** | `edgeMargin` 与 `roi` 都在**深度系** | 二者按深度图像素给出；D2C 关闭时若按彩色分辨率填会错位 | 文档写明坐标系；调参时以 `_mask.png`（深度域）为准 |
| **M6** | 形态学核**偶数自动 +1** | `morphKernelSize = 6` 会静默变成 `7` | 配置页 / 文档写明；避免「填了 6 实际 7」的困惑 |

## 八、验收判据

**A 档（离线，无需相机）**：

1. `simulation.algorithmType = "DepthPuffAlgo"` 时程序启动无警告，日志出现算法注册与 `LoadConfig` 成功。✅ **已验（TR-127）**：`[DepthPuffAlgo] config loaded (183 bytes) …`，**无** `not registered` 警告。
2. 关掉 `vision.algorithms.DepthPuffAlgo` 节点（删键）后重启 ⇒ 走 `{}` + 内置默认值，**不报错**（§十 裁定 1）。✅ **已验（TR-127）**：`[DepthPuffAlgo] config empty (2 bytes) → built-in defaults`（**INFO 非 WARN**）。
3. 配置页 / `config.json` 中**不再出现算法私有键名于 `vision.*` 通用域**。✅ **已验（TR-127）**：通用域仅 `confidenceThreshold` / `depthZMin` / `depthZMax` / `templatePath` / `algorithms`。
4. 改 `heightBandMm` 后重启，仿真检出的目标个数按预期变化（验证参数真的被消费）。✅ **已验（2026-10-14，TR-126 离线自测）**：合成帧上 `heightBandMm` `35 → 60` ⇒ 检出个数 `1 → 2`。

**B 档（Sim）**：

5. 识别动作日志 `[SequenceWorker] Vision calib: pixel(u,v) depth=… intr(…)` 与 `[SequenceWorker] Vision target: … → robot(…)` 成对出现。⏳ **未验**——真机实测走的是**视觉页「单次检测」**（日志 `[VisionTestPage] 检测完成：发现 1/2/3 个目标`），**未跑含「识别」动作的方案**。
6. 仿真下**检出 1 个目标**（§六 推演），且 `results.front()` 是最优者。✅ **已验（2026-10-14，TR-126 离线自测）**：合成帧 `heightBandMm = 35` ⇒ **1 个**（`area=2815` ≈ πr²=2827、`z = topDepth = 55.0`、`score=0.962`）；`confidence` 单调不增 ⇒ `front()` 即最佳。⚠ 以**离线合成帧**替代「跑 Sim 程序」，且合成帧是**平盘 + 无噪声** ⇒ `stabilityScore` 恒满分、**验不出该项判别力**（§六 已预告）。
7. `[SimAlgo]` 前缀的日志**不再出现**（确认已切到 `DepthPuffAlgo`）。✅ **已验（TR-127）**：冒烟窗口内无 `[SimAlgo]`。

> **离线自测工具（2026-10-14，TR-126）**：`out/smoke/tmp_puff_algo_check.cpp`（**临时、非交付物**；手工 `cl.exe` + `link.exe` 链接已构建的 `DepthPuffAlgo.cpp.obj` + `Config.lib`，**未改任何受版本控制的文件**——故**不需要**动 `tests/CMakeLists.txt`，也就绕开了 `compile_guide.md` §3.5 的 vcpkg 陷阱）。**11 项全 PASS**，覆盖 A4 / B5 / B6 + 坐标归一 + TR-116 守卫 + 阈值过滤。重跑：`bash out/smoke/tmp_puff_build.sh && PATH=out/build/x64-Debug:$PATH ./out/smoke/tmp_puff_algo_check.exe`。

**C 档（真机）**：

8. 真机采图 → 视觉页结果表出现 `area` / `minDepthMm` 有意义的非零值。✅ **已验（2026-10-14 用户真机实测，TR-126）**：算法日志 `best area=675~917 meanDepth=161.5~213.6mm topDepth=161.0~211.0mm score=0.833~0.931`，视觉页 `检测完成：发现 1 / 2 / 3 个目标`。⚠ 走的是视觉页「单次检测」；⚠ **同批暴露 M1**：`confidenceThreshold = 0.85` 把 10~12 个候选滤到 1~3 个，两次运行 `best score < 0.85` ⇒ **全部滤掉**、报「未找到目标」。
9. 抓取落点与 `PixelToRobot` 结果自洽（须先完成 `hand_eye_calibration_plan.md` 的标定）。⏳ **未验**——待标定。
10. 按 `_mask.png` 量出的单泡芙面积与结果表 `area` 同量级（验证坐标系与单位）。⏳ **未验**——待真机量测。

## 九、台账与文档同步

- **已落地**：阶段 0 + 阶段 1 记 **TR-126**（2026-10-14，C1 / C3 / C4 / C5 与本算法本体同批）；阶段 2 的 Sim 冒烟 + A 档离线验收（①②③）+ 一处**配置覆盖顺序缺陷**修复记 **TR-127**（2026-10-14）。
- 落地后须同步：`TEST_RECORD.md` → 本文状态 → `algorithm_config_plan.md`（§六 各 C 项状态、§九 顺序）→ `doc/config.md`（新键 schema）→ `doc/README.md` 状态列 → `AGENTS.md` 的 Config / Current Phase。
- **无新增配置文件** ⇒ `build_release.bat` **无需改动**（与 `inspection_result_panel_plan.md` 的 `inspection.json` 情形不同——那个必须改打包脚本，此处只是 `config.json` 内加节点）。

## 十、已裁定项（2026-10-14：4 项**全部按「建议」列执行**）

> 随 TR-126 落地。下表「建议（= 裁定）」列即生效结论，各行末尾标注落地情况。

| # | 议题 | 选项 | 建议（= 裁定） |
| :--- | :--- | :--- | :--- |
| 1 | 老项目 `depthMm`（区域均值）是否保留为日志量 | ① 丢弃；② 留在 `SPDLOG_INFO` 里供调参对照 | **②**——调参时「均值 vs 最高点」的差值能反映泡芙穹顶高度，成本仅为一行日志 ✅ **已执行**：`Detect` 的汇总日志同时打 `meanDepth` 与 `topDepth` |
| 2 | 是否加「调参可视化」（把全部连通域的 `area` / `mean depth` / `score` 画出来） | ① 本期不做；② 与本体同批做 | **①**——`implementation_roadmap.md` §9.2 把它单列为 `0.5~1 人天`；先跑通通路，真机调参受阻时再补 ✅ **已执行**（本期不做） |
| 3 | `SimAlgo` 是否保留 | ① 保留（回退路径 + 契约对照）；② 删除 | **①**——`simulation.algorithmType` 一切即换，是回退与对照的唯一手段 ✅ **已执行**：保留，且行为**逐字节不变** |
| 4 | 算法类命名 `DepthPuffAlgo` 是否定稿 | ① 定稿；② 改名 | **①**——已写入 `algorithm_config_plan.md` §五 的目标 schema，改名会同时影响契约文档与配置键 ✅ **已执行**：定稿 `DepthPuffAlgo` |
