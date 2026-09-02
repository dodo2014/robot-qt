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

参考命令（编译 `out/smoke/tipcheck/tipcheck.cpp` 等独立 Qt 小工具，复用第 3 节 env，
加 Qt include 多版本子目录转发头）：

```bash
export MSYS2_ARG_CONV_EXCL="*"
export INCLUDE="<MSVC>/include;<SDK>/Include/10.0.26100.0/ucrt;<SDK>/Include/10.0.26100.0/shared;<SDK>/Include/10.0.26100.0/um;<QT>/include;<QT>/include/QtWidgets;<QT>/include/QtGui;<QT>/include/QtCore;<QT>/include/QtWidgets/6.11.1;<QT>/include/QtGui/6.11.1;<QT>/include/QtCore/6.11.1;<QT>/include/QtWidgets/6.11.1/QtWidgets;<QT>/include/QtGui/6.11.1/QtGui;<QT>/include/QtCore/6.11.1/QtCore"
export PATH="<MSVC>/bin/Hostx64/x64;<SDK>/Bin/10.0.26100.0/x64;<QT>/bin:$PATH"
"<MSVC>/bin/Hostx64/x64/cl.exe" /nologo /std:c++17 /permissive- /Zc:__cplusplus /EHsc /O2 /utf-8 tipcheck.cpp /Fe:tipcheck.exe /link /LIBPATH:"<QT>/lib" Qt6Widgets.lib Qt6Gui.lib Qt6Core.lib /LIBPATH:"<MSVC>/lib/x64" /LIBPATH:"<SDK>/Lib/10.0.26100.0/ucrt/x64" /LIBPATH:"<SDK>/Lib/10.0.26100.0/um/x64" /SUBSYSTEM:CONSOLE
```

**windeployqt 陷阱**：对 release exe（链接 Qt6Widgets.lib）跑 windeployqt 会部署
**debug dll（Qt6Cored.dll 等带 d 后缀）**，导致 "Qt6Core.dll cannot open"。
手工拷贝 release DLL + 平台插件：
```bash
cp "<QT>/bin/Qt6Core.dll" "<QT>/bin/Qt6Gui.dll" "<QT>/bin/Qt6Widgets.dll" .
mkdir -p plugins/platforms && cp "<QT>/plugins/platforms/qwindows.dll" plugins/platforms/
```
**Git Bash PATH 陷阱**：`export PATH="$QT/bin:$PATH"` 后运行 Windows exe 报
`Qt6Core.dll cannot open` — DLL 加载走的是 Windows 进程的 PATH 而非 bash PATH，
解决 = 把 dll 放到 exe 旁（同上拷贝），不要依赖 PATH。

## 9. Tooltip 验证工具 tipcheck（2026-09-01 落盘）

`out/smoke/tipcheck/tipcheck.cpp` —— 复刻主程序 QSS + Fusion + 深色 UI，弹出真实 tooltip，
`QScreen::grabWindow(0)` 截图分析像素白底占比。**任何后续 tooltip 样式改动都必须
跑它做像素级验证**，避免再次踩"以为修好了但用户侧仍暗色"的坑。

四模式对比：
- `tipcheck.exe nopal` — 仅 QSS QToolTip 规则（验证旧 QSS 方案 → 实测无效）
- `tipcheck.exe palette` — QSS QToolTip + setPalette（验证 setPalette 方案 → 实测无效）
- `tipcheck.exe qtiplabel` — QSS QToolTip, QTipLabel 规则无 setPalette（验证纯 QSS 方案）
- `tipcheck.exe current` — QSS QTipLabel + setPalette（当前主程序方案，最强）

编译命令见第 8 节。运行需 Qt6Core/Gui/Widgets.dll + plugins/platforms/qwindows.dll
部署在 exe 旁（见第 8 节末尾）。
