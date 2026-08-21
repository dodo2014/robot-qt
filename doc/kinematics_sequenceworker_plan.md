# 「小脑」运动学与 TCP 核心库重构 + 「大脑」工艺流程执行引擎 — 执行计划

状态：已确认（2026-08-19，与 gemini_qr.md 结论对齐）。
依据：`doc/gemini_qr.md` 正逆解对话结论 + 现有源码核对。
范围：本次仅计划落盘，**尚未开始编码**，等待用户确认后执行。

## 一、核心模型结论（gemini_qr.md 摘要，重构依据）

1. **轴映射（已与 LogicalAxis 一致）**：J1 大臂旋转（伺服）｜J2 小臂旋转（舵机）｜Z 升降｜R 夹爪 Pitch 翻转（舵机）｜夹爪张合｜挤出。
2. **物理模型降维**：L1 用**水平投影 174.35mm**；大臂倾斜只体现为 Z 偏移；R(Pitch) 是垂直面翻转，不参与平面正逆解。
3. **降维决策**：抓取姿态恒垂直朝下、灌装位置固定可示教 → **正逆解退化为 2D 平面三角 + 独立 Z**，无需 Eigen 3D 齐次矩阵；R 直接透传目标角。
4. **Home Offset（机械零点↔逻辑零点）**：`逻辑角度 = 机械角度 - homeOffset`（J1=102°、J2=28°，config 已就绪）。**本次落地**。所有界面只显示逻辑角度与世界坐标。
5. **TCP 与 IK 严格分离**：外层 `ApplyTCPOffset`（抓取朝下时纯平移减法，dx=53/dy=0/dz=-130）→ 内层纯逆解。
6. **4 类标定参数**：本体物理参数（L1/L2/Z0/HomeOffset）＋TCP 偏移＋相机内参＋3D 手眼外参 4×4 矩阵。
7. **数据流**：相机像素+深度 → 内参 → 相机坐标 → 手眼矩阵 → 基座坐标 → TCP 剥离 → IK → 电机。

## 二、现状差距（已核对源码）

| 项 | 现状 | 差距 |
|---|---|---|
| `src/Core/Kinematics.h/.cpp` | 旧 SCARA 模型：`l1_=168.5/l2_=190/l3_=145.3`；`Pose3D`/`JointAngles` 数组；R 按 Roll(yaw) 参与计算 | 改为 `Pose{x,y,z,r}`/`Joints{j1,j2,z,r}`；L1=174.35；R 独立透传；新增 `ApplyTCPOffset` |
| `src/Core/CoordTransform.h/.cpp` | 旧平面模型：`camRotation+offset`；gripOffset 与 TCP 耦合；`yaw=j1+j2+j4`(Roll 语义) | 改手眼 4×4 矩阵(Eigen) + TCP 剥离分离；删旧耦合逻辑 |
| Home Offset | config 已有 `homeOffset`，`HardwareManager` 已读入 `AxisConfig.homePos`，但 `AxisConverter` 换算**未使用** | 逻辑角度机制未落地；回零后目前显示 0°，落地后显示 -102°/-28° |
| `src/Core/Trajectory.*` | 依赖旧 `Pose3D`/`JointAngles` | 适配新结构体 |
| `src/Logic/PickCycleController.*` | 硬编码卡轴号 0/2/3、`servoJ3` 命名；**未被任何 UI 接线（死代码）** | 重构为"视觉抓取单周期"预置模板，走 HardwareManager 门面 |
| `src/Logic/SequenceWorker.*` | 不存在 | 新增（大脑核心） |
| `src/UI/AutoRunPage.*` | 5 按钮全 stub；坐标面板/日志框静态假数据；相机占位框未接流 | 接线大脑 + FK+TCP 实时显示 + `frameReady` 实时画面 |
| `src/UI/ProcessPage.*` | "示教读取" stub | 当前关节 FK 计算 TCP 坐标填充点位 |
| `src/UI/ConfigPage.cpp` 运动学 Tab | L1 输入框默认 285（config 已 174.35）；axisParams 数组(3 项)与 axes 对象冗余 | 与 config 对齐；TCP 偏移(53/0/-130)/手眼矩阵输入框已存在，本次真正接入算法 |

## 三、执行计划

### 阶段 1 — 小脑核心库（无 UI 依赖，先行）

**T1. 重写 `src/Core/Kinematics.h/.cpp`**（核心）

