#!/usr/bin/env bash
set -euo pipefail

DIR=/root/qemu_cmd/multivm
CONFIG="$DIR/config.env"
OUTPUTD_SRC="$DIR/gvt-outputd.c"
OUTPUTD_BIN="$DIR/gvt-outputd"
OUTPUTD_SOCK=/run/gvt-outputd.sock
OUTPUTD_LOG="$DIR/gvt-outputd.log"
OUTPUTD_PID="$DIR/gvt-outputd.pid"
OUTPUTD_STATUS=/run/gvt-outputd.status
WEB_SCRIPT="$DIR/gvt-output-web.py"
WEB_LOG="$DIR/gvt-output-web.log"
WEB_PID="$DIR/gvt-output-web.pid"
WEB_PORT=${GVT_MULTI_WEB_PORT:-8098}
BASE_DISK=/root/qemu_cmd/archive/gvtg-spice-net-audio-20260530-1209/win10-gvtg-spice-net-audio.qcow2
MDEV_PARENT=/sys/devices/pci0000:00/0000:00:02.0
MDEV_TYPE=i915-GVTg_V5_8
MDEV_CREATE="$MDEV_PARENT/mdev_supported_types/$MDEV_TYPE/create"
QEMU_BIN=/usr/local/src/project/qemu/build/qemu-system-x86_64
BR_IF=br0

mkdir -p "$DIR"

new_web_password() {
    python3 - <<'PY'
import secrets
import string
alphabet = string.ascii_letters + string.digits
print("".join(secrets.choice(alphabet) for _ in range(16)))
PY
}

create_config_if_needed() {
    if [ -f "$CONFIG" ]; then
        return
    fi

    cat >"$CONFIG" <<EOF
VM1_UUID=$(uuidgen)
VM2_UUID=$(uuidgen)
WEB_PASSWORD=$(new_web_password)
EOF
}

ensure_config_defaults() {
    if ! grep -q '^WEB_PASSWORD=' "$CONFIG" 2>/dev/null; then
        printf 'WEB_PASSWORD=%s\n' "$(new_web_password)" >>"$CONFIG"
    fi
}

load_config() {
    create_config_if_needed
    ensure_config_defaults
    # shellcheck disable=SC1090
    source "$CONFIG"
}

stop_one() {
    local name=$1
    local qmp="$DIR/$name-qmp.sock"
    local pidfile="$DIR/$name.pid"

    if [ -S "$qmp" ]; then
        python3 - "$qmp" <<'PY' || true
import json
import socket
import sys

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(3)
sock.connect(sys.argv[1])
try:
    sock.recv(4096)
except Exception:
    pass
sock.sendall((json.dumps({"execute": "qmp_capabilities", "id": 1}) + "\r\n").encode())
try:
    sock.recv(4096)
except Exception:
    pass
sock.sendall((json.dumps({"execute": "quit", "id": 2}) + "\r\n").encode())
sock.close()
PY
    fi

    sleep 2
    if [ -f "$pidfile" ]; then
        local pid
        pid=$(cat "$pidfile" 2>/dev/null || true)
        if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            sleep 2
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        fi
    fi
    rm -f "$pidfile" "$qmp" "$DIR/$name-monitor.sock"
}

stop_all() {
    stop_web
    stop_one vm1
    stop_one vm2
    stop_outputd
    for tap in tap-win10a tap-win10b; do
        ip link set "$tap" down 2>/dev/null || true
        ip tuntap del dev "$tap" mode tap 2>/dev/null || true
    done
}

stop_outputd() {
    if [ -f "$OUTPUTD_PID" ]; then
        local pid
        pid=$(cat "$OUTPUTD_PID" 2>/dev/null || true)
        if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            sleep 1
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        fi
    fi
    rm -f "$OUTPUTD_PID" "$OUTPUTD_SOCK" "$OUTPUTD_STATUS"
}

stop_web() {
    if [ -f "$WEB_PID" ]; then
        local pid
        pid=$(cat "$WEB_PID" 2>/dev/null || true)
        if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            sleep 1
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        fi
    fi
    rm -f "$WEB_PID"
}

remove_mdev_if_present() {
    local uuid=$1
    if [ -e "/sys/bus/mdev/devices/$uuid/remove" ]; then
        echo 1 >"/sys/bus/mdev/devices/$uuid/remove"
    fi
}

