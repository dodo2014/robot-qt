---
name: creampuff-build
description: 在 CreamPuffRobot 仓库（Qt6 + MSVC + Ninja + vcpkg）里安全地编译 Debug/Release、判定增量构建结果是否可信、跑 Sim 冒烟。当需要编译本仓库、改完代码要验证、或遇到「改了代码却没生效 / 启动即崩 / 冒烟日志没到 Initialize complete」时使用。
agent_created: true
---

# CreamPuffRobot 安全编译与冒烟

## 为什么需要这个 skill

本仓库 ninja 的**头文件依赖追踪失效**：`build.ninja` 用 `deps = gcc`，而中文版 `cl.exe`
输出「**注意: 包含文件:**」而非 `Note: including file:`，依赖没进 `.ninja_deps`。

后果：**改头文件后，只有被改的那个 `.cpp` 会重编，包含该头的其他 TU 不重编。**
若头文件改动涉及**类成员 / 类大小 / 虚表**，就产生「编译期完全静默、运行期堆越界」的
ABI 不一致。

> 2026-09-22 真实事故：给 `ManualControlPage.h` 加两个 `QVector<QString>` 成员后只做增量
> 构建 → `MainWindow.cpp.obj` 未重编（它 `#include "ManualControlPage.h"` 且
> `new ManualControlPage()`）→ 按**旧类大小**分配、按**新布局**写成员 → 启动即崩。
> 排查绕了很久，因为崩溃现场毫无线索。

## 铁律

**只改 `.cpp` → 增量构建可靠（源文件按 mtime 直接跟踪）。**
**改任何 `.h` / `.hpp` / `CMakeLists.txt` → 一律 `-t clean` 全清重建，不做增量。**

## 第 0 步（必做）：构建新鲜度预检

```bash
cd /d/workspace/projects/CreamPuffRobot
python .workbuddy-ai/skills/creampuff-build/scripts/check_build_freshness.py out/build/x64-Debug src
```

判据 = **对每个头文件 H，所有（含传递）包含 H 的 `.cpp`，其 `.obj` 都必须晚于 H**。
退出码 `0` = 可信；`1` = 不可信（须全清重建），脚本会点名「哪个头 / 哪个包含者 / 旧了多少秒」。

⚠ 不要用「最旧 obj vs 最新头文件」的 mtime 全序比较——它既会被 CMake 探测产物
（`CompilerId*.obj`、`ShowIncludes/main.obj`）与上一轮配置遗留的孤儿 obj 绊出**假阳性**，
又**抓不到**「改了头文件但只有一个 TU 重编」这个真正的危险情形。

## 第 1 步：编译（Bash 跑 Git Bash，环境变量手工内联）

PowerShell 工具在本沙箱**不回 stdout**，`build_release.bat` / `build_debug.bat` 会**静默无输出退出**
（vcvars64 依赖 `reg.exe` 被拦）。可用路径只有这一条：

```bash
cd /d/workspace/projects/CreamPuffRobot
export MSYS2_ARG_CONV_EXCL="*"
export INCLUDE="D:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/include;D:/Windows Kits/10/Include/10.0.26100.0/ucrt;D:/Windows Kits/10/Include/10.0.26100.0/shared;D:/Windows Kits/10/Include/10.0.26100.0/um"
export LIB="D:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/lib/x64;D:/Windows Kits/10/Lib/10.0.26100.0/ucrt/x64;D:/Windows Kits/10/Lib/10.0.26100.0/um/x64"
export PATH="/d/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:/d/Windows Kits/10/Bin/10.0.26100.0/x64:/d/Qt/Tools/Ninja:/d/Qt/Tools/CMake_64/bin:/d/Qt/6.11.1/msvc2022_64/bin:$PATH"
"/d/Qt/Tools/Ninja/ninja.exe" -C "D:/workspace/projects/CreamPuffRobot/out/build/x64-Debug" CreamPuffRobot 2>&1 | grep -aEi "error|错误|警告 C|warning C[0-9]|FAILED|Linking CXX exe|no work" | tail -20
```

