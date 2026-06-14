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
- `scripts/gvt-qm`
- `deploy/systemd/gvt-qemu@.service`

Current baseline:

- QEMU `gvt-stream` H.264 RTP video path.
- SPICE audio/session retained.
- Native QEMU TCP input.
- Optional power-saving mode.
- Multi-VM physical DP/HDMI switching through `gvt-outputd`.

PVE-style VM lifecycle:

```bash
./deploy/install-gvt-qm.sh
gvt-qm start win10
gvt-qm status win10
gvt-qm logs win10
gvt-qm stop win10
```

The VM config lives in `/etc/gvt-qm/win10.conf`, and the QEMU process is owned
by `gvt-qemu@win10.service`, so closing the SSH terminal does not kill the VM.
The default Windows 10 baseline publishes:

- gvt-stream control/video port `5004`
- SPICE audio/session port `5900`
- native input port `5905`

The underlying QEMU display option is still explicit in the generated command:
`-display gvt-stream,rendernode=/dev/dri/renderD128,codec=h265,port=5004`.

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
