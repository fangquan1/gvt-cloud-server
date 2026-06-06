@echo off
setlocal
cd /d "%~dp0\.."
set "PYEXE=%LOCALAPPDATA%\Python\pythoncore-3.14-64\python.exe"
if not exist "%PYEXE%" set "PYEXE=python"

"%PYEXE%" ".\physical-output\start_vm_physical.py" start --restart --connector DP-1
start "GVT Physical Remote Input" "%PYEXE%" ".\direct-stream\client\gvt_control_overlay.py" --host 192.168.0.188 --port 5905 --width 960 --height 600 --alpha 0.18 --motion-interval-ms 8 --debug-log ".\physical-output\remote-input.log"
call ".\physical-output\start-remote-audio.bat"
