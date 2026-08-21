# 「小脑」重构 + 「大脑」引擎 — 测试方案

配套计划：`doc/kinematics_sequenceworker_plan.md`。
依据：`doc/gemini_qr.md`、`AGENTS.md` 防回归硬规约（测试台账 + 修改前验算）。
前置：**用户确认计划后再执行本方案**。

## 一、测试目标

1. 验证「小脑」（Kinematics/CoordTransform/Home Offset）数学正确性与行为符合 gemini_qr.md 模型。
2. 验证「大脑」（SequenceWorker）全流程/单步/中断/线程安全。
3. 回归现有已验收功能，确认无破坏（尤其手动页、软限位、回零、视觉页）。

## 二、测试环境

| 环境 | 配置 | 用途 |
|---|---|---|
| 仿真 | config `simulation.enabled=true`，SimCard/SimServo/SimCamera/SimAlgo | 全量功能与 GUI 自动化 |
| 真机 | config `simulation.enabled=false`，Bopai + XRServo（网线连卡，关 wifi） | 有条件时回归 |
| 构建 | Debug + Release 双编译（VCVS 环境，见 AGENTS.md） | 编译门禁 |

执行命令（Debug，CLI）：
```
cmd /c "call vcvars64.bat && ninja CreamPuffRobot"
```
编译前先结束运行中的 `CreamPuffRobot.exe`（防 LNK1168）。

## 三、测试用例

### A. 构建门禁

| 编号 | 动作 | 期望结果 |
|---|---|---|
| A01 | Debug 全量编译 | EXITCODE=0，无 W4 新增警告 |
| A02 | Release 全量编译（build_release.bat） | EXITCODE=0 |
| A03 | 仿真启动（Debug exe） | 存活无崩溃，手动页状态灯正常 |

### B. 小脑 — Kinematics（数学正确性）

| 编号 | 动作 | 期望结果 |
|---|---|---|
| K01 | 随机多组 joints → `Forward` → `Inverse`（同构型，含 TCP） | 往返还原，误差 < 1e-6 度/mm |
| K02 | 目标距离 > L1+L2_eff（如 (400,0)） | 返回 false + `SPDLOG_WARN`，不崩溃 |
| K03 | 目标落入甜甜圈内孔（r < \|L1−L2_eff\|，含原点） | 返回 false + `SPDLOG_WARN`（原点物理不可达，已删旧"原点解"逻辑） |
| K04 | 同一目标 elbow_up/elbow_down 双解均返回；`InverseSmart` 按当前 J2 就近选解 | 双解有效；就近解正确、无大甩臂 |
| K05 | 任意 target.r（如 90/-90） | out.r 完全透传，不被 J1/J2 改变 |
| K06 | `SetTCP(53,0,-130)` 后正逆解 | l2_eff = L2+53；Z = 电机高度+z0−h1−tcpDown；TCP 内化，不再有外部 ApplyTCPOffset |
| K07 | 目标超出逻辑限位 | 返回 false 并提示，不崩溃 |

### C. 小脑 — CoordTransform

| 编号 | 动作 | 期望结果 |
|---|---|---|
| C01 | 手眼矩阵 = 单位阵，`CameraToRobot(xc,yc,zc)` | 输出 = 输入 |
| C02 | 手眼矩阵含平移 (12.3,-5.7,38.1)（config 现值） | 输出 = 输入 + 平移 |
| C03 | `PixelToRobot(u,v,depth)`（设置内参后） | 像素+深度 → 基座坐标，链路无异常 |

### D. 小脑 — Home Offset 落地（**最大行为变更**）

| 编号 | 动作 | 期望结果 |
|---|---|---|
| H01 | J1 回零（仿真/真机） | 显示 **-102°**（逻辑=机械 0−offset）；J2 回零显示 **-28°** |
| H02 | 手动 Go J1 → 逻辑 10° | 下发机械 112°（10+102）；回读显示 10°；软限位校验用逻辑值 |
| H03 | Servo 轴（J2/R）Go/点动 | 下发 = 逻辑+offset；回读 = 机械−offset；点动目标与遥测一致 |
| H04 | 软限位回归：点动撞界自动停、Go 越界拒绝、反向离开放行 | 与落地前行为一致（判定恒为逻辑坐标） |
| H05 | inverted 轴（J1 direction=1）点动/Go/回读 | 方向、显示、限位判断仍一致（逻辑坐标取反语义不变） |
| H06 | linear 轴（Z/夹爪/挤出，offset=0）点动/Go | 与落地前完全一致（无偏移叠加） |
| H07 | 回零流程回归 | 撞限归 0（机械）→ 显示逻辑偏移角；回零中禁止点动并提示 |
| H08 | **J1 限位范围配置核对** | 回零逻辑点 -102 与 config `limitMin=-102/limitMax=8` 的允许区间一致（-102 恰为下限）；逻辑 8 允许、9/-103 拒绝 |

### E. 大脑 — SequenceWorker