- 新结构体 `Pose{x,y,z,r}`、`Joints{j1,j2,z,r}`；删除旧 `Pose3D`/`JointAngles`。
- `void SetParams(double l1xy, double l2, double z0)`（参数由上层从 config 读取喂入）。
- `Pose ApplyTCPOffset(const Pose& target, double dx, double dy, double dz)`：TCP→腕点（抓取朝下时平移减法）。
- `bool Inverse(const Pose& wrist, Joints& out, bool elbowUp=true)`：余弦定理解析法；超臂展 `sqrt(x²+y²)>L1+L2` 返回 false + `SPDLOG_WARN`；原点奇点；逻辑限位校验；`out.r = wrist.r` 透传。
- `Pose Forward(const Joints& joints, bool tcp=false)`：法兰/TCP 坐标。
- 双解就近选择：`InverseSmart(wrist, out, currentJ2)`（elbow_up/down + 按当前 J2 就近，避免大甩臂）。

**T2. 重构 `src/Core/CoordTransform.h/.cpp`**

- Eigen 手眼矩阵（数据源 `config.tcpCalibration.handEyeMatrix`，已存在）。
- `Pose PixelToRobot(u,v,depth)`：内参 → 相机坐标 → 手眼 → 基座。
- `Pose CameraToRobot(xc,yc,zc)`：手眼一步（`PuffResult` 输出相机系物理坐标，直接可用）。
- 删除旧 `camRotation+gripOffset` 平面逻辑。

**T3. Home Offset 落地**

- Card 轴：`AxisConverter::ToPulse` 加 offset、`ToPhysical` 减 offset（逻辑=机械-offset，`AxisConfig.homePos` 已注入）。
- Servo 轴：`HardwareManager` MoveAbs/MoveJog/JogTick/GetPosition 的 Servo 分支同样做 offset 换算（逻辑+offset 下发、ReadAngle-offset 回读）——**Home Offset 不只 AxisConverter，Servo 分支必须同步**。
- 回归核对：MoveAbs 下发、MoveJog、GetPosition、PollTick 回读、软限位（逻辑坐标语义不变）、inverted 方向轴、夹爪/挤出 linear 轴（offset=0 无影响）。

**T4. `src/Core/Trajectory.cpp` 适配新结构体。**

### 阶段 2 — 大脑执行引擎（新增）

**T5. 新增 `src/Logic/SequenceWorker.h/.cpp`**（QObject）

- 可移入 `QThread`，禁止直接操作 UI；`QWaitCondition+QMutex` 单步模式。
- `RunSequence(const SchemeData&)` 遍历 `actions`，switch-case 分发：
  - Move：逐点 `ApplyTCPOffset → Inverse → HardwareManager::MoveAbs(J1/J2/Z/R)`；`speedPercent` 换算速度；逐点到位等待（`IsAxisBusy`/`axisMoveFinished`）。
  - Vision：真实路径 `Detect → CameraToRobot → 目标`；无相机时模拟（延时+日志）。
  - Extrude：挤出/回抽 `MoveAbs(Extruder)`。
  - Delay：事件循环等待（不阻塞信号）。
  - Gripper：`MoveAbs(Gripper, open/close)`。
- 中断：`Stop()`/`EmergencyStop()` 置 cancel 标志，当前动作安全停止后跳出循环。
- 信号：`actionStarted(int index, QString name)`、`schemeFinished`、`interrupted`、`errorOccurred`、`logMessage`。
- 入口使能门禁 `IsGlobalEnabled()`（复用现有安全门禁）。

**T6. 重构 `PickCycleController` 为预置模板**

- 删除硬编码卡轴号 0/2/3，全部走 `HardwareManager::MoveAbs(LogicalAxis,...)`。
- 改用新 Kinematics：`Detect → CameraToRobot → ApplyTCPOffset → Inverse`。
- 保留使能门禁；作为"视觉抓取单周期"模板供 SequenceWorker/后续复用。

### 阶段 3 — UI 接线与配置联动

**T7. `MainWindow`**：创建 `SequenceWorker`+`QThread`（成员持有、析构安全停止）；顶栏"手动/自动"切换联动 worker 可用性。

**T8. `AutoRunPage`**：接线 启动/复位/停止/初始化/急停 5 按钮；坐标面板改 FK+TCP 实时显示（`stateUpdated` → `Forward(tcp=true)`）；日志框接 `logMessage`；两相机占位框接 `frameReady` 实时画面（复用 VisionTestPage 渲染）。

