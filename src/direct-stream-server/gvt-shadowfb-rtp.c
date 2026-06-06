// SPDX-License-Identifier: MIT
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/ioctl.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#define GVT_SHADOWFB_IOC_MAGIC 'G'

struct gvt_shadowfb_info {
	uint32_t version;
	uint32_t flags;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t fourcc;
	uint64_t modifier;
	uint64_t size;
	uint64_t frame_id;
	uint64_t last_blit_ns;
	uint64_t last_frame_ns;
};

struct gvt_shadowfb_export {
	uint32_t flags;
	int32_t fd;
};

struct gvt_shadowfb_blit {
	uint32_t flags;
	uint32_t timeout_ms;
	uint64_t frame_id;
};

#define GVT_SHADOWFB_IOC_GET_INFO \
	_IOR(GVT_SHADOWFB_IOC_MAGIC, 0x01, struct gvt_shadowfb_info)
#define GVT_SHADOWFB_IOC_EXPORT_DMABUF \
	_IOWR(GVT_SHADOWFB_IOC_MAGIC, 0x02, struct gvt_shadowfb_export)
#define GVT_SHADOWFB_IOC_TRIGGER_BLIT \
	_IOWR(GVT_SHADOWFB_IOC_MAGIC, 0x03, struct gvt_shadowfb_blit)

static volatile sig_atomic_t stop_requested;

static void on_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static int64_t monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_until(int64_t target_ns)
{
	for (;;) {
		int64_t now = monotonic_ns();
		int64_t diff = target_ns - now;
		struct timespec ts;

		if (diff <= 0)
			return;

		ts.tv_sec = diff / 1000000000LL;
		ts.tv_nsec = diff % 1000000000LL;
		nanosleep(&ts, NULL);
	}
}

static void print_usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <client-ip> [port] [fps] [bitrate-kbps]\n"
		"env: SHADOWFB_PATH=/sys/kernel/debug/dri/0/gvt/vgpu1/shadow_fb\n",
		argv0);
}

static uint64_t sample_frame_hash(const uint8_t *base, uint32_t width,
				  uint32_t height, uint32_t stride)
{
	uint64_t h = 1469598103934665603ULL;
	uint32_t gx, gy;

	for (gy = 0; gy < 36; gy++) {
		uint32_t y = height > 1 ? (uint64_t)gy * (height - 1) / 35 : 0;
		const uint8_t *row = base + (size_t)y * stride;

		for (gx = 0; gx < 64; gx++) {
			uint32_t x = width > 1 ? (uint64_t)gx * (width - 1) / 63 : 0;
			uint32_t p;

			memcpy(&p, row + (size_t)x * 4, sizeof(p));
			h ^= p;
			h *= 1099511628211ULL;
		}
	}

	return h;
}

