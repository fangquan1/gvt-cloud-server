# GVT Cloud Server Notes

## Purpose

This repository provides the server-side QEMU patch for the current GVT-g cloud
desktop route. It does not ship a VM manager, web control plane, kernel
shadow framebuffer code, or physical display switcher.

The core implementation is `src/qemu/gvt-stream.c`. The distributable QEMU patch
is `patches/qemu-gvt-stream.patch`.

## Tested Baseline

- Host OS: openEuler on Intel iGPU hardware with GVT-g enabled.
- QEMU source: around `10.2.90`; current deployed path is usually
  `/usr/local/src/project/qemu`.
- Guest: Windows 10 with Intel GVT-g vGPU, QEMU Guest Agent, and network driver.
- Render node: `/dev/dri/renderD128`.
- Encoder: VAAPI with Intel iHD driver.
- Video path: `-display gvt-stream,...`.
- Audio/session path: SPICE server, display disabled.

## Dependencies

Install the normal QEMU build toolchain plus these feature dependencies:

```bash
dnf install -y git gcc gcc-c++ make ninja-build meson pkgconf-pkg-config \
  glib2-devel pixman-devel zlib-devel libdrm-devel mesa-libgbm-devel \
  mesa-libEGL-devel libepoxy-devel spice-server-devel \
  gstreamer1-devel gstreamer1-plugins-base-devel \
  gstreamer1-plugins-bad-free-devel libva-devel libva-utils
```

The exact package names can vary by distro. The important pkg-config libraries
are `gstreamer-1.0`, `gstreamer-app-1.0`, `gstreamer-allocators-1.0`,
`gstreamer-video-1.0`, `libdrm`, OpenGL/EGL/GBM, Pixman, and SPICE server.

Runtime environment:

```bash
export LD_LIBRARY_PATH=/usr/local/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export GST_PLUGIN_PATH=/usr/local/lib64/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}
export GST_PLUGIN_SYSTEM_PATH_1_0=/usr/local/lib64/gstreamer-1.0:/usr/lib64/gstreamer-1.0
export LIBVA_DRIVER_NAME=iHD
export LIBVA_DRIVERS_PATH=/usr/lib64/dri:/usr/local/lib64/dri
```

## Version Compatibility

The current working host uses GStreamer `1.22.5` and loads the VAAPI plugin from
`/usr/local/lib64/gstreamer-1.0/libgstvaapi.so`. Keep the GStreamer headers,
libraries, and plugins on one matching stack. In practice this means:

- Build QEMU against the same GStreamer version that will be used at runtime.
- Keep `LD_LIBRARY_PATH`, `GST_PLUGIN_PATH`, and `GST_PLUGIN_SYSTEM_PATH_1_0`
  pointed at the same prefix, currently `/usr/local/lib64`.
- Do not mix distro GStreamer libraries with a manually installed
  `libgstvaapi.so`, or the reverse.
- Rebuild QEMU after changing the GStreamer, VAAPI, libdrm, or Mesa stack.

Version or prefix mismatches usually show up as `gst-inspect-1.0 vaapih264enc`
not finding the element, plugin load warnings, `encode-pipeline-create-failed`,
or a runtime pipeline that silently falls back away from the expected VAAPI
path.

Quick check on the server:

```bash
export LD_LIBRARY_PATH=/usr/local/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export GST_PLUGIN_PATH=/usr/local/lib64/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}
export GST_PLUGIN_SYSTEM_PATH_1_0=/usr/local/lib64/gstreamer-1.0:/usr/lib64/gstreamer-1.0
export LIBVA_DRIVER_NAME=iHD
export LIBVA_DRIVERS_PATH=/usr/lib64/dri:/usr/local/lib64/dri

gst-inspect-1.0 --version
gst-inspect-1.0 vaapih264enc
gst-inspect-1.0 vaapih265enc
vainfo --display drm --device /dev/dri/renderD128
```

## Apply And Build

From a clean QEMU source tree:

```bash
cd /usr/local/src/project/qemu
git apply /path/to/gvt-cloud-server/patches/qemu-gvt-stream.patch

mkdir -p build
cd build
../configure --target-list=x86_64-softmmu --enable-kvm --enable-opengl --enable-spice
ninja qemu-system-x86_64
```

For an already patched tree, copy only the current backend source and rebuild:

```bash
install -m 0644 /path/to/gvt-cloud-server/src/qemu/gvt-stream.c \
  /usr/local/src/project/qemu/ui/gvt-stream.c
ninja -C /usr/local/src/project/qemu/build qemu-system-x86_64
```

The patch adds:

- `ui/gvt-stream.c`
- `ui/meson.build` registration for the `gvt-stream` UI module
- `qapi/ui.json` support for `-display gvt-stream,...`

## Run