| 编号 | 动作 | 期望结果 |
|---|---|---|
| S01 | 方案 [Move→Gripper→Delay→Extrude→Move] 自动运行 | 日志逐步打印 actionStarted(序号,名称)；坐标正确；schemeFinished 发出 |
| S02 | Move 动作含多点表 + speedPercent=50 | 逐点到位（复用 axisMoveFinished/IsAxisBusy 等待）；速度按 50% 折算 |
| S03 | Vision 动作（仿真 SimCamera+SimAlgo） | Detect → CameraToRobot 得基座目标；模拟路径（无相机）延时+日志 |
| S04 | 单步模式：SetStepMode(true) 后运行 | 每执行完一个动作挂起；NextStep 唤醒执行下一步 |
| S05 | 运行中 Stop() | 当前动作安全停止、跳出循环、发 interrupted |
| S06 | 运行中 EmergencyStop() | 中断 + 断使能；恢复需重新手动使能 |
| S07 | 未使能时 RunSequence | 拒绝 + 提示（使能门禁复用） |
| S08 | worker 移入 QThread，运行中操作 UI | 主线程不卡（点导航/按钮即时响应） |

### F. UI 接线

| 编号 | 动作 | 期望结果 |
|---|---|---|
| U01 | AutoRunPage 启动/复位/停止/初始化/急停 | 5 按钮接线有效（对应 RunSequence/HomeAll/Stop/Emergency 等） |
| U02 | AutoRunPage 坐标面板 | 随 stateUpdated 实时刷新（FK+TCP，即夹爪尖端坐标） |
| U03 | AutoRunPage 日志框 | 显示 SequenceWorker logMessage |
| U04 | AutoRunPage 两相机占位框 | 接入 frameReady 实时画面（RGB/识别叠加） |
| U05 | ProcessPage「示教读取」 | 当前关节 FK(tcp=true) 坐标填充选中行 |
| U06 | 手动/自动模式切换 | 自动：手动页全禁用、自动页启动可用；手动：反之（互锁见开发文档.md） |

### G. GUI 自动化回归（现有功能防回归）

| 编号 | 场景 | 期望结果 |
|---|---|---|
| G01 | 手动页 6 轴点动积分 | MoveJog/StopJog 位置累计正确 |
| G02 | 每轴状态点 + 一键回零提示 | 使能绿/运行蓝/告警红；未使能回零提示 |
| G03 | 急停 | 全轴 halt + 舵机 Torque OFF |
| G04 | 舵机点动/停止 | 平滑、Stop 保持力矩 |
| G05 | 未连接硬件 | 无效类型不动作、有提示 |
| G06 | 软限位提示 | 状态点橙色 + 底部聚合提示、点动离开清除 |

### H. 真机验证（有条件时，config 切 Bopai+XRServo）

| 编号 | 场景 | 期望结果 |
|---|---|---|
| M01 | J1 回零 → 显示 -102°；Go 逻辑 10° → 定位到位 | 逻辑角度机制真机生效 |
| M02 | Z/夹爪每圈脉冲标定回归 + Go 定位 | calibrationPending 处理、精度复验 |
| M03 | J2/R 舵机 offset 生效 + 点动/Go | 无反向运动、无顿挫、无热重连误触发 |
| M04 | SequenceWorker 真机全流程冒烟 | Move/Gripper/Delay/Extrude 逐步执行、到位等待正确 |
| M05 | 拔网线/舵机离线 | 热重连后逻辑角度/使能状态正确恢复 |

## 四、验收标准

1. 全部 A/B/C/D/E/F/G 用例通过（仿真环境）。
2. 任一现有已验收用例（G 组）回归失败 → 停止推进，先修复。
3. IK 往返误差 < 1e-6；超臂展/限位拒绝只记日志不崩溃。
4. Home Offset 生效行为与计划 §四 描述一致。
5. Debug + Release 编译通过；真机冒烟（H 组）无崩溃。
6. `TEST_RECORD.md` 逐条记账（🟢 已通过 / 踩坑备注）。

## 五、执行顺序建议

1. A 构建门禁 → B/C 小脑数学验证（纯逻辑，最快暴露数学问题）。
2. T3 Home Offset 落地后跑 D 组（行为变更，重点回归 H04/H05/H06/H07）。
3. E/F 大脑引擎 + UI 接线验证。
4. G 全量 GUI 自动化回归。
5. 有条件接真机跑 H 组。
6. 全部通过后更新 `TEST_RECORD.md`、`doc/config.md`、`AGENTS.md`、`worklog`。

## 六、防回归硬规约（遵守）

- 修改 `HardwareManager`/`AxisConverter`/软限位/回零前，先读 `TEST_RECORD.md` 现有 🟢 用例，推演是否被破坏。
- 每个用例通过后立即记账，不攒批。
- 严禁在 `QDoubleSpinBox`/`QLineEdit` per-widget QSS 写 `font-size`（Qt6 polish 崩溃陷阱）。
- SequenceWorker 严禁直接操作 UI 控件（跨线程崩溃 0xc0000005）。
