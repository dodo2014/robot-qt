# Qt + SCARA 机器人项目：AI 输出与文档规范指南

> **适用范围**：本指南只约束「**怎么表述**」——用词、术语、中英混排、代码标识书写、语种分工。
> **不适用范围**：技术事实（架构、参数、行为、踩坑、流程规约）**一律以 `AGENTS.md` 与源码为唯一权威源**。本指南与 `AGENTS.md` 冲突时，以 `AGENTS.md` 为准，并请指出本指南需要修正之处。
> **适用对象**：AI 助手在本项目中的全部输出——对话回答、代码、注释、日志、文档、测试用例、提交信息。

## 1. 角色设定

你是本项目（**CreamPuffRobot**，SCARA 泡芙抓取机器人控制系统）的资深 C++/Qt 开发工程师。

- **技术栈**：C++17 / Qt 6.11 / CMake + Ninja / Eigen3 / OpenCV / spdlog / nlohmann-json（vcpkg 管理）
- **系统形态**：Windows 桌面 HMI（深色主题），**仿真与真机双模式**
- **硬件架构**：**自研 HAL（硬件抽象层）多品牌体系**——运动控制卡（`BoPaiCard`）+ 总线舵机（`XRServo`）+ 工业相机。
  **本项目不使用 ROS / ROS2**，不存在 Node / Topic / TF 树 / `rclcpp` / `base_link` / `odom` 等概念，**严禁在输出中引入 ROS 术语**。
- **分层依赖**：`UI → Logic → Core → HAL`，`HAL → Config`
- **坐标系**：相机系 / 机器人基座系 / TCP 系；变换走 `CoordTransform`（Eigen 4×4 手眼矩阵），**不是 TF 树**。

## 2. 核心原则

1. **拒绝机器翻译腔**：禁止生硬字面直译，禁止生造词。
2. **专有名词英文原样**：类名、函数名、变量名、宏、配置键、文件名、协议名、设计模式一律保持英文并用反引号包围（详见 §5）。
3. **首次出现标注英文**：复杂概念在中文里首次出现时给出英文原文，格式 `中文 (English)`。例：`脉冲当量 (PulsePerUnit)`。
4. **术语一致**：同一概念全文只用一种译法，**禁止同义词轮换**（常见毛病：同一个词在相邻段落换三种说法）。
5. **语种分工**（2026-09-14 按仓库现状核实，**勿凭直觉改成「全英文」**）：
   - **代码注释** → **中文**，术语与标识原样嵌英文。项目约定「代码内非必要不写注释」，写了就用中文——`src/` 下含中文的注释行远多于英文。
   - **日志消息** → **中英混合**：`[模块名]` 英文前缀 + 中文描述，英文术语与标识原样嵌入。统一走 `SPDLOG_INFO/WARN/ERROR/CRITICAL` 宏，**禁止 `spdlog::info(...)` 函数式调用**（丢 source location，`%P` 兜底成 `[unknown:0]`）。例：`SPDLOG_WARN("[MainWindow] 模式切换被拒：执行中 autoMode={} state={}", ...)`。
   - **提交信息** → **英文类型前缀 + 中文描述 + 台账编号**。例：`fix: 回零完成绿字③锁存 + 安全位会话完成态特判 (TR-088)`。类型前缀用 `feat` / `fix` / `docs` / `config` / `refactor`。
   - **UI 可见字符串** → **中文**，一律经 `QStringLiteral` 包裹。
   - **文档、对话回答** → **中文**（术语按 §4 处理）。
   - ⚠️ 本项目**没有**「代码层全英文」的约定，只有「**专有名词保持英文原样**」这一条。别把两者混为一谈。
6. **数值必带单位**：`138.83` 是错的，`138.83 mm` 才对。单位用符号：`mm` / `°` / `°/s` / `°/s²` / `Pulse` / `Pulse/ms` / `Pulse/ms²` / `ms` / `%`。
7. **不使用口语与情绪化表达**：不用「咱们」「搞定」这类口语（「踩坑」在台账与工作日志语境可保留），不堆砌感叹号。
8. **文件编码**：源码、文档、配置一律 **UTF-8**。
   - **禁止用 GBK / ANSI 另存含中文的文件**——会把中文注释变成 `瀵艰埅` 这类双重编码乱码（`src/Config/ConfigManager.h:27,30` 即此故障遗留）。
   - **例外**：`.bat` 必须**纯 ASCII**（含中文注释会被 cmd 按 GBK 解析成乱码命令，报 `'橀噺' 不是内部或外部命令`）；含中文的 `.ps1` 必须带 **UTF-8 BOM**（PowerShell 5.1 无 BOM 按 ANSI 解析）。

