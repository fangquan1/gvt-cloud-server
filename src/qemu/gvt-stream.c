/*
 * GVT-g stream display backend.
 *
 * Attach to QEMU's GL/DMABUF display path, accept control/input clients, and
 * forward scanout DMABUF fds to the standalone gvt-streamd encoder process.
 * QEMU intentionally does not contain the video encoder/RTP sender.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
#include "gvt-stream-ipc.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "ui/console.h"
#include "ui/dmabuf.h"
#include "ui/input.h"
#include "ui/surface.h"
#include "ui/egl-context.h"
#include "ui/egl-helpers.h"

#define GVT_STREAM_IDLE_SAMPLE_W 64
#define GVT_STREAM_IDLE_SAMPLE_H 36
#define GVT_STREAM_IDLE_SAMPLE_N \
    (GVT_STREAM_IDLE_SAMPLE_W * GVT_STREAM_IDLE_SAMPLE_H)
#define GVT_STREAM_FOURCC_XR24 0x34325258u
#define GVT_STREAM_WAKE_PUMP_NO_SCANOUT_MS 5000u
#define GVT_STREAM_WAKE_PUMP_SUSPENDED_MS 45000u
#define GVT_STREAM_DIRTY_BLOCK_DEFAULT 16u
#define GVT_STREAM_DIRTY_PARTIAL_PPM_DEFAULT 150000u
#define GVT_STREAM_DIRTY_GLOBAL_PPM_DEFAULT 350000u
typedef struct GVTStreamDisplay {
    DisplayChangeListener dcl;
    QemuDmaBuf *scanout;
    uint64_t scanout_count;
    uint64_t update_count;
    uint64_t cursor_count;
    uint64_t release_count;
    int64_t last_report_ms;
    int64_t last_update_ms;
    uint64_t report_updates;
    uint64_t refresh_ms;
    uint64_t report_ms;
    uint64_t import_count;
    uint64_t import_fail_count;
    uint64_t capture_count;
    uint64_t capture_fail_count;
    uint64_t scanout_disable_count;
    uint64_t capture_ms;
    uint64_t idle_capture_ms;
    uint64_t idle_after_ms;
    uint64_t idle_probe_ms;
    uint64_t idle_changed_ppm;
    uint64_t idle_pixel_delta;
    uint64_t capture_max;
    uint64_t last_capture_checksum;
    int64_t last_capture_ms;
    uint64_t startup_pump_ms;
    uint64_t startup_pump_interval_ms;
    int64_t startup_pump_until_ms;
    QEMUTimer *startup_pump_timer;
    int64_t last_activity_ms;
    int64_t last_content_change_ms;
    int64_t last_probe_ms;
    int64_t last_wakeup_pulse_ms;
    uint64_t last_probe_diff_ppm;
    uint64_t idle_probe_count;
    uint64_t idle_wake_count;
    uint64_t wakeup_pulse_count;
    bool idle_sample_valid;
    uint32_t idle_sample[GVT_STREAM_IDLE_SAMPLE_N];
    bool low_bandwidth;
    bool dirty_valid;
    bool dirty_prev_valid;
    uint8_t *dirty_prev;
    size_t dirty_prev_size;
    int dirty_prev_width;
    int dirty_prev_height;
    int dirty_prev_stride;
    uint32_t dirty_x;
    uint32_t dirty_y;
    uint32_t dirty_w;
    uint32_t dirty_h;
    uint32_t dirty_mode;
    uint32_t dirty_block_size;
    uint64_t dirty_pixel_delta;
    uint64_t dirty_partial_max_ppm;
    uint64_t dirty_global_min_ppm;
    uint64_t dirty_global_burst_frames;
    uint64_t dirty_global_frames_left;
    uint64_t dirty_changed_pixels;
    uint64_t dirty_diff_ppm;
    uint64_t dirty_background_seq;
    uint64_t dirty_frame_count;
    uint64_t dirty_static_count;
    uint64_t dirty_partial_count;
    uint64_t dirty_full_count;
    uint64_t dirty_global_count;
    uint64_t dirty_skip_count;
    int dirty_target_bitrate;
    int encode_fps;
    int encode_bitrate;
    int encode_idle_bitrate;
    int encode_still_bitrate;
    int encode_keyint;
    uint64_t rtp_port;
    uint64_t rtp_fec;
    uint64_t rtp_fec_important;
    int rtp_mtu;
    char *video_codec;
    char *encode_rate_control;
    char *capture_dir;
    char *rtp_host;
    DisplaySurface *capture_surface;
    bool scanout_lost;
    egl_fb guest_fb;
    egl_fb capture_fb;
    bool verbose;
    bool import_test;
    char *external_socket;
    int external_fd;
    bool external_start_sent;
    uint64_t external_seq;
    uint64_t external_frame_count;
    uint64_t external_no_scanout_count;
    uint64_t external_send_fail_count;
    uint64_t external_connect_fail_count;
    int64_t external_last_connect_warn_ms;
    uint64_t external_stats_count;
    uint64_t external_encoded;
    uint64_t external_encode_failures;
} GVTStreamDisplay;

typedef struct GVTStreamInputServer GVTStreamInputServer;
typedef struct GVTStreamControlClient GVTStreamControlClient;
typedef struct GVTStreamControlServer GVTStreamControlServer;

typedef struct GVTStreamInputClient {
    GVTStreamInputServer *server;
    int fd;
    GString *buffer;
} GVTStreamInputClient;

struct GVTStreamInputServer {
    int listen_fd;
    GList *clients;
    uint64_t connected;
    uint64_t messages;
    uint64_t events;
    uint64_t parse_errors;
    uint64_t debug_logs;
};

struct GVTStreamControlClient {
    int fd;
    char host[INET_ADDRSTRLEN];
    GString *buffer;
    int64_t accepted_ms;
    uint64_t messages;
};

struct GVTStreamControlServer {
    int listen_fd;
    char listen_host[INET_ADDRSTRLEN];
    uint64_t listen_port;
    GList *clients;
    uint64_t connected;
    uint64_t messages;
    uint64_t parse_errors;
};

static const DisplayChangeListenerOps gvt_stream_ops;
static GVTStreamInputServer *gvt_stream_input_server;
static GVTStreamDisplay *gvt_stream_input_display;
static GVTStreamControlServer *gvt_stream_control_server;
static GVTStreamDisplay *gvt_stream_control_display;
static GVTStreamControlClient *gvt_stream_control_active_client;
static int64_t gvt_stream_last_input_ms;
static int64_t gvt_stream_last_input_seq;
static int64_t gvt_stream_last_input_capture_seq;
static uint64_t gvt_stream_spice_port;
static uint64_t gvt_stream_input_port;

static void gvt_stream_capture_frame(GVTStreamDisplay *gdpy, int64_t now_ms);
static void gvt_stream_startup_pump_arm(GVTStreamDisplay *gdpy,
                                        int64_t now_ms);
static void gvt_stream_startup_pump_stop(GVTStreamDisplay *gdpy);
static bool gvt_stream_wake_if_suspended(void);
static const char *gvt_stream_dirty_mode_name(uint32_t mode);
static void gvt_stream_dirty_reset(GVTStreamDisplay *gdpy);

static uint64_t gvt_stream_port_slot(uint64_t control_port)
{
    if (control_port < 5004) {
        return 0;
    }
    return (control_port - 5004) / 4;
}

static uint64_t gvt_stream_default_spice_port(uint64_t control_port)
{
    return 5900 + gvt_stream_port_slot(control_port);
}

static uint64_t gvt_stream_default_input_port(uint64_t control_port)
{
    return 5905 + gvt_stream_port_slot(control_port);
}

static uint64_t gvt_stream_getenv_u64(const char *name,
                                      uint64_t defval,
                                      uint64_t minval,
                                      uint64_t maxval)
{
    const char *env = g_getenv(name);
    uint64_t val;
    char *end = NULL;

    if (!env || !*env) {
        return defval;
    }

    errno = 0;
    val = g_ascii_strtoull(env, &end, 0);
    if (errno || end == env || (end && *end)) {
        warn_report("gvt-stream: ignoring invalid %s=%s", name, env);
        return defval;
    }
    if (val < minval) {
        return minval;
    }
    if (val > maxval) {
        return maxval;
    }
    return val;
}

static bool gvt_stream_getenv_bool(const char *name, bool defval)
{
    const char *env = g_getenv(name);

    if (!env || !*env) {
        return defval;
    }
    if (!g_ascii_strcasecmp(env, "1") ||
        !g_ascii_strcasecmp(env, "on") ||
        !g_ascii_strcasecmp(env, "yes") ||
        !g_ascii_strcasecmp(env, "true")) {
        return true;
    }
    if (!g_ascii_strcasecmp(env, "0") ||
        !g_ascii_strcasecmp(env, "off") ||
        !g_ascii_strcasecmp(env, "no") ||
        !g_ascii_strcasecmp(env, "false")) {
        return false;
    }

    warn_report("gvt-stream: ignoring invalid %s=%s", name, env);
    return defval;
}

static int64_t gvt_stream_last_activity_ms(GVTStreamDisplay *gdpy);

static uint64_t gvt_stream_effective_capture_ms(GVTStreamDisplay *gdpy,
                                                int64_t now_ms)
{
    int64_t last_activity_ms = gvt_stream_last_activity_ms(gdpy);
    uint64_t capture_ms = gdpy->capture_ms;
    uint64_t fps_capture_ms;

    if (gdpy->idle_capture_ms > gdpy->capture_ms &&
        gdpy->idle_after_ms && last_activity_ms &&
        now_ms - last_activity_ms > gdpy->idle_after_ms) {
        capture_ms = gdpy->idle_capture_ms;
    }

    if (gdpy->encode_fps > 0) {
        fps_capture_ms = (1000 + (uint64_t)gdpy->encode_fps - 1) /
                         (uint64_t)gdpy->encode_fps;
        if (fps_capture_ms > capture_ms) {
            capture_ms = fps_capture_ms;
        }
    }
    return capture_ms;
}

static int64_t gvt_stream_last_activity_ms(GVTStreamDisplay *gdpy)
{
    int64_t last_activity_ms = gdpy->last_activity_ms;

    if (gdpy->last_content_change_ms > last_activity_ms) {
        last_activity_ms = gdpy->last_content_change_ms;
    }
    if (gvt_stream_last_input_ms > last_activity_ms) {
        last_activity_ms = gvt_stream_last_input_ms;
    }
    return last_activity_ms;
}

static int gvt_stream_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void gvt_stream_external_close(GVTStreamDisplay *gdpy)
{
    if (!gdpy) {
        return;
    }

    gdpy->external_start_sent = false;
    if (gdpy->external_fd < 0) {
        return;
    }

    qemu_set_fd_handler(gdpy->external_fd, NULL, NULL, NULL);
    close(gdpy->external_fd);
    gdpy->external_fd = -1;
}

static bool gvt_stream_external_message_valid(const GVTStreamIpcMessage *msg,
                                              ssize_t len)
{
    return len == sizeof(*msg) &&
        msg->magic == GVT_STREAM_IPC_MAGIC &&
        msg->version == GVT_STREAM_IPC_VERSION &&
        msg->size == sizeof(*msg);
}

static void gvt_stream_external_warn_connect_failed(GVTStreamDisplay *gdpy,
                                                    const char *detail)
{
    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    gdpy->external_connect_fail_count++;
    if (gdpy->verbose || gdpy->external_connect_fail_count <= 5 ||
        now_ms - gdpy->external_last_connect_warn_ms >= 1000) {
        warn_report("gvt-stream-external: connect %s failed: %s "
                    "failures=%" PRIu64,
                    gdpy->external_socket, detail,
                    gdpy->external_connect_fail_count);
        gdpy->external_last_connect_warn_ms = now_ms;
    }
}

static void gvt_stream_external_read(void *opaque)
{
    GVTStreamDisplay *gdpy = opaque;

    for (;;) {
        GVTStreamIpcMessage msg;
        ssize_t ret = recv(gdpy->external_fd, &msg, sizeof(msg), MSG_DONTWAIT);

        if (ret > 0) {
            if (!gvt_stream_external_message_valid(&msg, ret)) {
                warn_report("gvt-stream-external: ignoring invalid message bytes=%zd",
                            ret);
                continue;
            }
            if (msg.type == GVT_STREAM_IPC_STATS) {
                gdpy->external_stats_count++;
                gdpy->external_encoded = msg.encoded;
                gdpy->external_encode_failures = msg.encode_failures;
                if (gdpy->verbose || gdpy->external_stats_count <= 5 ||
                    gdpy->external_stats_count % 60 == 0) {
                    error_report("gvt-stream-external: stats #%" PRIu64
                                 " encoded=%" PRIu64 " failures=%" PRIu64,
                                 gdpy->external_stats_count,
                                 gdpy->external_encoded,
                                 gdpy->external_encode_failures);
                }
            }
            continue;
        }
        if (ret == 0) {
            warn_report("gvt-stream-external: streamd closed socket");
            gvt_stream_external_close(gdpy);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        warn_report("gvt-stream-external: recv failed: %s", strerror(errno));
        gvt_stream_external_close(gdpy);
        return;
    }
}

static bool gvt_stream_external_connect(GVTStreamDisplay *gdpy)
{
    struct sockaddr_un addr = { 0 };
    int fd;
    int ret;

    if (!gdpy) {
        return false;
    }
    if (gdpy->external_fd >= 0) {
        return true;
    }
    if (!gdpy->external_socket || !*gdpy->external_socket) {
        warn_report("gvt-stream-external: missing GVT_STREAMD_SOCKET");
        return false;
    }
    if (strlen(gdpy->external_socket) >= sizeof(addr.sun_path)) {
        warn_report("gvt-stream-external: socket path too long: %s",
                    gdpy->external_socket);
        return false;
    }

    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        warn_report("gvt-stream-external: socket failed: %s", strerror(errno));
        return false;
    }
    qemu_set_cloexec(fd);

    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, gdpy->external_socket, sizeof(addr.sun_path));
    do {
        ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        gvt_stream_external_warn_connect_failed(gdpy, strerror(errno));
        close(fd);
        return false;
    }

    gvt_stream_set_nonblock(fd);
    gdpy->external_fd = fd;
    gdpy->external_connect_fail_count = 0;
    gdpy->external_last_connect_warn_ms = 0;
    qemu_set_fd_handler(fd, gvt_stream_external_read, NULL, gdpy);
    error_report("gvt-stream-external: connected socket=%s fd=%d",
                 gdpy->external_socket, fd);
    return true;
}

static void gvt_stream_external_fill_common(GVTStreamDisplay *gdpy,
                                            GVTStreamIpcMessage *msg,
                                            GVTStreamIpcType type,
                                            int64_t now_ms)
{
    memset(msg, 0, sizeof(*msg));
    msg->magic = GVT_STREAM_IPC_MAGIC;
    msg->version = GVT_STREAM_IPC_VERSION;
    msg->type = type;
    msg->size = sizeof(*msg);
    msg->seq = ++gdpy->external_seq;
    msg->pts_ns = now_ms > 0 ? (uint64_t)now_ms * 1000000ULL : 0;
    msg->fps = gdpy->encode_fps;
    if (gdpy->low_bandwidth) {
        msg->flags |= GVT_STREAM_IPC_FLAG_LOW_BANDWIDTH;
    }
    msg->bitrate = gdpy->encode_bitrate;
    msg->idle_bitrate = gdpy->encode_idle_bitrate;
    msg->still_bitrate = gdpy->encode_still_bitrate;
    msg->keyint = gdpy->encode_keyint;
    msg->mtu = gdpy->rtp_mtu;
    msg->rtp_port = gdpy->rtp_port;
    msg->rtp_fec = gdpy->rtp_fec;
    msg->rtp_fec_important = gdpy->rtp_fec_important;
    if (gdpy->rtp_host) {
        g_strlcpy(msg->host, gdpy->rtp_host, sizeof(msg->host));
    }
    if (gdpy->video_codec) {
        g_strlcpy(msg->codec, gdpy->video_codec, sizeof(msg->codec));
    }
    if (gdpy->encode_rate_control) {
        g_strlcpy(msg->rate_control, gdpy->encode_rate_control,
                  sizeof(msg->rate_control));
    }
}

static bool gvt_stream_external_send_message(GVTStreamDisplay *gdpy,
                                             GVTStreamIpcMessage *msg,
                                             int fd_to_send)
{
    struct iovec iov = {
        .iov_base = msg,
        .iov_len = sizeof(*msg),
    };
    struct msghdr hdr = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };
    char control[CMSG_SPACE(sizeof(int))];
    ssize_t ret;

    if (!gvt_stream_external_connect(gdpy)) {
        gdpy->external_send_fail_count++;
        return false;
    }

    if (fd_to_send >= 0) {
        struct cmsghdr *cmsg;

        memset(control, 0, sizeof(control));
        hdr.msg_control = control;
        hdr.msg_controllen = sizeof(control);
        cmsg = CMSG_FIRSTHDR(&hdr);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));
        hdr.msg_controllen = sizeof(control);
    }

    ret = sendmsg(gdpy->external_fd, &hdr, MSG_NOSIGNAL);
    if (ret != sizeof(*msg)) {
        gdpy->external_send_fail_count++;
        if (ret < 0) {
            warn_report("gvt-stream-external: send type=%u failed: %s",
                        msg->type, strerror(errno));
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                gvt_stream_external_close(gdpy);
            }
        } else {
            warn_report("gvt-stream-external: short send type=%u bytes=%zd",
                        msg->type, ret);
        }
        return false;
    }
    return true;
}

static bool gvt_stream_external_send_start(GVTStreamDisplay *gdpy,
                                           int64_t now_ms)
{
    GVTStreamIpcMessage msg;
    bool ok;

    gvt_stream_external_fill_common(gdpy, &msg, GVT_STREAM_IPC_START,
                                    now_ms);
    ok = gvt_stream_external_send_message(gdpy, &msg, -1);
    if (ok) {
        gdpy->external_start_sent = true;
        error_report("gvt-stream-external: start target=%s:%u codec=%s "
                     "fps=%u bitrate=%u keyint=%u mtu=%u fec=%u/%u "
                     "socket=%s",
                     msg.host, msg.rtp_port, msg.codec,
                     msg.fps, msg.bitrate, msg.keyint, msg.mtu,
                     msg.rtp_fec, msg.rtp_fec_important,
                     gdpy->external_socket ?: "");
    }
    return ok;
}

static void gvt_stream_external_send_stop(GVTStreamDisplay *gdpy,
                                          int64_t now_ms)
{
    GVTStreamIpcMessage msg;

    if (!gdpy) {
        return;
    }
    gdpy->external_start_sent = false;
    if (gdpy->external_fd < 0) {
        return;
    }

    gvt_stream_external_fill_common(gdpy, &msg, GVT_STREAM_IPC_STOP, now_ms);
    gvt_stream_external_send_message(gdpy, &msg, -1);
}

static void gvt_stream_external_send_no_scanout(GVTStreamDisplay *gdpy,
                                                int64_t now_ms)
{
    GVTStreamIpcMessage msg;

    if (!gdpy || !gdpy->rtp_host || !gdpy->rtp_port) {
        return;
    }
    if (!gdpy->external_start_sent &&
        !gvt_stream_external_send_start(gdpy, now_ms)) {
        return;
    }

    gvt_stream_external_fill_common(gdpy, &msg, GVT_STREAM_IPC_NO_SCANOUT,
                                    now_ms);
    if (gvt_stream_external_send_message(gdpy, &msg, -1)) {
        gdpy->external_no_scanout_count++;
        if (gdpy->verbose || gdpy->external_no_scanout_count <= 5 ||
            gdpy->external_no_scanout_count % 60 == 0) {
            error_report("gvt-stream-external: no-scanout #%" PRIu64
                         " seq=%" PRIu64,
                         gdpy->external_no_scanout_count, msg.seq);
        }
    }
}

static bool gvt_stream_external_send_frame(GVTStreamDisplay *gdpy,
                                           QemuDmaBuf *dmabuf,
                                           int64_t now_ms)
{
    GVTStreamIpcMessage msg;
    const int *fds;
    const uint32_t *offsets;
    const uint32_t *strides;
    int n_fds = 0;
    int n_offsets = 0;
    int n_strides = 0;
    uint32_t planes;
    bool ok;

    if (!gdpy || !dmabuf || !gdpy->rtp_host || !gdpy->rtp_port) {
        return false;
    }

    planes = qemu_dmabuf_get_num_planes(dmabuf);
    fds = qemu_dmabuf_get_fds(dmabuf, &n_fds);
    offsets = qemu_dmabuf_get_offsets(dmabuf, &n_offsets);
    strides = qemu_dmabuf_get_strides(dmabuf, &n_strides);
    if (planes != 1 || !fds || n_fds < 1 ||
        !strides || n_strides < 1 ||
        !qemu_dmabuf_get_width(dmabuf) ||
        !qemu_dmabuf_get_height(dmabuf) ||
        qemu_dmabuf_get_fourcc(dmabuf) != GVT_STREAM_FOURCC_XR24) {
        gdpy->external_send_fail_count++;
        error_report("gvt-stream-external: frame-unsupported planes=%u "
                     "fds=%d strides=%d size=%ux%u fourcc=0x%08x",
                     planes, n_fds, n_strides,
                     qemu_dmabuf_get_width(dmabuf),
                     qemu_dmabuf_get_height(dmabuf),
                     qemu_dmabuf_get_fourcc(dmabuf));
        return false;
    }

    if (!gdpy->external_start_sent &&
        !gvt_stream_external_send_start(gdpy, now_ms)) {
        return false;
    }

    gvt_stream_external_fill_common(gdpy, &msg, GVT_STREAM_IPC_FRAME,
                                    now_ms);
    msg.width = qemu_dmabuf_get_width(dmabuf);
    msg.height = qemu_dmabuf_get_height(dmabuf);
    msg.fourcc = qemu_dmabuf_get_fourcc(dmabuf);
    msg.stride = strides[0];
    msg.offset = (offsets && n_offsets > 0) ? offsets[0] : 0;
    msg.modifier = qemu_dmabuf_get_modifier(dmabuf);
    if (gdpy->low_bandwidth && gdpy->dirty_valid) {
        msg.flags |= GVT_STREAM_IPC_FLAG_DIRTY_VALID;
        msg.dirty_x = gdpy->dirty_x;
        msg.dirty_y = gdpy->dirty_y;
        msg.dirty_w = gdpy->dirty_w;
        msg.dirty_h = gdpy->dirty_h;
        msg.dirty_ppm = gdpy->dirty_diff_ppm;
        msg.dirty_mode = gdpy->dirty_mode;
        msg.dirty_block_size = gdpy->dirty_block_size;
        msg.dirty_pixels = gdpy->dirty_changed_pixels;
        msg.background_seq = gdpy->dirty_background_seq;
        if (gdpy->dirty_mode == GVT_STREAM_DIRTY_STATIC) {
            msg.flags |= GVT_STREAM_IPC_FLAG_DIRTY_STATIC;
        } else if (gdpy->dirty_mode == GVT_STREAM_DIRTY_PARTIAL) {
            msg.flags |= GVT_STREAM_IPC_FLAG_DIRTY_PARTIAL;
        } else if (gdpy->dirty_mode == GVT_STREAM_DIRTY_GLOBAL) {
            msg.flags |= GVT_STREAM_IPC_FLAG_DIRTY_GLOBAL;
        }
    }

    ok = gvt_stream_external_send_message(gdpy, &msg, fds[0]);
    if (ok) {
        gdpy->external_frame_count++;
        if (gdpy->verbose || gdpy->external_frame_count <= 5 ||
            gdpy->external_frame_count % 60 == 0) {
            error_report("gvt-stream-external: frame-sent #%" PRIu64
                         " seq=%" PRIu64 " fd=%d size=%ux%u stride=%u "
                         "modifier=0x%016" PRIx64
                         " dirty=%s rect=%u,%u %ux%u ppm=%u target_bitrate=%d",
                         gdpy->external_frame_count, msg.seq, fds[0],
                         msg.width, msg.height, msg.stride, msg.modifier,
                         gvt_stream_dirty_mode_name(msg.dirty_mode),
                         msg.dirty_x, msg.dirty_y, msg.dirty_w, msg.dirty_h,
                         msg.dirty_ppm, gdpy->dirty_target_bitrate);
        }
    }
    return ok;
}

static int64_t gvt_stream_qdict_get_clamped_int(QDict *dict,
                                                const char *key,
                                                int64_t min,
                                                int64_t max,
                                                int64_t defval)
{
    int64_t val = qdict_get_try_int(dict, key, defval);

    if (val < min) {
        return min;
    }
    if (val > max) {
        return max;
    }
    return val;
}

static bool gvt_stream_parse_qcode(const char *name, QKeyCode *qcode)
{
    int value;

    if (!name || !*name) {
        return false;
    }
    value = qapi_enum_parse(&QKeyCode_lookup, name, -1, NULL);
    if (value < 0 || value >= Q_KEY_CODE__MAX) {
        return false;
    }
    *qcode = value;
    return true;
}

static bool gvt_stream_parse_button(const char *name, InputButton *button)
{
    int value;

    if (!name || !*name) {
        return false;
    }
    value = qapi_enum_parse(&InputButton_lookup, name, -1, NULL);
    if (value < 0 || value >= INPUT_BUTTON__MAX) {
        return false;
    }
    *button = value;
    return true;
}

static void gvt_stream_input_send_qcode(const char *name, bool down,
                                        GVTStreamInputServer *server)
{
    QKeyCode qcode;

    if (!gvt_stream_parse_qcode(name, &qcode)) {
        server->parse_errors++;
        warn_report("gvt-stream-input: ignoring unknown qcode=%s",
                    name ?: "");
        return;
    }
    qemu_input_event_send_key_qcode(NULL, qcode, down);
    server->events++;
}

static void gvt_stream_input_process_dict(GVTStreamInputServer *server,
                                          QDict *dict);

static void gvt_stream_input_process_batch(GVTStreamInputServer *server,
                                           QDict *dict)
{
    QList *items = qdict_get_qlist(dict, "items");
    const QListEntry *entry;

    if (!items) {
        server->parse_errors++;
        return;
    }

    QLIST_FOREACH_ENTRY(items, entry) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));

        if (item) {
            gvt_stream_input_process_dict(server, item);
        } else {
            server->parse_errors++;
        }
    }
}

static void gvt_stream_input_process_combo(GVTStreamInputServer *server,
                                           QDict *dict)
{
    QList *qcodes = qdict_get_qlist(dict, "qcodes");
    const QListEntry *entry;
    GArray *parsed;
    int i;

    if (!qcodes) {
        server->parse_errors++;
        return;
    }

    parsed = g_array_new(FALSE, FALSE, sizeof(QKeyCode));
    QLIST_FOREACH_ENTRY(qcodes, entry) {
        QString *qstr = qobject_to(QString, qlist_entry_obj(entry));
        QKeyCode qcode;

        if (!qstr ||
            !gvt_stream_parse_qcode(qstring_get_str(qstr), &qcode)) {
            server->parse_errors++;
            continue;
        }
        g_array_append_val(parsed, qcode);
    }

    for (i = 0; i < parsed->len; i++) {
        QKeyCode qcode = g_array_index(parsed, QKeyCode, i);
        qemu_input_event_send_key_qcode(NULL, qcode, true);
        server->events++;
    }
    for (i = parsed->len - 1; i >= 0; i--) {
        QKeyCode qcode = g_array_index(parsed, QKeyCode, i);
        qemu_input_event_send_key_qcode(NULL, qcode, false);
        server->events++;
    }
    g_array_free(parsed, TRUE);
}

static void gvt_stream_input_process_dict(GVTStreamInputServer *server,
                                          QDict *dict)
{
    const char *type = qdict_get_try_str(dict, "type");

    if (!type) {
        server->parse_errors++;
        return;
    }

    if (!g_strcmp0(type, "batch")) {
        gvt_stream_input_process_batch(server, dict);
        return;
    }
    if (!g_strcmp0(type, "move")) {
        int x = gvt_stream_qdict_get_clamped_int(dict, "x", 0, 0x7fff, 0);
        int y = gvt_stream_qdict_get_clamped_int(dict, "y", 0, 0x7fff, 0);

        qemu_input_queue_abs(NULL, INPUT_AXIS_X, x, 0, 0x7fff);
        qemu_input_queue_abs(NULL, INPUT_AXIS_Y, y, 0, 0x7fff);
        server->events += 2;
        return;
    }
    if (!g_strcmp0(type, "button")) {
        const char *name = qdict_get_try_str(dict, "button") ?: "left";
        bool down = qdict_get_try_bool(dict, "down", false);
        InputButton button;

        if (!gvt_stream_parse_button(name, &button)) {
            server->parse_errors++;
            warn_report("gvt-stream-input: ignoring unknown button=%s", name);
            return;
        }
        qemu_input_queue_btn(NULL, button, down);
        server->events++;
        return;
    }
    if (!g_strcmp0(type, "wheel")) {
        int64_t delta = qdict_get_try_int(dict, "delta", 0);
        InputButton button = delta > 0 ? INPUT_BUTTON_WHEEL_UP :
                                        INPUT_BUTTON_WHEEL_DOWN;

        qemu_input_queue_btn(NULL, button, true);
        qemu_input_queue_btn(NULL, button, false);
        server->events += 2;
        return;
    }
    if (!g_strcmp0(type, "key")) {
        gvt_stream_input_send_qcode(qdict_get_try_str(dict, "qcode"),
                                    qdict_get_try_bool(dict, "down", false),
                                    server);
        return;
    }
    if (!g_strcmp0(type, "combo")) {
        gvt_stream_input_process_combo(server, dict);
        return;
    }

    server->parse_errors++;
}

static void gvt_stream_input_process_line(GVTStreamInputClient *client,
                                          const char *line)
{
    GVTStreamInputServer *server = client->server;
    Error *err = NULL;
    QObject *obj;
    QDict *dict;
    const char *type;
    int64_t seq;
    int64_t client_wall_ms;
    int64_t client_queue_ms;
    int64_t recv_ms;
    int64_t recv_wall_ms;
    int64_t done_ms;

    if (!line || !*line) {
        return;
    }
    recv_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    recv_wall_ms = g_get_real_time() / 1000;

    obj = qobject_from_json(line, &err);
    if (err) {
        server->parse_errors++;
        warn_report("gvt-stream-input: json parse failed: %s",
                    error_get_pretty(err));
        error_free(err);
        return;
    }
    dict = qobject_to(QDict, obj);
    if (!dict) {
        server->parse_errors++;
        qobject_unref(obj);
        return;
    }

    type = qdict_get_try_str(dict, "type") ?: "?";
    seq = qdict_get_try_int(dict, "_seq", 0);
    client_wall_ms = qdict_get_try_int(dict, "_client_wall_ms", 0);
    client_queue_ms = qdict_get_try_int(dict, "_client_queue_ms", -1);

    gvt_stream_last_input_ms = recv_ms;
    if (seq > 0) {
        gvt_stream_last_input_seq = seq;
    }
    gvt_stream_wake_if_suspended();
    gvt_stream_input_process_dict(server, dict);
    qemu_input_event_sync();
    done_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    server->messages++;
    if (seq > 0 && (seq <= 80 || (seq % 120) == 0 ||
                    g_strcmp0(type, "move"))) {
        server->debug_logs++;
        error_report("latency-input-recv seq=%" PRId64 " type=%s client_queue_ms=%" PRId64
                     " server_process_ms=%" PRId64 " clock_delta_ms=%" PRId64
                     " messages=%" PRIu64 " events=%" PRIu64
                     " parse_errors=%" PRIu64,
                     seq, type, client_queue_ms, done_ms - recv_ms,
                     client_wall_ms ? recv_wall_ms - client_wall_ms : 0,
                     server->messages, server->events, server->parse_errors);
    }
    qobject_unref(obj);
}

static void gvt_stream_input_client_close(GVTStreamInputClient *client)
{
    if (!client) {
        return;
    }
    qemu_set_fd_handler(client->fd, NULL, NULL, NULL);
    close(client->fd);
    client->server->clients = g_list_remove(client->server->clients, client);
    g_string_free(client->buffer, TRUE);
    g_free(client);
}

static void gvt_stream_input_client_read(void *opaque)
{
    GVTStreamInputClient *client = opaque;
    char tmp[4096];

    for (;;) {
        ssize_t ret = read(client->fd, tmp, sizeof(tmp));

        if (ret > 0) {
            char *nl;

            g_string_append_len(client->buffer, tmp, ret);
            while ((nl = strchr(client->buffer->str, '\n'))) {
                g_autofree char *line =
                    g_strndup(client->buffer->str, nl - client->buffer->str);
                g_string_erase(client->buffer, 0,
                               nl - client->buffer->str + 1);
                gvt_stream_input_process_line(client, line);
            }
            if (client->buffer->len > 1024 * 1024) {
                client->server->parse_errors++;
                warn_report("gvt-stream-input: closing oversized client buffer");
                gvt_stream_input_client_close(client);
                return;
            }
            continue;
        }
        if (ret == 0) {
            gvt_stream_input_client_close(client);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        gvt_stream_input_client_close(client);
        return;
    }
}

static void gvt_stream_input_accept(void *opaque)
{
    GVTStreamInputServer *server = opaque;

    for (;;) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(server->listen_fd, (struct sockaddr *)&addr, &addrlen);
        GVTStreamInputClient *client;
        int one = 1;

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                warn_report("gvt-stream-input: accept failed: %s",
                            strerror(errno));
            }
            return;
        }

        qemu_set_cloexec(fd);
        gvt_stream_set_nonblock(fd);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        client = g_new0(GVTStreamInputClient, 1);
        client->server = server;
        client->fd = fd;
        client->buffer = g_string_new(NULL);
        server->clients = g_list_prepend(server->clients, client);
        server->connected++;
        qemu_set_fd_handler(fd, gvt_stream_input_client_read, NULL, client);
        error_report("gvt-stream-input: client connected from %s fd=%d total=%" PRIu64,
                     inet_ntoa(addr.sin_addr), fd, server->connected);
        gvt_stream_input_client_read(client);
    }
}

static void gvt_stream_input_start(uint64_t control_port)
{
    const char *host = g_getenv("GVT_STREAM_INPUT_HOST") ?: "0.0.0.0";
    uint64_t port = gvt_stream_getenv_u64("GVT_STREAM_INPUT_PORT",
                                          gvt_stream_input_port ?:
                                          gvt_stream_default_input_port(control_port),
                                          0, 65535);
    struct sockaddr_in addr = { 0 };
    int fd;

    if (!port || gvt_stream_input_server) {
        return;
    }
    gvt_stream_input_port = port;

    fd = qemu_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error_report("gvt-stream-input: socket failed: %s", strerror(errno));
        exit(1);
    }

    socket_set_fast_reuse(fd);
    qemu_set_cloexec(fd);
    gvt_stream_set_nonblock(fd);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        error_report("gvt-stream-input: invalid listen host %s", host);
        close(fd);
        exit(1);
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_report("gvt-stream-input: bind %s:%" PRIu64 " failed: %s",
                     host, port, strerror(errno));
        close(fd);
        exit(1);
    }
    if (listen(fd, 4) < 0) {
        error_report("gvt-stream-input: listen failed: %s", strerror(errno));
        close(fd);
        exit(1);
    }

    gvt_stream_input_server = g_new0(GVTStreamInputServer, 1);
    gvt_stream_input_server->listen_fd = fd;
    qemu_set_fd_handler(fd, gvt_stream_input_accept, NULL,
                        gvt_stream_input_server);
    error_report("gvt-stream-input: listening on %s:%" PRIu64, host, port);
}

static void gvt_stream_control_apply_stop(GVTStreamDisplay *gdpy)
{
    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    if (!gdpy) {
        return;
    }
    gvt_stream_startup_pump_stop(gdpy);
    gvt_stream_external_send_stop(gdpy, now_ms);
    g_clear_pointer(&gdpy->rtp_host, g_free);
    gdpy->rtp_port = 0;
    error_report("gvt-stream-control: stream stopped");
}

static bool gvt_stream_wake_if_suspended(void)
{
    Error *err = NULL;

    if (!runstate_check(RUN_STATE_SUSPENDED)) {
        return false;
    }

    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER, &err);
    if (err) {
        warn_report("gvt-stream-control: wakeup request failed: %s",
                    error_get_pretty(err));
        error_free(err);
    } else {
        error_report("gvt-stream-control: wakeup requested from suspended VM");
    }
    return true;
}

static void gvt_stream_control_wakeup_input_pulse(const char *reason)
{
    static bool toggle;
    int center = INPUT_EVENT_ABS_MAX / 2;
    int delta = INPUT_EVENT_ABS_MAX / 64;
    int pos = center + (toggle ? delta : -delta);

    toggle = !toggle;
    qemu_input_queue_abs(NULL, INPUT_AXIS_X,
                         pos,
                         INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX);
    qemu_input_queue_abs(NULL, INPUT_AXIS_Y,
                         pos,
                         INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX);
    qemu_input_event_sync();
    qemu_input_event_send_key_qcode(NULL, Q_KEY_CODE_WAKE, true);
    qemu_input_event_send_key_qcode(NULL, Q_KEY_CODE_WAKE, false);
    error_report("gvt-stream-control: wakeup input pulse sent reason=%s pos=%d "
                 "key=wake",
                 reason ?: "unknown", pos);
}

static bool gvt_stream_control_apply_start(GVTStreamDisplay *gdpy,
                                           const char *host,
                                           uint64_t port,
                                           const char *codec,
                                           int64_t fps,
                                           int64_t bitrate,
                                           int64_t keyint)
{
    bool was_suspended;

    if (!gdpy || !host || !*host || !port) {
        warn_report("gvt-stream-control: ignoring invalid START target");
        return false;
    }
    if (codec && *codec &&
        g_ascii_strcasecmp(codec, "h264") &&
        g_ascii_strcasecmp(codec, "h265") &&
        g_ascii_strcasecmp(codec, "hevc")) {
        warn_report("gvt-stream-control: ignoring invalid codec %s", codec);
        codec = NULL;
    }

    g_free(gdpy->rtp_host);
    gdpy->rtp_host = g_strdup(host);
    gdpy->rtp_port = port;
    if (codec && *codec) {
        g_free(gdpy->video_codec);
        gdpy->video_codec = g_strdup(!g_ascii_strcasecmp(codec, "hevc") ?
                                     "h265" : codec);
    }
    if (fps >= 1 && fps <= 120) {
        gdpy->encode_fps = fps;
    }
    if (bitrate >= 256 && bitrate <= 100000) {
        gdpy->encode_bitrate = bitrate;
        if (gdpy->encode_still_bitrate <= 0 ||
            gdpy->encode_still_bitrate > gdpy->encode_bitrate) {
            gdpy->encode_still_bitrate = gdpy->low_bandwidth ?
                MAX(512, gdpy->encode_bitrate * 35 / 100) :
                gdpy->encode_bitrate;
        }
        if (gdpy->encode_idle_bitrate >= gdpy->encode_bitrate) {
            gdpy->encode_idle_bitrate = 0;
        }
    }
    if (keyint >= 1 && keyint <= 300) {
        gdpy->encode_keyint = keyint;
    }
    gdpy->last_capture_ms = 0;
    if (gdpy->low_bandwidth) {
        gvt_stream_dirty_reset(gdpy);
    }
    gvt_stream_last_input_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    was_suspended = gvt_stream_wake_if_suspended();
    if (was_suspended || !gdpy->scanout) {
        gvt_stream_control_wakeup_input_pulse(was_suspended ?
                                             "suspended" : "no-scanout");
        gdpy->last_wakeup_pulse_ms = gvt_stream_last_input_ms;
        gdpy->wakeup_pulse_count++;
    }
    error_report("gvt-stream-control: stream target=%s:%u codec=%s "
                 "fps=%d bitrate=%d keyint=%d capture_ms=%" PRIu64,
                 gdpy->rtp_host, (unsigned)gdpy->rtp_port, gdpy->video_codec,
                 gdpy->encode_fps, gdpy->encode_bitrate, gdpy->encode_keyint,
                 gvt_stream_effective_capture_ms(gdpy,
                                                 gvt_stream_last_input_ms));
    error_report("gvt-stream-control: session ports video_udp=%u spice_tcp=%" PRIu64
                 " input_tcp=%" PRIu64,
                 (unsigned)gdpy->rtp_port, gvt_stream_spice_port,
                 gvt_stream_input_port);
    if (!gvt_stream_external_send_start(gdpy, gvt_stream_last_input_ms)) {
        warn_report("gvt-stream-control: external streamd start failed");
        g_clear_pointer(&gdpy->rtp_host, g_free);
        gdpy->rtp_port = 0;
        return false;
    }
    if (gdpy->scanout && !was_suspended) {
        gvt_stream_capture_frame(gdpy, gvt_stream_last_input_ms);
    } else if (gdpy->scanout) {
        error_report("gvt-stream-control: delaying first capture until wakeup pump");
    } else {
        warn_report("gvt-stream-control: stream armed but no dmabuf scanout yet");
    }
    gvt_stream_startup_pump_arm(gdpy, gvt_stream_last_input_ms);
    if ((was_suspended || !gdpy->scanout) && gdpy->startup_pump_until_ms &&
        gdpy->startup_pump_ms < (was_suspended ?
                                 GVT_STREAM_WAKE_PUMP_SUSPENDED_MS :
                                 GVT_STREAM_WAKE_PUMP_NO_SCANOUT_MS)) {
        uint64_t extend_ms = was_suspended ?
                             GVT_STREAM_WAKE_PUMP_SUSPENDED_MS :
                             GVT_STREAM_WAKE_PUMP_NO_SCANOUT_MS;
        gdpy->startup_pump_until_ms = gvt_stream_last_input_ms + extend_ms;
        error_report("gvt-stream-control: startup pump extended after %s "
                     "duration_ms=%" PRIu64,
                     was_suspended ? "wakeup" : "no-scanout", extend_ms);
    }
    return true;
}

static void gvt_stream_control_current_size(GVTStreamDisplay *gdpy,
                                            uint32_t *width,
                                            uint32_t *height)
{
    if (!width || !height) {
        return;
    }
    *width = 0;
    *height = 0;
    if (!gdpy) {
        return;
    }
    if (gdpy->scanout) {
        *width = qemu_dmabuf_get_width(gdpy->scanout);
        *height = qemu_dmabuf_get_height(gdpy->scanout);
        if (*width && *height) {
            return;
        }
    }
    if (gdpy->capture_surface) {
        *width = surface_width(gdpy->capture_surface);
        *height = surface_height(gdpy->capture_surface);
    }
}

static void gvt_stream_control_send_status(GVTStreamControlClient *client,
                                           bool ok,
                                           const char *error)
{
    GVTStreamDisplay *gdpy = gvt_stream_control_display;
    char message[512];
    int64_t start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    uint64_t video_udp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    ssize_t ret;

    if (!client) {
        return;
    }

    if (gdpy && gdpy->rtp_port) {
        video_udp = gdpy->rtp_port;
    } else if (gvt_stream_control_server) {
        video_udp = gvt_stream_control_server->listen_port;
    }
    gvt_stream_control_current_size(gdpy, &width, &height);

    if (ok) {
        snprintf(message, sizeof(message),
                 "{\"ok\":true,\"video_udp\":%" PRIu64
                 ",\"spice_tcp\":%" PRIu64
                 ",\"input_tcp\":%" PRIu64
                 ",\"width\":%u"
                 ",\"height\":%u"
                 ",\"fps\":%d"
                 ",\"bitrate\":%d"
                 ",\"codec\":\"%s\"}\n",
                 video_udp,
                 gvt_stream_spice_port,
                 gvt_stream_input_port,
                 width,
                 height,
                 gdpy ? gdpy->encode_fps : 59,
                 gdpy ? gdpy->encode_bitrate : 18000,
                 (gdpy && gdpy->video_codec) ? gdpy->video_codec : "h265");
    } else {
        snprintf(message, sizeof(message),
                 "{\"ok\":false,\"error\":\"%s\"}\n",
                 error ?: "request failed");
    }

    ret = send(client->fd, message, strlen(message), MSG_NOSIGNAL);
    error_report("gvt-stream-control: status-send ok=%d bytes=%zu ret=%zd "
                 "send_ms=%" PRId64 " since_accept_ms=%" PRId64,
                 ok, strlen(message), ret,
                 qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start_ms,
                 start_ms - client->accepted_ms);
    if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        warn_report("gvt-stream-control: status send failed: %s",
                    strerror(errno));
    }
}

static void gvt_stream_control_process_line(GVTStreamControlClient *client,
                                            const char *line)
{
    int64_t start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t parse_done_ms;
    int64_t apply_start_ms;
    int64_t apply_done_ms;
    Error *err = NULL;
    QObject *obj;
    QDict *dict;
    const char *type;

    if (!line || !*line) {
        return;
    }
    client->messages++;
    error_report("gvt-stream-control: message #%" PRIu64
                 " bytes=%zu since_accept_ms=%" PRId64,
                 client->messages, strlen(line), start_ms - client->accepted_ms);

    obj = qobject_from_json(line, &err);
    if (err) {
        gvt_stream_control_server->parse_errors++;
        warn_report("gvt-stream-control: json parse failed: %s",
                    error_get_pretty(err));
        error_free(err);
        return;
    }

    dict = qobject_to(QDict, obj);
    if (!dict) {
        gvt_stream_control_server->parse_errors++;
        qobject_unref(obj);
        return;
    }
    parse_done_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    type = qdict_get_try_str(dict, "type");
    if (!g_strcmp0(type, "start")) {
        const char *host = qdict_get_try_str(dict, "host") ?: client->host;
        const char *codec = qdict_get_try_str(dict, "codec");
        uint64_t port = qdict_get_try_int(dict, "video_port", 0);
        int64_t fps = gvt_stream_qdict_get_clamped_int(dict, "fps",
                                                       0, 120, 0);
        int64_t bitrate = gvt_stream_qdict_get_clamped_int(dict, "bitrate",
                                                           0, 100000, 0);
        int64_t keyint = gvt_stream_qdict_get_clamped_int(dict, "keyint",
                                                          0, 300, 0);
        bool started;

        if (!port) {
            port = qdict_get_try_int(dict, "port", 0);
        }
        if (!port && gvt_stream_control_server) {
            port = gvt_stream_control_server->listen_port;
        }
        apply_start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        started = gvt_stream_control_apply_start(gvt_stream_control_display,
                                                 host, port, codec,
                                                 fps, bitrate, keyint);
        apply_done_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        gvt_stream_control_send_status(client, started,
                                       "invalid start request");
        error_report("gvt-stream-control: start timing parse_ms=%" PRId64
                     " apply_ms=%" PRId64 " total_ms=%" PRId64,
                     parse_done_ms - start_ms,
                     apply_done_ms - apply_start_ms,
                     qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start_ms);
        if (started) {
            gvt_stream_control_active_client = client;
            gvt_stream_control_server->messages++;
        }
    } else if (!g_strcmp0(type, "status")) {
        gvt_stream_control_send_status(client, true, NULL);
        gvt_stream_control_server->messages++;
    } else if (!g_strcmp0(type, "stop")) {
        if (gvt_stream_control_active_client == client) {
            gvt_stream_control_active_client = NULL;
        }
        gvt_stream_control_apply_stop(gvt_stream_control_display);
        gvt_stream_control_server->messages++;
    } else {
        gvt_stream_control_server->parse_errors++;
        warn_report("gvt-stream-control: ignoring unknown type=%s",
                    type ?: "");
    }

    qobject_unref(obj);
}

static void gvt_stream_control_client_close(GVTStreamControlClient *client)
{
    if (!client) {
        return;
    }
    qemu_set_fd_handler(client->fd, NULL, NULL, NULL);
    close(client->fd);
    if (gvt_stream_control_active_client == client) {
        gvt_stream_control_active_client = NULL;
        gvt_stream_control_apply_stop(gvt_stream_control_display);
    }
    if (gvt_stream_control_server) {
        gvt_stream_control_server->clients =
            g_list_remove(gvt_stream_control_server->clients, client);
    }
    g_string_free(client->buffer, TRUE);
    g_free(client);
}

static void gvt_stream_control_client_read(void *opaque)
{
    GVTStreamControlClient *client = opaque;
    char tmp[4096];

    for (;;) {
        ssize_t ret = read(client->fd, tmp, sizeof(tmp));

        if (ret > 0) {
            char *nl;

            g_string_append_len(client->buffer, tmp, ret);
            while ((nl = strchr(client->buffer->str, '\n'))) {
                g_autofree char *line =
                    g_strndup(client->buffer->str, nl - client->buffer->str);
                g_string_erase(client->buffer, 0,
                               nl - client->buffer->str + 1);
                gvt_stream_control_process_line(client, line);
            }
            if (client->buffer->len > 1024 * 1024) {
                gvt_stream_control_server->parse_errors++;
                warn_report("gvt-stream-control: closing oversized client buffer");
                gvt_stream_control_client_close(client);
                return;
            }
            continue;
        }
        if (ret == 0) {
            gvt_stream_control_client_close(client);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        gvt_stream_control_client_close(client);
        return;
    }
}

static void gvt_stream_control_accept(void *opaque)
{
    GVTStreamControlServer *server = opaque;

    for (;;) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(server->listen_fd, (struct sockaddr *)&addr, &addrlen);
        GVTStreamControlClient *client;
        int one = 1;
        int64_t accept_ms;

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                warn_report("gvt-stream-control: accept failed: %s",
                            strerror(errno));
            }
            return;
        }

        accept_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        qemu_set_cloexec(fd);
        gvt_stream_set_nonblock(fd);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        client = g_new0(GVTStreamControlClient, 1);
        client->fd = fd;
        if (!inet_ntop(AF_INET, &addr.sin_addr, client->host,
                       sizeof(client->host))) {
            snprintf(client->host, sizeof(client->host), "%s", "0.0.0.0");
        }
        client->accepted_ms = accept_ms;
        client->buffer = g_string_new(NULL);
        server->clients = g_list_prepend(server->clients, client);
        server->connected++;
        qemu_set_fd_handler(fd, gvt_stream_control_client_read, NULL, client);
        error_report("gvt-stream-control: client connected from %s fd=%d total=%" PRIu64,
                     client->host, fd, server->connected);
        error_report("gvt-stream-control: advertised ports control_tcp=%" PRIu64
                     " spice_tcp=%" PRIu64 " input_tcp=%" PRIu64,
                     server->listen_port, gvt_stream_spice_port,
                     gvt_stream_input_port);
        /*
         * Most clients send START immediately after connect.  Drain that first
         * line here as well as via the fd handler so the protocol does not
         * depend on a second readability wakeup after accept.
         */
        int64_t poll_start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int poll_ret = poll(&pfd, 1, 1000);
        int64_t poll_done_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        error_report("gvt-stream-control: first-message-poll ret=%d revents=0x%x "
                     "poll_wait_ms=%" PRId64 " since_accept_ms=%" PRId64,
                     poll_ret, pfd.revents, poll_done_ms - poll_start_ms,
                     poll_done_ms - accept_ms);
        if (poll_ret > 0 && (pfd.revents & POLLIN)) {
            gvt_stream_control_client_read(client);
        }
    }
}

