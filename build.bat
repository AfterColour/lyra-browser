@echo off
rem ==========================================================================
rem  Lyra build script. Works from any console (cmd, PowerShell, Explorer).
rem    build.bat          - configure (if needed) + build
rem    build.bat fresh    - wipe the CMake cache and configure from scratch
rem    build.bat run      - build and start run\Lyra.exe
rem                         (arguments combine: build.bat fresh run)
rem  Lyra is built against WebKit with the GStreamer media backend (YouTube, MSE):
rem  WebKitBuild\ReleaseGst, made by C:\WebKit\build-webkit-gst.bat.
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

rem --- 3. Arguments (any order): fresh, run --------------------------------------------
rem   ("gst" is still accepted and changes nothing: every build is the GStreamer one now.)
set "FRESH="
set "RUN="
set "BUILDDIR=build"
set "RUNDIR=run"
set "WEBKITDIR=C:/WebKit/WebKit/WebKitBuild/ReleaseGst"
for %%a in (%*) do (
    if /i "%%a"=="fresh" set "FRESH=--fresh"
    if /i "%%a"=="run" set "RUN=1"
)
set "WEBKITDIR_WIN=%WEBKITDIR:/=\%"
if not exist "%WEBKITDIR_WIN%\bin\WebKit2.dll" goto :no_webkit

rem --- 4. Leftover browser processes keep the WebKit DLLs locked: stop them --------
rem Only those started from this run\ folder: an installed Lyra (and its WebKit processes) stays open.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$run = '%~dp0run\'; Get-Process Lyra,MyBrowser,WebKitGPUProcess,WebKitWebProcess,WebKitNetworkProcess -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($run, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Write-Host ('[build] ' + $_.Name + ' is still running, stopping it'); Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }; Start-Sleep -Milliseconds 300"
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

rem --- 5. Configure + build ----------------------------------------------------
rem run\ used to hold the WebKit build without GStreamer: keep that one aside as run-old\
rem (nothing is deleted) so no DLL of the old build is left among the new ones.
if exist "%~dp0run\Lyra.exe" if not exist "%~dp0run\gstreamer-1.0" (
    if exist "%~dp0run-old" (
        echo [build] run\ is from the old build and run-old\ already exists: move one of them away first.
        goto :fail
    )
    echo [build] run\ is from the build without GStreamer: keeping it as run-old\
    move "%~dp0run" "%~dp0run-old" >nul || goto :fail
)
rem build\ configured against the other WebKit build: configure it fresh.
if exist "%BUILDDIR%\CMakeCache.txt" findstr /i /c:"WEBKIT_BUILD_DIR:PATH=%WEBKITDIR%" "%BUILDDIR%\CMakeCache.txt" >nul || (
    if exist "%BUILDDIR%\CMakeCache.txt" (
        echo [build] %BUILDDIR% was set up for another WebKit build: configuring it fresh
        set "FRESH=--fresh"
    )
)
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

:no_webkit
echo [build] WebKit with GStreamer not found: %WEBKITDIR%
echo [build] Build it first with C:\WebKit\build-webkit-gst.bat
goto :fail
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
