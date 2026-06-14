from __future__ import annotations

import argparse
import json
import os
import socket
import time
from pathlib import Path

import paramiko


ROOT = Path(__file__).resolve().parents[1]
QMP = "/root/qemu_cmd/win10-gvt-stream-qmp.sock"
RUN_SCRIPT = "/root/qemu_cmd/run-win10-gvt-stream-diag.sh"
LOG = "/root/qemu_cmd/win10-gvt-stream-diag.log"


def read_connection() -> tuple[str, str, str]:
    host = os.environ.get("GVT_SERVER_HOST", "").strip()
    user = os.environ.get("GVT_SERVER_USER", "root").strip()
    password = os.environ.get("GVT_SERVER_PASSWORD", "")
    if not host or not password:
        raise SystemExit(
            "Set GVT_SERVER_HOST, GVT_SERVER_USER and GVT_SERVER_PASSWORD "
            "before running this helper."
        )
    return host, user, password


def local_ip_for(remote_host: str) -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect((remote_host, 22))
        return sock.getsockname()[0]
    finally:
        sock.close()


def connect() -> tuple[paramiko.SSHClient, str]:
    host, user, password = read_connection()
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        hostname=host,
        username=user,
        password=password,
        look_for_keys=False,
        allow_agent=False,
        timeout=8,
        banner_timeout=8,
        auth_timeout=8,
    )
    return client, host


def run(client: paramiko.SSHClient, command: str, timeout: int = 25) -> str:
    _, stdout, stderr = client.exec_command(command, timeout=timeout)
    out = stdout.read().decode("utf-8", errors="replace")
    err = stderr.read().decode("utf-8", errors="replace")
    return out + (("\n[stderr]\n" + err) if err.strip() else "")


def qmp_quit(client: paramiko.SSHClient) -> None:
    command = f"""
python3 - <<'PY'
import json
import os
import socket

sock_path = {QMP!r}
if not os.path.exists(sock_path):
    raise SystemExit(0)

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(3)
sock.connect(sock_path)
try:
    sock.recv(4096)
except Exception:
    pass
sock.sendall((json.dumps({{"execute": "qmp_capabilities", "id": 1}}) + "\\r\\n").encode())
try:
    sock.recv(4096)
except Exception:
    pass
sock.sendall((json.dumps({{"execute": "quit", "id": 2}}) + "\\r\\n").encode())
sock.close()
PY
sleep 4
pid=$(ps -eo pid=,cmd= | awk '/[q]emu-system-x86_64/ && /gvt-stream/ {{print $1; exit}}')
if [ -n "${{pid:-}}" ]; then
  kill "$pid" 2>/dev/null || true
  sleep 3
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null || true
  fi
fi
"""
    run(client, command, timeout=10)


