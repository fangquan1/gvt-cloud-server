# GVT Cloud Server

Minimal server-side patch set for the GVT-g cloud desktop project.

This repository now keeps only the host/QEMU display path:

- `src/qemu/gvt-stream.c`: the current QEMU `gvt-stream` display backend.
- `patches/qemu-gvt-stream.patch`: the patch to apply to a QEMU source tree.
- `SERVER.md`: build, run, verification, and troubleshooting notes.

The old kernel shadow framebuffer route, direct-stream RTP streamer, physical
output daemon, Python control plane, multi-VM helper scripts, and Windows batch
launchers were removed from `current`. They remain available in Git history if
they are ever needed for reference.

Current baseline:

- GVT-g VFIO display DMABUF is consumed inside QEMU.
- VAAPI/GStreamer encodes H.264 or H.265 RTP video.
- SPICE is retained for audio/session, not primary video.
- QEMU native TCP input is handled by the `gvt-stream` backend.
- Client disconnect stops encoding.
- Guest display sleep/no-scanout reconnect is handled by cached frame fallback
  plus wakeup and automatic return to DMABUF encoding.

Start with [SERVER.md](SERVER.md).