static void gvt_stream_control_start(uint64_t port)
{
    const char *host = g_getenv("GVT_STREAM_CONTROL_HOST") ?: "0.0.0.0";
    struct sockaddr_in addr = { 0 };
    int fd;

    if (!port || gvt_stream_control_server) {
        return;
    }

    fd = qemu_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error_report("gvt-stream-control: socket failed: %s", strerror(errno));
        exit(1);
    }

    qemu_set_cloexec(fd);
    gvt_stream_set_nonblock(fd);
    socket_set_fast_reuse(fd);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        error_report("gvt-stream-control: invalid listen host %s", host);
        close(fd);
        exit(1);
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_report("gvt-stream-control: bind %s:%" PRIu64 " failed: %s",
                     host, port, strerror(errno));
        close(fd);
        exit(1);
    }
    if (listen(fd, 8) < 0) {
        error_report("gvt-stream-control: listen failed: %s", strerror(errno));
        close(fd);
        exit(1);
    }

    gvt_stream_control_server = g_new0(GVTStreamControlServer, 1);
    gvt_stream_control_server->listen_fd = fd;
    snprintf(gvt_stream_control_server->listen_host,
             sizeof(gvt_stream_control_server->listen_host), "%s", host);
    gvt_stream_control_server->listen_port = port;
    qemu_set_fd_handler(fd, gvt_stream_control_accept, NULL,
                        gvt_stream_control_server);
    error_report("gvt-stream-control: listening on %s:%" PRIu64, host, port);
    error_report("gvt-stream-control: port map control_tcp=%" PRIu64
                 " video_udp=client-request spice_tcp=%" PRIu64
                 " input_tcp=%" PRIu64,
                 port, gvt_stream_spice_port, gvt_stream_input_port);
}