- `INCLUDE` / `LIB` 是 Windows 变量 → `;` + Windows 路径；**`PATH` 是 Git Bash 自己的变量 → `:` + `/d/...`**。
- 全清重建：在 ninja 前插 `"...ninja.exe" -C "<build>" -t clean >/dev/null 2>&1 &&`。
- Debug 目录 `out/build/x64-Debug`，Release `out/build/x64-Release`；**两边都要各来一次**。
- 全量约 1m20s（47 个 TU）。长命令用后台运行，别设小 timeout。

## 第 2 步：只看关键行

成功标志 `[N/N] Linking CXX executable CreamPuffRobot.exe` 后接 `[POST_BUILD] ...`。

⚠ **MSVC 是中文版**：关键词是「**错误**」「**警告 C**」「**注意: 包含文件:**」，grep 英文
`error` / `warning C` 会漏掉一部分（但 `warning C####` 这种带编号的仍是英文，故两者都带上）。

⚠ **别用裸 `error` 做过滤**（2026-09-23 实测）：MSVC 的「注意: 包含文件:」行里**含 `error` 的头文件名**会把输出淹掉——`system_error`、`__msvc_system_error_abi.hpp`、`winerror.h` 每行都命中 `-i error`，Debug 全量构建时能刷出上百行噪音，把真正要找的 `[N/N] Linking` 挤出 `tail`。
**改用带上下文的模式**：`grep -aE "error C[0-9]{4}|error LNK[0-9]{4}|error MSB[0-9]{4}|错误|警告 C|warning C[0-9]{4}|FAILED|Linking CXX exe|no work"`。

⚠ grep 必须加 `-a`，否则见到 windeployqt 的 GBK 字节会判定为二进制而停止输出。

**既有警告基线（不算新增）**：`SimCamera.cpp:126/127 C4244`、`SimCard.cpp:156 C4100`、
`BoPaiCard.cpp:237 C4244`、SDK `Filter.hpp:598 C4996`、`main.cpp:48 C4273`。

## LNK1168（exe 被占用）

```powershell
Get-Process -Name CreamPuffRobot -ErrorAction SilentlyContinue | Stop-Process -Force
```

Bash 下 `taskkill //IM` 会被 MSYS2 吃掉参数；`tasklist //FI` 过滤也失效（用 `tasklist | grep -ai creampuff` 才看得到）。

### 先判「被占用的是哪个 build」再决定编谁（2026-09-23 补）

`tasklist | grep -ai creampuff` 只能告诉你**有**进程，**分不出 Debug 还是 Release**。
用「独占写打开」探测哪个 exe 被锁——**被锁的那个 build 编不了，另一个可以照编**：

```bash
cd /d/workspace/projects/CreamPuffRobot
python -c "
for p in [r'out/build/x64-Debug/CreamPuffRobot.exe', r'out/build/x64-Release/CreamPuffRobot.exe']:
    try:
        f = open(p, 'r+b'); f.close(); print(p, '-> NOT locked')
    except Exception as e:
        print(p, '-> LOCKED', type(e).__name__)"
```

Windows 下运行中的 exe 会拒绝 `r+b` 打开（`PermissionError`）。**不要用 `mv` 改名试探**
（万一成功就把文件挪走了）。实测：用户开着的实例锁 Debug、Release 空闲 → 直接编 Release
完成编译验证，Debug 留待用户关闭后补链。

⚠ 不要为了编 Debug 就杀用户进程（他可能正在真机调试）。先编能编的那个，把另一个如实记为待补。

⚠ `-t clean` 会连 exe 一起删。若之后编译**失败**，该 build 目录会**没有 exe**——别误判成
「exe 不见了 = 环境坏了」，看 `[N/N] Linking` 那一行有没有出现即可。

## 第 3 步：Sim 冒烟

**唯一判据 = 当日 `log/creampuff_<date>.log` 新增 `Initialize complete`**，不要靠脚本提示行。

