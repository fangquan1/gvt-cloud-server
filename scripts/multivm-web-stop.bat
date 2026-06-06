@echo off
setlocal
cd /d "%~dp0.."
"%LOCALAPPDATA%\Python\pythoncore-3.14-64\python.exe" physical-output\multivm.py web-stop
