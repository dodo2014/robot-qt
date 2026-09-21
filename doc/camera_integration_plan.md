# 3D 相机接入（Orbbec Gemini 336）与视觉检测页重构方案

> **需求来源**：用户提供的两份架构指令（由 Gemini 整理）——「3D 相机 HAL 层接入与 Orbbec SDK 封装」与「重构视觉检测专业调试页面」。
> **本文性质**：**方案 → 🟡 S0–S4 已实施（TR-099，2026-09-20），S5/S6 未实施**。S0–S4 的真机实测结果见 §五 各节末尾的「✅ 落地情况」；S5（视觉页重构）的规格已拆到 `vision_test_page_redesign_plan.md`（=P4），S6 的检测结果通道与渲染节流待 P4/P5。
> **前置经验**：相机侧的关键实现与踩坑来自既有 Python 项目 `D:\workspace\projects\robot-single-thread\src\vision\orbbec_camera.py`（812 行，同型号 Gemini 336L，已在生产环境跑过），本文 §四 是它的经验移植清单——**这部分优先于 SDK 文档的乐观描述**。

## 一、需求评审结论

### 1.1 判定合理（可直接采纳）

| # | 需求点 | 采纳理由 |
| :--- | :--- | :--- |
| 1 | SDK 放 `3rdparty/orbbec/`、`USE_ORBBEC` option 控制、POST_BUILD 拷 DLL | 与现有 `USE_BOPAI` / `USE_XRSERVO` 范式完全同构（`src/HAL/CMakeLists.txt:5-6,44-52`、根 `CMakeLists.txt:140-151`），零新概念 |
| 2 | 「先打通取流，暂不管算法」 | 算法是独立的 `IPuffAlgorithm` 接口，相机接入不需要碰它 |
| 3 | 异步取帧、禁止阻塞主线程 | 现有 `CameraManager` 已是独立 `QThread` + worker 模型（`src/HAL/camera/CameraManager.cpp:15-20`） |
| 4 | 内存安全，不用原始指针管理图像 | 项目全面 `unique_ptr` / 值语义 |
| 5 | 配置驱动、算法参数不上面 | 与项目「`config/config.json` 单一事实源」一致 |
| 6 | D2C 对齐是刚需 | RGB 检出像素须映射到深度取 Z，再经手眼矩阵换算基座坐标，不对齐整条链路失效 |
| 7 | 四宫格布局 + 左侧窄控制面板 | 标准机器视觉调参布局，符合现有深色主题（`VisionTestPage.cpp:172` 用 `#262c34`） |

### 1.2 判定错误（须修正后实施）

