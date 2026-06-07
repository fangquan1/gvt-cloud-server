from __future__ import annotations

import json
import os
import platform
import re
import shlex
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

    def gvt_profiles(self) -> list[dict[str, Any]]:
        return self.runtime.gvt_profiles()

    def desktop(self, desktop_id: str) -> dict[str, Any]:
        desktop = self._desktop_config(desktop_id)
        pid, running = self.runtime.pid_status(desktop)
        mode = self._state.get("modes", {}).get(desktop.id, desktop.mode)
        profile = self._desktop_profile(desktop)
        return {
            "id": desktop.id,
            "name": desktop.name,
            "state": "running" if running else "stopped",
            "pid": pid,
            "mode": mode,
            "gvt_profile": profile,
            "resources": self._desktop_resources(desktop),
            "ports": {
                "spice": desktop.spice_port,
                "input": desktop.input_port,
                "video": desktop.video_port,
                "audio": desktop.audio_port,
            },
            "resolution": self._profile_resolution(profile) or desktop.resolution,
            "overlay": desktop.overlay,
            "tap": desktop.tap,
            "mac": desktop.mac,
            "gvt_stream": self._stream_summary(desktop) if running else self._empty_stream_summary(),
            "qemu_command": self._qemu_command(desktop) if running else {"args": [], "line": ""},
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
        profile = str(payload.get("gvt_profile") or payload.get("profile") or self._desktop_profile(desktop))
        self._validate_profile(profile)
        self._set_profile_state(desktop.id, profile)
        resources = self._desktop_resources(desktop)
        if self.config.commands.get("desktop_resources"):
            self._run(
                "desktop_resources",
                id=desktop.id,
                vcpus=resources["vcpus"],
                memory_mib=resources["memory_mib"],
            )
        client_host = str(payload.get("client_host") or payload.get("_client_host") or "")
        result = self._run("desktop_start", id=desktop.id, mode=mode, profile=profile, client_host=client_host)
        return self._with_command(self.desktop(desktop.id), result)

    def stop_desktop(self, desktop_id: str) -> dict[str, Any]:
        desktop = self._desktop_config(desktop_id)
        result = self._run("desktop_stop", id=desktop.id)
        if self.runtime.pid_status(desktop)[1]:
            raise ApiError(500, f"{desktop.id} is still running after stop command")
        return self._with_command(self.desktop(desktop.id), result)

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
        return self._with_command(self.desktop(desktop.id), result)

    def set_desktop_profile(self, desktop_id: str, profile: str) -> dict[str, Any]:
        desktop = self._desktop_config(desktop_id)
        if self.runtime.pid_status(desktop)[1]:
            raise ApiError(409, "stop the desktop before changing GVT-g profile")
        self._validate_profile(profile)
        self._set_profile_state(desktop.id, profile)
        result = self._run("desktop_profile", id=desktop.id, profile=profile)
        return self._with_command(self.desktop(desktop.id), result)

    def set_desktop_resources(self, desktop_id: str, payload: dict[str, Any]) -> dict[str, Any]:
        desktop = self._desktop_config(desktop_id)
        if self.runtime.pid_status(desktop)[1]:
            raise ApiError(409, "stop the desktop before changing CPU or memory")
        vcpus = self._as_int(payload.get("vcpus"), desktop.vcpus)
        memory_mib = self._as_int(payload.get("memory_mib"), desktop.memory_mib)
        self._validate_resources(vcpus, memory_mib)
        self._set_resource_state(desktop.id, vcpus, memory_mib)
        result = self._run("desktop_resources", id=desktop.id, vcpus=vcpus, memory_mib=memory_mib)
        return self._with_command(self.desktop(desktop.id), result)

    def select_output(self, source: str) -> dict[str, Any]:
        self._desktop_config(source)
        self._state["output_source"] = source
        self._save_state()
        result = self._run("output_select", source=source)
        if result["ok"] and not self.config.commands.get("output_select"):
            self.runtime.select_output(source)
        return self._with_command(self.status(), result)

    def select_input(self, source: str) -> dict[str, Any]:
        self._desktop_config(source)
        self._state["input_source"] = source
        self._save_state()
        result = self._run("input_select", source=source)
        return self._with_command(self.status(), result)

    def select_audio(self, source: str) -> dict[str, Any]:
        self._desktop_config(source)
        self._state["audio_source"] = source
        self._save_state()
        result = self._run("audio_select", source=source)
        return self._with_command(self.status(), result)

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

    def _with_command(self, payload: dict[str, Any], result: dict[str, Any]) -> dict[str, Any]:
        payload["last_command"] = result
        return payload

    def _desktop_config(self, desktop_id: str) -> DesktopConfig:
        for desktop in self.config.desktops:
            if desktop.id == desktop_id:
                return desktop
        raise ApiError(404, "desktop not found")

    def _load_state(self) -> dict[str, Any]:
        path = Path(self.config.runtime.state_file)
        if not path.exists():
            return {"modes": {}, "profiles": {}}
        try:
            data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
            return data if isinstance(data, dict) else {"modes": {}, "profiles": {}}
        except Exception:
            return {"modes": {}, "profiles": {}}

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

    def _set_profile_state(self, desktop_id: str, profile: str) -> None:
        profiles = self._state.setdefault("profiles", {})
        profiles[desktop_id] = profile
        self._save_state()

    def _set_resource_state(self, desktop_id: str, vcpus: int, memory_mib: int) -> None:
        resources = self._state.setdefault("resources", {})
        resources[desktop_id] = {"vcpus": vcpus, "memory_mib": memory_mib}
        self._save_state()

    def _desktop_profile(self, desktop: DesktopConfig) -> str:
        return str(self._state.get("profiles", {}).get(desktop.id, desktop.gvt_profile))

    def _desktop_resources(self, desktop: DesktopConfig) -> dict[str, int]:
        raw = self._state.get("resources", {}).get(desktop.id, {})
        if not isinstance(raw, dict):
            raw = {}
        return {
            "vcpus": self._as_int(raw.get("vcpus"), desktop.vcpus),
            "memory_mib": self._as_int(raw.get("memory_mib"), desktop.memory_mib),
        }

    def _validate_resources(self, vcpus: int, memory_mib: int) -> None:
        max_vcpus = max(1, min(os.cpu_count() or 1, 16))
        if vcpus < 1 or vcpus > max_vcpus:
            raise ApiError(400, f"vcpus must be between 1 and {max_vcpus}")
        if memory_mib < 1024 or memory_mib > 32768:
            raise ApiError(400, "memory_mib must be between 1024 and 32768")
        if memory_mib % 256 != 0:
            raise ApiError(400, "memory_mib must be a multiple of 256")

    def _as_int(self, value: Any, default: int) -> int:
        try:
            return int(value)
        except (TypeError, ValueError):
            return default

    def _qemu_command(self, desktop: DesktopConfig) -> dict[str, Any]:
        args = self.runtime.qemu_cmdline_for_desktop(desktop.id)
        return {"args": args, "line": shlex.join(args) if args else ""}

    def _validate_profile(self, profile: str) -> None:
        profiles = self.gvt_profiles()
        if profiles and profile not in {str(item.get("id")) for item in profiles}:
            raise ApiError(400, "invalid GVT-g profile")

    def _profile_resolution(self, profile: str) -> str:
        for item in self.gvt_profiles():
            if item.get("id") == profile:
                return str(item.get("resolution") or "")
        return ""

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

    def _empty_stream_summary(self) -> dict[str, Any]:
        return {
            "fps": None,
            "capture_ms": None,
            "encoded": None,
            "encode_failures": None,
        }
