@echo off
setlocal
cd /d "%~dp0\.."
set "PYEXE=%LOCALAPPDATA%\Python\pythoncore-3.14-64\python.exe"
if not exist "%PYEXE%" set "PYEXE=python"

"%PYEXE%" ".\physical-output\start_vm_physical.py" stop
call ".\physical-output\stop-remote-audio.bat" >nul 2>nul
call ".\physical-output\stop-remote-input.bat" >nul 2>nul