| # | 需求原文 | 事实 | 处理 |
| :--- | :--- | :--- | :--- |
| E1 | 「为我编写 `ICamera.h`」 | **已存在**（`src/HAL/interfaces/ICamera.h`），9 个纯虚方法：`Open` / `Close` / `IsOpened` / `StartStream` / `StopStream` / `IsStreaming` / `CaptureFrame` / `GetIntrinsics` / `SetIntrinsics` / `GetLastError` | **不重写**。需求的 `Connect(snOrIp)` / `Disconnect()` 与现有 `Open(deviceId, w, h, fps)` / `Close()` 语义重复，改名要同步改 5 处调用点（`HardwareManager`、`CameraManager`、`SimCamera`、`VisionTestPage`、`AutoRunPage`），换不来任何能力。**只做非纯虚扩展** |
| E2 | 「`CameraFrame` 内部需安全地持有 `cv::Mat`」 | 现为 `std::vector<uint8_t> colorData` + `std::vector<float> depthData`（`ICamera.h:16-24`）；HAL 静态库**不依赖 OpenCV**（`src/HAL/CMakeLists.txt:74-79` 只链 Qt6::Core/Gui/Config/spdlog）；项目 OpenCV 走 vcpkg = **4.12.0** | **不改**。`cv::Mat` 是浅拷贝引用计数，跨线程传递后 SDK 回调线程复用缓冲区会读到撕裂帧；而现有 `frameReady(const CameraFrame&)` 靠**值语义 + Qt 跨线程自动深拷贝**保证安全（`CameraCaptureWorker.cpp:10-13` 已 `qRegisterMetaType<CameraFrame>`）。**格式归一在 `OrbbecCamera` 内部做** |
| E3 | 「如 `OB_ALIGN_D2C_HW_MODE` 或 SDK 对应的对齐选项」 | 枚举名正确（`ObTypes.h:586-587`），但 **`Pipeline::start` 的 align 仅支持 HW 模式**（`hpp/Pipeline.hpp:436` 注明「Currently, only `ALIGN_D2C_HW_MODE` supported. For other align modes, please using the AlignFilter interface」）；且该型号**高分辨率下 HW 对齐不可用**（实测上限：彩色 640×480@30 MJPG + 深度 848×480@30 Y16） | 高分辨率**必须走软件对齐**：`ob::Align` 滤波器逐帧 `process()`。见 §四 |
| E4 | 「使用 `OB_ALIGN_D2C_HW_MODE` 或 SDK 对应的对齐选项」中提到的 `AlignFilter` | Python SDK 侧类名确实是 `AlignFilter`（`pyorbbecsdk`），**C++ SDK v2.9.3 里对应类是 `ob::Align`**（`hpp/Filter.hpp:441`，构造 `Align(OB_STREAM_COLOR)` + `setConfigValue("AlignType", ...)`）；`AlignFilter` 在 C++ 侧不存在 | 用 `ob::Align`。**不要用 `config.set_align_mode(SW_MODE)`**——参考实现明确记录该路径走不通 |
| E5 | 「注册设备的连接与断开回调」 | SDK v2.9.3 有 `Context::registerDeviceChangedCallback`（`hpp/Context.hpp:222`；`setDeviceChangedCallback` 已 deprecated）。**但参考实现明确弃用**：文件头注释「不再使用 `set_device_changed_callback`，坚决不用底层 C++ 回调，极易引发崩溃」，用户实测「底层异常不返回，并且会造成程序崩溃」 | **不注册回调**，改**轮询式状态探测**（复用项目 `pollTimer_` 范式）。见 §四 |
| E6 | 「曝光控制：`[自动曝光]` + `[曝光值]` 1~10000」 | 属性确实存在（`Property.h:795,800`：`OB_PROP_COLOR_AUTO_EXPOSURE_BOOL` / `OB_PROP_COLOR_EXPOSURE_INT`），范围可用 `Device::getIntPropertyRange` 动态取 | **本版移除该控件**，固定用自动曝光（用户裁定）。配置保留 `autoExposure` 键，默认 `true` |
| E7 | 「彩色流格式预置 `YUYV, MJPG, Y16, RGB, BGR, BGRA, RGBA`」 | **`Y16` 是深度/红外格式，彩色流没有 Y16**；且高分辨率彩色实际多为 MJPG（参考实现注释：「1280×720 不支持 RGB，优先使用 MJPG」） | 彩色格式按「实际枚举结果」填充，不从需求预置项照抄 |
| E8 | 「分辨率预置 `640x480, 848x480, 960x540, 1280x720, 1920x1080`」 | 预置项是**假设**，该机型实际支持集未知 | 从 `pipeline->getStreamProfileList(OB_SENSOR_COLOR/DEPTH)` **枚举后填充下拉框**。硬编码选到不支持的组合会直接开流失败 |
| E9 | 「在 `Initialize` 中读取 `communication.camera.sn`，通过工厂反射实例化 `OrbbecCamera`」 | 工厂反射需要的是**类型名**，SN 是**设备标识**——两者不是一回事。且现状 `HardwareManager.cpp:187` 读的是 `simulation.cameraType` | 拆成两步：`simulation.cameraType` 选型 → `communication.camera.sn` 选设备 |
| E10 | 四宫格右下结果表列：`序号, X, Y, Z, R, 耗时ms` | 现有表为 10 列（`VisionTestPage.cpp:292-298`），含 置信度 / U / V / 宽 / 高 | **保留 10 列**（进算法阶段还需要这些），另加「耗时 ms」列 |

### 1.3 判定遗漏（需求未覆盖但必做）

| # | 遗漏项 | 说明 |
| :--- | :--- | :--- |
| M1 | **配置键三套打架** | `simulation.cameraType`（`HardwareManager.cpp:187`）✓ 存在于 config；`simulation.cameraDeviceId`（`CameraManager.cpp:42`，默认 `"CAM-SIM-001"`）✗ config 里没有；`vision.frameWidth` / `frameHeight` / `frameFps`（`HardwareManager.cpp:193-195`）✗ config 里没有。三处都在读**不存在的键**，走硬编码默认值 |
| M2 | **`VisionTestPage.cpp:462` 现存缺陷** | `OnOpenCamera` 内无条件 `deviceIdEdit_->setText("CAM-SIM-001")`，把用户填的 SN 覆盖掉——SN 输入框形同虚设。**接真机前必须先删** |
| M3 | **CMake 拷 DLL 必须白名单** | 需求说「POST_BUILD 的 dll 自动拷贝」。`3rdparty/orbbec/bin/` 里有 `opencv_world340.dll`（OpenCV 3.4.0），与项目 vcpkg 的 4.12.0 混放会埋雷。**只拷 `OrbbecSDK.dll`** |
| M4 | **`build_release.bat` 打包白名单要加 `OrbbecSDK.dll`** | 该脚本打包文件集是显式白名单（TR-096 刚修好，80 文件），不补则 Release 包拷到目标机跑不起来 |
| M5 | **切相机类型需重建实例** | `HardwareManager::Initialize` 只在启动时 `CameraFactory::Create` 一次（`HardwareManager.cpp:187-188`），而型号下拉是从工厂注册表动态取的（`VisionTestPage.cpp:111`）——接入后会出现 `SimCamera` / `OrbbecCamera` 两项，切换需明确路径 |
| M6 | **`HardwareManager.cpp:182-185` 无条件给算法喂 json** | 该处无条件 `LoadConfig("{\"confidenceThreshold\":...}")`，与「算法配置可选」语义冲突（见 §二 决策 5），应改为「有配置才加载」 |
| M7 | **UI 渲染需节流** | `OnFrameReady` 目前每帧都 `RenderLatest()`（`VisionTestPage.cpp:520-528`），大分辨率下 `QImage::copy` + `setPixmap` 是主瓶颈 |
| M8 | **相机所有权与多页订阅** | `VisionTestPage` 与 `AutoRunPage` 都连 `HardwareManager::frameReady`（`VisionTestPage.cpp:123`、`AutoRunPage.cpp:43`），而 `VisionTestPage::OnStopStream` 停的是**全局流**（`:514`）。现靠 Sim 无副作用掩盖。**已裁定改「持有者集合」模型 → 见 §2.1** |
| M9 | **检测结果算出后无处消费** | 识别动作的执行分支（`SequenceWorker::StepRunner` 的 `case ActionType::Vision`）已做 `CameraToRobot` 手眼换算，但 `robot` 曾是局部变量、只打日志后丢弃——方案里没有任何动作能消费它。**已裁定新增「视觉引导」动作（P5 已落地）→ 见 `doc/vision_guide_action_plan.md`** |

