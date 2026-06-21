# GVT Cloud Server Notes

## Purpose

This repository provides the server-side QEMU patch for the current GVT-g cloud
desktop route plus a minimal systemd runner for launching the patched QEMU. It
does not ship the old web control plane, kernel shadow framebuffer code, or
physical display switcher.

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

## Install GStreamer VAAPI Plugins

First install the distro packages that provide the core GStreamer, VAAPI, DRM,
GBM, EGL, and SPICE development files:

```bash
dnf install -y git gcc gcc-c++ make ninja-build meson pkgconf-pkg-config \
  glib2-devel pixman-devel zlib-devel libdrm libdrm-devel \
  mesa-libgbm mesa-libgbm-devel mesa-libEGL mesa-libEGL-devel \
  libepoxy-devel spice-server-devel \
  gstreamer1 gstreamer1-devel \
  gstreamer1-plugins-base gstreamer1-plugins-base-devel \
  gstreamer1-plugins-good gstreamer1-plugins-bad-free \
  gstreamer1-plugins-bad-free-devel gstreamer1-libav \
  libva libva-devel libva-utils intel-gmmlib
```

On the current openEuler test host the distro core GStreamer package is
`1.22.5`, but the distro `gstreamer1-plugins-bad-free` package is older. The
working deployment therefore installs the matching `gst-plugins-bad` codec
parser library and `gstreamer-vaapi` plugin from source into `/usr/local`.

Use the same GStreamer minor version as `gst-inspect-1.0 --version` reports:

```bash
export GST_VER=1.22.5
export DEPS=/usr/local/src/project/deps
mkdir -p "$DEPS"
cd "$DEPS"

wget -c "https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-${GST_VER}.tar.xz"
tar -xf "gst-plugins-bad-${GST_VER}.tar.xz"
cd "gst-plugins-bad-${GST_VER}"
meson setup build-codecparsers \
  -Dauto_features=disabled \
  -Dvideoparsers=enabled \
  -Dexamples=disabled \
  -Dtests=disabled \
  -Dintrospection=disabled \
  -Dnls=disabled \
  -Dprefix=/usr/local \
  -Dlibdir=lib64
ninja -C build-codecparsers
ninja -C build-codecparsers install
ldconfig

cd "$DEPS"
wget -c "https://gstreamer.freedesktop.org/src/gstreamer-vaapi/gstreamer-vaapi-${GST_VER}.tar.xz"
tar -xf "gstreamer-vaapi-${GST_VER}.tar.xz"
cd "gstreamer-vaapi-${GST_VER}"
meson setup build-vaapi \
  -Dexamples=disabled \
  -Dtests=disabled \
  -Dprefix=/usr/local \
  -Dlibdir=lib64
ninja -C build-vaapi
ninja -C build-vaapi install
ldconfig
```

Verify that the VAAPI encoders are found from `/usr/local`:

```bash
export LD_LIBRARY_PATH=/usr/local/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export GST_PLUGIN_PATH=/usr/local/lib64/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}
export GST_PLUGIN_SYSTEM_PATH_1_0=/usr/local/lib64/gstreamer-1.0:/usr/lib64/gstreamer-1.0
export LIBVA_DRIVER_NAME=iHD
export LIBVA_DRIVERS_PATH=/usr/lib64/dri:/usr/local/lib64/dri

gst-inspect-1.0 vaapih264enc | grep -E 'Long-name|Filename|Version'
gst-inspect-1.0 vaapih265enc | grep -E 'Long-name|Filename|Version'
vainfo --display drm --device /dev/dri/renderD128
```

Expected current plugin identity:

```text
Filename                 /usr/local/lib64/gstreamer-1.0/libgstvaapi.so
Version                  1.22.5
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
install -m 0644 /path/to/gvt-cloud-server/src/qemu/gvt-stream-ipc.h \
  /usr/local/src/project/qemu/ui/gvt-stream-ipc.h
ninja -C /usr/local/src/project/qemu/build qemu-system-x86_64
```

The patch adds:

- `ui/gvt-stream.c`
- `ui/gvt-stream-ipc.h`
- `ui/meson.build` registration for the `gvt-stream` UI module
- `qapi/ui.json` support for `-display gvt-stream,...`

Build the standalone encoder helper:

```bash
cd /usr/local/src/project/gvt-cloud-server
bash scripts/build-gvt-streamd /usr/local/bin/gvt-streamd
```

## Run

Minimal command shape:

```bash
export GVT_STREAM_REFRESH_MS=17
export GVT_STREAM_REPORT_MS=1000
export GVT_STREAM_VERBOSE=0
export GVT_STREAM_SPICE_PORT=5900
export GVT_STREAM_STARTUP_PUMP_MS=1500
export GVT_STREAMD_SOCKET=/root/qemu_cmd/win10-gvt-streamd.sock

/usr/local/src/project/qemu/build/qemu-system-x86_64 \
  --nodefaults -enable-kvm -cpu host -m 4096 -smp 4 -boot order=c \
  -display gvt-stream,rendernode=/dev/dri/renderD128,codec=h265,port=5004 \
  -spice port=5900,addr=0.0.0.0,disable-ticketing=on,agent-mouse=off,playback-compression=off,streaming-video=off,image-compression=off,display=none \
  -device vfio-pci-nohotplug,sysfsdev=/sys/bus/pci/devices/0000:00:02.0/<VGPU_UUID>,display=on,x-igd-opregion=on,ramfb=on \
  -hda /path/to/win10.qcow2 \
  -netdev tap,id=net0,ifname=tap-win10,script=no,downscript=no \
  -device e1000e,netdev=net0,mac=52:54:00:10:00:88 \
  -k en-us -device qemu-xhci -device usb-tablet -device usb-kbd \
  -device virtio-serial-pci \
  -chardev socket,path=/root/qemu_cmd/win10-gvt-stream-qga.sock,server=on,wait=off,id=qga0 \
  -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 \
  -chardev spicevmc,id=vdagent0,name=vdagent \
  -device virtserialport,chardev=vdagent0,name=com.redhat.spice.0 \
  -audiodev spice,id=audio0,timer-period=5000,out.frequency=48000,out.channels=2,out.format=s16,out.buffer-length=100000 \
  -device ich9-intel-hda \
  -device hda-duplex,audiodev=audio0 \
  -monitor unix:/root/qemu_cmd/win10-gvt-stream-monitor.sock,server,nowait \
  -qmp unix:/root/qemu_cmd/win10-gvt-stream-qmp.sock,server,nowait
```

Default ports are derived from the display/control port:

- video/control TCP or RTP target: `5004`
- SPICE audio/session TCP: `5900`
- native input TCP: `5905`

## Optional Systemd Runner

The repository also ships a minimal systemd wrapper for running a QEMU VM as a
service. This is the small `gvt-qm` path from the earlier deployment work, not
the removed Python web/API control plane.

Install it on the host:

```bash
cd /usr/local/src/project/gvt-cloud-server
bash deploy/install-gvt-qm.sh
```

Installed files:

- `/usr/local/bin/gvt-qm`: operator command wrapper.
- `/usr/local/sbin/gvt-qm-run`: launches QEMU from one VM config.
- `/etc/systemd/system/gvt-qemu@.service`: systemd template unit.
- `/etc/gvt-qm/win10.conf`: example VM config copied on first install.

Edit `/etc/gvt-qm/win10.conf` before starting. At minimum set:

```bash
QEMU_BIN=/usr/local/src/project/qemu/build/qemu-system-x86_64
VM_DISK=/root/qemu_cmd/win10-gvtg.qcow2
VGPU_UUID=e582c6f5-e5cd-4a8e-9443-55a013e9f193
RENDER_NODE=/dev/dri/renderD128
TAP_IF=tap-win10
BR_IF=br0
VIDEO_PORT=5004
SPICE_PORT=5900
INPUT_PORT=5905
CODEC=h265
```

Then manage the VM:

```bash
gvt-qm start win10
gvt-qm status win10
gvt-qm logs win10
gvt-qm stop win10
```

Equivalent raw systemd commands:

```bash
systemctl start gvt-qemu@win10.service
systemctl status gvt-qemu@win10.service
journalctl -u gvt-qemu@win10.service -f
systemctl stop gvt-qemu@win10.service
```

The runner configures the same runtime environment used above:
`LD_LIBRARY_PATH`, `GST_PLUGIN_PATH`, `GST_PLUGIN_SYSTEM_PATH_1_0`,
`LIBVA_DRIVER_NAME=iHD`, `LIBVA_DRIVERS_PATH`, `GVT_STREAM_*` ports, capture
timing, and FEC disabled by default. `gvt-qm-run` always starts
`/usr/local/bin/gvt-streamd`, waits for `GVT_STREAMD_SOCKET`, exports the
socket path to QEMU, monitors the helper, and removes the helper process/socket
when QEMU exits. The helper log defaults to
`/root/qemu_cmd/<vm-id>-gvt-streamd.log`.

## Encoding And Control Behavior

`gvt-stream` listens to QEMU's GL/DMABUF display callbacks. QEMU forwards
scanout DMABUF fds to `gvt-streamd`; the encoder and RTP sender are no longer
linked into QEMU:

```text
GVT-g VFIO DMABUF -> QEMU gvt-stream -> Unix SOCK_SEQPACKET/SCM_RIGHTS
  -> gvt-streamd appsrc -> VAAPI postproc/encoder -> RTP
```

The v2 IPC still carries one-plane `XR24/BGRx` DMABUF frames, plus optional
dirty-region metadata for low-bandwidth experiments. CPU raw frame IPC and the
previous in-QEMU encoder/RTP path are intentionally removed.

The backend defaults to H.265/HEVC and still accepts H.264 when the client asks
for it. Useful runtime knobs:

- `GVT_STREAM_VIDEO_CODEC=h264|h265`
- `GVT_STREAM_ENCODE_BITRATE=18000`
- `GVT_STREAM_ENCODE_RATE_CONTROL=cbr`
- `GVT_STREAM_RTP_FEC=0`
- `GVT_STREAM_RTP_FEC_IMPORTANT=0`
- `GVT_STREAMD_SOCKET=/root/qemu_cmd/<vm-id>-gvt-streamd.sock`
- `GVT_STREAM_IDLE_CAPTURE_MS`, `GVT_STREAM_IDLE_AFTER_MS`,
  `GVT_STREAM_IDLE_PROBE_MS` for optional power-saving behavior

Experimental low-bandwidth knobs:

- `GVT_STREAM_LOW_BANDWIDTH=1` enables per-frame EGL readback and dirty-region
  classification in QEMU.
- `GVT_STREAM_LOW_BANDWIDTH_ROI=1` makes `gvt-streamd` encode only the dirty
  rectangle for partial frames. This produces a small H.265 picture and requires
  the client ROI compositor path to be enabled.
- `GVT_STREAM_DIRTY_BLOCK_SIZE=16` controls the diff grid size.
- `GVT_STREAM_DIRTY_PIXEL_DELTA=8` is the per-channel noise threshold.
- `GVT_STREAM_DIRTY_PARTIAL_MAX_PPM=150000` marks small changes as partial
  candidates.
- `GVT_STREAM_DIRTY_GLOBAL_MIN_PPM=350000` immediately switches full-screen
  changes back to global mode.
- `GVT_STREAM_DIRTY_GLOBAL_BURST_FRAMES=2` keeps global mode for a short burst
  after a large change.
- `GVT_STREAM_ENCODE_STILL_BITRATE` optionally overrides the low-bandwidth
  small-dirty-region bitrate. If unset, QEMU uses about 35% of the requested
  bitrate when low-bandwidth mode is enabled.

With `GVT_STREAM_LOW_BANDWIDTH_ROI=0`, low-bandwidth behavior is compatible with
the existing RTP viewer: static frames are not re-submitted after the first full
frame, partial candidates are encoded as full-size H.265 pictures at reduced
VAAPI bitrate, and global changes switch back to full bitrate immediately.
With `GVT_STREAM_LOW_BANDWIDTH_ROI=1`, partial candidates are encoded as
dirty-rectangle-sized H.265 pictures by pointing `GstVideoMeta` at the subregion
inside the source DMABUF. QEMU also sends per-frame ROI metadata over the active
control TCP connection, for example `type=frame`, `mode=partial|global|full`,
`x/y/w/h`, source `width/height`, `dirty_ppm`, and `background` sequence. The
client can use that metadata to composite dirty-rectangle pictures over its
cached full-frame background. Leave ROI disabled unless the matching client
compositor is enabled for the experiment.

When the client disconnects, the active control connection closes and QEMU
asks `gvt-streamd` to stop encoding. When the guest display sleeps or scanout
disappears, QEMU sends a no-scanout notification, sends a wakeup pulse, and
`gvt-streamd` can continue serving its cached last frame until live DMABUF frames
return.

## Verification

Expected log lines:

```text
gvt-stream-input: listening on 0.0.0.0:5905
gvt-stream-control: listening on 0.0.0.0:5004
gvt-qm-run: streamd_socket=...
gvt-stream-external: connected socket=...
gvt-stream-external: frame-sent ...
gvt-streamd: encode-start ...
gvt-streamd: dmabuf-push-ok ...
gvt-stream: update-stats ... encoded=0 ... external_sent=... streamd_encoded=...
```

External-mode negative checks:

```text
gvt-stream: cached-frame-save reason=live-refresh
gvt-stream: encode-start ... path=dmabuf
```

Those lines should not appear in the current streamd-only QEMU path.

Reconnect/sleep validation should show one or more of:

```text
gvt-stream-external: no-scanout ...
gvt-stream-control: wakeup requested from suspended VM
gvt-stream-control: wakeup input pulse sent reason=no-scanout
gvt-streamd: no-scanout ...
```

After client stop/disconnect, `streamd_encoded=` in later `update-stats` lines
should stop increasing.

## Troubleshooting

- If `-display gvt-stream` is unknown, the patch was not applied or QAPI files
  were not regenerated by the QEMU build.
- If `gvt-streamd` dependencies are missing, `scripts/build-gvt-streamd` will
  fail while resolving `gstreamer-*` or `libdrm`.
- If VAAPI encode fails, verify `vainfo --display drm --device /dev/dri/renderD128`
  and `gst-inspect-1.0 vaapih265enc`.
- If the guest shows no live frame after sleep, check for no-scanout, streamd
  cached-frame, and wakeup log lines first, then confirm the Windows guest
  actually resumed.
- Do not mix this route with the removed legacy experiments. The current branch
  is intentionally only the QEMU `gvt-stream` path.
