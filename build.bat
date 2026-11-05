@echo off
rem ==========================================================================
rem  Lyra build script. Works from any console (cmd, PowerShell, Explorer).
rem    build.bat          - configure (if needed) + build
rem    build.bat fresh    - wipe the CMake cache and configure from scratch
rem    build.bat run      - build and start run\Lyra.exe
rem    build.bat gst      - build against WebKit with GStreamer (YouTube/MSE) into run-gst\
rem                         (arguments combine: build.bat gst fresh run)
rem ==========================================================================
setlocal

rem --- 1. Visual Studio environment (INCLUDE/LIB for the Windows SDK) ---------
rem NOTE: no "( ... )" blocks around paths with "Program Files (x86)" - the ")" would
rem end the block and abort the script.
if defined VSINSTALLDIR goto :have_vs
set "VSPATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :vs_fallback
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
:vs_fallback
if not defined VSPATH set "VSPATH=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools"
if not exist "%VSPATH%\Common7\Tools\VsDevCmd.bat" goto :no_vs
call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo
if errorlevel 1 goto :no_vs
:have_vs

rem --- 2. Compiler: clang-cl from the LLVM install (same as WebKit) -----------
set "CLANGCL=%ProgramFiles%\LLVM\bin\clang-cl.exe"
if exist "%CLANGCL%" goto :have_clang
for /f "delims=" %%i in ('where clang-cl 2^>nul') do (
    set "CLANGCL=%%i"
    goto :have_clang
)
:have_clang
if not exist "%CLANGCL%" goto :no_clang
set "CLANGCL=%CLANGCL:\=/%"

cd /d "%~dp0"

rem --- 3. Arguments (any order): fresh, run, gst ------------------------------------
rem   gst = build against WebKit with the GStreamer backend (WebKitBuild\ReleaseGst,
rem         made by C:\WebKit\build-webkit-gst.bat) into build-gst\ and run-gst\.
set "FRESH="
set "RUN="
set "BUILDDIR=build"
set "RUNDIR=run"
set "WEBKITDIR=C:/WebKit/WebKit/WebKitBuild/Release"
for %%a in (%*) do (
    if /i "%%a"=="fresh" set "FRESH=--fresh"
    if /i "%%a"=="run" set "RUN=1"
    if /i "%%a"=="gst" (
        set "BUILDDIR=build-gst"
        set "RUNDIR=run-gst"
        set "WEBKITDIR=C:/WebKit/WebKit/WebKitBuild/ReleaseGst"
    )
)

rem --- 4. Leftover browser processes keep the WebKit DLLs locked: stop them --------
for %%p in (Lyra.exe MyBrowser.exe WebKitGPUProcess.exe WebKitWebProcess.exe WebKitNetworkProcess.exe) do (
    tasklist /fi "imagename eq %%p" 2>nul | find /i "%%p" >nul && (
        echo [build] %%p is still running, stopping it:
        taskkill /f /t /im %%p
    )
)
rem A process that cannot be stopped still lets its files be renamed: move them aside so
rem the new ones can be copied in. The *.locked files are removed by a later build.
for %%d in (run run-gst) do if exist "%~dp0%%d" (
    del /q "%~dp0%%d\*.locked" >nul 2>&1
    for %%f in ("%~dp0%%d\*.dll" "%~dp0%%d\*.exe") do (
        2>nul ( >>"%%~f" call; ) || (
            echo [build] %%~nxf is in use, moving it aside
            move /y "%%~f" "%%~f.%RANDOM%.locked" >nul
        )
    )
)

rem --- 4. Configure + build ----------------------------------------------------
rem A build folder configured when the project lived in C:\WebKit\MyBrowser cannot be
rem reused from C:\WebKit\Lyra: configure it from scratch.
if exist "%BUILDDIR%\CMakeCache.txt" findstr /i /c:"WebKit/MyBrowser" "%BUILDDIR%\CMakeCache.txt" >nul && (
    echo [build] %BUILDDIR% was set up for the old MyBrowser folder: configuring it fresh
    set "FRESH=--fresh"
)
set "RUNPATH=%~dp0%RUNDIR%"
set "RUNPATH=%RUNPATH:\=/%"
cmake %FRESH% -S . -B %BUILDDIR% -G Ninja "-DCMAKE_CXX_COMPILER=%CLANGCL%" "-DWEBKIT_BUILD_DIR=%WEBKITDIR%" "-DLYRA_RUN_DIR=%RUNPATH%"
if errorlevel 1 goto :fail
cmake --build %BUILDDIR%
if errorlevel 1 goto :fail

echo.
echo [build] OK: %~dp0%RUNDIR%\Lyra.exe
if defined RUN start "" "%~dp0%RUNDIR%\Lyra.exe"
exit /b 0

:no_vs
echo [build] Visual Studio C++ build tools not found. Install "Desktop development with C++".
goto :fail
:no_clang
echo [build] clang-cl.exe not found. Install LLVM or add it to PATH.
goto :fail
:fail
echo.
echo [build] FAILED - see the messages above.
pause
exit /b 1
