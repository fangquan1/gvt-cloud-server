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
    def __init__(self) -> None:
        self.commands: list[list[str]] = []

    def run(self, command: list[str], timeout: int = 60) -> CommandResult:
        self.commands.append(command)
        return CommandResult(command, 0, "ok", "")


def make_service(tmp: Path) -> tuple[GvtCloudService, FakeRunner]:
    root = tmp / "runtime"
    root.mkdir()
    (root / "vm1.pid").write_text("1234", encoding="utf-8")
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
    runner = FakeRunner()
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
            self.assertEqual(vm1["state"], "running")
            self.assertEqual(vm1["gvt_stream"]["fps"], 59.8)
            self.assertEqual(vm1["gvt_stream"]["capture_ms"], 16)
            self.assertEqual(vm1["gvt_stream"]["encode_failures"], 0)

    def test_actions_dispatch_to_multivm_script_templates(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            service, runner = make_service(Path(raw))
            service.set_desktop_mode("vm1", "power_save")
            service.start_desktop("vm1")
            service.stop_desktop("vm1")
            service.select_input("vm2")
            self.assertEqual(runner.commands[0][-3:], ["set-mode", "vm1", "power_save"])
            self.assertEqual(runner.commands[1][-2:], ["start-vm", "vm1"])
            self.assertEqual(runner.commands[2][-2:], ["stop-vm", "vm1"])
            self.assertEqual(runner.commands[3][-2:], ["input-select", "vm2"])

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
