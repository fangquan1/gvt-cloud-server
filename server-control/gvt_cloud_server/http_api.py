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
                elif path == "/api/gvtg-profiles" and method == "GET":
                    self.send_json(200, {"profiles": service.gvt_profiles()})
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
