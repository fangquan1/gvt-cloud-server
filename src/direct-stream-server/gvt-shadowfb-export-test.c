// SPDX-License-Identifier: MIT
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void print_fourcc(uint32_t fourcc)
{
	char s[5] = {
		(char)(fourcc & 0xff),
		(char)((fourcc >> 8) & 0xff),
		(char)((fourcc >> 16) & 0xff),
		(char)((fourcc >> 24) & 0xff),
		0,
	};
	printf("%s", s);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/sys/kernel/debug/dri/0/gvt/vgpu1/shadow_fb";
	struct gvt_shadowfb_info info;
	struct gvt_shadowfb_export exp = { .fd = -1 };
	struct gvt_shadowfb_blit blit = {};
	struct stat st;
	int fd;
	int ret;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return 1;
	}

	memset(&info, 0, sizeof(info));
	ret = ioctl(fd, GVT_SHADOWFB_IOC_GET_INFO, &info);
	if (ret < 0) {
		fprintf(stderr, "GET_INFO failed: %s\n", strerror(errno));
		return 2;
	}

	printf("info: %ux%u stride=%u fourcc=", info.width, info.height, info.stride);
	print_fourcc(info.fourcc);
	printf(" modifier=0x%" PRIx64 " size=%" PRIu64 " frame=%" PRIu64 " last_blit_ns=%" PRIu64 "\n",
	       info.modifier, info.size, info.frame_id, info.last_blit_ns);

	ret = ioctl(fd, GVT_SHADOWFB_IOC_TRIGGER_BLIT, &blit);
	if (ret < 0) {
		fprintf(stderr, "TRIGGER_BLIT failed: %s\n", strerror(errno));
		return 3;
	}
	printf("triggered blit: frame=%" PRIu64 "\n", blit.frame_id);

	ret = ioctl(fd, GVT_SHADOWFB_IOC_EXPORT_DMABUF, &exp);
	if (ret < 0) {
		fprintf(stderr, "EXPORT_DMABUF failed: %s\n", strerror(errno));
		return 4;
	}
	if (exp.fd < 0)
		exp.fd = ret;

	if (fstat(exp.fd, &st) < 0) {
		fprintf(stderr, "fstat dmabuf fd failed: %s\n", strerror(errno));
		return 5;
	}

	printf("exported dmabuf fd=%d size=%lld mode=0%o\n",
	       exp.fd, (long long)st.st_size, st.st_mode & 07777);
	close(exp.fd);
	close(fd);
	return 0;
}