static void gvt_stream_fourcc_to_str(uint32_t fourcc, char out[5])
{
    int i;

    out[0] = fourcc & 0xff;
    out[1] = (fourcc >> 8) & 0xff;
    out[2] = (fourcc >> 16) & 0xff;
    out[3] = (fourcc >> 24) & 0xff;
    out[4] = 0;

    for (i = 0; i < 4; i++) {
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) {
            out[i] = '.';
        }
    }
}

static void gvt_stream_log_dmabuf(GVTStreamDisplay *gdpy,
                                  const char *event,
                                  QemuDmaBuf *dmabuf)
{
    const uint32_t *offsets;
    const uint32_t *strides;
    const int *fds;
    int n_offsets = 0;
    int n_strides = 0;
    int n_fds = 0;
    uint32_t planes;
    char fourcc[5];
    GString *fd_buf;
    GString *stride_buf;
    GString *offset_buf;
    int i;

    if (!dmabuf) {
        error_report("gvt-stream: %s console=%d dmabuf=NULL",
                     event, qemu_console_get_index(gdpy->dcl.con));
        return;
    }

    planes = qemu_dmabuf_get_num_planes(dmabuf);
    fds = qemu_dmabuf_get_fds(dmabuf, &n_fds);
    offsets = qemu_dmabuf_get_offsets(dmabuf, &n_offsets);
    strides = qemu_dmabuf_get_strides(dmabuf, &n_strides);
    gvt_stream_fourcc_to_str(qemu_dmabuf_get_fourcc(dmabuf), fourcc);

    fd_buf = g_string_new(NULL);
    stride_buf = g_string_new(NULL);
    offset_buf = g_string_new(NULL);
    for (i = 0; i < planes; i++) {
        g_string_append_printf(fd_buf, "%s%d", i ? "," : "",
                               i < n_fds ? fds[i] : -1);
        g_string_append_printf(stride_buf, "%s%u", i ? "," : "",
                               i < n_strides ? strides[i] : 0);
        g_string_append_printf(offset_buf, "%s%u", i ? "," : "",
                               i < n_offsets ? offsets[i] : 0);
    }

    error_report("gvt-stream: %s #%" PRIu64 " console=%d dmabuf=%p "
                 "fds=[%s] planes=%u size=%ux%u backing=%ux%u "
                 "xy=%u,%u strides=[%s] offsets=[%s] fourcc=%s/0x%08x "
                 "modifier=0x%016" PRIx64 " y0_top=%d allow_fences=%d "
                 "fence_fd=%d sync=%p draw_submitted=%d",
                 event, gdpy->scanout_count,
                 qemu_console_get_index(gdpy->dcl.con), dmabuf,
                 fd_buf->str, planes,
                 qemu_dmabuf_get_width(dmabuf),
                 qemu_dmabuf_get_height(dmabuf),
                 qemu_dmabuf_get_backing_width(dmabuf),
                 qemu_dmabuf_get_backing_height(dmabuf),
                 qemu_dmabuf_get_x(dmabuf), qemu_dmabuf_get_y(dmabuf),
                 stride_buf->str, offset_buf->str, fourcc,
                 qemu_dmabuf_get_fourcc(dmabuf),
                 qemu_dmabuf_get_modifier(dmabuf),
                 qemu_dmabuf_get_y0_top(dmabuf),
                 qemu_dmabuf_get_allow_fences(dmabuf),
                 qemu_dmabuf_get_fence_fd(dmabuf),
                 qemu_dmabuf_get_sync(dmabuf),
                 qemu_dmabuf_get_draw_submitted(dmabuf));

    g_string_free(fd_buf, TRUE);
    g_string_free(stride_buf, TRUE);
    g_string_free(offset_buf, TRUE);
}

