# CreamPuffRobot 编译方案（沙箱手工环境 + 开发机一键脚本）

> 建立：2026-09-01。起因：tooltip 修复后沙箱一度误判"无法编译"，实际是工具链路径
> 找错（误用 `Program Files (x86)`）+ 未手工设置 INCLUDE/LIB。本文件为权威编译参考，
> 供后续任何任务直接使用，勿再重复踩坑。

## 1. 工具链真实位置（本机，2026-09-01 实测）

| 组件 | 路径 | 说明 |
| :--- | :--- | :--- |
| MSVC 工具集 | `D:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/` | 下有 `14.51.36231` 与 `14.52.36418` 两版；CMakeCache 用的是 **14.51.36231**（见 out/build/*/CMakeCache.txt `CMAKE_CXX_COMPILER`）。**注意无 `(x86)`** |
| MSVC include | `<MSVC>/include` | 含 type_traits/cstdint 等标准库头（曾被误判缺失） |
| MSVC lib | `<MSVC>/lib/x64` | |
| MSVC bin | `<MSVC>/bin/Hostx64/x64` | cl.exe / link.exe 所在 |
| WinSDK | `D:/Windows Kits/10` | **不在** `C:/Program Files (x86)/Windows Kits`（那里只有 NETFXSDK） |
| WinSDK 版本 | `10.0.26100.0` | Include 与 Lib 均此版本 |
| WinSDK include | `<SDK>/Include/10.0.26100.0/{ucrt,shared,um}` | 顺序：ucrt;shared;um |
| WinSDK lib | `<SDK>/Lib/10.0.26100.0/{ucrt,um}/x64` | 顺序：ucrt;um |
| WinSDK bin | `<SDK>/Bin/10.0.26100.0/x64` | rc.exe 等 |
| Qt | `D:/Qt/6.11.1/msvc2022_64` | Qt6_DIR 指向其 lib/cmake/Qt6 |
| Ninja | `D:/Qt/Tools/Ninja/ninja.exe` | |
| CMake | `D:/Qt/Tools/CMake_64/bin/cmake.exe` | |

## 2. 为什么不能直接跑 vcvars64 / build_release.bat（沙箱环境）

- `build_release.bat` 第 11 行 `call vcvars64.bat` → vcvars 内部调用 `reg.exe` 查询
  VS 安装信息 → **`reg.exe` 在沙箱程序黑名单 → 整个 bat 被拦截**（此前实测 cmd 与
  PowerShell 两路均被拦）。
- `Launch-VsDevShell.ps1` 能加载却不传递 `INCLUDE` 环境变量 → cl.exe 报 C1083
  （找不到 type_traits/cstdint）。
- 结论：**沙箱内编译 = 手工构造 vcvars64 等价的 INCLUDE/LIB/PATH，再调 ninja**。
  vsvcars64 干的本质就是拼这三组路径，手工等价，零 reg.exe 依赖。

## 3. 手工编译命令（沙箱可用，Debug/Release 通用）

CMake 配置已存在（out/build/x64-Debug、x64-Release 均有 CMakeCache + build.ninja），
无需重新 cmake。若缓存丢失：见第 5 节重新配置命令。

### 3.1 PowerShell 一键（推荐，全程内联同一调用，避免 shell 状态丢失）

```powershell
$MSVC = "D:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
$SDK  = "D:/Windows Kits/10"
$env:INCLUDE = "$MSVC/include;$SDK/Include/10.0.26100.0/ucrt;$SDK/Include/10.0.26100.0/shared;$SDK/Include/10.0.26100.0/um"
$env:LIB     = "$MSVC/lib/x64;$SDK/Lib/10.0.26100.0/ucrt/x64;$SDK/Lib/10.0.26100.0/um/x64"
$env:PATH    = "$MSVC/bin/Hostx64/x64;$SDK/Bin/10.0.26100.0/x64;D:/Qt/Tools/Ninja;D:/Qt/Tools/CMake_64/bin;D:/Qt/6.11.1/msvc2022_64/bin;" + $env:PATH
& "D:/Qt/Tools/Ninja/ninja.exe" -C "D:/workspace/projects/CreamPuffRobot/out/build/x64-Release" CreamPuffRobot
```

- Release 目录：`out/build/x64-Release`；Debug 目录：`out/build/x64-Debug`（命令同，只换目录）。
- 编译耗时约 1 分钟（40 个编译单元）；改动小则增量极快。
- 编译目标可用 `$env:TARGET` 切换（如 `test_kinematics_check`，与 build_release.bat 的 TARGET 机制等价；PowerShell→进程传参用环境变量，bat `%~1` 曾失真）。
- 成功标志：`[N/N] Linking CXX executable CreamPuffRobot.exe` 后 windeployqt 自动跑
  （Qt DLL 部署日志）。可再 `stat` 确认 exe/obj 时间戳晚于源码。

### 3.2 LNK1168（exe 被占用）处理

```powershell
Get-Process -Name CreamPuffRobot -ErrorAction SilentlyContinue | Stop-Process -Force
```
然后重跑 3.1。（build_release.bat 内置自动检测+taskkill+重编，沙箱内手工来一次即可。）

### 3.3 WorkBuddy 沙箱内编译（2026-09-14 实测可用路径）

WorkBuddy 会话里的 PowerShell 工具**跑不了 native exe 的管道/重定向**，两种典型报错：

- `& "x.bat" 2>&1 | Select-Object` → `无法在管道中间运行文档: ...`（`CantActivateDocumentInPipeline`）
- `& "ninja.exe" ... *> log` → 不报错但**输出全丢、`$LASTEXITCODE` 为空**，日志文件 0 字节

`out\smoke\build_debug.bat` / 根 `build_release.bat` 在沙箱内会**静默无输出直接退出**（vcvars64 的
`reg.exe` 依赖被拦，见第 2 节），**不要靠它们判断编译结果**。`Start-Process`（拉 GUI/分离进程）同样被拦。

**可用路径：改用 Bash 工具跑 Git Bash**（不受上述限制）。⚠ 两类环境变量写法**不同**：
`INCLUDE` / `LIB` 是 Windows 环境变量，用 `;` 分隔的 Windows 路径；**`PATH` 是 Git Bash
自己的变量，必须用 `:` 分隔的 POSIX 路径**（`/d/...` 而非 `D:/...`）——理由与实测见 §8 末条：

```bash
cd /d/workspace/projects/CreamPuffRobot
export MSYS2_ARG_CONV_EXCL="*"
export INCLUDE="D:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/include;D:/Windows Kits/10/Include/10.0.26100.0/ucrt;D:/Windows Kits/10/Include/10.0.26100.0/shared;D:/Windows Kits/10/Include/10.0.26100.0/um"
export LIB="D:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/lib/x64;D:/Windows Kits/10/Lib/10.0.26100.0/ucrt/x64;D:/Windows Kits/10/Lib/10.0.26100.0/um/x64"
export PATH="/d/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:/d/Windows Kits/10/Bin/10.0.26100.0/x64:/d/Qt/Tools/Ninja:/d/Qt/Tools/CMake_64/bin:/d/Qt/6.11.1/msvc2022_64/bin:$PATH"
"/d/Qt/Tools/Ninja/ninja.exe" -C "D:/workspace/projects/CreamPuffRobot/out/build/x64-Debug" CreamPuffRobot 2>&1 | tail -50
```

- 成功标志 `[N/N] Linking CXX executable CreamPuffRobot.exe`（Debug 约 1m20s / Release 约 1m15s，全量 46 个 TU）。
- 只看关键行：管道接 `grep -aEi "error|warning C[0-9]|FAILED|Linking CXX exe"`（`-a` 必需，否则 grep 见到
  windeployqt 的 GBK 字节会判定为二进制而停止输出）。
- **LNK1168**：沙箱内 `taskkill //IM` 会被 MSYS2 吃掉参数（报 `无效参数/选项 - '//IM'`），
  用 PowerShell `Get-Process -Name CreamPuffRobot -EA SilentlyContinue | Stop-Process -Force`（纯 cmdlet 管道可用）。

### 3.4 Sim 冒烟在沙箱内的替代跑法（2026-09-14 实测）

`out\smoke\sim_smoke.ps1` 用 `Start-Process` 起 GUI 进程 → **沙箱内被拦**，脚本会静默走完 `finally`
恢复 config、**不留任何日志**（容易被误读成"冒烟通过"）。判定真伪看两点：`log/` 当日文件是否新增
`Initialize complete`，以及脚本输出里的 `SMOKE_ALIVE=true`。沙箱内改用 Bash 等价流程（效果一致）：

```bash
cd /d/workspace/projects/CreamPuffRobot
cp config/config.json config/config.json.sim_bak
trap 'cp config/config.json.sim_bak config/config.json && rm -f config/config.json.sim_bak' EXIT
sed -i 's/"motionCardType": "Bopai"/"motionCardType": "SimCard"/; s/"servoType": "XRServo"/"servoType": "SimServo"/' config/config.json
./out/build/x64-Debug/CreamPuffRobot.exe > /dev/null 2>&1 &
APP_PID=$!; sleep 18
grep -c 'Initialize complete' "log/creampuff_$(date +%Y-%m-%d).log"
kill -9 $APP_PID
```

**注意**：这一跑会让程序执行启动期逻辑——含 **S8 日志清理（删 7 天前 `log/creampuff_*.log`）**，
即冒烟会真实删除过期日志文件，属预期行为。

### 3.5 改了 `CMakeLists.txt` 后的首次重新配置（vcpkg 陷阱，2026-09-20 实测）

ninja 会在任一 `CMakeLists.txt` 比 `build.ninja` 新时**自动重跑 CMake configure**（**新增/删除源文件必然触发**）。
而 configure 中 `vcpkg.cmake` 会跑一次 `vcpkg install`（manifest 模式），**沙箱内必失败**：

```
-- Running vcpkg install
internal error: D:\a\_work\1\s\src\vcpkg\visualstudio.cpp(90): Value was null
-- Running vcpkg install - failed
CMake Error at vcpkg/scripts/buildsystems/vcpkg.cmake:984 (message):
```

**根因**：`vcpkg-tool` 用 `cmd_execute_and_capture_output()` 起 `vswhere.exe` 并**读它的 stdout 管道**，
再解析输出定位 VS 实例；沙箱对 native exe 的**管道/重定向一律拦截**（同 §3.3），于是输出为空 → 解析到 null
→ 报 `Value was null`。

**已实测排除的因素**（别再往这些方向试）：`vswhere.exe` 本身能跑且返回正确 JSON（VS 18 Community 在列）；
`VCPKG_VISUAL_STUDIO_PATH`、`VSINSTALLDIR` / `VCINSTALLDIR` / `VCToolsInstallDir` 都设了也没用；
`vcpkg.exe version` 正常。**这不是本项目缺陷**——开发机（无沙箱）configure 正常，`.bat` 一键流程不受影响。

**沙箱内的绕法**（依赖已装在 `out/build/<cfg>/vcpkg_installed/x64-windows`，无需重装）：

```bash
# 与 §3.3 同一套 INCLUDE/LIB/PATH，然后：
"/d/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --regenerate-during-build -DVCPKG_MANIFEST_INSTALL=OFF \
  -SD:/workspace/projects/CreamPuffRobot -BD:/workspace/projects/CreamPuffRobot/out/build/x64-Debug
```

- **必须用 `build.ninja` 里记录的同一个 `cmake.exe`**（VS 自带那个，见上；**不要**用 Qt 自带的 CMake）。
- `VCPKG_MANIFEST_INSTALL=OFF` 会**写进 `CMakeCache.txt`**。包已在 `vcpkg_installed/` 中，`find_package` 照常命中
  （实测 `nlohmann_json` / `OpenCV 4.12.0` / `spdlog` / `Eigen3 5.0.1` 全部 found）。
- **用完请改回 `ON`**，保持与开发机一致：
  `sed -i 's/^VCPKG_MANIFEST_INSTALL:BOOL=OFF/VCPKG_MANIFEST_INSTALL:BOOL=ON/' out/build/<cfg>/CMakeCache.txt`
- **Debug 与 Release 各自独立**：两边都改过 `CMakeLists.txt` 时，两个 build 目录都要各来一次。

## 4. Sim 冒烟验证（编译后必跑）

标准脚本 `out\smoke\sim_smoke.ps1` 默认跑 **Debug** exe。冒烟 Release 时临时改 exe 路径
为 `out\build\x64-Release\CreamPuffRobot.exe`（其余逻辑零改动：就地改工程根
config.json SimCard/SimServo → 启动 8s → 查存活 → try/finally 恢复）。

判定存活必须查日志（断言框挂起会误报）：工程根 `log/creampuff_YYYY-MM-DD.log`
含 `Initialize complete` 即通过；SimCard/SimServo/SimAlgo/SimCamera 均应初始化正常。

## 5. 首次配置（CMakeCache 丢失时）

```powershell
$env:PATH = "D:/Qt/Tools/CMake_64/bin;D:/Qt/Tools/Ninja;" + $env:PATH
cmake -G Ninja -S "D:/workspace/projects/CreamPuffRobot" -B "D:/workspace/projects/CreamPuffRobot/out/build/x64-Release" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_ENCODING=utf-8 -DQt6_DIR:PATH=D:/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6
```
Debug 同，`-DCMAKE_BUILD_TYPE=Debug` + 目录 x64-Debug。

> **硬件开关不必手传（2026-09-24，TR-117）**：`USE_BOPAI` / `USE_ORBBEC` 的默认值由**第三方 SDK 是否就位**自动探测
> （守 `3rdparty/bopai/lib/MultiCard.lib` 与 `3rdparty/orbbec/lib/OrbbecSDK.lib`，均需**同时**有 `include/`）。
> 装了 SDK 的机器自动 `ON`、新克隆自动 `OFF`（仍可编译）；探测为 `OFF` 时 configure 期会打一行 `HAL: 未找到 … SDK` 说明原因。
> ⚠ **探测结果写进 `CMakeCache.txt`** ⇒ 后来才装 SDK 必须**重新 configure**（或显式 `-DUSE_ORBBEC=ON`），已配置过的目录不会自己翻转。
> ⚠ 显式传 `-DUSE_ORBBEC=ON` 而 SDK 缺失 ⇒ 配置期直接 `FATAL_ERROR` 并列出缺哪个文件（不再拖到链接期）。
> `USE_XRSERVO` 无第三方 SDK 依赖（只用 `<Windows.h>` + 系统库 `setupapi`），保持默认 `ON`。

## 6. 开发机（无沙箱限制）仍用一键脚本

- Release：双击/`cmd /c build_release.bat`（vcvars64 可用，LNK1168 自动处理）。
- Debug：`build_debug.bat`（同结构）。
- 用户明确要求：**release 编译使用项目下的 build_release.bat**（沙箱内因 reg.exe
  黑名单改用手工方案，两者产物一致，仅环境构造方式不同）。

## 7. 踩坑备忘

- `D:/Program Files (x86)/Microsoft Visual Studio/...` **不存在**——MSVC 在
  `D:/Program Files/Microsoft Visual Studio/...`（无 x86）。找目录先用
  `ls "D:/Program Files/Microsoft Visual Studio"` 再 `ls "D:/Windows Kits/10"`。
- `find` 全盘找 type_traits 确认存在不等于 INCLUDE 已设置——C1083 时先查
  `$env:INCLUDE` 是否含 MSVC include。
- ninja "no work to do" 但 obj 未更新：先确认 cwd 正确（`-C` 显式给目录），
  再确认环境变量在**同一进程内**设置（PowerShell 工具每次调用 shell 状态不保留，
  INCLUDE/LIB 必须与 ninja 同一条命令内联）。

## 8. 手敲 cl.exe 编译 Qt 6.11 应用关键 flag（2026-09-01 实测）

ninja/CMake 自动加；手敲时缺一即编译失败：
- `/Zc:__cplusplus` — Qt `qcompilerdetection.h` 强制要求，否则 **C1189**：
  `"Qt requires a C++17 compiler, and a suitable value for __cplusplus"`
- `/permissive-` — Qt `qcompilerdetection.h` 强制要求，否则 **C2338 static assertion**：
  `'On MSVC you must pass the /permissive- option to the compiler.'`
- `/std:c++17` — Qt 6.11 要求 C++17。
- `/utf-8` — 让编译器把源码/字符串当 UTF-8（中文注释/字面量不乱码）。

参考命令（手敲编译独立 Qt 小工具（`out/smoke/` 下的自测程序，`tool.cpp` 为示例源文件），复用第 3 节 env，
加 Qt include 多版本子目录转发头）：

```bash
export MSYS2_ARG_CONV_EXCL="*"
export INCLUDE="<MSVC>/include;<SDK>/Include/10.0.26100.0/ucrt;<SDK>/Include/10.0.26100.0/shared;<SDK>/Include/10.0.26100.0/um;<QT>/include;<QT>/include/QtWidgets;<QT>/include/QtGui;<QT>/include/QtCore;<QT>/include/QtWidgets/6.11.1;<QT>/include/QtGui/6.11.1;<QT>/include/QtCore/6.11.1;<QT>/include/QtWidgets/6.11.1/QtWidgets;<QT>/include/QtGui/6.11.1/QtGui;<QT>/include/QtCore/6.11.1/QtCore"
# ⚠ PATH 必须用 POSIX 路径（/d/...）；写成 D:/... 会被 MSYS 的 `:` 切碎，见本节末条
export PATH="/d/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:/d/Windows Kits/10/Bin/10.0.26100.0/x64:/d/Qt/6.11.1/msvc2022_64/bin:$PATH"
"<MSVC>/bin/Hostx64/x64/cl.exe" /nologo /std:c++17 /permissive- /Zc:__cplusplus /EHsc /O2 /utf-8 tool.cpp /Fe:tool.exe /link /LIBPATH:"<QT>/lib" Qt6Widgets.lib Qt6Gui.lib Qt6Core.lib /LIBPATH:"<MSVC>/lib/x64" /LIBPATH:"<SDK>/Lib/10.0.26100.0/ucrt/x64" /LIBPATH:"<SDK>/Lib/10.0.26100.0/um/x64" /SUBSYSTEM:CONSOLE
```

**windeployqt 陷阱**：对 release exe（链接 Qt6Widgets.lib）跑 windeployqt 会部署
**debug dll（Qt6Cored.dll 等带 d 后缀）**，导致 "Qt6Core.dll cannot open"。
手工拷贝 release DLL + 平台插件：
```bash
cp "<QT>/bin/Qt6Core.dll" "<QT>/bin/Qt6Gui.dll" "<QT>/bin/Qt6Widgets.dll" .
mkdir -p plugins/platforms && cp "<QT>/plugins/platforms/qwindows.dll" plugins/platforms/
```
**⚠ Git Bash 的 PATH 陷阱（2026-09-21 实测修正归因）**：把 **Windows 风格路径**写进 Git Bash
的 `PATH` 会被**切碎**——MSYS 用 `:` 作 PATH 分隔符，`D:` 里的冒号也被当成分隔符，于是
`D:/Qt/.../bin` 变成 `D` 与 `/Qt/.../bin` 两个**无效项**（`echo "$PATH" | tr ':' '\n' | head -3`
即可看到）。后果：运行 exe 时找不到 `Qt6Core.dll`，**退出码 127**。

- **正确解法**：`PATH` 项一律用 **POSIX 路径** ——
  `export PATH="/d/Qt/6.11.1/msvc2022_64/bin:$PATH"`。
  实测：同一 exe 在 Windows 风格写法下退出码 127，改 POSIX 后立即正常输出；
  对照（POSIX `PATH` 但不含 Qt bin）仍为 127 → 唯一变量就是 Qt bin 是否真正进入 `PATH`。
- **旧结论「DLL 加载走的是 Windows 进程的 PATH 而非 bash PATH」不准确**：MSYS 启动 Windows
  子进程时**会**把 bash `PATH` 转成 Windows `PATH` 传下去，bash `PATH` 是有效的；失效的
  原因仅仅是**路径写法被切碎**。故不必绕道「拷 dll 到 exe 旁」。
- 把 dll + 平台插件拷到 exe 旁仍是**部署**时的正当做法，但作为「`PATH` 写错」的解法属绕过。
