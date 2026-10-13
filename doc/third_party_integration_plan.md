# 第三方对接方案（第三方调用方案 + 拍照动作）

> **性质**：⏳ **计划中**（2026-10-09 成稿）——本文是**方案**，不是已落地实现。技术事实（架构、参数、行为、踩坑）一律以 `AGENTS.md` 与源码为唯一权威源；本文与二者冲突时以二者为准。
>
> **需求来源**：2026-10-09 用户提出「第三方软件调用工艺流程方案执行动作 + 采集流程中拍摄的照片做模型分析」，并分三批对 **21 项**议题作出裁定（第一批 13 项 + 第二批 8 项，见 §一）。
>
> **实施状态**：未实施。**前置阻塞项**见 §十一（`CaptureFrame()` 并发互斥），须先修。
>
> **配套文档**：`doc/process.md`（`process.json` schema）、`doc/config.md`（`config.json` schema）、`doc/architecture.md`（分层依赖）、`doc/camera_integration_plan.md`（相机 HAL）、`doc/multi_camera_plan.md`（`cameraId` 字段）、`doc/action_tree_concurrency_plan.md`（资源互斥校验）。



---

## 一、需求与裁定

### 1.1 需求原文

第三方软件需要：① 调用本软件的**工艺流程方案**执行动作；② 采集流程中拍摄的**照片**，交给自带的模型分析模块做缺陷分析。

### 1.2 第一批 13 项裁定（用户，2026-10-09）

| #  | 议题      | 裁定                                    | 对本方案的影响                                                                  |
| :- | :------ | :------------------------------------ | :----------------------------------------------------------------------- |
| 1  | 第三方位置   | **另一软件 / 另一台机器**，需跨机通信                | 排除命名管道，**必须 TCP**；本软件作 server，第三方作 client                                |
| 2  | 交付方式    | **路径引用**与**直接推送**两者可单选、可同选            | 做成**两个独立开关**，不是三选一的枚举                                                    |
| 3  | 拍照标识    | 动作级增加标识（数字 / 字符串），用于缺陷类型分类；路径引用时随通知推送 | `ActionData::captureTag`（**统一按字符串存**，数字由第三方自行解析）                         |
| 4  | 静止 / 运动 | **由方案编排决定**：眼在手外时机械臂可动；眼在手内时不可动       | **无需新增开关**——现有资源模型天然支持（§1.3）                                             |
| 5  | 图像内容    | 默认彩色；相机支持时可选**深度**与**点云**             | 深度有数据可直接交付；**点云当前无数据源**（§二.5）                                            |
| 6  | 触发时机    | 由**工艺流程节点**触发                         | 走新增动作类型，不是外挂触发                                                           |
| 7  | 第三方权限   | **只允许整体方案的启动 / 停止**                   | 控制命令仅 `run` / `stop` 两条（另有握手 `hello` 与只读 `query`）；不开放单步 / 点动 / 改参数 / 改方案 |
| 8  | 停止语义    | 远程停止用 `StopImmediate()`；**急停不开放远程**   | `EmergencyStop()` 保留为现场安全回路专属                                            |
| 9  | 并发连接    | **不允许多个第三方同时连接**                      | 单连接独占，第二个连接在 accept 层被拒                                                  |
| 10 | 推送图片格式  | 可选，且需说明**省流**与**无损**的区别               | 新增 `captureFormat`；区别见 §六                                                |
| 11 | 认证方式    | **token + IP 白名单**                    | 双层：白名单在 accept 层，token 在握手层（§4.5）                                        |
| 12 | 方案引用    | 需要**稳定的 `schemeId`**                  | `SchemeData` 新增 `schemeId`，第三方按 id 而非名称引用（§八）                            |
| 13 | 启动门禁    | 第三方启动前要求**已回零 + 已使能**                 | **现有门禁已具备两道**（§二.3），只需把拒绝原因回传                                            |

### 1.3 第一批裁定 4 的落点：不需要新开关

本项目为**眼在手外**（`AGENTS.md` 明确）。拍照动作占用的资源是 `{Camera}`，移动动作占用的资源是 `{Axis_J1, Axis_J2, Axis_Z, Axis_R}`（`ProcessManager.cpp:304` 的 `resourceKeysOf`）——**两者不相交**。因此：

- **放进分组** → 与移动动作并发（机械臂可动，拍的是「此刻最新帧」）
- **不放进分组** → 与移动动作串行（机械臂静止）

这就是「由方案编排确定」的现成落点，UI 上不需要任何额外开关。

> ⚠ **语义陷阱（须写入用户手册）**：并发时拍的是**此刻最新帧**，与移动进度**没有时序关系**。要表达「走到某点之后拍」，必须串行编排（不放进分组），不能靠并发。
>
> ⚠ **眼在手内不支持**：本项目相机装于机外固定位。将来若改为眼在手内（相机装末端），拍照动作必须声明**独占四轴**的资源集合，否则会拍到运动模糊的画面。届时需另开一条资源声明，不在本方案范围。

### 1.4 第二批 8 项裁定（用户，2026-10-09）

| #  | 议题               | 裁定                                                            | 对本方案的影响                                                                                        |
| :- | :--------------- | :------------------------------------------------------------ | :--------------------------------------------------------------------------------------------- |
| 1  | 拍照失败语义           | **默认中止流程**（fail-closed）；新增 `captureRequired` 开关，**默认 `true`** | `ActionData::captureRequired`；`false` 时拍失败只记日志 + 发 `captureFailed` 事件后**继续执行**（§5.6）           |
| 2  | 节点级重试            | `Capture` **不参与**节点级重试                                        | ⚠ 现有 `retryable` 表达式已是**白名单**（仅 Vision / Move / 分组），`Capture` 天然不在内 → **无需改代码**，只需补注释与用例（§3.4） |
| 3  | 推送格式             | `captureFormat` 默认 `"png"`；**JPEG 质量不可配**                     | 质量参数写死为常量，不进 `process.json` 也不进 `config.json`（§6.4）                                            |
| 4  | `bindAddress` 语义 | **本轮为澄清项**（见 §9.5）                                            | 它是**本机（机器人侧）监听绑定的本地网卡地址**，**不是**第三方地址；第三方地址归 `allowedClients`。取值（留空 = 全网卡 / 绑具体网卡）待用户确认        |
| 5  | `schemeId` 形态    | **8 位十六进制短串**，须保证不重复                                          | 32 bit 空间；唯一性靠生成时查重 + **即时落盘**（§8.2 / §8.3）                                                    |
| 6  | 显式握手             | **不做**「第三方 ACK 后动作才结束」的握手                                     | 确认 §5.1「交付与执行解耦」。⚠ 与 `hello`（**认证握手，保留**）不是一回事——token 认证必须有应用层交换（§4.4）                         |
| 7  | 点云               | **本期不做**                                                      | `captureContent` 的 `"color_depth_cloud"` 选项**不上线**，只留 `"color"` / `"color_depth"`（§3.5）        |
| 8  | `query` 命令       | **开放**                                                        | 命令集确定为 **4 条**：`hello` / `run` / `stop` / `query`（§4.4）                                        |

---

## 二、现状核实（关键事实与证据）

> 全部结论均由源码 / 离线文档核实，非推断。带 `file:line`。