static uint64_t gvt_stream_checksum_surface(DisplaySurface *surface)
{
    uint8_t *data = surface_data(surface);
    int width = surface_width(surface);
    int height = surface_height(surface);
    int stride = surface_stride(surface);
    uint64_t hash = 1469598103934665603ULL;
    int x, y, b;

    for (y = 0; y < height; y++) {
        const uint8_t *row = data + y * stride;

        for (x = 0; x < width; x++) {
            for (b = 0; b < 4; b++) {
                hash ^= row[x * 4 + b];
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

static const char *gvt_stream_dirty_mode_name(uint32_t mode)
{
    switch (mode) {
    case GVT_STREAM_DIRTY_FULL:
        return "full";
    case GVT_STREAM_DIRTY_STATIC:
        return "static";
    case GVT_STREAM_DIRTY_PARTIAL:
        return "partial";
    case GVT_STREAM_DIRTY_GLOBAL:
        return "global";
    default:
        return "unknown";
    }
}

static void gvt_stream_dirty_reset(GVTStreamDisplay *gdpy)
{
    gdpy->dirty_valid = false;
    gdpy->dirty_prev_valid = false;
    gdpy->dirty_prev_width = 0;
    gdpy->dirty_prev_height = 0;
    gdpy->dirty_prev_stride = 0;
    gdpy->dirty_x = 0;
    gdpy->dirty_y = 0;
    gdpy->dirty_w = 0;
    gdpy->dirty_h = 0;
    gdpy->dirty_mode = GVT_STREAM_DIRTY_UNKNOWN;
    gdpy->dirty_changed_pixels = 0;
    gdpy->dirty_diff_ppm = 0;
    gdpy->dirty_global_frames_left = 0;
    gdpy->dirty_target_bitrate = gdpy->encode_bitrate;
}

static void gvt_stream_dirty_copy_frame(GVTStreamDisplay *gdpy,
                                        DisplaySurface *surface)
{
    uint8_t *data = surface_data(surface);
    int width = surface_width(surface);
    int height = surface_height(surface);
    int stride = surface_stride(surface);
    size_t need;

    if (!data || width <= 0 || height <= 0 || stride <= 0) {
        gvt_stream_dirty_reset(gdpy);
        return;
    }

    need = (size_t)stride * height;
    if (gdpy->dirty_prev_size != need) {
        gdpy->dirty_prev = g_realloc(gdpy->dirty_prev, need);
        gdpy->dirty_prev_size = need;
    }

    memcpy(gdpy->dirty_prev, data, need);
    gdpy->dirty_prev_width = width;
    gdpy->dirty_prev_height = height;
    gdpy->dirty_prev_stride = stride;
    gdpy->dirty_prev_valid = true;
}

static bool gvt_stream_dirty_pixel_changed(const uint8_t *oldp,
                                           const uint8_t *newp,
                                           uint64_t pixel_delta)
{
    int db = abs((int)oldp[0] - (int)newp[0]);
    int dg = abs((int)oldp[1] - (int)newp[1]);
    int dr = abs((int)oldp[2] - (int)newp[2]);

    return (uint64_t)(db + dg + dr) > pixel_delta * 3;
}

static int gvt_stream_dirty_target_bitrate(GVTStreamDisplay *gdpy,
                                           uint32_t mode,
                                           uint64_t diff_ppm)
{
    int still = gdpy->encode_still_bitrate > 0 ?
        gdpy->encode_still_bitrate : gdpy->encode_bitrate;

    if (mode == GVT_STREAM_DIRTY_PARTIAL) {
        uint64_t scaled = (uint64_t)gdpy->encode_bitrate *
            MAX(diff_ppm, 10000ULL) * 3ULL / 1000000ULL;
        int target = (int)MAX((uint64_t)still, scaled);

        return MIN(target, gdpy->encode_bitrate);
    }
    if (mode == GVT_STREAM_DIRTY_STATIC) {
        return still;
    }
    return gdpy->encode_bitrate;
}

static void gvt_stream_update_dirty_state(GVTStreamDisplay *gdpy,
                                          DisplaySurface *surface)
{
    uint8_t *data = surface_data(surface);
    int width = surface_width(surface);
    int height = surface_height(surface);
    int stride = surface_stride(surface);
    uint32_t block = gdpy->dirty_block_size ?: GVT_STREAM_DIRTY_BLOCK_DEFAULT;
    int min_x = width;
    int min_y = height;
    int max_x = -1;
    int max_y = -1;
    uint64_t changed_pixels = 0;
    uint64_t total_pixels;
    uint64_t diff_ppm;
    int by, bx;
    uint32_t mode;

    gdpy->dirty_valid = false;
    gdpy->dirty_mode = GVT_STREAM_DIRTY_UNKNOWN;
    gdpy->dirty_target_bitrate = gdpy->encode_bitrate;

    if (!data || width <= 0 || height <= 0 || stride < width * 4) {
        return;
    }

    total_pixels = (uint64_t)width * height;
    if (!gdpy->dirty_prev_valid ||
        gdpy->dirty_prev_width != width ||
        gdpy->dirty_prev_height != height ||
        gdpy->dirty_prev_stride != stride ||
        gdpy->dirty_prev_size < (size_t)stride * height) {
        gdpy->dirty_x = 0;
        gdpy->dirty_y = 0;
        gdpy->dirty_w = width;
        gdpy->dirty_h = height;
        gdpy->dirty_changed_pixels = total_pixels;
        gdpy->dirty_diff_ppm = 1000000;
        gdpy->dirty_mode = GVT_STREAM_DIRTY_FULL;
        gdpy->dirty_valid = true;
        gdpy->dirty_background_seq++;
        gdpy->dirty_frame_count++;
        gdpy->dirty_full_count++;
        gdpy->dirty_target_bitrate = gdpy->encode_bitrate;
        gvt_stream_dirty_copy_frame(gdpy, surface);
        return;
    }

    block = CLAMP(block, 4u, 128u);
    for (by = 0; by < height; by += block) {
        int ey = MIN(by + (int)block, height);

        for (bx = 0; bx < width; bx += block) {
            int ex = MIN(bx + (int)block, width);
            bool block_changed = false;
            int y;

            for (y = by; y < ey && !block_changed; y++) {
                const uint8_t *old_row =
                    gdpy->dirty_prev + (size_t)y * stride + (size_t)bx * 4;
                const uint8_t *new_row =
                    data + (size_t)y * stride + (size_t)bx * 4;
                int x;

                for (x = bx; x < ex; x++) {
                    if (gvt_stream_dirty_pixel_changed(old_row, new_row,
                                                       gdpy->dirty_pixel_delta)) {
                        block_changed = true;
                        break;
                    }
                    old_row += 4;
                    new_row += 4;
                }
            }

            if (block_changed) {
                min_x = MIN(min_x, bx);
                min_y = MIN(min_y, by);
                max_x = MAX(max_x, ex);
                max_y = MAX(max_y, ey);
                changed_pixels += (uint64_t)(ex - bx) * (ey - by);
            }
        }
    }

    diff_ppm = total_pixels ?
        changed_pixels * 1000000ULL / total_pixels : 1000000ULL;

    if (!changed_pixels) {
        mode = GVT_STREAM_DIRTY_STATIC;
        gdpy->dirty_x = 0;
        gdpy->dirty_y = 0;
        gdpy->dirty_w = 0;
        gdpy->dirty_h = 0;
        gdpy->dirty_static_count++;
    } else if (diff_ppm >= gdpy->dirty_global_min_ppm) {
        mode = GVT_STREAM_DIRTY_GLOBAL;
        gdpy->dirty_global_frames_left =
            gdpy->dirty_global_burst_frames > 1 ?
            gdpy->dirty_global_burst_frames - 1 : 0;
        gdpy->dirty_background_seq++;
        gdpy->dirty_global_count++;
        gdpy->dirty_x = 0;
        gdpy->dirty_y = 0;
        gdpy->dirty_w = width;
        gdpy->dirty_h = height;
    } else if (gdpy->dirty_global_frames_left) {
        mode = GVT_STREAM_DIRTY_GLOBAL;
        gdpy->dirty_global_frames_left--;
        gdpy->dirty_global_count++;
        gdpy->dirty_x = 0;
        gdpy->dirty_y = 0;
        gdpy->dirty_w = width;
        gdpy->dirty_h = height;
    } else if (diff_ppm <= gdpy->dirty_partial_max_ppm) {
        mode = GVT_STREAM_DIRTY_PARTIAL;
        gdpy->dirty_partial_count++;
        gdpy->dirty_x = min_x;
        gdpy->dirty_y = min_y;
        gdpy->dirty_w = max_x - min_x;
        gdpy->dirty_h = max_y - min_y;
    } else {
        mode = GVT_STREAM_DIRTY_FULL;
        gdpy->dirty_full_count++;
        gdpy->dirty_x = 0;
        gdpy->dirty_y = 0;
        gdpy->dirty_w = width;
        gdpy->dirty_h = height;
    }

    gdpy->dirty_mode = mode;
    gdpy->dirty_changed_pixels = changed_pixels;
    gdpy->dirty_diff_ppm = diff_ppm;
    gdpy->dirty_valid = true;
    gdpy->dirty_frame_count++;
    gdpy->dirty_target_bitrate =
        gvt_stream_dirty_target_bitrate(gdpy, mode, diff_ppm);
    gvt_stream_dirty_copy_frame(gdpy, surface);
}

static void gvt_stream_update_idle_sample(GVTStreamDisplay *gdpy,
                                          DisplaySurface *surface,
                                          int64_t now_ms)
{
    uint8_t *data = surface_data(surface);
    int width = surface_width(surface);
    int height = surface_height(surface);
    int stride = surface_stride(surface);
    uint32_t sample[GVT_STREAM_IDLE_SAMPLE_N];
    uint64_t changed = 0;
    int x, y, sx, sy, idx;

    if (!data || width <= 0 || height <= 0) {
        return;
    }

    for (sy = 0; sy < GVT_STREAM_IDLE_SAMPLE_H; sy++) {
        y = GVT_STREAM_IDLE_SAMPLE_H == 1 ? 0 :
            (int)((int64_t)sy * (height - 1) / (GVT_STREAM_IDLE_SAMPLE_H - 1));
        for (sx = 0; sx < GVT_STREAM_IDLE_SAMPLE_W; sx++) {
            const uint8_t *p;

            x = GVT_STREAM_IDLE_SAMPLE_W == 1 ? 0 :
                (int)((int64_t)sx * (width - 1) / (GVT_STREAM_IDLE_SAMPLE_W - 1));
            p = data + (size_t)y * stride + (size_t)x * 4;
            idx = sy * GVT_STREAM_IDLE_SAMPLE_W + sx;
            sample[idx] = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];

            if (gdpy->idle_sample_valid) {
                uint32_t old = gdpy->idle_sample[idx];
                int db = abs((int)(old & 0xff) - (int)(sample[idx] & 0xff));
                int dg = abs((int)((old >> 8) & 0xff) -
                             (int)((sample[idx] >> 8) & 0xff));
                int dr = abs((int)((old >> 16) & 0xff) -
                             (int)((sample[idx] >> 16) & 0xff));

                if ((uint64_t)(db + dg + dr) > gdpy->idle_pixel_delta * 3) {
                    changed++;
                }
            }
        }
    }

    if (!gdpy->idle_sample_valid) {
        memcpy(gdpy->idle_sample, sample, sizeof(sample));
        gdpy->idle_sample_valid = true;
        gdpy->last_content_change_ms = now_ms;
        gdpy->last_probe_diff_ppm = 1000000;
        return;
    }

    gdpy->last_probe_diff_ppm =
        changed * 1000000ULL / GVT_STREAM_IDLE_SAMPLE_N;
    if (gdpy->last_probe_diff_ppm > gdpy->idle_changed_ppm) {
        if (gvt_stream_effective_capture_ms(gdpy, now_ms) > gdpy->capture_ms) {
            gdpy->idle_wake_count++;
        }
        gdpy->last_content_change_ms = now_ms;
        memcpy(gdpy->idle_sample, sample, sizeof(sample));
    }
}

static void gvt_stream_probe_activity(GVTStreamDisplay *gdpy,
                                      QemuDmaBuf *dmabuf,
                                      int64_t now_ms)
{
#ifdef CONFIG_GBM
    uint32_t width, height, texture;
    bool do_idle_probe;
    bool do_dirty_probe;

    if (!dmabuf) {
        return;
    }

    do_dirty_probe = gdpy->low_bandwidth;
    do_idle_probe = gdpy->idle_probe_ms &&
        (!gdpy->last_probe_ms ||
         now_ms - gdpy->last_probe_ms >= gdpy->idle_probe_ms);
    if (!do_dirty_probe && !do_idle_probe) {
        return;
    }

    egl_dmabuf_import_texture(dmabuf);
    texture = qemu_dmabuf_get_texture(dmabuf);
    if (!texture) {
        gdpy->capture_fail_count++;
        return;
    }

    width = qemu_dmabuf_get_width(dmabuf);
    height = qemu_dmabuf_get_height(dmabuf);
    if (!width || !height) {
        return;
    }

    if (gdpy->guest_fb.texture != texture ||
        gdpy->guest_fb.width != width || gdpy->guest_fb.height != height) {
        egl_fb_destroy(&gdpy->guest_fb);
        egl_fb_setup_for_tex(&gdpy->guest_fb, width, height, texture, false);
        gdpy->guest_fb.dmabuf = dmabuf;
    }

    if (gdpy->capture_fb.width != width || gdpy->capture_fb.height != height) {
        egl_fb_destroy(&gdpy->capture_fb);
        egl_fb_setup_new_tex(&gdpy->capture_fb, width, height);
    }

    if (!gdpy->capture_surface ||
        surface_width(gdpy->capture_surface) != width ||
        surface_height(gdpy->capture_surface) != height) {
        g_clear_pointer(&gdpy->capture_surface, qemu_free_displaysurface);
        gdpy->capture_surface = qemu_create_displaysurface(width, height);
    }

    egl_fb_blit(&gdpy->capture_fb, &gdpy->guest_fb,
                qemu_dmabuf_get_y0_top(dmabuf));
    egl_fb_read(gdpy->capture_surface, &gdpy->capture_fb);
    gdpy->last_capture_checksum =
        gvt_stream_checksum_surface(gdpy->capture_surface);
    if (do_idle_probe) {
        gdpy->last_probe_ms = now_ms;
        gdpy->idle_probe_count++;
        gvt_stream_update_idle_sample(gdpy, gdpy->capture_surface, now_ms);
    }
    if (do_dirty_probe) {
        gvt_stream_update_dirty_state(gdpy, gdpy->capture_surface);
    }
#endif
}

static bool gvt_stream_write_ppm(GVTStreamDisplay *gdpy, const char *path)
{
    DisplaySurface *surface = gdpy->capture_surface;
    FILE *fp;
    uint8_t *line;
    uint8_t *data;
    int width;
    int height;
    int stride;
    int x, y;
    bool ok = true;

    fp = fopen(path, "wb");
    if (!fp) {
        error_report("gvt-stream: capture-open-failed path=%s error=%s",
                     path, strerror(errno));
        return false;
    }

    width = surface_width(surface);
    height = surface_height(surface);
    stride = surface_stride(surface);
    data = surface_data(surface);
    line = g_malloc(width * 3);

    if (fprintf(fp, "P6\n%d %d\n255\n", width, height) < 0) {
        ok = false;
        goto out;
    }

    for (y = 0; y < height; y++) {
        const uint8_t *src = data + (height - 1 - y) * stride;

        for (x = 0; x < width; x++) {
            line[x * 3 + 0] = src[x * 4 + 2];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + 0];
        }
        if (fwrite(line, width * 3, 1, fp) != 1) {
            ok = false;
            break;
        }
    }

out:
    if (fclose(fp) != 0) {
        ok = false;
    }
    g_free(line);
    if (!ok) {
        error_report("gvt-stream: capture-write-failed path=%s error=%s",
                     path, strerror(errno));
    }
    return ok;
}


static void gvt_stream_capture_frame(GVTStreamDisplay *gdpy, int64_t now_ms)
{
#ifdef CONFIG_GBM
    QemuDmaBuf *dmabuf = gdpy->scanout;
    uint32_t width, height, texture;
    g_autofree char *path = NULL;
    bool had_texture;
    bool stream_active;
    bool counted = false;
    uint64_t capture_ms;

    stream_active = gdpy->rtp_host && gdpy->rtp_port;
    if (!gdpy->capture_dir && !stream_active) {
        return;
    }
    if (gdpy->capture_max && gdpy->capture_count >= gdpy->capture_max) {
        return;
    }

    capture_ms = gvt_stream_effective_capture_ms(gdpy, now_ms);
    if (gdpy->last_capture_ms &&
        now_ms - gdpy->last_capture_ms < capture_ms) {
        return;
    }

    if (!dmabuf) {
        if (stream_active) {
            gdpy->capture_count++;
            gdpy->last_capture_ms = now_ms;
            gvt_stream_external_send_no_scanout(gdpy, now_ms);
        }
        return;
    }

    gvt_stream_probe_activity(gdpy, dmabuf, now_ms);

    if (stream_active) {
        if (gvt_stream_last_input_seq > 0 &&
            gvt_stream_last_input_capture_seq != gvt_stream_last_input_seq &&
            gvt_stream_last_input_ms > 0 &&
            now_ms - gvt_stream_last_input_ms < 1000) {
            gvt_stream_last_input_capture_seq = gvt_stream_last_input_seq;
            error_report("latency-video-after-input seq=%" PRId64
                         " input_to_capture_ms=%" PRId64
                         " next_capture=%" PRIu64
                         " external_sent=%" PRIu64,
                         gvt_stream_last_input_seq,
                         now_ms - gvt_stream_last_input_ms,
                         gdpy->capture_count + 1,
                         gdpy->external_frame_count);
        }
        gdpy->capture_count++;
        gdpy->last_capture_ms = now_ms;
        if (gdpy->low_bandwidth && gdpy->dirty_valid &&
            gdpy->dirty_mode == GVT_STREAM_DIRTY_STATIC) {
            gdpy->dirty_skip_count++;
            if (gdpy->verbose || gdpy->dirty_skip_count <= 5 ||
                gdpy->dirty_skip_count % 60 == 0) {
                error_report("gvt-stream-lowbw: skip-static #%" PRIu64
                             " seq_next=%" PRIu64 " background=%" PRIu64
                             " ppm=%" PRIu64 " frames=%" PRIu64,
                             gdpy->dirty_skip_count, gdpy->external_seq + 1,
                             gdpy->dirty_background_seq,
                             gdpy->dirty_diff_ppm, gdpy->dirty_frame_count);
            }
            counted = true;
            if (!gdpy->capture_dir) {
                return;
            }
        } else {
            gvt_stream_external_send_frame(gdpy, dmabuf, now_ms);
            counted = true;
            if (!gdpy->capture_dir) {
                return;
            }
        }
    }

    if (!gdpy->capture_dir) {
        return;
    }
    if (!counted) {
        gdpy->capture_count++;
        gdpy->last_capture_ms = now_ms;
    }

    had_texture = qemu_dmabuf_get_texture(dmabuf) != 0;
    egl_dmabuf_import_texture(dmabuf);
    texture = qemu_dmabuf_get_texture(dmabuf);
    if (!texture) {
        gdpy->capture_fail_count++;
        error_report("gvt-stream: capture-import-failed failures=%" PRIu64,
                     gdpy->capture_fail_count);
        return;
    }
    if (!had_texture) {
        gdpy->import_count++;
        error_report("gvt-stream: capture-import-ok #%" PRIu64
                     " console=%d dmabuf=%p texture=%u",
                     gdpy->import_count,
                     qemu_console_get_index(gdpy->dcl.con), dmabuf, texture);
    }

    width = qemu_dmabuf_get_width(dmabuf);
    height = qemu_dmabuf_get_height(dmabuf);

    if (gdpy->guest_fb.texture != texture ||
        gdpy->guest_fb.width != width || gdpy->guest_fb.height != height) {
        egl_fb_destroy(&gdpy->guest_fb);
        egl_fb_setup_for_tex(&gdpy->guest_fb, width, height, texture, false);
        gdpy->guest_fb.dmabuf = dmabuf;
    }

    if (gdpy->capture_fb.width != width || gdpy->capture_fb.height != height) {
        egl_fb_destroy(&gdpy->capture_fb);
        egl_fb_setup_new_tex(&gdpy->capture_fb, width, height);
    }

    if (!gdpy->capture_surface ||
        surface_width(gdpy->capture_surface) != width ||
        surface_height(gdpy->capture_surface) != height) {
        g_clear_pointer(&gdpy->capture_surface, qemu_free_displaysurface);
        gdpy->capture_surface = qemu_create_displaysurface(width, height);
    }

    egl_fb_blit(&gdpy->capture_fb, &gdpy->guest_fb,
                qemu_dmabuf_get_y0_top(dmabuf));
    egl_fb_read(gdpy->capture_surface, &gdpy->capture_fb);

    gdpy->last_capture_checksum =
        gvt_stream_checksum_surface(gdpy->capture_surface);

    path = g_strdup_printf("%s/gvt-stream-%06" PRIu64 "-%ux%u.ppm",
                           gdpy->capture_dir, gdpy->capture_count,
                           width, height);
    if (!gvt_stream_write_ppm(gdpy, path)) {
        gdpy->capture_fail_count++;
        return;
    }

    error_report("gvt-stream: capture-ok #%" PRIu64 " console=%d path=%s "
                 "size=%ux%u checksum=0x%016" PRIx64,
                 gdpy->capture_count, qemu_console_get_index(gdpy->dcl.con),
                 path, width, height, gdpy->last_capture_checksum);
#else
    if (gdpy->capture_dir) {
        gdpy->capture_fail_count++;
        error_report("gvt-stream: capture unavailable without GBM");
    }
#endif
}

static void gvt_stream_startup_pump_stop(GVTStreamDisplay *gdpy)
{
    if (!gdpy) {
        return;
    }
    gdpy->startup_pump_until_ms = 0;
    if (gdpy->startup_pump_timer) {
        timer_del(gdpy->startup_pump_timer);
    }
}

static void gvt_stream_startup_pump_cb(void *opaque)
{
    GVTStreamDisplay *gdpy = opaque;
    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    uint64_t interval_ms;
    bool active;

    if (!gdpy || !gdpy->startup_pump_until_ms) {
        return;
    }

    active = gdpy->rtp_host && gdpy->rtp_port;
    if (!active) {
        gvt_stream_startup_pump_stop(gdpy);
        return;
    }

    if (now_ms <= gdpy->startup_pump_until_ms) {
        if (!gdpy->scanout &&
            (!gdpy->last_wakeup_pulse_ms ||
             now_ms - gdpy->last_wakeup_pulse_ms >= 1000)) {
            gvt_stream_control_wakeup_input_pulse("no-scanout-pump");
            gdpy->last_wakeup_pulse_ms = now_ms;
            gdpy->wakeup_pulse_count++;
        }
        gvt_stream_capture_frame(gdpy, now_ms);
    }

    if (now_ms >= gdpy->startup_pump_until_ms) {
        gdpy->startup_pump_until_ms = 0;
        return;
    }

    interval_ms = gdpy->startup_pump_interval_ms ?:
                  gvt_stream_effective_capture_ms(gdpy, now_ms);
    if (gdpy->startup_pump_interval_ms) {
        uint64_t effective_ms = gvt_stream_effective_capture_ms(gdpy, now_ms);

        if (effective_ms > interval_ms) {
            interval_ms = effective_ms;
        }
    }
    if (!interval_ms) {
        interval_ms = 17;
    }
    timer_mod(gdpy->startup_pump_timer, now_ms + interval_ms);
}

static void gvt_stream_startup_pump_arm(GVTStreamDisplay *gdpy,
                                        int64_t now_ms)
{
    uint64_t interval_ms;
    int64_t until_ms;

    if (!gdpy || !gdpy->startup_pump_ms || !gdpy->startup_pump_timer) {
        return;
    }

    interval_ms = gdpy->startup_pump_interval_ms ?:
                  gvt_stream_effective_capture_ms(gdpy, now_ms);
    if (gdpy->startup_pump_interval_ms) {
        uint64_t effective_ms = gvt_stream_effective_capture_ms(gdpy, now_ms);

        if (effective_ms > interval_ms) {
            interval_ms = effective_ms;
        }
    }
    if (!interval_ms) {
        interval_ms = 17;
    }
    until_ms = now_ms + gdpy->startup_pump_ms;
    if (gdpy->startup_pump_until_ms > until_ms) {
        until_ms = gdpy->startup_pump_until_ms;
    }
    gdpy->startup_pump_until_ms = until_ms;
    timer_mod(gdpy->startup_pump_timer, now_ms + interval_ms);
    error_report("gvt-stream-control: startup pump armed duration_ms=%" PRId64
                 " interval_ms=%" PRIu64,
                 gdpy->startup_pump_until_ms - now_ms, interval_ms);
}

static void gvt_stream_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);
}