## 二、已确认决策

| # | 决策 | 来源 |
| :--- | :--- | :--- |
| 1 | **`ICamera` 不重写**，只在需要时做非纯虚扩展（设备枚举）；`CameraFrame` 保持 `std::vector` 不引入 `cv::Mat` | 本文评审 E1 / E2 |
| 2 | **对齐策略：高分辨率走软件对齐（`ob::Align` 逐帧滤波器）**；HW 对齐仅在 640×480 彩色 + 848×480 深度档可用。不使用 `config.set_align_mode(SW_MODE)` | 用户实测 |
| 3 | **不注册 SDK 设备插拔回调**，改轮询式探测 + 分级恢复 | 用户实测（回调导致崩溃） |
| 4 | **曝光控件本版移除**，固定自动曝光 | 用户裁定 |
| 5 | **算法模板配置是可选填项**：配置数据会随算法版本迭代，不填则用算法内置默认值 | 用户裁定 |
| 6 | **`AutoRunPage` 只订阅检测结果（叠加图 + 结果），不订阅原始帧流** | 用户裁定 |
| 7 | 常用工作档位：**彩色 1280×720 + 深度 848×480** | 用户实测 |
| 8 | 保留现有 `CameraManager` 的 `QTimer` 拉模型，`OrbbecCamera` 内部用「最新帧槽」适配 | 本文评审 E2 |
| 9 | **相机流改「持有者集合」模型**：流是全局共享资源，持有者 = `{VisionPage, AutoRun}`，**流运行 ⟺ 集合非空**；页面启停 = acquire / release | 用户裁定（§2.1） |
| 10 | **自动运行期间，视觉页的「相机开关 / 启停采集」置灰**；检测、截图、视图切换等只读操作保留可用 | 用户裁定（修正 TR-075 的既有约定） |
| 11 | **相机为眼在手外（Eye-to-Hand，固定安装）**；相机安装参数与基座标的关系标定**待实现**（现行 `tcpCalibration.handEyeMatrix` 为占位值） | 用户裁定 |

### 2.1 相机流生命周期：持有者集合模型

**背景**：`VisionTestPage` 与 `AutoRunPage` 都订阅 `HardwareManager::frameReady`，但**流的启停入口目前只在视觉页**（`VisionTestPage.cpp:463/477/496/499/514`）——`AutoRunPage` 完全没有流控制。若按「谁启动谁停止」实现，会出三类故障：

| 场景 | 后果 |
| :--- | :--- |
| 视觉页启流 → 自动运行又启一次 → 自动运行结束停流 | 切回视觉页**黑屏**（流已被别人停掉） |
| 自动运行中，视觉页点「停止采集」（停的是全局流） | **打断正在执行的识别动作**（拿不到帧） |
| 自动运行启流 → 切回视觉页 | 页面不知道该显示还是该禁用 |

**根因**：「流的生命周期」被绑在了「页面操作」上，而流实际是**全局共享资源**。

**模型**：流归 `HardwareManager` 单例持有，内部维护**持有者集合**：

```
持有者集合 = { "VisionPage", "AutoRun" }
流运行 ⟺ 集合非空
```

| 入口 | 动作 |
| :--- | :--- |
| 视觉页「开始采集」 | `AcquireCamera("VisionPage")` |
| 视觉页「停止采集」 | `ReleaseCamera("VisionPage")` |
| 自动运行启动 | `AcquireCamera("AutoRun")` |
| 自动运行结束 / 停止 / 出错 | `ReleaseCamera("AutoRun")` |

视觉页释放时若自动运行仍持有 → **不真停**，状态提示「自动运行仍在使用相机」。