ensure_two_vgpus() {
    load_config
    stop_all

    for dev in /sys/bus/mdev/devices/*; do
        [ -e "$dev" ] || continue
        local real
        real=$(readlink -f "$dev")
        case "$real" in
            "$MDEV_PARENT"/*)
                remove_mdev_if_present "$(basename "$dev")"
                ;;
        esac
    done

    sleep 1
    echo "$VM1_UUID" >"$MDEV_CREATE"
    echo "$VM2_UUID" >"$MDEV_CREATE"
}

create_overlays() {
    qemu-img info "$BASE_DISK" >/dev/null
    if [ ! -f "$DIR/win10-vm1.qcow2" ]; then
        qemu-img create -f qcow2 -F qcow2 -b "$BASE_DISK" "$DIR/win10-vm1.qcow2"
    fi
    if [ ! -f "$DIR/win10-vm2.qcow2" ]; then
        qemu-img create -f qcow2 -F qcow2 -b "$BASE_DISK" "$DIR/win10-vm2.qcow2"
    fi
}

build_outputd() {
    if [ ! -f "$OUTPUTD_SRC" ]; then
        echo "missing $OUTPUTD_SRC" >&2
        exit 1
    fi
    cc -O2 -Wall -Wextra -o "$OUTPUTD_BIN" "$OUTPUTD_SRC" $(pkg-config --cflags --libs libdrm)
}

detect_connector() {
    local forced=${GVT_MULTI_KMS_CONNECTOR:-}
    local path

    if [ -n "$forced" ]; then
        echo "$forced"
        return
    fi

    for path in /sys/class/drm/card0-*/status; do
        [ -e "$path" ] || continue
        if [ "$(cat "$path" 2>/dev/null || true)" = connected ]; then
            basename "$(dirname "$path")" | sed 's/^card0-//'
            return
        fi
    done
}

start_outputd() {
    local connector=$1

    build_outputd
    if [ -f "$OUTPUTD_PID" ] && kill -0 "$(cat "$OUTPUTD_PID")" 2>/dev/null; then
        echo "gvt-outputd already running pid=$(cat "$OUTPUTD_PID")"
        return
    fi
    rm -f "$OUTPUTD_SOCK"
    if [ -n "$connector" ]; then
        nohup "$OUTPUTD_BIN" --socket "$OUTPUTD_SOCK" --connector "$connector" --status "$OUTPUTD_STATUS" >"$OUTPUTD_LOG" 2>&1 &
    else
        nohup "$OUTPUTD_BIN" --socket "$OUTPUTD_SOCK" --status "$OUTPUTD_STATUS" >"$OUTPUTD_LOG" 2>&1 &
    fi
    echo $! >"$OUTPUTD_PID"
    sleep 1
    if ! kill -0 "$(cat "$OUTPUTD_PID")" 2>/dev/null; then
        echo "gvt-outputd exited during startup" >&2
        tail -80 "$OUTPUTD_LOG" >&2 || true
        exit 1
    fi
    echo "gvt-outputd started pid=$(cat "$OUTPUTD_PID") socket=$OUTPUTD_SOCK connector=${connector:-auto}"
}

start_web() {
    load_config
    if [ ! -f "$WEB_SCRIPT" ]; then
        echo "missing $WEB_SCRIPT" >&2
        exit 1
    fi
    if [ -f "$WEB_PID" ] && kill -0 "$(cat "$WEB_PID")" 2>/dev/null; then
        echo "gvt-output-web already running pid=$(cat "$WEB_PID") url=http://192.168.0.188:$WEB_PORT/"
        return
    fi
    nohup python3 "$WEB_SCRIPT" \
        --host 0.0.0.0 \
        --port "$WEB_PORT" \
        --socket "$OUTPUTD_SOCK" \
        --status "$OUTPUTD_STATUS" \
        --config "$CONFIG" \
        >"$WEB_LOG" 2>&1 &
    echo $! >"$WEB_PID"
    sleep 1
    if ! kill -0 "$(cat "$WEB_PID")" 2>/dev/null; then
        echo "gvt-output-web exited during startup" >&2
        tail -80 "$WEB_LOG" >&2 || true
        exit 1
    fi
    echo "gvt-output-web started pid=$(cat "$WEB_PID") url=http://192.168.0.188:$WEB_PORT/"
}

