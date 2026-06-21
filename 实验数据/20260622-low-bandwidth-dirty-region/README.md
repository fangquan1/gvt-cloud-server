# 2026-06-22 low-bandwidth dirty-region foundation

## 目标

为 VAAPI H.265 低带宽远程画面传输打基础，优先覆盖静态背景加小区域高频变化场景：

- 在 QEMU `gvt-stream` 侧做服务端脏区域检测。
- 首帧后静态帧不再重复送入 `gvt-streamd` 编码。
- 小脏区帧标记为 partial candidate，并让 `gvt-streamd` 动态降低 VAAPI 编码 bitrate。
- 全屏/大面积变化在 1 帧内标记为 global，并保持短 burst 回到全码率。

## 参考实现结论

Sunshine 侧主要依赖捕获后端提供 damage/dirty 信息，例如 PipeWire 的
`SPA_META_VideoDamage`、Wayland screencopy damage 回调等；编码器本身仍以完整
picture 为主要输入。当前 GVT-g 链路没有客户端区域合成协议，因此本轮先复刻服务端
damage 分类与编码策略基础，不伪造客户端无法理解的局部 RTP 负载。

## 改动摘要

- IPC 升级到 v2，新增 dirty mode、dirty rect、dirty ppm、background sequence 和
  low-bandwidth flag。
- QEMU `gvt-stream` 新增 `GVT_STREAM_LOW_BANDWIDTH=1` 模式：
  - 每帧通过已有 EGL readback surface 做块级 diff。
  - 默认 16x16 block、像素阈值 8。
  - 无变化帧标记 static 并跳过 streamd 投喂。
  - 小变化标记 partial candidate。
  - 大变化标记 global，并用 `GVT_STREAM_DIRTY_GLOBAL_BURST_FRAMES=2` 保持全局模式。
- `gvt-streamd` 消费 dirty metadata：
  - partial candidate 时把 VAAPI encoder bitrate 调到 still bitrate。
  - `GVT_STREAM_LOW_BANDWIDTH_ROI=1` 时，partial candidate 只把 dirty rect
    作为 `GstVideoMeta` 编码窗口送入 VAAPI encoder。
  - full/global 时立即切回基准 bitrate。
  - 增加 dirty 分类、bitrate-change、RTP bytes、ROI frame 和静态跳帧观测日志。
- `gvt-qm-run` 和示例配置暴露低带宽及 ROI 实验参数。
- `patches/qemu-gvt-stream.patch` 已同步更新。

## 当前边界

服务端已经具备 opt-in ROI 编码路径：开启 `GVT_STREAM_LOW_BANDWIDTH_ROI=1`
后，partial candidate 会编码成 dirty-rectangle-sized H.265 picture。当前普通
viewer 仍然是直接 D3D11 sink，没有背景缓存/合成层，因此默认保持
`LOW_BANDWIDTH_ROI=0`；真正作为用户可用路径还需要新增客户端 compositor。

## 验证

本地：

```bash
git diff --check
bash -n scripts/gvt-qm-run scripts/build-gvt-streamd deploy/install-gvt-qm.sh
```

远端临时编译，不覆盖运行环境：

```bash
ssh root@192.168.0.188 "rm -rf /tmp/gvt-lowbw-build && mkdir -p /tmp/gvt-lowbw-build/src/qemu /tmp/gvt-lowbw-build/src/gvt-streamd /tmp/gvt-lowbw-build/scripts"
scp src/qemu/gvt-stream-ipc.h root@192.168.0.188:/tmp/gvt-lowbw-build/src/qemu/gvt-stream-ipc.h
scp src/gvt-streamd/gvt-streamd.c root@192.168.0.188:/tmp/gvt-lowbw-build/src/gvt-streamd/gvt-streamd.c
scp scripts/build-gvt-streamd root@192.168.0.188:/tmp/gvt-lowbw-build/scripts/build-gvt-streamd
ssh root@192.168.0.188 "chmod +x /tmp/gvt-lowbw-build/scripts/build-gvt-streamd && cd /tmp/gvt-lowbw-build && bash scripts/build-gvt-streamd /tmp/gvt-lowbw-build/gvt-streamd-lowbw"
```

结果：

- `gvt-streamd-lowbw` 编译通过，输出 `/tmp/gvt-lowbw-build/gvt-streamd-lowbw`。
- 使用远端 QEMU `compile_commands.json` 的真实编译参数，`gvt-stream.c` 编译到
  `/tmp/gvt-lowbw-build/gvt-stream.o` 通过。
- ROI 版 streamd 临时编译通过，输出 `/tmp/gvt-roi-build/gvt-streamd-roi`。
- ROI 版 QEMU `gvt-stream.c` 使用远端真实编译参数编译到
  `/tmp/gvt-roi-build/gvt-stream.o` 通过。
- `gst-inspect-1.0 vaapih265enc` 确认 `bitrate`、`keyframe-period` 和
  `rate-control` 属性存在且可写。

未执行：

- 未替换远端正在运行的 QEMU/source tree。
- 未重启 VM。
- 未做秒表小区域带宽实测和全屏切换实测。
- 未实现客户端背景缓存/局部帧合成。

## Commit

- `427a043 Add low bandwidth dirty-region foundation`
- `353596e Add opt-in ROI dirty-region encoding`

## 下一步

- 在远端可中断窗口部署 `LOW_BANDWIDTH=1`，跑秒表/静态桌面/小窗口切全屏三组带宽对比。
- 根据日志中的 `dirty=partial|global|static` 和 `dirty_ppm` 调整阈值。
- 在客户端增加背景缓存和 ROI H.265 小帧合成路径，使
  `LOW_BANDWIDTH_ROI=1` 可以作为正常观看路径使用。
