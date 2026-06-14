#!/usr/bin/env bash
set -euo pipefail

QMP_SOCK=${GVT_STREAM_QMP_SOCK:-/root/qemu_cmd/win10-gvt-stream-qmp.sock}

python3 - "$QMP_SOCK" <<'PY'
import json
import os
import socket
import sys

sock_path = sys.argv[1]
if not os.path.exists(sock_path):
    raise SystemExit(0)

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(3)
sock.connect(sock_path)
try:
    sock.recv(4096)
except Exception:
    pass
sock.sendall((json.dumps({"execute": "qmp_capabilities", "id": 1}) + "\r\n").encode())
try:
    sock.recv(4096)
except Exception:
    pass
sock.sendall((json.dumps({"execute": "quit", "id": 2}) + "\r\n").encode())
sock.close()
PY

sleep 4
pid=$(ps -eo pid=,cmd= | awk '/[q]emu-system-x86_64/ && /gvt-stream/ {print $1; exit}')
if [ -n "${pid:-}" ]; then
  kill "$pid" 2>/dev/null || true
  sleep 3
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null || true
  fi
fi

echo "gvt-stream QEMU stopped"
