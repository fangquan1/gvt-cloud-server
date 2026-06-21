# 2026-06-21 streamd dynamic FPS pacing

## Goal

Move user-requested stream FPS pacing out of QEMU and into `gvt-streamd`, so
changing FPS through the client START request does not require QEMU-side capture
pacing changes.

## Commit

- `2307999 Move stream fps pacing into streamd`

## Code Changes

- `src/qemu/gvt-stream.c`
  - Removed the previous `encode_fps` contribution to
    `gvt_stream_effective_capture_ms()`.
  - Removed startup-pump clamping against FPS-derived capture interval.
  - QEMU now keeps capture pacing at the configured capture interval and only
    forwards START parameters to streamd.
- `src/gvt-streamd/gvt-streamd.c`
  - Added per-START frame interval state based on `fps`.
  - Added streamd-side frame pacing before appsrc push.
  - Live frames that arrive before the next target frame time update the cached
    last frame, close their fd, and increment `dropped_frames` without entering
    the encoder.

## Build And Deploy

Remote host: `192.168.0.188`

Commands:

```bash
scp src/qemu/gvt-stream.c root@192.168.0.188:/usr/local/src/project/qemu/ui/gvt-stream.c
scp src/gvt-streamd/gvt-streamd.c root@192.168.0.188:/usr/local/src/project/gvt-cloud-server/src/gvt-streamd/gvt-streamd.c
ssh root@192.168.0.188 "cd /usr/local/src/project/gvt-cloud-server && bash scripts/build-gvt-streamd /usr/local/bin/gvt-streamd"
ssh root@192.168.0.188 "ninja -C /usr/local/src/project/qemu/build qemu-system-x86_64"
```

Build result:

- `gvt-streamd` rebuilt to `/usr/local/bin/gvt-streamd` with no warnings after
  format-cast cleanup.
- QEMU rebuilt successfully: `ninja ... qemu-system-x86_64`.

Runtime reload:

```bash
ssh root@192.168.0.188 "kill <old-gvt-qm-run-pid>; nohup /usr/local/sbin/gvt-qm-run win10-external >/root/qemu_cmd/win10-external-runner.out 2>&1 &"
```

New runtime processes observed:

- runner: `/usr/local/sbin/gvt-qm-run win10-external`
- streamd: `/usr/local/bin/gvt-streamd --socket /root/qemu_cmd/win10-gvt-streamd.sock`
- QEMU: `/usr/local/src/project/qemu/build/qemu-system-x86_64 ... -display gvt-stream,...`

## Validation

Sent direct control START/STOP requests to `192.168.0.188:5004`.

### 30 FPS / 2 Mbps

START response:

```text
{"ok":true,"video_udp":5004,"spice_tcp":5900,"input_tcp":5905,"width":1920,"height":1200,"fps":30,"bitrate":2000,"codec":"h265"}
```

QEMU log excerpt:

```text
gvt-stream-control: stream target=127.0.0.1:5004 codec=h265 fps=30 bitrate=2000 keyint=30 capture_ms=17
gvt-stream-external: start target=127.0.0.1:5004 codec=h265 fps=30 bitrate=2000 keyint=30
gvt-stream: update-stats ... external_sent=302 ... streamd_encoded=154 ... bitrate=2000 target_bitrate=2000 capture_ms=17
```

streamd log excerpt:

```text
gvt-streamd: start #1 target=127.0.0.1:5004 codec=h265 fps=30 interval_ns=33333333 bitrate=2000 keyint=30
gvt-streamd: frame-drop #=1 source=live target_fps=30 ...
gvt-streamd: dmabuf-push-ok #=120 source=live ... dropped=115 failures=0
gvt-streamd: stop #1 frames=154 cached=0 dropped=148 failures=0
```

Interpretation:

- QEMU continued to capture/feed at `capture_ms=17`.
- streamd accepted START `fps=30 bitrate=2000`.
- streamd encoded roughly half of the incoming frames and dropped the rest.

### 59 FPS / 2 Mbps

START response:

```text
{"ok":true,"video_udp":5004,"spice_tcp":5900,"input_tcp":5905,"width":1920,"height":1200,"fps":59,"bitrate":2000,"codec":"h265"}
```

streamd log excerpt:

```text
gvt-streamd: start #2 target=127.0.0.1:5004 codec=h265 fps=59 interval_ns=16949152 bitrate=2000 keyint=59
gvt-streamd: dmabuf-push-ok #=180 source=live ... dropped=148 failures=0
gvt-streamd: dmabuf-push-ok #=240 source=live ... dropped=148 failures=0
gvt-streamd: dmabuf-push-ok #=300 source=live ... dropped=148 failures=0
gvt-streamd: stop #2 frames=342 cached=0 dropped=148 failures=0
```

Interpretation:

- The cumulative dropped counter did not increase during the 59 FPS run.
- QEMU still reported `capture_ms=17`.
- FPS pacing is now streamd-side and START-driven.

## Notes

- The deploy itself required one QEMU restart to load the reverted QEMU binary.
  After this change, changing the client FPS should not require QEMU restart;
  it should be applied through the next START/reconnect to streamd.
- `systemctl restart gvt-qemu@win10-external.service` expands the instance to
  `win10/external` with the current unit. The live VM was recovered and is
  running through the normal direct entrypoint
  `/usr/local/sbin/gvt-qm-run win10-external`.