static void gvt_stream_gfx_update(DisplayChangeListener *dcl,
                                  int x, int y, int w, int h)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    if (gdpy->verbose) {
        error_report("gvt-stream: gfx-update console=%d rect=%d,%d %dx%d",
                     qemu_console_get_index(dcl->con), x, y, w, h);
    }
}

static void gvt_stream_gfx_switch(DisplayChangeListener *dcl,
                                  DisplaySurface *surface)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    if (!surface) {
        error_report("gvt-stream: gfx-switch console=%d surface=NULL",
                     qemu_console_get_index(dcl->con));
        return;
    }

    if (gdpy->verbose || surface_is_placeholder(surface)) {
        error_report("gvt-stream: gfx-switch console=%d surface=%dx%d "
                     "format=0x%x placeholder=%d",
                     qemu_console_get_index(dcl->con),
                     surface_width(surface), surface_height(surface),
                     surface_format(surface), surface_is_placeholder(surface));
    }
}

static void gvt_stream_scanout_disable(DisplayChangeListener *dcl)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);
    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    bool active = gdpy->rtp_host && gdpy->rtp_port;

    gdpy->scanout_disable_count++;
    gdpy->scanout = NULL;
    gdpy->scanout_lost = true;
    egl_fb_destroy(&gdpy->guest_fb);
    egl_fb_destroy(&gdpy->capture_fb);
    g_clear_pointer(&gdpy->capture_surface, qemu_free_displaysurface);
    gvt_stream_dirty_reset(gdpy);
    if (active) {
        gvt_stream_external_send_no_scanout(gdpy, now_ms);
        gvt_stream_startup_pump_arm(gdpy, now_ms);
    }
    error_report("gvt-stream: scanout-disable #%" PRIu64 " console=%d "
                 "cached=%d active=%d",
                 gdpy->scanout_disable_count, qemu_console_get_index(dcl->con),
                 0, active);
}

