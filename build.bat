@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo === ImageBrowser 编译 ===
echo 源码目录: %CD%
echo.

where cmake >nul 2>nul
if errorlevel 1 (
    echo [错误] 找不到 cmake，请先安装 CMake 并把它加进 PATH。
    goto :fail
)

if not exist "build\CMakeCache.txt" (
    echo [1/2] 配置工程 - Visual Studio 17 2022 x64
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 goto :fail
) else (
    echo [1/2] 已有 build 目录，跳过配置
)

echo.
echo [2/2] 编译 Release
cmake --build build --config Release
if errorlevel 1 goto :fail

if not exist "build\Release\ImageBrowser.exe" (
    echo [错误] 没找到 build\Release\ImageBrowser.exe
    goto :fail
)

copy /y "build\Release\ImageBrowser.exe" "ImageBrowser.exe" >nul
echo.
echo === 完成 ===
echo 输出: %CD%\ImageBrowser.exe
for %%A in ("ImageBrowser.exe") do echo 大小: %%~zA 字节
goto :done

:fail
echo.
echo [失败] 编译没有完成，请看上面的输出。
pause
exit /b 1

:done
pause
exit /b 0
