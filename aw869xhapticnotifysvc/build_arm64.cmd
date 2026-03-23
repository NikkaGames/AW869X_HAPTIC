@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -host_arch=amd64 -arch=arm64 || exit /b 1
set SDKROOT=%ProgramFiles(x86)%\Windows Kits\10
if not exist "%SDKROOT%\Include" (
    echo Windows SDK not found at "%SDKROOT%"
    exit /b 1
)
set SDKVER=
for /f %%I in ('dir /b /ad "%SDKROOT%\Include" ^| sort /r') do (
    set SDKVER=%%I
    goto :sdk_found
)
:sdk_found
if "%SDKVER%"=="" (
    echo Windows SDK version not found
    exit /b 1
)
set SCRIPT_DIR=%~dp0
if "%SCRIPT_DIR:~-1%"=="\" set SCRIPT_DIR=%SCRIPT_DIR:~0,-1%
set OUTDIR=%SCRIPT_DIR%\ARM64\Release
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
cl /nologo /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
  /I"%SDKROOT%\Include\%SDKVER%\ucrt" ^
  /I"%SDKROOT%\Include\%SDKVER%\shared" ^
  /I"%SDKROOT%\Include\%SDKVER%\um" ^
  /Fe:"%OUTDIR%\aw869xhapticnotifysvc.exe" ^
  "%SCRIPT_DIR%\aw869xhapticnotifysvc.cpp" ^
  /link ^
  /LIBPATH:"%SDKROOT%\Lib\%SDKVER%\ucrt\arm64" ^
  /LIBPATH:"%SDKROOT%\Lib\%SDKVER%\um\arm64" ^
  advapi32.lib setupapi.lib wevtapi.lib