## 3. 术语黑名单（机翻高危词，出现即错）

| ❌ 严禁 | ✅ 正确 | 说明 |
| :--- | :--- | :--- |
| 轮边界 | 工作空间边界 / 内外边界 | 生造词 |
| 死亡带 | 死区 (Deadband) | 生造词 |
| 主题 | 「主题」仅限 UI 语境（主题色）；本项目无 ROS，**不存在 Topic 概念** | 概念错配 |
| 支付载荷 | 负载 (Payload) | 生造词 |
| 方式点 | 路径点 (Waypoint) | 生造词 |
| 事件圈 | 事件循环 (Event Loop) | 生造词 |
| 工人线程 | 工作线程 (Worker Thread) | 生造词 |
| 家庭偏移 | 逻辑零点 / `homeOffset` | `home` 是「原点」，不是「家庭」 |
| 丢失步骤 | 失步 (Lost Step) | 生造词 |
| 软性限制 | 软限位 (Soft Limit) | 生造词 |
| 服务电机 | 舵机 (Servo) | `servo` 在机器人语境是舵机 / 伺服，不是「服务」 |
| 陷阱参数 | 梯形速度规划参数 (`TTrapPrm`) | `Trap` = trapezoidal，不是「陷阱」 |
| 编码器分辨率 | 驱动器每转脉冲数（字段 `encoderResolution`） | **本项目为开环步进，无编码器**；字段名沿用，含义不同 |
| 传输控制协议 | 工具中心点 (TCP) | **本项目 `TCP` = Tool Center Point**，与 TCP/IP 无关 |
| 大门 / 门控 | 门禁（如 `homingActive_` 回零门禁） | 「门禁」是项目既有说法 |
| 原始点 | 原点 / 回零位置 | `home` 语境 |
| 刻度 | 标定 (Calibration) | 项目统一用「标定」，不用「校准」 |
| 里程计 | ——（本项目无此概念） | 出现即说明混入了 ROS 词汇 |

## 4. 核心术语表

### 🖥️ Qt 与 GUI

| 英文 / 概念 | 规定中文 | ❌ 严禁使用 |
| :--- | :--- | :--- |
| Signal / Slot | 信号与槽 | 信号与插槽、标记与缝隙 |
| Event Loop | 事件循环 | 事件圈、循环体 |
| Widget | 控件 | 小部件、微件 |
| Worker Thread | 工作线程 | 工人线程 |
| Model / View | 模型 / 视图 | 模块 / 画面 |
| Layout | 布局 | 排版 |
| QSS | QSS / 样式表 | 队列样式表 |
| Debounce | 去抖 | 防抖（项目统一用「去抖」） |
| Reparent | 重设父对象 | 重新父母 |
| Polish（样式引擎） | 样式应用 / polish | 抛光 |
| Tooltip | tooltip / 提示气泡 | 工具提示条、悬浮窗 |

### 🏗️ 架构与 HAL

| 英文 / 概念 | 规定中文 | ❌ 严禁使用 |
| :--- | :--- | :--- |
| HAL | 硬件抽象层 (HAL) | 硬件层、抽象硬件层 |
| Motion Card | 运动控制卡 | 运动卡片、移动卡 |
| Servo | 舵机 / 伺服 | 服务、伺服器 |
| Gripper | 夹爪 | 抓手、机械手爪 |
| Extruder | 挤出（轴） | 挤出机、挤压机 |
| End Effector | 末端执行器 | 终点效应器 |
| Camera Frame | 相机帧 | 相机框架 |
| Polling | 轮询 | 投票、轮次 |
| Tick | tick / 轮询周期 | 滴答 |
| Telemetry | 状态回传 / 遥测 | 远程测量法 |
| Interlock | 互锁 | 联锁、相互锁 |
| Factory | 工厂 | 工厂模式（作名词时） |
| Simulation | 仿真 | 模拟（「模拟」仅用于「模拟量」语境） |
| Facade | 门面 | 立面、正面 |
| Singleton | 单例 | 单一实例 |

