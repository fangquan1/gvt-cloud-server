# GitHub 开源仓库与分支登记

## 当前 GitHub 状态

- GitHub 账号: `fangquan1`
- 已安装 GitHub CLI: `gh 2.93.0`。
- 已用 `fangquan1` 登录并创建/推送主项目仓库。
- GitHub 上游可直接 fork 的组件已 fork；GitLab 上游组件已在 GitHub 创建同名 mirror/work 仓库。

## 主项目仓库

| 用途 | 目标仓库 | 本地仓库 | 默认开发分支 | 状态 |
| --- | --- | --- | --- | --- |
| 客户端 | `https://github.com/fangquan1/gvt-cloud-client` | `repos/gvt-cloud-client` | `gvt-cloud-client-mvp` | 已推送，当前分支 HEAD 以 GitHub 为准 |
| 服务端 | `https://github.com/fangquan1/gvt-cloud-server` | `repos/gvt-cloud-server` | `gvt-cloud-server-mvp` | 已推送，当前分支 HEAD 以 GitHub 为准 |

后续所有产品代码应优先进入这两个仓库:

- 客户端 UI、Windows 客户端、GStreamer/SPICE glue、输入 overlay、打包脚本进入 `gvt-cloud-client`。
- QEMU 后端补丁管理、`gvt-outputd`、Web/API、VM 启动编排、服务端脚本进入 `gvt-cloud-server`。

## 上游组件 fork 计划

| 组件 | 上游来源 | 目标 fork/镜像 | 当前项目分支 | 当前改动来源 | 说明 |
| --- | --- | --- | --- | --- | --- |
| QEMU | `https://github.com/qemu/qemu` | `https://github.com/fangquan1/qemu` | `gvt-cloud-gvt-stream-20260607` | 远端 `/usr/local/src/project/qemu` 当前 worktree | 已推送，当前提交 `4e364bdc51fc` |
| SPICE server | `https://gitlab.freedesktop.org/spice/spice` | `https://github.com/fangquan1/spice` | `gvt-cloud-spice-h264-20260607` | 远端 `/usr/local/src/project/spice` 当前 worktree | 已推送，当前提交 `b3331bc14431` |
| spice-gtk | `https://gitlab.freedesktop.org/spice/spice-gtk` | `https://github.com/fangquan1/spice-gtk` | `gvt-cloud-client-audio-input-20260607` | `third_party/spice-gtk-v0.37` | 已推送，基于 tag `v0.37` |
| virt-viewer | `https://gitlab.com/virt-viewer/virt-viewer` | `https://github.com/fangquan1/virt-viewer` | `gvt-cloud-client-viewer-20260607` | `third_party/virt-viewer-v11.0` | 已推送，基于 tag `v11.0` |
| Linux kernel | `https://github.com/gregkh/linux` | `https://github.com/fangquan1/linux` | `gvt-cloud-shadowfb-6.6.40-20260607` | 远端 `/usr/src/linux-6.6.40/drivers/gpu/drm/i915/gvt/shadow_fb.*` | 已推送，基于 `v6.6.40`，当前提交 `9632906a7487` |

备注:

- GStreamer 当前作为运行时依赖和 Windows runtime，不建议 fork，除非后续实际修改 GStreamer 插件。
- 如果 GitHub 不能直接 fork GitLab 上游，应在 GitHub 新建同名 mirror 仓库，再添加上游 remote。

## 2026-06-07 远端扫描与补提交记录

- 已连接远端服务器并扫描 `/usr/local/src/project/qemu`、`/usr/local/src/project/spice`、`/usr/src/linux-6.6.40`、`/root/qemu_cmd`、`/root/qemu_cmd/multivm`。
- QEMU fork 已同步远端当前 worktree 改动，包括 `ui/gvt-stream.c`、QEMU/SPICE 显示与音频相关补丁、`qapi` 配置和构建文件，提交 `4e364bdc51fc`。
- SPICE fork 已同步远端 H.264/stream 调参和诊断改动，包括 `server/dcc.cpp`、`server/display-channel.cpp`、`server/gstreamer-encoder.c`、`server/video-stream.cpp`、`server/video-stream.h`，提交 `b3331bc14431`。
- Linux fork 已同步远端当前 `shadow_fb.c/.h` 基线，当前 gvt-stream 路线下默认禁用旧 shadowfb BLT，提交 `9632906a7487`。
- 服务端仓库已同步远端 `run-win10-gvt-stream-diag.sh` 和当前 kernel shadowfb 基线，提交 `898311750924`。
- 服务端仓库中的 `scripts/multivm_remote.sh` 保留公开仓库脱敏版本；远端脚本会直接打印 Web 密码，公开仓库不保留这种输出。
- 未提交远端 `.before-*` 备份文件、运行日志、进程快照、config/env 密码、VM 镜像、构建产物和私有调试输出。