**T9. `ProcessPage`**："示教读取" → 当前关节 `Forward(tcp=true)` 填充点位。

**T10. `ConfigPage`**：运动学 Tab 默认值与 config 对齐（174.35/166.86/0）；axisParams 冗余数组按需清理；九点标定保留 stub（相机未装）。

### 阶段 4 — 验证与文档

**T11.** 按 `doc/kinematics_sequenceworker_test.md` 执行全量测试 + `TEST_RECORD.md` 记账。
**T12.** 更新 `doc/config.md`、`AGENTS.md`、`doc/worklog/`；Debug + Release 双编译验证。

## 四、对现有功能的影响分析

| 现有功能 | 影响 | 说明 |
|---|---|---|
| **手动控制页位置显示/Go/JOG** | **行为变更（预期）** | Home Offset 落地后：回零后 J1 显示 **-102°**、J2 显示 **-28°**（逻辑角度）；Go 目标为逻辑角度；点动方向不变。真机已验证的"回零归零"行为改为"回零显示逻辑偏移角" |
| **软限位（手动页提示/拦截）** | 语义不变，需核对 | 判定全用逻辑坐标（GetLimitMin/Max 与 GetPosition 同为逻辑），机制不变。**注意**：J1 当前 config `limitMin=0/limitMax=110` 为逻辑范围，回零点为逻辑 -102（不在范围内），回零后不能向负方向点动——需现场核对限位范围合理性（见测试方案 §4 用例 T03） |
| **回零（HomeAxis）** | 流程不变，显示变更 | 撞限→计数器归 0（机械 0）→ 显示逻辑 = 0−offset；`homingActive_` 门禁、`homeMaxDis` 等不受影响；`PollTick` 回读经 offset 修正后软限位判断一致 |
| **运动学参数 Tab** | 小改 | L1/L2/Z0 默认值对齐 config（174.35/166.86/0） |
| **TCP 与标定 Tab** | 参数被真正使用 | toolOffset(53/0/-130) 与 handEyeMatrix 首次接入 Kinematics/CoordTransform（此前仅存储） |
| **视觉检测页** | 基本无影响 | `PuffResult` 相机系输出不变，手眼转换发生在下游 SequenceWorker/模板层 |
| **手动页状态灯/报警/急停/断使能** | 无影响 | 不涉及本次改动面 |
| **PickCycleController** | 重构 | 目前死代码（未被接线），重构后作为模板，无对外影响 |
| **Trajectory** | 内部适配 | 仅 Core 内部使用，无 UI 依赖 |
| **XRServo/BoPaiCard 协议层** | 无影响 | 本次不动协议；Home Offset 在上层换算 |
| **编译** | Pose3D/JointAngles 删除 | 引用方（Trajectory、PickCycleController）须同步迁移，避免编译断裂 |

## 五、风险与回归点

1. **Home Offset 是最大行为变更**：T3 落地后手动页显示/回零/Go 全变为逻辑角度，需完整回归；Servo 分支（J2/R）的 offset 极易遗漏。
2. **J1 软限位范围合理性**：逻辑范围 [0,110] 与回零逻辑点 -102 的配置一致性需现场核对（可能需调整 limitMin）。
3. **Kinematics 全面迁移**：`Trajectory`、`PickCycleController` 是仅有的引用方，T1 后立即改这两处，避免编译断裂。
4. **SequenceWorker 线程安全**：不得碰 UI；中断须安全停止当前动作后再跳循环；到位等待不得依赖 UI 线程。

## 六、验收标准

1. 仿真模式：新建方案（Move→Gripper→Delay→Extrude→Move），自动运行日志逐步打印、坐标正确、单步/停止/急停均可中断。
2. IK 正逆解往返误差 < 1e-6；超臂展目标返回 false 且只记日志不崩溃。
3. Home Offset 生效：回零后 J1=-102°、J2=-28°；手动 Go 以逻辑角度下发。
4. Debug + Release 编译通过；真机（Bopai+XRServo）冒烟无崩溃。
5. `TEST_RECORD.md` 全量记账。

## 七、参考

- `doc/gemini_qr.md`：正逆解/TCP/坐标系/Home Offset 对话结论（权威依据）。
- `doc/kinematics_sequenceworker_test.md`：测试方案（影响回归 + 新功能验证）。
- `doc/开发文档.md`：页面功能需求（手动/自动互锁、示教、单步执行）。
