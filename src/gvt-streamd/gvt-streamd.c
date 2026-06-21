/*
 * Standalone GVT-g stream encoder.
 *
 * Receives single-plane XR24/BGRx DMABUF frames from QEMU over a Unix
 * SOCK_SEQPACKET socket and encodes them to RTP with the same GStreamer/VAAPI
 * pipeline shape used by the earlier prototype, now outside QEMU.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <glib.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "../qemu/gvt-stream-ipc.h"

#define GVT_STREAMD_DEFAULT_SOCKET "/tmp/gvt-streamd.sock"
#define GVT_STREAMD_FOURCC_XR24 0x34325258u

typedef struct GVTStreamd {
    char *socket_path;
    int listen_fd;
    int client_fd;

    bool started;
    uint32_t flags;
    uint32_t fps;
    uint32_t bitrate;
    uint32_t idle_bitrate;
    uint32_t still_bitrate;
    uint32_t keyint;
    uint32_t mtu;
    uint32_t rtp_port;
    uint32_t rtp_fec;
    uint32_t rtp_fec_important;
    char host[GVT_STREAM_IPC_HOST_MAX];
    char codec[GVT_STREAM_IPC_CODEC_MAX];
    char rate_control[GVT_STREAM_IPC_RATE_CONTROL_MAX];

    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *encoder;
    GstAllocator *dmabuf_allocator;
    GstClockTime pts;
    GstClockTime duration;
    int64_t last_wall_ms;
    uint64_t frame_interval_ns;
    uint64_t next_frame_due_ns;
    int pipeline_width;
    int pipeline_height;

    int last_fd;
    GVTStreamIpcMessage last_meta;

    uint64_t starts;
    uint64_t stops;
    uint64_t frames;
    uint64_t dropped_frames;
    uint64_t cached_frames;
    uint64_t no_scanout;
    uint64_t failures;
    uint64_t stats_sent;
} GVTStreamd;

static volatile sig_atomic_t stop_requested;

static int64_t streamd_now_ms(void)
{
    return g_get_monotonic_time() / 1000;
}

static uint64_t streamd_now_ns(void)
{
    return (uint64_t)g_get_monotonic_time() * 1000ULL;
}

static uint64_t streamd_frame_interval_ns(uint32_t fps)
{
    if (fps < 1) {
        fps = 59;
    }
    return gst_util_uint64_scale_int(1, GST_SECOND, (int)fps);
}

static uint64_t streamd_frame_time_ns(const GVTStreamIpcMessage *msg)
{
    if (msg && msg->pts_ns) {
        return msg->pts_ns;
    }
    return streamd_now_ns();
}

static void streamd_signal_handler(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int streamd_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool streamd_message_valid(const GVTStreamIpcMessage *msg, ssize_t len)
{
    return len == sizeof(*msg) &&
        msg->magic == GVT_STREAM_IPC_MAGIC &&
        msg->version == GVT_STREAM_IPC_VERSION &&
        msg->size == sizeof(*msg);
}

static const char *streamd_codec(GVTStreamd *s)
{
    return s->codec[0] ? s->codec : "h265";
}

static const char *streamd_rate_control(GVTStreamd *s)
{
    return s->rate_control[0] ? s->rate_control : "cbr";
}

static void streamd_send_stats(GVTStreamd *s)
{
    GVTStreamIpcMessage msg;
    struct iovec iov;
    struct msghdr hdr;
    ssize_t ret;

    if (s->client_fd < 0) {
        return;
    }

    memset(&msg, 0, sizeof(msg));
    msg.magic = GVT_STREAM_IPC_MAGIC;
    msg.version = GVT_STREAM_IPC_VERSION;
    msg.type = GVT_STREAM_IPC_STATS;
    msg.size = sizeof(msg);
    msg.encoded = s->frames;
    msg.encode_failures = s->failures;

    memset(&iov, 0, sizeof(iov));
    iov.iov_base = &msg;
    iov.iov_len = sizeof(msg);
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;

    ret = sendmsg(s->client_fd, &hdr, MSG_NOSIGNAL);
    if (ret == sizeof(msg)) {
        s->stats_sent++;
    }
}

static void streamd_poll_bus(GVTStreamd *s)
{
    GstBus *bus;
    GstMessage *msg;

    if (!s->pipeline) {
        return;
    }

    bus = gst_element_get_bus(s->pipeline);
    while ((msg = gst_bus_pop_filtered(bus,
                                       GST_MESSAGE_ERROR |
                                       GST_MESSAGE_WARNING |
                                       GST_MESSAGE_EOS))) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = NULL;
            gchar *debug = NULL;

            s->failures++;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("gvt-streamd: encode-bus-error from=%s message=%s debug=%s\n",
                       GST_OBJECT_NAME(msg->src),
                       err ? err->message : "unknown",
                       debug ? debug : "");
            g_clear_error(&err);
            g_free(debug);
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
            GError *err = NULL;
            gchar *debug = NULL;

            gst_message_parse_warning(msg, &err, &debug);
            g_printerr("gvt-streamd: encode-bus-warning from=%s message=%s debug=%s\n",
                       GST_OBJECT_NAME(msg->src),
                       err ? err->message : "unknown",
                       debug ? debug : "");
            g_clear_error(&err);
            g_free(debug);
        } else {
            g_printerr("gvt-streamd: encode-bus-eos\n");
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

static void streamd_encoder_finish(GVTStreamd *s)
{
    GstBus *bus;
    GstMessage *msg;

    if (!s->pipeline || !s->appsrc) {
        return;
    }

    gst_app_src_end_of_stream(GST_APP_SRC(s->appsrc));
    bus = gst_element_get_bus(s->pipeline);
    msg = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
                                     GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = NULL;
            gchar *debug = NULL;

            s->failures++;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("gvt-streamd: encode-error message=%s debug=%s\n",
                       err ? err->message : "unknown",
                       debug ? debug : "");
            g_clear_error(&err);
            g_free(debug);
        } else {
            g_printerr("gvt-streamd: encode-eos frames=%" PRIu64 "\n",
                       s->frames);
        }
        gst_message_unref(msg);
    } else {
        s->failures++;
        g_printerr("gvt-streamd: encode-eos-timeout frames=%" PRIu64 "\n",
                   s->frames);
    }

    gst_object_unref(bus);
    gst_element_set_state(s->pipeline, GST_STATE_NULL);
    if (s->encoder) {
        gst_object_unref(s->encoder);
    }
    gst_object_unref(s->appsrc);
    gst_object_unref(s->pipeline);
    s->encoder = NULL;
    s->appsrc = NULL;
    s->pipeline = NULL;
    s->pipeline_width = 0;
    s->pipeline_height = 0;
    s->pts = 0;
    s->last_wall_ms = 0;
}

static bool streamd_encoder_start(GVTStreamd *s, uint32_t width,
                                  uint32_t height)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *pipeline_desc = NULL;
    GstCaps *caps;
    GstStateChangeReturn state_ret;
    bool encode_h265 = !g_ascii_strcasecmp(streamd_codec(s), "h265") ||
        !g_ascii_strcasecmp(streamd_codec(s), "hevc");
    const char *encoder = encode_h265 ? "vaapih265enc" : "vaapih264enc";
    const char *encoder_opts = encode_h265 ?
        "max-bframes=0 refs=1 " :
        "max-bframes=0 refs=1 cabac=false aud=true ";
    const char *parser = encode_h265 ? "h265parse" : "h264parse";
    const char *payloader = encode_h265 ? "rtph265pay" : "rtph264pay";

    if (!s->started || !s->host[0] || !s->rtp_port) {
        return false;
    }
    if (s->pipeline &&
        s->pipeline_width == (int)width &&
        s->pipeline_height == (int)height) {
        return true;
    }
    if (s->pipeline) {
        g_printerr("gvt-streamd: encode-restart old_size=%dx%d new_size=%ux%u\n",
                   s->pipeline_width, s->pipeline_height, width, height);
        streamd_encoder_finish(s);
    }

    if (s->rtp_fec || s->rtp_fec_important) {
        pipeline_desc = g_strdup_printf(
            "appsrc name=src is-live=true format=time do-timestamp=false block=false "
            "! queue leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 "
            "! vaapipostproc format=nv12 scale-method=fast "
            "! video/x-raw(memory:VASurface),format=NV12 "
            "! %s name=enc rate-control=%s bitrate=%u keyframe-period=%u "
            "%s"
            "! %s config-interval=1 "
            "! %s pt=96 ssrc=2222 config-interval=1 mtu=%u "
            "! rtpulpfecenc pt=122 percentage=%u percentage-important=%u multipacket=true "
            "! udpsink host=%s port=%u sync=false async=false",
            encoder, streamd_rate_control(s), s->bitrate, s->keyint,
            encoder_opts, parser, payloader, s->mtu,
            s->rtp_fec, s->rtp_fec_important, s->host, s->rtp_port);
    } else {
        pipeline_desc = g_strdup_printf(
            "appsrc name=src is-live=true format=time do-timestamp=false block=false "
            "! queue leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 "
            "! vaapipostproc format=nv12 scale-method=fast "
            "! video/x-raw(memory:VASurface),format=NV12 "
            "! %s name=enc rate-control=%s bitrate=%u keyframe-period=%u "
            "%s"
            "! %s config-interval=1 "
            "! %s pt=96 ssrc=2222 config-interval=1 mtu=%u "
            "! udpsink host=%s port=%u sync=false async=false",
            encoder, streamd_rate_control(s), s->bitrate, s->keyint,
            encoder_opts, parser, payloader, s->mtu, s->host, s->rtp_port);
    }

    s->pipeline = gst_parse_launch(pipeline_desc, &error);
    if (error || !s->pipeline) {
        s->failures++;
        g_printerr("gvt-streamd: encode-pipeline-create-failed error=%s desc=%s\n",
                   error ? error->message : "unknown", pipeline_desc);
        if (s->pipeline) {
            gst_object_unref(s->pipeline);
            s->pipeline = NULL;
        }
        return false;
    }

    s->appsrc = gst_bin_get_by_name(GST_BIN(s->pipeline), "src");
    if (!s->appsrc) {
        s->failures++;
        g_printerr("gvt-streamd: encode-appsrc-not-found\n");
        gst_object_unref(s->pipeline);
        s->pipeline = NULL;
        return false;
    }
    s->encoder = gst_bin_get_by_name(GST_BIN(s->pipeline), "enc");
    if (!s->encoder) {
        g_printerr("gvt-streamd: encode-encoder-not-found adaptive bitrate disabled\n");
    }

    caps = gst_caps_new_simple("video/x-raw",
                               "format", G_TYPE_STRING, "BGRx",
                               "width", G_TYPE_INT, (int)width,
                               "height", G_TYPE_INT, (int)height,
                               "framerate", GST_TYPE_FRACTION,
                               (int)s->fps, 1,
                               NULL);
    if (s->flags & GVT_STREAM_IPC_FLAG_DMABUF_CAPS_FEATURE) {
        gst_caps_set_features(caps, 0,
                              gst_caps_features_new("memory:DMABuf", NULL));
    }
    gst_app_src_set_caps(GST_APP_SRC(s->appsrc), caps);
    gst_caps_unref(caps);

    s->duration = gst_util_uint64_scale_int(1, GST_SECOND, (int)s->fps);
    state_ret = gst_element_set_state(s->pipeline, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        s->failures++;
        g_printerr("gvt-streamd: encode-pipeline-start-failed\n");
        if (s->encoder) {
            gst_object_unref(s->encoder);
        }
        gst_object_unref(s->appsrc);
        gst_object_unref(s->pipeline);
        s->encoder = NULL;
        s->appsrc = NULL;
        s->pipeline = NULL;
        return false;
    }

    s->pipeline_width = (int)width;
    s->pipeline_height = (int)height;
    g_printerr("gvt-streamd: encode-start codec=%s rtp=%s:%u size=%ux%u "
               "fps=%u rate_control=%s bitrate=%u keyint=%u mtu=%u fec=%u/%u "
               "dmabuf_caps=%d\n",
               streamd_codec(s), s->host, s->rtp_port, width, height,
               s->fps, streamd_rate_control(s), s->bitrate, s->keyint,
               s->mtu, s->rtp_fec, s->rtp_fec_important,
               !!(s->flags & GVT_STREAM_IPC_FLAG_DMABUF_CAPS_FEATURE));
    return true;
}

static void streamd_stamp_buffer(GVTStreamd *s, GstBuffer *buf)
{
    int64_t now_ms = streamd_now_ms();
    GstClockTime duration = s->duration;

    if (s->last_wall_ms) {
        int64_t delta_ms = now_ms - s->last_wall_ms;

        if (delta_ms < 1) {
            delta_ms = 1;
        } else if (delta_ms > 1000) {
            delta_ms = 1000;
        }
        duration = (GstClockTime)delta_ms * GST_MSECOND;
    }

    GST_BUFFER_PTS(buf) = s->pts;
    GST_BUFFER_DTS(buf) = s->pts;
    GST_BUFFER_DURATION(buf) = duration;
    s->pts += duration;
    s->last_wall_ms = now_ms;
}

static void streamd_cache_last_frame(GVTStreamd *s,
                                     const GVTStreamIpcMessage *msg,
                                     int fd)
{
    int keep_fd;

    if (fd < 0) {
        return;
    }

    keep_fd = dup(fd);
    if (keep_fd < 0) {
        s->failures++;
        g_printerr("gvt-streamd: cache-dup-failed fd=%d error=%s\n",
                   fd, strerror(errno));
        return;
    }

    if (s->last_fd >= 0) {
        close(s->last_fd);
    }
    s->last_fd = keep_fd;
    s->last_meta = *msg;
}

static bool streamd_should_push_frame(GVTStreamd *s,
                                      const GVTStreamIpcMessage *msg,
                                      const char *source)
{
    uint64_t frame_ns;
    uint64_t interval_ns;

    interval_ns = s->frame_interval_ns ?:
                  streamd_frame_interval_ns(s->fps);
    if (!interval_ns) {
        return true;
    }

    frame_ns = streamd_frame_time_ns(msg);
    if (!s->next_frame_due_ns) {
        s->next_frame_due_ns = frame_ns + interval_ns;
        return true;
    }

    if (frame_ns >= s->next_frame_due_ns) {
        do {
            s->next_frame_due_ns += interval_ns;
        } while (s->next_frame_due_ns <= frame_ns);
        return true;
    }

    s->dropped_frames++;
    if (s->dropped_frames <= 5 || s->dropped_frames % 60 == 0) {
        g_printerr("gvt-streamd: frame-drop #=%" PRIu64
                   " source=%s target_fps=%u next_due_ms=%" PRIu64
                   " frame_ms=%" PRIu64 " encoded=%" PRIu64 "\n",
                   s->dropped_frames, source ?: "live", s->fps,
                   (uint64_t)(s->next_frame_due_ns / 1000000ULL),
                   (uint64_t)(frame_ns / 1000000ULL), s->frames);
    }
    return false;
}

static bool streamd_push_dmabuf(GVTStreamd *s,
                                const GVTStreamIpcMessage *msg,
                                int fd,
                                const char *source)
{
    GstBuffer *buf;
    GstMemory *mem;
    GstFlowReturn flow;
    gsize plane_offsets[GST_VIDEO_MAX_PLANES] = { 0 };
    gint plane_strides[GST_VIDEO_MAX_PLANES] = { 0 };
    size_t size;

    if (!msg || !msg->width || !msg->height ||
        msg->fourcc != GVT_STREAMD_FOURCC_XR24 || !msg->stride) {
        s->failures++;
        if (fd >= 0) {
            close(fd);
        }
        g_printerr("gvt-streamd: frame-unsupported source=%s size=%ux%u "
                   "fourcc=0x%08x stride=%u\n",
                   source ?: "unknown", msg ? msg->width : 0,
                   msg ? msg->height : 0, msg ? msg->fourcc : 0,
                   msg ? msg->stride : 0);
        return false;
    }

    if (fd < 0) {
        if (s->last_fd < 0) {
            return false;
        }
        fd = dup(s->last_fd);
        if (fd < 0) {
            s->failures++;
            g_printerr("gvt-streamd: cached-dup-failed error=%s\n",
                       strerror(errno));
            return false;
        }
    } else {
        streamd_cache_last_frame(s, msg, fd);
    }

    if (!streamd_should_push_frame(s, msg, source)) {
        if (fd >= 0) {
            close(fd);
        }
        return true;
    }

    if (!streamd_encoder_start(s, msg->width, msg->height)) {
        close(fd);
        return false;
    }
    if (!s->dmabuf_allocator) {
        s->dmabuf_allocator = gst_dmabuf_allocator_new();
        if (!s->dmabuf_allocator) {
            s->failures++;
            close(fd);
            g_printerr("gvt-streamd: dmabuf-allocator-create-failed\n");
            return false;
        }
    }

    size = (size_t)msg->offset + (size_t)msg->stride * msg->height;
    mem = gst_dmabuf_allocator_alloc(s->dmabuf_allocator, fd, size);
    if (!mem) {
        s->failures++;
        close(fd);
        g_printerr("gvt-streamd: dmabuf-memory-alloc-failed size=%zu\n",
                   size);
        return false;
    }

    buf = gst_buffer_new();
    gst_buffer_append_memory(buf, mem);
    plane_offsets[0] = msg->offset;
    plane_strides[0] = msg->stride;
    gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE,
                                   GST_VIDEO_FORMAT_BGRx,
                                   msg->width, msg->height, 1,
                                   plane_offsets, plane_strides);
    streamd_stamp_buffer(s, buf);

    flow = gst_app_src_push_buffer(GST_APP_SRC(s->appsrc), buf);
    streamd_poll_bus(s);
    if (flow != GST_FLOW_OK) {
        s->failures++;
        g_printerr("gvt-streamd: dmabuf-push-failed source=%s flow=%s\n",
                   source ?: "unknown", gst_flow_get_name(flow));
        return false;
    }

    s->frames++;
    if (!g_strcmp0(source, "cached")) {
        s->cached_frames++;
    }
    if (s->frames <= 5 || s->frames % 60 == 0) {
        g_printerr("gvt-streamd: dmabuf-push-ok #=%" PRIu64
                   " source=%s size=%ux%u stride=%u cached=%" PRIu64
                   " dropped=%" PRIu64 " failures=%" PRIu64 "\n",
                   s->frames, source ?: "live", msg->width, msg->height,
                   msg->stride, s->cached_frames, s->dropped_frames,
                   s->failures);
        streamd_send_stats(s);
    }
    return true;
}

static void streamd_apply_start(GVTStreamd *s, const GVTStreamIpcMessage *msg)
{
    s->starts++;
    s->started = true;
    s->flags = msg->flags;
    s->fps = (msg->fps >= 1 && msg->fps <= 120) ? msg->fps : 59;
    s->bitrate = msg->bitrate ?: 18000;
    s->idle_bitrate = msg->idle_bitrate;
    s->still_bitrate = msg->still_bitrate ?: s->bitrate;
    s->keyint = msg->keyint ?: 59;
    s->frame_interval_ns = streamd_frame_interval_ns(s->fps);
    s->next_frame_due_ns = 0;
    s->mtu = msg->mtu ?: 1400;
    s->rtp_port = msg->rtp_port;
    s->rtp_fec = msg->rtp_fec;
    s->rtp_fec_important = msg->rtp_fec_important;
    g_strlcpy(s->host, msg->host, sizeof(s->host));
    g_strlcpy(s->codec, msg->codec[0] ? msg->codec : "h265",
              sizeof(s->codec));
    if (!g_ascii_strcasecmp(s->codec, "hevc")) {
        g_strlcpy(s->codec, "h265", sizeof(s->codec));
    }
    g_strlcpy(s->rate_control,
              msg->rate_control[0] ? msg->rate_control : "cbr",
              sizeof(s->rate_control));

    streamd_encoder_finish(s);
    g_printerr("gvt-streamd: start #%" PRIu64 " target=%s:%u codec=%s "
               "fps=%u interval_ns=%" PRIu64
               " bitrate=%u keyint=%u mtu=%u fec=%u/%u flags=0x%x\n",
               s->starts, s->host, s->rtp_port, s->codec, s->fps,
               s->frame_interval_ns, s->bitrate, s->keyint, s->mtu,
               s->rtp_fec, s->rtp_fec_important, s->flags);
}

static void streamd_apply_stop(GVTStreamd *s)
{
    s->stops++;
    s->started = false;
    s->next_frame_due_ns = 0;
    streamd_encoder_finish(s);
    streamd_send_stats(s);
    g_printerr("gvt-streamd: stop #%" PRIu64 " frames=%" PRIu64
               " cached=%" PRIu64 " dropped=%" PRIu64
               " failures=%" PRIu64 "\n",
               s->stops, s->frames, s->cached_frames, s->dropped_frames,
               s->failures);
}

static void streamd_apply_no_scanout(GVTStreamd *s,
                                     const GVTStreamIpcMessage *msg)
{
    (void)msg;
    s->no_scanout++;
    if (s->last_fd >= 0) {
        streamd_push_dmabuf(s, &s->last_meta, -1, "cached");
    }
    if (s->no_scanout <= 5 || s->no_scanout % 60 == 0) {
        g_printerr("gvt-streamd: no-scanout #%" PRIu64
                   " has_cached=%d frames=%" PRIu64 "\n",
                   s->no_scanout, s->last_fd >= 0, s->frames);
    }
}

static void streamd_handle_message(GVTStreamd *s,
                                   const GVTStreamIpcMessage *msg,
                                   int fd)
{
    switch (msg->type) {
    case GVT_STREAM_IPC_START:
        if (fd >= 0) {
            close(fd);
        }
        streamd_apply_start(s, msg);
        break;
    case GVT_STREAM_IPC_STOP:
        if (fd >= 0) {
            close(fd);
        }
        streamd_apply_stop(s);
        break;
    case GVT_STREAM_IPC_FRAME:
        streamd_push_dmabuf(s, msg, fd, "live");
        break;
    case GVT_STREAM_IPC_NO_SCANOUT:
        if (fd >= 0) {
            close(fd);
        }
        streamd_apply_no_scanout(s, msg);
        break;
    default:
        if (fd >= 0) {
            close(fd);
        }
        s->failures++;
        g_printerr("gvt-streamd: ignoring unknown message type=%u\n",
                   msg->type);
        break;
    }
}

static int streamd_recv_fd(struct msghdr *hdr)
{
    struct cmsghdr *cmsg;

    for (cmsg = CMSG_FIRSTHDR(hdr); cmsg; cmsg = CMSG_NXTHDR(hdr, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int fd = -1;

            memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
            return fd;
        }
    }
    return -1;
}

static bool streamd_read_client(GVTStreamd *s)
{
    for (;;) {
        GVTStreamIpcMessage msg;
        struct iovec iov;
        struct msghdr hdr;
        char control[CMSG_SPACE(sizeof(int))];
        ssize_t ret;
        int fd;

        memset(&msg, 0, sizeof(msg));
        memset(&iov, 0, sizeof(iov));
        iov.iov_base = &msg;
        iov.iov_len = sizeof(msg);
        memset(&hdr, 0, sizeof(hdr));
        memset(control, 0, sizeof(control));
        hdr.msg_iov = &iov;
        hdr.msg_iovlen = 1;
        hdr.msg_control = control;
        hdr.msg_controllen = sizeof(control);

        ret = recvmsg(s->client_fd, &hdr, MSG_DONTWAIT);
        if (ret > 0) {
            fd = streamd_recv_fd(&hdr);
            if (!streamd_message_valid(&msg, ret)) {
                s->failures++;
                if (fd >= 0) {
                    close(fd);
                }
                g_printerr("gvt-streamd: invalid message bytes=%zd\n", ret);
                continue;
            }
            streamd_handle_message(s, &msg, fd);
            continue;
        }
        if (ret == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        g_printerr("gvt-streamd: recvmsg failed: %s\n", strerror(errno));
        return false;
    }
}

static void streamd_close_client(GVTStreamd *s)
{
    if (s->client_fd >= 0) {
        close(s->client_fd);
        s->client_fd = -1;
    }
    s->started = false;
    s->next_frame_due_ns = 0;
    streamd_encoder_finish(s);
}

static void streamd_accept_client(GVTStreamd *s)
{
    for (;;) {
        int fd = accept(s->listen_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                g_printerr("gvt-streamd: accept failed: %s\n", strerror(errno));
            }
            return;
        }

        streamd_set_nonblock(fd);
        if (s->client_fd >= 0) {
            g_printerr("gvt-streamd: replacing existing client\n");
            streamd_close_client(s);
        }
        s->client_fd = fd;
        g_printerr("gvt-streamd: client connected fd=%d\n", fd);
    }
}

static bool streamd_listen(GVTStreamd *s)
{
    struct sockaddr_un addr = { 0 };

    if (strlen(s->socket_path) >= sizeof(addr.sun_path)) {
        g_printerr("gvt-streamd: socket path too long: %s\n", s->socket_path);
        return false;
    }

    unlink(s->socket_path);
    s->listen_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (s->listen_fd < 0) {
        g_printerr("gvt-streamd: socket failed: %s\n", strerror(errno));
        return false;
    }
    streamd_set_nonblock(s->listen_fd);

    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, s->socket_path, sizeof(addr.sun_path));
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        g_printerr("gvt-streamd: bind %s failed: %s\n",
                   s->socket_path, strerror(errno));
        close(s->listen_fd);
        s->listen_fd = -1;
        return false;
    }
    chmod(s->socket_path, 0600);
    if (listen(s->listen_fd, 1) < 0) {
        g_printerr("gvt-streamd: listen failed: %s\n", strerror(errno));
        close(s->listen_fd);
        s->listen_fd = -1;
        return false;
    }

    g_printerr("gvt-streamd: listening socket=%s\n", s->socket_path);
    return true;
}

static void streamd_run(GVTStreamd *s)
{
    while (!stop_requested) {
        struct pollfd fds[2];
        nfds_t nfds = 0;
        int ret;

        memset(fds, 0, sizeof(fds));
        fds[nfds].fd = s->listen_fd;
        fds[nfds].events = POLLIN;
        nfds++;
        if (s->client_fd >= 0) {
            fds[nfds].fd = s->client_fd;
            fds[nfds].events = POLLIN | POLLHUP | POLLERR;
            nfds++;
        }

        ret = poll(fds, nfds, 500);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            g_printerr("gvt-streamd: poll failed: %s\n", strerror(errno));
            break;
        }
        if (ret == 0) {
            streamd_poll_bus(s);
            continue;
        }

        if (fds[0].revents & POLLIN) {
            streamd_accept_client(s);
        }
        if (nfds > 1 && fds[1].revents) {
            if ((fds[1].revents & (POLLHUP | POLLERR)) ||
                !streamd_read_client(s)) {
                g_printerr("gvt-streamd: client disconnected\n");
                streamd_close_client(s);
            }
        }
        streamd_poll_bus(s);
    }
}

static void streamd_cleanup(GVTStreamd *s)
{
    streamd_close_client(s);
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
    if (s->socket_path) {
        unlink(s->socket_path);
    }
    if (s->last_fd >= 0) {
        close(s->last_fd);
        s->last_fd = -1;
    }
    if (s->dmabuf_allocator) {
        gst_object_unref(s->dmabuf_allocator);
        s->dmabuf_allocator = NULL;
    }
    g_free(s->socket_path);
}

static void streamd_usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [--socket PATH]\n", argv0);
}

int main(int argc, char **argv)
{
    GVTStreamd s;
    const char *socket_path;
    int i;

    memset(&s, 0, sizeof(s));
    s.listen_fd = -1;
    s.client_fd = -1;
    s.last_fd = -1;
    s.fps = 59;
    s.bitrate = 18000;
    s.still_bitrate = 18000;
    s.keyint = 59;
    s.mtu = 1400;
    s.frame_interval_ns = streamd_frame_interval_ns(s.fps);
    g_strlcpy(s.codec, "h265", sizeof(s.codec));
    g_strlcpy(s.rate_control, "cbr", sizeof(s.rate_control));

    socket_path = g_getenv("GVT_STREAMD_SOCKET");
    s.socket_path = g_strdup((socket_path && *socket_path) ?
                             socket_path : GVT_STREAMD_DEFAULT_SOCKET);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            g_free(s.socket_path);
            s.socket_path = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            streamd_usage(argv[0]);
            return 0;
        } else {
            streamd_usage(argv[0]);
            return 2;
        }
    }

    gst_init(&argc, &argv);
    signal(SIGINT, streamd_signal_handler);
    signal(SIGTERM, streamd_signal_handler);

    if (!streamd_listen(&s)) {
        streamd_cleanup(&s);
        return 1;
    }

    streamd_run(&s);
    streamd_cleanup(&s);
    g_printerr("gvt-streamd: exit frames=%" PRIu64 " cached=%" PRIu64
               " failures=%" PRIu64 " stats=%" PRIu64 "\n",
               s.frames, s.cached_frames, s.failures, s.stats_sent);
    return 0;
}
