#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  gvt-qemu-win10.sh --client CLIENT_IP [--port 5004] [--spice-port 5900]
                   [--input-port 5905] [--codec h264] [--mode realtime|powersave]

Starts the current Windows 10 GVT-g desktop with one user-facing command.
The display backend is still QEMU gvt-stream; this wrapper only hides the
environment variables and legacy diagnostic script details.
EOF
}

CLIENT_HOST=""
VIDEO_PORT=5004
SPICE_PORT=5900
INPUT_PORT=5905
CODEC=h264
MODE=realtime
RUN_SCRIPT=${GVT_QEMU_RUN_SCRIPT:-/root/qemu_cmd/run-win10-gvt-stream-diag.sh}

while [ $# -gt 0 ]; do
  case "$1" in
    --client|--client-host)
      CLIENT_HOST=${2:-}
      shift 2
      ;;
    --port|--video-port)
      VIDEO_PORT=${2:-}
      shift 2
      ;;
    --spice-port)
      SPICE_PORT=${2:-}
      shift 2
      ;;
    --input-port)
      INPUT_PORT=${2:-}
      shift 2
      ;;
    --codec)
      CODEC=${2:-}
      shift 2
      ;;
    --mode)
      MODE=${2:-}
      shift 2
      ;;
    --run-script)
      RUN_SCRIPT=${2:-}
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [ -z "$CLIENT_HOST" ]; then
  echo "--client is required for the current RTP transport" >&2
  usage >&2
  exit 2
fi

case "$MODE" in
  realtime)
    export GVT_STREAM_CAPTURE_MS=17
    export GVT_STREAM_IDLE_CAPTURE_MS=17
    export GVT_STREAM_IDLE_AFTER_MS=0
    export GVT_STREAM_IDLE_PROBE_MS=0
    ;;
  powersave)
    export GVT_STREAM_CAPTURE_MS=17
    export GVT_STREAM_IDLE_CAPTURE_MS=66
    export GVT_STREAM_IDLE_AFTER_MS=1500
    export GVT_STREAM_IDLE_PROBE_MS=500
    export GVT_STREAM_IDLE_CHANGED_PPM=3000
    export GVT_STREAM_IDLE_PIXEL_DELTA=8
    ;;
  *)
    echo "--mode must be realtime or powersave" >&2
    exit 2
    ;;
esac

case "$CODEC" in
  h264|h265|hevc) ;;
  *)
    echo "--codec must be h264 or h265" >&2
    exit 2
    ;;
esac

export GVT_STREAM_RTP_HOST="$CLIENT_HOST"
export GVT_STREAM_RTP_PORT="$VIDEO_PORT"
export GVT_STREAM_RTP_FEC=0
export GVT_STREAM_RTP_FEC_IMPORTANT=0
export GVT_STREAM_INPUT_HOST=0.0.0.0
export GVT_STREAM_INPUT_PORT="$INPUT_PORT"
export GVT_STREAM_SPICE_PORT="$SPICE_PORT"
export GVT_STREAM_SPICE_AUDIO_PORT="$SPICE_PORT"
export GVT_STREAM_VIDEO_CODEC="$CODEC"
export GVT_STREAM_ENCODE_PATH=${GVT_STREAM_ENCODE_PATH:-dmabuf}
export GVT_STREAM_REFRESH_MS=${GVT_STREAM_REFRESH_MS:-17}
export GVT_STREAM_ENCODE_FPS=${GVT_STREAM_ENCODE_FPS:-59}
export GVT_STREAM_ENCODE_BITRATE=${GVT_STREAM_ENCODE_BITRATE:-18000}
export GVT_STREAM_ENCODE_KEYINT=${GVT_STREAM_ENCODE_KEYINT:-59}
export GVT_STREAM_IMPORT_TEST=${GVT_STREAM_IMPORT_TEST:-0}

exec "$RUN_SCRIPT"
