from __future__ import annotations

import json
import os
import platform
import re
import secrets
import time
from pathlib import Path
from typing import Any

from . import __version__
from .config import DesktopConfig, ServerConfig, VALID_MODES
from .runtime import CommandResult, ControlRuntime


STAT_FIELDS = {
    "fps": re.compile(r"\bfps=([0-9.]+)"),
    "capture_ms": re.compile(r"\bcapture_ms=([0-9]+)"),
    "encoded": re.compile(r"\bencoded=([0-9]+)"),
    "encode_failures": re.compile(r"\bencode_failures=([0-9]+)"),
}


class ApiError(Exception):
    def __init__(self, status: int, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.message = message


class GvtCloudService:
    def __init__(self, config: ServerConfig, runtime: ControlRuntime | None = None) -> None:
        self.config = config
        self.runtime = runtime or ControlRuntime(config)
        self.sessions: dict[str, float] = {}
        self._state = self._load_state()

    def require_password(self) -> None:
        if not self.config.auth_password():
            raise ApiError(500, "server password is not configured")

    def login(self, password: str) -> dict[str, Any]:
        self.require_password()
        if not secrets.compare_digest(password, self.config.auth_password()):
            raise ApiError(401, "invalid credentials")
        token = secrets.token_urlsafe(32)
        expires_at = time.time() + self.config.auth.token_ttl_seconds
        self.sessions[token] = expires_at
        return {"token": token, "expires_at": int(expires_at)}

    def check_token(self, token: str | None) -> bool:
        if not token:
            return False
        expires_at = self.sessions.get(token)
        if not expires_at:
            return False
        if expires_at < time.time():
            self.sessions.pop(token, None)
            return False
        return True

    def logout(self, token: str | None) -> None:
        if token:
            self.sessions.pop(token, None)

    def status(self) -> dict[str, Any]:
        outputd = self._outputd_summary()
        return {
            "version": __version__,
            "host": self._host_summary(),
            "outputd": outputd,
            "active_source": outputd.get("active") or self._state.get("output_source", ""),
            "input_source": self._state.get("input_source", ""),
            "audio_source": self._state.get("audio_source", ""),
            "desktops": self.desktops(),
        }

    def desktops(self) -> list[dict[str, Any]]:
        return [self.desktop(item.id) for item in self.config.desktops]

    def desktop(self, desktop_id: str) -> dict[str, Any]:
        desktop = self._desktop_config(desktop_id)
        pid, running = self.runtime.pid_status(desktop)
        mode = self._state.get("modes", {}).get(desktop.id, desktop.mode)
        return {
            "id": desktop.id,
            "name": desktop.name,
            "state": "running" if running else "stopped",
            "pid": pid,
            "mode": mode,
            "ports": {
                "spice": desktop.spice_port,
                "input": desktop.input_port,
                "video": desktop.video_port,
                "audio": desktop.audio_port,
            },
            "resolution": desktop.resolution,
            "overlay": desktop.overlay,
            "tap": desktop.tap,
            "mac": desktop.mac,
            "gvt_stream": self._stream_summary(desktop),
        }

    def setup(self) -> dict[str, Any]:
        return self._run("setup")

    def start_desktop(self, desktop_id: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        desktop = self._desktop_config(desktop_id)
        payload = payload or {}
        mode = str(payload.get("mode") or self._state.get("modes", {}).get(desktop.id, desktop.mode))
        if mode not in VALID_MODES:
            raise ApiError(400, "invalid mode")
        self._set_mode_state(desktop.id, mode)
        return self._run("desktop_start", id=desktop.id, mode=mode)

    def stop_desktop(self, desktop_id: str) -> dict[str, Any]:
        desktop = self._desktop_config(desktop_id)
        return self._run("desktop_stop", id=desktop.id)

    def restart_desktop(self, desktop_id: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        self._desktop_config(desktop_id)
        self.stop_desktop(desktop_id)
        return self.start_desktop(desktop_id, payload)

    def set_desktop_mode(self, desktop_id: str, mode: str) -> dict[str, Any]:
        desktop = self._desktop_config(desktop_id)
        if mode not in VALID_MODES:
            raise ApiError(400, "invalid mode")
        self._set_mode_state(desktop.id, mode)
        result = self._run("desktop_mode", id=desktop.id, mode=mode)
        result["mode"] = mode
        return result

    def select_output(self, source: str) -> dict[str, Any]:
        self._desktop_config(source)
        self._state["output_source"] = source
        self._save_state()
        result = self._run("output_select", source=source)
        if result["ok"] and not self.config.commands.get("output_select"):
            self.runtime.select_output(source)
        result["source"] = source
        return result

    def select_input(self, source: str) -> dict[str, Any]:
        self._desktop_config(source)
        self._state["input_source"] = source
        self._save_state()
        result = self._run("input_select", source=source)
        result["source"] = source
        return result

    def select_audio(self, source: str) -> dict[str, Any]:
        self._desktop_config(source)
        self._state["audio_source"] = source
        self._save_state()
        result = self._run("audio_select", source=source)
        result["source"] = source
        return result

    def logs(self, desktop_id: str) -> dict[str, Any]:
        desktop = self._desktop_config(desktop_id)
        return {"id": desktop.id, "log": self.runtime.safe_log(desktop.log_file)}

    def _run(self, name: str, **values: object) -> dict[str, Any]:
        result = self.runtime.command(name, **values)
        response = self._command_response(result)
        if not result.ok:
            raise ApiError(500, response.get("stderr") or response.get("stdout") or "command failed")
        return response

    def _command_response(self, result: CommandResult) -> dict[str, Any]:
        return {
            "ok": result.ok,
            "returncode": result.returncode,
            "command": result.command,
            "stdout": result.stdout[-4000:],
            "stderr": result.stderr[-4000:],
        }

    def _desktop_config(self, desktop_id: str) -> DesktopConfig:
        for desktop in self.config.desktops:
            if desktop.id == desktop_id:
                return desktop
        raise ApiError(404, "desktop not found")

    def _load_state(self) -> dict[str, Any]:
        path = Path(self.config.runtime.state_file)
        if not path.exists():
            return {"modes": {}}
        try:
            data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
            return data if isinstance(data, dict) else {"modes": {}}
        except Exception:
            return {"modes": {}}

    def _save_state(self) -> None:
        path = Path(self.config.runtime.state_file)
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(path.suffix + ".tmp")
        tmp.write_text(json.dumps(self._state, indent=2, sort_keys=True), encoding="utf-8")
        tmp.replace(path)

    def _set_mode_state(self, desktop_id: str, mode: str) -> None:
        modes = self._state.setdefault("modes", {})
        modes[desktop_id] = mode
        self._save_state()

    def _host_summary(self) -> dict[str, Any]:
        return {
            "name": platform.node(),
            "system": platform.system(),
            "release": platform.release(),
            "version": self.config.runtime.host_version,
            "cpu_count": os.cpu_count(),
            "memory": self._memory_summary(),
        }

    def _memory_summary(self) -> dict[str, int] | None:
        meminfo = Path("/proc/meminfo")
        if not meminfo.exists():
            return None
        values: dict[str, int] = {}
        for line in meminfo.read_text(encoding="utf-8", errors="ignore").splitlines():
            key, _, rest = line.partition(":")
            if key in {"MemTotal", "MemAvailable"}:
                values[key] = int(rest.strip().split()[0]) * 1024
        return values or None

    def _outputd_summary(self) -> dict[str, Any]:
        status = self.runtime.outputd_status()
        sources = status.get("sources") if isinstance(status.get("sources"), list) else []
        return {
            "active": status.get("active", ""),
            "connector": status.get("connector", ""),
            "mode": status.get("mode", ""),
            "failed": status.get("failed", 0),
            "cursor": status.get("cursor", {}),
            "received": status.get("received", 0),
            "presented": status.get("presented", 0),
            "sources": sources,
            "error": status.get("error", ""),
        }

    def _stream_summary(self, desktop: DesktopConfig) -> dict[str, Any]:
        text = self.runtime.safe_log(desktop.log_file)
        summary: dict[str, Any] = {
            "fps": None,
            "capture_ms": None,
            "encoded": None,
            "encode_failures": None,
        }
        for line in reversed(text.splitlines()):
            if "update-stats" not in line:
                continue
            for key, pattern in STAT_FIELDS.items():
                match = pattern.search(line)
                if not match:
                    continue
                summary[key] = float(match.group(1)) if key == "fps" else int(match.group(1))
            break
        return summary
