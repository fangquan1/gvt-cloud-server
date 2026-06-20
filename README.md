# GVT Cloud Server

Minimal server-side patch set for the GVT-g cloud desktop project.

This repository keeps the host/QEMU display path plus a minimal systemd runner:

- `src/qemu/gvt-stream.c`: the current QEMU `gvt-stream` display backend.
- `src/qemu/gvt-stream-ipc.h`: v1 QEMU-to-streamd IPC message format.
- `src/gvt-streamd/gvt-streamd.c`: standalone DMABUF encoder process.
- `patches/qemu-gvt-stream.patch`: the patch to apply to a QEMU source tree.
- `SERVER.md`: build, run, verification, and troubleshooting notes.
- `deploy/systemd/gvt-qemu@.service`: optional systemd unit for QEMU VMs.
- `scripts/gvt-qm` and `scripts/gvt-qm-run`: small helpers for the unit.

The old kernel shadow framebuffer route, direct-stream RTP streamer, physical
output daemon, Python web/API control plane, multi-VM helper scripts, and
Windows batch launchers were removed from `current`. They remain available in
Git history if they are ever needed for reference.

Current baseline:

- GVT-g VFIO display DMABUF is consumed by QEMU and forwarded to `gvt-streamd`.
- VAAPI/GStreamer encoding and RTP sending happen only in `gvt-streamd`.
- H.265 is the default codec; H.264 remains available as a client-selected mode.
- SPICE is retained for audio/session, not primary video.
- QEMU native TCP input is handled by the `gvt-stream` backend.
- Client disconnect stops encoding.
- Guest display sleep/no-scanout reconnect is handled by cached frame fallback
  in `gvt-streamd` plus wakeup and automatic return to live DMABUF frames.
- The old in-QEMU encoder/RTP sender has been removed; `current-detach` is now
  the future mainline shape to merge into `current`.

Start with [SERVER.md](SERVER.md).