setup_tap() {
    local tap=$1

    ip link set "$tap" down 2>/dev/null || true
    ip tuntap del dev "$tap" mode tap 2>/dev/null || true
    ip tuntap add dev "$tap" mode tap
    ip link set "$tap" master "$BR_IF"
    ip link set "$tap" up
}

start_one() {
    local name=$1
    local uuid=$2
    local disk=$3
    local tap=$4
    local mac=$5
    local spice_port=$6
    local input_port=$7
    local kms_connector=$8
    local log="$DIR/$name.log"
    local qmp="$DIR/$name-qmp.sock"
    local mon="$DIR/$name-monitor.sock"
    local pidfile="$DIR/$name.pid"
    local upper
    local mode_var
    local vm_mode

    if [ ! -e "/sys/bus/mdev/devices/$uuid" ]; then
        echo "missing mdev $uuid for $name" >&2
        exit 1
    fi
    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "$name already running pid=$(cat "$pidfile")"
        return
    fi

    setup_tap "$tap"
    rm -f "$qmp" "$mon"
    upper=$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')
    mode_var="${upper}_MODE"
    vm_mode="${!mode_var:-physical}"

    (
        export LD_LIBRARY_PATH=/usr/local/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
        export GST_PLUGIN_PATH=/usr/local/lib64/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}
        export GST_PLUGIN_SYSTEM_PATH_1_0=/usr/local/lib64/gstreamer-1.0:/usr/lib64/gstreamer-1.0
        export LIBVA_DRIVER_NAME=iHD
        export LIBVA_DRIVERS_PATH=/usr/lib64/dri:/usr/local/lib64/dri
        export GVT_STREAM_REFRESH_MS=16
        export GVT_STREAM_REPORT_MS=1000
        export GVT_STREAM_VERBOSE=0
        export GVT_STREAM_KMS_CONNECTOR="$kms_connector"
        export GVT_STREAM_KMS_DEVICE=/dev/dri/card0
        export GVT_STREAM_KMS_ATOMIC=1
        export GVT_STREAM_PUBLISH_SOCKET="$OUTPUTD_SOCK"
        export GVT_STREAM_SOURCE_ID="$name"
        export GVT_STREAM_INPUT_HOST=0.0.0.0
        export GVT_STREAM_INPUT_PORT="$input_port"
        export GVT_STREAM_CAPTURE_MAX=0
        export GVT_STREAM_IMPORT_TEST=0
        export GVT_STREAM_ENCODE_PATH=dmabuf
        export GVT_STREAM_CAPTURE_MS=16
        export GVT_STREAM_IDLE_CHANGED_PPM=3000
        export GVT_STREAM_IDLE_PIXEL_DELTA=8
        case "$vm_mode" in
            realtime)
                export GVT_STREAM_IDLE_CAPTURE_MS=16
                export GVT_STREAM_IDLE_AFTER_MS=0
                export GVT_STREAM_IDLE_PROBE_MS=0
                ;;
            power_save)
                export GVT_STREAM_IDLE_CAPTURE_MS=66
                export GVT_STREAM_IDLE_AFTER_MS=1500
                export GVT_STREAM_IDLE_PROBE_MS=500
                ;;
            physical|"")
                export GVT_STREAM_IDLE_CAPTURE_MS=16
                export GVT_STREAM_IDLE_AFTER_MS=0
                export GVT_STREAM_IDLE_PROBE_MS=0
                ;;
            *)
                echo "unknown mode $vm_mode for $name" >&2
                exit 2
                ;;
        esac
        unset GVT_STREAM_RTP_HOST GVT_STREAM_RTP_PORT GVT_AUDIO_RTP_HOST GVT_AUDIO_RTP_PORT
        unset GVT_STREAM_CAPTURE_DIR GVT_STREAM_ENCODE_FILE

        nohup "$QEMU_BIN" \
            --nodefaults -enable-kvm -cpu host -m 2048 -smp 2 -boot order=c \
            -name "$name" \
            -display gvt-stream,rendernode=/dev/dri/renderD128,codec=h264 \
            -spice port="$spice_port",addr=0.0.0.0,disable-ticketing=on,agent-mouse=off,playback-compression=off,streaming-video=off,image-compression=off,disable-copy-paste=on,disable-agent-file-xfer=on,display=none \
            -device vfio-pci-nohotplug,sysfsdev="/sys/bus/pci/devices/0000:00:02.0/$uuid",display=on,x-igd-opregion=on,ramfb=on \
            -hda "$disk" \
            -netdev tap,id=net0,ifname="$tap",script=no,downscript=no \
            -device e1000e,netdev=net0,mac="$mac" \
            -k en-us -device qemu-xhci -device usb-tablet -device usb-kbd \
            -audiodev spice,id=audio0 \
            -device ich9-intel-hda \
            -device hda-duplex,audiodev=audio0 \
            -monitor unix:"$mon",server,nowait \
            -qmp unix:"$qmp",server,nowait \
            >"$log" 2>&1 &
        echo $! >"$pidfile"
    )

    sleep 3
    if ! kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "$name exited during startup; last log lines:" >&2
        tail -160 "$log" >&2 || true
        exit 1
    fi
    echo "$name started pid=$(cat "$pidfile") disk=$disk spice=$spice_port input=$input_port mode=$vm_mode kms=${kms_connector:-none}"
}