**「切回视觉页要不要显示」的答案：要显示，且这不是可选项**——视觉页订阅的是 `frameReady`，只要流在跑就必然有帧；要让它不显示反而需额外写代码。这也正是 TR-075 把视觉页定义为「生产观察通道」的本意。

**由此产生一处对 TR-075 的修正**：`VisionTestPage.cpp:28-31` 注释写「自动模式下…操作类控件（相机开关/采集/检测/视图切换/截图）保持可用」——其中**相机开关与启停采集必须改为置灰**（否则上表第二类故障必然发生）；检测、截图、视图切换等只读操作保留可用。

**实现落点**：`HardwareManager` 新增 `AcquireCamera(const std::string& owner)` / `ReleaseCamera(const std::string& owner)`，内部用 `std::set<std::string>` 记录持有者；集合由空变非空时启动流、由非空变空时停止流。现有 `StartCameraStream` / `StopCameraStream`（`HardwareManager.h:131-132`）改造为 acquire/release 的薄封装（或保留为无持有者的裸接口供调试）。`VisionTestPage::SetParamsLocked`（`:160-168`）的控件清单同步调整。

## 三、探查核实的关键事实

### 3.1 现状代码

- **`ICamera` 已存在**，9 个纯虚方法（`src/HAL/interfaces/ICamera.h:26-45`）；`CameraFrame{timestamp, width, height, colorData, depthData, pointCloud}`（`:16-24`），深度为 `float`（mm）。
- **取帧链路**：`CameraManager`（`QThread` + `CameraCaptureWorker`）→ `CameraCaptureWorker::CaptureTick` 每 tick 调 `camera_->CaptureFrame()`（`CameraCaptureWorker.cpp:56-62`）→ `HardwareManager::frameReady` 转发（`HardwareManager.cpp:89`）。
- **工厂注册范式**：`REGISTER_CAMERA("SimCamera", SimCamera)`（`SimCamera.cpp:10`），由 `CameraFactory::Instance().Create(typeName)` 创建（`HALFactory.h:104-115,61`）。
- **帧转换**：`FrameConverter::ColorToQImage` 只认 **RGB888**（`FrameConverter.cpp:14-18`）——**所有非 RGB 输入必须在相机实现内归一化，否则 UI 花屏**。`DepthToQImage` 接受 `float` mm 并按伪彩渐变渲染（`:54-81`）。
- **`HardwareManager::Initialize` 相机段**（`:187-197`）：读 `simulation.cameraType` → `Create` → `SetCamera` → `CameraOpen(w, h, fps)`。
- **`FrameSaver`** 已实现异步存图（独立 `QThread`，`VisionTestPage.cpp:125-133`）——需求要的「保存 RGB / Depth / 叠加图」可直接复用，只需加三路开关。

### 3.2 Orbbec SDK 2.9.3

- 已在 `3rdparty/orbbec/`：`include/libobsensor/{h,hpp}`、`lib/OrbbecSDK.lib`、`bin/OrbbecSDK.dll`、`examples/src`（含 `2.device.hot_plugin`、`3.advanced.hw_d2c_align`、`1.stream.multi_streams`、`5.wrapper.opencv` 可参考）。
- 版本 **2.9.3**（`lib/OrbbecSDKVersion.cmake`）。
- C++ API 要点：`ob::Pipeline`、`ob::Config`、`ob::Align`（软对齐滤波器）、`ob::DeviceList`、`Pipeline::start(config)` / `start(config, frameSetCallback)`（`hpp/Pipeline.hpp:333,346`）、`getStreamProfileList(OBSensorType)`（`:423`）、`StreamProfileList::getVideoStreamProfile(w, h, format, fps)`（`hpp/StreamProfile.hpp:523`）、`Device::getIntPropertyRange` / `setBoolProperty`（`hpp/Device.hpp:244,189`）。
- 属性枚举在 **`h/Property.h`**（不在 `ObTypes.h`）：`OB_PROP_COLOR_AUTO_EXPOSURE_BOOL = 2000`、`OB_PROP_COLOR_EXPOSURE_INT = 2001`、`OB_PROP_COLOR_GAIN_INT = 2002`。
- 对齐枚举在 `h/ObTypes.h:586-587`：`ALIGN_D2C_HW_MODE` / `ALIGN_D2C_SW_MODE`。

### 3.3 参考实现（`robot-single-thread/src/vision/orbbec_camera.py`）

