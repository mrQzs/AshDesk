@echo off
rem Build the AshDesk desktop app (MSVC + Ninja). Usage: build.bat [extra args for cmake --build]
rem Qt: set QT_DIR, default C:\Qt\6.10.1\msvc2022_64.
setlocal
set "VSDIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
rem vswhere.exe has gone missing before: fall back to the standard VS 2022 folders
if not defined VSDIR for %%e in (Enterprise Professional Community BuildTools) do if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\%%e"
if not defined VSDIR ( echo Visual Studio C++ toolset not found & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not defined QT_DIR set "QT_DIR=C:\Qt\6.10.1\msvc2022_64"
if not exist build\CMakeCache.txt cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_PREFIX_PATH=%QT_DIR%" || exit /b 1
cmake --build build %* || exit /b 1
