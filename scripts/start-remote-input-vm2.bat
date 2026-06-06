@echo off
setlocal
cd /d "%~dp0\.."
set "PYEXE=%LOCALAPPDATA%\Python\pythoncore-3.14-64\python.exe"
if not exist "%PYEXE%" set "PYEXE=python"
call ".\physical-output\stop-remote-input.bat" >nul 2>nul
start "GVT Remote Input VM2" "%PYEXE%" ".\direct-stream\client\gvt_control_overlay.py" --host 192.168.0.188 --port 5906 --width 1024 --height 768 --alpha 0.18 --motion-interval-ms 8 --invert-case --debug-log ".\physical-output\remote-input-vm2.log"
