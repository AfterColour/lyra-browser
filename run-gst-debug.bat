@echo off
rem ==========================================================================
rem  Runs run\Lyra.exe with GStreamer logging enabled.
rem  One log per process: C:\WebKit\gstlogs\gst-<pid>.log
rem  The folder is cleared on every start, so it only has the latest run.
rem ==========================================================================
setlocal
set "LOGDIR=C:\WebKit\gstlogs"
if exist "%LOGDIR%" del /q "%LOGDIR%\*.log" >nul 2>&1
if not exist "%LOGDIR%" mkdir "%LOGDIR%"

rem Warnings and errors from everything, details from the WebKit parts that
rem draw video (sink -> frame converter -> player) and from caps negotiation.
set "GST_DEBUG=2,webkit*:5,webkitvideosink:6,webkitvideoframeconverter:6,GST_CAPS:3,GST_PERFORMANCE:4,d3d11*:3,basesink:4,decodebin3:4,playbin3:4"
set "GST_DEBUG_FILE=%LOGDIR%\gst-%%p.log"
set "GST_DEBUG_NO_COLOR=1"
rem WebKit-side trace of the video drawing path (GStreamer logs are compiled out in vcpkg Release).
set "LYRA_MEDIA_TRACE=%LOGDIR%\trace"
rem A minidump whenever a page's main thread stalls for 0.7 s or more (crash-report.bat reads them).
set "LYRA_STALL_DUMPS=C:\WebKit\crashdumps"
if not exist "%LYRA_STALL_DUMPS%" mkdir "%LYRA_STALL_DUMPS%"

echo [debug] Logs: %LOGDIR%
start "" "%~dp0run\Lyra.exe"
