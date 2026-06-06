#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=/dev/null
source "$SCRIPT_DIR/gvt-direct-env.sh"

CLIENT_HOST=${1:-${CLIENT_HOST:-}}
PORT=${PORT:-5004}
WIDTH=${WIDTH:-1920}
HEIGHT=${HEIGHT:-1200}
FPS=${FPS:-60}
BITRATE_KBPS=${BITRATE_KBPS:-12000}
KEYFRAME_PERIOD=$(( FPS > 1 ? FPS / 2 : 1 ))
FEC_PERCENTAGE=${FEC_PERCENTAGE:-20}
FEC_PERCENTAGE_IMPORTANT=${FEC_PERCENTAGE_IMPORTANT:-60}

if [ -z "$CLIENT_HOST" ] && [ -n "${SSH_CONNECTION:-}" ]; then
  CLIENT_HOST=$(printf '%s\n' "$SSH_CONNECTION" | awk '{print $1}')
fi

if [ -z "$CLIENT_HOST" ]; then
  echo "usage: $0 <client-ip>" >&2
  echo "or set CLIENT_HOST=<client-ip>" >&2
  exit 2
fi

echo "Streaming VAAPI H.264 RTP test pattern to ${CLIENT_HOST}:${PORT}"
echo "Mode: ${WIDTH}x${HEIGHT}@${FPS}, bitrate=${BITRATE_KBPS} kbps, keyint=${KEYFRAME_PERIOD}, fec=${FEC_PERCENTAGE}/${FEC_PERCENTAGE_IMPORTANT}"

exec gst-launch-1.0 -e -v \
  videotestsrc is-live=true pattern=ball \
  ! "video/x-raw,format=NV12,width=${WIDTH},height=${HEIGHT},framerate=${FPS}/1" \
  ! queue leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 \
  ! vaapih264enc rate-control=cbr bitrate="$BITRATE_KBPS" keyframe-period="$KEYFRAME_PERIOD" max-bframes=0 refs=1 cabac=false aud=true \
  ! h264parse config-interval=1 \
  ! rtph264pay pt=96 ssrc=2222 config-interval=1 mtu=1000 \
  ! rtpulpfecenc pt=122 percentage="$FEC_PERCENTAGE" percentage-important="$FEC_PERCENTAGE_IMPORTANT" multipacket=true \
  ! udpsink host="$CLIENT_HOST" port="$PORT" sync=false async=false
