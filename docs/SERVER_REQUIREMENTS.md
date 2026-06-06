# GVT 云桌面服务端需求规划

## 仓库

- 目标 GitHub 仓库: `https://github.com/fangquan1/gvt-cloud-server`
- 本地初始仓库: `repos/gvt-cloud-server`
- 默认开发分支: `gvt-cloud-server-mvp`
- 当前状态: 本地需求、脚本与关键源码备份已整理；远端 GitHub 仓库待创建/推送。

## 产品定位

服务端运行在 openEuler/KVM/QEMU 主机上，负责 GVT-g vGPU 桌面的启动、视频输出、输入转发、SPICE 音频、物理 DP/HDMI 输出和多 VM 切换。它的核心价值是 host/QEMU 控制台能力: 不依赖 Windows guest 内的远控软件，也能看到启动过程、驱动初始化和异常状态。

## 当前可用基线

截至 2026-06-06，最可用路线:

```text
Windows 10 GVT-g VM
  -> QEMU gvt-stream display backend
  -> H.264 RTP 到客户端
  -> SPICE 音频/session
  -> QEMU 原生 TCP 输入
```

同时，物理屏输出和多 VM 切换已跑通:

```text
VM/QEMU gvt-stream publisher
  -> /run/gvt-outputd.sock
  -> gvt-outputd
  -> DRM atomic commit
  -> DP/HDMI
```

当前关键验证点:

- QEMU 日志可见 `gvt-stream-input: listening on 0.0.0.0:5905`。
- QEMU 日志可见 `gvt-stream: encode-start ... fps=60 ... fec=0/0 path=dmabuf`。
- 实时模式 `capture_ms=16`，服务端可接近 60fps。
- 节能模式静止时 `capture_ms=66`，动态或输入后恢复 `capture_ms=16`。
- `gvt-outputd` 状态 `failed=0`，物理输出连接器当前基线为 `DP-1`。

## 非目标

- 不把 Sunshine/Moonlight/RDP 作为主显示链路。
- 不把虚拟机镜像、服务器密码、Web 密码、root 密钥或完整敏感日志提交到公开仓库。
- 不继续以压缩包归档作为主要版本管理方式。
- 不把绕过 QEMU DMABUF 检查的实验当作稳定方案。

## 服务端组件

### QEMU gvt-stream backend

职责:

- 接收 GVT-g VFIO display DMABUF scanout/update。
- 使用 EGL/GL/VAAPI 路径编码 H.264。
- 通过 RTP/UDP 输出到客户端。
- 提供原生 TCP 输入监听端口。
- 可选把 DMABUF 发布给 `gvt-outputd`。
- 支持实时/节能模式。

当前关键源码来源:

- 远端: `/usr/local/src/project/qemu/ui/gvt-stream.c`
- 本地最新备份: `physical-output/remote-work/gvt-stream.c`
- 低延迟基线备份: `direct-stream/remote-current-gvt-stream.c`

必须保留的当前策略:

- 默认 `GVT_STREAM_RTP_FEC=0`、`GVT_STREAM_RTP_FEC_IMPORTANT=0`。
- 保留 `queue leaky=downstream max-size-buffers=2`。
- 不恢复会造成花屏的客户端主动丢 H.264 包默认方案。
- 默认关闭 `GVT_STREAM_IMPORT_TEST`。
- PTS/duration 按真实 wall-clock 推进。

### gvt-outputd

职责:

- 独占 host DRM/KMS。
- 接收多个 QEMU 发布的 DMABUF。
- 管理 active source。
- 通过 atomic commit 输出到 DP/HDMI。
- 根据远程输入坐标绘制 host-side 硬件光标。

当前源码:

- `physical-output/gvt-outputd.c`
- 远端路径: `/root/qemu_cmd/multivm/gvt-outputd.c`

当前 socket/status/log:

- Socket: `/run/gvt-outputd.sock`
- Status: `/run/gvt-outputd.status`
- Log: `/root/qemu_cmd/multivm/gvt-outputd.log`

### 管理 API / Web 服务

职责:

- 登录认证。
- 查询主机、VM、输出和端口状态。
- 启动/停止 VM。
- 切换 active source。
- 切换输入和音频策略。
- 输出脱敏日志摘要。

当前源码:

- `physical-output/gvt-output-web.py`
- 远端路径: `/root/qemu_cmd/multivm/gvt-output-web.py`
- 当前 Web: `http://192.168.0.188:8098/`

第一版管理 API 可以先从当前 Web 服务扩展，后续再拆成正式 daemon。

### VM 启动与编排

职责:

- 创建/检查 vGPU mdev。
- 创建 tap 并挂入 `br0`。
- 选择 overlay 镜像。
- 配置 SPICE 音频端口、原生输入端口、RTP 目标、节能参数。
- 支持多 VM 配置。
- 支持 QMP 停止旧 QEMU，失败再 kill。

当前脚本:

