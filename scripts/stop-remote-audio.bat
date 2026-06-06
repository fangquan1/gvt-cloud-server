@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$patterns = @('GVT Physical Remote Audio', 'GVT SPICE Audio', 'spice://192.168.0.188:5900');" ^
  "Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'remote-viewer.exe' -and $_.CommandLine } | ForEach-Object {" ^
  "  $proc = $_;" ^
  "  foreach ($pattern in $patterns) {" ^
  "    if ($proc.CommandLine -like ('*' + $pattern + '*')) {" ^
  "      try { Stop-Process -Id $proc.ProcessId -Force -ErrorAction Stop } catch {};" ^
  "      break;" ^
  "    }" ^
  "  }" ^
  "}"