| 位置 | 关键做法 |
| :--- | :--- |
| `:14` 注释 | **弃用底层 C++ 插拔回调**——「极易引发崩溃」 |
| `:31-32` | 重连冷却 `_reconnect_cooldown = 3.0` s，防重连风暴 |
| `:64-65` | 软对齐滤波器创建：`AlignFilter(align_to_stream=OBStreamType.COLOR_STREAM)` |
| `:74-94` | 彩色格式**尝试队列** `[RGB, YUYV, MJPG]` 逐个试，全失败才回落默认流 |
| `:105-107` | 深度 profile 匹配失败时回落 `enable_stream(DEPTH_STREAM)` |
| `:207-218` | 连接时**等待设备枚举最多 15 s**（0.5 s 轮询） |
| `:226` | `ctx.enable_net_device_enumeration(True)` 支持网口相机 |
| `:232-235` | 无设备时**强行销毁 `Context`**，逼 SDK 重新扫描 USB 枚举——解决「插上了但软件认不出」 |
| `:243-256` | **僵尸设备探测**：主动 `dev.get_device_info()` 触发真实 USB 通讯，抛异常即判假死 → 销毁 `Context` + 等 2 s |
| `:294-333` | `pipeline.stop()` 在设备拔出后会抛 `Bad Magic` 甚至段错误 → **必须吞异常 + `finally` 强制置空** |
| `:479-494` | 彩色帧转换：`MJPG` 走 `cv2.imdecode` 再转 RGB；`RGB` 直接 reshape。（**YUYV 分支未实现**——移植时须补） |
| `:496-499` | 深度帧：`np.uint16` reshape，即 **Y16** |
| `:524-536` | **软对齐可能因光线突变偶发失败** → 只跳过该帧，**不判为断线** |
| `:553-556` | 帧取完立即释放底层引用，不让 C++ 对象离开函数 |
| `:559-565` | 帧率健康：保留最近 30 帧时间戳，低于 **2 fps** 视为流异常 |
| `:575-599` | `flush_frames` 排空积压帧（给自动曝光留时间） |
| `:655-660` | 定期**深冲洗**：每 600 s 丢 30 帧，防网口 RTP/UDP 缓冲饱和 |

`detect_algo.py` 侧的恢复策略（`:225-321`）：

| 触发条件 | 动作 |
| :--- | :--- |
| 连接成功后 | **等 1.5 s 光学传感器预热** + `flush_frames(2)`，否则立刻取图会「假掉帧」 |
| 连续 5 次掉帧 | 深冲洗 30 帧 + 等 2 s |
| 连续 20 次掉帧 | **Pipeline 软重启**（`disconnect` + 等 2 s） |
| 连续 40 次掉帧 | **全量 `Context` 重建**（`hardware_reset`）+ 等 6 s，计数退回 2 |
| 连续 10 次连不上 | 同上全量重建 + 等 6 s，计数退回 5 |
| 后台 `keep_alive` 每 30 s | `is_alive()` + `check_device_exist()` + `check_stream_responsive(800ms)`；连续 3 次超时 → 深冲洗 10 帧 |

**两条最硬的教训**：

1. `:127-128` 注释——「**拔掉瞬间直接去取图会导致底层的 `wait_for_frames` 直接崩溃**」，所以必须先探活再取帧。
2. `:96-121` 提供**主动热重启**接口（加工周期结束后刷新 USB 链路），防待机假死。

## 四、相机去抖与断线恢复（经验移植）

> 项目术语：**去抖 (Debounce)**，不用「防抖」（`doc_output_standards_guide.md:73`）。本节的「去抖」指**连接状态去抖 + 曝光稳定**两层，不是图像防抖。

### 4.1 为什么不用 SDK 插拔回调

`Context::registerDeviceChangedCallback` 的 Python 侧对应实现已在生产环境验证过，结论是**弃用**：回调在 SDK 内部线程触发，设备拔出瞬间底层状态不一致，回调路径上的异常**不会正常返回**，直接导致进程崩溃。C++ 侧是同一套 SDK 实现，风险同源。

替代方案：**轮询探测**。项目已有现成范式——`HardwareManager::pollTimer_`（50 ms 主轮询）内做边沿检测与降频计数（`servoPollCounter_`、`servoNextReconnectMs_`、`servoReconnectBackoffMs_`，见 `HardwareManager.h:188-199`），舵机热重连的指数退避（2 s→4 s→…→30 s cap）与「重连成功也冷却 30 s」的教训可直接复用。

### 4.2 分级恢复策略（C++ 侧设计）

| 层级 | 触发 | 动作 |
| :--- | :--- | :--- |
| L0 探测 | `pollTimer_` 降频（如每 500 ms） | 先 `IsAlive()`（设备枚举计数 > 0）**再**取帧；连续失败计数 +1 |
| L1 轻度 | 连续 5 次取帧失败 | 丢弃积压帧（`flush` 语义）+ 退避 2 s |
| L2 中度 | 连续 20 次 | `pipeline->stop()` + 重建 pipeline（吞异常） |
| L3 重度 | 连续 40 次 | 全量重建 `Context` + 等 6 s，计数退回低档 |
| L4 连接 | 连续 10 次 `Open` 失败 | 同 L3 |
| 僵尸 | 枚举到设备但 `getDeviceInfo()` 抛异常 | 销毁 `Context` + 等 2 s |
| 冷却 | 任何重连动作 | 3 s 内不重复发起 |
| 预热 | 每次连接成功后 | 等 1.5 s + 丢弃 2~5 帧，稳定自动曝光 |

### 4.3 状态上报

