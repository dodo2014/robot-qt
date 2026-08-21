# 真机联调计划（阶段 2：大脑+小脑全链路）— 2026-08-25 起

状态：**待执行**（代码部分 T1-T12 已全部完成并编译通过，下周开始真机联调）。
配套：`doc/kinematics_sequenceworker_plan.md`（执行计划，代码已完成）、`doc/kinematics_sequenceworker_test.md`（测试方案 H 组）、`doc/test/card_axis_test.md`（卡轴手动测试）、`doc/test/homing_debug_report.md`（回零调试）。

## 一、目标

在真机（BoPai 运动卡 + XR 舵机）上验证「小脑」（Home Offset 逻辑角度 + Kinematics/TCP 内化）与「大脑」（SequenceWorker 流程引擎 + AutoRunPage UI 接线）全链路正确性，覆盖从手动单轴到自动方案执行的完整闭环。

## 二、硬件/接线与配置现状（已确认）

| 逻辑轴 | 硬件 | 卡 axis / 舵机 id | 换算参数 | 状态 |
|---|---|---|---|---|
| J1 大臂 | 步进+谐波1:100 | axis1 / home1 / portId=0 | 25600 Pulse/rev, gear 0.01 → **7111.11 脉冲/度** | ✅ 已真机验证（换算/方向/回零/软限位/Go），**Home Offset 落地后待复测** |
| J2 小臂 | XR 舵机 | id=0（portId=0） | 0-180°，minPulse 500/maxPulse 2500 | ⚠️ FashionStar 协议重写后**待复测**；offset=28 待验证 |
| Z 升降 | 步进+皮带0.5+丝杆5mm | axis2 / home2 / portId=1 | 25600, gear 0.5, lead 5 → **10240 脉冲/mm** | ⚠️ **`calibrationPending=true` 待标定** |
| R 翻转 | XR 舵机 | id=1（portId=1） | 0-180°，minPulse 500/maxPulse 2500 | ⚠️ 待复测 |
| 夹爪 | 步进丝杆 lead2 | axis4 / home4 / portId=3 | 40000 Pulse/rev（XINJE DP3L1-224 拨码全 OFF） | ✅ 已标定（calibPending=false），软限位 [-5,0] 已测 |
| 挤出 | 步进 | axis3（**未接电机**） | 32000, gear 1, lead 10 | ❌ 不测（无电机） |

**Home Offset 现状**：J1 offset=102、J2 offset=28、其余 0。逻辑限位 J1=[-102,8]、J2=[-28,152]、Z=[0,195]、夹爪=[-5,0]、R=[0,180]。
**网络**：pcIp=192.168.0.100、卡 192.168.0.1、port 60000；**必须关闭 wifi** 才能 `MC_Open`。
**config 注意**：`simulation.enabled=true` 但 `motionCardType=Bopai`、`servoType=XRServo`——**确认 enabled 语义**（若 enabled=true 走仿真分支则需改 false，见阶段 0）。

## 三、执行顺序（每阶段验收不通过即停）

### 阶段 0：前置准备（0.5 天）

1. **config 核对**：确认 `simulation.enabled` 与 type 组合能正确走真机（对照 `HardwareManager::Initialize` 分支）；必要时改 `enabled=false`。
2. **接线核对**：卡网线接 PC、J1/Z/夹爪接 axis1/2/4、J2/R 共连 COM3、轴 home 端子接 home1/2/4。
3. **网络**：关 wifi → `ipconfig` 确认 PC 网卡 IP=192.168.0.x → `ping 192.168.0.1` 通。
4. **重建**：改 config 后重新 build（POST_BUILD 复制 config 到输出目录）或手动覆盖输出目录 config.json。
5. **安全**：手放急停旁；机械无干涉（J1 臂范围、Z 上下、夹爪行程）。
6. **日志**：`log/creampuff.log`，关注 HardwareManager/BoPaiCard/XRServo 连接与换算日志。

### 阶段 1：连接 + 使能 + Home Offset 首验（1 天）

| # | 动作 | 期望 |
|---|---|---|
| 1.1 | 启动 → 手动页使能 | J1/Z/夹爪状态点绿无 alarm；J2/R 舵机已连接；日志无 Connect failed |
| 1.2 | 位置显示 | 全部显示 0（未回零前逻辑=机械，未回零仍 0） |
| 1.3 | **J1 回零** | 机械归 0 → **显示 -102°**（Home Offset 生效，H01） |
| 1.4 | **J2 回零** | 归 0 → **显示 -28°**（H01，Servo 分支 offset 验证） |
| 1.5 | R 回零 | 显示 0（offset=0 无偏移） |

> **阶段 1 是最大行为变更验证**。回零显示逻辑偏移角（-102/-28）正确即 Home Offset 全链路通过。若 J1 回零显示 0 → 检查 HardwareManager 回读减 offset 分支。

