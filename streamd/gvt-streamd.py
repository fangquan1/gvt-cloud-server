#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import socket
import socketserver
import threading
import time
from dataclasses import dataclass


@dataclass
class StreamState:
    active_token: str | None = None
    active_client: str = ""
    active_port: int = 0
    active_codec: str = ""


class StreamSessionHandler(socketserver.StreamRequestHandler):
    server: "StreamServer"

    def handle(self) -> None:
        token = f"{time.monotonic_ns()}:{self.client_address[0]}:{self.client_address[1]}"
        self.request.settimeout(self.server.hello_timeout)
        try:
            raw = self.rfile.readline(self.server.max_line)
        except OSError as exc:
            self.server.log(f"client {self.client_address[0]} hello failed: {exc}")
            return
        if not raw:
            return

        try:
            payload = json.loads(raw.decode("utf-8", errors="replace"))
        except json.JSONDecodeError as exc:
            self.server.log(f"client {self.client_address[0]} bad json: {exc}")
            self._write({"ok": False, "error": "bad json"})
            return

        if payload.get("type") != "start":
            self._write({"ok": False, "error": "first message must be start"})
            return

        client_ip = self.client_address[0]
        video_port = self._port(payload.get("video_port") or payload.get("port"))
        codec = str(payload.get("codec") or self.server.default_codec).lower()
        if codec == "hevc":
            codec = "h265"
        if codec not in {"h264", "h265"}:
            self._write({"ok": False, "error": "codec must be h264 or h265"})
            return

        try:
            self.server.qemu_control({"type": "start", "host": client_ip, "port": video_port, "codec": codec})
        except OSError as exc:
            self.server.log(f"qemu control START failed: {exc}")
            self._write({"ok": False, "error": "qemu is not ready"})
            return

        with self.server.state_lock:
            self.server.state.active_token = token
            self.server.state.active_client = client_ip
            self.server.state.active_port = video_port
            self.server.state.active_codec = codec
        self.server.log(f"stream start {client_ip}:{video_port} codec={codec}")
        self._write({"ok": True})

        self.request.settimeout(None)
        try:
            while self.rfile.readline(self.server.max_line):
                pass
        except OSError:
            pass
        finally:
            self._stop_if_active(token)

    def _port(self, value: object) -> int:
        try:
            port = int(value)
        except (TypeError, ValueError):
            port = self.server.default_video_port
        if not 1 <= port <= 65535:
            port = self.server.default_video_port
        return port

    def _write(self, payload: dict[str, object]) -> None:
        self.wfile.write((json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8"))
        self.wfile.flush()

    def _stop_if_active(self, token: str) -> None:
        with self.server.state_lock:
            if self.server.state.active_token != token:
                return
            client = self.server.state.active_client
            port = self.server.state.active_port
            self.server.state = StreamState()
        try:
            self.server.qemu_control({"type": "stop"})
            self.server.log(f"stream stop {client}:{port}")
        except OSError as exc:
            self.server.log(f"qemu control STOP failed: {exc}")


class StreamServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(
        self,
        server_address: tuple[str, int],
        qemu_socket: str,
        default_video_port: int,
        default_codec: str,
    ) -> None:
        super().__init__(server_address, StreamSessionHandler)
        self.qemu_socket = qemu_socket
        self.default_video_port = default_video_port
        self.default_codec = default_codec
        self.hello_timeout = 8.0
        self.max_line = 4096
        self.state = StreamState()
        self.state_lock = threading.Lock()

    def qemu_control(self, payload: dict[str, object]) -> None:
        data = (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(3.0)
            sock.connect(self.qemu_socket)
            sock.sendall(data)

    def log(self, message: str) -> None:
        print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {message}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="GVT stream session daemon")
    parser.add_argument("--listen", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5004)
    parser.add_argument("--qemu-socket", default="/run/gvt-stream/qemu.sock")
    parser.add_argument("--video-port", type=int, default=5004)
    parser.add_argument("--codec", default="h265", choices=["h264", "h265"])
    args = parser.parse_args()

    server = StreamServer((args.listen, args.port), args.qemu_socket, args.video_port, args.codec)
    server.log(f"listening on {args.listen}:{args.port}, qemu_socket={args.qemu_socket}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.log("stopping")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
