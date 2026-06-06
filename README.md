# GVT Cloud Server

Server-side control plane and host/QEMU display path for the GVT-g cloud
desktop project.

Target repository: `https://github.com/fangquan1/gvt-cloud-server`

Start with:

- `docs/SERVER_REQUIREMENTS.md`
- `server-control/README.md`
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

Control plane MVP:

```bash
export GVT_SERVER_API_PASSWORD='change-me'
export PYTHONPATH="$PWD/server-control"
python3 -m gvt_cloud_server --config /etc/gvt-cloud-server/config.json
```

Run local checks from the repository root:

```bash
python -m unittest discover -s server-control/tests
python -m compileall -q server-control/gvt_cloud_server
```

Do not commit VM images, passwords, private `.env` files, root keys, full logs,
or generated binaries.
