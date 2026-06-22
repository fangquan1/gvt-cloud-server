/*
 * GVT stream external encoder IPC protocol.
 *
 * v3 is intentionally narrow: one XR24/BGRx DMABUF plane per FRAME message,
 * optional dirty-region/ROI metadata, and one SCM_RIGHTS fd over AF_UNIX
 * SOCK_SEQPACKET.
 */

#ifndef GVT_STREAM_IPC_H
#define GVT_STREAM_IPC_H

#include <stdint.h>

#define GVT_STREAM_IPC_MAGIC 0x47565344u /* GVSD */
#define GVT_STREAM_IPC_VERSION 3

#define GVT_STREAM_IPC_HOST_MAX 64
#define GVT_STREAM_IPC_CODEC_MAX 16
#define GVT_STREAM_IPC_RATE_CONTROL_MAX 16

#define GVT_STREAM_IPC_FLAG_DMABUF_CAPS_FEATURE (1u << 0)
#define GVT_STREAM_IPC_FLAG_ENCODE_FLIP         (1u << 1)
#define GVT_STREAM_IPC_FLAG_DIRTY_VALID         (1u << 2)
#define GVT_STREAM_IPC_FLAG_DIRTY_STATIC        (1u << 3)
#define GVT_STREAM_IPC_FLAG_DIRTY_PARTIAL       (1u << 4)
#define GVT_STREAM_IPC_FLAG_DIRTY_GLOBAL        (1u << 5)
#define GVT_STREAM_IPC_FLAG_LOW_BANDWIDTH       (1u << 6)
#define GVT_STREAM_IPC_FLAG_ENCODE_ROI          (1u << 7)
#define GVT_STREAM_IPC_FLAG_CPU_ROI             (1u << 8)

typedef enum GVTStreamIpcType {
    GVT_STREAM_IPC_START = 1,
    GVT_STREAM_IPC_STOP = 2,
    GVT_STREAM_IPC_FRAME = 3,
    GVT_STREAM_IPC_NO_SCANOUT = 4,
    GVT_STREAM_IPC_STATS = 5,
} GVTStreamIpcType;

typedef enum GVTStreamIpcDirtyMode {
    GVT_STREAM_DIRTY_UNKNOWN = 0,
    GVT_STREAM_DIRTY_FULL = 1,
    GVT_STREAM_DIRTY_STATIC = 2,
    GVT_STREAM_DIRTY_PARTIAL = 3,
    GVT_STREAM_DIRTY_GLOBAL = 4,
} GVTStreamIpcDirtyMode;

typedef struct GVTStreamIpcMessage {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t size;
    uint32_t flags;
    uint64_t seq;
    uint64_t pts_ns;

    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t stride;
    uint32_t offset;
    uint32_t fps;
    uint64_t modifier;

    uint32_t dirty_x;
    uint32_t dirty_y;
    uint32_t dirty_w;
    uint32_t dirty_h;
    uint32_t dirty_ppm;
    uint32_t dirty_mode;
    uint32_t dirty_block_size;
    uint32_t reserved0;
    uint64_t dirty_pixels;
    uint64_t background_seq;

    uint32_t bitrate;
    uint32_t idle_bitrate;
    uint32_t still_bitrate;
    uint32_t keyint;
    uint32_t mtu;
    uint32_t rtp_port;
    uint32_t rtp_fec;
    uint32_t rtp_fec_important;

    uint64_t encoded;
    uint64_t encode_failures;
    uint64_t encoded_bytes;
    uint64_t roi_frames;

    char host[GVT_STREAM_IPC_HOST_MAX];
    char codec[GVT_STREAM_IPC_CODEC_MAX];
    char rate_control[GVT_STREAM_IPC_RATE_CONTROL_MAX];
} GVTStreamIpcMessage;

#endif /* GVT_STREAM_IPC_H */
