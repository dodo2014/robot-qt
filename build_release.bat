@echo off
rem ============================================================
rem  CreamPuffRobot Release one-click build script
rem  Usage: double-click or run build_release.bat
rem  Output: out\build\x64-Release\CreamPuffRobot.exe
rem  Optional: set env TARGET to build another target, e.g.
rem    PowerShell: $env:TARGET='test_kinematics_check'; .\build_release.bat
rem  NOTE: keep this file ASCII-only; non-ASCII in .bat breaks cmd code-page parsing.
rem ============================================================
call "D:\PROGRA~2\MICROS~1\18\COMMUN~1\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=D:\Qt\Tools\CMake_64\bin;D:\Qt\Tools\Ninja;D:\Qt\6.11.1\msvc2022_64\bin;%PATH%"

rem Project root = directory of this script (strip trailing backslash)
set "SRC=%~dp0"
set "SRC=%SRC:~0,-1%"
set "BUILD=%SRC%\out\build\x64-Release"

if not exist "%BUILD%\CMakeCache.txt" (
    cmake -G Ninja -S "%SRC%" -B "%BUILD%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_ENCODING=utf-8 -DQt6_DIR:PATH=D:/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6
    if errorlevel 1 exit /b 1
)
if "%TARGET%"=="" set "TARGET=CreamPuffRobot"
cmake --build "%BUILD%" --target %TARGET%
if errorlevel 1 exit /b 1
echo.
echo Build OK: %BUILD%\%TARGET%.exe

