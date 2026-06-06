#!/usr/bin/env bash
set -euo pipefail

QEMU_LOG=/root/qemu_cmd/win10-direct-shadowfb-nodisplay.log
MONITOR_SOCK=/root/qemu_cmd/win10-monitor.sock
QMP_SOCK=/root/qemu_cmd/win10-qmp.sock
TAP_IF=tap-win10
BR_IF=br0
VM_DISK=/root/qemu_cmd/archive/gvtg-spice-net-audio-20260530-1209/win10-gvtg-spice-net-audio.qcow2
QEMU_BIN=/usr/local/src/project/qemu/build/qemu-system-x86_64
VGPU_SYSFS=/sys/bus/pci/devices/0000:00:02.0/f8cd7bd7-eabf-4d0b-ab00-d899e4107ae7

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
export LIBVA_DRIVER_NAME=iHD
export LIBVA_DRIVERS_PATH=/usr/lib64/dri:/usr/local/lib64/dri

nohup "$QEMU_BIN" \
  --nodefaults -enable-kvm -cpu host -m 4096 -smp 4 -boot order=c \
  -display none \
  -device vfio-pci-nohotplug,sysfsdev="$VGPU_SYSFS",display=off,x-igd-opregion=on \
  -hda "$VM_DISK" \
  -netdev tap,id=net0,ifname="$TAP_IF",script=no,downscript=no \
  -device e1000e,netdev=net0,mac=52:54:00:10:00:88 \
  -k en-us -device qemu-xhci -device usb-tablet \
  -monitor unix:"$MONITOR_SOCK",server,nowait \
  -qmp unix:"$QMP_SOCK",server,nowait \
  >"$QEMU_LOG" 2>&1 &
echo $! >/root/qemu_cmd/win10-direct-shadowfb.pid
sleep 3
if ! kill -0 "$(cat /root/qemu_cmd/win10-direct-shadowfb.pid)" 2>/dev/null; then
  echo "QEMU exited during startup. Last log lines:" >&2
  tail -140 "$QEMU_LOG" >&2 || true
  exit 1
fi

echo "QEMU direct-shadowfb nodisplay started, pid=$(cat /root/qemu_cmd/win10-direct-shadowfb.pid), disk=$VM_DISK, log=$QEMU_LOG, qmp=$QMP_SOCK"