```bash
cd /d/workspace/projects/CreamPuffRobot ; cp config/config.json /tmp/cfg_orig.json ; LOG="log/creampuff_$(date +%Y-%m-%d).log" ; BEFORE=$(grep -ac 'Initialize complete' "$LOG") ; sed -i 's/"motionCardType": "Bopai"/"motionCardType": "SimCard"/; s/"servoType": "XRServo"/"servoType": "SimServo"/; s/"cameraType": "OrbbecCamera"/"cameraType": "SimCamera"/' config/config.json ; ./out/build/x64-Debug/CreamPuffRobot.exe > /tmp/smoke_out.txt 2>/tmp/smoke_err.txt & APP_PID=$! ; sleep 22 ; AFTER=$(grep -ac 'Initialize complete' "log/creampuff_$(date +%Y-%m-%d).log") ; cp /tmp/cfg_orig.json config/config.json ; echo "SMOKE BEFORE=$BEFORE AFTER=$AFTER" ; kill -9 $APP_PID 2>/dev/null ; echo "SMOKE_DONE"
```

⚠ **备份放 `/tmp`（仓库外），还原语句放在 `kill` 之前**（2026-09-23 改）。原脚本把备份写成 `config/config.json.sim_bak` 并把还原放在 `kill` 之后：一旦 shell 被 SIGTERM（见下条），还原就整条丢掉，config 会**永久留在 Sim**。新写法有两个好处——备份在仓库外不污染 `git status`；而 config 在**启动时就读进内存**（`ConfigManager::load()`），所以运行中还原**不影响**冒烟实例，却能在 shell 猝死时仍然保住还原。

⚠ **改仓库根的 `config/config.json` 是对的，别被 POST_BUILD 的拷贝误导**：CMake 会把 `config.json` / `process.json` 拷进 `out/build/<cfg>/`，但 `ConfigPaths::resolve()`（`src/Config/ConfigPaths.cpp:40`）的**首选候选是编译期烧进去的 `<PROJECT_SOURCE_DIR>/config/<fileName>`**，exe 目录那份只是便携部署的兜底（候选 2/3）。同理日志目录规则 1「exe 在源码树内 → 工程根 `log/`」（`:126`）。

⚠ **`A && B && C & D` 会把 `&` 之前整条链放进子 shell，链内变量赋值全丢**（`BEFORE` 读成空）。
必须用 `;` 分隔，且 `&` 只跟在 exe 命令后。
⚠ 末尾 `kill -9 $!` 有时会连带把自身 shell 打成 SIGTERM → 后续 `echo` 不落 stdout。
**因此单独复核三件事**：① `grep -ac 'Initialize complete'` 的计数；② `config/config.json` 是否已还原（判据 = `git diff --stat -- config/config.json` 与冒烟**前**的数值一致，而不是「必须出现 `Bopai`」——用户可能本来就切在 Sim）；③ 进程是否真退出（`tasklist | grep -ai creampuff`）。
⚠ **`Initialize complete` 只在日志里判，不要看 `SMOKE` 那行**：`echo` 可能不落 stdout（见上）。
⚠ **Bash 里 grep「中文模式」会静默失效**（2026-09-23 实测）：`grep -a -c '视觉' log/creampuff_<date>.log` 返回 **0**，而同期 ASCII 的 `Vision` 有 2501 条命中——**`0` 与「真的没有」不可区分**。本次差点把「日志里没有『视觉』」当成「视觉动作没跑过」的证据。**判据：查中文一律走 ripgrep（Grep 工具，UTF-8 正常）**；非要在 Bash 里查，先用一个**已知必然存在**的中文串（如 `导航切换到页面`）做对照，验证 grep 本身有效再下结论。
⚠ **先看进程是否真没了**：`tasklist | grep -ai creampuff`（Bash）。**不要用 PowerShell `Get-Process`**——本沙箱 PowerShell 工具**不回 stdout**，「无匹配」与「没在跑」输出一模一样，会误判成「软件已关闭」（2026-09-23 踩过）。
⚠ 冒烟会真实删除 7 天前的 `log/creampuff_*.log`（S8 启动清理），属预期。
⚠ **冒烟必须把三处都置 Sim**：`motionCardType` / `servoType` / **`cameraType`**。前两处原脚本已覆盖；`cameraType` 是 2026-09-23 补上的——当时用户把 config 切到了 `OrbbecCamera` 且软件正在运行，冒烟实例会去抢**真机相机**。
⚠ **Debug exe 被占用时（LNK1168 / 用户正开着软件）可改用 Release exe 冒烟**：`./out/build/x64-Release/CreamPuffRobot.exe`，判据不变。但真机相机/运动卡被占用时，仍须等用户关闭软件——否则两个实例争硬件。