### 阶段 2：单轴点动 + Go 逻辑坐标验证（1 天）

| # | 动作 | 期望 |
|---|---|---|
| 2.1 | J1 点动 +（逻辑方向） | 显示向 + 递增；撞 +8 自动停、状态点橙、底部提示；反向离开放行（H04/H05） |
| 2.2 | J1 Go 逻辑 5° | 下发机械 107°（5+102）→ 回读显示 5°（H02） |
| 2.3 | J1 Go 越界（如 9°） | 拒绝下发 + 提示（H08 限位） |
| 2.4 | **J2 舵机点动 +/‑** | 无反向、无顿挫、目标=逻辑+28 下发；遥测显示逻辑角（H03） |
| 2.5 | **J2 Go 逻辑 5°** | 下发机械 33° → 回读 5°（H03） |
| 2.6 | R 点动/Go | offset=0，行为与旧版一致 |
| 2.7 | Z/夹爪点动 | linear 轴 offset=0，方向正确（H06） |

### 阶段 3：Z 标定（0.5-1 天，`calibrationPending=true` 处理）

| # | 动作 | 期望 |
|---|---|---|
| 3.1 | Z 低速点动 1mm/s × 5s | 理论 5mm；实测位移 d；**每圈脉冲 = 25600 × 5 / d** 复核（皮带 0.5+丝杆5 → 理论 10240 脉冲/mm，实测误差<2% 即通过，无需改配置） |
| 3.2 | Z 行程实测 | 量实际 Z 行程 → 修正 `limitMin/limitMax`（当前 [0,195] 目测值） |
| 3.3 | Z 软限位 | 点动撞边界自动停 + 提示；越界 Go 拒绝 |
| 3.4 | 标定完成 → `calibrationPending` 置 false（改 config） | 记录每圈脉冲实测值 |

> **Z 无 home 参数**（homeDir/homeSns/homeMaxDis 空）。若需回零：先确认 Z 是否有 Home 开关，无则评估是否需软件回零（参考 homing_debug_report §5.3 新轴 Checklist）。

### 阶段 4：回零完整回归 + 停止/急停（0.5-1 天）

| # | 动作 | 期望 |
|---|---|---|
| 4.1 | 全轴一键回零 | J1→-102、J2→-28、R→0、Z/夹爪按各自零位；回零中禁止点动有提示 |
| 4.2 | 点动中按停止 | 卡轴减速停、舵机保持锁力不垂落（CMD24 停止） |
| 4.3 | 运动中急停 | 卡轴全 halt + 舵机失力可拨动；状态点告警/失能；复位可重新使能 |
| 4.4 | 急停后卡无残留运动 | 位置不再变化 |
| 4.5 | **软件退出断使能** | 正常关窗口 → 舵机 Damping 松力（aboutToQuit）；直接关 cmd → ConsoleSignalHandler → ShutdownHalt |

### 阶段 5：SequenceWorker 真机全流程（1 天，**本轮核心**）

**前置**：手动页使能 + 一键回零（J1/J2/R；Z 若回零未通则用当前位，方案点位避开）。在 ProcessPage 建方案。

| # | 方案（按可用轴拼装） | 期望 |
|---|---|---|
| 5.1 | Move（示教读取 2-3 点，speedPercent=50%） | 逐点 InverseSmart → MoveAbs(J2→R→J1→Z) → 到位等待；日志逐步 actionStarted；无大甩臂（就近选解） |
| 5.2 | 加 Gripper 开/合动作 | 夹爪按 GetLimitMax/Min 开合到位 |
| 5.3 | 加 Delay 1s | 等待 1s 不卡 UI |
| 5.4 | （可选）加 Vision | 真实相机未装 → 走模拟延时路径（无相机降级，验证不崩溃） |
| 5.5 | 方案完成后 schemeFinished | 状态「✅ 完成」、启动按钮恢复 |
| 5.6 | 运行中 Stop | 当前动作安全停止 + interrupted + 保持使能 |
| 5.7 | 运行中急停 | EmergencyStop + 断使能；需重新手动使能 |
| 5.8 | 未使能时启动 | 拒绝 + 提示（门禁） |
| 5.9 | **运行中编辑运动学参数** | ReloadFromConfig 被 running 门禁跳过（不竞争）；下次启动用新参数（D3 验证） |
| 5.10 | **运行中关闭窗口** | 进程不挂死、10s 内退出（ShutdownWorker，D4 验证） |

### 阶段 6：AutoRunPage UI 接线真机（0.5-1 天）

