# 配置数据归属与分文件方案

> **需求来源**：用户 2026-09-18 提问「相机、视觉、算法的配置数据，是否需要分文件存储？」
> **本文性质**：**方案 → 🟡 已实施待真机**（2026-09-20 完成 §五 C1–C6 + §七 原子写，台账 **TR-097**；§六 分文件**未实施**，触发条件未满足）。文中「现状」事实的核实时点为 2026-09-18；**§三 3.3 / 3.4 / 3.5 描述的是实施前的行为**，实施后的现状以 `AGENTS.md` 与源码为准。
> **关联文档**：`doc/config.md`（`config.json` schema，§四 C7 对其有一处修正）、`doc/process.md`（`process.json` schema）、`doc/camera_integration_plan.md`（相机接入，§四 C1 / C2 与其 M1 相关）、`doc/vision_test_page_redesign_plan.md`（视觉页重构，§3.3 是其 V5 的完整证据）。

## 一、问题与结论

**问题**：相机、视觉、算法三个域的配置数据，是否需要分文件存储？

**结论：现在不分。** 理由不是「分文件不好」，而是**分文件解决不了当前的主要问题**——「算法配置」这条链路目前没有闭环：有配置项、有写入口、有界面、有日志，但没有任何一处真正用它控制检测行为。分文件只是把一个不生效的链路切成两个文件，并把「这个配置项在哪个文件」变成新的认知负担。

**将来该分**，触发条件与分法见 §六。

## 二、判断标准

分文件只有一个正当理由：**不同配置的「所有者、变更频率、交付方式」不同**。

按此标准衡量三个域：

| 域 | 绑定对象 | 变更频率 | 交付方式 | 该不该独立成文件 |
| :--- | :--- | :--- | :--- | :--- |
| 相机 | 现场设备（更换相机或工位才变） | 极低 | 随现场走 | 有理由 |
| 视觉阈值 | 产品与光照（调参期频繁修改） | 中 | 随产品走 | 有理由 |
| 算法选型 | 软件版本（随代码走） | 极低 | 随程序走 | 有理由 |

三个域确实各不相同，因此**方向上是该分的**。但分文件的前提是这些配置**真的有消费点**——现行状态不满足，见 §三。

## 三、现状核实

### 3.1 `config.json` 的规模与域清单

`config/config.json` 共 **254 行 / 6699 字节 / 187 个叶值 (Leaf Value)**，顶层 9 个域：

| 域 | 叶值数 | 说明 |
| :--- | ---: | :--- |
| `axes` | 117 | 6 个逻辑轴（`Axis_J1` / `Axis_J2` / `Axis_Z` / `Axis_R` / `Axis_Gripper` / `Axis_Extruder`），每轴 12~21 项 |
| `kinematics` | 21 | 连杆参数 `links` / `axisParams` / 安全位 `safePos` |
| `tcpCalibration` | 19 | 含 `handEyeMatrix` 的 16 个元素；**当前仍为占位值** |
| `communication` | 16 | 运动控制卡 / 舵机 / 相机 SN / 舵机列表 |
| `simulation` | 5 | 硬件实现选型 |
| `vision` | 4 | `confidenceThreshold` / `depthZMin` / `depthZMax` / `exposure` |
| `production` | 3 | 循环生产设置 |
| `version` / `$schema` | 各 1 | 版本号与描述字段 |

**观察**：`axes` 一个域占全文件叶值的 62.6%，而 `vision` 只有 4 项。按「域」分文件在数量上是不均衡的。

### 3.2 写入口全清单

`ConfigManager` 的写入统一走 `ConfigManager::instance().set(path, value)`。全库写入口如下：

| 配置域 | 写入口 | 处数 |
| :--- | :--- | ---: |
| `axes.*`（除 `jogSpeed`） | `ConfigPage.cpp:1144-1222`（标签页「电控与映射」） | 1 |
| `axes.*.jogSpeed` | `ConfigPage.cpp`（同上）+ `ManualControlPage.cpp:366`（手动页点动速度，经 `HardwareManager::SetJogSpeed` → `AxisConfigService.cpp:69-79`） | **2** |
| `communication.motionCard.*` | `ConfigPage.cpp:267,272,277` | 1 |
| `communication.servo.*` | `ConfigPage.cpp:303,318` | 1 |
| `communication.camera.sn` | `ConfigPage.cpp:335` | 1（**无读点**，见 §3.4） |
| `kinematics.*` | `ConfigPage.cpp:383-506`（标签页「运动学参数」） | 1 |
| `kinematics.safePos.*` | `ConfigPage.cpp:439` + `ManualControlPage.cpp:660-664`（示教安全位） | **2** |
| `tcpCalibration.*` | `ConfigPage.cpp:634`（标签页「TCP与标定」） | 1 |
| `vision.confidenceThreshold` | `ConfigPage.cpp:564` + `VisionTestPage.cpp:330` | **2** |
| `vision.depthZMin` | `ConfigPage.cpp:583` + `VisionTestPage.cpp:337` | **2** |
| `vision.depthZMax` | `ConfigPage.cpp:592` + `VisionTestPage.cpp:344` | **2** |
| `vision.exposure` | `ConfigPage.cpp` + `VisionTestPage.cpp:351` | **2**（已裁定删除） |
| `production.loop.*` | `AutoRunPage.cpp:719-721` | 1 |
| `simulation.*` | **无写入口** | **0** |

