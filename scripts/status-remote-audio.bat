@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$procs = Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'remote-viewer.exe' -and $_.CommandLine -and ($_.CommandLine -like '*GVT Physical Remote Audio*' -or $_.CommandLine -like '*GVT SPICE Audio*' -or $_.CommandLine -like '*spice://192.168.0.188:5900*') };" ^
  "if (-not $procs) { Write-Host 'remote audio client: not running'; exit 1 };" ^
  "$procs | Select-Object ProcessId, CommandLine | Format-List"