`ICamera` 非 `QObject`，无法直接发信号。方案：`OrbbecCamera` 维护**状态快照**（`enum class CameraLinkState { Disconnected, Connecting, Streaming, Degraded }` + `lastError`），由 `HardwareManager::PollTick` 边沿检测后经现有 `connectionChanged()` / 新增 `cameraStateChanged(CameraLinkState)` 广播给 UI。**不在相机实现里碰 Qt 对象**（沿用「HAL 底层不依赖 Qt 信号」的既有边界，见 `HardwareManager.h:229-232` 的 `CameraManager` 委托设计）。

## 五、实现方案（S0–S6）

### S0 前置修复（不依赖 SDK，可立即做）

1. 删 `VisionTestPage.cpp:462` 的 SN 覆盖行（M2）。
2. 统一配置键（M1）：相机**选型**留 `simulation.cameraType`（与 `motionCardType` / `servoType` 同族）；相机**通信与流参数**归 `communication.camera.*`（`sn` 已在此处）；删除对 `simulation.cameraDeviceId`、`vision.frameWidth` / `frameHeight` / `frameFps` 的读取，改读新键。
3. 同步 `doc/config.md` 的 `vision` 段（`:110-117`）。
4. **删除死代码 `PickCycleController`**（用户 2026-09-17 裁定）：删除 `src/Logic/PickCycleController.h/.cpp` 与 `src/Logic/CMakeLists.txt:6,11` 的引用。依据：全库无调用点；且其 `ExecuteOneCycle`（`:146-153`）把**相机系坐标当基座坐标**用（跳过 `CameraToRobot`），留着是隐患。文档引用同步清理：`AGENTS.md:80/283/316/328`、`doc/architecture.md:47`、`doc/auto_run_loop_production.md:22,460`（`doc/worklog/` 按项目规约**不改历史**）。

> **✅ 落地情况（2026-09-20，TR-099）**：
> - 第 1、2 项**已在 P1（C2/C4）顺手完成**（`VisionTestPage.cpp` 的 SN 覆盖行与幽灵键读取均已删除），P3 只补了「新键真正被读」这一半——读点集中在新增的 `CameraManager::ReadCameraStreamConfig()`。
> - 第 3 项：`doc/config.md` 的 `communication.camera` 段已补全 10 个流参数键，`vision` 段标注了曝光键归属（P4）。
> - 第 4 项：`PickCycleController.h/.cpp` 已删（沙箱内 `rm` 被静默拦截，改用 `python os.remove` 并 `ls` + `git status` 复核）；`src/Logic/CMakeLists.txt` 与 5 处文档引用已同步；`doc/archive/kinematics_sequenceworker_plan.md` / `doc/test/card_axis_test.md` / `TEST_RECORD.md` 属**历史记录**，按规约不改。
> - 另补一项方案未列的：`config/config.json` 的 `communication.camera.sn` 由占位值 `"336L"` 改为真机实测 SN `CP9KB530005S`（原值在真机上匹配不到设备，每次开流都会多一条 fallback WARN；`OrbbecCamera` 的 fallback 行为本身保留）。

### S1 CMake 与 SDK 接入

- `src/HAL/CMakeLists.txt`：加 `option(USE_ORBBEC "Build Orbbec 3D camera support" OFF)`；`HAL_SOURCES` 加 `camera/OrbbecCamera.cpp`；`HAL_HEADERS` 加 `camera/OrbbecCamera.h`；`target_include_directories(HAL PRIVATE ${CMAKE_SOURCE_DIR}/3rdparty/orbbec/include)`；`target_compile_definitions(HAL PRIVATE USE_ORBBEC)`；`target_link_libraries(HAL PRIVATE ${CMAKE_SOURCE_DIR}/3rdparty/orbbec/lib/OrbbecSDK.lib)`。
- 根 `CMakeLists.txt`：照 `BOPAI_DLLS` 范式（`:140-151`）加 POST_BUILD，**只拷 `OrbbecSDK.dll` 单文件**（M3）。
- **验收**：`USE_ORBBEC=ON` / `OFF` 两种配置均可编译。

