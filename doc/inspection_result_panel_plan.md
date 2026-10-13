# 自动运行页检测结果面板方案

> **性质**：⏳ **计划中**（2026-10-09 成稿，2026-10-10 修订入口形态）——本文是**方案**，不是已落地实现。技术事实（架构、参数、行为、踩坑）一律以 `AGENTS.md` 与源码为唯一权威源；本文与二者冲突时以二者为准。
>
> **需求来源**：用户 2026-09-24 提出「自动运行页目前只显示检测的图片和叠加图，需要增加检测项的图片和结果展示」；2026-10-09 就三项待裁定项作出裁定；2026-10-10 追加两项裁定——**入口形态定为页签**（否决上一稿的「按钮 + 非模态对话框」）、新增 `ui.inspectionPanel.show` 开关（见 §1.2）。
>
> **实施状态**：未实施。无前置阻塞项（与 `third_party_integration_plan.md` 的 `CaptureFrame()` 互斥不同，本方案不引入新的并发路径）。
>
> **配套文档**：`doc/camera_integration_plan.md`（检测结果通道 `detectionReady` / 决策 6）、`doc/multi_camera_plan.md`（`DetectionResult` 的相机归因，字段协调见 §8.2）、`doc/third_party_integration_plan.md`（第三方对接与拍照动作，边界见 §1.4）、`doc/vision_test_page_redesign_plan.md`（视觉页四宫格，滚动区与表格样式参照）、`doc/config_ownership_plan.md`（配置分层原则）、`doc/process.md`、`doc/config.md`。

---

## 一、需求与裁定

### 1.1 需求原文

自动运行页目前只显示「检测图片」与「叠加图」两格。需要增加：

1. **检测项的图片与结果展示**；
2. 界面左侧一个**可上下铺满的滑动框**，显示实时检测结果图片；
3. 右侧显示各检测项结果：**检测项名称、数值标准范围、单项判定 OK / NG、整体判定 OK / NG**，以及**统计数据导出**；
4. 界面需适配不同检测目标（本项目 = 泡芙，另一项目 = 成品鞋），做成**插件式**；
5. 询问入口形态：自动运行页加「检测结果」按钮弹对话框，还是单独做一个菜单页。
6. （2026-10-10 追加）提出**分两个页签**——一个显示原先的 RGB + 叠加图，一个显示检测结果；并询问能否加一个 `show` 配置，让不做检测的项目不显示该页签。

> 术语约定：需求里的「检测箱」按裁定 1 等同于**检测项**；「检测项的图片」按上下文即**检测结果图**（每轮一张叠加图），不是「每个检测项一张截图」。

### 1.2 五项裁定（用户）

| # | 议题 | 裁定 | 对本方案的影响 |
| :- | :--- | :--- | :--- |
| 1 | 「检测箱」语义（2026-10-09） | **即「检测项」本身**，不是「一组检测项的容器」 | 规格 schema **单级**即可（`items[]` 平铺），不引入二级分组 |
| 2 | 方案落盘（2026-10-09） | 落 `doc/inspection_result_panel_plan.md`（本文） | — |
| 3 | 检测规格落点（2026-10-09） | **独立文件 `config/inspection.json`** | 不并入 `config.json`，也不挂到 `process.json` 的方案上（理由见 §4.4） |
| 4 | 入口形态（2026-10-10） | **`AutoRunPage` 内分两个页签**：页签 1 = 原先的 RGB + 叠加图；页签 2 = 检测结果 | **否决**上一稿的「按钮 + 非模态对话框」，也否决单独菜单页（§6.1）。页签切换**不触发** `AutoRunPage::hideEvent`，相机持有不受影响（§6.2） |
| 5 | 显示开关（2026-10-10） | **新增 `ui.inspectionPanel.show`**（默认 `true`），不做检测的项目可关闭 | 与「无规格文件则不建页签 2」构成两层信号（§6.6） |

### 1.3 需求拆解为四个子问题

| # | 子问题 | 落点 | 复杂度 |
| :- | :--- | :--- | :--- |
| 1 | **检测项数值从哪来** | 现有链路**没有**这个概念（现状核实 #1）⇒ 新增数据模型 + 算法侧产出（§三 / §八） | **高**（本方案的主要工作量） |
| 2 | 标准范围从哪来、谁判 OK / NG | 规格文件 + 纯函数判定器（§三.5 / §四） | 中 |
| 3 | 界面怎么摆、结果图怎么限流 | 页签视图 + 环形缓冲（§五） | 中 |
| 4 | 怎么适配不同检测项目 | 三层可替换，视图层用编译期注册表（§七） | 低（本期只做默认实现） |

### 1.4 ⚠ 与第三方对接方案的边界（须先厘清）

两个方案都在「检测 / 分析」上落地，但**主体与去向不同**：

| | 本方案 | `third_party_integration_plan.md` |
| :- | :--- | :--- |
| 谁做分析 | **本软件**（`IPuffAlgorithm` 实现） | **第三方**（其自带模型） |
| 结论去向 | 本机界面 + CSV 导出 | **不出本机** |
| 触发动作 | 现有「识别」动作（`ActionType::Vision`） | 新增「拍照」动作（`ActionType::Capture = 6`） |
| 上行载荷 | `detectionReady`（增 `cycleIndex`） | `captureReady` / `captureData` / `captureFailed` |

**结论：本期两者独立**——检测结论不出本机，`captureReady` 载荷**不变**。

若将来第三方需要读检测结论，在 `captureReady` 的 JSON 描述帧里**追加一个可选 `inspection` 字段**即可（向后兼容，不破坏现有协议）。⚠ **不要**为此改动 `captureData` 的二进制帧结构。

⚠ **另一处易混的重叠**：第三方方案的 `ActionData::captureTag`（动作级字符串标识，用于**缺陷类型分类**）与本方案的 `items[].key`（检测项**稳定标识**）语义相近但**不是一回事**——前者标识「这张图是拍什么的」，后者标识「这个数值测的是什么」。**不要合并这两个字段**，它们的生命周期与用途都不同。

---

## 二、现状核实（关键事实与证据）

> 全部结论均由源码核实，非推断。带 `file:line`。