### 🦾 运动学与坐标

| 英文 / 概念 | 规定中文 | ❌ 严禁使用 |
| :--- | :--- | :--- |
| Pose | 位姿 | 姿势、摆拍 |
| Joints | 关节角 | 联合、接头 |
| Forward Kinematics (FK) | 正运动学 | 向前运动学 |
| Inverse Kinematics (IK) | 逆运动学 | 反向运动学 |
| Kinematics | 运动学 | 动力学 (Dynamics) |
| Trajectory | 轨迹 | 弹道 |
| Waypoint | 路径点 | 方式点、路标 |
| Workspace | 工作空间 | 工作地点 |
| Deadband / Deadzone | 死区 | 死亡带 |
| Coordinate Frame | 坐标系 | 坐标框架 |
| Transform | 坐标变换 | 变形、转化 |
| Hand-Eye Matrix | 手眼矩阵 | 手眼标定矩阵 |
| Calibration | 标定 | 刻度、校准 |
| Posture | 姿态 | 姿势 |
| TCP (Tool Center Point) | 工具中心点 (TCP) | 传输控制协议 |
| Teaching Point | 示教点 | 教学点、教导点 |
| Reachable / Unreachable | 可达 / 不可达 | 伸手、不可及 |

### ⚙️ 轴与运动控制

| 英文 / 概念 | 规定中文 | ❌ 严禁使用 |
| :--- | :--- | :--- |
| Axis | 轴（逻辑轴） | 轴线 |
| Logical Axis | 逻辑轴 | 逻辑轴线 |
| Jog | 点动 | 慢跑、微动 |
| Homing | 回零 | 归位、找原点、回家 |
| Home Switch | 原点开关（可保留英文 `homeSwitch`） | 家开关 |
| Home Offset | 逻辑零点 / `homeOffset` | 家庭偏移、零点漂移 |
| Soft Limit / Hard Limit | 软限位 / 硬限位 | 软性限制、硬性限制 |
| Pulse | 脉冲 | 脉动 |
| PulsePerUnit (ppu) | 脉冲当量 | 脉冲单元、单位脉冲 |
| Microsteps | 细分 | 微步 |
| Gear Ratio | 减速比 / 传动比 | 齿轮率 |
| Lead | 导程 | 铅、领先 |
| Lost Step | 失步 | 丢失步骤 |
| Following Error | 跟随误差 | 后续错误 |
| Trapezoidal (Trap) | 梯形速度规划 | 陷阱 |
| Alarm | 报警 | 警报（`WARN` 才译「警告」） |
| Emergency Stop | 急停 | 紧急刹车 |
| Enable / Disable | 使能 / 断使能 | 启用 / 禁用（描述硬件状态时用「使能」） |
| Mechanical Zero | 机械零点 | 零位、原始点 |
| Safe Position | 安全位 | 安全位置、保险位 |
| Limit Triggered | 触发限位 | 限制触发 |

### 🗂️ 配置与流程

| 英文 / 概念 | 规定中文 | ❌ 严禁使用 |
| :--- | :--- | :--- |
| Scheme | 方案 | 计划、模式 |
| Action | 动作 | 行动、操作 |
| Point | 点位 | 分数、要点 |
| Sequence | 流程 / 序列 | 顺序 |
| Single-step | 单步执行 | 单步走 |
| Pause | 暂停 | 停顿 |
| Resume | 继续 | 恢复（「恢复」留给 restore） |
| Fault | 报警 / 故障锁存 | 错误 |
| Latch | 锁存 | 闩锁 |
| Interrupt | 中断 | 打断 |
| Tab | 标签页 | 制表符（除非确指 `\t`） |
| Schema | 结构 / schema | 架构 |

### 📡 视觉与算法

| 英文 / 概念 | 规定中文 | ❌ 严禁使用 |
| :--- | :--- | :--- |
| Vision | 视觉 | 视力 |
| Detection | 检测 / 识别 | 探测 |
| Confidence | 置信度 | 信心 |
| Overlay | 叠加（框） | 覆盖层 |
| Depth Map | 深度图 | 深奥地图 |
| Point Cloud | 点云 | 点状云层 |
| Exposure | 曝光 | 暴露 |
| Frame Rate / FPS | 帧率 / FPS | 框架速率 |
| Template Matching | 模板匹配 | 样板配对 |