## 需要提交到 QEMU 分支的当前改动

优先顺序:

1. `ui/gvt-stream.c`: 原生 display backend、DMABUF scanout、H.264 RTP、原生输入、实时/节能、publish socket。
2. `ui/meson.build` 或相关构建文件: 注册 `gvt-stream` 后端。
3. 当前启动脚本引用的环境变量说明:
   - `GVT_STREAM_RTP_HOST`
   - `GVT_STREAM_RTP_PORT`
   - `GVT_STREAM_ENCODE_FPS`
   - `GVT_STREAM_CAPTURE_MS`
   - `GVT_STREAM_RTP_FEC`
   - `GVT_STREAM_RTP_FEC_IMPORTANT`
   - `GVT_STREAM_IDLE_*`
   - `GVT_STREAM_PUBLISH_SOCKET`
   - `GVT_STREAM_SOURCE_ID`

## 需要提交到 SPICE 分支的当前改动

历史 SPICE H.264 优化不再是主线，但仍应作为实验分支保存:

- `video-stream.h`: MAX_FPS、stream start condition、timeout 调整。
- `display-channel.cpp`: `SPICE_GVTG_MIN_STREAM_PIXELS`。
- `gstreamer-encoder.c`: H.264/VAAPI 诊断日志、码率调参。
- `reds.cpp` / 输入相关补丁: SPICE embedded client 阶段的输入修复记录。

整理时要明确标注: 这些不是当前默认产品路线，只作为回滚和技术参考。

## 需要提交到客户端仓库的当前资料

- `ui-test/` 原型。
- `direct-stream/client/gvt_spice_viewer.c`。
- `direct-stream/client/gvt_control_overlay.py`。
- `direct-stream/client/spice_glib_probe.c`。
- `direct-stream/client/receive-h264-rtp.bat`。
- `direct-stream/client/receive-opus-rtp.bat`。
- `start-gvt-spice-embedded-client.bat`。
- `start-gvt-spice-embedded-client-节能模式.bat`。
- `停止gvt-spice-embedded-client.bat`。
- `客户端需求规划.md`。

不提交:

- `.exe`
- `.log`
- 大型 GStreamer runtime
- 虚拟机镜像
- 服务器/GitHub/Web 密码

## 需要提交到服务端仓库的当前资料

- `服务端需求规划.md`。
- `physical-output/gvt-outputd.c`。
- `physical-output/gvt-output-web.py`。
- `physical-output/multivm.py`。
- `physical-output/multivm_remote.sh`。
- `physical-output/remote-work/gvt-stream.c`。
- `physical-output/remote-work/meson.build`。
- `physical-output/*.bat` 管理脚本。
- `direct-stream/start_gvt_stream_qemu.py`。
- `direct-stream/remote-current-gvt-stream.c`。
- `direct-stream/server/*.sh` 和 `direct-stream/server/*.c`。
- `direct-stream/kernel/*.patch` 和 `direct-stream/kernel/gvt-shadowfb-uapi.h`。

不提交:

- pycache
- 日志
- 截图调试输出
- 可执行文件
- qcow2 镜像
- 任何包含密码的 `.env` 实例文件

## 后续远端创建命令参考

已安装并登录 `gh`。如需重建仓库，可参考:

```powershell
gh repo create fangquan1/gvt-cloud-client --public --description "GVT-g cloud desktop Windows client"
gh repo create fangquan1/gvt-cloud-server --public --description "GVT-g cloud desktop QEMU/server control plane"
```

推送本地仓库:

```powershell
cd repos/gvt-cloud-client
git remote add origin https://github.com/fangquan1/gvt-cloud-client.git
git push -u origin gvt-cloud-client-mvp

cd ..\gvt-cloud-server
git remote add origin https://github.com/fangquan1/gvt-cloud-server.git
git push -u origin gvt-cloud-server-mvp
```

fork/mirror 上游后:

```powershell
git remote add fangquan https://github.com/fangquan1/qemu.git
git switch -c gvt-cloud-gvt-stream-20260607
git push -u fangquan gvt-cloud-gvt-stream-20260607
```

其他组件按登记表使用对应分支名。