| # | 事实 | 证据 | 对本方案的影响 |
| :- | :--- | :--- | :--- |
| 1 | **当前链路没有任何「检测项」概念** | `PuffResult`（`src/HAL/interfaces/IPuffAlgorithm.h:8-19`）只有 `x` / `y` / `z` / `yaw` / `confidence` / `pixelU` / `pixelV` / `width` / `height`；`DetectionResult`（`src/Logic/SequenceWorker.h:65-70`）= `{frame, results, threshold}` | 「检测项」是**全新数据链**，不是加控件。须先补 §三 |
| 2 | `width` / `height` 是检测框的**像素**尺寸 | `SimAlgo::Detect` 内由色块累积器的 `maxX - minX + 1` 得出（`src/HAL/algorithm/SimAlgo.cpp` 的 `Acc` 结构） | 物理量须换算（§3.4），且换算依赖算法语义 ⇒ **测量值应由算法产出** |
| 3 | HAL **接口层无 Qt** | `src/HAL/interfaces/` 下全部头文件只含 `<string>` / `<vector>` / `<memory>` / `<cstdint>`（已核，零 `include <Q`） | `Measurement` 必须用 `std::string` / `std::vector`，**不得引入 `QString`** |
| 4 | 自动运行页现布局 | `AutoRunPage::SetupUI`（`src/UI/AutoRunPage.cpp:94-394`）`QHBoxLayout` → 左侧 `m_cameraRgbLabel` + `m_cameraOverlayLabel`（各 stretch 1，`:129-130`）；右侧 LCD / 状态 / 坐标 / 日志（stretch 1）/ 方案 / 循环 / 5 按钮 | **左侧 60% 列改为页签容器**（页签 1 = 现有两格，页签 2 = 检测结果）；**右侧控制列完全不动**（§5.1） |
| 5 | 自动模式**导航白名单会强制跳回自动运行页** | `MainWindow::ApplyModeState`（`src/UI/MainWindow.cpp:819-826`）自动模式下 `stack_->setCurrentIndex(kPageAutoRun)` | **单独菜单页在自动运行场景下站不住**（§6.1） |
| 6 | 项目已有 `QTabWidget` **深色样式先例** | `src/UI/VisionTestPage.cpp:624-631`（`QTabWidget::pane { border: none; }` + `QTabBar::tab:selected { background: #2f6f9f; color: white; }`） | 页签样式直接沿用，不新造一套（§6.3） |
| 7 | 渲染节流（M7）**未做** | `doc/camera_integration_plan.md:46` 列为未做项、`:272` 仍在待办 | 结果图必须**按轮**而非按帧（§5.2） |
| 8 | 视觉页已有 `QScrollArea` + `QTableWidget` 实现可参照 | `src/UI/VisionTestPage.cpp:360-366`（左 320 px 滚动栏）、`:634`（结果表） | 滚动区与表格样式沿用同款做法 |
| 9 | 配置路径已有**唯一实现** | `ConfigPaths::resolve(fileName)` / `configJsonPath()` / `processJsonPath()`（`src/Config/ConfigPaths.h:26-32`） | `inspection.json` 走 `ConfigPaths::resolve`，**不新写第四套路径解析**（`config_ownership_plan.md` C5 的教训） |
| 10 | 相机内参可由实现真实提供 | `CameraIntrinsics {fx, fy, cx, cy, k1, k2, p1, p2, width, height}`（`src/HAL/interfaces/ICamera.h:8-15`）；`ICamera::GetIntrinsics()`（`:174`）；`OrbbecCamera` 在 `Open()` 后由 SDK 填充 | 像素 → 物理换算有依据（§3.4） |
| 11 | Sim 场景的检测目标**带像素半径与深度** | `SimVision::TargetSpec {int r,g,b; int radius; float depthMm; ...}`（`src/HAL/camera/SimVision.h:9-23`） | Sim 下可产出**可反算真值**的「直径」测量项 ⇒ B 档用例能离线验收（§十） |
| 12 | 算法注册宏名为 `REGISTER_PUFF_ALGORITHM` | `src/HAL/core/HALFactory.h:117`；`SimAlgo` 以 `REGISTER_PUFF_ALGORITHM("SimAlgo", SimAlgo)` 注册（`src/HAL/algorithm/SimAlgo.cpp:14`） | §7.2 的复用依据 |
| 13 | 台账当前 **TR-001 ~ TR-122**；阶段 14 已被第三方方案占用 | `TEST_RECORD.md:3`；`doc/third_party_integration_plan.md` §十二 | 本方案用例接**阶段 15**；TR 编号**不预留号段**，实施时按当日台账最大号追加 |
| 14 | ⚠ **`build_release.bat` 只复制 `config.json` + `process.json`** | `build_release.bat:94-95`（`copy /Y "%BUILD%\config.json"` / `process.json`） | 新增 `inspection.json` 后**必须同步改打包脚本**，否则部署包缺规格文件（§九） |
| 15 | worker 线程**绝不读 `ConfigManager`** | `src/Logic/SequenceWorker.cpp:106` 的同名约定；`AlgorithmParams` 的 `SnapshotParams()` 即为绕开它而设计（`src/HAL/interfaces/IPuffAlgorithm.h:21-33`） | 规格读取**只能在主线程**，判定调用点须避开 worker 线程（§3.5） |
| 16 | 循环轮次在 worker 内可读 | `impl_->cycleIndex`（`src/Logic/SequenceWorker.cpp:114`，1-based，`:619` 赋值后 `:620` 发 `cycleChanged`） | `DetectionResult` 带轮次零成本（§8.3） |
| 17 | ⚠ **页签切换不触发 `AutoRunPage::hideEvent`** | `QTabWidget` 的「页面」是本页的**子控件**，本页自身始终可见 ⇒ 页面级的 `showEvent` / `hideEvent`（`src/UI/AutoRunPage.cpp:422-444`）不因页签切换而触发 | 页签方案**不会**释放相机持有——这正是它优于「单独菜单页」的关键（§6.2） |
| 18 | ⚠ 渲染判据现为**页面级**可见性 | `OnDetectionReady` 的 `if (!isVisible()) return;`（`src/UI/AutoRunPage.cpp:493`）判的是 `AutoRunPage` 自身 | 加页签后**必须下沉一格**改判 `m_cameraRgbLabel->isVisible()`，否则页签 2 激活时仍给隐藏标签做全帧缩放（§6.4） |

---

## 三、数据模型（新增）

### 3.1 三个结构，职责严格分离

| 结构 | 所在层 | 职责 | 变化频率 |
| :- | :--- | :--- | :--- |
| `Measurement` | HAL（`IPuffAlgorithm.h`） | 算法测出的**原始数值** | 每目标、每轮 |
| `InspectionItem` / `InspectionSpec` | Config（`inspection.json`） | **标准范围**与判定规则 | 换产品时改 |
| `InspectionItemResult` / `InspectionReport` | Logic（判定器输出） | 数值 + 规格 → **逐项判定 + 整体判定** | 每轮 |

⚠ **判定不放进算法实现里**。算法只负责「测出数值」，判定交给**统一的评估器**。否则每个算法插件都要重复实现一遍区间比较与整体汇总逻辑，且标准范围会散落进代码——换产品就得改代码，插件化的意义直接消失。

### 3.2 `Measurement`（HAL 层，纯 C++）

```cpp
// src/HAL/interfaces/IPuffAlgorithm.h
struct Measurement
{
    std::string key;      // 与 InspectionSpec 的 items[].key 对应，稳定标识
    double      value = 0.0;
};

struct PuffResult
{
    // ... 现有字段全部不变 ...
    std::vector<Measurement> measures;   // 新增：本目标的测量项（可为空）
};
```