| #  | 事实                           | 证据                                                                                                                                                                                                                                                                                                                                                                           | 对本方案的影响                                                                                                                |
| :- | :--------------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------------- |
| 1  | **全库零网络与 IPC 能力**            | 全仓检索无 `QTcpServer` / `QLocalServer` / `QNetworkAccessManager` / `Qt6::Network`；`CMakeLists.txt` 只 `find_package(Qt6 ... Core Widgets)`                                                                                                                                                                                                                                       | 接口层**从零新增**，并须补链接 `Qt6::Network`                                                                                       |
| 2  | **执行引擎的事件语义已完备**             | `SequenceWorker.h:150-177` 已提供 `actionStarted` / `actionFinished` / `groupStarted` / `groupFinished` / `schemeFinished` / `cycleChanged` / `stateChanged` / `errorOccurred` / `detectionReady` / `visionResultReady`；公开入口 `RunSequence` / `RunSingleAction` / `Stop` / `StopImmediate` / `Pause` / `Resume`（`:95-137`）                                                       | 接口层本质是「**订阅信号上行 + 把命令翻译成 `RunSequence` / `StopImmediate`**」——**`SequenceWorker` 一行不用改**。这是整套方案能做得小的根本原因                |
| 3  | **启动门禁已含「已使能 + 已回零」两道**      | `SequenceWorker.cpp:219` 起的 `RunSequence` 入口：先查 `HardwareManager::IsGlobalEnabled()`（拒绝文案「轴未使能，请先手动使能」），再查 `IsSystemHomed()`（拒绝文案「系统未回零，请先一键回零」），均发 `errorOccurred`                                                                                                                                                                                                          | 用户第 13 项**无需新增门禁**——只需把 `errorOccurred` 的文案转发给第三方。⚠ **更正既有记忆**：项目记忆曾记「现有门禁只拦『未使能』」，**该条不成立**，回零互锁（2026-08-31 引入）已在同一入口 |
| 4  | **`CaptureFrame()` 无管线级互斥**  | `CameraCaptureWorker.cpp:58` 的 `CaptureTick` 在**独立采集线程**按 fps 调 `camera_->CaptureFrame()`；`SequenceWorker.cpp:999` 在 worker 线程也调同一个 `camera->CaptureFrame()`。而 `OrbbecCamera::CaptureFrame()`（`:623`）内部直接操作 `pipeline_` / `align_` / `colorConv_`，**只有 `frameMutex_`（护 `lastGoodFrame_`）与 `errMutex_`（护 `lastError_`），没有管线级互斥**；`HandleGrabFailure()` 还会 `DestroyPipeline()` | **前置阻塞项**（§十一）：两个线程可同时进入同一 pipeline，且其中一个可能正在重建管线 → UB / 随机崩溃。拍照动作会把这个路径高频化，**必须先修**                                   |
| 5  | **点云没有数据源**                  | `pointCloud` 全库仅 `ICamera.h:41` 一处**声明**，无任何实现填充                                                                                                                                                                                                                                                                                                                             | 界面上的「点云」勾选框若先上线，勾了也拿不到数据 → **已裁定本期不做**（§1.4 裁定 7），该选项不上线                                                               |
| 6  | **`FrameSaver` 是异步队列且超限丢最旧** | `FrameSaver` 队列上限 30 张，超限丢最旧                                                                                                                                                                                                                                                                                                                                                 | 拍照动作**不复用它**，改**同步落盘**（§五）——「丢图不可接受」且需保证「落盘完成才能发通知」                                                                    |
| 7  | **`ProcessPage` 详情页索引会撞号**   | `ProcessPage.cpp:93` 的 `kGroupPageIndex = 7`；`:917` 用 `static_cast<int>(node->type) + 1` 当 stack 索引                                                                                                                                                                                                                                                                          | 新增 `Capture = 6` → 索引 7 **与分组页撞号**，`setCurrentIndex` 越界被 Qt 静默忽略 → 界面错显。必须把分组页挪到 8（§3.3）                               |
| 8  | **相机持有登记是「消费前置」**            | `AutoRunPage.cpp:651` 在启动时 `AcquireCamera("AutoRun")`，`:955` 在 Idle / Fault 释放。现有「识别」动作靠这条登记让流保持运行（`SequenceWorker` 取帧是直接 `hw.camera()->CaptureFrame()`，不自己登记）                                                                                                                                                                                                               | **第三方调用不经 `AutoRunPage`，这个登记不会发生** → 接口层必须**自持** `AcquireCamera` 登记（§4.2）                                              |
| 9  | **相机内参由 SDK 真实填充**           | `OrbbecCamera.cpp:388-397` 在 `Open()` 后从 SDK 写入 `intrinsics_.fx/fy/cx/cy/k1/k2/p1/p2/width/height`；`GetIntrinsics()`（`:833`）返回该成员                                                                                                                                                                                                                                            | 元数据契约里的 `intrinsics` **可取到真值**（`SimCamera` 为默认值 640×480）                                                               |
| 10 | **手眼矩阵来源明确**                 | `CoordTransform.h` 的 `SetHandEyeMatrix` 注明源为 `config.tcpCalibration.handEyeMatrix`（4×4 row-major，16 元素）                                                                                                                                                                                                                                                                      | 元数据契约里的 `handEyeMatrix` 直接读该配置键                                                                                        |


## 四、接口层（新增 `src/Interface/`）

### 4.1 位置与依赖方向

新增第 6 个源码子目录 `src/Interface/`，作为**与 `src/UI/` 平级的适配层**（不是 `Logic` 的一部分——`Logic` 不应知道外部接口的存在）：

```
依赖方向： Interface → Logic（RunSequence / StopImmediate / 订阅信号）
           Interface → HAL   （仅 AcquireCamera / ReleaseCamera / IsGlobalEnabled / IsSystemHomed 门禁查询）
           Interface → Config（读 interface.* 配置）
           Interface → Qt6::Network
装配点：   MainWindow 构造期创建 InterfaceServer，注入 SequenceWorker*；生命周期与主窗口一致
```

新增文件：

| 文件                                       | 职责                                               |
| :--------------------------------------- | :----------------------------------------------- |
| `src/Interface/InterfaceServer.h/.cpp`   | `QTcpServer` 子类；accept 层白名单、握手认证、单连接独占、命令分派、事件上行 |
| `src/Interface/InterfaceProtocol.h/.cpp` | 帧编解码（长度前缀 + 类型字节）、JSON 载荷构造                      |
| `src/Interface/CaptureStore.h/.cpp`      | 拍照落盘：目录解析、文件名生成、sidecar JSON、manifest、保留轮转       |

CMake 改动：根 `CMakeLists.txt` 补 `find_package(Qt6 REQUIRED COMPONENTS ... Network)`、`target_link_libraries` 补 `Qt6::Network`、`add_subdirectory(src/Interface)`。

### 4.2 生命周期与相机持有

**接口层必须自持相机登记**（§二.8）。`InterfaceServer` 在**服务启用且收到首个合法 `run` 命令**时 `HardwareManager::instance().AcquireCamera("Interface")`，在会话结束（`stateChanged` 到 `Idle` / `Fault`）时 `ReleaseCamera("Interface")`。

理由：`SequenceWorker` 取帧是**直接调** `hw.camera()->CaptureFrame()`（`SequenceWorker.cpp:999`），**不自己登记**；现有识别动作之所以能取到帧，是靠 `AutoRunPage.cpp:651` 的 `AcquireCamera("AutoRun")`。第三方调用**不经过该页**，缺这条登记则流未启动，`OrbbecCamera::CaptureFrame()` 在 `!wantStreaming_` 时返回**全零空帧**且不触发重连。

> 持有者名沿用既有命名风格（`"VisionPage"` / `"AutoRun"` / `"AutoRunPreview"`）→ 新增 `"Interface"`。`ReleaseCamera` 幂等，未持有只记一行日志（`HardwareManager.cpp:367`）。

### 4.3 传输与帧协议

**TCP 长连接**，`QTcpServer` 监听，第三方作 client。TCP 上要同时跑 JSON 与二进制，必须有帧协议：

```
[4 字节 大端长度][1 字节 类型][载荷]
   0x01 = JSON（UTF-8）
   0x02 = 二进制（图片数据）
```

推送图片时**先发一条 JSON 描述帧**（含 `bytes` / `fileName` / `tag` / 元数据），**紧接着发二进制帧**，接收方按序配对。这样 JSON 帧可单独抓包调试，且不把 base64 内嵌（一帧 2.6 MB 内嵌会拖垮链路）。

### 4.4 命令集与事件集

**下行命令（只有 4 条，其中 1 条只读）**：

| 命令      | 载荷                                                                                                          | 行为                                                                                                                                       |
| :------ | :---------------------------------------------------------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------------------------------- |
| `hello` | `{"cmd":"hello","token":"...","proto":1}`                                                                   | **认证握手**（保留，见下）+ token 认证；回 `{"evt":"hello","ok":true,"sessionId":"...","schemes":[{"schemeId":"...","schemeName":"..."}]}`。**同时承担方案发现** |
| `run`   | `{"cmd":"run","schemeId":"...","loop":{"mode":"single\|count\|infinite","count":3,"returnToSafePos":true}}` | 按 `schemeId` 取方案 → `RunSequence(scheme, loop)`；门禁拒绝时回 `errorOccurred`                                                                    |
| `stop`  | `{"cmd":"stop"}`                                                                                            | 转 `StopImmediate()`（减速停，保持使能）                                                                                                            |
| `query` | `{"cmd":"query"}`                                                                                           | **只读**（§1.4 裁定 8 **开放**）：回 `WorkerState` / 门禁状态 / 当前循环进度。不改变任何状态，不扩大安全面                                                                  |

