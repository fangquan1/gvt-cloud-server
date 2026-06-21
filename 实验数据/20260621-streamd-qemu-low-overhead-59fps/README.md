# 2026-06-21 streamd/QEMU 低开销优化 59fps 验证

## 本轮目标

- 降低 host/QEMU 侧 gvt-stream 和外置 `gvt-streamd` 在 59fps/H.265 路线下的 CPU/日志开销。
- 能安全降低内存的改动才保留；会破坏 59fps 稳定性的 pipeline/caps 改动不合入。
- 全部验证只使用 59fps，不使用 30fps 结论。

## 改动摘要

- QEMU `gvt-stream`
  - `gvt_stream_effective_capture_ms()` 纳入 `encode_fps` 对应的最小帧间隔，低 fps 配置不会继续按 17ms 过度 pump。
  - startup pump interval 使用 capture interval 与 startup interval 的较大值。
  - 统计 capture interval skip 次数为 `external_throttled`，便于观察节流效果。
  - external frame/no-scanout 日志从每 60 帧降为每 300 帧，前 5 帧仍保留。
- `gvt-streamd`
  - 成功推帧、drop、no-scanout 日志从每 60 帧降为每 300 帧，前 5 帧仍保留。
  - `streamd_send_stats()` 仍保持每 60 帧回传，避免 QEMU 诊断和测试统计变粗。

## 未保留的尝试

- QEMU 设置 `GVT_STREAM_IPC_FLAG_DMABUF_CAPS_FEATURE`：59fps smoke 触发 GStreamer `not-negotiated`，并出现 fd 增长风险。
- VAAPI low-power/quality/mbbrc/cpb 参数：59fps smoke 触发 `not-negotiated`。
- `gvt-streamd` queue `max-size-buffers=1`：59fps 下不稳定或低 FPS。

## 构建和部署

- QEMU 构建：
  - `ninja -C /usr/local/src/project/qemu/build qemu-system-x86_64`
- streamd 构建：
  - `cd /usr/local/src/project/gvt-cloud-server && bash scripts/build-gvt-streamd /usr/local/bin/gvt-streamd`
- 远端运行入口：
  - `/usr/local/sbin/gvt-qm-run win10-external`
- 远端运行配置：
  - `/etc/gvt-qm/win10-external.conf`

## 运行环境

- 服务端：`192.168.0.188`
- QEMU：`/usr/local/src/project/qemu/build/qemu-system-x86_64`
- streamd：`/usr/local/bin/gvt-streamd`
- 编码：H.265/HEVC
- 分辨率：1920x1200
- 目标帧率：59fps
- 目标码率：18Mbps
- RTP/video/control port：5004
- SPICE port：5900
- native input port：5905

## 最终全量测试

- 测试时间：2026-06-21 20:51:04 - 20:55:35
- 客户端命令：
  - `powershell.exe -ExecutionPolicy Bypass -File .\test-tools\run-gvt-full-test.ps1 -BatchMode -SkipInstall -SkipReboot -DesktopTimeoutSec 240 -MarkerStageTimeoutSec 150 -StageHeartbeatSec 10 -SmokeDurationSec 20 -SmokeWarmupSec 4 -LatencyTrials 3 -OutDir test_output\data\streamd-qemu-low-overhead-59fps-final3-20260621`
- 输出目录：
  - `C:\job\gvt-cloud-current\client\test_output\data\streamd-qemu-low-overhead-59fps-final3-20260621\full-20260621-205103`
- HTML 报告：
  - `C:\job\gvt-cloud-current\client\test_output\data\streamd-qemu-low-overhead-59fps-final3-20260621\full-20260621-205103\report.html`

## 最终结果

- 总体结果：通过
- 客户端壳启动：通过，命令行确认 `--stream-fps "59"`、`--video-codec "h265"`、18Mbps。
- 视频 smoke：通过
  - client depay avg：59.12 fps
  - client decode avg：58.08 fps
  - server fps avg：58.79 fps
  - encode failures：0
  - stream-control startup：515 ms
  - gst-ready：437 ms
- 音频：通过
  - clipped：0%
  - dropout windows：0%
  - pop candidates：0
  - severe pop candidates：0
- AV sync：通过
  - nearest paired events：9
  - audio-minus-video avg：-168.4 ms
  - relative drift max：54.75 ms
- 输入到画面延迟：通过
  - median：1170.1 ms

## 开销采样

- 采样文件：`sampler-final3.csv`
- 采样窗口：2026-06-21 20:51:04 - 20:55:35
- 采样字段：CPU%、RSS、PSS、PrivateDirty、线程数、fd 数，每 1 秒采样一次。
- 注意：QEMU CPU/RSS/PSS 是整个 QEMU 进程，包含 guest vCPU/设备模拟/内存，不等同于纯 gvt-stream 开销。

| role | samples | CPU avg | CPU p95 | CPU max | RSS avg | RSS max | PSS avg | PSS max | PrivateDirty avg | threads max | fd max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| streamd | 259 | 5.36% | 6.70% | 7.64% | 90.8 MiB | 96.2 MiB | 85.2 MiB | 90.6 MiB | 54.0 MiB | 5 | 19 |
| qemu | 259 | 21.72% | 98.95% | 309.16% | 4261.8 MiB | 4266.7 MiB | 4251.3 MiB | 4256.2 MiB | 4147.6 MiB | 14 | 121 |

## 日志摘录

- `streamd-tail.log`
  - 日志已变为 `#=8100, 8400, 8700...`，确认成功推帧日志每 300 帧输出一次。
  - final3 全量期间 `dropped=0 failures=0`。
- `qemu-tail.log`
  - `fps=58.82`
  - `external_sent` 持续增长
  - `external_send_failures=0`
  - `streamd_failures=0`
  - `external_throttled` 可见，说明 capture interval skip 已被统计。

## 产物

- `sampler-final3.csv`
- `sampler-summary-final3.json`
- `streamd-tail.log`
- `qemu-tail.log`
- full report：`C:\job\gvt-cloud-current\client\test_output\data\streamd-qemu-low-overhead-59fps-final3-20260621\full-20260621-205103\report.md`

## Commit

- 待回填

## 结论

- 59fps/H.265 全量通过，QEMU 和 streamd 的帧级日志写入频率已从约每秒一次降低到约每 5 秒一次。
- streamd 59fps 编码期间 CPU 平均约 5.36%，p95 约 6.70%，峰值 7.64%。
- streamd 活跃编码期间 RSS/PSS 仍主要由 GStreamer/VAAPI pipeline 工作集决定；尝试降低 pipeline 缓冲的改动在 59fps 下不稳定，因此没有保留。