static void gvt_stream_scanout_texture(DisplayChangeListener *dcl,
                                       uint32_t backing_id,
                                       bool backing_y_0_top,
                                       uint32_t backing_width,
                                       uint32_t backing_height,
                                       uint32_t x, uint32_t y,
                                       uint32_t w, uint32_t h,
                                       void *d3d_tex2d)
{
    error_report("gvt-stream: scanout-texture console=%d tex=%u "
                 "backing=%ux%u rect=%u,%u %ux%u y0_top=%d d3d=%p",
                 qemu_console_get_index(dcl->con), backing_id,
                 backing_width, backing_height, x, y, w, h,
                 backing_y_0_top, d3d_tex2d);
}

static bool gvt_stream_has_dmabuf(DisplayChangeListener *dcl)
{
    return true;
}

static void gvt_stream_scanout_dmabuf(DisplayChangeListener *dcl,
                                      QemuDmaBuf *dmabuf)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);
    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    gdpy->scanout = dmabuf;
    gdpy->scanout_lost = false;
    gdpy->scanout_count++;
    if (gdpy->verbose || gdpy->scanout_count <= 8) {
        gvt_stream_log_dmabuf(gdpy, "scanout-dmabuf", dmabuf);
    }
    if ((gdpy->import_test || gdpy->capture_dir) && dmabuf) {
#ifdef CONFIG_GBM
        egl_dmabuf_import_texture(dmabuf);
        if (qemu_dmabuf_get_texture(dmabuf)) {
            gdpy->import_count++;
            error_report("gvt-stream: import-test-ok #%" PRIu64
                         " console=%d dmabuf=%p texture=%u",
                         gdpy->import_count, qemu_console_get_index(dcl->con),
                         dmabuf, qemu_dmabuf_get_texture(dmabuf));
        } else {
            gdpy->import_fail_count++;
            error_report("gvt-stream: import-test-failed #%" PRIu64
                         " console=%d dmabuf=%p",
                         gdpy->import_fail_count,
                         qemu_console_get_index(dcl->con), dmabuf);
        }
#else
            gdpy->import_fail_count++;
            error_report("gvt-stream: import-test unavailable without GBM");
#endif
    }
    gvt_stream_capture_frame(gdpy, now_ms);
}

