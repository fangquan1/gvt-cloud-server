from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "direct-stream" / "start_gvt_stream_qemu.py"
REMOTE = "/root/qemu_cmd/multivm/multivm_remote.sh"
LOCAL_REMOTE = ROOT / "physical-output" / "multivm_remote.sh"
REMOTE_OUTPUTD = "/root/qemu_cmd/multivm/gvt-outputd.c"
LOCAL_OUTPUTD = ROOT / "physical-output" / "gvt-outputd.c"
REMOTE_WEB = "/root/qemu_cmd/multivm/gvt-output-web.py"
LOCAL_WEB = ROOT / "physical-output" / "gvt-output-web.py"


def load_helper():
    spec = importlib.util.spec_from_file_location("gvtq", HELPER)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {HELPER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def upload(client) -> None:
    sftp = client.open_sftp()
    try:
        try:
            sftp.mkdir("/root/qemu_cmd/multivm")
        except OSError:
            pass
        sftp.put(str(LOCAL_REMOTE), REMOTE)
        sftp.put(str(LOCAL_OUTPUTD), REMOTE_OUTPUTD)
        sftp.put(str(LOCAL_WEB), REMOTE_WEB)
    finally:
        sftp.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=[
        "setup", "start", "stop", "status", "select",
        "outputd-restart", "web-start", "web-stop", "web-status",
    ])
    parser.add_argument("source", nargs="?")
    parser.add_argument("--upload-only", action="store_true")
    args = parser.parse_args()

    helper = load_helper()
    client, _host = helper.connect()
    try:
        upload(client)
        print(helper.run(client, f"chmod +x {REMOTE}", timeout=10))
        if not args.upload_only:
            extra = f" {args.source}" if args.source else ""
            print(helper.run(client, f"{REMOTE} {args.command}{extra}", timeout=90))
    finally:
        client.close()


if __name__ == "__main__":
    main()
