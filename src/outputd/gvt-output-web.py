#!/usr/bin/env python3
from __future__ import annotations

import argparse
import html
import hmac
import http.cookies
import json
import os
import secrets
import socket
import struct
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


MAGIC = 0x4756544F
VERSION = 1
MSG_SELECT = 2
SOURCE_LEN = 32

INDEX_HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>GVT Output Switch</title>
<style>
:root { color-scheme: dark; font-family: "Segoe UI", Arial, sans-serif; }
body { margin: 0; background: #101214; color: #e8ecef; }
main { max-width: 860px; margin: 0 auto; padding: 28px 20px 40px; }
header { display: flex; align-items: center; justify-content: space-between; gap: 16px; margin-bottom: 20px; }
h1 { font-size: 26px; font-weight: 650; margin: 0; letter-spacing: 0; }
.muted { color: #9aa4ad; font-size: 14px; }
.panel { border: 1px solid #2b3137; border-radius: 8px; background: #171a1d; padding: 18px; margin-top: 14px; }
.row { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; }
.metric { min-width: 136px; padding: 12px; border-radius: 6px; background: #20252a; }
.label { display: block; color: #9aa4ad; font-size: 12px; margin-bottom: 6px; }
.value { font-size: 20px; font-weight: 650; overflow-wrap: anywhere; }
button { border: 1px solid #3f474f; background: #252b31; color: #f2f5f7; border-radius: 6px; padding: 11px 16px; font-size: 15px; cursor: pointer; }
button:hover { background: #303841; }
button.active { border-color: #71c7a3; background: #18342b; }
button:disabled { opacity: .55; cursor: wait; }
table { width: 100%; border-collapse: collapse; margin-top: 10px; }
th, td { text-align: left; border-bottom: 1px solid #2b3137; padding: 10px 8px; }
th { color: #9aa4ad; font-size: 12px; font-weight: 600; }
.error { color: #ffb4a8; }
input { width: 100%; box-sizing: border-box; border: 1px solid #3f474f; background: #111417; color: #f2f5f7; border-radius: 6px; padding: 12px; font-size: 16px; }
form { max-width: 360px; }
</style>
</head>
<body>
<main>
<header>
  <div>
    <h1>GVT Output Switch</h1>
    <div class="muted">Atomic KMS source control</div>
  </div>
  <button id="logout" type="button">Logout</button>
</header>
<section class="panel">
  <div class="row">
    <div class="metric"><span class="label">Active</span><span class="value" id="active">-</span></div>
    <div class="metric"><span class="label">Connector</span><span class="value" id="connector">-</span></div>
    <div class="metric"><span class="label">Mode</span><span class="value" id="mode">-</span></div>
    <div class="metric"><span class="label">Failed</span><span class="value" id="failed">-</span></div>
  </div>
  <div class="row" style="margin-top:16px">
    <button id="vm1" type="button" onclick="selectSource('vm1')">VM1</button>
    <button id="vm2" type="button" onclick="selectSource('vm2')">VM2</button>
  </div>
  <p class="muted" id="message"></p>
</section>
<section class="panel">
  <div class="muted">Sources</div>
  <table>
    <thead><tr><th>Name</th><th>Frames</th><th>Framebuffer</th><th>Size</th></tr></thead>
    <tbody id="sources"></tbody>
  </table>
</section>
</main>
<script>
async function api(path, options) {
  const res = await fetch(path, options || {});
  if (res.status === 401) location.href = "/login";
  if (!res.ok) throw new Error(await res.text());
  return await res.json();
}
function setText(id, value) {
  document.getElementById(id).textContent = value || "-";
}
async function refresh() {
  try {
    const data = await api("/api/status");
    setText("active", data.active);
    setText("connector", data.connector);
    setText("mode", data.mode);
    setText("failed", String(data.failed ?? "-"));
    for (const id of ["vm1", "vm2"]) {
      document.getElementById(id).classList.toggle("active", data.active === id);
    }
    const rows = (data.sources || []).map(s =>
      `<tr><td>${s.name}</td><td>${s.frames}</td><td>${s.fb}</td><td>${s.width}x${s.height}</td></tr>`);
    document.getElementById("sources").innerHTML = rows.join("") || "<tr><td colspan='4'>No sources yet</td></tr>";
    document.getElementById("message").className = "muted";
    document.getElementById("message").textContent = `received=${data.received ?? 0} presented=${data.presented ?? 0}`;
  } catch (err) {
    document.getElementById("message").className = "error";
    document.getElementById("message").textContent = String(err.message || err);
  }
}
async function selectSource(source) {
  for (const id of ["vm1", "vm2"]) document.getElementById(id).disabled = true;
  try {
    await api("/api/select", {method: "POST", headers: {"Content-Type": "application/x-www-form-urlencoded"}, body: `source=${encodeURIComponent(source)}`});
    await refresh();
  } finally {
    for (const id of ["vm1", "vm2"]) document.getElementById(id).disabled = false;
  }
}
document.getElementById("logout").onclick = async () => {
  await fetch("/logout", {method: "POST"});
  location.href = "/login";
};
refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
"""

LOGIN_HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Login</title>
<style>
body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #101214; color: #e8ecef; font-family: "Segoe UI", Arial, sans-serif; }
section { width: min(360px, calc(100vw - 40px)); }
h1 { font-size: 24px; margin: 0 0 16px; letter-spacing: 0; }
input, button { width: 100%; box-sizing: border-box; border-radius: 6px; padding: 12px; font-size: 16px; }
input { border: 1px solid #3f474f; background: #111417; color: #f2f5f7; margin-bottom: 12px; }
button { border: 1px solid #3f474f; background: #252b31; color: #f2f5f7; cursor: pointer; }
p { color: #ffb4a8; min-height: 20px; }
</style>
</head>
<body>
<section>
<h1>GVT Output Switch</h1>
<form method="post" action="/login">
<input type="password" name="password" placeholder="Password" autofocus>
<button type="submit">Login</button>
</form>
<p>__ERROR__</p>
</section>
</body>
</html>
"""


def load_config(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.exists():
        return out
    for line in path.read_text(errors="ignore").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        out[key.strip()] = value.strip().strip("\"'")
    return out


class App:
    def __init__(self, args: argparse.Namespace) -> None:
        self.socket_path = args.socket
        self.status_path = Path(args.status)
        self.sessions: set[str] = set()
        config = load_config(Path(args.config))
        self.password = args.password or os.environ.get("GVT_WEB_PASSWORD") or config.get("WEB_PASSWORD") or ""
        if not self.password:
            raise SystemExit("missing web password")

    def authed(self, handler: BaseHTTPRequestHandler) -> bool:
        raw = handler.headers.get("Cookie", "")
        cookie = http.cookies.SimpleCookie(raw)
        value = cookie.get("gvt_session")
        return bool(value and value.value in self.sessions)

    def login(self, password: str) -> str | None:
        if not hmac.compare_digest(password, self.password):
            return None
        session = secrets.token_urlsafe(24)
        self.sessions.add(session)
        return session

    def logout(self, handler: BaseHTTPRequestHandler) -> None:
        raw = handler.headers.get("Cookie", "")
        cookie = http.cookies.SimpleCookie(raw)
        value = cookie.get("gvt_session")
        if value:
            self.sessions.discard(value.value)

    def status(self) -> dict:
        if not self.status_path.exists():
            return {"active": "", "received": 0, "presented": 0, "failed": 0, "sources": []}
        try:
            return json.loads(self.status_path.read_text())
        except Exception as exc:
            return {"active": "", "error": str(exc), "sources": []}

    def select(self, source: str) -> None:
        source_bytes = source.encode("ascii", "ignore")[: SOURCE_LEN - 1]
        source_bytes += b"\0" * (SOURCE_LEN - len(source_bytes))
        msg = struct.pack("<IIIIIIIIQQ32s", MAGIC, VERSION, MSG_SELECT,
                          0, 0, 0, 0, 0, 0, 0, source_bytes)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            sock.sendto(msg, self.socket_path)
        finally:
            sock.close()


def make_handler(app: App):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args) -> None:
            print("%s %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), fmt % args), flush=True)

        def send_text(self, code: int, body: str, content_type: str = "text/plain") -> None:
            data = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def send_head_only(self, code: int = 200) -> None:
            self.send_response(code)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()

        def send_json(self, obj: object) -> None:
            data = json.dumps(obj).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def redirect(self, target: str) -> None:
            self.send_response(302)
            self.send_header("Location", target)
            self.end_headers()

        def read_form(self) -> dict[str, list[str]]:
            length = int(self.headers.get("Content-Length", "0"))
            data = self.rfile.read(length).decode()
            return urllib.parse.parse_qs(data)

        def require_auth(self) -> bool:
            if app.authed(self):
                return True
            self.send_text(401, "unauthorized")
            return False

        def do_GET(self) -> None:
            if self.path == "/login":
                self.send_text(200, LOGIN_HTML.replace("__ERROR__", ""),
                               "text/html; charset=utf-8")
            elif self.path == "/api/status":
                if self.require_auth():
                    self.send_json(app.status())
            elif self.path == "/":
                if app.authed(self):
                    self.send_text(200, INDEX_HTML, "text/html; charset=utf-8")
                else:
                    self.redirect("/login")
            else:
                self.send_text(404, "not found")

        def do_HEAD(self) -> None:
            if self.path in {"/", "/login"}:
                self.send_head_only(200)
            else:
                self.send_head_only(404)

        def do_POST(self) -> None:
            if self.path == "/login":
                form = self.read_form()
                session = app.login(form.get("password", [""])[0])
                if not session:
                    body = LOGIN_HTML.replace("__ERROR__",
                                              html.escape("Login failed"))
                    self.send_text(403, body, "text/html; charset=utf-8")
                    return
                self.send_response(302)
                self.send_header("Location", "/")
                self.send_header("Set-Cookie", f"gvt_session={session}; Path=/; HttpOnly; SameSite=Lax")
                self.end_headers()
            elif self.path == "/logout":
                app.logout(self)
                self.send_response(204)
                self.send_header("Set-Cookie", "gvt_session=; Path=/; Max-Age=0")
                self.end_headers()
            elif self.path == "/api/select":
                if not self.require_auth():
                    return
                form = self.read_form()
                source = form.get("source", [""])[0]
                if source not in {"vm1", "vm2"}:
                    self.send_text(400, "bad source")
                    return
                app.select(source)
                self.send_json({"ok": True, "source": source})
            else:
                self.send_text(404, "not found")

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8088)
    parser.add_argument("--socket", default="/run/gvt-outputd.sock")
    parser.add_argument("--status", default="/run/gvt-outputd.status")
    parser.add_argument("--config", default="/root/qemu_cmd/multivm/config.env")
    parser.add_argument("--password")
    args = parser.parse_args()

    app = App(args)
    ThreadingHTTPServer.allow_reuse_address = True
    server = ThreadingHTTPServer((args.host, args.port), make_handler(app))
    print(f"gvt-output-web: listening http://{args.host}:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