**三类问题**：

1. **同一配置项有两个写入口**：`axes.*.jogSpeed`、`kinematics.safePos.*`、`vision.*` 四组。两个入口是否同步刷新界面，现行无约定。
2. **`simulation.*` 无写入口**：只能手改 `config.json`。`doc/config.md` 将其映射到标签页「硬件类型选择 (仿真/真机)」，而 `ConfigPage.cpp:177-181` 实际只有 5 个标签页（通信与连接 / 运动学参数 / 视觉与工艺参数 / TCP与标定 / 电控与映射），**该标签页不存在**（见 §四 C7）。
3. **`ConfigPage.cpp` 单文件 1261 行**，5 个标签页的构造与绑定全在其中。

### 3.3 视觉与算法配置的读写关系

`vision` 段 4 个配置项、动作级算法参数，各自的读点与**生效状态**如下：

| 配置项 | 读点 | 读取时机 | 生效状态 |
| :--- | :--- | :--- | :--- |
| `vision.depthZMin` / `depthZMax` | `SimAlgo.cpp:30-32`，在 `:92` 做深度筛选 | 每次检测实时读 | **生效** |
| `vision.confidenceThreshold` | `VisionTestPage.cpp:645`（叠加框绘制阈值） | 每次渲染实时读 | **仅影响显示** |
| `vision.confidenceThreshold` | `HardwareManager.cpp:183`，喂给 `algo->LoadConfig()` | 启动时读一次 | **未生效** |
| `action.threshold` | `SequenceWorker.cpp:889` | 每次执行 | **仅打印日志** |
| `action.templateName` | 无读点 | — | **无消费点** |
| `action.visionType` | `SequenceWorker.cpp:886` | 每次执行 | **仅打印日志** |

**「未生效」的原因**：`SimAlgo::LoadConfig`（`SimAlgo.cpp:122-126`）只把 json 字符串存入成员 `config_`（`:124`）并打印字节数（`:125`），**从不解析**。全库只有 `SimAlgo` 实现了该接口。

`SimAlgo` 也从不做置信度筛选——`confidence` 只在 `:101` 计算、在 `:114-115` 用于 `LocateBest` 取最大值，没有任何阈值比较。

**由此产生的一个设计问题**：同一个概念「算法阈值」被拆在两个文件、两个粒度上——全局的 `vision.confidenceThreshold`（`config.json`）与动作级的 `action.threshold`（`process.json` 的 `ActionData`）。**谁优先，现行没有定义。**

### 3.4 相机配置：读写链路完全断开

相机设备标识涉及两个键，二者互不相通：

| 键 | 写入口 | 读点 | 结果 |
| :--- | :--- | :--- | :--- |
| `communication.camera.sn` | `ConfigPage.cpp:335`（标签页「通信与连接」输入框） | **无** | 值被写入文件后**无人读取** |
| `simulation.cameraDeviceId` | **无**（该键在 `config.json` 中不存在） | `CameraManager.cpp:42-43`（`CameraManager::Open` 传给 `camera_->Open`） | 永远走硬编码默认值 `"CAM-SIM-001"` |

另有一处：`VisionTestPage.cpp:462` 在 `OnOpenCamera` 内无条件 `deviceIdEdit_->setText("CAM-SIM-001")`，覆盖用户在界面填入的值；且该界面值本身也不参与打开相机（`HardwareManager::CameraOpen` 的签名不含设备标识，设备标识只能由 `CameraManager::Open` 从配置读取）。

**即：用户在配置页填 SN → 存入 `communication.camera.sn` → 无读点；用户在视觉页填 SN → 被覆盖 → 也不参与；实际打开相机时读的是另一个不存在的键。三个环节没有一处连通。** 接入真机前必须先修通（`camera_integration_plan.md` 的 M1 / M2 已记录，本文补充「两个键互不相通」这一层）。

### 3.5 配置文件查找路径的三套实现