- `direct-stream/start_gvt_stream_qemu.py`
- `physical-output/multivm.py`
- `physical-output/multivm_remote.sh`
- `physical-output/multivm-start.bat`
- `physical-output/multivm-stop.bat`
- `physical-output/multivm-status.bat`

### SPICE 音频/session

职责:

- 保留 SPICE playback/session。
- 不启用 SPICE display 作为主视频。
- 支持每 VM 独立 SPICE 端口。

当前基线:

- VM1 SPICE: `5900`
- VM2 SPICE: `5901`

### shadow framebuffer fallback

定位:

- 当前主线已转向 QEMU 原生 `gvt-stream` DMABUF。
- `shadowfb` 仍作为历史 fallback 和内核研究资料保留。

当前资料:

- `direct-stream/kernel/0001-gvt-shadowfb-debugfs-export-dmabuf.patch`
- `direct-stream/kernel/0002-gvt-shadowfb-debugfs-ioctl-body.patch`
- `direct-stream/kernel/work/shadow_fb.c`
- `direct-stream/kernel/work/shadow_fb.h`

## 运行模式

### 客户端解码实时模式

默认生产模式:

- `capture_ms=16`
- `idle_after_ms=0`
- `idle_probe_ms=0`
- `fec=0/0`
- 客户端 `video_latency=15`

### 客户端解码节能模式

可选承载更多桌面的模式:

- 动态或输入后 `capture_ms=16`
- 静止约 1500ms 后 `idle_capture_ms=66`
- `idle_probe_ms=500`
- `idle_changed_ppm=3000`
- `idle_pixel_delta=8`

### 物理 DP/HDMI 输出模式

- QEMU 发布 DMABUF 到 `/run/gvt-outputd.sock`。
- `gvt-outputd` 统一控制 KMS，避免多个 QEMU 抢 connector。
- Web/API 或脚本切换 active source。
- 远程输入坐标用于 host-side cursor。

## 多 VM 当前基线

- vGPU 类型: 两个 `i915-GVTg_V5_8`
- 分辨率: `1024x768`
- VM1:
  - overlay: `/root/qemu_cmd/multivm/win10-vm1.qcow2`
  - SPICE: `5900`
  - 输入: `5905`
  - tap: `tap-win10a`
  - MAC: `52:54:00:10:01:88`
- VM2:
  - overlay: `/root/qemu_cmd/multivm/win10-vm2.qcow2`
  - SPICE: `5901`
  - 输入: `5906`
  - tap: `tap-win10b`
  - MAC: `52:54:00:10:02:88`

## 管理 API 需求

建议第一版 API:

- `POST /api/login`
- `GET /api/status`
- `GET /api/desktops`
- `GET /api/desktops/{id}`
- `POST /api/desktops/{id}/start`
- `POST /api/desktops/{id}/stop`
- `POST /api/desktops/{id}/restart`
- `POST /api/desktops/{id}/mode`
- `POST /api/output/select`
- `POST /api/input/select`
- `POST /api/audio/select`
- `GET /api/logs/{id}`

状态返回至少包含:

- host 名称、版本、CPU/内存、连接器。
- VM 名称、状态、端口、分辨率、模式、QEMU PID。
- `gvt-outputd`: active source、failed count、cursor 状态。
- `gvt-stream`: fps、capture_ms、encoded、encode_failures。

## 开源依赖与 fork 分支

详细登记见 `GitHub开源仓库与分支登记.md`。服务端侧至少需要管理:

- QEMU fork/分支: `gvt-cloud-gvt-stream-20260607`
- SPICE server fork/分支: `gvt-cloud-spice-h264-20260607`
- Linux kernel fork/分支: `gvt-cloud-shadowfb-6.6.40-20260607`

## 里程碑

### M1: 仓库化和可启动

- 把 QEMU `gvt-stream.c`、`gvt-outputd.c`、Web/API、启动脚本纳入 Git。
- 清理敏感配置，改为 `.env.example`。
- 提供一键实时模式启动和状态查询。

### M2: API 化

- 将当前 Web 切换服务扩展为客户端可用 API。
- 支持 VM 列表、启动、停止、模式切换、active source 切换。
- 日志接口必须脱敏和裁剪。

### M3: 多 VM 稳定

- 支持 VM1/VM2 同时运行和切换。
- `gvt-outputd` 热重启后能恢复或请求 QEMU 重新发布 DMABUF。
- 输入、音频和物理输出联动。

### M4: 工程化

- QEMU/SPICE/Linux fork 分支提交当前补丁。
- 增加构建文档、安装文档和回滚文档。
- 增加服务端健康检查和故障恢复。

## 验收标准

- 单 VM 实时模式可稳定启动，日志 `encode_failures=0`。
- 客户端 15ms 无 FEC 基线可连接，输入手感保持当前水平。
- 节能模式静止降帧，动态/输入恢复 60fps。
- 多 VM 物理输出 `vm1 -> vm2 -> vm1` 切换 `failed=0`。
- 敏感信息不进入公开仓库。
