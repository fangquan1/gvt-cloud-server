# GVT Cloud Server

Minimal server-side patch set for the GVT-g cloud desktop project.

This repository keeps the host/QEMU display path plus a minimal systemd runner:

- `src/qemu/gvt-stream.c`: the current QEMU `gvt-stream` display backend.
- `src/qemu/gvt-stream-ipc.h`: v1 QEMU-to-streamd IPC message format.
- `src/gvt-streamd/gvt-streamd.c`: experimental standalone DMABUF encoder.
- `patches/qemu-gvt-stream.patch`: the patch to apply to a QEMU source tree.
- `SERVER.md`: build, run, verification, and troubleshooting notes.
- `deploy/systemd/gvt-qemu@.service`: optional systemd unit for QEMU VMs.
- `scripts/gvt-qm` and `scripts/gvt-qm-run`: small helpers for the unit.

The old kernel shadow framebuffer route, direct-stream RTP streamer, physical
output daemon, Python web/API control plane, multi-VM helper scripts, and
Windows batch launchers were removed from `current`. They remain available in
Git history if they are ever needed for reference.

Current baseline:

- GVT-g VFIO display DMABUF is consumed inside QEMU.
- VAAPI/GStreamer encodes H.264 or H.265 RTP video.
- SPICE is retained for audio/session, not primary video.
- QEMU native TCP input is handled by the `gvt-stream` backend.
- Client disconnect stops encoding.
- Guest display sleep/no-scanout reconnect is handled by cached frame fallback
  plus wakeup and automatic return to DMABUF encoding.
- On `current-detach`, `GVT_STREAM_EXTERNAL=1` enables an experimental
  `gvt-streamd` process that receives scanout DMABUF fds from QEMU and performs
  RTP encoding outside the QEMU process. The default remains the in-QEMU path.

Start with [SERVER.md](SERVER.md).