static void gvt_stream_cursor_dmabuf(DisplayChangeListener *dcl,
                                     QemuDmaBuf *dmabuf, bool have_hot,
                                     uint32_t hot_x, uint32_t hot_y)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    gdpy->cursor_count++;
    gdpy->last_activity_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    error_report("gvt-stream: cursor-dmabuf #%" PRIu64 " console=%d "
                 "dmabuf=%p have_hot=%d hot=%u,%u",
                 gdpy->cursor_count, qemu_console_get_index(dcl->con),
                 dmabuf, have_hot, hot_x, hot_y);
    if (gdpy->verbose && dmabuf) {
        gvt_stream_log_dmabuf(gdpy, "cursor-detail", dmabuf);
    }
}

static void gvt_stream_cursor_position(DisplayChangeListener *dcl,
                                       uint32_t pos_x, uint32_t pos_y)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    gdpy->last_activity_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (gdpy->verbose) {
        error_report("gvt-stream: cursor-position console=%d pos=%u,%u",
                     qemu_console_get_index(dcl->con), pos_x, pos_y);
    }
}

static void gvt_stream_release_dmabuf(DisplayChangeListener *dcl,
                                      QemuDmaBuf *dmabuf)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    gdpy->release_count++;
    if (gdpy->scanout == dmabuf) {
        gdpy->scanout = NULL;
        gdpy->scanout_lost = true;
    }
    if ((gdpy->import_test || gdpy->capture_dir) && dmabuf) {
#ifdef CONFIG_GBM
        egl_dmabuf_release_texture(dmabuf);
#endif
    }
    if (gdpy->verbose) {
        error_report("gvt-stream: release-dmabuf #%" PRIu64 " console=%d "
                     "dmabuf=%p",
                     gdpy->release_count, qemu_console_get_index(dcl->con),
                     dmabuf);
    }
}

static void gvt_stream_gl_update(DisplayChangeListener *dcl,
                                 uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);
    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    gdpy->update_count++;
    gdpy->report_updates++;
    gdpy->last_update_ms = now_ms;

    if (!gdpy->last_report_ms) {
        gdpy->last_report_ms = now_ms;
    }

    if (gdpy->verbose) {
        error_report("gvt-stream: gl-update console=%d rect=%u,%u %ux%u "
                     "scanout=%p total=%" PRIu64,
                     qemu_console_get_index(dcl->con), x, y, w, h,
                     gdpy->scanout, gdpy->update_count);
    }

    gvt_stream_capture_frame(gdpy, now_ms);

    if (now_ms - gdpy->last_report_ms >= gdpy->report_ms) {
        int64_t delta = now_ms - gdpy->last_report_ms;
        double fps = delta > 0 ? (double)gdpy->report_updates * 1000.0 / delta : 0.0;

        error_report("gvt-stream: update-stats console=%d updates=%" PRIu64
                     " interval_ms=%" PRId64 " fps=%.2f last_rect=%u,%u %ux%u "
                     "scanouts=%" PRIu64 " cursor=%" PRIu64 " releases=%" PRIu64
                     " imports=%" PRIu64 " import_failures=%" PRIu64
                     " captures=%" PRIu64 " capture_failures=%" PRIu64
                     " scanout_disable=%" PRIu64 " scanout_lost=%d "
                     " encoded=%" PRIu64 " encode_failures=%" PRIu64
                     " dmabuf=%" PRIu64 " cpu=%" PRIu64
                     " external=%d external_sent=%" PRIu64
                     " external_no_scanout=%" PRIu64
                     " external_send_failures=%" PRIu64
                     " streamd_encoded=%" PRIu64
                     " streamd_failures=%" PRIu64
                     " bitrate=%d target_bitrate=%d capture_ms=%" PRIu64
                     " dirty=%s dirty_rect=%u,%u %ux%u dirty_ppm=%" PRIu64
                     " dirty_frames=%" PRIu64 " dirty_static=%" PRIu64
                     " dirty_partial=%" PRIu64 " dirty_full=%" PRIu64
                     " dirty_global=%" PRIu64 " dirty_skipped=%" PRIu64
                     " probe_diff_ppm=%" PRIu64 " probes=%" PRIu64
                     " idle_wakes=%" PRIu64 " wake_pulses=%" PRIu64
                     " input_msgs=%" PRIu64 " input_events=%" PRIu64
                     " input_parse_errors=%" PRIu64 " last_input_age_ms=%" PRId64
                     " last_checksum=0x%016" PRIx64,
                     qemu_console_get_index(dcl->con), gdpy->report_updates,
                     delta, fps, x, y, w, h, gdpy->scanout_count,
                     gdpy->cursor_count, gdpy->release_count,
                     gdpy->import_count, gdpy->import_fail_count,
                     gdpy->capture_count, gdpy->capture_fail_count,
                     gdpy->scanout_disable_count, gdpy->scanout_lost,
                     (uint64_t)0, gdpy->external_encode_failures,
                     (uint64_t)0, (uint64_t)0,
                     1, gdpy->external_frame_count,
                     gdpy->external_no_scanout_count,
                     gdpy->external_send_fail_count,
                     gdpy->external_encoded,
                     gdpy->external_encode_failures,
                     gdpy->encode_bitrate,
                     gdpy->dirty_target_bitrate,
                     gvt_stream_effective_capture_ms(gdpy, now_ms),
                     gvt_stream_dirty_mode_name(gdpy->dirty_mode),
                     gdpy->dirty_x, gdpy->dirty_y,
                     gdpy->dirty_w, gdpy->dirty_h,
                     gdpy->dirty_diff_ppm, gdpy->dirty_frame_count,
                     gdpy->dirty_static_count, gdpy->dirty_partial_count,
                     gdpy->dirty_full_count, gdpy->dirty_global_count,
                     gdpy->dirty_skip_count,
                      gdpy->last_probe_diff_ppm, gdpy->idle_probe_count,
                      gdpy->idle_wake_count, gdpy->wakeup_pulse_count,
                      gvt_stream_input_server ?
                      gvt_stream_input_server->messages : 0,
                      gvt_stream_input_server ?
                      gvt_stream_input_server->events : 0,
                      gvt_stream_input_server ?
                      gvt_stream_input_server->parse_errors : 0,
                      gvt_stream_last_input_ms ?
                      now_ms - gvt_stream_last_input_ms : -1,
                     gdpy->last_capture_checksum);

        gdpy->last_report_ms = now_ms;
        gdpy->report_updates = 0;
    }
}

static const DisplayChangeListenerOps gvt_stream_ops = {
    .dpy_name               = "gvt-stream",
    .dpy_refresh            = gvt_stream_refresh,
    .dpy_gfx_update         = gvt_stream_gfx_update,
    .dpy_gfx_switch         = gvt_stream_gfx_switch,
    .dpy_gl_scanout_disable = gvt_stream_scanout_disable,
    .dpy_gl_scanout_texture = gvt_stream_scanout_texture,
    .dpy_has_dmabuf         = gvt_stream_has_dmabuf,
    .dpy_gl_scanout_dmabuf  = gvt_stream_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf   = gvt_stream_cursor_dmabuf,
    .dpy_gl_cursor_position = gvt_stream_cursor_position,
    .dpy_gl_release_dmabuf  = gvt_stream_release_dmabuf,
    .dpy_gl_update          = gvt_stream_gl_update,
};

static bool gvt_stream_is_compatible_dcl(DisplayGLCtx *dgc,
                                         DisplayChangeListener *dcl)
{
    return dcl->ops == &gvt_stream_ops;
}

static QEMUGLContext gvt_stream_create_context(DisplayGLCtx *dgc,
                                               QEMUGLParams *params)
{
    return qemu_egl_create_context(dgc, params, qemu_egl_rn_ctx);
}

static const DisplayGLCtxOps gvt_stream_gl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = gvt_stream_is_compatible_dcl,
    .dpy_gl_ctx_create            = gvt_stream_create_context,
    .dpy_gl_ctx_destroy           = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current      = qemu_egl_make_context_current,
};

static void early_gvt_stream_init(DisplayOptions *opts)
{
    DisplayGLMode mode = opts->has_gl ? opts->gl : DISPLAY_GL_MODE_ON;

    egl_init(opts->u.gvt_stream.rendernode, mode, &error_fatal);
}