⚠ **为什么放在 `PuffResult` 里，而不是另开一个载荷**：测量值天然属于「单个目标的检测结果」——泡芙的直径、成品鞋的鞋码都是**每个目标一组**。放这里让 `detectionReady` 自动带上，UI 侧零额外链路，且 `Detect` / `LocateBest` 的**签名不用改**。

⚠ **为什么用 `std::string` / `std::vector`**：`src/HAL/interfaces/` 是**纯 C++ 接口层**（现状核实 #3），引入 `QString` 会把 Qt 依赖拖进 HAL 接口。UI 侧用 `QString::fromStdString` 转换（项目既有做法）。

⚠ **不新增 `IPuffAlgorithm` 虚函数**。理由：新增虚函数会让**所有既有实现**都必须实现（否则编译失败或静默回落），而测量值本就是「检测结果的附带产物」，跟着 `PuffResult` 出最自然。

### 3.3 整图级检测项（预留，本期 UI 不展示）

有些检测项不属于单个目标（「整箱数量」「有无异物」）。为**避免二次改载荷**，`InspectionItemResult` 保留一个归属字段：

```cpp
struct InspectionItemResult
{
    QString key;
    QString label;
    QString unit;
    double  value     = 0.0;
    bool    measured  = false;     // false = 算法未产出该 key
    Verdict verdict   = Verdict::Skipped;
    int     targetIndex = 0;       // -1 = 整图级；>= 0 = 第 N 个目标（与 results 下标对应）
};
```

⚠ **本期表格按「主目标」口径显示**：与 `SequenceWorker` 视觉分支取 `results.front()` 的既有口径一致（`src/Logic/SequenceWorker.cpp:1022`）。`targetIndex` 字段**保留但 UI 不分栏**——等真有整箱多目标检测需求时再扩，届时不用改载荷。

### 3.4 像素 → 物理量的换算（由算法负责）

小孔成像模型：`物理尺寸 = 像素尺寸 × z / fx`（`z` 为相机系深度，单位 mm；`fx` 为像素焦距）。

⚠ **三条限制，必须知情**：

1. **只对垂直于光轴的平面成立**。倾斜面会因透视关系系统性偏大。
2. **球体的外接框宽度 ≠ 物理直径**。泡芙是球体，投影边界与外接框之间需要几何补偿；Sim 场景可用 `SimVision::TargetSpec::radius` 直接反算真值做对照（现状核实 #11）。
3. **Sim 的 z 只作参考**。`SimAlgo` 在 D2C 关闭时用**比例最近邻**做深度取样（`SimAlgo.cpp` 的 `depthAlignWarned_` 已留痕），两传感器视场不同，像素级对应关系并不严格成立 ⇒ **Sim 下的物理量仅供链路验证，不作精度判据**。

**结论**：换算逻辑写在**算法实现**里——只有算法知道自己的像素语义（是外接框？拟合圆直径？主轴长度？）。判定器与视图**只消费数值**，不做任何换算。

### 3.5 判定器：纯函数，调用点在主线程

```cpp
// src/Logic/Inspection.h
enum class Verdict { Ok, Ng, Skipped };

struct InspectionReport
{
    quint64                       cycleIndex = 0;
    qint64                        timestamp  = 0;
    Verdict                       overall    = Verdict::Skipped;
    QVector<InspectionItemResult> items;
};

// src/Logic/InspectionEvaluator.h —— 纯函数，无全局状态，可离线单测
class InspectionEvaluator
{
public:
    static InspectionReport Evaluate(const InspectionSpec& spec,
                                     const QVector<PuffResult>& results,
                                     quint64 cycleIndex, qint64 timestampMs);
};
```

⚠ **判定调用点放在 UI 线程（`AutoRunPage::OnDetectionReady`），不放 worker 线程**。依据：

- worker 线程**绝不读 `ConfigManager`**（现状核实 #15）。判定需要规格，规格是配置数据。若要在 worker 侧判定，就必须把 `InspectionSpec` 做成快照随 `RunSequence` 进线程——那会**改动 `RunSequence` 的签名**，而 `third_party_integration_plan.md` 的 `InterfaceServer` 也要用这个签名（§4.4 的 `run` 命令），改动面会外溢到未实施的方案上。
- 判定是**纯函数**，放哪一层都不产生架构成本；放 UI 侧零侵入。
- 本期判定结果的**唯一消费者就是 UI**（§1.4 已定「检测结论不出本机」）。

**将来第三方要读检测结论时**的扩展路径：在接口层的 `captureReady` 载荷里追加可选 `inspection` 字段，由 `InterfaceServer` 自己调同一个判定器即可——**判定器是纯函数，复用零成本**，不需要把判定搬进 worker。

### 3.6 判定规则（三条，写死）

| # | 规则 | 说明 |
| :- | :--- | :--- |
| 1 | **单项**：`min ≤ value ≤ max` → OK，否则 NG | 上下限为**闭区间**（边界值算合格） |
| 2 | **缺测**：算法未产出该 `key` → `required` 为真判 **NG**（文案「未测到」）；为假判 **跳过** | `required` 用于表达「本项目不检这一项」 |
| 3 | **整体**：全部非跳过项均 OK → 整体 **OK**；任一项 NG → 整体 **NG**；**全部跳过 → 整体跳过** | ⚠ 全部跳过时**绝不显示 OK**——避免「什么都没测却显示合格」 |

---

## 四、检测规格 `config/inspection.json`

### 4.1 schema（单级，依裁定 1）

```json
{
    "schema": "CreamPuffRobot inspection spec",
    "projectType": "puff",
    "displayName": "泡芙外观检测",
    "items": [
        { "key": "diameter",  "label": "直径",     "unit": "mm", "min": 43.0, "max": 47.0, "nominal": 45.0, "required": true },
        { "key": "height",    "label": "高度",     "unit": "mm", "min": 28.5, "max": 31.5, "nominal": 30.0, "required": true },
        { "key": "roundness", "label": "圆度",     "unit": "",   "min": 0.90, "max": 1.00, "nominal": 0.95, "required": true },
        { "key": "surface_defect", "label": "表面缺陷", "unit": "处", "min": 0.0, "max": 0.0, "nominal": 0.0, "required": true }
    ]
}
```

| 键 | 类型 | 说明 |
| :--- | :--- | :--- |
| `schema` | string | 文件标识，供未来格式升级判据 |
| `projectType` | string | 项目标识，用于选**视图实现**（§7.3）。`"puff"` / `"shoe"` / … |
| `displayName` | string | 面板标题 |
| `items[].key` | string | **稳定标识**，与算法产出的 `Measurement::key` 对齐。⚠ 一旦被 CSV 导出引用就**不应再改** |
| `items[].label` | string | 显示名（中文） |
| `items[].unit` | string | 单位符号（`mm` / `°` / `处`），可为空串 |
| `items[].min` / `max` | double | **闭区间**上下限 |
| `items[].nominal` | double | 名义值，**仅用于显示**（不参与判定） |
| `items[].required` | bool | `true` = 缺测即判 NG；`false` = 缺测判跳过（不计入整体判定） |

> ⚠ 上表数值为**示例**，非项目实测规格。实际上下限须由工艺给出后填入。

