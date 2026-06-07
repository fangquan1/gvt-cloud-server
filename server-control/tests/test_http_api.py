from __future__ import annotations

import json
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from gvt_cloud_server.config import config_from_dict
from gvt_cloud_server.http_api import make_handler
from gvt_cloud_server.runtime import CommandResult, ControlRuntime
from gvt_cloud_server.service import GvtCloudService


class FakeRunner:
    def run(self, command: list[str], timeout: int = 60) -> CommandResult:
        return CommandResult(command, 0, "ok", "")


class HttpApiTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name) / "runtime"
        root.mkdir()
        (root / "vm1.log").write_text("", encoding="utf-8")
        (root / "vm2.log").write_text("", encoding="utf-8")
        config = config_from_dict(
            {
                "auth": {"password": "pw"},
                "runtime": {
                    "root_dir": str(root).replace("\\", "/"),
                    "state_file": str(Path(self.tmp.name) / "state.json"),
                },
            }
        )
        service = GvtCloudService(config, ControlRuntime(config, FakeRunner()))
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(service))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def request(self, method: str, path: str, body: dict | None = None, token: str | None = None) -> dict:
        data = None if body is None else json.dumps(body).encode("utf-8")
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        request = urllib.request.Request(self.base + path, data=data, headers=headers, method=method)
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.loads(response.read().decode("utf-8"))

    def test_login_and_status(self) -> None:
        login = self.request("POST", "/api/login", {"password": "pw"})
        self.assertTrue(login["ok"])
        status = self.request("GET", "/api/status", token=login["token"])
        self.assertEqual(len(status["desktops"]), 2)

    def test_protected_endpoint_requires_auth(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as raised:
            self.request("GET", "/api/status")
        self.assertEqual(raised.exception.code, 401)
        raised.exception.close()

    def test_mode_endpoint(self) -> None:
        login = self.request("POST", "/api/login", {"password": "pw"})
        result = self.request("POST", "/api/desktops/vm1/mode", {"mode": "power_save"}, token=login["token"])
        self.assertEqual(result["mode"], "power_save")
        self.assertEqual(result["last_command"]["ok"], True)

    def test_profile_endpoints(self) -> None:
        login = self.request("POST", "/api/login", {"password": "pw"})
        profiles = self.request("GET", "/api/gvtg-profiles", token=login["token"])
        self.assertIn("profiles", profiles)
        result = self.request(
            "POST",
            "/api/desktops/vm1/profile",
            {"profile": "i915-GVTg_V5_4"},
            token=login["token"],
        )
        self.assertEqual(result["gvt_profile"], "i915-GVTg_V5_4")

    def test_resources_endpoint(self) -> None:
        login = self.request("POST", "/api/login", {"password": "pw"})
        result = self.request(
            "POST",
            "/api/desktops/vm1/resources",
            {"vcpus": 4, "memory_mib": 8192},
            token=login["token"],
        )
        self.assertEqual(result["resources"], {"vcpus": 4, "memory_mib": 8192})
        self.assertEqual(result["last_command"]["ok"], True)


if __name__ == "__main__":
    unittest.main()
