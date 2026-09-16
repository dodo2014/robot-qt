@echo off
setlocal
rem ============================================================
rem  CreamPuffRobot Release one-click build + package script
rem  Usage: double-click or run build_release.bat
rem  Build output  : out\build\x64-Release\CreamPuffRobot.exe
rem  Package output: out\dist\CreamPuffRobot\  (+ CreamPuffRobot_x64_Release.zip)
rem  Optional: set env TARGET to build another target, e.g.
rem    PowerShell: $env:TARGET='test_kinematics_check'; .\build_release.bat
rem  Optional: set env SKIP_PACKAGE=1 to build only, without refreshing out\dist.
rem  Auto-handles LNK1168 (exe is running): kills CreamPuffRobot.exe and retries once.
rem  NOTE 1: keep this file ASCII-only; non-ASCII in .bat breaks cmd code-page parsing.
rem  NOTE 2: this file uses LF line endings (same as before). Keep the packaged
rem          section free of multi-line parenthesised blocks to stay LF-safe.
rem ============================================================
call "D:\PROGRA~2\MICROS~1\18\COMMUN~1\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=D:\Qt\Tools\CMake_64\bin;D:\Qt\Tools\Ninja;D:\Qt\6.11.1\msvc2022_64\bin;%PATH%"

rem Project root = directory of this script (strip trailing backslash)
set "SRC=%~dp0"
set "SRC=%SRC:~0,-1%"
set "BUILD=%SRC%\out\build\x64-Release"
set "LOG=%~dp0build_release.log"

if not exist "%BUILD%\CMakeCache.txt" (
    cmake -G Ninja -S "%SRC%" -B "%BUILD%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_ENCODING=utf-8 -DQt6_DIR:PATH=D:/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6
    if errorlevel 1 exit /b 1
)
if "%TARGET%"=="" set "TARGET=CreamPuffRobot"
cmake --build "%BUILD%" --target %TARGET% 1>"%LOG%" 2>&1
if errorlevel 1 goto :retry
echo.
echo Build OK: %BUILD%\%TARGET%.exe
del "%LOG%" >nul 2>&1
goto :package

:retry
findstr /C:"LNK1168" "%LOG%" >nul 2>&1
if errorlevel 1 (
    type "%LOG%"
    del "%LOG%" >nul 2>&1
    exit /b 1
)
echo [build_release] LNK1168 detected: exe is running. Killing CreamPuffRobot.exe and retrying once...
taskkill /IM CreamPuffRobot.exe /F >nul 2>&1
ping -n 2 127.0.0.1 >nul
cmake --build "%BUILD%" --target %TARGET%
if errorlevel 1 (
    echo [build_release] Retry failed. Check output above.
    del "%LOG%" >nul 2>&1
    exit /b 1
)
echo.
echo Build OK: %BUILD%\%TARGET%.exe (after LNK1168 auto-retry)
del "%LOG%" >nul 2>&1

rem ============================================================
rem  Packaging stage (added 2026-09-16)
rem  Collects exactly the deployable runtime set from the build dir:
rem    CreamPuffRobot.exe + top-level *.dll (Qt6 / MSVC runtime /
rem    MultiCard SDK / OpenCV / spdlog / fmt) + config.json +
rem    process.json + the Qt plugin folders.
rem  Deliberately EXCLUDED (build-only artefacts): CMakeCache.txt,
rem    CMakeFiles\, build.ninja, vcpkg_installed\, src\, tests\,
rem    CreamPuffRobot_autogen\, .qt\, *.pdb, *.lib.
rem  The deployed app reads config.json / process.json from the folder
rem    next to the exe (see ConfigPage::ensureConfigLoaded).
rem ============================================================
:package
if /I not "%TARGET%"=="CreamPuffRobot" goto :pkg_skip_target
if "%SKIP_PACKAGE%"=="1" goto :pkg_skip_flag

set "DIST=%SRC%\out\dist\CreamPuffRobot"
set "ZIP=%SRC%\out\dist\CreamPuffRobot_x64_Release.zip"
echo [build_release] Packaging to %DIST% ...

if exist "%DIST%" rmdir /S /Q "%DIST%"
if exist "%DIST%" goto :pkg_err_inuse
mkdir "%DIST%"
if not exist "%DIST%" goto :pkg_err_create

rem --- main binary ---
copy /Y "%BUILD%\CreamPuffRobot.exe" "%DIST%\" >nul
rem --- runtime DLLs: Qt6 + MSVC runtime + MultiCard SDK + OpenCV + spdlog/fmt ---
copy /Y "%BUILD%\*.dll" "%DIST%\" >nul
rem --- runtime config: the app reads config.json / process.json next to the exe ---
copy /Y "%BUILD%\config.json" "%DIST%\" >nul
copy /Y "%BUILD%\process.json" "%DIST%\" >nul
rem --- Qt plugins (platforms\qwindows.dll is mandatory; the rest are optional) ---
for %%P in (platforms imageformats styles iconengines generic tls networkinformation translations) do @if exist "%BUILD%\%%P" xcopy /E /I /Q /Y "%BUILD%\%%P" "%DIST%\%%P\" >nul

if not exist "%DIST%\CreamPuffRobot.exe" goto :pkg_err_create
if not exist "%DIST%\platforms\qwindows.dll" goto :pkg_err_plugin

echo [build_release] Compressing to %ZIP% ...
if exist "%ZIP%" del /Q "%ZIP%" >nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%DIST%\*' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 goto :pkg_zip_warn

echo [build_release] Package OK.
echo [build_release] Folder: %DIST%
echo [build_release] Zip   : %ZIP%
exit /b 0

:pkg_zip_warn
echo [build_release] WARNING: zip step failed; the folder package is still usable.
echo [build_release] Folder: %DIST%
exit /b 0

:pkg_skip_target
echo [build_release] TARGET=%TARGET% is not the main app, skipping packaging.
exit /b 0

:pkg_skip_flag
echo [build_release] SKIP_PACKAGE=1, skipping packaging.
exit /b 0

:pkg_err_inuse
echo [build_release] ERROR: cannot clean "%DIST%" - still in use?
exit /b 1

:pkg_err_create
echo [build_release] ERROR: package folder incomplete: %DIST%
exit /b 1

:pkg_err_plugin
echo [build_release] ERROR: Qt platform plugin missing: %DIST%\platforms\qwindows.dll
exit /b 1