## 冒烟失败时的排查顺序

1. **先验构建完整性**（第 0 步脚本）。`Initialize complete` 发生在 `MainWindow` 页面构造
   **之后**，所以「没到 `Initialize complete`」**不能**推断「页面代码有问题」。
2. **崩溃日志尾部不可信**：`spdlog::flush_every(1s)` 会丢掉崩溃前 ≤1 s 的行。日志停在哪一行
   不代表崩在哪一行。
3. 拿 obj 与头文件 mtime 直接对：`ls -la --time-style=+%H:%M:%S <源> <obj>`。**任何 obj 早于
   它包含的头文件 = 构建不可信**，先 `-t clean` 重建再说，别去读代码。

## 测试目标（`tests/`）必须**显式点名**构建

`tests/CMakeLists.txt` 第一行就写明：「**不参与主程序依赖链，`ninja CreamPuffRobot` 不会编译它们**」。
所以：

- 日常的 `ninja -C <build> CreamPuffRobot`（包括本 skill 第 1 步）**永远不会**产出 `test_camera_check.exe` 等；
- 而 `-t clean` **会**把已构建的 test exe 一并删掉 → **每次全清重建后它们就消失**，且不会被后续任何主程序构建恢复。

**表现（2026-09-23 实际发生）**：阶段 11C 用例 11.29 报 `out\build\x64-Release\test_camera_check.exe` **不存在**，
11.29~11.32 整批被卡住。根因就是当天做过多次 `-t clean`（TR-108/111/112/113），而这些目标从没被显式点名重建。

**修法**（按配置各来一次，命令与第 1 步同款，只换目标名）：

```bash
ninja -C out/build/x64-Release test_camera_check test_kinematics_check test_node_tree_check
ninja -C out/build/x64-Debug   test_camera_check test_kinematics_check test_node_tree_check
```

⚠ **产物落点不一致，别按统一路径找**（由各自 `RUNTIME_OUTPUT_DIRECTORY` 决定）：

| 目标 | 落点 | 原因 |
|---|---|---|
| `test_camera_check.exe` | **构建根**（与 `CreamPuffRobot.exe` 同级） | 要用根目录的 Qt DLL + POST_BUILD 拷来的 `OrbbecSDK.dll` / `extensions/` |
| `test_node_tree_check.exe` | **构建根** | 链接 `Config`、用到 `QString`，同样依赖根目录 Qt DLL |
| `test_kinematics_check.exe` | **`tests/` 子目录** | 只链 `Core` + `spdlog`，不需要 Qt DLL |

⚠ **`tests/` 子目录里可能有「旧版孤儿 exe」**：`RUNTIME_OUTPUT_DIRECTORY` 是 2026-09-20 才补上的，
在那之前的产物留在 `tests/`。**判断「exe 在不在」只看构建根**，否则会被孤儿文件骗到（本次实测：
`out/build/x64-Debug/tests/test_camera_check.exe` 存在，但它是 **2026-09-20 13:18** 的旧物，构建根里当时并没有）。
同类的还有 `out/build/x64-Debug/test_sequenceworker.exe`（2026-08-20，该目标已不在 `CMakeLists` 里）。

⚠ `test_camera_check` 受 `if(USE_ORBBEC)` 保护——`USE_ORBBEC=OFF` 时**目标根本不存在**（不是构建失败）。
本机两个 build 的 `CMakeCache.txt` 均为 `USE_ORBBEC:BOOL=ON`。

