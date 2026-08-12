# HAL 目录重组 + HardwareManager 拆分 — 执行计划

状态：已确认，由同一 agent 按 A → B → 目录重组 → 文档 顺序执行。

## 一、目标

- `src/HAL` 31 个文件按职责分层到子目录。
- `HardwareManager.cpp`（983 行）拆分出低耦合子模块 `CameraManager`、`AxisConfigService`。
- **对外 API 与 UI 层零改动**（HardwareManager 保留转发方法）。
- 不引入任何行为变化；每阶段独立编译 + 回归。

## 二、执行顺序

1. **阶段 A — CameraManager**：新建 `src/HAL/camera/CameraManager.*`；HardwareManager 相机成员/方法改为持有并转发；`frameReady` 转发。编译 + 回归。
2. **阶段 B — AxisConfigService**：新建 `src/HAL/core/AxisConfigService.*`；`AxisUnit/GetJogSpeed/GetMaxSpeed/SetJogSpeed/GetLimitMin/Max/IsWithinSoftLimits` 迁入；HardwareManager 保留转发。编译 + 回归。
3. **目录重组**：移入 `interfaces/core/motioncard/servo/camera/algorithm`；改 CMake 源路径 + include dir 追加全部子目录；改外部 `HAL/Xxx.h` 前缀（9 处）。编译 + 回归。
4. **文档**：更新本文件 + AGENTS.md（目录结构、拆分说明、重构经验）。

## 三、目标目录结构

```
src/HAL/
  interfaces/   I*.h（IMotionCard/IAxisServo/IEndEffector/ICamera/IPuffAlgorithm）
  core/         HardwareManager.*, AxisConverter.*, AxisMap.h, HALFactory.h, AxisConfigService.*
  motioncard/   IMotionCard.h + BoPaiCard.* + SimCard.*
  servo/        IAxisServo.h + XRServo.* + SimServo.*
  camera/       ICamera.h + SimCamera.* + CameraCaptureWorker.* + FrameSaver.* + FrameConverter.* + SimVision.h + CameraManager.*
  algorithm/    IPuffAlgorithm.h + SimAlgo.*
```

## 四、关键事实（已核实）

- HAL 内部平铺 include（`#include "SimCard.h"`）靠 include dir 按文件名解析，子目录移动后无需改。
- 外部引用（UI/Core/Logic/main）用 `HAL/Xxx.h`，需改前缀共 9 处：
  `HardwareManager.h`×4、`AxisMap.h`×1、`HALFactory.h`×1、`FrameConverter.h`×1、`FrameSaver.h`×1。
- 所有头文件名全局唯一。
- `src/HAL/CMakeLists.txt` 显式列源文件，需更新路径。

## 五、阶段 A — CameraManager

- 新类（QObject）：成员 `ICamera* camera_`、`QThread* cameraThread_`、`CameraCaptureWorker* cameraWorker_`、`bool cameraStreaming_`。
- 方法：`Open/Close/StartStream/StopStream/IsStreaming`；信号 `frameReady`。
- HardwareManager：持有 `unique_ptr<CameraManager>`；`CameraOpen/Close/Start/Stop/IsStreaming` 转发；`frameReady` 转发；`camera()` 访问器保留。
- 收益：−53 行 −4 成员。

## 六、阶段 B — AxisConfigService

- 新类（无 Qt 信号）：`AxisUnit/GetJogSpeed/GetMaxSpeed/SetJogSpeed/GetLimitMin/GetLimitMax/IsWithinSoftLimits`。
- 持有 `QVector<AxisConfig>&` 引用 + ConfigManager 读。
- HardwareManager 保留转发方法（UI 零改动）。
- 收益：−104 行。

## 七、明确不做

- `PollTick/JogTick/CheckAxisBusy` 与运动门面（MoveAbs/MoveJog/Home/Enable）保持一体：共享 `axisEnabled_/homingActive_/axisBusyUntilMs_/软限位/报警` 可变状态并交叉 emit 信号，强行拆类会跨对象共享状态、复杂度上升。

## 八、验收清单（每阶段）

1. Debug + Release 编译 EXITCODE=0（编译前结束运行中的 CreamPuffRobot.exe 防 LNK1168）。
2. 仿真冒烟：启动存活，手动页点动/Go/回零/停止/急停状态灯正常。
3. J1 真机回归：连接使能 → 点动方向 → Go 到位 → 停止 → 回零归零 → 回零中禁止点动并提示。
4. 相机回归：开流/关流/截图不崩。

## 九、时间预估

| 任务 | 预估 |
|---|---|
| 阶段 A | 2–3 h |
| 阶段 B | 2–3 h |
| 目录重组 | 2–4 h |
| 编译 + 回归 | 1–2 h |
| 文档 | 0.5–1 h |
| 合计 | 8–13 h |
