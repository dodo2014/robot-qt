# 阶段 2 轮 B — T7–T10 UI 接线执行计划

## 总体架构

```
MainWindow
├── owns SequenceWorker* + QThread* (T7)
├── passes to AutoRunPage via SetSequenceWorker()
├── connects ConfigPage::paramsChanged → ReloadFromConfig() (T10)
└── ProcessPage uses HardwareManager + Kinematics directly (T9)
```

## T7 — MainWindow 创建 SequenceWorker + QThread

**文件**：`src/UI/MainWindow.h`、`src/UI/MainWindow.cpp`

- `MainWindow.h`：`#include "SequenceWorker.h"`，新增 `SequenceWorker* sequenceWorker_`、`QThread* workerThread_`
- `MainWindow.cpp` 构造函数末尾：创建 QThread + SequenceWorker，moveToThread，connect 清理，start
- 析构/aboutToQuit：quit + wait
- 公开方法 `GetSequenceWorker()` 供注入

## T8 — AutoRunPage 全功能接线

**文件**：`src/UI/AutoRunPage.h`、`src/UI/AutoRunPage.cpp`

### 控件
- `schemeCombo_`：QComboBox，方案下拉
- `logTextEdit_`：QTextEdit，滚动日志
- `cameraRgbLabel_` / `cameraOverlayLabel_`：QLabel，相机画面
- `worker_`：SequenceWorker*，由 MainWindow 注入

### 按钮接线
| 按钮 | 动作 |
|---|---|
| 启动 | RunSequence(选中方案) |
| 复位 | HardwareManager::HomeAll() |
| 停止 | worker_->Stop() |
| 初始化 | HardwareManager::Initialize() |
| 急停 | worker_->EmergencyStop() |

### 信号连接
- `stateUpdated` → FK → 坐标面板更新
- `frameReady` → QLabel 显示
- `logMessage` → QTextEdit 追加
- actionStarted/finished/schemeFinished/interrupted/errorOccurred → 状态更新

## T9 — ProcessPage 示教读取

**文件**：`src/UI/ProcessPage.h`、`src/UI/ProcessPage.cpp`

- OnTeachRead：GetPosition() → Kinematics::Forward() → 填充 PointData 新行到 m_pointTable

## T10 — ConfigPage 参数喂入

**文件**：`src/UI/ConfigPage.h`、`src/UI/ConfigPage.cpp`、`src/UI/MainWindow.cpp`

- ConfigPage.h 新增信号 `paramsChanged()`
- 运动学/TCP 编辑 lambda 末尾 emit
- MainWindow 连接 signal → `sequenceWorker_->ReloadFromConfig()`

## 验证

1. Debug 编译通过
2. 仿真冒烟（按钮、坐标、日志、相机）
3. Release 编译
4. 记账

## 风险

- 跨线程信号：QString 自动队列连接，安全
- FK 每 50ms 计算：轻量，不卡 UI
- CameraFrame 缩放：scaled 保持比例