| # | 动作 | 期望 |
|---|---|---|
| 6.1 | 方案下拉 | 显示 ProcessPage 建好的方案（非"（无方案）"）；ProcessPage 增删后切回刷新（D1 验证） |
| 6.2 | 启动/复位/停止/初始化/急停 5 按钮 | 对应 RunSequence/HomeAll/Stop/Initialize/EmergencyStop；启动置灰、完成/失败/停止恢复（D2 验证） |
| 6.3 | 坐标面板 | 随 stateUpdated 实时 FK+TCP 显示夹爪尖端坐标；**不卡顿**（缓存关节位，D5 验证） |
| 6.4 | 日志框 | 逐条显示 logMessage，滚动到底，上限 1000 行（D13） |
| 6.5 | 双相机框 | 接 frameReady（SimCamera 画面）；真机相机未装时无画面不崩溃 |
| 6.6 | 示教读取（ProcessPage） | 当前关节 FK 填充点位并持久化到 action.points（D8 验证） |

### 阶段 7：异常与回归（0.5 天）

| # | 动作 | 期望 |
|---|---|---|
| 7.1 | 运动中拔网线 | 程序不崩溃、有错误日志；恢复后重新使能可重连 |
| 7.2 | 运动中拔 COM3（舵机） | 不崩溃；热重连自动恢复；逻辑角度/使能状态正确（M05） |
| 7.3 | 切回 SimCard/SimServo 回归 | 手动页点动/Go/急停/软限位 + AutoRunPage 方案 + 坐标面板，确认无回归 |
| 7.4 | Debug + Release 双编译 | EXITCODE=0 |

## 四、验收标准（对应 kinematics_sequenceworker_plan.md §六）

1. Home Offset 真机生效：J1 回零显示 -102°、J2 回零显示 -28°；手动 Go 以逻辑角度下发、回读一致。
2. Z 标定完成：每圈脉冲实测复核（误差<2%），`calibrationPending=false`，软限位修正。
3. J2/R 舵机：offset 生效、点动/Go 无反向无顿挫、无热重连误触发。
4. SequenceWorker 真机方案全流程（Move 多点→Gripper→Delay→…）逐步执行、到位等待正确；Stop/急停/未使能门禁/运行中改参/运行中关窗全部符合预期。
5. AutoRunPage 5 按钮 + 坐标 FK 面板 + 日志 + 方案下拉 + 示教读取真机可用。
6. 拔网线/拔 COM3 不崩溃、热重连恢复。
7. Debug + Release 编译通过；`TEST_RECORD.md` 全量记账（🟢/踩坑备注）。

## 五、风险与注意事项

1. **Home Offset 行为变更**：回零后显示 -102/-28 而非 0，操作人员需知晓（与旧版不同）。
2. **J1 软限位 [‑102,8]**：回零逻辑点 -102 恰为下限，回零后只能向 + 点动；Go 目标不得 < -102。
3. **Z 未回零风险**：若 Z 回零未配置，方案点位 Z 必须落在 [0,195] 且避开机械限位；建议先完成 Z 回零配置再跑自动方案。
4. **步进开环无编码器**：失步/堵转检测不到，急停/限位后位置可能漂移，重新回零再继续；加速应逐步提 maxAccel→maxSpeed 到临界再退回一格。
5. **挤出轴未接电机**：方案不含 Extrude（或 Extrude 动作在 SequenceWorker 会 MoveAbs(Extruder) → 下发到 axis3 无电机——**若方案含 Extrude 需确认不报错或轴3已使能**，建议本轮方案不含 Extrude）。
6. **GUI 完整退出路径**：之前 Start-Process 测试有控制台 CTRL_CLOSE 伪影，本轮真机用真实窗口点关闭按钮验证（阶段 4.5/5.10）。
7. **每阶段验收不通过即停**，先查 `log/creampuff.log` 再定位；改动核心逻辑前先读 `TEST_RECORD.md` 防回归（AGENTS.md 硬规约）。

## 六、测试记录（执行时填写）

| 阶段 | 测试项 | 结果 | 备注 |
|------|--------|------|------|
| 1 | 连接/使能/回零 Home Offset | | |
| 2 | 单轴点动 + Go 逻辑坐标 | | |
| 3 | Z 标定 | | 记录实测每圈脉冲 |
| 4 | 回零回归 + 停止/急停/退出 | | |
| 5 | SequenceWorker 方案 | | |
| 6 | AutoRunPage UI 接线 | | |
| 7 | 异常/回归/编译 | | |

## 七、工时估算

| 阶段 | 预估 |
|---|---|
| 0 前置 | 0.5 天 |
| 1 连接+Home Offset 首验 | 1 天 |
| 2 单轴点动+Go | 1 天 |
| 3 Z 标定 | 0.5-1 天 |
| 4 回零回归+停止/急停 | 0.5-1 天 |
| 5 SequenceWorker 方案 | 1 天 |
| 6 AutoRunPage UI | 0.5-1 天 |
| 7 异常/回归/编译 | 0.5 天 |
| **合计** | **约 5.5-7 天** |
