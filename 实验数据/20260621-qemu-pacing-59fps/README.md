# 2026-06-21 恢复 QEMU 侧帧率 pacing 验证

## 目标

- 回滚 `2307999 Move stream fps pacing into streamd` 的行为变化。
- 恢复由 QEMU 按 `encode_fps` 控制 capture/send 间隔，避免 QEMU 高频投喂后再由 `gvt-streamd` 丢帧。
- 验证仍使用 59fps/H.265，不使用 30fps 结论。

## 对比

### 回滚前

- QEMU `gvt_stream_effective_capture_ms()` 只看 `capture_ms` / idle capture。
- QEMU 仍把 `fps` 发给 `gvt-streamd`。
- `gvt-streamd` 维护：
  - `frame_interval_ns`
  - `next_frame_due_ns`
  - `dropped_frames`
  - `streamd_should_push_frame()`
- 帧进入 streamd 后按 PTS/monotonic time 决定是否推给 GStreamer，早到的帧会在 streamd 内关闭 fd 并丢掉。

### 回滚后

- QEMU `gvt_stream_effective_capture_ms()` 重新把 `encode_fps` 换算成最小 capture interval：
  - 59fps 对应约 17ms。
- startup pump 在显式 interval 存在时也取 `effective_capture_ms()` 的较大值。
- `gvt-streamd` 不再做 fps pacing，不再记录 `dropped_frames`；收到 QEMU 投喂的有效帧就进入编码链路。

## 代码提交

- `5206281 Move stream pacing back to QEMU`

## 构建和部署

- QEMU：
  - `ninja -C /usr/local/src/project/qemu/build qemu-system-x86_64`
- streamd：
  - `cd /usr/local/src/project/gvt-cloud-server && bash scripts/build-gvt-streamd /usr/local/bin/gvt-streamd`
- 远端运行：
  - `/usr/local/sbin/gvt-qm-run win10-external`

## 最终 59fps 全量测试

- 时间：2026-06-21 23:41:01 - 23:45:23
- 命令：
  - `powershell.exe -ExecutionPolicy Bypass -File .\test-tools\run-gvt-full-test.ps1 -BatchMode -SkipInstall -SkipReboot -DesktopTimeoutSec 240 -MarkerStageTimeoutSec 150 -StageHeartbeatSec 10 -SmokeDurationSec 20 -SmokeWarmupSec 4 -LatencyTrials 3 -OutDir test_output\data\qemu-pacing-59fps-final-20260621`
- 输出目录：
  - `C:\job\gvt-cloud-current\client\test_output\data\qemu-pacing-59fps-final-20260621\full-20260621-234100`
- HTML 报告：
  - `C:\job\gvt-cloud-current\client\test_output\data\qemu-pacing-59fps-final-20260621\full-20260621-234100\report.html`

## 结果

- 总体结果：通过
- client shell：通过，命令行确认 `--stream-fps "59"` / H.265 / 18Mbps。
- 视频 smoke：通过
  - client depay avg：59.32 fps
  - client decode avg：58.2 fps
  - server fps avg：58.8 fps
  - encode failures：0
  - stream-control startup：500 ms
  - gst-ready：406 ms
- 音频：通过
  - clipped：0%
  - dropout windows：0%
  - pop candidates：0
  - severe pop candidates：0
- AV sync：通过
  - nearest paired events：8
  - relative drift max：45.21 ms
- 输入到画面延迟：通过
  - median：1188.9 ms

## 日志确认

- `gvt-streamd` 推帧日志变回：
  - `cached=0 failures=0`
- 不再出现 streamd pacing 版本里的：
  - `interval_ns=...`
  - `dropped=...`
  - `frame-drop #=...`
- QEMU 日志显示：
  - `capture_ms=17`
  - `external_sent=30050`
  - `streamd_encoded=30050`
  - `external_send_failures=0`
  - `streamd_failures=0`

## 结论

- 已恢复到 QEMU 侧按 fps 投喂帧、streamd 只编码发送的节奏。
- 59fps/H.265 全量测试通过，可以让用户继续主观观察快速动态画面和马赛克情况。