> ⚠ **两种「握手」不要混淆**（§1.4 裁定 6）：
>
> |    | `hello` 认证握手                          | 「ACK 后动作才结束」握手     |
> | :- | :------------------------------------ | :----------------- |
> | 作用 | 换 token、报 `sessionId`、列方案             | 让执行引擎等第三方确认收到图再往下走 |
> | 裁定 | **保留**（token 认证必须有应用层交换，否则裁定 11 无法落地） | **不做**（会拖死产线，§5.1） |
>
> 裁定 6 的「不做显式握手」指的是**后者**。

**明确不开放**：单步（`NextStep`）、点动（`MoveJog`）、暂停 / 继续、改方案、改参数、`EmergencyStop()`。

> ⚠ **`stop` 的语义选择**：`Stop()` 是「就近停」（当前动作跑完才退，`SequenceWorker.h:111`），`StopImmediate()` 是「减速停 + 废弃上下文退回就绪态」（`:116`）。按裁定 8 用后者。⚠ 二者**都不是**急停——急停（`EmergencyStop()`，断使能）保留为现场安全回路专属，不开放远程（开环步进断使能会因惯性滑行丢步，坐标系错乱）。

**上行事件**：直接转发 `SequenceWorker` 现有信号，**执行引擎零改动**：

| 事件                                 | 来源信号             | 载荷要点                                                                                                                                   |
| :--------------------------------- | :--------------- | :------------------------------------------------------------------------------------------------------------------------------------- |
| `stateChanged`                     | `stateChanged`   | `state` / `pauseReason`                                                                                                                |
| `actionStarted` / `actionFinished` | 同名信号             | `nodeId`（**持久化编号**，非行号）                                                                                                                |
| `groupStarted` / `groupFinished`   | 同名信号             | 分组粒度                                                                                                                                   |
| `cycleChanged`                     | `cycleChanged`   | `index`（从 1 起）/ `total`（< 0 = 无限）                                                                                                      |
| `schemeFinished`                   | `schemeFinished` | 全部循环完成                                                                                                                                 |
| `errorOccurred`                    | `errorOccurred`  | **门禁拒绝也走这条**（含「轴未使能」/「系统未回零」文案）                                                                                                        |
| `captureReady`                     | 新增（拍照动作发）        | 路径引用通道，见 §五                                                                                                                            |
| `captureData`                      | 新增（拍照动作发）        | 直接推送通道：JSON 描述帧 + 二进制帧                                                                                                                 |
| `captureFailed`                    | 新增（拍照动作发）        | **仅 `captureRequired=false` 时发**（§1.4 裁定 1）：拍照失败但流程继续。载荷含 `nodeId` / `cycleIndex` / `tag` / `reason`。⚠ **不复用 `errorOccurred`**——见 §5.6 |


### 4.5 认证：token + IP 白名单（裁定 11）

两层职责不同，**不能互相替代**：

|       | IP 白名单                                | token                         |
| :---- | :------------------------------------ | :---------------------------- |
| 判在哪一层 | TCP accept（L3/L4）                     | 应用层握手（`hello` 命令）             |
| 拒谁    | 来源地址不在名单的连接                           | 拿到连接但 token 不对的会话             |
| 能不能绕过 | 能——同网段伪造源 IP 需二层权限但不算难；**NAT 后面直接失效** | 难（前提：token 足够随机 + **常数时间比较**） |
| 误杀风险  | 有（DHCP / NAT / 多网卡）                   | 无                             |

**白名单实现位置**：重写 `QTcpServer::incomingConnection(qintptr)`（`virtual protected`），在 **accept 阶段**判定：

```cpp
void InterfaceServer::incomingConnection(qintptr fd)
{
    auto* sock = new QTcpSocket(this);
    if (!sock->setSocketDescriptor(fd)) { delete sock; return; }

    if (!IsClientAllowed(sock->peerAddress())) {
        SPDLOG_WARN("[Interface] 拒绝连接，源地址不在白名单: {}",
                    sock->peerAddress().toString().toStdString());
        sock->abort();          // 不回任何应用层数据，避免被探测
        sock->deleteLater();    // 不进 pending 队列
        return;
    }
    addPendingConnection(sock); // 交回机制，随后 newConnection() 照常发出
}
```

> ⚠ **`addPendingConnection()` 不可省**：Qt 离线文档（`D:/Qt/Docs/Qt-6.11.1/qtnetwork/qtcpserver.html`）明确——重写 `incomingConnection` 时若自建 socket，**必须**调用它把 socket 交回 Pending Connections 机制，否则 `newConnection()` 信号不发。
>
> ⚠ **为什么必须在这一步判、不能等 `newConnection()` 槽**：默认实现会把每个连接都塞进 pending 队列（`maxPendingConnections()` 默认 30）。先接受再断开会让非法连接占满队列，把**合法第三方挡在外面**——白名单本为防此，反成拒绝服务。

**白名单匹配**（支持单机与 CIDR 网段）：

```cpp
bool InterfaceServer::IsClientAllowed(const QHostAddress& peer) const
{
    const auto list = ConfigManager::instance().getValue<std::vector<std::string>>(
        "interface.allowedClients", std::vector<std::string>{});
    if (list.empty()) return true;                 // 未配置 = 不启用白名单

    bool ok = false;
    const quint32 v4 = peer.toIPv4Address(&ok);    // 归一化 IPv4-mapped
    const QHostAddress normalized = ok ? QHostAddress(v4) : peer;

    for (const auto& entry : list) {
        const auto subnet = QHostAddress::parseSubnet(QString::fromStdString(entry));
        if (subnet.second >= 0 && normalized.isInSubnet(subnet)) return true;
    }
    return false;
}
```

相关 Qt API 均已核对离线文档（`qhostaddress.html`）：

- `QHostAddress::parseSubnet(const QString&)` → `std::pair<QHostAddress, int>`（地址, 前缀长度）；**解析失败时前缀为 -1**，故必须判 `>= 0`，否则非法串会退化成「匹配任意地址」。支持 `a.b.c.d/n`、`a.b.c.d/255.255.255.255`、缩写 `192.168.0`（按实际八位组数自动判 `/24`）、IPv6 `/n`（0~128）。
- `QHostAddress::isInSubnet(const std::pair<QHostAddress, int>&)` → `bool`。
- `QHostAddress::toIPv4Address(bool* ok)` **明确支持 RFC4291 IPv4-mapped IPv6**（`::ffff:a.b.c.d`）并置 `ok = true` —— 这是上面归一化能成立的原因。

**三个会真实咬人的坑**：

| 坑                | 现象                                                                                         | 处置                                                              |
| :--------------- | :----------------------------------------------------------------------------------------- | :-------------------------------------------------------------- |
| IPv4-mapped IPv6 | 服务端 `listen(QHostAddress::Any)` 时，IPv4 客户端可能报成 `::ffff:192.168.0.50`，与白名单里的字符串不相等 → 合法连接被拒 | 上面的 `toIPv4Address` 归一化；**或把 `bindAddress` 绑成具体 IPv4 地址**，从源头避免 |
| NAT / 路由         | 第三方跨网段经路由器进来，服务端看到的是**网关 IP**，不是第三方真实 IP                                                   | 白名单填网关地址；或要求同网段直连                                               |
| DHCP 漂移          | 第三方机器重启换 IP，白名单突然失效（故障形态是「合法用户被拒」，比被入侵更难排查）                                                | 第三方配静态 IP 或 DHCP 保留；**不要用主机名**——TCP 层拿不到主机名，反向 DNS 不可靠且有延迟      |

> **建议**：`allowedClients` **默认留空**（只靠 token），只在第三方与机器人处于明确的独立网段、且 IP 可固定时再启用。同网段场景下白名单的安全增益有限，而「合法用户被拒且原因不直观」的运维成本是确定的。真要加固，比白名单更值钱的是**token 常数时间比较**与**连续握手失败 N 次后临时封 IP（指数退避）**——后者是动态的，不受 DHCP 影响。

### 4.6 单连接独占（裁定 9）

`InterfaceServer` 自持 `QTcpSocket* client_`。已有活跃连接时，新连接在 `incomingConnection` 里被拒（与白名单同层，记 WARN 日志带源地址）。断开后 `client_` 置空，允许新连接。

⚠ 需定义「活跃」的判定：`hello` 成功后即视为活跃；未 `hello` 的连接在 `idleTimeoutSec` 内无有效命令则被关闭，避免半开连接长期占位。

---

## 五、拍照动作执行时序