set_vm_mode() {
    local name=${1:-}
    local mode=${2:-}

    case "$name" in
        vm1|vm2) ;;
        *)
            echo "set-mode requires vm1 or vm2" >&2
            exit 2
            ;;
    esac
    case "$mode" in
        realtime|power_save|physical) ;;
        *)
            echo "set-mode requires realtime, power_save or physical" >&2
            exit 2
            ;;
    esac
    load_config
    python3 - "$CONFIG" "$name" "$mode" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
key = sys.argv[2].upper() + "_MODE"
mode = sys.argv[3]
lines = path.read_text().splitlines() if path.exists() else []
out = []
seen = False
for line in lines:
    if line.startswith(key + "="):
        out.append(f"{key}={mode}")
        seen = True
    else:
        out.append(line)
if not seen:
    out.append(f"{key}={mode}")
path.write_text("\n".join(out) + "\n")
PY
    echo "$name mode=$mode"
}

start_all() {
    load_config
    create_overlays
    local connector

    connector=$(detect_connector)
    if [ -z "$connector" ]; then
        echo "no connected KMS connector found; starting both VMs without physical output"
    else
        echo "using KMS connector $connector for vm1"
    fi
    start_outputd "$connector"

    start_one vm1 "$VM1_UUID" "$DIR/win10-vm1.qcow2" tap-win10a 52:54:00:10:01:88 5900 5905 ""
    start_one vm2 "$VM2_UUID" "$DIR/win10-vm2.qcow2" tap-win10b 52:54:00:10:02:88 5901 5906 ""
    start_web
}

start_vm() {
    local name=${1:-}
    load_config
    create_overlays
    case "$name" in
        vm1)
            start_outputd "$(detect_connector)"
            start_one vm1 "$VM1_UUID" "$DIR/win10-vm1.qcow2" tap-win10a 52:54:00:10:01:88 5900 5905 ""
            ;;
        vm2)
            start_outputd "$(detect_connector)"
            start_one vm2 "$VM2_UUID" "$DIR/win10-vm2.qcow2" tap-win10b 52:54:00:10:02:88 5901 5906 ""
            ;;
        *)
            echo "start-vm requires vm1 or vm2" >&2
            exit 2
            ;;
    esac
}

stop_vm() {
    local name=${1:-}
    case "$name" in
        vm1|vm2)
            stop_one "$name"
            ;;
        *)
            echo "stop-vm requires vm1 or vm2" >&2
            exit 2
            ;;
    esac
}

restart_vm() {
    local name=${1:-}
    stop_vm "$name"
    start_vm "$name"
}

select_state() {
    local key=$1
    local source=${2:-}

    case "$source" in
        vm1|vm2) ;;
        *)
            echo "$key requires vm1 or vm2" >&2
            exit 2
            ;;
    esac
    load_config
    python3 - "$CONFIG" "$key" "$source" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
key = sys.argv[2].upper().replace("-", "_")
source = sys.argv[3]
lines = path.read_text().splitlines() if path.exists() else []
out = []
seen = False
for line in lines:
    if line.startswith(key + "="):
        out.append(f"{key}={source}")
        seen = True
    else:
        out.append(line)
if not seen:
    out.append(f"{key}={source}")
path.write_text("\n".join(out) + "\n")
PY
    echo "$key=$source"
}

