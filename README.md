# GVT Cloud Server

Server-side control plane and host/QEMU display path for the GVT-g cloud
desktop project.

Target repository: `https://github.com/fangquan1/gvt-cloud-server`

Start with:

- `docs/SERVER_REQUIREMENTS.md`
- `src/qemu/gvt-stream.c`
- `src/outputd/gvt-outputd.c`
- `src/outputd/gvt-output-web.py`
- `scripts/start_gvt_stream_qemu.py`

Current baseline:

- QEMU `gvt-stream` H.264 RTP video path.
- SPICE audio/session retained.
- Native QEMU TCP input.
- Optional power-saving mode.
- Multi-VM physical DP/HDMI switching through `gvt-outputd`.

Do not commit VM images, passwords, private `.env` files, root keys, full logs,
or generated binaries.