> **✅ 落地情况（2026-09-20，TR-099）**：`USE_ORBBEC` 默认 **OFF**（SDK 是闭源二进制，仅真机需要；OFF 时 `OrbbecCamera.cpp` 不参与编译，`simulation.cameraType` 回落 `SimCamera` 即一键回仿真）。**ON / OFF 两种配置均已实测编译通过**（Debug）。POST_BUILD 用**显式白名单**拷 `OrbbecSDK.dll` + `extensions/`（实测输出目录无 `opencv_world340.dll`、无 60+ 个 `ob_*.exe` 示例）。`build_release.bat` 打包段同步补了 `extensions\` 目录（顶层 `*.dll` 通配本就覆盖 `OrbbecSDK.dll`）。

### S2 设备枚举（真机里程碑 1）

- `ICamera` 加非纯虚方法：`virtual std::vector<CameraDeviceInfo> EnumerateDevices() { return {}; }`（`CameraDeviceInfo{id, name, connection}`），`SimCamera` 零改动。
- `OrbbecCamera::EnumerateDevices()` 走 `Context::queryDevices()` + `enableNetDeviceEnumeration(true)`。
- UI：「刷新设备」按钮 → SN 下拉框填充。
- **验收**：插上相机点刷新，下拉框出现真实 SN。

> **✅ 落地情况（2026-09-20，TR-099）**：真机枚举通过——`id='CP9KB530005S'  name='Orbbec Gemini 336'  conn='USB3.2'`。
> 视觉页的 SN 控件由 `QLineEdit` 改为 **`QComboBox` + 「刷新」按钮**（`VisionTestPage::OnRefreshDevices`）：显示串带型号（`SN (型号)`），`userData` 存**裸 SN**，选中即写回 `communication.camera.sn`。整页 2×2 四宫格重排仍归 **P4**。
> `HardwareManager::EnumerateCameras()` → `CameraManager::EnumerateDevices()` → `ICamera::EnumerateDevices()`（**非纯虚**，`SimCamera` 零改动，返回空列表）。

### S3 打通取流（需求核心）

`OrbbecCamera` 实现要点：

1. `Open(deviceId, w, h, fps)`：按 SN 匹配设备 → 创建 `Pipeline` / `Config` → **动态枚举 profile**（按 `getVideoStreamProfile` 尝试 `[RGB, YUYV, MJPG]`，失败回落默认流）→ `enableFrameSync()` → `start(config)` → 等 1.5 s 预热。
2. 对齐：按配置选择 **HW**（`Config` 的 align，仅 640×480 + 848×480 档）或 **SW**（`ob::Align(OB_STREAM_COLOR)` 逐帧 `process()`，失败只跳帧不判断线）。
3. 帧归一化：彩色 → **RGB8**（MJPG 需解码；**YUYV 分支须补**，参考实现漏了）；深度 → `float` mm 填 `depthData`。
4. **最新帧槽**：`std::mutex` + `std::shared_ptr<const CameraFrame>` 双缓冲，`CaptureFrame()` 返回副本——与现有 `CameraManager` 拉模型零改动衔接，且天然防阻塞。
5. 取帧前先探活（避免拔线瞬间崩溃）。
6. **验收**：视觉页单画面出 RGB 与 Depth 伪彩；`FrameConverter::ColorToQImage` 不花屏。

> **✅ 落地情况（2026-09-20，TR-099）**：真机取流通过（`tests/test_camera_check.exe --stream 10`）——
> ```
> 生效 profile: color 1280x720 / depth 848x480
> 采集 10.0 s：有效帧 302（30.2 fps），空帧 0
> 首帧 1280x720，colorData=2764800 字节（== w*h*3，RGB888 归一化正确），深度首值=1369.0 mm
> ```
> App 内也验过一遍（`simulation.cameraType=OrbbecCamera`）：`[OrbbecCamera] Opened SN='CP9KB530005S' color=1280x720@30 fmt=MJPG depth=848x480 align=SW`。
> **Release 60 s 长稳**（`out/build/x64-Release/test_camera_check.exe --stream 60`）：`采集 60.0 s：有效帧 1801（30.0 fps），空帧 0`，12 项检查全 PASS、退出码 0 —— **帧率零衰减、无掉帧、无崩溃**。
> **实现期间踩到 3 处 SDK 2.9.3 与预期不符，已修**：① `OBCameraIntrinsic` **不含** `k1/k2/p1/p2`（畸变在单独的 `OBCameraDistortion`）；② `ob::Frame` **没有** `getWidth/getHeight`（只有 `ob::VideoFrame` 有，须 `as<ob::VideoFrame>()`）；③ `FormatConvertFilter` 的 YUYV 分支参考实现漏了，本实现已补（`FORMAT_YUYV_TO_RGB`）。

### S4 去抖与分级恢复

按 §4.2 落地，复用 `pollTimer_` 与舵机退避范式。

- **验收**：运行中拔线 → 状态转 `Disconnected` + 自动停流；插回 → 自动恢复出图；反复插拔 10 次不崩溃、日志无刷屏。

> **✅ 落地情况（2026-09-20，TR-099）**：代码已就位——`OrbbecCamera` 内实现 L1/L2/L3 分级恢复（5/20/40 次失败，3 s 冷却，L3 重建 `Context`）；`HardwareManager::PollCamera()` 以 **500 ms 降频**（50 ms tick × 10）做链路状态**边沿检测**并广播 `cameraStateChanged(CameraLinkState)`，**恢复动作一律不在 PollTick 里做**（L3 要 sleep 6 s，放 50 ms 轮询里会卡死 UI 线程）。
> **⚠ 本项验收尚未完成**：拔插线必须**人工动手**（沙箱与自动化都无法模拟），请在 `test_camera_check --stream 700` 运行期间拔线再插回，观察日志中链路状态是否 `Disconnected → Connecting → Streaming` 且进程不退出。
> **已有部分证据（但不构成验收）**：Release 配置连续取流 **60 s（1801 帧 / 30.0 fps / 空帧 0 / 退出码 0）** 未见帧率衰减或崩溃——这只覆盖「静置长稳」的一半，**完全没覆盖拔插线**；10 min 长稳也仍待跑。

### S5 视觉检测页重构（可与 S2–S4 并行，SimCamera 即可验收）

> **详细规格见 `doc/vision_test_page_redesign_plan.md`**（2026-09-17 补齐）——本节只保留与相机接入强耦合的要点；控件清单、布局细节、旧功能映射对账、评审项 V1–V12 与验收判据**一律以该文档为准**，勿在此处重复维护。

- 左 320 px `QScrollArea` + 右侧 2×2 四宫格（RGB / Depth / Overlay / 数据+日志）。
- 相机控制组：刷新设备 + SN 下拉、彩色/深度两套流参数（**枚举填充**）、`D2C 软对齐` 复选框、`[开始采集]` / `[停止采集]`。
- 算法测试组：算法引擎下拉、**模板配置（可选填）** + `[浏览...]`、存图三开关（复用 `FrameSaver`）、`[单次检测]` / `[连续检测]` / `[加载本地图片]`。
- **移除曝光控件**（决策 4）；**移除视图切换按钮**（三格同时显示，不再需要切换）。
- 启停采集改走持有者集合（决策 9 / §2.1）。
- **验收**：Sim 模式下四宫格全功能、随窗口自适应。

### S6 收口

1. 检测结果通道（决策 6）：新增 `detectionReady` 信号（叠加图 + 结果），`AutoRunPage` 改订阅它，与视觉页解耦；`AutoRunPage` 的原始帧订阅移除。
2. **相机流持有者模型（决策 9 / §2.1）**：`HardwareManager` 新增 `AcquireCamera(owner)` / `ReleaseCamera(owner)` + 持有者集合；自动运行启动 / 结束分别 acquire / release，视觉页启停改走同一对接口。
3. **视觉页互锁修正（决策 10）**：自动运行期间置灰「相机开关 / 启停采集」——改 `VisionTestPage.cpp:28-31` 注释与 `SetParamsLocked`（`:160-168`）控件清单；检测 / 截图 / 视图切换保持可用。
4. 渲染节流（M7）。
5. `build_release.bat` 打包白名单加 `OrbbecSDK.dll`（M4）。
6. `AGENTS.md` / `doc/architecture.md` / `doc/config.md` 同步；`TEST_RECORD.md` 追加 **TR-097 起**。
7. **验收**：Debug + Release 双编译 EXITCODE=0 + Sim 冒烟 `Initialize complete` + 打包文件集核对。

## 六、配置 schema 变更

```jsonc
"communication": {
    "camera": {
        "sn": "336L",              // 设备序列号（已存在）
        "colorWidth": 1280,        // 常用档：1280×720（决策 7）
        "colorHeight": 720,
        "colorFps": 30,
        "colorFormat": "MJPG",     // 高分辨率彩色实际为 MJPG
        "depthWidth": 848,         // 常用档：848×480
        "depthHeight": 480,
        "depthFps": 30,
        "depthFormat": "Y16",
        "d2cAlign": "SW",          // "HW" 仅 640×480 + 848×480 档可用；高分辨率必须 "SW"
        "autoExposure": true       // 本版固定 true
    }
}
```

`simulation.cameraType` 保持现状（`"SimCamera"` / `"OrbbecCamera"`）。

## 七、风险与回退

| 风险 | 影响 | 回退 |
| :--- | :--- | :--- |
| SDK 崩溃（回调 / `stop()` / 拔线瞬间取帧） | 进程退出 | 不注册回调、吞 `stop()` 异常、取帧前探活；`USE_ORBBEC=OFF` 一键回到 Sim |
| 软对齐偶发失败 | 单帧丢失 | 只跳帧不判断线（参考实现同款处理）；连续失败升级到 L1 |
| 高分辨率帧拷贝开销（1280×720×3 + 848×480×4 ≈ 4.3 MB/帧，30 fps ≈ 130 MB/s） | UI 掉帧 | 渲染节流（M7）；`AutoRunPage` 退出原始帧订阅（决策 6） |
| `opencv_world340.dll` 混入输出目录 | 运行时 DLL 冲突 | POST_BUILD 白名单只拷 `OrbbecSDK.dll`（M3） |
| Release 包缺 `OrbbecSDK.dll` | 目标机跑不起来 | 同步 `build_release.bat`（M4） |
| 分辨率组合不被支持 | 开流失败 | 下拉框从 SDK 枚举填充（E8），而非硬编码预置 |
| 配置键迁移遗漏 | 读到默认值 | S0 统一 + `doc/config.md` 同步 + 日志打印实际生效值 |

## 八、验收与台账

- 每阶段：**Debug + Release 双编译**（EXITCODE=0）+ **Sim 冒烟**（判据 = 当日日志含 `Initialize complete`）。
- 真机项：设备枚举、取流、软对齐、插拔恢复、分辨率切换——**须真机执行**，Sim 无法替代。
- 台账：从 **TR-097** 起逐阶段追加（当前最大 `TR-096`）。