select_source() {
    local source=${1:-}
    if [ -z "$source" ]; then
        echo "select requires source name, for example vm1 or vm2" >&2
        exit 2
    fi
    python3 - "$OUTPUTD_SOCK" "$source" <<'PY'
import socket
import struct
import sys

magic = 0x4756544f
version = 1
msg_type = 2
source = sys.argv[2].encode("ascii", "ignore")[:31]
source = source + b"\0" * (32 - len(source))
msg = struct.pack("<IIIIIIIIQQ32s", magic, version, msg_type,
                  0, 0, 0, 0, 0, 0, 0, source)
sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
sock.sendto(msg, sys.argv[1])
PY
}

status_all() {
    load_config
    echo "--- config ---"
    cat "$CONFIG"
    echo "--- mdevs ---"
    find /sys/bus/mdev/devices -maxdepth 1 -mindepth 1 -type l -printf '%f -> %l\n' 2>/dev/null || true
    for t in "$MDEV_PARENT"/mdev_supported_types/i915-GVTg_*; do
        [ -d "$t" ] || continue
        printf '%s available=' "$(basename "$t")"
        cat "$t/available_instances" 2>/dev/null || true
    done
    echo "--- qemu ---"
    ps -eo pid,etime,%cpu,%mem,rss,cmd | awk '/[q]emu-system-x86_64/ && /gvt-stream/ {print}'
    echo "--- outputd ---"
    ps -eo pid,etime,%cpu,%mem,rss,cmd | awk '/[g]vt-outputd/ {print}'
    tail -40 "$OUTPUTD_LOG" 2>/dev/null || true
    echo "--- web ---"
    ps -eo pid,etime,%cpu,%mem,rss,cmd | awk '/[g]vt-output-web.py/ {print}'
    tail -20 "$WEB_LOG" 2>/dev/null || true
    echo "web url=http://192.168.0.188:$WEB_PORT/ password is stored in $CONFIG"
    echo "--- outputd status ---"
    cat "$OUTPUTD_STATUS" 2>/dev/null || true
    echo "--- ports ---"
    ss -ltnp | grep -E ':(5900|5901|5905|5906)\b' || true
    echo "--- connectors ---"
    for path in /sys/class/drm/card0-*/status; do
        [ -e "$path" ] || continue
        printf '%s status=' "$(basename "$(dirname "$path")")"
        cat "$path" 2>/dev/null || true
    done
    echo "--- vm1 log ---"
    grep -nE 'gvt-stream: (listener|scanout-dmabuf|update-stats)|gvt-stream-kms|failed|error' "$DIR/vm1.log" 2>/dev/null | tail -50 || true
    echo "--- vm2 log ---"
    grep -nE 'gvt-stream: (listener|scanout-dmabuf|update-stats)|gvt-stream-kms|failed|error' "$DIR/vm2.log" 2>/dev/null | tail -50 || true
    echo "--- memory ---"
    free -h
}

case "${1:-status}" in
    setup)
        load_config
        ensure_two_vgpus
        create_overlays
        status_all
        ;;
    start)
        start_all
        status_all
        ;;
    select)
        select_source "${2:-}"
        status_all
        ;;
    start-vm)
        start_vm "${2:-}"
        status_all
        ;;
    stop-vm)
        stop_vm "${2:-}"
        ;;
    restart-vm)
        restart_vm "${2:-}"
        status_all
        ;;
    set-mode)
        set_vm_mode "${2:-}" "${3:-}"
        ;;
    input-select)
        select_state input_source "${2:-}"
        ;;
    audio-select)
        select_state audio_source "${2:-}"
        ;;
    web-start)
        start_web
        status_all
        ;;
    outputd-restart)
        connector=$(detect_connector)
        stop_outputd
        start_outputd "$connector"
        status_all
        ;;
    web-stop)
        stop_web
        ;;
    web-status)
        load_config
        ps -eo pid,etime,%cpu,%mem,rss,cmd | awk '/[g]vt-output-web.py/ {print}'
        tail -40 "$WEB_LOG" 2>/dev/null || true
        echo "web url=http://192.168.0.188:$WEB_PORT/ password is stored in $CONFIG"
        ;;
    stop)
        stop_all
        ;;
    status)
        status_all
        ;;
    *)
        echo "usage: $0 {setup|start|stop|status|select|start-vm|stop-vm|restart-vm|set-mode|input-select|audio-select|outputd-restart|web-start|web-stop|web-status}" >&2
        exit 2
        ;;
esac