def start(args: argparse.Namespace) -> None:
    client, remote_host = connect()
    try:
        client_host = args.client_host or local_ip_for(remote_host)
        run_script = args.run_script or RUN_SCRIPT
        if args.restart:
            qmp_quit(client)
            time.sleep(4)

        keyint = args.keyint or args.fps
        command = f"""
set -euo pipefail
unset GVT_STREAM_CAPTURE_DIR
unset GVT_STREAM_ENCODE_FILE
export GVT_STREAM_IMPORT_TEST={1 if args.import_test else 0}
export GVT_STREAM_ENCODE_PATH={args.path}
export GVT_STREAM_ENCODE_FLIP={1 if args.flip else 0}
export GVT_STREAM_DMABUF_CAPS_FEATURE={1 if args.dmabuf_caps_feature else 0}
export GVT_STREAM_CAPTURE_MS={args.capture_ms}
export GVT_STREAM_IDLE_CAPTURE_MS={args.idle_capture_ms}
export GVT_STREAM_IDLE_AFTER_MS={args.idle_after_ms}
export GVT_STREAM_IDLE_PROBE_MS={args.idle_probe_ms}
export GVT_STREAM_IDLE_CHANGED_PPM={args.idle_changed_ppm}
export GVT_STREAM_IDLE_PIXEL_DELTA={args.idle_pixel_delta}
export GVT_STREAM_CAPTURE_MAX=0
export GVT_STREAM_RTP_HOST={client_host}
export GVT_STREAM_RTP_PORT={args.port}
export GVT_STREAM_RTP_FEC={args.fec}
export GVT_STREAM_RTP_FEC_IMPORTANT={args.fec_important}
export GVT_STREAM_INPUT_HOST=0.0.0.0
export GVT_STREAM_INPUT_PORT={args.input_port}
export GVT_STREAM_ENCODE_MAX=0
export GVT_STREAM_ENCODE_FPS={args.fps}
export GVT_STREAM_ENCODE_BITRATE={args.bitrate}
export GVT_STREAM_ENCODE_KEYINT={keyint}
export GVT_STREAM_VIDEO_CODEC={args.video_codec}
export GVT_AUDIO_RTP_HOST={client_host}
export GVT_AUDIO_RTP_PORT={args.audio_port}
export GVT_AUDIO_RTP_BITRATE={args.audio_bitrate}
{run_script}
"""
        print(run(client, command, timeout=35))
        print(f"gvt-stream RTP target: {client_host}:{args.port}")
        if "audio-rtp" in run_script or args.audio_rtp:
            print(f"gvt-audio RTP target: {client_host}:{args.audio_port}")
    finally:
        client.close()


def stop(_: argparse.Namespace) -> None:
    client, _ = connect()
    try:
        qmp_quit(client)
        print("gvt-stream QEMU quit requested")
    finally:
        client.close()


def status(_: argparse.Namespace) -> None:
    client, _ = connect()
    try:
        command = f"""
ps -eo pid,etime,%cpu,%mem,cmd | awk '/[q]emu-system-x86_64/ && /gvt-stream/ {{print}}'
echo --- audio rtp ---
grep -nE 'gvt-audio-rtp: (start|stats|drop|fail|error)' {LOG} | tail -30 || true
echo --- video ---
grep -nE 'gvt-stream: (listener|scanout-dmabuf|encode-start|encode-push-ok|update-stats)|failed|error|timeout' {LOG} | tail -60 || true
"""
        print(run(client, command, timeout=12))
    finally:
        client.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("start")
    p.add_argument("--client-host")
    p.add_argument("--port", type=int, default=5004)
    p.add_argument("--fps", type=int, default=60)
    p.add_argument("--bitrate", type=int, default=18000)
    p.add_argument("--video-codec", choices=["h264", "h265"], default="h264")
    p.add_argument("--keyint", type=int)
    p.add_argument("--path", choices=["dmabuf", "cpu"], default="dmabuf")
    p.add_argument("--flip", action="store_true")
    p.add_argument("--import-test", action="store_true")
    p.add_argument("--dmabuf-caps-feature", action="store_true")
    p.add_argument("--capture-ms", type=int, default=16)
    p.add_argument("--idle-capture-ms", type=int, default=16)
    p.add_argument("--idle-after-ms", type=int, default=0)
    p.add_argument("--idle-probe-ms", type=int, default=0)
    p.add_argument("--idle-changed-ppm", type=int, default=3000)
    p.add_argument("--idle-pixel-delta", type=int, default=8)
    p.add_argument("--fec", type=int, default=0)
    p.add_argument("--fec-important", type=int, default=0)
    p.add_argument("--input-port", type=int, default=5905)
    p.add_argument("--run-script", default=RUN_SCRIPT)
    p.add_argument("--audio-rtp", action="store_true")
    p.add_argument("--audio-port", type=int, default=5006)
    p.add_argument("--audio-bitrate", type=int, default=96000)
    p.add_argument("--restart", action="store_true")
    p.set_defaults(func=start)

    p = sub.add_parser("stop")
    p.set_defaults(func=stop)

    p = sub.add_parser("status")
    p.set_defaults(func=status)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
