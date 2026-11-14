@echo off
rem ==========================================================================
rem  Publishes the Lyra version written in CMakeLists.txt on GitHub
rem  (installer + LGPL sources, engine branch to the lyra-webkit fork).
rem    release.bat           publish
rem    release.bat dry       build everything, publish nothing
rem  Details: tools\release.py
rem ==========================================================================
setlocal
cd /d "%~dp0"
set "PY=python"
where python >nul 2>&1 || set "PY=%ProgramFiles%\Python311\python.exe"
if /i "%~1"=="dry" ("%PY%" tools\release.py --dry-run) else ("%PY%" tools\release.py)
if errorlevel 1 (
    echo.
    echo [release] FAILED - see the messages above.
)
pause
