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

Single-command VM start for the current Windows 10 baseline:

```bash
./scripts/gvt-qemu-win10.sh --client 192.168.0.110 --port 5004
```

This wrapper expands to the existing QEMU gvt-stream command, disables FEC,
uses H.265 by default, and publishes:

- gvt-stream video RTP on the selected `--port`
- SPICE audio/session on `--spice-port` (default `5900`)
- native input on `--input-port` (default `5905`)

The underlying QEMU display option is now explicit as
`-display gvt-stream,host=<client>,port=<port>,codec=<codec>,...`. The current
RTP transport is still server-push, so `--client` is required until the display
backend grows a true viewer-style session handshake.

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