⚠ 构建 ≠ 运行。`test_node_tree_check` 会临时改写 `config/process.json`（RAII 恢复），**不得在真机调试中运行**。

### 这些工具的**中文输出乱码** = 执行字符集 UTF-8 vs 控制台代码页 936（2026-09-23 修，TR-115）

- **现象**：用户在 cmd.exe 里跑 `test_camera_check.exe`，中文全是乱码；但**重定向到文件再打开却正常**。
- **机理（三层，缺一层就会误判）**：本仓 CMake 全局 `/utf-8`（`CMakeLists.txt:45`）⇒ MSVC 的**执行字符集**
  也是 UTF-8 ⇒ `std::printf` 吐的是 **UTF-8 字节**；而本机 `GetACP()` = `GetConsoleOutputCP()` = **936**（GBK）
  ⇒ 控制台按 GBK 解释这些字节 → 乱码。文件没有代码页概念，所以「重定向那条路」看不出问题。
- **取证手法（可复用）**：① ctypes 取 `GetACP()` / `GetConsoleOutputCP()`（`reg query` 会被沙箱黑名单拦掉，别试）；
  ② `grep -a` 在 exe 里搜某个中文字面量的 **UTF-8 字节序列**（如 `设备枚举` 的 `$'\xe8\xae\xbe...'`），命中即证「程序输出 UTF-8」。
- **修法**：三个控制台工具各在 `main()` 首行加 `SetConsoleOutputCP(CP_UTF8);`（`#ifdef _WIN32` 包裹），
  并以 `WIN32_LEAN_AND_MEAN` + `NOMINMAX` 包裹 `<windows.h>`（否则 `min`/`max` 宏会污染 `std::min`/`std::max`）。
  ⚠ 副作用需知情：它改的是**该控制台窗口**的输出代码页、进程退出**不自动还原**（对本工程是期望行为）。
- ⚠ **不要**改成「让用户先 `chcp 65001`」——脚本与现场都容易漏，且与「工具应当自解释」相悖。

### 跑测试工具：重定向到文件再读，别在控制台里读

⚠ **旧记忆「沙箱内跑不了 native exe 的管道/重定向」已被推翻**（2026-09-23 实测）：
`./test_camera_check.exe --stream 60 > out.txt 2>&1` 正常产出 5298 字节且 `exit=0`，`--stream 700` 长跑同样可用。
真正跑不了的是 `.bat`。做法：**重定向到 `.workbuddy-ai/tmp_*.txt`（已 gitignore）再用 Read 看**——
既绕开上面的乱码问题，也避免 SDK 的 `PropertyServer` / `PresetHandler` 警告刷屏（可用 `grep -avE` 先滤掉）。
⚠ 长跑（如 11.32 的 `--stream 700` ≈ 11.7 min）**超出 Bash 工具单次超时上限**，须 `run_in_background`。

## 其它

- **`error C2144`（「X 的前面应有 `;`」）+ `error C2761`（「不允许重新声明成员」）成对出现，且行号落在
  一个函数签名上 → 十有八九是编辑残留的「无函数体重复声明」**，不是真的语法问题（2026-09-23 实测：
  大段插入新函数时把 `void X::OnFrameReady(...)` 的**函数体丢了**，只剩签名，真定义在同文件另一处）。
  判据 = `grep -n "<函数名>" <文件>`，若同一签名出现**两次**且其中一处后面紧跟另一个函数定义 → 删掉那一处。
- 新增测试目标必须显式设 `RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"`，否则找不到 Qt DLL 启动即失败。
- 新增/删除源文件会触发 CMake reconfigure，而 configure 里的 `vcpkg install` **在沙箱内必失败**
  （`vcpkg-tool` 读 `vswhere.exe` 的 stdout 管道被拦 → `Value was null`）。改 `CMakeLists.txt` 需用户侧实跑。
- 判断 Qt API 语义**不要靠记忆**：本机 `D:/Qt/Docs/Qt-6.11.1/` 是完整离线文档，
  `grep` 关键词 + 剥 HTML 标签即可读原文。
