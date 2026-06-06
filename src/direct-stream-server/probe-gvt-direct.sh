#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=/dev/null
source "$SCRIPT_DIR/gvt-direct-env.sh"

echo "== kernel =="
hostname
uname -a
cat /proc/cmdline || true

echo
echo "== gvt shadowfb =="
find /sys/kernel/debug/dri -maxdepth 5 -type f \( -name 'shadow_fb*' -o -name status \) -print 2>/dev/null | sort || true
for f in /sys/kernel/debug/dri/*/gvt/vgpu*/shadow_fb; do
  [ -r "$f" ] || continue
  echo "-- $f"
  cat "$f"
done

echo
echo "== vaapi =="
vainfo --display drm --device /dev/dri/renderD128 2>&1 | sed -n '1,120p' || true
gst-inspect-1.0 vaapih264enc 2>&1 | sed -n '1,120p' || true

echo
echo "== network =="
ip -br link
for dev in /sys/class/net/*; do
  name=$(basename "$dev")
  [ "$name" = lo ] && continue
  echo "-- $name"
  ethtool -i "$name" 2>/dev/null | sed -n '1,20p' || true
  ethtool -k "$name" 2>/dev/null | grep -E 'scatter-gather|tcp-segmentation|generic-segmentation|tx-checksumming|highdma|hw-tc-offload' || true
done

