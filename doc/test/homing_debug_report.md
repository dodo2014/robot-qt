# 回零功能调试分析报告

**硬件**: BoPai 运动控制卡（单轴回零 `MC_HomeStart`）  
**测试轴**: 轴1 J1（步进电机 + 1:100 谐波减速器，Pulse/deg ≈ 7111）  
**软件版本**: 运动卡 SDK CN6 版  
**报告日期**: 2026-08-12

---

## 1. 初始现象

回零操作无法正常完成，表现为以下症状之一：

| 症状 | 观察方式 | 频率 |
|---|---|---|
| 电机短暂抖动后立即停止 | 视觉观察 | 频繁 |
| `MC_HomeStart` 返回错误码 1（轴忙/未就绪） | 日志 | 间歇 |
| 回零开始后约 300ms 被中断，状态灯熄灭 | PollTick 日志 | 几乎每次 |
| 程序崩溃，异常码 `0xe06d7363`（死锁/异常） | Windows 事件 | 偶发 |
| 已能正常使能、点动、Go，唯独回零失败 | 完整测试流程 | 100% |

**关键线索**：将软限位范围从 `[0, 90]` 改为 `[-180, 90]` 后回零成功——说明根因与软限位校验逻辑有关。

---

## 2. 根因分析（三个独立 bug，按发现顺序）

### Bug 1: `MC_HomeSns` 位掩码传递错误

**位置**: `src/HAL/motioncard/BoPaiCard.cpp` — `HomeAxis`

**错误代码**:
```cpp
// 原代码：不论 homeSns 配置值是什么，永远传 0x1（只指定轴0高有效）
MC_HomeSns(axisId, 0x1);
```

**问题**: `MC_HomeSns` 的第二个参数是**全局位掩码**（每 bit 对应一轴），不是单轴值。原代码无论 J1 的 `homeSns` 配置是 0（低有效）还是 1（高有效），永远以 `0x1`（轴 0 高有效）写入卡寄存器。这导致：
- `homeSns=0` 时低有效极性未生效
- `homeSns=1` 时恰好生效（因为 `0x1` 碰巧=轴 0 高有效）
- 但对于非零轴号或需要低有效（`homeSns=0`）的轴，行为异常

**J1 真机确认**: 配置 `homeSns=0`（低有效）配合 `homeDir=1`（逆时针搜索），电机向机械限位方向运动并成功触发 HOME 信号。

**修复**:
```cpp
// 先读出当前寄存器值，再按 homeSns 值置位或清零对应 bit
if (homeSns >= 0) {
    unsigned long sl = MC_GetHomeSns(axisId);  // 读当前全局掩码
    if (homeSns == 1)
        sl |= (1UL << axisId);        // 设该轴高有效
    else
        sl &= ~(1UL << axisId);       // 设该轴低有效
    MC_HomeSns(axisId, sl);
}
```

---

### Bug 2: `ulHomeMaxDis = 0` 被固件解析为"搜索 0 距离"

**位置**: `src/HAL/motioncard/BoPaiCard.cpp` — `HomeAxis`

**错误代码**:
```cpp
prm.ulHomeMaxDis = 0;  // 原意："不限"
```

**问题**: 部分 BoPai 卡固件将 `ulHomeMaxDis=0` 解释为"搜索距离=0 脉冲"而非"不限制搜索距离"，导致回零启动后立即完成（电机未实际运动到限位）。这与"电机抖动一下就停"的现象吻合。

**修复**:
```cpp
prm.ulHomeMaxDis = static_cast<unsigned long>(homeMaxDis);
```

**配置**: 新增 `axes.<key>.homeMaxDis`（单位 Pulse）。J1 设 `1,500,000`（≈211°，远大于 90° 行程，有充裕余量）。

> **公式**: `homeMaxDis ≥ 2 × 全行程脉冲数`（确保能覆盖整个机械行程）

---

### Bug 3: PollTick 软限位校验误杀回零运动

**这是导致"改软限位为 [-180, 90] 就正常"的直接根因。**

**位置**: `src/HAL/core/HardwareManager.cpp` — `PollTick`

**错误代码（简化）**:
```cpp
for (int i = 0; i < axisCount_; ++i) {
    auto st = GetAxisStatus(i);
    double pos = AxisConverter::stepsToPosition(...);

    if (st.running && (pos <= limitMin || pos >= limitMax)) {
        StopJog(i);  // 立即停止——不管是不是在回零
    }
}
```

**问题链条**:

1. 回零开始前，当前位置 `position = 0`（刚上电/上次停止位置）
2. 软限位配置为 `limitMin = 0`（回零目标位置）
3. 回零启动后 `st.running = true`
4. PollTick 检测到 `position <= limitMin`（即 `0 <= 0`）
5. 触发 `StopJog(MC_Stop)`——在下发 `MC_HomeStart` 之后的第一个 PollTick 周期（约 10~20ms）就把回零终止了
6. 回零被终止后 `st.running = false`，但 `MC_HomeStart` 已被中断，Home 状态未完成

**时序示意**:
```
T+0ms     HomeAxis() → MC_HomeStart(axisId)  // 回零启动
T+5ms     homingActive_[i] = true            // 门禁置位
T+16ms    PollTick() → position=0 ≤ 0 → StopJog()  // 软限位误杀!
T+20ms    电机停止，回零失败
```

**为什么改 [-180, 90] 就好了**: 因为 `position=0` 不再 ≤ `limitMin=-180`，软限位条件不满足。

**修复**:
```cpp
// 回零中的轴不参与软限位校验
if (st.running && !homingActive_[i] && (pos <= limitMin || pos >= limitMax)) {
    StopJog(i);
}
```