### 4.2 加载与生效时机

- **路径**：`ConfigPaths::resolve("inspection.json")`（现状核实 #9）。三者都不存在时返回候选 1，调用方须按「文件不存在」处理。
- ⚠ **生效时机必须显式定义**。`inspection.json` 是**独立文件**，不走 `ConfigManager` 的内存 `root_`，所以既有那条「经 UI 改即时生效 / 直接编辑文件需重启」的规则**不能直接沿用**——须自己定义：

| 场景 | 建议行为 |
| :--- | :--- |
| 启动时 | 加载一次 |
| 经 UI 改（若本期做编辑入口） | 重载并**即时生效** |
| 直接编辑文件 | **需重启**（与 `config.json` 保持同一心智模型） |

⚠ **不要静默做文件监视**（`QFileSystemWatcher`）——它会让「为什么刚才改了没生效」变成不可解释的现象，且编辑器保存过程会触发多次中间态。若将来确实需要「改文件即生效」，应提供**显式重载按钮**并打日志。

- 建议新增 `InspectionSpecStore`（`src/Config/`），提供 `Load(bool force)` 与 `Current()`；**幂等加载**（已加载且文件仍存在则直接返回），与 `ProcessManager::load` / `ConfigPaths::ensureConfigLoaded` 的既有约定一致。
- ⚠ **解析失败 / 文件缺失**：回落**空规格**（`items` 为空）+ 打一条明确日志。此时面板显示「无检测项配置」，整体判定为**跳过**——**绝不判 OK**（§十二 的风险项）。

### 4.3 多目标与多产品

⚠ **单文件 vs 按项目多文件**：本期**单文件**（换产品 = 替换文件内容）。

若将来要「一台设备存多份规格并在线切换」，再扩为 `config/inspection/<projectType>.json` + `config.json` 里一个选择键。**本期不做**——先跑通单产品，避免为一个未确认的需求增加配置层级。

### 4.4 为什么用独立文件（裁定 3 的理由，供追溯）

| 备选 | 否决理由 |
| :--- | :--- |
| 并入 `config/config.json` | 检测规格是**按产品变**的（换产品换整份），而 `config.json` 是**按设备变**的（装机一次）。混在一起会让「同一台设备切产品」变成整份设备配置的读写 |
| 挂 `config/process.json` 的方案 | 语义上「换产品 = 换方案」也顺，但 `process.json` 是**动作树**，塞检测规格会污染其 schema；且方案可复制 / 改名，规格会跟着漂 |
| **独立 `config/inspection.json`** | 与 `ConfigPaths` 的「配置目录」约定一致；换产品 = 换一个文件。`config_ownership_plan.md` 的结论是「**现有**键现在不分」，但**新键应一开始就放对地方** |

---

## 五、界面设计

### 5.1 布局：页签只包左侧列

入口是 `AutoRunPage` 左侧列内的 `QTabWidget`（§六），两个页签：

