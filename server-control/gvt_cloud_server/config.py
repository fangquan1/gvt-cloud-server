from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


VALID_MODES = {"realtime", "realtime30", "power_save", "physical"}


@dataclass(frozen=True)
class DesktopConfig:
    id: str
    name: str
    spice_port: int
    input_port: int
    video_port: int = 5004
    audio_port: int = 5900
    vcpus: int = 4
    memory_mib: int = 4096
    resolution: str = "1024x768"
    overlay: str = ""
    pid_file: str = ""
    log_file: str = ""
    mode: str = "realtime"
    gvt_profile: str = "i915-GVTg_V5_8"
    tap: str = ""
    mac: str = ""


@dataclass(frozen=True)
class AuthConfig:
    password_env: str = "GVT_SERVER_API_PASSWORD"
    password: str = ""
    token_ttl_seconds: int = 86400


@dataclass(frozen=True)
class RuntimeConfig:
    root_dir: str = "/root/qemu_cmd/multivm"
    state_file: str = "/run/gvt-cloud-server/state.json"
    outputd_socket: str = "/run/gvt-outputd.sock"
    outputd_status: str = "/run/gvt-outputd.status"
    host_version: str = "gvt-cloud-server"
    log_tail_lines: int = 200
    log_max_bytes: int = 65536


@dataclass(frozen=True)
class ServerConfig:
    bind_host: str = "0.0.0.0"
    bind_port: int = 8098
    auth: AuthConfig = field(default_factory=AuthConfig)
    runtime: RuntimeConfig = field(default_factory=RuntimeConfig)
    desktops: tuple[DesktopConfig, ...] = field(default_factory=tuple)
    commands: dict[str, list[str]] = field(default_factory=dict)

    def auth_password(self) -> str:
        return os.environ.get(self.auth.password_env, "") or self.auth.password


def default_desktops(root_dir: str) -> tuple[DesktopConfig, ...]:
    return (
        DesktopConfig(
            id="vm1",
            name="Windows 10 VM1",
            spice_port=5900,
            input_port=5905,
            video_port=5004,
            audio_port=5900,
            vcpus=4,
            memory_mib=4096,
            overlay=f"{root_dir}/win10-vm1.qcow2",
            pid_file=f"{root_dir}/vm1.pid",
            log_file=f"{root_dir}/vm1.log",
            gvt_profile="i915-GVTg_V5_8",
            tap="tap-win10a",
            mac="52:54:00:10:01:88",
        ),
        DesktopConfig(
            id="vm2",
            name="Windows 10 VM2",
            spice_port=5901,
            input_port=5906,
            video_port=5008,
            audio_port=5901,
            vcpus=4,
            memory_mib=4096,
            overlay=f"{root_dir}/win10-vm2.qcow2",
            pid_file=f"{root_dir}/vm2.pid",
            log_file=f"{root_dir}/vm2.log",
            gvt_profile="i915-GVTg_V5_8",
            tap="tap-win10b",
            mac="52:54:00:10:02:88",
        ),
    )


def default_commands(root_dir: str) -> dict[str, list[str]]:
    script = f"{root_dir}/multivm_remote.sh"
    return {
        "setup": [script, "setup"],
        "desktop_start": [script, "start-vm", "{id}", "{client_host}"],
        "desktop_stop": [script, "stop-vm", "{id}"],
        "desktop_restart": [script, "restart-vm", "{id}"],
        "desktop_mode": [script, "set-mode", "{id}", "{mode}"],
        "desktop_profile": [script, "set-profile", "{id}", "{profile}"],
        "desktop_resources": [script, "set-resources", "{id}", "{vcpus}", "{memory_mib}"],
        "output_select": [script, "select", "{source}"],
        "input_select": [script, "input-select", "{source}"],
        "audio_select": [script, "audio-select", "{source}"],
        "status": [script, "status"],
    }


def _as_int(value: Any, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _desktop_from_dict(data: dict[str, Any], root_dir: str) -> DesktopConfig:
    desktop_id = str(data["id"])
    baseline = {d.id: d for d in default_desktops(root_dir)}.get(desktop_id)
    defaults = baseline or DesktopConfig(id=desktop_id, name=desktop_id, spice_port=0, input_port=0)
    mode = str(data.get("mode", defaults.mode))
    if mode not in VALID_MODES:
        mode = defaults.mode
    return DesktopConfig(
        id=desktop_id,
        name=str(data.get("name", defaults.name)),
        spice_port=_as_int(data.get("spice_port"), defaults.spice_port),
        input_port=_as_int(data.get("input_port"), defaults.input_port),
        video_port=_as_int(data.get("video_port"), defaults.video_port),
        audio_port=_as_int(data.get("audio_port"), defaults.audio_port),
        vcpus=_as_int(data.get("vcpus"), defaults.vcpus),
        memory_mib=_as_int(data.get("memory_mib"), defaults.memory_mib),
        resolution=str(data.get("resolution", defaults.resolution)),
        overlay=str(data.get("overlay", defaults.overlay)),
        pid_file=str(data.get("pid_file", defaults.pid_file)),
        log_file=str(data.get("log_file", defaults.log_file)),
        mode=mode,
        gvt_profile=str(data.get("gvt_profile", defaults.gvt_profile)),
        tap=str(data.get("tap", defaults.tap)),
        mac=str(data.get("mac", defaults.mac)),
    )


def config_from_dict(data: dict[str, Any]) -> ServerConfig:
    runtime_data = data.get("runtime") or {}
    root_dir = str(runtime_data.get("root_dir", "/root/qemu_cmd/multivm"))
    runtime = RuntimeConfig(
        root_dir=root_dir,
        state_file=str(runtime_data.get("state_file", "/run/gvt-cloud-server/state.json")),
        outputd_socket=str(runtime_data.get("outputd_socket", "/run/gvt-outputd.sock")),
        outputd_status=str(runtime_data.get("outputd_status", "/run/gvt-outputd.status")),
        host_version=str(runtime_data.get("host_version", "gvt-cloud-server")),
        log_tail_lines=_as_int(runtime_data.get("log_tail_lines"), 200),
        log_max_bytes=_as_int(runtime_data.get("log_max_bytes"), 65536),
    )
    auth_data = data.get("auth") or {}
    auth = AuthConfig(
        password_env=str(auth_data.get("password_env", "GVT_SERVER_API_PASSWORD")),
        password=str(auth_data.get("password", "")),
        token_ttl_seconds=_as_int(auth_data.get("token_ttl_seconds"), 86400),
    )
    desktops_data = data.get("desktops")
    desktops = (
        tuple(_desktop_from_dict(item, root_dir) for item in desktops_data)
        if isinstance(desktops_data, list)
        else default_desktops(root_dir)
    )
    commands = data.get("commands") if isinstance(data.get("commands"), dict) else default_commands(root_dir)
    normalized_commands = {str(k): [str(part) for part in v] for k, v in commands.items()}
    return ServerConfig(
        bind_host=str(data.get("bind_host", "0.0.0.0")),
        bind_port=_as_int(data.get("bind_port"), 8098),
        auth=auth,
        runtime=runtime,
        desktops=desktops,
        commands=normalized_commands,
    )


def load_config(path: str | Path | None) -> ServerConfig:
    if not path:
        return config_from_dict({})
    config_path = Path(path)
    if not config_path.exists():
        return config_from_dict({})
    return config_from_dict(json.loads(config_path.read_text(encoding="utf-8")))