```
① 接口层已 AcquireCamera("Interface")           → 流运行中
② 动作进入 PrepareRunnerSegment 的 case Capture
③ InMainThread { camera->CaptureFrame() }        → 取帧（复用既有取帧路径）
④ 帧有效性校验                                    → 无效则进入 ④′
④′ 失败分支（§5.6）：
     captureRequired = true  → 置 r.failed = true → 会话级出口 → 中止整个方案
     captureRequired = false → 记 WARN + emit captureFailed → 段完成，继续下一节点
⑤ 同步落盘：写临时文件 → rename → 写 sidecar JSON → 更新 manifest
⑥ captureDeliverPath   → emit captureReady（JSON 通知）
⑦ captureDeliverStream → emit captureData（JSON 描述帧 + 二进制帧）
⑧ 段完成 → 返回 true
```

**关键约束**：

1. **交付与执行解耦**。执行只等「**本地落盘完成**」，**不等网络 ACK**。第三方网络抖动不能拖死产线；断线时事件缓存、重连补发。⚠ 这条已由 §1.4 裁定 6 明确为**不做显式 ACK 握手**。
2. **通知前图必须已就绪**。顺序固定：先写图 → `rename`（原子可见）→ 再写 sidecar → 再发通知。保证第三方收到通知时不会读到半个文件。
3. **不复用 `FrameSaver`**。它是异步队列且超限**丢最旧**（上限 30 张），而拍照动作「丢图不可接受」，且要求「落盘完成才能发通知」——异步队列反而让时序更难保证。工艺节点频率低，同步写对节拍影响可控（但 PNG 编码耗时须计入，见 §六）。`FrameSaver` 保持现状继续服务视觉页。
4. **帧有效性校验按 TR-116 契约**：
   - 彩色：`colorData.size() >= width × height × 3`
   - 深度：按 `depthWidth` / `depthHeight`（**0 = 回落 `width` / `height`**），且 `depthData.size() >= depthWidth × depthHeight`
   - ⚠ 按彩色 `width` 索引深度数据会越界闪退（`0xC0000005`，用例 11.35 实测）
5. **相机持有**：拍照动作**不自己登记**持有——由接口层在会话期统一登记（§4.2），避免动作级 acquire/release 与会话生命周期错配。

### 5.6 失败语义与 `captureRequired`（§1.4 裁定 1）

**默认 fail-closed**：`captureRequired = true` 时，取帧失败 / 空帧 / 落盘失败 → `r.failReason = "拍照失败：取帧无效"`、`r.failed = true`，走与识别动作同构的既有路径（`SequenceWorker.cpp:943` → `aborting` → `:912` `stopAndWaitAll()` → 会话出口 `:672` `emit errorOccurred` + `return false`），**整个方案中止**。

⚠ **文案须与识别动作区分**：识别失败是「视觉未检出目标」（**业务语义**——图是好的，只是没找到目标）；拍照失败是「取帧失败 / 帧无效 / 落盘失败」（**故障语义**——相机或磁盘出了问题）。二者对第三方的处置完全不同，不可共用一句文案。

`captureRequired = false` 时，同一次失败改为：

```
记 SPDLOG_WARN → emit captureFailed{nodeId, cycleIndex, tag, reason} → r.phase = PhaseDone → 继续下一节点
```

四条硬约束：

| #  | 约束                                                                 | 理由                                                                                                                                                                   |
| :- | :----------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1  | **必须发 `captureFailed` 事件**，不能只记日志                                  | 否则第三方分不清「这一轮还没拍」与「拍了但失败被跳过」——图片数量对不上却无法归因                                                                                                                            |
| 2  | **不得复用 `errorOccurred`**                                           | 已核实全库 7 处 `emit errorOccurred`（`:228` `:234` `:297` `:303` `:566` `:613` `:672`）**全部**伴随会话中止。第三方若把「收到 `errorOccurred`」当「流程挂了」，复用即误报。须保住「`errorOccurred` ⟺ 会话中止」这个不变量 |
| 3  | **必须有兜底**：连续失败 N 次（建议 N = 3）或**相机流本身掉线**（`isStreaming()` 为假）仍判失败中止 | 否则相机被拔掉后整晚会空转，第三方收到一长串 `captureFailed` 却不知道硬件已没了                                                                                                                     |
| 4  | `captureRequired = false` **只豁免拍照动作自身**                            | 不得影响其他动作类型的失败语义；也不改变交付通道行为（网络断连由接口层自己的重连/缓存处理，§5.1）                                                                                                                  |

---

## 六、推送图片格式：省流 vs 无损（裁定 10）

### 6.1 两种格式的本质区别

|                        | **PNG**                | **JPEG**                                     |
| :--------------------- | :--------------------- | :------------------------------------------- |
| 压缩类型                   | **无损**（Deflate / LZ77） | **有损**（DCT 变换 + 量化）                          |
| 色彩深度                   | 支持 8 位与 **16 位**       | **仅 8 位**                                    |
| 典型体积（1280×720 彩色）      | 约 **1.0 ~ 2.5 MB**     | q85 约 **150 ~ 400 KB**；q75 约 **80 ~ 200 KB** |
| 典型体积（848×480 深度，16 位）  | 约 **0.6 ~ 0.9 MB**     | **不支持**                                      |
| 千兆网（≈ 110 MB/s 实测）单张传输 | 约 10 ~ 25 ms           | 约 2 ~ 4 ms                                   |
| 百兆网（≈ 11.8 MB/s）单张传输   | 约 90 ~ 210 ms          | 约 15 ~ 35 ms                                 |
| 对缺陷分类的影响               | 保留原始像素，细裂纹 / 表面纹理不失真   | 高频信息被量化抹掉，**细小缺陷可能消失**                       |
| 可否复现判据                 | 可（像素级一致）               | **不可**（同图不同编码器/质量参数结果不同）                     |

> ⚠ **体积为量级估算**，非实测值。实际体积随画面内容波动很大（大面积平坦区域 PNG 可压到很小，噪声多的画面 JPEG 也压不下去）。落地时须用真机图实测并回填。

### 6.2 三条硬结论

1. **JPEG 不能用于深度图**。JPEG 只支持 8 位通道，而深度是 16 位（`Y16`）。深度只能走 PNG（`QImage::Format_Grayscale16`，Qt 5.13 起支持，`qimage.html` 已核）或原始二进制。
2. **本地留存一律 PNG**。本地磁盘不是瓶颈，而缺陷分类（尤其细裂纹、表面纹理）对高频信息敏感——有损压缩可能直接把要检的缺陷抹掉。故建议 `captureFormat` **只作用于推送通道**，本地留存固定无损。
3. **若第三方需要像素级复现判据，必须 PNG 或原始数据**。JPEG 编码结果依赖编码器实现与质量参数，不可复现。

### 6.3 ⚠ PNG 编码耗时须计入节拍

PNG（Deflate）编码 1280×720 彩色约 **30 ~ 80 ms**。同步落盘时这笔时间**直接进工艺节拍**。三个选项：

| 选项                      | 节拍代价        | 复杂度 | 时序确定性                        |
| :---------------------- | :---------- | :-- | :--------------------------- |
| **a. 同步 PNG 落盘**（建议起步）  | +30 ~ 80 ms | 低   | **高**（返回即已落盘）                |
| b. 同步写无损原始缓冲 + 后台编码 PNG | +5 ~ 15 ms  | 中   | 中（须定义「原始已落盘」还是「PNG 已生成」才算完成） |
| c. 复用 `FrameSaver` 异步队列 | ≈ 0         | 低   | **低**（丢图 + 通知时序不可控，§五.3 已排除） |

**建议**：先做 (a)，并把「PNG 编码耗时」列为真机实测项（§十二 C 档）；若实测节拍不可接受，再评估 (b)。不要为了省 50 ms 先上 (c) 而牺牲「不丢图」这一硬要求。

### 6.4 裁定：默认 PNG，JPEG 质量不可配（§1.4 裁定 3）

| 项                   | 裁定          | 说明                                                                                                                  |
| :------------------ | :---------- | :------------------------------------------------------------------------------------------------------------------ |
| `captureFormat` 默认值 | **`"png"`** | 与 §6.2 的「本地留存一律 PNG」一致，默认走无损。省流是**显式选择**的让步，不是默认                                                                    |
| JPEG 质量参数           | **不可配**     | 写死为常量（建议 q85，`QImage::save(..., "JPEG", 85)`）。**不新增** `captureJpegQuality` 字段，**不进** `process.json` / `config.json` |

