/* SPDX-License-Identifier: MIT */
#ifndef GVT_SHADOWFB_UAPI_H
#define GVT_SHADOWFB_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define GVT_SHADOWFB_IOC_MAGIC 'G'
#define GVT_SHADOWFB_BUFFER_MASK 0xffu
#define GVT_SHADOWFB_INFO_BUFFER_COUNT(flags) ((flags) & 0xffu)
#define GVT_SHADOWFB_INFO_ACTIVE_BUFFER(flags) (((flags) >> 8) & 0xffu)

struct gvt_shadowfb_info {
	__u32 version;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 stride;
	__u32 fourcc;
	__u64 modifier;
	__u64 size;
	__u64 frame_id;
	__u64 last_blit_ns;
	__u64 last_frame_ns;
};

struct gvt_shadowfb_export {
	__u32 flags;
	__s32 fd;
};

struct gvt_shadowfb_blit {
	__u32 flags;
	__u32 timeout_ms;
	__u64 frame_id;
};

#define GVT_SHADOWFB_IOC_GET_INFO \
	_IOR(GVT_SHADOWFB_IOC_MAGIC, 0x01, struct gvt_shadowfb_info)

#define GVT_SHADOWFB_IOC_EXPORT_DMABUF \
	_IOWR(GVT_SHADOWFB_IOC_MAGIC, 0x02, struct gvt_shadowfb_export)

#define GVT_SHADOWFB_IOC_TRIGGER_BLIT \
	_IOWR(GVT_SHADOWFB_IOC_MAGIC, 0x03, struct gvt_shadowfb_blit)

#endif
