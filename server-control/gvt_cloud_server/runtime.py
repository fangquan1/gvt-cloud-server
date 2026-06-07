from __future__ import annotations

import json
import os
import re
import socket
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

from .config import DesktopConfig, RuntimeConfig, ServerConfig


MAGIC = 0x4756544F
VERSION = 1
MSG_SELECT = 2
MSG_REMOVE = 3
SOURCE_LEN = 32
MDEV_PARENT = Path("/sys/devices/pci0000:00/0000:00:02.0")

SECRET_PATTERNS = [
    re.compile(r"(?i)\b([A-Z0-9_-]*(?:password|passwd|pwd|token|secret|api[_-]?key)[A-Z0-9_-]*)\s*=\s*([^\s]+)"),
    re.compile(r"(?i)(--password|--token|--secret)\s+([^\s]+)"),
    re.compile(r"(?i)(Authorization:\s*Bearer\s+)[A-Za-z0-9._~+/=-]+"),
]


@dataclass
class CommandResult:
    command: list[str]
    returncode: int
    stdout: str
    stderr: str

    @property
    def ok(self) -> bool:
        return self.returncode == 0


class Runner(Protocol):
    def run(self, command: list[str], timeout: int = 60) -> CommandResult:
        ...


class LocalRunner:
    def run(self, command: list[str], timeout: int = 60) -> CommandResult:
        completed = subprocess.run(
            command,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        return CommandResult(command, completed.returncode, completed.stdout, completed.stderr)


def redact(text: str) -> str:
    out = text
    for pattern in SECRET_PATTERNS:
        if pattern.pattern.startswith("(?i)(Authorization"):
            out = pattern.sub(r"\1[REDACTED]", out)
        elif "password|passwd" in pattern.pattern:
            out = pattern.sub(r"\1=[REDACTED]", out)
        else:
            out = pattern.sub(r"\1 [REDACTED]", out)
    return out


def tail_file(path: str | Path, max_lines: int, max_bytes: int) -> str:
    file_path = Path(path)
    if not file_path.exists():
        return ""
    size = file_path.stat().st_size
    with file_path.open("rb") as handle:
        if size > max_bytes:
            handle.seek(size - max_bytes)
        data = handle.read(max_bytes)
    text = data.decode("utf-8", errors="replace")
    return "\n".join(text.splitlines()[-max_lines:])


class ControlRuntime:
    def __init__(self, config: ServerConfig, runner: Runner | None = None) -> None:
        self.config = config
        self.runner = runner or LocalRunner()

    def command(self, name: str, **values: object) -> CommandResult:
        template = self.config.commands.get(name)
        if not template:
            return CommandResult([], 0, "", "")
        args = [part.format(**values) for part in template]
        return self.runner.run(args, timeout=90)

    def create_qcow2(self, path: str | Path, size_gib: int) -> CommandResult:
        image_path = Path(path)
        image_path.parent.mkdir(parents=True, exist_ok=True)
        return self.runner.run(["qemu-img", "create", "-f", "qcow2", str(image_path), f"{size_gib}G"], timeout=180)

    def read_json_file(self, path: str | Path) -> dict:
        file_path = Path(path)
        if not file_path.exists():
            return {}
        try:
            return json.loads(file_path.read_text(encoding="utf-8", errors="replace"))
        except Exception as exc:
            return {"error": str(exc)}

    def outputd_status(self) -> dict:
        return self.read_json_file(self.config.runtime.outputd_status)

    def pid_status(self, desktop: DesktopConfig) -> tuple[int | None, bool]:
        found_pid = self.qemu_pid_for_desktop(desktop.id)
        if not desktop.pid_file:
            return found_pid, found_pid is not None
        path = Path(desktop.pid_file)
        if not path.exists():
            return found_pid, found_pid is not None
        try:
            pid = int(path.read_text(encoding="utf-8", errors="ignore").strip())
        except ValueError:
            return found_pid, found_pid is not None
        if self.pid_matches_desktop(pid, desktop.id):
            return pid, True
        return found_pid or pid, found_pid is not None

    def pid_exists(self, pid: int) -> bool:
        if pid <= 0:
            return False
        if os.name == "nt":
            return True
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False

    def qemu_cmdline_for_desktop(self, desktop_id: str) -> list[str]:
        pid = self.qemu_pid_for_desktop(desktop_id)
        if pid is None or os.name == "nt":
            return []
        try:
            raw = Path(f"/proc/{pid}/cmdline").read_bytes()
        except OSError:
            return []
        parts = [part.decode("utf-8", errors="replace") for part in raw.split(b"\0") if part]
        if parts and "qemu-system" in Path(parts[0]).name:
            return parts
        return []

    def pid_matches_desktop(self, pid: int, desktop_id: str) -> bool:
        if pid <= 0:
            return False
        if os.name == "nt":
            return self.pid_exists(pid)
        try:
            raw = Path(f"/proc/{pid}/cmdline").read_bytes()
        except OSError:
            return False
        parts = [part.decode("utf-8", errors="ignore") for part in raw.split(b"\0") if part]
        if not parts or "qemu-system" not in Path(parts[0]).name:
            return False
        return "-name" in parts and desktop_id in parts

    def qemu_pid_for_desktop(self, desktop_id: str) -> int | None:
        if os.name == "nt":
            return None
        proc = Path("/proc")
        for entry in proc.iterdir():
            if not entry.name.isdigit():
                continue
            pid = int(entry.name)
            if self.pid_matches_desktop(pid, desktop_id):
                return pid
        return None

    def gvt_profiles(self) -> list[dict]:
        base = MDEV_PARENT / "mdev_supported_types"
        if not base.exists():
            return []
        profiles: list[dict] = []
        for path in sorted(base.glob("i915-GVTg_*")):
            description = (path / "description").read_text(encoding="utf-8", errors="replace") if (path / "description").exists() else ""
            item: dict[str, object] = {
                "id": path.name,
                "available_instances": self._read_int(path / "available_instances"),
                "description": description,
            }
            for line in description.splitlines():
                key, _, value = line.partition(":")
                if key and value:
                    item[key.strip()] = value.strip()
            profiles.append(item)
        return profiles

    def _read_int(self, path: Path) -> int:
        try:
            return int(path.read_text(encoding="utf-8", errors="ignore").strip())
        except Exception:
            return 0

    def select_output(self, source: str) -> None:
        self._send_outputd_message(MSG_SELECT, source)

    def remove_output(self, source: str) -> None:
        self._send_outputd_message(MSG_REMOVE, source)

    def _send_outputd_message(self, msg_type: int, source: str) -> None:
        if not hasattr(socket, "AF_UNIX"):
            return
        if os.name != "nt" and not Path(self.config.runtime.outputd_socket).exists():
            return
        source_bytes = source.encode("ascii", "ignore")[: SOURCE_LEN - 1]
        source_bytes += b"\0" * (SOURCE_LEN - len(source_bytes))
        msg = struct.pack(
            "<IIIIIIIIQQ32s",
            MAGIC,
            VERSION,
            msg_type,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            source_bytes,
        )
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            sock.sendto(msg, self.config.runtime.outputd_socket)
        finally:
            sock.close()

    def safe_log(self, path: str) -> str:
        runtime: RuntimeConfig = self.config.runtime
        return redact(tail_file(path, runtime.log_tail_lines, runtime.log_max_bytes))