**为什么质量不可配**：质量参数一暴露，就变成「每个动作可不同 → 第三方拿到的图判据不一致 → 缺陷模型无法比较不同批次」。缺陷分类要求**输入可比**，编码参数必须全局一致。若将来确实需要调，应作为**全局常量改代码**，而不是做成每动作可配的字段。

> ⚠ 注意 `captureFormat` **只作用于推送通道**；本地留存恒为 PNG（§6.2 第 2 条）。也就是说：`captureFormat = "jpeg"` 时，本地是 PNG、推送是 JPEG——同一张图两种编码，第三方若同时用了两个通道会看到体积不一致，这是**预期行为**，须写入对接说明。

---

## 七、元数据契约

### 7.1 目录结构

```
<interface.capture.outputDir>/<sessionId>/<cycleIndex>/
├── 7_20261009_173012_345.png            彩色图
├── 7_20261009_173012_345.depth.png      深度图（captureContent 含深度时）
├── 7_20261009_173012_345.json           sidecar 元数据（与图同名）
└── manifest.json                        本轮汇总（含该轮全部拍照记录）
```

- `sessionId`：一次 `run` → `schemeFinished` 为一段会话，格式 `YYYYMMDD-HHMMSS-<4 位随机>`。
- `cycleIndex`：循环轮次（从 1 起）。
- 文件名前缀 = `nodeId`（**持久化节点编号**），中段 = 拍照时刻，尾段 = 毫秒。同节点同轮多次拍照不覆盖。

### 7.2 sidecar JSON 字段

```json
{
    "sessionId": "20261009-173012-4f2a",
    "cycleIndex": 3,
    "nodeId": 7,
    "nodeName": "拍照_工位A",
    "tag": "A02",
    "timestamp": "2026-10-09T17:30:12.345+08:00",
    "cameraId": "CP9KB530005S",
    "colorPath": "7_20261009_173012_345.png",
    "colorWidth": 1280,
    "colorHeight": 720,
    "depthPath": "7_20261009_173012_345.depth.png",
    "depthWidth": 848,
    "depthHeight": 480,
    "pose":     { "x": 85.0, "y": 92.0, "z": 22.0, "r": 2.0 },
    "joints":   { "j1": 0.0, "j2": 0.0, "z": 0.0, "r": 179.9 },
    "intrinsics": { "fx": 0.0, "fy": 0.0, "cx": 0.0, "cy": 0.0,
                    "k1": 0.0, "k2": 0.0, "p1": 0.0, "p2": 0.0 },
    "handEyeMatrix": [ 1,0,0,180, 0,1,0,-5.7, 0,0,1,38.1, 0,0,0,1 ]
}
```

> ⚠ **位姿与关节角是「图 → 工件实际位置」的前提，缺了模型只能看图说话**。这比「图存哪儿」重要得多——故本节是本方案的核心契约之一，不是可选附加项。

| 字段              | 来源                                             | 备注                                                                                                 |
| :-------------- | :--------------------------------------------- | :------------------------------------------------------------------------------------------------- |
| `pose`          | `HardwareManager::GetPosition`（`:93`）四轴 + 正运动学 | 单位 mm / °                                                                                          |
| `joints`        | 同上（**逻辑关节角**）                                  | `z` 是 **Z 电机坐标**（回零后=0、向下为负），**与 `pose.z` 不是同一个量**，两者差固定偏移（`doc/process.md` 已记）                    |
| `intrinsics`    | `ICamera::GetIntrinsics()`                     | `OrbbecCamera` 在 `Open()` 后由 SDK 真实填充（`:388-397`）；`SimCamera` 为默认 640×480                          |
| `handEyeMatrix` | `config.tcpCalibration.handEyeMatrix`          | 4×4 row-major，16 元素。⚠ 当前 config 内为近似单位阵，**真机须先标定**（`doc/vision_guide_action_plan.md` 亦记「须先标定手眼矩阵」） |
| `cameraId`      | `communication.camera.sn`                      | P1 已收敛为单一键；`doc/multi_camera_plan.md` 的 `cameraId` 落地后改按动作绑定的相机取值                                  |

### 7.3 路径引用通知载荷（`captureReady`）

```json
{
    "evt": "captureReady",
    "sessionId": "20261009-173012-4f2a",
    "cycleIndex": 3,
    "nodeId": 7,
    "tag": "A02",
    "fileName": "7_20261009_173012_345.png",
    "filePath": "D:/.../capture/20261009-173012-4f2a/3/7_20261009_173012_345.png",
    "width": 1280, "height": 720,
    "depthFile": "7_20261009_173012_345.depth.png",
    "timestamp": "2026-10-09T17:30:12.345+08:00"
}
```

完整元数据以 sidecar JSON 为准（通知只带第三方立即需要的字段 + 路径，避免帧内 JSON 过大）。

### 7.4 容量保留


`interface.capture.retentionDays`（默认 30 天）。轮转策略参照 `src/main.cpp` 的 `PurgeOldLogs`——⚠ **用「最近 N 个会话目录」而非「N 天前」**：现场机器是间歇开机的，按「N 天」判会在长假后一次性清空（TR-122 已踩过同类问题，日志保留判据即因此从「7 天」改为「最近 7 个日期文件」）。

---

## 八、稳定 `schemeId`（裁定 12）

### 8.1 为什么必须有

`run` 命令若按**方案名**引用，用户在 UI 上改名就会**静默断链**——第三方下一次启动会找不到方案，且错误原因不直观。故新增稳定标识。

### 8.2 落点

| 项                  | 规则                                                                                                                                                                                                                         |
| :----------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 字段                 | `SchemeData::schemeId`（string）                                                                                                                                                                                             |
| 形态                 | **8 位十六进制短串**（§1.4 裁定 5），小写，如 `"3f9a2c71"`。**不含**连字符、不含 `0x` 前缀                                                                                                                                                            |
| 生成                 | `QUuid::createUuid().toString(QUuid::WithoutBraces)` 取前 8 位，或 `QRandomGenerator::global()->generate()` 格式化 `%08x`                                                                                                          |
| 唯一性                | ⚠ **32 bit 空间（约 4.29e9）**。按「生成 → 与现有全部 `schemeId` 比对 → 重复则重生成」实现；`generateUniqueSchemeName`（`ProcessManager.h:144`）已有同类查重做法可参照。生日界：约 **77000 个方案**才有 50% 碰撞概率，本项目方案数量级（个位数 ~ 几十）下重复概率可忽略，但**查重循环必须写**——它把「概率可忽略」变成「确定性不重复」 |
| 与 `schemeName` 的关系 | `schemeName` 是**可改的显示名**；`schemeId` 是**第三方引用的稳定键**。协议里一律用 `schemeId`                                                                                                                                                       |
| 大小写                | 生成时统一小写；解析时按**大小写不敏感**比对（第三方可能手抄成大写），避免现场「看起来一样却匹配不上」                                                                                                                                                                      |

### 8.3 ⚠ 迁移必须**即时落盘**，不能惰性升级

现有 `id` / `nextActionId` 的迁移是**惰性**的——只加载不编辑就不写盘（`doc/process.md` 已记，功能上无影响）。

**但 `schemeId` 不能照抄这个做法**：若加载时生成、却因用户从不编辑而不落盘，则**每次启动 id 都会重新生成** → 第三方保存的引用**每次重启即失效**，且故障表现为「昨天还能用今天不行」。

**故 `schemeId` 必须在首次加载发现缺失时立即写回 `process.json`**（或至少在内存中稳定并强制一次 `save()`）。这是与既有迁移策略**刻意不同**的一点，须在实现时显式标注。

### 8.4 UI 入口

`ProcessPage` 方案下拉旁增加**只读**的 `schemeId` 显示 + 复制按钮，便于现场把 id 填进第三方配置。不做编辑入口（id 一旦被引用就不该改）。

---

## 九、配置 schema 增量

### 9.1 `config/config.json` 新增 `interface` 节

```json
"interface": {
    "enabled": false,
    "bindAddress": "",
    "listenPort": 9527,
    "authToken": "",
    "allowedClients": [],
    "idleTimeoutSec": 30,
    "capture": {
        "outputDir": "capture",
        "retentionDays": 30
    }
}
```

