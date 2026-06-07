from __future__ import annotations

import argparse
import http.cookies
import json
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from .config import load_config
from .service import ApiError, GvtCloudService


def _json_bytes(payload: object) -> bytes:
    return json.dumps(payload, ensure_ascii=False).encode("utf-8")


def make_handler(service: GvtCloudService):
    class Handler(BaseHTTPRequestHandler):
        server_version = "GvtCloudServer/0.1"

        def log_message(self, fmt: str, *args: object) -> None:
            print("%s %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), fmt % args), flush=True)

        def send_json(self, status: int, payload: object, token: str | None = None) -> None:
            data = _json_bytes(payload)
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            if token:
                self.send_header("Set-Cookie", f"gvt_session={token}; Path=/; HttpOnly; SameSite=Lax")
            self.end_headers()
            self.wfile.write(data)

        def send_error_json(self, status: int, message: str) -> None:
            self.send_json(status, {"ok": False, "error": message})

        def read_body(self) -> dict[str, Any]:
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length) if length else b""
            if not raw:
                return {}
            content_type = self.headers.get("Content-Type", "")
            if "application/json" in content_type:
                data = json.loads(raw.decode("utf-8"))
                return data if isinstance(data, dict) else {}
            parsed = urllib.parse.parse_qs(raw.decode("utf-8"))
            return {key: values[-1] for key, values in parsed.items()}

        def handle_upload(self) -> None:
            content_type = self.headers.get("Content-Type", "")
            if "multipart/form-data" not in content_type:
                self.send_error_json(400, "expected multipart upload")
                return
            _, params = content_type.split(";", 1)
            boundary = ""
            for part in params.split(";"):
                key, _, value = part.strip().partition("=")
                if key.lower() == "boundary":
                    boundary = value.strip().strip('"')
            if not boundary:
                self.send_error_json(400, "missing multipart boundary")
                return
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0:
                self.send_error_json(400, "missing file")
                return
            if length > 20 * 1024 * 1024 * 1024:
                self.send_error_json(413, "upload too large")
                return
            kind = ""
            boundary_line = ("--" + boundary).encode("ascii", "ignore")
            file_boundary = b"\r\n" + boundary_line
            remaining = length

            def read_line() -> bytes:
                nonlocal remaining
                line = self.rfile.readline()
                remaining -= len(line)
                return line

            def read_chunk(max_size: int) -> bytes:
                nonlocal remaining
                if remaining <= 0:
                    return b""
                chunk = self.rfile.read(min(max_size, remaining))
                remaining -= len(chunk)
                return chunk

            first = read_line()
            if not first.startswith(boundary_line):
                self.send_error_json(400, "invalid multipart body")
                return
            while True:
                headers: dict[str, str] = {}
                while True:
                    line = read_line()
                    if line in {b"\r\n", b"\n", b""}:
                        break
                    key, _, value = line.decode("utf-8", errors="replace").partition(":")
                    headers[key.lower()] = value.strip()
                disposition = headers.get("content-disposition", "")
                disp_params: dict[str, str] = {}
                for item in disposition.split(";"):
                    key, _, value = item.strip().partition("=")
                    if key:
                        disp_params[key.lower()] = value.strip().strip('"')
                name = disp_params.get("name", "")
                if name == "kind":
                    data = b""
                    while True:
                        line = read_line()
                        if line.startswith(boundary_line):
                            break
                        data += line
                    kind = data.decode("utf-8", errors="replace").strip()
                    if line.strip().endswith(b"--"):
                        break
                elif name == "file":
                    filename = disp_params.get("filename", "")
                    if not filename:
                        self.send_error_json(400, "missing file")
                        return
                    target = service.upload_target_path(kind, filename)
                    with target.open("wb") as handle:
                        buffer = b""
                        keep = len(file_boundary) + 8
                        while True:
                            chunk = read_chunk(1024 * 1024)
                            if not chunk:
                                target.unlink(missing_ok=True)
                                self.send_error_json(400, "unterminated multipart file")
                                return
                            buffer += chunk
                            index = buffer.find(file_boundary)
                            if index >= 0:
                                handle.write(buffer[:index])
                                break
                            if len(buffer) > keep:
                                handle.write(buffer[:-keep])
                                buffer = buffer[-keep:]
                    self.send_json(200, {"ok": True, "path": str(target), "kind": kind, "filename": target.name})
                    return
                else:
                    while True:
                        line = read_line()
                        if line.startswith(boundary_line) or not line:
                            break
                    if line.strip().endswith(b"--"):
                        break
            self.send_error_json(400, "missing file")

        def token(self) -> str | None:
            auth = self.headers.get("Authorization", "")
            if auth.startswith("Bearer "):
                return auth.removeprefix("Bearer ").strip()
            cookie = http.cookies.SimpleCookie(self.headers.get("Cookie", ""))
            value = cookie.get("gvt_session")
            return value.value if value else None

        def require_auth(self) -> bool:
            if service.check_token(self.token()):
                return True
            self.send_error_json(401, "unauthorized")
            return False

        def handle_api(self, method: str) -> None:
            try:
                path = urllib.parse.urlparse(self.path).path
                parts = [part for part in path.split("/") if part]
                if path == "/healthz" and method == "GET":
                    self.send_json(200, {"ok": True})
                    return
                if path == "/api/login" and method == "POST":
                    body = self.read_body()
                    result = service.login(str(body.get("password", "")))
                    self.send_json(200, {"ok": True, **result}, token=result["token"])
                    return
                if not self.require_auth():
                    return
                if path == "/api/logout" and method == "POST":
                    service.logout(self.token())
                    self.send_json(200, {"ok": True})
                elif path == "/api/setup" and method == "POST":
                    self.send_json(200, service.setup())
                elif path == "/api/status" and method == "GET":
                    self.send_json(200, service.status())
                elif path == "/api/desktops" and method == "GET":
                    self.send_json(200, {"desktops": service.desktops()})
                elif path == "/api/desktops" and method == "POST":
                    self.send_json(200, service.create_desktop(self.read_body()))
                elif path == "/api/gvtg-profiles" and method == "GET":
                    self.send_json(200, {"profiles": service.gvt_profiles()})
                elif path == "/api/uploads" and method == "POST":
                    self.handle_upload()
                elif len(parts) == 3 and parts[:2] == ["api", "desktops"] and method == "GET":
                    self.send_json(200, service.desktop(parts[2]))
                elif len(parts) == 4 and parts[:2] == ["api", "desktops"] and method == "POST":
                    desktop_id, action = parts[2], parts[3]
                    body = self.read_body()
                    body.setdefault("client_host", self.client_address[0])
                    if action == "start":
                        self.send_json(200, service.start_desktop(desktop_id, body))
                    elif action == "stop":
                        self.send_json(200, service.stop_desktop(desktop_id))
                    elif action == "restart":
                        self.send_json(200, service.restart_desktop(desktop_id, body))
                    elif action == "mode":
                        self.send_json(200, service.set_desktop_mode(desktop_id, str(body.get("mode", ""))))
                    elif action == "profile":
                        self.send_json(200, service.set_desktop_profile(desktop_id, str(body.get("profile", ""))))
                    elif action == "resources":
                        self.send_json(200, service.set_desktop_resources(desktop_id, body))
                    elif action == "iso":
                        self.send_json(200, service.set_desktop_iso(desktop_id, body))
                    elif action == "delete":
                        self.send_json(200, service.delete_desktop(desktop_id, body))
                    else:
                        self.send_error_json(404, "not found")
                elif path == "/api/output/select" and method == "POST":
                    body = self.read_body()
                    source = str(body.get("source") or body.get("desktop_id") or "")
                    self.send_json(200, service.select_output(source))
                elif path == "/api/input/select" and method == "POST":
                    body = self.read_body()
                    source = str(body.get("source") or body.get("desktop_id") or "")
                    self.send_json(200, service.select_input(source))
                elif path == "/api/audio/select" and method == "POST":
                    body = self.read_body()
                    source = str(body.get("source") or body.get("desktop_id") or "")
                    self.send_json(200, service.select_audio(source))
                elif len(parts) == 3 and parts[:2] == ["api", "logs"] and method == "GET":
                    self.send_json(200, service.logs(parts[2]))
                else:
                    self.send_error_json(404, "not found")
            except ApiError as exc:
                self.send_error_json(exc.status, exc.message)
            except json.JSONDecodeError:
                self.send_error_json(400, "invalid json")
            except Exception as exc:
                self.send_error_json(500, str(exc))

        def do_GET(self) -> None:
            self.handle_api("GET")

        def do_POST(self) -> None:
            self.handle_api("POST")

        def do_OPTIONS(self) -> None:
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.end_headers()

    return Handler


def run_server(service: GvtCloudService, host: str, port: int) -> ThreadingHTTPServer:
    ThreadingHTTPServer.allow_reuse_address = True
    httpd = ThreadingHTTPServer((host, port), make_handler(service))
    print(f"gvt-cloud-server: listening http://{host}:{port}", flush=True)
    httpd.serve_forever()
    return httpd


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="/etc/gvt-cloud-server/config.json")
    parser.add_argument("--host")
    parser.add_argument("--port", type=int)
    args = parser.parse_args()
    config = load_config(args.config)
    service = GvtCloudService(config)
    service.require_password()
    run_server(service, args.host or config.bind_host, args.port or config.bind_port)


if __name__ == "__main__":
    main()