int main(int argc, char **argv)
{
	const char *path = getenv("SHADOWFB_PATH");
	const char *convert_mode = getenv("GVT_CONVERT");
	const char *source_mode = getenv("GVT_SOURCE_MODE");
	const char *client_host;
	int port = 5004;
	int fps = 60;
	int bitrate = 12000;
	int keyframe_period;
	int fec_percentage = 20;
	int fec_percentage_important = 60;
	bool adaptive = true;
	bool wrap_source = false;
	int idle_fps = 8;
	int idle_after_ms = 350;
	int keepalive_ms = 1000;
	struct gvt_shadowfb_info info;
	struct gvt_shadowfb_export exp = { .fd = -1 };
	size_t visible_size;
	void *mapped;
	int shadow_fd;
	int ret;
	GstElement *pipeline;
	GstElement *appsrc;
	GstCaps *caps;
	GstBufferPool *pool = NULL;
	GstStructure *pool_config;
	GstClockTime pts = 0;
	GstClockTime duration;
	char *pipeline_desc;
	GError *error = NULL;
	int64_t frame_period_ns;
	int64_t active_period_ns;
	int64_t idle_period_ns;
	int64_t next_frame_ns;
	int64_t stats_ns;
	int64_t active_until_ns = 0;
	int64_t last_push_ns = 0;
	uint64_t last_hash = 0;
	bool have_hash = false;
	uint64_t frames = 0;
	uint64_t pushed = 0;
	uint64_t skipped = 0;

	if (argc < 2) {
		print_usage(argv[0]);
		return 2;
	}

	client_host = argv[1];
	if (argc > 2)
		port = atoi(argv[2]);
	if (argc > 3)
		fps = atoi(argv[3]);
	if (argc > 4)
		bitrate = atoi(argv[4]);
	if (!path)
		path = "/sys/kernel/debug/dri/0/gvt/vgpu1/shadow_fb";
	if (fps <= 0)
		fps = 60;
	if (getenv("FEC_PERCENTAGE"))
		fec_percentage = atoi(getenv("FEC_PERCENTAGE"));
	if (getenv("FEC_PERCENTAGE_IMPORTANT"))
		fec_percentage_important = atoi(getenv("FEC_PERCENTAGE_IMPORTANT"));
	if (getenv("GVT_ADAPTIVE"))
		adaptive = atoi(getenv("GVT_ADAPTIVE")) != 0;
	if (getenv("GVT_IDLE_FPS"))
		idle_fps = atoi(getenv("GVT_IDLE_FPS"));
	if (getenv("GVT_IDLE_AFTER_MS"))
		idle_after_ms = atoi(getenv("GVT_IDLE_AFTER_MS"));
	if (getenv("GVT_KEEPALIVE_MS"))
		keepalive_ms = atoi(getenv("GVT_KEEPALIVE_MS"));
	if (!source_mode)
		source_mode = "copy";
	if (fec_percentage < 0)
		fec_percentage = 0;
	if (fec_percentage > 100)
		fec_percentage = 100;
	if (fec_percentage_important < 0)
		fec_percentage_important = 0;
	if (fec_percentage_important > 100)
		fec_percentage_important = 100;
	keyframe_period = fps / 2;
	if (keyframe_period < 1)
		keyframe_period = 1;
	if (idle_fps <= 0)
		idle_fps = 1;
	if (idle_fps > fps)
		idle_fps = fps;
	if (idle_after_ms < 0)
		idle_after_ms = 0;
	if (keepalive_ms < 0)
		keepalive_ms = 0;
	wrap_source = !strcmp(source_mode, "wrap");

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	shadow_fd = open(path, O_RDWR | O_CLOEXEC);
	if (shadow_fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return 1;
	}

	memset(&info, 0, sizeof(info));
	if (ioctl(shadow_fd, GVT_SHADOWFB_IOC_GET_INFO, &info) < 0) {
		fprintf(stderr, "GET_INFO failed: %s\n", strerror(errno));
		return 1;
	}

	if (info.fourcc != 0x34325258) {
		fprintf(stderr, "unsupported fourcc=0x%x; expected XR24\n", info.fourcc);
		return 1;
	}

	ret = ioctl(shadow_fd, GVT_SHADOWFB_IOC_EXPORT_DMABUF, &exp);
	if (ret < 0) {
		fprintf(stderr, "EXPORT_DMABUF failed: %s\n", strerror(errno));
		return 1;
	}
	if (exp.fd < 0)
		exp.fd = ret;

	mapped = mmap(NULL, info.size, PROT_READ, MAP_SHARED, exp.fd, 0);
	if (mapped == MAP_FAILED) {
		fprintf(stderr, "mmap dmabuf failed: %s\n", strerror(errno));
		return 1;
	}

	visible_size = (size_t)info.width * info.height * 4;
	duration = gst_util_uint64_scale_int(1, GST_SECOND, fps);
	active_period_ns = 1000000000LL / fps;
	idle_period_ns = 1000000000LL / idle_fps;
	frame_period_ns = active_period_ns;

	gst_init(&argc, &argv);

	if (convert_mode && !strcmp(convert_mode, "vaapi")) {
		if (wrap_source) {
			pipeline_desc = g_strdup_printf(
				"appsrc name=src is-live=true format=time do-timestamp=false block=true max-buffers=1 "
				"! vaapipostproc format=nv12 scale-method=fast "
				"! video/x-raw(memory:VASurface),format=NV12 "
				"! vaapih264enc rate-control=cbr bitrate=%d keyframe-period=%d max-bframes=0 refs=1 cabac=false aud=true "
				"! h264parse config-interval=1 "
				"! rtph264pay pt=96 ssrc=2222 config-interval=1 mtu=1000 "
				"! rtpulpfecenc pt=122 percentage=%d percentage-important=%d multipacket=true "
				"! udpsink host=%s port=%d sync=false async=false",
				bitrate, keyframe_period, fec_percentage, fec_percentage_important, client_host, port);
		} else {
			pipeline_desc = g_strdup_printf(
				"appsrc name=src is-live=true format=time do-timestamp=false block=false "
				"! queue leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 "
				"! vaapipostproc format=nv12 scale-method=fast "
				"! video/x-raw(memory:VASurface),format=NV12 "
				"! vaapih264enc rate-control=cbr bitrate=%d keyframe-period=%d max-bframes=0 refs=1 cabac=false aud=true "
				"! h264parse config-interval=1 "
				"! rtph264pay pt=96 ssrc=2222 config-interval=1 mtu=1000 "
				"! rtpulpfecenc pt=122 percentage=%d percentage-important=%d multipacket=true "
				"! udpsink host=%s port=%d sync=false async=false",
				bitrate, keyframe_period, fec_percentage, fec_percentage_important, client_host, port);
		}
	} else {
		convert_mode = "cpu";
		wrap_source = false;
		pipeline_desc = g_strdup_printf(
			"appsrc name=src is-live=true format=time do-timestamp=false block=false "
			"! queue leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 "
			"! videoconvert n-threads=4 "
			"! video/x-raw,format=NV12 "
			"! vaapih264enc rate-control=cbr bitrate=%d keyframe-period=%d max-bframes=0 refs=1 cabac=false aud=true "
			"! h264parse config-interval=1 "
			"! rtph264pay pt=96 ssrc=2222 config-interval=1 mtu=1000 "
			"! rtpulpfecenc pt=122 percentage=%d percentage-important=%d multipacket=true "
			"! udpsink host=%s port=%d sync=false async=false",
			bitrate, keyframe_period, fec_percentage, fec_percentage_important, client_host, port);
	}

	pipeline = gst_parse_launch(pipeline_desc, &error);
	g_free(pipeline_desc);
	if (!pipeline) {
		fprintf(stderr, "pipeline create failed: %s\n", error ? error->message : "unknown");
		return 1;
	}

	appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
	caps = gst_caps_new_simple("video/x-raw",
				   "format", G_TYPE_STRING, "BGRx",
				   "width", G_TYPE_INT, (int)info.width,
				   "height", G_TYPE_INT, (int)info.height,
				   "framerate", GST_TYPE_FRACTION, fps, 1,
				   NULL);
	gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);

	if (wrap_source && info.stride != info.width * 4) {
		fprintf(stderr, "wrap source requires tightly packed stride; falling back to copy\n");
		wrap_source = false;
	}
	if (!wrap_source) {
		pool = gst_buffer_pool_new();
		pool_config = gst_buffer_pool_get_config(pool);
		gst_buffer_pool_config_set_params(pool_config, caps, visible_size, 3, 6);
		if (!gst_buffer_pool_set_config(pool, pool_config)) {
			fprintf(stderr, "failed to configure buffer pool\n");
			return 1;
		}
		if (!gst_buffer_pool_set_active(pool, TRUE)) {
			fprintf(stderr, "failed to activate buffer pool\n");
			return 1;
		}
	}
	gst_caps_unref(caps);

	if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		fprintf(stderr, "failed to start pipeline\n");
		return 1;
	}

	fprintf(stderr,
		"streaming shadowfb %ux%u stride=%u to %s:%d fps=%d bitrate=%d kbps keyint=%d fec=%d/%d convert=%s source=%s adaptive=%d idle_fps=%d idle_after_ms=%d\n",
		info.width, info.height, info.stride, client_host, port, fps, bitrate,
		keyframe_period, fec_percentage, fec_percentage_important, convert_mode,
		wrap_source ? "wrap" : "copy",
		adaptive ? 1 : 0, idle_fps, idle_after_ms);

	next_frame_ns = monotonic_ns();
	stats_ns = next_frame_ns;
	active_until_ns = next_frame_ns + (int64_t)idle_after_ms * 1000000LL;
	while (!stop_requested) {
		struct gvt_shadowfb_blit blit = {};
		GstBuffer *buf;
		GstMapInfo map;
		GstFlowReturn flow;
		uint8_t *dst;
		const uint8_t *src;
		int64_t now_ns;
		bool active;
		bool changed = true;
		bool keepalive = false;
		uint64_t hash;
		uint32_t y;

		sleep_until(next_frame_ns);
		now_ns = monotonic_ns();
		active = !adaptive || now_ns < active_until_ns;
		frame_period_ns = active ? active_period_ns : idle_period_ns;
		next_frame_ns = now_ns + frame_period_ns;

		if (ioctl(shadow_fd, GVT_SHADOWFB_IOC_TRIGGER_BLIT, &blit) < 0) {
			fprintf(stderr, "TRIGGER_BLIT failed: %s\n", strerror(errno));
			break;
		}

		src = mapped;
		hash = sample_frame_hash(src, info.width, info.height, info.stride);
		if (have_hash) {
			changed = hash != last_hash;
		} else {
			have_hash = true;
		}
		last_hash = hash;
		if (adaptive && changed) {
			active_until_ns = now_ns + (int64_t)idle_after_ms * 1000000LL;
			active = true;
		}
		if (keepalive_ms > 0 && last_push_ns > 0 &&
		    now_ns - last_push_ns >= (int64_t)keepalive_ms * 1000000LL) {
			keepalive = true;
		}
		if (adaptive && !active && !changed && !keepalive) {
			frames++;
			skipped++;
			goto maybe_print_stats;
		}

		if (wrap_source) {
			buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
							 mapped, visible_size, 0,
							 visible_size, NULL, NULL);
			if (!buf)
				break;
		} else {
			if (gst_buffer_pool_acquire_buffer(pool, &buf, NULL) != GST_FLOW_OK)
				break;

			gst_buffer_map(buf, &map, GST_MAP_WRITE);
			dst = map.data;
			for (y = 0; y < info.height; y++)
				memcpy(dst + (size_t)y * info.width * 4,
				       src + (size_t)y * info.stride,
				       (size_t)info.width * 4);
			gst_buffer_unmap(buf, &map);
		}

		GST_BUFFER_PTS(buf) = pts;
		GST_BUFFER_DTS(buf) = pts;
		GST_BUFFER_DURATION(buf) = duration;
		pts += duration;

		flow = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf);
		frames++;
		if (flow == GST_FLOW_OK) {
			pushed++;
			last_push_ns = now_ns;
		} else {
			fprintf(stderr, "appsrc push failed: %s\n", gst_flow_get_name(flow));
			break;
		}

maybe_print_stats:
		if (monotonic_ns() - stats_ns >= 1000000000LL) {
			fprintf(stderr, "shadowfb-rtp: frames=%" PRIu64 " pushed=%" PRIu64
				" skipped=%" PRIu64 " mode=%s changed=%d last_frame=%" PRIu64 "\n",
				frames, pushed, skipped, active ? "active" : "idle",
				changed ? 1 : 0, blit.frame_id);
			frames = 0;
			pushed = 0;
			skipped = 0;
			stats_ns = monotonic_ns();
		}
	}

	gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
	gst_element_set_state(pipeline, GST_STATE_NULL);
	if (pool) {
		gst_buffer_pool_set_active(pool, FALSE);
		gst_object_unref(pool);
	}
	gst_object_unref(appsrc);
	gst_object_unref(pipeline);
	munmap(mapped, info.size);
	close(exp.fd);
	close(shadow_fd);
	return 0;
}
