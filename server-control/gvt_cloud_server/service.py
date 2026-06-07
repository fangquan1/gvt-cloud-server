from __future__ import annotations

import json
import os
import platform
import re
import shlex
import secrets
import time
import uuid
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
        outputd_status_valid = not outputd.get("error")
        active_source = str(outputd.get("active") or "") if outputd_status_valid else str(self._state.get("output_source", ""))
        return {
            "version": __version__,
            "host": self._host_summary(),
            "outputd": outputd,
            "active_source": active_source,
            "input_source": self._state.get("input_source", ""),
            "audio_source": self._state.get("audio_source", ""),
            "desktops": self.desktops(),
        }

    def desktops(self) -> list[dict[str, Any]]:
        return [self.desktop(item.id) for item in self._all_desktop_configs()]

    def gvt_profiles(self) -> list[dict[str, Any]]:
        return self.runtime.gvt_profiles()

    def upload_target_path(self, kind: str, filename: str) -> Path:
        if kind not in {"iso", "qcow2"}:
            raise ApiError(400, "upload kind must be iso or qcow2")
        lowered = filename.lower()
        if kind == "iso" and not lowered.endswith(".iso"):
            raise ApiError(400, "ISO upload must end with .iso")
        if kind == "qcow2" and not lowered.endswith(".qcow2"):
            raise ApiError(400, "qcow2 upload must end with .qcow2")
        return self.runtime.upload_path(kind, filename)

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
            "install_iso": self._desktop_iso(desktop),
            "disk_size_gib": desktop.disk_size_gib,
            "tap": desktop.tap,
            "mac": desktop.mac,
            "gvt_stream": self._stream_summary(desktop) if running else self._empty_stream_summary(),
            "qemu_command": self._qemu_command(desktop) if running else {"args": [], "line": ""},
        }

    def setup(self) -> dict[str, Any]:
        return self._run("setup")

    def create_desktop(self, payload: dict[str, Any]) -> dict[str, Any]:
        name = str(payload.get("name") or "Windows Desktop").strip() or "Windows Desktop"
        if name in {desktop.name for desktop in self._all_desktop_configs()}:
            raise ApiError(409, "desktop name already exists")
        desktop_id = self._desktop_id_from_name(str(payload.get("id") or name))
        if desktop_id in {desktop.id for desktop in self._all_desktop_configs()}:
            raise ApiError(409, "desktop id already exists")
        mode = str(payload.get("mode") or "realtime")
        if mode not in VALID_MODES:
            raise ApiError(400, "invalid mode")
        profile = str(payload.get("gvt_profile") or payload.get("profile") or "i915-GVTg_V5_8")
        self._validate_profile(profile)
        vcpus = self._as_int(payload.get("vcpus"), 4)
        memory_mib = self._as_int(payload.get("memory_mib"), 4096)
        self._validate_resources(vcpus, memory_mib)
        disk_size_gib = self._as_int(payload.get("disk_size_gib"), 80)
        if disk_size_gib < 20 or disk_size_gib > 1024:
            raise ApiError(400, "disk_size_gib must be between 20 and 1024")

        qcow2_path = str(payload.get("qcow2_path") or "").strip()
        install_iso = str(payload.get("install_iso") or payload.get("iso_path") or "").strip()
        if not qcow2_path:
            qcow2_path = str(Path(self.config.runtime.root_dir) / "disks" / f"{desktop_id}.qcow2")
            result = self.runtime.create_qcow2(qcow2_path, disk_size_gib)
            if not result.ok:
                response = self._command_response(result)
                raise ApiError(500, response.get("stderr") or response.get("stdout") or "failed to create qcow2")

        ports = self._allocate_ports()
        index = len(self._all_desktop_configs()) + 1
        desktop_data = {
            "id": desktop_id,
            "name": name,
            "spice_port": ports["spice"],
            "input_port": ports["input"],
            "video_port": ports["video"],
            "audio_port": ports["spice"],
            "vcpus": vcpus,
            "memory_mib": memory_mib,
            "resolution": self._profile_resolution(profile) or "1024x768",
            "overlay": qcow2_path,
            "pid_file": f"{self.config.runtime.root_dir}/{desktop_id}.pid",
            "log_file": f"{self.config.runtime.root_dir}/{desktop_id}.log",
            "mode": mode,
            "gvt_profile": profile,
            "tap": f"tap-{desktop_id}"[:15],
            "mac": self._mac_for_index(index),
            "install_iso": install_iso,
            "disk_size_gib": disk_size_gib,
            "uuid": str(uuid.uuid4()),
        }
        self._state.setdefault("desktops", {})[desktop_id] = desktop_data
        self._set_mode_state(desktop_id, mode, save=False)
        self._set_profile_state(desktop_id, profile, save=False)
        self._set_resource_state(desktop_id, vcpus, memory_mib, save=False)
        self._save_state()
        return self.desktop(desktop_id)

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
        self.runtime.remove_output(desktop.id)
        if self._state.get("output_source") == desktop.id:
            self._state["output_source"] = ""
            self._save_state()
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

    def set_desktop_iso(self, desktop_id: str, payload: dict[str, Any]) -> dict[str, Any]:
        desktop = self._desktop_config(desktop_id)
        if self.runtime.pid_status(desktop)[1]:
            raise ApiError(409, "stop the desktop before changing ISO attachment")
        iso_path = str(payload.get("iso_path") or payload.get("install_iso") or "").strip()
        dynamic = self._state.setdefault("desktops", {}).get(desktop.id)
        if isinstance(dynamic, dict):
            dynamic["install_iso"] = iso_path
        else:
            self._state.setdefault("iso", {})[desktop.id] = iso_path
        self._save_state()
        return self.desktop(desktop.id)

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
        for desktop in self._all_desktop_configs():
            if desktop.id == desktop_id:
                return desktop
        raise ApiError(404, "desktop not found")

    def _all_desktop_configs(self) -> list[DesktopConfig]:
        desktops = list(self.config.desktops)
        raw_desktops = self._state.get("desktops", {})
        if isinstance(raw_desktops, dict):
            for item in raw_desktops.values():
                if isinstance(item, dict):
                    desktops.append(self._desktop_from_state(item))
        return desktops

    def _desktop_from_state(self, data: dict[str, Any]) -> DesktopConfig:
        return DesktopConfig(
            id=str(data.get("id", "")),
            name=str(data.get("name") or data.get("id") or "desktop"),
            spice_port=self._as_int(data.get("spice_port"), 0),
            input_port=self._as_int(data.get("input_port"), 0),
            video_port=self._as_int(data.get("video_port"), 0),
            audio_port=self._as_int(data.get("audio_port"), self._as_int(data.get("spice_port"), 0)),
            vcpus=self._as_int(data.get("vcpus"), 4),
            memory_mib=self._as_int(data.get("memory_mib"), 4096),
            resolution=str(data.get("resolution") or "1024x768"),
            overlay=str(data.get("overlay") or ""),
            pid_file=str(data.get("pid_file") or f"{self.config.runtime.root_dir}/{data.get('id', 'desktop')}.pid"),
            log_file=str(data.get("log_file") or f"{self.config.runtime.root_dir}/{data.get('id', 'desktop')}.log"),
            mode=str(data.get("mode") or "realtime"),
            gvt_profile=str(data.get("gvt_profile") or "i915-GVTg_V5_8"),
            tap=str(data.get("tap") or f"tap-{data.get('id', 'desktop')}")[:15],
            mac=str(data.get("mac") or "52:54:00:10:99:88"),
            install_iso=str(data.get("install_iso") or ""),
            disk_size_gib=self._as_int(data.get("disk_size_gib"), 80),
        )

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

    def _set_mode_state(self, desktop_id: str, mode: str, save: bool = True) -> None:
        modes = self._state.setdefault("modes", {})
        modes[desktop_id] = mode
        if save:
            self._save_state()

    def _set_profile_state(self, desktop_id: str, profile: str, save: bool = True) -> None:
        profiles = self._state.setdefault("profiles", {})
        profiles[desktop_id] = profile
        if save:
            self._save_state()

    def _set_resource_state(self, desktop_id: str, vcpus: int, memory_mib: int, save: bool = True) -> None:
        resources = self._state.setdefault("resources", {})
        resources[desktop_id] = {"vcpus": vcpus, "memory_mib": memory_mib}
        if save:
            self._save_state()

    def _desktop_profile(self, desktop: DesktopConfig) -> str:
        return str(self._state.get("profiles", {}).get(desktop.id, desktop.gvt_profile))

    def _desktop_iso(self, desktop: DesktopConfig) -> str:
        raw_desktop = self._state.get("desktops", {}).get(desktop.id)
        if isinstance(raw_desktop, dict):
            return str(raw_desktop.get("install_iso") or "")
        return str(self._state.get("iso", {}).get(desktop.id, desktop.install_iso))

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

    def _desktop_id_from_name(self, raw: str) -> str:
        return re.sub(r"[^a-zA-Z0-9_-]+", "-", raw.strip().lower()).strip("-") or "desktop"

    def _allocate_ports(self) -> dict[str, int]:
        desktops = self._all_desktop_configs()
        used_spice = {desktop.spice_port for desktop in desktops}
        used_input = {desktop.input_port for desktop in desktops}
        used_video = {desktop.video_port for desktop in desktops}

        def next_port(start: int, used: set[int], step: int = 1) -> int:
            port = start
            while port in used:
                port += step
            return port

        return {
            "spice": next_port(5900, used_spice),
            "input": next_port(5905, used_input),
            "video": next_port(5004, used_video, 2),
        }

    def _mac_for_index(self, index: int) -> str:
        value = max(1, min(index, 255))
        return f"52:54:00:10:{value:02x}:88"

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
        active_name = str(status.get("active", ""))
        active_source = next(
            (item for item in sources if isinstance(item, dict) and item.get("name") == active_name),
            None,
        )
        source_resolution = ""
        if active_source and active_source.get("width") and active_source.get("height"):
            source_resolution = f"{active_source.get('width')}x{active_source.get('height')}"
        physical_mode = str(status.get("mode", ""))
        return {
            "active": active_name,
            "connector": status.get("connector", ""),
            "mode": source_resolution or physical_mode,
            "source_resolution": source_resolution,
            "physical_mode": physical_mode,
            "active_source": active_source or {},
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
