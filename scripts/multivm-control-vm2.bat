@echo off
setlocal
cd /d "%~dp0\.."
set "PYEXE=%LOCALAPPDATA%\Python\pythoncore-3.14-64\python.exe"
if not exist "%PYEXE%" set "PYEXE=python"
"%PYEXE%" physical-output\multivm.py select vm2
call ".\physical-output\start-remote-input-vm2.bat"