| 键                       | 类型     | 默认          | 说明                                                                                                                                                      |
| :---------------------- | :----- | :---------- | :------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `enabled`               | bool   | `false`     | 总开关。**默认关**——不启用时接口层不监听端口                                                                                                                               |
| `bindAddress`           | string | `""`        | **本机（机器人侧）监听时绑定的本地网卡地址**——**不是**第三方地址（第三方地址归 `allowedClients`）。空 = `QHostAddress::Any`（监听全部网卡，多网卡友好）。⚠ 代价：必须做 IPv4-mapped 归一化（§4.5）。**语义详解与取值建议见 §9.5** |
| `listenPort`            | int    | `9527`      | ⚠ **须避开运动卡网段**：`communication.motionCard.ip = 192.168.0.1`（卡）、`pcIp = 192.168.0.100`（本机）。端口亦须避开 8080 / 3389 等常用端口                                       |
| `authToken`             | string | `""`        | 空 = **拒绝所有 `hello`**（宁可连不上，不可无认证放行）                                                                                                                     |
| `allowedClients`        | array  | `[]`        | 空 = 不启用白名单。每项为单机 `192.168.0.50` 或网段 `192.168.0.0/24`                                                                                                    |
| `idleTimeoutSec`        | int    | `30`        | 未 `hello` 或空闲超时后关闭连接                                                                                                                                    |
| `capture.outputDir`     | string | `"capture"` | 相对**工程根**的目录（沿用 `ConfigPaths` 的路径解析约定）                                                                                                                  |
| `capture.retentionDays` | int    | `30`        | 保留策略，见 §7.4                                                                                                                                             |

### 9.2 `config/process.json` 增量

`SchemeData` 增 `schemeId`（string）；`ActionData` 增 §3.5 的 **8 个** `capture*` 字段。均为**新增键，不影响旧文件读取**（缺省即默认值），升级惰性（`schemeId` 例外，见 §8.3）。

### 9.3 生效时机（沿用既有约束）

`ConfigManager::getValue` 读的是内存里的 `root_`，故：

- **经 UI 页改 → 立即生效**（本方案实现取「每次 accept 现读配置」，不缓存，连变更信号都不需要）
- **直接编辑 `config.json` 文件 → 需要重启**（内存 `root_` 不会自动重载）

`AGENTS.md` 已把这条记为通用陷阱，第三方接口同样适用，须写入文档。

### 9.4 UI 入口

`ConfigPage` 新增 `QGroupBox("第三方接口")`，与该页现有的「硬件映射 / 安全与运动限制 / 传动与换算参数」三个分组并列（`ConfigPage.cpp:894` / `:912` / `:968`）。字段：启用开关、监听端口、绑定地址、token（密码框 + 显示按钮）、白名单（多行文本，一行一项）、留图目录、保留会话数。

### 9.5 `bindAddress` 语义详解（§1.4 裁定 4 的澄清项）

**一句话：它是本机（机器人 PC）在监听时「挂到哪块网卡上」的地址，即服务端自己的地址，不是第三方地址。**

`QTcpServer::listen()` 的第一个参数就是它（离线文档 `qtcpserver.html:180` 已核）：

```cpp
bool QTcpServer::listen(const QHostAddress &address = QHostAddress::Any, quint16 port = 0)
```

**三个地址概念必须分清**（现场最容易混淆的一处）：

| 配置键              | 是**谁**的地址      | 作用                        | 例子             |
| :--------------- | :------------- | :------------------------ | :------------- |
| `bindAddress`    | **本机（机器人 PC）** | 服务端监听**绑到哪块网卡**           | `192.168.1.20` |
| `listenPort`     | 本机             | 监听端口                      | `9527`         |
| `allowedClients` | **第三方**        | 允许**哪些来源**连进来（accept 层过滤） | `192.168.1.50` |

**取值语义**（`qhostaddress.html` 已核）：

| 取值             | 等价常量                      | 效果                                                                                          |
| :------------- | :------------------------ | :------------------------------------------------------------------------------------------ |
| `""`（空，本方案默认）  | `QHostAddress::Any`       | 监听**全部网卡**。第三方从任一网段都能连（仍受 `allowedClients` 约束）                                              |
| `0.0.0.0`      | `QHostAddress::AnyIPv4`   | 只监听 IPv4 的全部网卡（文档原文：*a socket bound with this address will listen only on IPv4 interfaces*） |
| `::`           | `QHostAddress::AnyIPv6`   | 只监听 IPv6 全部网卡                                                                               |
| `127.0.0.1`    | `QHostAddress::LocalHost` | ⚠ **只允许本机连接**。跨机场景填这个 = 第三方**永远连不上**，且症状是「连接被拒绝」而不是「认证失败」，现场极易误判成防火墙问题                      |
| `192.168.1.20` | —                         | 只在该网卡上监听，其它网卡不可达                                                                            |

**本机网卡现状**：`config.json` 目前只有一条网络信息——`communication.motionCard.pcIp = 192.168.0.100`（与运动卡 `192.168.0.1` 同网段）。即**运动卡占用了一条独立链路**。若机器人 PC 另有一块接厂区局域网的网卡（第三方通常在那张网上），则：

- 绑 `""`（Any）：两张网都监听，第三方走局域网能连。代价是运动卡网段上能访问该端口的主机也进得来（仍受 `allowedClients` + token 约束）。
- 绑具体网卡 IP：只有那张网监听，**运动卡网段完全不可达**，攻击面最窄。

**建议：留空（Any）起步，但 `allowedClients` 必须配置、不得留空。** 理由：现场网卡 IP 常变、多网卡时容易配错，而配错的症状是「连不上」且排查成本高；`allowedClients` 已能在 accept 层把来源收窄到具体 IP / 网段，安全增益相同而不易配错。待现场网络拓扑稳定后，再收紧为具体网卡。

> ⚠ **不要**把 `bindAddress` 填成 `192.168.0.100`（运动卡网段的 PC 地址）。那会让第三方流量与运动卡流量挤在同一条链路上——运动卡的实时性优先，不应被图片流打扰。第三方应走另一张网卡。
>
> ⚠ `QHostAddress::Any` 监听时，IPv4 客户端可能被报成 `::ffff:a.b.c.d` → 这正是 §4.5 必须做 IPv4-mapped 归一化的原因（绑具体 IPv4 网卡可顺带规避）。

> **待确认**：`bindAddress` 默认留空（Any）是否接受？接受则按 §4.5 已实现的归一化方案走。

---

## 十、改动清单

| 文件                                     | 改动                                                                                                                                                                                                                                                            |
| :------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `src/Config/ProcessManager.h`          | `ActionType` 追加 `Capture`；`ActionData` 增 **8 个** `capture*` 字段；`SchemeData` 增 `schemeId`                                                                                                                                                                      |
| `src/Config/ProcessManager.cpp`        | `actionTypeName` / `resourceKeysOf` 各加一个 case；`schemeId` 生成（8 位 hex + 查重）与**即时落盘**；序列化往返                                                                                                                                                                      |
| `src/Logic/SequenceWorker.h/.cpp`      | `SegmentCountOf` / `ExecuteNode` / `PrepareRunnerSegment` / `DispatchRunnerSegment` / `ReissueRunnerSegment` 各加 `Capture` 分支；拍照执行体（含 `captureRequired` 分支）；`captureReady` / `captureData` / `captureFailed` 信号；`:762` 注释补「`Capture` 不在重试白名单」（**逻辑无需改动**，§3.4） |
| `src/UI/ProcessPage.cpp`               | 两处类型下拉加 `"拍照"`；新增 Page 8 拍照详情页；`kGroupPageIndex` 7 → 8；方案下拉旁 `schemeId` 只读显示                                                                                                                                                                                  |
| `src/UI/ConfigPage.h/.cpp`             | 新增 `QGroupBox("第三方接口")` 及其控件与接线                                                                                                                                                                                                                               |
| `src/UI/MainWindow.cpp`                | 装配 `InterfaceServer`（注入 `SequenceWorker*`）；生命周期管理                                                                                                                                                                                                             |
| `src/Interface/*`（新增）                  | `InterfaceServer` / `InterfaceProtocol` / `CaptureStore`                                                                                                                                                                                                      |
| `CMakeLists.txt`                       | `find_package` 补 `Network`；`target_link_libraries` 补 `Qt6::Network`；`add_subdirectory(src/Interface)`                                                                                                                                                         |
| `config/config.json`                   | 新增 `interface` 节（§9.1）                                                                                                                                                                                                                                        |
| `doc/config.md`                        | 新增 §interface                                                                                                                                                                                                                                                 |
| `doc/process.md`                       | `ActionType` 表加 `6=拍照`；`ActionData` 字段表加 **8 项**；`SchemeData` 表加 `schemeId`                                                                                                                                                                                   |
| `doc/architecture.md`                  | 新增 `src/Interface/` 子目录与依赖方向                                                                                                                                                                                                                                  |
| `AGENTS.md`                            | 速查与架构节登记新子目录、新配置节、新动作类型                                                                                                                                                                                                                                       |
| `doc/README.md`                        | 登记本文                                                                                                                                                                                                                                                          |
| `doc/test/real_machine_plan_phase3.md` | 追加阶段 14 用例（§十二）                                                                                                                                                                                                                                               |
| `TEST_RECORD.md`                       | 实施时逐项追加（见 §十一 / §十二 编号规划）                                                                                                                                                                                                                                     |