`homingActive_[i]` 生命周期：
- `HomeAxis()` 置 `true`
- `PollTick` 检测 `!running` 复位
- `StopAxis`/`StopJog`/`DisableAll`/`EmergencyStop` 清除

---

## 3. 调试过程中发现的关联问题

### 问题 A: HomeAxis 持锁后调 `GetAxisStatus` 导致死锁

**现象**: 程序崩溃，Windows 事件日志显示异常码 `0xe06d7363`。

**原因**: `HomeAxis` 已持有 `std::mutex mutex_`，内部又调 `GetAxisStatus`（同一互斥锁）→ 二次加锁死锁 → 抛出 C++ 异常。

**触发条件**: `MC_HomeStart` 返回 0（成功）后，下发 `MC_HomeStart` 需要检查状态。

**修复**: 持锁内直接读 `lastStatus_[axisId]`，不调 `GetAxisStatus`。

---

### 问题 B: `MC_HomeStart` 返回 1（轴忙/未就绪竞态）

**现象**: 刚 `MC_HomeStop` 后立即 `MC_HomeStart`，卡返回错误码 1。

**原因**: `MC_HomeStop` 异步执行，卡端 HOME 状态机尚未完全退出——后续 `MC_HomeStart` 被拒绝。

**修复**: 加 150ms 延迟 + 重试（≤3 次），启动前再发一次 `MC_Stop` 确保前序状态完全清除。

---

### 问题 C: `StopAxis` 仅 `MC_Stop` 无法退出 HOME 状态机

**现象**: 回零中点停止后，后续点动无反应。

**原因**: `MC_Stop` 清除了 trap 运动，但卡端 HOME 状态机未退出——后续 Jog/Trap 命令被卡端的 HOME 状态锁定拒绝。

**修复**: `StopAxis` 增加 `MC_HomeStop + MC_Stop` 顺序（先退出 Home 再停止运动）。

---

## 4. 修复汇总

| 文件 | 改动 | 说明 |
|---|---|---|
| `IMotionCard.h` | AxisConfig 加 `homeMaxDis` | 新增配置字段 |
| `HardwareManager.cpp` | PollTick 软限位加 `!homingActive_` guard | Bug 3 修复 |
| `HardwareManager.cpp` | LoadAxisConfigs 读取 `homeMaxDis` | 配置读取 |
| `BoPaiCard.cpp` | `MC_HomeSns` 读-改-写 bit | Bug 1 修复 |
| `BoPaiCard.cpp` | `ulHomeMaxDis` 从 config 读取（非 0） | Bug 2 修复 |
| `BoPaiCard.cpp` | `MC_HomeStart` 延时 150ms + 重试 ≤3 次 | 问题 B 修复 |
| `BoPaiCard.cpp` | 持锁内直读 `lastStatus_` | 问题 A 修复 |
| `BoPaiCard.cpp` | `StopAxis` 增加 `MC_HomeStop + MC_Stop` | 问题 C 修复 |
| `IMotionCard.h` | MotorStatus 加 `homeSwitch`/`homeFail` | 诊断增强 |
| `config.json` | J1 加 `homeMaxDis: 1500000` | 配置 |

---

## 5. 经验总结（后续轴调试参考）

### 5.1 回零调不通时的排查顺序

1. **检查软限位范围**：回零起始位置是否在 `[limitMin, limitMax]` 内？边界值是否恰好等于 limit？（Bug 3）
2. **检查 `homeMaxDis`**：是否非零？（Bug 2）
3. **检查 `homeSns` 极性**：真机用万用表/示波器确认 HOME 传感器输出电平（有效时高/低），对照 config 的 `homeSns`（Bug 1）
4. **检查 `homeDir`**：电机实际运动方向是否正确？（向 HOME 传感器方向运动）
5. **看日志**：确认 `MC_HomeStart` 返回值 → `homeSwitch` 信号是否出现过 → `homeFail` 是否置位

### 5.2 通用经验

- **`ulHomeMaxDis` 必须非零**：不同品牌卡的 SDK 对 0 的解释不一致。BoPai 部分固件 0="搜索 0 距离"，对用户语义不可见。任何时候都设一个合理大值（全行程 2~3 倍）。
- **软限位与回零的关系**：PollTick 软限位校验作用于所有 `running=true` 的轴——包括正在回零的轴。必须用回零专有门禁 (`homingActive_`) 豁免回零期间的边界检测。
- **卡端状态机是异步的**：`MC_HomeStop`/`MC_Stop` 后卡端的 HOME/Trap 状态机清除需要时间，后续命令前加延迟。
- **互斥锁内禁止调公共访问方法**：持锁时只能读内部缓存（`lastStatus_`），任何调用 `GetXxx` 等公开方法的行为都可能隐性加锁。
- **配置项宁可冗余不省略**：`homeBackDis`（反向退出）当前用 0 也可以，但保留作为后续轴的备选项。删除一个字段的成本远高于保留它。

### 5.3 新轴回零 Checklist

- [ ] 驱动器脉冲/rev 确认（拨码/参数），`encoderResolution` 配置正确
- [ ] 减速比/齿轮比/导程确认，`gearRatio`/`lead` 配置 → 换算验证（手动 Go 固定距离测量）
- [ ] HOME 传感器信号极性确认（万用表），配置 `homeSns` 正确
- [ ] 电机搜索方向确认（向 HOME 传感器），配置 `homeDir` 正确
- [ ] `homeMaxDis` 设非零值（全行程脉冲数 × 2~3）
- [ ] 软限位范围不与 `homeMaxDis` 冲突
- [ ] 回零速度 Pulse/ms 换算正确