```
┌──────────────────────────── AutoRunPage ────────────────────────────┐
│ ┌─ [实时画面] [检测结果] ─────────────────────┐ ┌─ 控制列（常驻）─┐ │
│ │                                             │ │ CT 2.4s 今日…   │ │
│ │  页签 1：RGB 相机画面                        │ │ 待机            │ │
│ │  ──────────────────────────────────────     │ │ 坐标 X Y Z R    │ │
│ │  页签 1：识别结果叠加图                      │ │ ┌─ 日志 ──────┐ │ │
│ │                                             │ │ │            │ │ │
│ │  ── 切到页签 2 ──                            │ │ └────────────┘ │ │
│ │  ┌─────────────────┐ ┌──────────────────┐   │ │ 循环 / 方案     │ │
│ │  │ #128 10:42:15 OK│ │ 整体判定 [OK]    │   │ │ [启动][暂停]    │ │
│ │  │  [结果叠加缩略] │ │ 良率 98.4%       │   │ │ [停止][清报警]  │ │
│ │  ├─────────────────┤ ├──────────────────┤   │ │ [初始化]        │ │
│ │  │ #127 10:42:12 NG│ │ 检测项 实测 标准 │   │ │                 │ │
│ │  │  [结果叠加缩略] │ │ 直径 44.8 43~47  │   │ │ ↑ 整列在页签之外│ │
│ │  ├─────────────────┤ │ 表面缺陷 1处 0 NG│   │ │   两页签都能点  │ │
│ │  │ #126 …          │ │ 累计 128 · NG 2  │   │ │                 │ │
│ │  │  ↑ 垂直滚动      │ │ [导出CSV][清空]  │   │ │                 │ │
│ │  └─────────────────┘ └──────────────────┘   │ │                 │ │
│ └─────────────────────────────────────────────┘ └─────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

页签 1 是**现有两格原样搬入**（`m_cameraRgbLabel` + `m_cameraOverlayLabel`），画面来源仍由 `ui.autoRunStreamPreview` 决定——`false` 显示「上轮检测原图 + 叠加图」（`RenderDetectionTiles`），`true` 显示实时流（`RenderCameraTiles`）。**两条路径的渲染代码一行不改**，只把可见性闸门下沉一格（§6.4）。

页签 2 内左右分：左侧 `QScrollArea`（可上下铺满）承载结果图列表，右侧自上而下为整体判定徽标 / 检测项判定表 / 统计与导出。

⚠ **页签只包左侧 60% 列，右侧控制列必须留在页签之外**，两条硬理由：

1. 「■ 停止」在按钮行里。按钮若被关进某一页签，操作员在另一页签上就点不到它——急停虽已在 MainWindow 顶栏全局可用，但「停止」是轻量动作，不该逼操作员升级到急停。
2. 状态 / 日志 / 坐标是**跨页签都要看的态势信息**，进页签就会出现「看结果时看不到状态」。

代价（须知情）：1280 屏下页签 2 的判定表只有约 340 px 宽，4 列够用但偏紧。若须更宽，只能把控制列整体下移为底部常驻条并压缩日志框——那是更大的改动，**本期不做**。

### 5.2 结果图滚动区（限流，硬约束）

| 项 | 规则 | 理由 |
| :--- | :--- | :--- |
| 粒度 | **每轮一张**（一次 `detectionReady` 一张），**不是每帧** | 渲染节流（M7）未做（现状核实 #7）；按帧会以 15 fps 做全分辨率 `ColorToQImage` + `setPixmap`，与用户正在看的页面抢 UI 线程 |
| 缓存 | **环形缓冲，上限 50 张**（可配） | 无界增长会在长稳运行时持续吃内存 |
| 存储形态 | 固定宽 **480 px** 的 `QPixmap`（≈ 0.5 MB/张） | 存全分辨率 `QImage`（1280×720 ≈ 2.7 MB/张）则 50 张 ≈ 135 MB |
| 追加位置 | 新结果插到**顶部**（最新在上） | 现场关心「刚才那件」；且滚动条停在顶部不跳动 |
| 不可见时 | **只入缓存、不产位图**，切回该页签时补绘 | 沿用 `AutoRunPage` 的 P3-4 既有策略（`src/UI/AutoRunPage.cpp:491-494`），避免停留在页签 1 时仍空转 |

⚠ **`QPixmap` 只能在 GUI 线程创建**。本面板的渲染全在 UI 线程（`detectionReady` 是 Queued 到主线程），符合该约束——**不要**为了「省 UI 线程」把渲染挪到 worker 线程。

### 5.3 检测项判定表

| 列 | 内容 | 来源 |
| :--- | :--- | :--- |
| 检测项 | `items[].label` | 规格 |
| 实测 | 数值 + 单位（保留 1~2 位小数） | 算法 `Measurement` |
| 标准 | `min ~ max` + 单位（如 `43.0 ~ 47.0 mm`） | 规格 |
| 判定 | **OK** / **NG** / 跳过 | 判定器 |

- 判定列用**颜色 + 文字双编码**（不单靠颜色——色盲用户与截图打印都要可读）。
- 未测到的行显示「**未测到**」而非空白，避免被误读成「0」。
- 多目标时按**主目标**口径（§3.3）。

### 5.4 整体判定与统计

- **整体判定徽标**：大号 OK / NG / 跳过 + 本轮轮次 + 时间。
- **统计**：累计轮数 / OK 数 / NG 数 / **良率**（`OK ÷ (OK + NG)`，⚠ **跳过项不进分母**）。
- ⚠ **统计口径 = 本次会话**（面板打开后累计），**不跨重启累加**。跨重启累加需要持久化，会把良率变成「被昨天数据污染的数」——若确需生产统计，应另立需求（见 §11.2 待确认 2）。

### 5.5 导出

- 格式 **CSV**，用 `QFileDialog::getSaveFileName`（项目既有用法：`src/UI/VisionTestPage.cpp:1307`）。
- ⚠ **必须写 UTF-8 BOM**（`EF BB BF`）：否则 Excel 打开中文列名乱码（`doc_output_standards_guide.md` §8 的编码要求）。
- 列：`轮次, 时间, 整体判定, <item1.label>, <item1.判定>, <item2.label>, <item2.判定>, …`
  —— **按规格动态生成列**，换产品自动适配，不硬编码列。
- 范围：**当前会话全部轮次**（与统计同口径）。

---

## 六、入口：`AutoRunPage` 内的两个页签

### 6.1 否决的两条路（依据均可验证）

1. **单独菜单页 —— 站不住。** 自动模式下 `MainWindow::ApplyModeState`（`src/UI/MainWindow.cpp:819-826`）强制 `setCurrentIndex(kPageAutoRun)`，新页**根本停不住**（现状核实 #5）。这不是偏好，是 TR-075 互锁的必然结果。
2. **按钮 + 非模态对话框 —— 可行但更差。** 弹窗要额外处理窗口标志（`Qt::Window` vs `Qt::Tool`）、位置 / 大小持久化、被主窗口盖住的风险，以及「关闭不析构」的生命周期。10 寸触控屏上这些都是净成本，而**页签用一次点击替代全部**——操作员本来就在自动运行页。

### 6.2 页签方案成立的关键事实

⚠ **页签切换不触发 `AutoRunPage::hideEvent`**（现状核实 #17）。`QTabWidget` 的「页面」是本页的**子控件**，本页自身始终可见 ⇒ 页面级的 `showEvent` / `hideEvent`（`src/UI/AutoRunPage.cpp:422-444`）不因页签切换而触发。

这一条同时解释了「菜单页为什么不行、页签为什么行」：菜单页是**换页**（触发 `hideEvent` → `TeardownStreamPreview()` → 释放相机持有），页签是**换子控件**（本页不动）。

⇒ 页签方案下「切走释放相机持有」**不存在**，也不需要任何「非模态」约束——因为根本没有对话框。

### 6.3 页签样式与结构

- **控件**：`QTabWidget`，样式**直接沿用视觉页既有实现**（`src/UI/VisionTestPage.cpp:624-631`：`QTabWidget::pane { border: none; }` + `QTabBar::tab:selected { background: #2f6f9f; color: white; }`）——不新造一套深色页签样式。
- **落点**：`AutoRunPage::SetupUI()` 的**左侧列**（`src/UI/AutoRunPage.cpp:102-130`）由「两个 `QLabel` 直挂 `QVBoxLayout`」改为「一个 `QTabWidget`」：
  - **页签 1「实时画面」**：`m_cameraRgbLabel` + `m_cameraOverlayLabel` 装进一个 `QWidget` 作页签页，**内部布局与现有代码逐行一致**。
  - **页签 2「检测结果」**：`InspectionResultPanel`（宿主：滚动区 + 环形缓存 + 统计导出，§九）+ 内嵌 `IInspectionView`（判定表，§7.3 / §7.4）。
- **创建时机**：**构造期判定**（§6.6），第一期不做运行时增删。

### 6.4 ⚠ 渲染闸门必须下沉一格（漏了会静默废掉既有优化）

现有 `OnDetectionReady` 用**页面级**可见性做闸门：

```cpp
// src/UI/AutoRunPage.cpp:493
if (!isVisible()) return;   // 判的是 AutoRunPage 自身
```

加页签后，页签 2 激活时页签 1 的标签已隐藏，但页面仍 `isVisible() == true` ⇒ 照样走 `RenderDetectionTiles()`：整帧 `ColorToQImage` + `scaled()` + `QPixmap::fromImage` **全部白做**。2026-09-23 那条「不可见只缓存、不产位图」的优化会被**静默废掉**（现状核实 #18）。

**改法**：闸门换成**标签自身**的可见性——`m_cameraRgbLabel->isVisible()`。`QWidget::isVisible()` 对子控件会连带检查所有祖先，正是需要的语义（页签未选中 ⇒ 标签不可见 ⇒ 早退）。

### 6.5 ⚠ 必须加页签切换重绘钩子

`RenderDetectionTiles` 用 `m_cameraOverlayLabel->contentsRect().size()` 当缩放目标（`src/UI/AutoRunPage.cpp:515`）。标签隐藏期间该尺寸不可靠（未按当前布局定尺）⇒ 切回页签 1 会看到一张**按错误尺寸缩放**的旧图，直到下一轮检测（节拍 2.4 s）才自愈。

目前这个补绘职责由 `showEvent` 承担（`src/UI/AutoRunPage.cpp:431-434`），但**页签切换不走 `showEvent`**（§6.2）。

**改法**：`connect(tabs, &QTabWidget::currentChanged, ...)` → 对**新变为可见**的那个页签补绘一次（页签 1 → `RenderDetectionTiles()`；页签 2 → 结果图列表按需补建缩略图）。

### 6.6 ⚠ 显示开关：`ui.inspectionPanel.show`

需求：「有的项目不需要展示检测结果」。做成**两层信号**：

| 层 | 判据 | 覆盖场景 |
| :--- | :--- | :--- |
| **自动** | `config/inspection.json` 不存在或解析失败 ⇒ 无规格 ⇒ **不建页签 2** | 「这个产品线根本不做检测判定」——零配置 |
| **显式** | `config.json` 的 `ui.inspectionPanel.show = false` ⇒ **不建页签 2** | 「有规格，但这次不想显示」（调试、临时关闭） |

- **键规格**：`ui.inspectionPanel.show`，bool，默认 `true`。落点与既有 `ui.autoRunStreamPreview` **同款**（`src/UI/ConfigPage.cpp:584` 勾选框 → `ConfigManager::set()` → `src/UI/MainWindow.cpp:504-508` 启动应用一次）。
- ⚠ **生效时机与 `autoRunStreamPreview` 不同**：后者只改**画面来源**，可即时切换；`show` 控制的是**页签的增删**（结构级）。第一期**只在构造期判定 ⇒ 改配置需重启**。理由：运行时 `addTab` / `removeTab` 还要额外维护当前索引、布局重算与补绘状态机，收益不值这个复杂度。该口径与 `doc/config.md` 已记的「手改本文件则需重启」一致。
- ⚠ **页签 2 不建时，不得让页签 1 退化成「唯一的页签」**——`QTabWidget` 即使只有一个页签，页签栏仍占约 35 px 竖向空间。**做法：页签数 ≤ 1 时把页签 1 的内容直接挂回原 `QVBoxLayout`，不创建 `QTabWidget`**，使不做检测的部署布局与改动前**完全一致**。

---

## 七、插件式：三层可替换

### 7.1 三层，本期只做两层

| 层 | 内容 | 本期 |
| :--- | :--- | :--- |
| **算法实现** | `IPuffAlgorithm` + `REGISTER_PUFF_ALGORITHM`（**已存在**，现状核实 #12） | 扩测量能力（§八） |
| **检测规格** | `config/inspection.json` | **做**（§四） |
| **视图实现** | `IInspectionView` + 注册表 | **只做默认通用实现**，注册表留口 |
| 判定器 + `InspectionReport` | 主程序固定 | **做**（§3.5） |

### 7.2 ⚠ 为什么**不**用 `QPluginLoader` 动态库插件

**第一性原理：插件的价值 = 「不重编译主程序就能换实现」。这个前提在本项目不成立。**

- 换检测项目（泡芙 → 成品鞋）**必然要换算法实现**，而本项目的算法是**编译期注册**的：`PuffAlgorithmFactory` 走 `REGISTER_PUFF_ALGORITHM` 宏（`src/HAL/core/HALFactory.h:117`），与 `MotionCard` / `AxisServo` / `Camera` 同族。
- 既然算法都要重编译，界面插件用动态库换来的「免重编译」收益为**零**，却要多付：每个插件一个 CMake target、Qt 版本 ABI 约束、部署时 dll 必须随行、插件加载失败时的降级路径。
- 项目已有**更轻的同款范式**（`HardwareFactory<T>` + 注册宏），复用它比新造一套更一致——**与 HAL 多品牌体系同构**。

**结论**：视图层沿用**编译期注册表**，与 HAL 完全同构。将来若真有「第三方独立开发插件」的需求，再评估 `QPluginLoader`——届时算法层也要一起改，不在本期。

### 7.3 视图接口（保持极薄）

```cpp
// src/UI/inspection/IInspectionView.h（新增）
class IInspectionView
{
public:
    virtual ~IInspectionView() = default;
    virtual QString  ProjectType() const = 0;            // 与 inspection.json 的 projectType 对应
    virtual QWidget* CreateView(QWidget* parent) = 0;    // 视图本体
};
```

- **选型**：`projectType` 命中注册表 → 用注册的实现；**未命中 → 回落默认通用视图**（数据驱动，永不失败）。
- ⚠ 接口**不做**「让插件接管整个面板」的设计——那会把窗口管理、环形缓存、导出、统计都推给插件，每个插件重复实现一遍。**插件只提供中间那一块视图**。

### 7.4 默认通用视图（数据驱动，覆盖本期全部需求）

`GenericInspectionView`：表行数、列标题、单位、CSV 列**全部从 `InspectionSpec` 动态生成**。

**判断依据**：泡芙与成品鞋的差异 90% 在**检测项列表不同**（直径 / 高度 / 圆度 vs 鞋码 / 鞋面缺陷 / 鞋底厚度），那是**数据**差异而非**控件**差异。所以「默认视图 + 换规格文件」已能满足两个项目。

只有出现「界面**结构**真的不同」的项目（例如成品鞋需要鞋面分区示意图、参考图并排对比），才写第二个实现注册进去。

---

## 八、算法侧与载荷改动

### 8.1 `IPuffAlgorithm` 的改动面

| 项 | 改动 |
| :--- | :--- |
| `PuffResult` | 加 `std::vector<Measurement> measures`（§3.2） |
| `Detect` / `LocateBest` | **签名不变**——测量值随返回值一起出 |
| `SimAlgo` | 填 `measures`：至少产出 `diameter`（由 `SimVision::TargetSpec` 的 `radius` / `depthMm` 与内参换算，现状核实 #11） |
| 未来真实算法 | 自行填其像素语义对应的测量项 |

### 8.2 ⚠ 与 `multi_camera_plan.md` 的字段协调

`multi_camera_plan.md` §5.2 与其现状表 S8 计划给 `DetectionResult` 加 **`cameraId` / `algorithmName`**（用于自动运行页按相机归因）；本方案也要给**同一个结构**加 `cycleIndex`。

**两处改的是同一个结构 ⇒ 必须协调**，否则两次改动会各加各的、互相覆盖，或在热点文件上反复冲突（`src/Logic/SequenceWorker.h`）。

| 实施顺序 | 做法 |
| :--- | :--- |
| **本方案先做**（建议） | 同时带上 `cameraId` / `algorithmName` 两个字段——单相机期取当前活动相机名即可，多相机落地后自然变真值，**不留二次改载荷** |
| 多相机先做 | 本方案随后追加 `cycleIndex` |

⚠ **不推荐两者并行改同一文件**。具体先后由 `doc/implementation_roadmap.md` 裁定（该文目前只覆盖 P0~P6，尚未登记这两篇新方案——见 §11.2 待确认 4）。

### 8.3 `detectionReady` 载荷的最终形态

```cpp
// src/Logic/SequenceWorker.h
struct DetectionResult
{
    CameraFramePtr      frame;
    QVector<PuffResult> results;       // 每个结果自带 measures（§3.2）
    double              threshold = 0.0;
    // ---- 本方案新增 ----
    quint64             cycleIndex = 0;   // 当前轮次（1-based），供 UI 标注「第 N 轮」
    // ---- 若与 multi_camera_plan 同批落地，另加 ----
    // QString          cameraId;
    // QString          algorithmName;
};
```

- `cycleIndex` 由 `SequenceWorker` 直接取 `impl_->cycleIndex`（现状核实 #16），**零成本**。
- ⚠ **判定结果不进载荷**（依 §3.5，判定在 UI 线程做）——载荷只带**原始事实**（帧 + 结果 + 轮次），派生结论由消费者算。这样载荷保持「事实层」，将来第三方要结论时在接口层复用同一个纯函数即可。
- ⚠ **元类型注册**：`DetectionResult` 已 `Q_DECLARE_METATYPE`（`src/Logic/SequenceWorker.h:293`），**加字段无需重新注册**。

---

## 九、改动清单

| 文件 | 改动 |
| :--- | :--- |
| `src/HAL/interfaces/IPuffAlgorithm.h` | 新增 `Measurement`；`PuffResult` 加 `measures` |
| `src/HAL/algorithm/SimAlgo.cpp` | `Detect` 填 `measures`（`diameter`，按内参与 `TargetSpec` 换算） |
| `src/Config/InspectionSpec.h/.cpp`（新增） | `InspectionItem` / `InspectionSpec` / `InspectionSpecStore`（走 `ConfigPaths::resolve`） |
| `src/Logic/Inspection.h`（新增） | `Verdict` / `InspectionItemResult` / `InspectionReport` |
| `src/Logic/InspectionEvaluator.h/.cpp`（新增） | **纯函数**判定器 |
| `src/Logic/SequenceWorker.h/.cpp` | `DetectionResult` 加 `cycleIndex`（+ 与多相机协调的字段，§8.2） |
| `src/UI/inspection/IInspectionView.h`（新增） | 视图接口 + 注册表（`REGISTER_INSPECTION_VIEW`） |
| `src/UI/inspection/GenericInspectionView.h/.cpp`（新增） | 默认数据驱动视图 |
| `src/UI/inspection/InspectionResultPanel.h/.cpp`（新增） | **页签 2 的宿主**：结果图滚动区 + 环形缓存 + 统计导出；判定表由内嵌的 `IInspectionView` 提供（§7.3） |
| `src/UI/AutoRunPage.h/.cpp` | 左侧列改 `QTabWidget`（页签 1 = 现有两格原样搬入）；页签 2 = `InspectionResultPanel`；**渲染闸门下沉一格**（§6.4）+ **页签切换重绘钩子**（§6.5）+ **页签数 ≤ 1 时不建 `QTabWidget`**（§6.6） |
| `src/UI/CMakeLists.txt` | 新增源文件（⚠ `src/UI/` 有独立 CMakeLists） |
| `config/inspection.json`（新增） | 泡芙规格（§4.1） |
| `config/config.json` | 加 `ui.inspectionPanel.show`（bool，默认 `true`，§6.6） |
| **`build_release.bat`** | ⚠ **必须加 `copy /Y "%BUILD%\inspection.json" "%DIST%\"`**——否则部署包缺规格文件（现状核实 #14）。该脚本是**纯 ASCII 约束**（含中文会被 cmd 按 GBK 解析成乱码命令） |
| `doc/config.md` | 登记 `ui.inspectionPanel.show`（含「改配置需重启」的口径） |
| `doc/architecture.md` | 新增 `src/Config/InspectionSpec*`、`src/Logic/Inspection*`、`src/UI/inspection/` |
| `AGENTS.md` | 速查 / 架构节登记新文件、新配置节、`inspection.json` |
| `doc/README.md` | 登记本文 |
| `doc/test/real_machine_plan_phase3.md` | 追加阶段 15 用例（§十） |
| `TEST_RECORD.md` | 实施时逐项追加（**不预留号段**，按当日台账最大号 + 1） |

---

## 十、测试用例规划（阶段 15）

追加到 `doc/test/real_machine_plan_phase3.md` 作**第十批 = 阶段 15「检测结果面板验收」**，编号 15.1 起（**追加末尾、不重排**，与既有批次规则一致）。共 **25 条**。

### A 档 离线（不需硬件、不需 Sim）

| 编号 | 判据 |
| :--- | :--- |
| 15.1 | 判定器**闭区间边界**：`value == min` 与 `value == max` 均判 **OK**；`min − ε` / `max + ε` 判 NG |
| 15.2 | 缺测语义：`required = true` 未测到 → **NG**（文案「未测到」）；`required = false` 未测到 → **跳过** |
| 15.3 | 整体判定：全部非跳过项 OK → 整体 OK；任一项 NG → 整体 NG；**全部跳过 → 整体跳过**（不得显示 OK） |
| 15.4 | 规格解析往返：`inspection.json` 全字段读入一致；`items` 为空数组时不崩溃 |
| 15.5 | 规格文件**缺失 / JSON 非法** ⇒ 回落空规格 + 明确日志，不崩溃，且**整体判跳过**（不得静默判全 OK） |
| 15.6 | **未知 `key` 前向兼容**：算法产出规格里没有的测量项 ⇒ 忽略且不报错 |
| 15.7 | `Measurement` 跨线程传递：`PuffResult.measures` 经 `detectionReady` 到达 UI 后内容逐项一致（元类型注册有效） |
| 15.8 | 导出 CSV 列数 == `3 + 2 × items.size()`；文件头含 UTF-8 BOM（`EF BB BF`） |
| 15.9 | 环形缓冲：连续投 100 轮 ⇒ 缓存**恒为 50**，最旧的被丢弃、最新的在最前 |
| 15.10 | 导出统计口径：**跳过项不进良率分母** |

### B 档 Sim（`SimCamera` / `SimCard`，不需真机）

| 编号 | 判据 |
| :--- | :--- |
| 15.11 | Sim 下跑含「识别」动作的方案 ⇒ 页签 2 收到结果：滚动区出现 1 张、右侧表格行数与规格 `items` 数一致 |
| 15.12 | Sim 的 `diameter` 测量值可用 `SimVision::TargetSpec::radius` + `depthMm` **反算真值对照**（容差按 §3.4 的换算模型给出，记录偏差） |
| 15.13 | **未检出目标**（空 `results`）⇒ 页签 2 **不新增图片卡片**，整体判定显示**跳过**（与「跑过但没检出」语义一致） |
| 15.14 | 循环生产 N 轮 ⇒ 滚动区出现 N 张、轮次编号连续；统计满足 `OK + NG + 跳过 == N` |
| 15.15 | **页签切换不释放相机持有**：开 `ui.autoRunStreamPreview=true` 后切到页签 2 ⇒ 日志中**无** `AutoRunPreview` 的 release；切回页签 1 画面仍在（对照 §6.2） |
| 15.16 | **渲染闸门已下沉**：页签 2 激活时跑一轮检测 ⇒ 页签 1 的标签**不被写入**（对照 P3-4 的既有验证方式，§6.4） |
| 15.17 | **页签切换补绘**：页签 1 激活状态下切回 ⇒ 叠加图**立即**按当前尺寸重绘，不等下一轮检测（§6.5） |
| 15.18 | 页签 2 不可见时投结果 ⇒ **只入缓存不产缩略图**（§5.2） |
| 15.19 | 页签来回切换 ⇒ 缓存与统计**保持**（不重置） |
| 15.20 | `projectType` 改为**未注册值** ⇒ 回落默认通用视图，不崩溃、表格仍按规格渲染 |
| 15.21 | 导出 CSV 用 Excel 打开：**中文列名不乱码**（BOM 实证） |
| 15.22 | **`show` 开关与单页签退化**：① `ui.inspectionPanel.show=false` 重启 ⇒ 无页签 2，且自动运行页布局与改动前**逐像素一致**（不残留页签栏）；② `inspection.json` 缺失 ⇒ 同上；③ 1200×700 最小尺寸下页签栏与既有控件**不折行 / 不裁切 / 不重叠** |

### C 档 真机（`OrbbecCamera` + 真机械臂）

| 编号 | 判据 |
| :--- | :--- |
| 15.23 | 真机连续生产 100 轮：页签 2 内存**不持续增长**（对照 TR-110 / 用例 11.32 的长稳判据） |
| 15.24 | 真机 `diameter` 实测值与量具对照（**须先标定手眼矩阵与内参**）：记录偏差，评估 §3.4 换算模型的精度 |
| 15.25 | 跑满 50 张缓存后继续跑：旧图被回收、UI 无可感知卡顿；自动运行 + 停留在页签 2 共 30 min 无崩溃、无冻结 |

---

## 十一、裁定状态

### 11.1 已裁定 5 项（用户）

见 §1.2。**除 §11.2 的待确认项外，无阻塞实施的遗留项。**

### 11.2 待确认项

| # | 项 | 待确认内容 | 建议 |
| :--- | :--- | :--- | :--- |
| 1 | 环形缓存上限 | 50 张是否合适？ | 50 张 ≈ 25 MB。现场若要更长历史可调大，但须同时评估内存 |
| 2 | 统计口径 | 本期 = **本次会话**（不跨重启）。是否需要跨重启累计？ | 本期**不跨重启**（避免良率被历史数据污染）；如需生产统计，另立需求 |
| 3 | ~~窗口状态落点~~ | **已消除**（2026-10-10）：入口改为页签后不再有对话框，无需持久化窗口位置 / 大小 | — |
| 4 | 与 `multi_camera_plan` 的实施顺序 | 谁先改 `DetectionResult`？ | 建议**本方案先做**并顺带带上 `cameraId` / `algorithmName`（§8.2）；两篇新方案均未登记进 `implementation_roadmap.md`，须补 |
| 5 | 检测项上下限数值 | §4.1 的 `43.0 ~ 47.0 mm` 等为**示例**，非实测 | 由工艺给出后填入 `inspection.json` |
| 6 | `show` 开关是否做成运行时切换 | 本期 = **构造期判定 ⇒ 改配置需重启**（§6.6）。是否需要「配置页勾选框 + 运行时增删页签」？ | 建议**本期不做**——结构级增删要维护当前索引、布局重算与补绘状态机，收益不值这个复杂度 |
| 7 | 页签 2 的左右分栏比例 | 判定表约 340 px 宽是否够用？ | 先按 55 / 45 做，真机上看一眼再调；若要更宽须把控制列下移为底部常驻条（本期不做，§5.1） |

### 11.3 建议实施顺序

| 步 | 内容 | 前置 |
| :--- | :--- | :--- |
| 1 | 数据模型：`Measurement` + `Inspection*` 结构 + **纯函数判定器**（可离线单测） | 无 |
| 2 | `config/inspection.json` + `InspectionSpecStore`（走 `ConfigPaths`） | 第 1 步 |
| 3 | 算法侧：`SimAlgo` 产出 `measures`；`DetectionResult` 加 `cycleIndex` | 第 1、2 步 |
| 4 | 界面：`InspectionResultPanel`（滚动区 + 判定表 + 统计导出 + 环形缓存） | 第 3 步 |
| 5 | 入口：`AutoRunPage` 左侧列改 `QTabWidget`（页签 1 = 现有两格）+ 渲染闸门下沉 + 页签切换补绘钩子 + `ui.inspectionPanel.show` | 第 4 步 |
| 6 | 打包：`build_release.bat` 加 `inspection.json` | 第 2 步（可与 4、5 并行） |
| 7 | 插件式：`IInspectionView` + 注册表 + 默认通用视图（**不阻塞验收**） | 第 4 步 |
| 8 | 阶段 15 用例执行 + `TEST_RECORD.md` 逐项追加 | 第 5、6 步 |

⚠ 改任何**头文件**后须 `-t clean` 全清重建（ninja 依赖追踪在 MSVC 中文版下失效，见 `AGENTS.md`）。

---

## 十二、风险与回退

| 风险 | 影响 | 回退 / 缓解 |
| :--- | :--- | :--- |
| **按帧渲染拖死 UI** | 界面卡顿 / 无响应 | 硬约束「每轮一张」（§5.2）+ 用例 15.18 / 15.24 |
| **环形缓存内存膨胀** | 长稳运行时内存持续增长 | 上限 50 + 存缩略图（§5.2）+ 用例 15.9 / 15.22 |
| **页签 2 激活时仍给隐藏标签渲染** | 静默废掉 P3-4 优化，UI 线程空转 | 渲染闸门下沉一格（§6.4）+ 用例 15.16 |
| **切回页签 1 显示错尺寸旧图** | 观感缺陷，最长 2.4 s 才自愈 | 页签切换补绘钩子（§6.5）+ 用例 15.17 |
| **按钮被关进页签 ⇒ 另一页签点不到「停止」** | **安全**：运行中无法正常中止 | 硬约束「页签只包左侧列」（§5.1）+ 用例 15.22 |
| **`show=false` 时残留页签栏** | 白白吃掉约 35 px 竖向空间 | 页签数 ≤ 1 时不建 `QTabWidget`（§6.6）+ 用例 15.22 |
| **页签栏挤坏 1200×700 布局** | 10 寸屏下折行 / 裁切 | 用例 15.22 |
| **规格文件缺失 / 非法** | 全部判 OK（假合格）——最坏情况 | 回落**空规格** + 整体判**跳过**（绝不显示 OK）+ 用例 15.5 |
| **换算模型失真**（球体 / 倾斜面） | 实测值与真值偏差大 | 明确「Sim 仅供链路验证」（§3.4）；真机标定后用 15.23 评估 |
| **CSV 中文乱码** | 导出不可用 | 强制 UTF-8 BOM + 用例 15.8 / 15.20 |
| **与多相机方案改同一结构冲突** | 互相覆盖 / 编译失败 | 字段一次谈妥（§8.2）+ 实施顺序由 roadmap 裁定 |
| **`inspection.json` 漏进打包** | 部署包缺规格 → 回落空规格（功能静默失效） | 改动清单已列 `build_release.bat`（§九） |

**整体回退**：删除 `config/inspection.json`（或把 `items` 置空、或置 `ui.inspectionPanel.show=false`）⇒ **不创建页签 2**，自动运行页布局回到改动前，**不影响任何既有功能**。`PuffResult.measures` 是**追加字段**，既有消费方零影响；页签 2 是**新增的子控件**，不改动自动运行页的按钮互锁与相机持有逻辑。