---

## 十一、前置阻塞项：`CaptureFrame()` 并发互斥

> **✅ 已于 2026-10-12 落地（TR-123）——但不是按下面 ① 的加锁路线，而是按 ② 的取帧收敛，且触发理由与本文无关**：该并发 UB 在泡芙现有「识别」动作路径上**独立存在**（`AutoRunPage.cpp:651` 启动方案即 `AcquireCamera("AutoRun")` 让流常驻，`SequenceWorker` 在主线程取帧），**不需要第三方对接即成立**；用户 2026-10-12 裁定按**泡芙现存风险**修复。下面 ② 那条「会改变识别动作取帧语义、须单独评估」的顾虑**已评估并接受**（取到的是最近一帧，延迟 ≤ `1/fps`）。**本文整体仍是非交付的扩展储备**（见 `doc/README.md` 速览注记），§十一 保留仅供追溯。

**这是拍照动作落地前的硬前置项**，建议**单独走一条 TR**（编号 **TR-123**），不与拍照动作混在一起。

**问题**（§二.4）：采集线程与 worker 线程可同时进入同一 `OrbbecCamera` pipeline，且其中一个可能正在 `DestroyPipeline()` 重建管线。

**两步方案**：

| 步               | 内容                                                                                                                                             | 优先级                 |
| :-------------- | :--------------------------------------------------------------------------------------------------------------------------------------------- | :------------------ |
| **① 加管线级互斥**    | `OrbbecCamera` 内新增 `std::mutex pipelineMutex_`，保护 `CaptureFrame()` / `HandleGrabFailure()` / `StartStream()` / `StopStream()` 的整段临界区。改动小、消除 UB | **立即**（拍照动作落地前必须完成） |
| **② 取帧收敛到单一路径** | `SequenceWorker` 不再直接 `camera->CaptureFrame()`，改为向采集线程取「最新帧快照」（共享只读 `CameraFramePtr`）。彻底消除双线程进管线，且复用采集线程已产出的帧、零额外取帧开销                          | 后续可选优化              |

> ⚠ **② 的代价须知情**：改用「最新帧快照」后，取到的帧最多滞后 `1/fps`（15 fps 下约 66 ms）。对拍照动作可接受（机械臂静止或对滞后不敏感），但**会改变现有「识别」动作的取帧语义**（当前是同步取一帧新鲜图），故 ② 须单独评估，不与 ① 同批做。

**回归判据**：`test_camera_check --stream 700` 长稳（参照 TR-110 / TR-111 / 用例 11.32 的做法：全程空帧 0、无内存持续增长、退出码 0）；并发场景另建离线复现程序，两线程高频对撞 `CaptureFrame()`。

---

## 十二、测试用例规划（阶段 14）

追加到 `doc/test/real_machine_plan_phase3.md` 作**第九批 = 阶段 14「第三方对接与拍照动作验收」**，编号 14.1 起（**追加末尾、不重排**，与既有批次规则一致）。共 **47 条**（14.1 ~ 14.47；14.35 起为第二批 8 项裁定的补充用例）。TR 编号：实现时从 **TR-123** 起（TR-123 为 §十一 的并发互斥修复）。

### A 档 离线（不需硬件、不需 Sim）

| 编号    | 判据                                                                                                         |
| :---- | :--------------------------------------------------------------------------------------------------------- |
| 14.1  | 帧编解码往返：构造 4 字节长度 + 类型 + 载荷，编码后解码逐字节一致；长度字段为**大端**                                                          |
| 14.2  | 长度前缀越界防护：声明长度 > 实际载荷时**不读越界**、按错误处理；声明长度超上限时拒绝                                                             |
| 14.3  | 白名单匹配：单机 `192.168.0.50` 命中/不命中；网段 `192.168.0.0/24` 边界（`.0` / `.255` / `.256` 非法）                           |
| 14.4  | 白名单 IPv4-mapped 归一化：`::ffff:192.168.0.50` **命中** `192.168.0.50`                                            |
| 14.5  | 白名单非法串：`192.168.0.0/99`、`abc` 等**不匹配任何地址**（前缀 -1 判据生效），且不崩溃                                                |
| 14.6  | 白名单为空 ⇒ **放行全部**（不启用语义）                                                                                    |
| 14.7  | `ActionType` 序列化往返：`Capture = 6` 写入后读回为 6；既有 0~5 不受影响（**整数持久化不错位**）                                        |
| 14.8  | 详情页索引：`Capture` 节点的 stack 索引为 7、分组页为 8 —— 二者**不相等**（防 §3.3 撞号回归）                                           |
| 14.9  | `capture*` 字段序列化往返：**8 个**字段写入 → 读回逐字段一致；旧文件缺这些键时取默认值不报错                                                   |
| 14.10 | `schemeId` 迁移**幂等**：无 `schemeId` 的旧 `process.json` 加载后 id 非空；**再次加载 id 不变**（§8.3 的核心判据）                    |
| 14.11 | `schemeId` **即时落盘**：加载后**不做任何编辑**，`process.json` 已含 `schemeId`（与 `id` / `nextActionId` 的惰性升级**行为不同**，须专门验） |
| 14.12 | `resourceKeysOf(Capture)` == `resourceKeysOf(Vision)` == `{"Camera"}`；分组校验拒绝「拍照 + 识别」同组                    |

### B 档 Sim（`SimCamera` / `SimCard`，不需真机）

| 编号    | 判据                                                                                             |
| :---- | :--------------------------------------------------------------------------------------------- |
| 14.13 | Sim 下跑含「拍照」动作的方案：动作正常完成，日志出现拍照相关行                                                              |
| 14.14 | `captureSaveLocal=true` ⇒ 目录按 `<sessionId>/<cycleIndex>/` 生成，彩色图与 sidecar JSON **成对存在**        |
| 14.15 | sidecar JSON 字段齐全（§7.2 全字段），`pose` / `joints` 有值                                               |
| 14.16 | `captureDeliverPath=true` ⇒ 收到 `captureReady`，且通知里 `filePath` 指向的文件**已存在且非空**（通知前已就绪）          |
| 14.17 | `captureDeliverStream=true` ⇒ 先收 JSON 描述帧、紧接二进制帧；`bytes` 与实际二进制长度一致                            |
| 14.18 | 两个开关**同选** ⇒ 两条通道都触发；**都不选** ⇒ 仅本地留存、无上行事件                                                     |
| 14.19 | `captureContent="color_depth"` ⇒ 深度图按 `depthWidth × depthHeight` 落盘，尺寸与彩色**不同**时也不崩（TR-116 契约） |
| 14.20 | token 错误 ⇒ `hello` 被拒、连接断开；**未 `hello` 直接发 `run`** ⇒ 被拒                                        |
| 14.21 | 单连接独占：第二个连接在 accept 层被拒（白名单层），第一个连接**不受影响**                                                    |
| 14.22 | `run` 用不存在的 `schemeId` ⇒ 拒绝且回明确错误，**不产生任何运动**                                                  |
| 14.23 | 未使能 / 未回零时 `run` ⇒ 回 `errorOccurred`，文案分别为「轴未使能，请先手动使能」/「系统未回零，请先一键回零」（§二.3 门禁复用）              |
| 14.24 | `stop` ⇒ 走 `StopImmediate`（减速停、保持使能），`stateChanged` 回 `Idle`；**不出现**断使能                        |
| 14.25 | 拍照动作**不在分组**内 ⇒ 与移动动作串行；**在分组**内 ⇒ 并发（日志时序可判）                                                  |
| 14.26 | 磁盘保留：造 N 个会话目录，按「最近 N 个」而非「N 天前」轮转（TR-122 同款判据）                                                |

### C 档 真机（`OrbbecCamera` + 真机械臂）

