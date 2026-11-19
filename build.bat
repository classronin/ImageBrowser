@echo off
rem ============================================================================
rem  ImageBrowser - ClassRonin : build script
rem  Usage:
rem      build.bat              -> Release x64, incremental
rem      build.bat debug        -> Debug   x64, incremental
rem      build.bat release clean-> Release x64, full rebuild
rem  Output: bin\x64\<Configuration>\ImageBrowser.exe  (+ ImageBrowser.ini at runtime)
rem  Requirements: Visual Studio 2022 with the "Desktop development with C++"
rem  workload (v143 toolset, Windows 10/11 SDK). No NuGet, no external deps:
rem  every third-party library is a static .lib already inside third_party\.
rem ============================================================================
setlocal EnableExtensions

set "CONFIG=Release"
set "ACTION=Build"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="debug"   ( set "CONFIG=Debug"   & shift & goto parse )
if /i "%~1"=="release" ( set "CONFIG=Release" & shift & goto parse )
if /i "%~1"=="clean"   ( set "ACTION=Rebuild" & shift & goto parse )
if /i "%~1"=="rebuild" ( set "ACTION=Rebuild" & shift & goto parse )
echo Unknown argument: %~1
echo Usage: build.bat [debug^|release] [clean]
exit /b 2
:parsed

cd /d "%~dp0"

rem ---- locate MSBuild -------------------------------------------------------
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"

if not defined MSBUILD (
	echo [error] MSBuild.exe not found. Install Visual Studio 2022 with "Desktop development with C++".
	exit /b 1
)

echo [info] MSBuild : %MSBUILD%
echo [info] Config  : %CONFIG% ^| x64
echo [info] Action  : %ACTION%
echo.

"%MSBUILD%" ImageBrowser.sln /t:%ACTION% /p:Configuration=%CONFIG% /p:Platform=x64 /m /v:m /nologo
if errorlevel 1 (
	echo.
	echo [error] build failed.
	exit /b 1
)

set "EXE=bin\x64\%CONFIG%\ImageBrowser.exe"
if not exist "%EXE%" (
	echo [error] expected output missing: %EXE%
	exit /b 1
)
for %%f in ("%EXE%") do echo [done] %%f  ^(%%~zf bytes^)
exit /b 0
