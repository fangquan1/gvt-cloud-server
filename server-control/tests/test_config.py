from __future__ import annotations

import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from gvt_cloud_server.config import config_from_dict


class ConfigTests(unittest.TestCase):
    def test_defaults_match_current_two_vm_baseline(self) -> None:
        config = config_from_dict({})
        self.assertEqual(config.bind_port, 8098)
        self.assertEqual([desktop.id for desktop in config.desktops], ["vm1", "vm2"])
        self.assertEqual(config.desktops[0].spice_port, 5900)
        self.assertEqual(config.desktops[0].gvt_profile, "i915-GVTg_V5_8")
        self.assertEqual(config.desktops[0].vcpus, 4)
        self.assertEqual(config.desktops[0].memory_mib, 4096)
        self.assertEqual(config.desktops[1].input_port, 5906)
        self.assertEqual(config.commands["desktop_start"], ["/root/qemu_cmd/multivm/multivm_remote.sh", "start-vm", "{id}", "{client_host}"])
        self.assertEqual(config.commands["desktop_profile"], ["/root/qemu_cmd/multivm/multivm_remote.sh", "set-profile", "{id}", "{profile}"])
        self.assertEqual(config.commands["desktop_resources"], ["/root/qemu_cmd/multivm/multivm_remote.sh", "set-resources", "{id}", "{vcpus}", "{memory_mib}"])

    def test_custom_root_rewrites_default_paths(self) -> None:
        config = config_from_dict({"runtime": {"root_dir": "/tmp/gvt"}})
        self.assertEqual(config.desktops[0].pid_file, "/tmp/gvt/vm1.pid")
        self.assertEqual(config.commands["output_select"][0], "/tmp/gvt/multivm_remote.sh")

    def test_empty_desktops_uses_default_baseline(self) -> None:
        config = config_from_dict({"desktops": []})
        self.assertEqual([desktop.id for desktop in config.desktops], ["vm1", "vm2"])


if __name__ == "__main__":
    unittest.main()
