#!/usr/bin/env bash
set -euo pipefail

QEMU_LOG=/root/qemu_cmd/win10-gvt-stream-diag.log
MONITOR_SOCK=/root/qemu_cmd/win10-gvt-stream-monitor.sock
QMP_SOCK=/root/qemu_cmd/win10-gvt-stream-qmp.sock
SPICE_AUDIO_PORT=${GVT_STREAM_SPICE_AUDIO_PORT:-${GVT_STREAM_SPICE_PORT:-5900}}
VIDEO_CODEC=${GVT_STREAM_VIDEO_CODEC:-h264}
RTP_HOST=${GVT_STREAM_RTP_HOST:-}
RTP_PORT=${GVT_STREAM_RTP_PORT:-}
TAP_IF=tap-win10
BR_IF=br0
VM_DISK=/root/qemu_cmd/archive/gvtg-spice-net-audio-20260530-1209/win10-gvtg-spice-net-audio.qcow2
QEMU_BIN=/usr/local/src/project/qemu/build/qemu-system-x86_64

for pid in $(ps -eo pid=,comm= | awk '$2 ~ /^qemu-system/ {print $1}'); do
  if tr '\0' ' ' </proc/$pid/cmdline | grep -q "$VM_DISK\|/root/vm/win10.qcow2"; then
    echo "win10 qemu is already running:" >&2
    tr '\0' ' ' </proc/$pid/cmdline >&2
    echo >&2
    exit 1
  fi
done

ip link set "$TAP_IF" down 2>/dev/null || true
ip tuntap del dev "$TAP_IF" mode tap 2>/dev/null || true
ip tuntap add dev "$TAP_IF" mode tap
ip link set "$TAP_IF" master "$BR_IF"
ip link set "$TAP_IF" up
rm -f "$MONITOR_SOCK" "$QMP_SOCK"

export LD_LIBRARY_PATH=/usr/local/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export GST_PLUGIN_PATH=/usr/local/lib64/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}
export GST_PLUGIN_SYSTEM_PATH_1_0=/usr/local/lib64/gstreamer-1.0:/usr/lib64/gstreamer-1.0
export LIBVA_DRIVER_NAME=iHD
export LIBVA_DRIVERS_PATH=/usr/lib64/dri:/usr/local/lib64/dri
# GVT-stream diagnostic profile: QEMU logs VFIO/GVT-g DMABUF scanout/update callbacks.
export GVT_STREAM_REFRESH_MS=${GVT_STREAM_REFRESH_MS:-16}
export GVT_STREAM_REPORT_MS=${GVT_STREAM_REPORT_MS:-1000}
export GVT_STREAM_VERBOSE=${GVT_STREAM_VERBOSE:-0}
export GVT_STREAM_SPICE_PORT="$SPICE_AUDIO_PORT"

EXTRA_INPUT_ARGS=()
input_idx=0
for evdev in ${GVT_STREAM_EVDEV_INPUTS:-}; do
  EXTRA_INPUT_ARGS+=( -object "input-linux,id=gvtinput${input_idx},evdev=${evdev},grab_all=on,repeat=on" )
  input_idx=$((input_idx + 1))
done

DISPLAY_OPTS="gvt-stream,rendernode=/dev/dri/renderD128,codec=${VIDEO_CODEC}"
if [ -n "$RTP_HOST" ] && [ -n "$RTP_PORT" ]; then
  DISPLAY_OPTS="${DISPLAY_OPTS},host=${RTP_HOST},port=${RTP_PORT}"
fi

nohup "$QEMU_BIN" \
  --nodefaults -enable-kvm -cpu host -m 4096 -smp 4 -boot order=c \
  -display "$DISPLAY_OPTS" \
  -spice port="$SPICE_AUDIO_PORT",addr=0.0.0.0,disable-ticketing=on,agent-mouse=off,playback-compression=off,streaming-video=off,image-compression=off,disable-copy-paste=on,disable-agent-file-xfer=on,display=none \
  -device vfio-pci-nohotplug,sysfsdev=/sys/bus/pci/devices/0000:00:02.0/f8cd7bd7-eabf-4d0b-ab00-d899e4107ae7,display=on,x-igd-opregion=on,ramfb=on \
  -hda "$VM_DISK" \
  -netdev tap,id=net0,ifname="$TAP_IF",script=no,downscript=no \
  -device e1000e,netdev=net0,mac=52:54:00:10:00:88 \
  -k en-us -device qemu-xhci -device usb-tablet -device usb-kbd \
  "${EXTRA_INPUT_ARGS[@]}" \
  -audiodev spice,id=audio0 \
  -device ich9-intel-hda \
  -device hda-duplex,audiodev=audio0 \
  -monitor unix:"$MONITOR_SOCK",server,nowait \
  -qmp unix:"$QMP_SOCK",server,nowait \
  >"$QEMU_LOG" 2>&1 &
echo $! >/root/qemu_cmd/win10-gvt-stream-diag.pid
sleep 3
if ! kill -0 "$(cat /root/qemu_cmd/win10-gvt-stream-diag.pid)" 2>/dev/null; then
  echo "QEMU exited during startup. Last log lines:" >&2
  tail -140 "$QEMU_LOG" >&2 || true
  exit 1
fi

echo "QEMU gvt-stream diag started, pid=$(cat /root/qemu_cmd/win10-gvt-stream-diag.pid), disk=$VM_DISK, log=$QEMU_LOG, qmp=$QMP_SOCK, spice_audio=0.0.0.0:$SPICE_AUDIO_PORT"
