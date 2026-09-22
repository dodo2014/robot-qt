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

⚠ grep 必须加 `-a`，否则见到 windeployqt 的 GBK 字节会判定为二进制而停止输出。

**既有警告基线（不算新增）**：`SimCamera.cpp:126/127 C4244`、`SimCard.cpp:156 C4100`、
`BoPaiCard.cpp:237 C4244`、SDK `Filter.hpp:598 C4996`、`main.cpp:48 C4273`。

## LNK1168（exe 被占用）

```powershell
Get-Process -Name CreamPuffRobot -ErrorAction SilentlyContinue | Stop-Process -Force
```

Bash 下 `taskkill //IM` 会被 MSYS2 吃掉参数；`tasklist //FI` 过滤也失效（用 `tasklist | grep -ai creampuff` 才看得到）。

## 第 3 步：Sim 冒烟

**唯一判据 = 当日 `log/creampuff_<date>.log` 新增 `Initialize complete`**，不要靠脚本提示行。

```bash
cd /d/workspace/projects/CreamPuffRobot ; LOG="log/creampuff_$(date +%Y-%m-%d).log" ; BEFORE=$(grep -ac 'Initialize complete' "$LOG") ; cp config/config.json config/config.json.sim_bak ; sed -i 's/"motionCardType": "Bopai"/"motionCardType": "SimCard"/; s/"servoType": "XRServo"/"servoType": "SimServo"/' config/config.json ; ./out/build/x64-Debug/CreamPuffRobot.exe > /tmp/smoke_out.txt 2>/tmp/smoke_err.txt & APP_PID=$! ; sleep 22 ; AFTER=$(grep -ac 'Initialize complete' "log/creampuff_$(date +%Y-%m-%d).log") ; echo "SMOKE BEFORE=$BEFORE AFTER=$AFTER" ; kill -9 $APP_PID 2>/dev/null ; sleep 1 ; cp config/config.json.sim_bak config/config.json ; rm -f config/config.json.sim_bak ; echo "SMOKE_DONE"
```

⚠ **`A && B && C & D` 会把 `&` 之前整条链放进子 shell，链内变量赋值全丢**（`BEFORE` 读成空）。
必须用 `;` 分隔，且 `&` 只跟在 exe 命令后。
⚠ 末尾 `kill -9 $!` 有时会连带把自身 shell 打成 SIGTERM → 后续 `echo` 不落 stdout。
**因此单独复核两件事**：`grep -ac 'Initialize complete'` 的计数、以及 `config/config.json`
是否已还原成 `Bopai` / `XRServo`（`git diff -- config/` 里不该出现 `motionCardType` / `servoType`）。
⚠ 冒烟会真实删除 7 天前的 `log/creampuff_*.log`（S8 启动清理），属预期。

## 冒烟失败时的排查顺序

1. **先验构建完整性**（第 0 步脚本）。`Initialize complete` 发生在 `MainWindow` 页面构造
   **之后**，所以「没到 `Initialize complete`」**不能**推断「页面代码有问题」。
2. **崩溃日志尾部不可信**：`spdlog::flush_every(1s)` 会丢掉崩溃前 ≤1 s 的行。日志停在哪一行
   不代表崩在哪一行。
3. 拿 obj 与头文件 mtime 直接对：`ls -la --time-style=+%H:%M:%S <源> <obj>`。**任何 obj 早于
   它包含的头文件 = 构建不可信**，先 `-t clean` 重建再说，别去读代码。

## 其它

- 新增测试目标必须显式设 `RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"`，否则找不到 Qt DLL 启动即失败。
- 新增/删除源文件会触发 CMake reconfigure，而 configure 里的 `vcpkg install` **在沙箱内必失败**
  （`vcpkg-tool` 读 `vswhere.exe` 的 stdout 管道被拦 → `Value was null`）。改 `CMakeLists.txt` 需用户侧实跑。
- 判断 Qt API 语义**不要靠记忆**：本机 `D:/Qt/Docs/Qt-6.11.1/` 是完整离线文档，
  `grep` 关键词 + 剥 HTML 标签即可读原文。