| 位置 | 函数 | 候选路径数 | 优先级逻辑 |
| :--- | :--- | ---: | :--- |
| `HardwareManager.cpp:29-50` | `EnsureConfigLoaded()` | 3 | 源码树 → 程序目录 → 程序目录上级 |
| `ConfigPage.cpp:93-114` | `ensureConfigLoaded()` | 3 | **与上一行逐字重复**（注释自述「与 ConfigPage 相同候选路径」） |
| `ProcessManager.cpp:13-20` | `processJsonPath()` | 2 | **源码树优先，但候选集不同** |

另：`ProcessManager::instance().load()` 被调用**两次**（`MainWindow.cpp:226` 与 `ProcessPage.cpp:78`），重复加载可能覆盖尚未保存的修改。

### 3.6 `ConfigManager` 的写语义

| 项 | 现状 | 影响 |
| :--- | :--- | :--- |
| 实例形态 | 单例，持有单个 `root_` 与单个 `filePath_`（`ConfigManager.h:47-49`） | 单文档模型，无法直接承载多文件 |
| `load()` | 整体替换 `root_`（`ConfigManager.cpp:37-59`） | 多文件需要定义加载顺序与覆盖规则，现无此机制 |
| `save()` | 整文件 `dump(4)`（`:74`）+ `Truncate` 打开（`:69`） | **非原子写**，见 §七 |
| 保存触发 | `set()` → `scheduleSave()` → 300 ms 去抖定时器（`:33-36,88-93`） | 连续修改合并为一次写入，行为正确 |

## 四、须修正项

| # | 问题 | 处理 |
| :--- | :--- | :--- |
| **C1** | `SimAlgo::LoadConfig` 是空实现（`SimAlgo.cpp:122-126`），`HardwareManager.cpp:181-185` 喂入的 `confidenceThreshold` 是死数据 | 二选一：**让 `LoadConfig` 真正解析并生效**，或**删除该通路并明确「本版不生效」**。不应保留一条看起来在工作、实际不工作的通路 |
| **C2** | 相机设备标识两个键互不相通（§3.4） | 统一为单一键（建议保留 `communication.camera.sn`，删除 `simulation.cameraDeviceId` 的读取），并补上读点；同时删除 `VisionTestPage.cpp:462` 的覆盖行 |
| **C3** | 同一配置项有两个写入口（`axes.*.jogSpeed`、`kinematics.safePos.*`、`vision.*` 四组） | 定为一处主写入口，另一处改为「同步刷新」而非独立写入；或明确两处都合法并保证双向同步 |
| **C4** | 不存在的配置项被读取，静默走硬编码默认值：`vision.frameWidth` / `frameHeight` / `frameFps`（`HardwareManager.cpp:193-195`）、`simulation.cameraDeviceId`（`CameraManager.cpp:43`） | 补入 `config.json`，或删除读取点改用既有键 |
| **C5** | 配置查找路径三套实现（§3.5） | **抽出唯一的路径解析函数**（放 `src/Config/`），三处调用点统一。此项**不分文件也应做**，且是分文件的前置条件 |
| **C6** | 全局与动作级算法参数的优先级未定义（§3.3） | 定为**动作级覆盖全局，全局作为默认值**，写入 `doc/process.md` |
| **C7** | `doc/config.md:12` 将 `simulation` 映射到标签页「硬件类型选择 (仿真/真机)」，该标签页不存在（`ConfigPage.cpp:177-181` 实际 5 个标签页） | 修正 `doc/config.md`：标注 `simulation.*` 当前**只能手改配置文件，界面无入口** |

> **实施状态（2026-09-20，TR-097）**：**C1–C7 全部完成**。C7 为文档项（`doc/config.md` 已标注 `simulation.*` 无界面入口，并同步复核了读点清单）。实施过程中的**三项额外发现**（`ProcessManager::save()` 同样非原子写、页面构造早于配置加载、`ProcessManager::load()` 双调用点）与一项**行为变更**（`confidenceThreshold` 由「只显示」变为「真正过滤」）见 `TEST_RECORD.md` TR-097 备注。

> **实施状态（2026-09-20，TR-097）**：**C1–C7 全部完成**。C7 为文档项（`doc/config.md` 已标注 `simulation.*` 无界面入口，并同步复核了读点清单）。实施过程中的**三项额外发现**（`ProcessManager::save()` 同样非原子写、页面构造早于配置加载、`ProcessManager::load()` 双调用点）与一项**行为变更**（`confidenceThreshold` 由「只显示」变为「真正过滤」）见 `TEST_RECORD.md` TR-097 备注。

## 五、第一步：现在做（不分文件）