static void gvt_stream_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;
    GVTStreamDisplay *gdpy;
    DisplayGLCtx *ctx;
    const char *host = opts->u.gvt_stream.host ?: "";
    const char *codec = opts->u.gvt_stream.codec ?: "diag";
    uint16_t port = opts->u.gvt_stream.has_port ? opts->u.gvt_stream.port : 0;
    int idx;

    gvt_stream_spice_port =
        gvt_stream_getenv_u64("GVT_STREAM_SPICE_PORT",
                              gvt_stream_default_spice_port(port),
                              0, 65535);
    gvt_stream_input_port =
        gvt_stream_getenv_u64("GVT_STREAM_INPUT_PORT",
                              gvt_stream_default_input_port(port),
                              0, 65535);
    error_report("gvt-stream: init host=%s port=%u codec=%s rendernode=%s "
                 "spice_tcp=%" PRIu64 " input_tcp=%" PRIu64,
                 host, port, codec, opts->u.gvt_stream.rendernode ?: "auto",
                 gvt_stream_spice_port, gvt_stream_input_port);
    gvt_stream_input_start(port);
    if (!host || !*host) {
        gvt_stream_control_start(port);
    }

    for (idx = 0;; idx++) {
        con = qemu_console_lookup_by_index(idx);
        if (!con || !qemu_console_is_graphic(con)) {
            break;
        }

        gdpy = g_new0(GVTStreamDisplay, 1);
        gdpy->dcl.con = con;
        gdpy->dcl.ops = &gvt_stream_ops;
        gdpy->external_fd = -1;
        gdpy->refresh_ms = gvt_stream_getenv_u64("GVT_STREAM_REFRESH_MS",
                                                 17, 1, 1000);
        gdpy->report_ms = gvt_stream_getenv_u64("GVT_STREAM_REPORT_MS",
                                                1000, 100, 60000);
        gdpy->verbose = gvt_stream_getenv_bool("GVT_STREAM_VERBOSE", false);
        gdpy->import_test = gvt_stream_getenv_bool("GVT_STREAM_IMPORT_TEST", false);
        gdpy->low_bandwidth =
            gvt_stream_getenv_bool("GVT_STREAM_LOW_BANDWIDTH", false);
        {
            const char *socket_path = g_getenv("GVT_STREAMD_SOCKET");

            gdpy->external_socket =
                g_strdup((socket_path && *socket_path) ? socket_path :
                         "/tmp/gvt-streamd.sock");
        }
        gdpy->capture_dir = g_strdup(g_getenv("GVT_STREAM_CAPTURE_DIR"));
        if (gdpy->capture_dir && !*gdpy->capture_dir) {
            g_clear_pointer(&gdpy->capture_dir, g_free);
        }
        gdpy->capture_ms = gvt_stream_getenv_u64("GVT_STREAM_CAPTURE_MS",
                                                 17, 16, 60000);
        gdpy->idle_capture_ms =
            gvt_stream_getenv_u64("GVT_STREAM_IDLE_CAPTURE_MS",
                                  gdpy->capture_ms, 16, 60000);
        gdpy->idle_after_ms =
            gvt_stream_getenv_u64("GVT_STREAM_IDLE_AFTER_MS",
                                  0, 0, 60000);
        gdpy->idle_probe_ms =
            gvt_stream_getenv_u64("GVT_STREAM_IDLE_PROBE_MS",
                                  0, 0, 60000);
        gdpy->idle_changed_ppm =
            gvt_stream_getenv_u64("GVT_STREAM_IDLE_CHANGED_PPM",
                                  3000, 0, 1000000);
        gdpy->idle_pixel_delta =
            gvt_stream_getenv_u64("GVT_STREAM_IDLE_PIXEL_DELTA",
                                  8, 0, 255);
        gdpy->dirty_block_size =
            gvt_stream_getenv_u64("GVT_STREAM_DIRTY_BLOCK_SIZE",
                                  GVT_STREAM_DIRTY_BLOCK_DEFAULT, 4, 128);
        gdpy->dirty_pixel_delta =
            gvt_stream_getenv_u64("GVT_STREAM_DIRTY_PIXEL_DELTA",
                                  gdpy->idle_pixel_delta, 0, 255);
        gdpy->dirty_partial_max_ppm =
            gvt_stream_getenv_u64("GVT_STREAM_DIRTY_PARTIAL_MAX_PPM",
                                  GVT_STREAM_DIRTY_PARTIAL_PPM_DEFAULT,
                                  0, 1000000);
        gdpy->dirty_global_min_ppm =
            gvt_stream_getenv_u64("GVT_STREAM_DIRTY_GLOBAL_MIN_PPM",
                                  GVT_STREAM_DIRTY_GLOBAL_PPM_DEFAULT,
                                  0, 1000000);
        gdpy->dirty_global_burst_frames =
            gvt_stream_getenv_u64("GVT_STREAM_DIRTY_GLOBAL_BURST_FRAMES",
                                  2, 1, 10);
        gdpy->capture_max = gvt_stream_getenv_u64("GVT_STREAM_CAPTURE_MAX",
                                                   0, 0, 1000000);
        gdpy->startup_pump_ms =
            gvt_stream_getenv_u64("GVT_STREAM_STARTUP_PUMP_MS",
                                  1500, 0, 10000);
        gdpy->startup_pump_interval_ms =
            gvt_stream_getenv_u64("GVT_STREAM_STARTUP_PUMP_INTERVAL_MS",
                                  gdpy->capture_ms, 1, 1000);
        if (gdpy->startup_pump_ms) {
            gdpy->startup_pump_timer =
                timer_new_ms(QEMU_CLOCK_REALTIME, gvt_stream_startup_pump_cb,
                             gdpy);
        }
        gdpy->encode_fps = gvt_stream_getenv_u64("GVT_STREAM_ENCODE_FPS",
                                                 59, 1, 120);
        gdpy->encode_bitrate = gvt_stream_getenv_u64("GVT_STREAM_ENCODE_BITRATE",
                                                    18000, 256, 100000);
        gdpy->encode_idle_bitrate =
            gvt_stream_getenv_u64("GVT_STREAM_ENCODE_IDLE_BITRATE",
                                  0, 0, 100000);
        {
            uint64_t still_default = gdpy->low_bandwidth ?
                MAX(512ULL, (uint64_t)gdpy->encode_bitrate * 35ULL / 100ULL) :
                (uint64_t)gdpy->encode_bitrate;

            gdpy->encode_still_bitrate =
                gvt_stream_getenv_u64("GVT_STREAM_ENCODE_STILL_BITRATE",
                                      still_default, 0, 100000);
        }
        gdpy->dirty_target_bitrate = gdpy->encode_bitrate;
        gdpy->encode_rate_control =
            g_strdup(g_getenv("GVT_STREAM_ENCODE_RATE_CONTROL") ?: "cbr");
        if (g_ascii_strcasecmp(gdpy->encode_rate_control, "cbr") &&
            g_ascii_strcasecmp(gdpy->encode_rate_control, "vbr") &&
            g_ascii_strcasecmp(gdpy->encode_rate_control, "cqp") &&
            g_ascii_strcasecmp(gdpy->encode_rate_control, "icq") &&
            g_ascii_strcasecmp(gdpy->encode_rate_control, "qvbr")) {
            warn_report("gvt-stream: unknown rate-control %s, falling back to cbr",
                        gdpy->encode_rate_control);
            g_free(gdpy->encode_rate_control);
            gdpy->encode_rate_control = g_strdup("cbr");
        }
        if (gdpy->encode_idle_bitrate >= gdpy->encode_bitrate) {
            gdpy->encode_idle_bitrate = 0;
        }
        if (gdpy->encode_still_bitrate <= 0) {
            gdpy->encode_still_bitrate = gdpy->encode_bitrate;
        }
        gdpy->encode_keyint = gvt_stream_getenv_u64("GVT_STREAM_ENCODE_KEYINT",
                                                    59, 1, 300);
        {
            const char *env_codec = g_getenv("GVT_STREAM_VIDEO_CODEC");
            const char *display_codec =
                (codec && *codec && g_ascii_strcasecmp(codec, "diag")) ?
                codec : "h265";
            gdpy->video_codec = g_strdup(env_codec ?: display_codec);
        }
        if (g_ascii_strcasecmp(gdpy->video_codec, "h264") &&
            g_ascii_strcasecmp(gdpy->video_codec, "h265") &&
            g_ascii_strcasecmp(gdpy->video_codec, "hevc")) {
            warn_report("gvt-stream: unknown codec %s, falling back to h265",
                        gdpy->video_codec);
            g_free(gdpy->video_codec);
            gdpy->video_codec = g_strdup("h265");
        }
        if (!g_ascii_strcasecmp(gdpy->video_codec, "hevc")) {
            g_free(gdpy->video_codec);
            gdpy->video_codec = g_strdup("h265");
        }
        gdpy->rtp_host = g_strdup(g_getenv("GVT_STREAM_RTP_HOST"));
        if (gdpy->rtp_host && !*gdpy->rtp_host) {
            g_clear_pointer(&gdpy->rtp_host, g_free);
        }
        if (!gvt_stream_input_display) {
            gvt_stream_input_display = gdpy;
        }
        if (!gvt_stream_control_display) {
            gvt_stream_control_display = gdpy;
        }
        if (!gdpy->rtp_host && host && *host && port) {
            gdpy->rtp_host = g_strdup(host);
        }
        gdpy->rtp_port = gvt_stream_getenv_u64("GVT_STREAM_RTP_PORT",
                                               gdpy->rtp_host ? port : 0,
                                               0, 65535);
        gdpy->rtp_fec = gvt_stream_getenv_u64("GVT_STREAM_RTP_FEC",
                                               0, 0, 100);
        gdpy->rtp_fec_important =
            gvt_stream_getenv_u64("GVT_STREAM_RTP_FEC_IMPORTANT", 0, 0, 100);
        gdpy->rtp_mtu = gvt_stream_getenv_u64("GVT_STREAM_RTP_MTU",
                                              1400, 576, 1400);
        if (gdpy->rtp_host && !gdpy->rtp_port) {
            warn_report("gvt-stream: disabling RTP, missing port for host %s",
                        gdpy->rtp_host);
            g_clear_pointer(&gdpy->rtp_host, g_free);
        }
        if (gdpy->capture_dir &&
            g_mkdir_with_parents(gdpy->capture_dir, 0755) < 0) {
            warn_report("gvt-stream: disabling capture, mkdir %s failed: %s",
                        gdpy->capture_dir, strerror(errno));
            g_clear_pointer(&gdpy->capture_dir, g_free);
        }
        gdpy->dcl.update_interval = gdpy->refresh_ms;

        ctx = g_new0(DisplayGLCtx, 1);
        ctx->ops = &gvt_stream_gl_ctx_ops;
        qemu_console_set_display_gl_ctx(con, ctx);

        error_report("gvt-stream: listener console=%d refresh_ms=%" PRIu64
                     " report_ms=%" PRIu64 " verbose=%d import_test=%d "
                     "low_bandwidth=%d dirty_block=%u dirty_delta=%" PRIu64
                     " dirty_partial_ppm=%" PRIu64 " dirty_global_ppm=%" PRIu64
                     " dirty_global_burst=%" PRIu64 " "
                     "capture_dir=%s capture_ms=%" PRIu64 " idle_capture_ms=%" PRIu64
                     " idle_after_ms=%" PRIu64 " idle_probe_ms=%" PRIu64
                     " idle_changed_ppm=%" PRIu64 " idle_pixel_delta=%" PRIu64
                     " capture_max=%" PRIu64 " startup_pump_ms=%" PRIu64
                     " startup_pump_interval_ms=%" PRIu64
                     " streamd_socket=%s"
                     " encode_fps=%d codec=%s "
                     "rate_control=%s bitrate=%d idle_bitrate=%d still_bitrate=%d "
                     "rtp=%s:%u mtu=%d",
                     qemu_console_get_index(con), gdpy->refresh_ms,
                     gdpy->report_ms, gdpy->verbose, gdpy->import_test,
                     gdpy->low_bandwidth, gdpy->dirty_block_size,
                     gdpy->dirty_pixel_delta,
                     gdpy->dirty_partial_max_ppm,
                     gdpy->dirty_global_min_ppm,
                     gdpy->dirty_global_burst_frames,
                     gdpy->capture_dir ?: "", gdpy->capture_ms,
                     gdpy->idle_capture_ms,
                     gdpy->idle_after_ms,
                     gdpy->idle_probe_ms, gdpy->idle_changed_ppm,
                     gdpy->idle_pixel_delta, gdpy->capture_max,
                     gdpy->startup_pump_ms, gdpy->startup_pump_interval_ms,
                     gdpy->external_socket ?: "",
                     gdpy->encode_fps, gdpy->video_codec,
                     gdpy->encode_rate_control, gdpy->encode_bitrate,
                     gdpy->encode_idle_bitrate, gdpy->encode_still_bitrate,
                     gdpy->rtp_host ?: "", (unsigned)gdpy->rtp_port,
                     gdpy->rtp_mtu);
        register_displaychangelistener(&gdpy->dcl);
    }
}

static QemuDisplay qemu_display_gvt_stream = {
    .type       = DISPLAY_TYPE_GVT_STREAM,
    .early_init = early_gvt_stream_init,
    .init       = gvt_stream_init,
};

static void register_gvt_stream(void)
{
    qemu_display_register(&qemu_display_gvt_stream);
}

type_init(register_gvt_stream);

module_dep("ui-opengl");