Minimal command shape:

```bash
export GVT_STREAM_REFRESH_MS=17
export GVT_STREAM_REPORT_MS=1000
export GVT_STREAM_VERBOSE=0
export GVT_STREAM_SPICE_PORT=5900
export GVT_STREAM_STARTUP_PUMP_MS=1500
export GVT_STREAM_CACHE_REFRESH_MS=1000

/usr/local/src/project/qemu/build/qemu-system-x86_64 \
  --nodefaults -enable-kvm -cpu host -m 4096 -smp 4 -boot order=c \
  -display gvt-stream,rendernode=/dev/dri/renderD128,codec=h264,port=5004 \
  -spice port=5900,addr=0.0.0.0,disable-ticketing=on,agent-mouse=off,playback-compression=off,streaming-video=off,image-compression=off,disable-copy-paste=on,disable-agent-file-xfer=on,display=none \
  -device vfio-pci-nohotplug,sysfsdev=/sys/bus/pci/devices/0000:00:02.0/<VGPU_UUID>,display=on,x-igd-opregion=on,ramfb=on \
  -hda /path/to/win10.qcow2 \
  -netdev tap,id=net0,ifname=tap-win10,script=no,downscript=no \
  -device e1000e,netdev=net0,mac=52:54:00:10:00:88 \
  -k en-us -device qemu-xhci -device usb-tablet -device usb-kbd \
  -device virtio-serial-pci \
  -chardev socket,path=/root/qemu_cmd/win10-gvt-stream-qga.sock,server=on,wait=off,id=qga0 \
  -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 \
  -audiodev spice,id=audio0 \
  -device ich9-intel-hda \
  -device hda-duplex,audiodev=audio0 \
  -monitor unix:/root/qemu_cmd/win10-gvt-stream-monitor.sock,server,nowait \
  -qmp unix:/root/qemu_cmd/win10-gvt-stream-qmp.sock,server,nowait
```

Default ports are derived from the display/control port:

- video/control TCP or RTP target: `5004`
- SPICE audio/session TCP: `5900`
- native input TCP: `5905`

## Encoding And Control Behavior

`gvt-stream` listens to QEMU's GL/DMABUF display callbacks. The primary path
pushes GVT-g scanout DMABUFs into GStreamer and VAAPI:

```text
GVT-g VFIO DMABUF -> QEMU gvt-stream -> appsrc -> VAAPI postproc/encoder -> RTP
```

The backend supports H.264 by default and H.265/HEVC when requested. Useful
runtime knobs:

- `GVT_STREAM_VIDEO_CODEC=h264|h265`
- `GVT_STREAM_ENCODE_PATH=dmabuf|cpu`
- `GVT_STREAM_ENCODE_BITRATE=18000`
- `GVT_STREAM_ENCODE_RATE_CONTROL=cbr`
- `GVT_STREAM_RTP_FEC=0`
- `GVT_STREAM_RTP_FEC_IMPORTANT=0`
- `GVT_STREAM_IDLE_CAPTURE_MS`, `GVT_STREAM_IDLE_AFTER_MS`,
  `GVT_STREAM_IDLE_PROBE_MS` for optional power-saving behavior

When the client disconnects, the active control connection closes and QEMU
stops encoding. When the guest display sleeps or scanout disappears, QEMU keeps
a cached frame, sends it first on reconnect, sends a wakeup pulse, and switches
back to the live DMABUF path when scanout returns.

## Verification

Expected log lines:

```text
gvt-stream-input: listening on 0.0.0.0:5905
gvt-stream-control: listening on 0.0.0.0:5004
gvt-stream: encode-start ... path=dmabuf
gvt-stream: update-stats ... encode_failures=0
```

Reconnect/sleep validation should show one or more of:

```text
gvt-stream: cached-frame-save ...
gvt-stream: cached-frame-push ...
gvt-stream-control: wakeup requested from suspended VM
gvt-stream-control: wakeup input pulse sent reason=no-scanout
gvt-stream: encode-restart old_path=cpu ... new_path=dmabuf
```

After client stop/disconnect, `encoded=` in later `update-stats` lines should
stop increasing.

## Troubleshooting

- If `-display gvt-stream` is unknown, the patch was not applied or QAPI files
  were not regenerated by the QEMU build.
- If GStreamer dependencies are missing, Meson will fail while resolving
  `gstreamer-*` or `libdrm`.
- If VAAPI encode fails, verify `vainfo --display drm --device /dev/dri/renderD128`
  and `gst-inspect-1.0 vaapih264enc`.
- If the guest shows no live frame after sleep, check for cached-frame and
  wakeup log lines first, then confirm the Windows guest actually resumed.
- Do not mix this route with the removed legacy experiments. The current branch
  is intentionally only the QEMU `gvt-stream` path.