| # | 事项 | 对应修正项 | 状态 |
| :--- | :--- | :--- | :--- |
| 1 | 处理 `SimAlgo::LoadConfig` 这条未生效的配置通路 | C1 | ✅ 2026-09-20 |
| 2 | 统一相机设备标识为单一键，补上读点 | C2 | ✅ 2026-09-20 |
| 3 | 一个配置项保留一个主写入口 | C3 | ✅ 2026-09-20（定为「两处都合法 + 双向同步」） |
| 4 | 补全或删除不存在的配置项 | C4 | ✅ 2026-09-20（**删除**幻影读点，未补键） |
| 5 | 抽出唯一的配置路径解析函数 | C5 | ✅ 2026-09-20（`src/Config/ConfigPaths.h/.cpp`） |
| 6 | 定义全局与动作级的优先级，写入 `doc/process.md` | C6 | ✅ 2026-09-20 |
| — | **§七 非原子写**（优先级高于分文件） | §七 | ✅ 2026-09-20（`ConfigManager` + **`ProcessManager`** 两处改 `QSaveFile`） |

这 6 项全部与「是否分文件」无关，且是分文件的前置条件。**先把它们做完，再评估分文件。**
—— **已全部完成（2026-09-20，TR-097）**；分文件的触发条件（§六）**仍未满足**。

## 六、第二步：分文件的触发条件与分法

### 6.1 触发条件

满足其一即启动：

| 条件 | 判据 | 依据 |
| :--- | :--- | :--- |
| **A. 交付形态变化** | 需要部署到第二台机器或第二个现场 | 分文件的主要收益是「更换现场只拷贝一份文件」，此时收益才兑现 |
| **B. 配置规模突破** | `config.json` 超过约 400 行，或某域叶值超过 20 个 | 当前 254 行 / 187 叶值，尚未达到；`axes` 域 117 叶值但属机器个体标定，不宜按域拆分 |

### 6.2 分法：按生命周期分，不按模块分

```
config/
├─ machine.json    轴参数 / 连杆参数 / TCP 标定     随机器走（出厂标定后基本不变）
├─ site.json       通信地址 / 串口 / 相机 SN        随现场走
├─ vision.json     相机流参数 / 阈值 / 模板路径      随产品与光照走（调参期频繁修改）
└─ process.json    工艺方案（现状不变）             随产品工艺走
```

**为什么按生命周期 (Lifecycle) 分而不按模块分**：按模块分（相机 / 视觉 / 算法）会导致「更换一个现场需要修改三个文件」，使分文件的收益被抵消。需要划分的是「谁修改、多久修改、拷贝给谁」，不是「代码里哪个模块读取」。

### 6.3 前置条件与技术代价

| 项 | 说明 |
| :--- | :--- |
| 前置：C5 必须已完成 | 路径解析必须先收敛为一个函数，否则分文件后会出现第四套 |
| `ConfigManager` 需要改造 | 当前是单例 + 单 `root_` + 单 `filePath_`（`ConfigManager.h:47-49`）。分文件需改为多文档模型，或按域复制多个管理器 |
| 已有先例不统一 | `ProcessManager` 是「独立文件 + 独立管理器」的先例，但它不是 `QObject`，`load()`（`ProcessManager.cpp:55`）/ `save()`（`:169`）是另一套手写实现，不复用 `ConfigManager`。分文件前应先决定是否统一这套范式 |
| 需定义加载顺序与覆盖规则 | 多文件之间若有同键，需明确优先级；现无此机制 |
| 需定义缺文件行为 | 某个文件缺失时应回落到默认值还是报错，需明确 |

## 七、另需说明：非原子写（独立缺陷）

`ConfigManager::save()`（`ConfigManager.cpp:62`）直接以 `QIODevice::WriteOnly | QIODevice::Truncate` 打开目标文件（`:69`）后写入 `dump(4)`（`:74`），**没有「写临时文件 + 改名」这一步**。

**后果**：写入过程中进程崩溃或掉电，`config.json` 会处于「已截断但未写完」状态，**整个配置文件丢失**（`load()` 遇到解析失败时会把 `root_` 重置为空对象，`:53-56`，即全部配置回落到默认值）。

**修法**：改为原子写 (Atomic Write)——写入同目录临时文件 → `flush` + 关闭 → 用改名覆盖目标文件。

**优先级**：此项与「是否分文件」无关，且后果比「分不分文件」严重，**建议优先于本文其他事项处理**。

## 八、验收与台账

- **已实施（2026-09-20，TR-097）**。§五 6 项 + §七 原子写全部落地；Debug + Release 双编译 `EXITCODE=0` + Sim 冒烟通过（判据 = 当日日志含 `Initialize complete`）。
- **仍未做**：真机枚举相机（C2 的最终判据，须接真机）；`confidenceThreshold` **过滤动作**的实跑观测（需在视觉检测页点一次「单次检测」看 `filtered N of M` 日志）。
- 涉及配置 schema 变更的项（C2 / C4 / C6）已同步 `doc/config.md` 与 `doc/process.md`；`AGENTS.md` 的 Config / ConfigManager / ProcessManager 三节已同步。
- §六 的分文件方案**暂不实施**，待触发条件满足后另开实施计划。