| 编号    | 判据                                                                                         |
| :---- | :----------------------------------------------------------------------------------------- |
| 14.27 | 真机拍照：PNG 落盘、尺寸与 `colorWidth × colorHeight` 一致、可正常打开                                        |
| 14.28 | **PNG 编码耗时实测**：单张落盘耗时记录（§6.3 决策依据），回填本文 §6.1 的体积估算                                         |
| 14.29 | 真机深度：`captureContent="color_depth"` 落 16 位深度 PNG（`Format_Grayscale16`），D2C 关时尺寸为原生 848×480 |
| 14.30 | `CaptureFrame()` 并发长稳：`--stream 700` 判据（空帧 0 / 无内存增长 / 退出码 0）+ 拍照动作高频穿插                    |
| 14.31 | 第三方实际联调：另一台机器上的 client 完成 `hello` → `run` → 收图 → `stop` 全链路                                |
| 14.32 | 断线重连：运行中断开网线，恢复后事件补发；**产线节拍不受影响**（交付与执行解耦的实证）                                              |
| 14.33 | IP 白名单真机：非白名单机器被拒且有 WARN 日志；白名单机器正常                                                        |
| 14.34 | `schemeId` 端到端：改名方案后第三方仍能用原 `schemeId` 启动（§8.1 的核心价值）                                      |

### 补充用例（第二批 8 项裁定，2026-10-09 追加）


> 按「**编号追加末尾、不重排**」的既有批次规则，本批统一接在 14.34 之后（编号 14.35 ~ 14.47），档位标注在表内。

| 编号 | 档 | 判据 |
| :--- | :--- | :--- |
| 14.35 | A | **`Capture` 不在重试白名单**（§1.4 裁定 2）：`retryCount = 2` 时，拍照动作失败**只执行一次、不重试**；同批 Vision / Move 失败则**重试**（对照组，防「白名单被误改宽」） |
| 14.36 | A | `captureRequired` 序列化往返；旧文件缺失时默认取 **`true`**（fail-closed 默认值不被静默改掉） |
| 14.37 | A | `schemeId` 形态：生成 1000 次，**全部**匹配 `^[0-9a-f]{8}$` 且**两两不重复**（查重循环生效，§8.2） |
| 14.38 | A | `schemeId` 匹配**大小写不敏感**：第三方传 `3F9A2C71` 能命中 `3f9a2c71` 的方案 |
| 14.39 | A | `captureContent` 前向兼容：加载含 `"color_depth_cloud"` 的文件 → 回落为 `"color"` 且**不报错**（点云本期不做，§1.4 裁定 7） |
| 14.40 | B | **`captureRequired = true`**（默认）：造取帧失败 → 方案**中止**，`errorOccurred` 文案为**拍照故障语义**（「取帧失败 / 帧无效」），**不等于**识别动作的「视觉未检出目标」（§5.6 文案区分） |
| 14.41 | B | **`captureRequired = false`**：造取帧失败 → 收到 `captureFailed`（含 `nodeId` / `cycleIndex` / `tag` / `reason`），且方案**继续跑完**（不中止） |
| 14.42 | B | `captureRequired = false` 失败时**不发 `errorOccurred`**——守住「`errorOccurred` ⟺ 会话中止」不变量（§5.6 约束 2） |
| 14.43 | B | `captureRequired = false` 的**兜底**：连续失败达阈值（3 次）或相机流掉线 ⇒ **仍中止**（§5.6 约束 3） |
| 14.44 | B | **分组重试连带重跑拍照**（§3.4 残留边界）：`retryCount = 1` + 组内含拍照、组内移动动作失败 ⇒ 拍照**执行两次**。⚠ 本用例是**已知行为的记录**，不是缺陷判据；须同时验「两次文件名毫秒不同、可区分」 |
| 14.45 | B | `query`（§1.4 裁定 8）：运行中 / 空闲时各回一次状态；**前后 `stateChanged` 无变化**（确认只读、不改变任何状态） |
| 14.46 | C | `captureFormat = "jpeg"` 推送：二进制帧以 `FF D8 FF` 开头（JPEG magic）；**本地留存仍为 PNG**；质量固定（两次编码同一输入，输出字节一致 → 证明参数未被外部影响） |
| 14.47 | C | `bindAddress` 取值（§9.5）：绑具体网卡 IP 时，另一网段不可达；留空（Any）时两侧均可连但 `allowedClients` 仍生效 |

---

## 十三、裁定状态

### 13.1 已裁定 21 项

第一批 13 项见 §1.2，第二批 8 项见 §1.4。**除 §13.2 一项澄清外，无遗留未决项。**

| 批次 | 项数 | 关键结论 |
| :--- | :--- | :--- |
| 第一批 | 13 | 跨机 TCP、双通道可单选可同选、静态/运动由编排决定、稳定 `schemeId`、只允许启动/停止、token + 白名单 |
| 第二批 | 8 | `captureRequired` 默认 `true`、`Capture` 不参与重试、`captureFormat` 默认 `png` 且 JPEG 质量不可配、`schemeId` 用 8 位 hex、不做 ACK 握手、点云本期不做、开放 `query` |

### 13.2 唯一待确认项

| # | 项 | 待确认内容 | 建议 |
| :--- | :--- | :--- | :--- |
| 1 | `bindAddress` 默认值（语义详见 §9.5） | 默认留空（`QHostAddress::Any`，监听全部网卡）是否接受？ | **接受留空**，但 `allowedClients` 必须配置、不得留空（§9.5 已给理由） |

### 13.3 建议实施顺序

| 步 | 内容 | 前置 |
| :--- | :--- | :--- |
| 1 | **TR-123**：`CaptureFrame()` 管线级互斥（§十一 ①） | 无。**必须先做**——否则拍照动作会在真机验收时以随机 `0xC0000005` 的形式暴露 |
| 2 | 动作层：`ActionType::Capture` + 8 个字段 + 界面两处下拉 + `kGroupPageIndex` 7→8（§三） | 第 1 步 |
| 3 | `schemeId`（含即时落盘）+ `config.json` 的 `interface` 节 + `ConfigPage` 分组（§八 / §九） | 可与第 2 步并行 |
| 4 | 接口层：帧协议 + 白名单 + 4 条命令 + 事件上行（§四） | 第 3 步 |
| 5 | 拍照执行体：取帧校验 + 同步落盘 + 双通道交付 + `captureRequired` 分支（§五 / §七） | 第 2、4 步 |
| 6 | 阶段 14 用例执行（14.1 ~ 14.47）+ `TEST_RECORD.md` 逐项追加 | 第 5 步 |

---

## 十四、风险与回退

| 风险 | 影响 | 回退 / 缓解 |
| :--- | :--- | :--- |
| `CaptureFrame()` 并发 UB | 随机崩溃（`0xC0000005`），且难复现 | **前置修复**（§十一），未修不得进拍照动作开发 |
| PNG 编码拖慢节拍 | 每张 +30 ~ 80 ms | 实测后评估 §6.3 选项 (b)；或降低拍照频率 |
| 磁盘被循环生产撑爆 | 现场停机 | `retentionDays` + 容量上限双保险；写入前检查剩余空间 |
| 接口层成为新的攻击面 | 远程启动机械臂 = 可远程造成伤害 | 命令集极小（4 条）+ 单连接独占 + token + 白名单；`enabled` 默认关；不开放急停 / 单步 / 点动 |
| 第三方网络抖动拖死产线 | 产线停摆 | 交付与执行**解耦**（§5.1 + §1.4 裁定 6）；事件缓存 + 重连补发 |
| `schemeId` 每次重启变化 | 第三方引用失效，且故障表现滞后 | **即时落盘**（§8.3），并有用例 14.10 / 14.11 守住 |
| 详情页索引撞号 | 界面错显（静默，不报错） | `kGroupPageIndex` 改 8 + 用例 14.8 |
| **`captureRequired = false` 掩盖相机故障** | 相机掉线后整晚空转，第三方收到一长串 `captureFailed` 却不知硬件已没了 | 连续失败阈值（3 次）+ 流掉线判定仍中止（§5.6 约束 3）+ 用例 14.43 |
| **分组重试连带重跑拍照** | 同节点同轮产生第二份图 + 第二条通知 | 已知行为（§3.4 残留边界）；第三方按 `timestamp` 区分；需严格一图一时把拍照放分组外；用例 14.44 |
| **`schemeId` 32 bit 碰撞** | 两方案同 id → 第三方启动错方案 | 生成时**查重循环**（§8.2）+ 用例 14.37；本项目方案数量级下概率可忽略，但查重把「概率」变成「确定」 |

**整体回退**：`interface.enabled = false` 即可完全关闭接口层（不监听端口、不建持有登记），回到现状。`ActionType::Capture` 是追加值，既有方案零影响；即使回退接口层，拍照动作仍可单独用于本地留存。
