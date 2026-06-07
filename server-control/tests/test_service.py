from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from gvt_cloud_server.config import config_from_dict
from gvt_cloud_server.runtime import CommandResult, ControlRuntime
from gvt_cloud_server.service import ApiError, GvtCloudService


class FakeRunner:
    def __init__(self, root: Path | None = None) -> None:
        self.root = root
        self.commands: list[list[str]] = []

    def run(self, command: list[str], timeout: int = 60) -> CommandResult:
        self.commands.append(command)
        if self.root and command[-2:] == ["stop-vm", "vm1"]:
            (self.root / "vm1.pid").unlink(missing_ok=True)
        if self.root and command[-2:] == ["stop-vm", "vm2"]:
            (self.root / "vm2.pid").unlink(missing_ok=True)
        return CommandResult(command, 0, "ok", "")


def make_service(tmp: Path) -> tuple[GvtCloudService, FakeRunner]:
    root = tmp / "runtime"
    root.mkdir()
    (root / "vm1.log").write_text(
        "WEB_PASSWORD=very-secret\n"
        "gvt-stream: update-stats fps=59.8 capture_ms=16 encoded=120 encode_failures=0\n",
        encoding="utf-8",
    )
    (root / "vm2.log").write_text("", encoding="utf-8")
    status = tmp / "outputd.status"
    status.write_text(
        json.dumps({"active": "vm1", "connector": "DP-1", "failed": 0, "sources": [{"name": "vm1"}]}),
        encoding="utf-8",
    )
    config = config_from_dict(
        {
            "auth": {"password": "pw"},
            "runtime": {
                "root_dir": str(root).replace("\\", "/"),
                "state_file": str(tmp / "state.json"),
                "outputd_status": str(status),
            },
        }
    )
    runner = FakeRunner(root)
    service = GvtCloudService(config, ControlRuntime(config, runner))
    return service, runner


class ServiceTests(unittest.TestCase):
    def test_login_and_token(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            service, _ = make_service(Path(raw))
            token = service.login("pw")["token"]
            self.assertTrue(service.check_token(token))
            with self.assertRaises(ApiError):
                service.login("bad")

    def test_status_contains_desktop_outputd_and_stream_stats(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            service, _ = make_service(Path(raw))
            status = service.status()
            self.assertEqual(status["outputd"]["active"], "vm1")
            vm1 = status["desktops"][0]
            self.assertEqual(vm1["state"], "stopped")
            self.assertEqual(vm1["gvt_profile"], "i915-GVTg_V5_8")
            self.assertEqual(vm1["resources"], {"vcpus": 4, "memory_mib": 4096})
            self.assertEqual(vm1["qemu_command"], {"args": [], "line": ""})
            self.assertIsNone(vm1["gvt_stream"]["fps"])

    def test_actions_dispatch_to_multivm_script_templates(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            service, runner = make_service(Path(raw))
            service.set_desktop_mode("vm1", "power_save")
            service.set_desktop_resources("vm1", {"vcpus": 3, "memory_mib": 6144})
            service.set_desktop_profile("vm2", "i915-GVTg_V5_4")
            service.start_desktop("vm1")
            service.stop_desktop("vm1")
            service.select_input("vm2")
            self.assertEqual(runner.commands[0][-3:], ["set-mode", "vm1", "power_save"])
            self.assertEqual(runner.commands[1][-4:], ["set-resources", "vm1", "3", "6144"])
            self.assertEqual(runner.commands[2][-3:], ["set-profile", "vm2", "i915-GVTg_V5_4"])
            self.assertEqual(runner.commands[3][-4:], ["set-resources", "vm1", "3", "6144"])
            self.assertEqual(runner.commands[4][-3:], ["start-vm", "vm1", ""])
            self.assertEqual(runner.commands[5][-2:], ["stop-vm", "vm1"])
            self.assertEqual(runner.commands[6][-2:], ["input-select", "vm2"])

    def test_create_desktop_registers_stopped_install_vm(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            service, runner = make_service(Path(raw))
            created = service.create_desktop({
                "name": "Windows Install",
                "vcpus": 4,
                "memory_mib": 4096,
                "disk_size_gib": 80,
                "iso_path": "/root/iso/windows.iso",
                "mode": "realtime",
                "gvt_profile": "i915-GVTg_V5_8",
            })
            self.assertEqual(created["state"], "stopped")
            self.assertEqual(created["install_iso"], "/root/iso/windows.iso")
            self.assertEqual(created["disk_size_gib"], 80)
            self.assertTrue(created["overlay"].replace("\\", "/").endswith("/disks/windows-install.qcow2"))
            self.assertEqual(runner.commands[0][:4], ["qemu-img", "create", "-f", "qcow2"])
            self.assertIn(created["id"], {item["id"] for item in service.desktops()})

    def test_delete_dynamic_desktop_keeps_disk_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            service, _ = make_service(Path(raw))
            created = service.create_desktop({"name": "Delete Me", "gvt_profile": "i915-GVTg_V5_8"})
            disk = Path(created["overlay"])
            disk.write_text("disk", encoding="utf-8")
            result = service.delete_desktop(created["id"], {})
            self.assertEqual(result["ok"], True)
            self.assertTrue(disk.exists())
            self.assertNotIn(created["id"], {item["id"] for item in service.desktops()})

    def test_delete_dynamic_desktop_can_delete_disk(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            service, _ = make_service(Path(raw))
            created = service.create_desktop({"name": "Delete Disk", "gvt_profile": "i915-GVTg_V5_8"})
            disk = Path(created["overlay"])
            disk.write_text("disk", encoding="utf-8")
            service.delete_desktop(created["id"], {"delete_disk": True})
            self.assertFalse(disk.exists())

    def test_logs_are_redacted(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            service, _ = make_service(Path(raw))
            log = service.logs("vm1")["log"]
            self.assertIn("WEB_PASSWORD=[REDACTED]", log)
            self.assertNotIn("very-secret", log)

    def test_invalid_desktop_and_mode_raise_api_error(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            service, _ = make_service(Path(raw))
            with self.assertRaises(ApiError):
                service.desktop("missing")
            with self.assertRaises(ApiError):
                service.set_desktop_mode("vm1", "turbo")


if __name__ == "__main__":
    unittest.main()