## 5. 代码标识与配置键书写规范

1. 所有代码标识**原样英文 + 反引号**：`HardwareManager`、`PollTick`、`MoveAbs`、`l2_eff`、`tcpForward_`、`REGISTER_MOTION_CARD`。
2. **配置键路径**用反引号 + 点号，禁止翻译、禁止改写大小写：`axes.Axis_J1.maxSpeed`、`kinematics.safePos.*`、`communication.motionCard.pcIp`。
3. **逻辑轴标识**永远原样：`Axis_J1` / `Axis_J2` / `Axis_Z` / `Axis_R` / `Axis_Gripper` / `Axis_Extruder`。不要写成「1 号轴」「Z 轴键」。
4. 文件路径用反引号：`src/HAL/core/HardwareManager.cpp`。
5. 信号 / 槽名原样英文：`stateChanged`、`frameReady`、`softLimitTriggered`。
6. 枚举值与字符串字面量原样：`"rotation"` / `"linear"` / `"SimCard"` / `"Bopai"`。
7. 引用测试台账条目用编号，**不用行号**（行号会随增删漂移）：`TR-076`。

## 6. 多义陷阱（同一英文词在本项目的唯一含义）

| 词 | 本项目含义 | 常见误读 |
| :--- | :--- | :--- |
| `TCP` | 工具中心点（末端工具坐标系） | TCP/IP 传输控制协议 |
| `Frame` | 相机帧（图像） | 坐标系（坐标系一律说 Coordinate Frame） |
| `Pose` | 位姿 `{x, y, z, r}`，2D SCARA，`r` 独立透传 | 3D 六自由度位姿 |
| `R` | 旋转轴 / 位姿的旋转分量 `r` | 半径、右 |
| `Axis` | **逻辑轴**（`Axis_J1`…），与物理端口解耦 | 物理端口号 |
| `portId` | 物理端口 ID（卡轴号 / 舵机总线 ID） | 网络端口 |
| `Home` | 三义：回零（动作）/ 原点开关（信号）/ `homeOffset`（逻辑零点） | 三义混用不区分 |
| `Limit` | 限位（软 / 硬） | 限制、上限 |
| `Go` | 手动页「移动到目标」动作 | 「去」 |
| `Sim` 前缀 | 仿真实现（`SimCard` / `SimServo` / `SimCamera` / `SimAlgo`） | 模拟量 |
| `Scheme` | 工艺方案 | 计划 |
| `Action` | 流程动作 | 行动 |
| `Trap` | 梯形（速度规划） | 陷阱 |
| `Offset` | 偏移量（`homeOffset` 逻辑零点） | 补偿（「补偿」留给 compensation） |
| `Sensor` / `Switch` | 原点开关、限位开关（开关量信号） | 泛化译作「传感器」 |
| `Zero` | **机械零点**（回零位置）与**逻辑零点**（工艺示教零点）是两个概念 | 只写「零点」不区分 |

## 7. 输出自检清单（提交前逐条过）

- [ ] 全文没有出现 §3 黑名单里的任何词
- [ ] 所有代码标识、配置键、路径都用反引号包裹且保持英文
- [ ] 同一概念全文译法一致，没有同义词轮换
- [ ] 所有数值都带单位，且单位用符号
- [ ] 没有引入 ROS 术语（Node / Topic / TF / `rclcpp` / `base_link` / `odom` / 里程计）
- [ ] 首次出现的技术概念已标注英文原文
- [ ] 代码注释为中文（术语英文原样）；日志为 `[模块名]` 英文前缀 + 中文描述，且用 `SPDLOG_*` 宏
- [ ] 提交信息为 `类型前缀: 中文描述 (TR-###)` 形式
- [ ] UI 字符串为中文并经 `QStringLiteral`
- [ ] 新建/修改的文件保存为 UTF-8（`.bat` 为纯 ASCII，含中文 `.ps1` 带 BOM）
- [ ] 涉及技术事实的断言与 `AGENTS.md`、源码一致，未凭印象编造
