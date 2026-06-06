@echo off
setlocal
cd /d "%~dp0\.."
set "REMOTE_VIEWER=C:\Program Files\VirtViewer v11.0-256\bin\remote-viewer.exe"

if not exist "%REMOTE_VIEWER%" (
  echo remote-viewer not found: "%REMOTE_VIEWER%"
  exit /b 1
)

call ".\physical-output\stop-remote-audio.bat" >nul 2>nul
start "GVT Physical Remote Audio" /min "%REMOTE_VIEWER%" --title "GVT Physical Remote Audio" --spice-disable-usbredir spice://192.168.0.188:5900
