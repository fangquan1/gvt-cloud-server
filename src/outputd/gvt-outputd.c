// Minimal GVT-g DMABUF output daemon.
//
// QEMU sends one primary-plane DMABUF fd plus metadata over a Unix datagram
// socket. This daemon owns DRM/KMS and can atomically switch the active source.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>

#define GVT_OUTPUTD_MAGIC 0x4756544f
#define GVT_OUTPUTD_VERSION 1
#define GVT_OUTPUTD_MSG_FRAME 1
#define GVT_OUTPUTD_MSG_SELECT 2
#define GVT_OUTPUTD_MSG_CURSOR_POS 4
#define GVT_OUTPUTD_SOURCE_LEN 32
#define GVT_OUTPUTD_MAX_SOURCES 8
#define GVT_OUTPUTD_CURSOR_W 64
#define GVT_OUTPUTD_CURSOR_H 64

typedef struct GVTOutputdFrameMsg {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t stride;
    uint32_t offset;
    uint64_t modifier;
    uint64_t sequence;
    char source[GVT_OUTPUTD_SOURCE_LEN];
} GVTOutputdFrameMsg;

typedef struct Source {
    char name[GVT_OUTPUTD_SOURCE_LEN];
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t stride;
    uint32_t offset;
    uint64_t modifier;
    uint64_t frames;
    uint32_t cursor_x;
    uint32_t cursor_y;
    bool cursor_seen;
    uint32_t fb_id;
    bool seen;
} Source;

typedef struct KmsState {
    int fd;
    char connector_name[64];
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t crtc_index;
    uint32_t primary_plane_id;
    uint32_t mode_blob_id;
    drmModeModeInfo mode;
    bool ready;
    bool active;
    bool cursor_ready;
    bool cursor_visible;
    uint32_t cursor_handle;
    uint32_t cursor_width;
    uint32_t cursor_height;

    uint32_t conn_crtc_id_prop;
    uint32_t crtc_active_prop;
    uint32_t crtc_mode_id_prop;
    uint32_t plane_fb_id_prop;
    uint32_t plane_crtc_id_prop;
    uint32_t plane_src_x_prop;
    uint32_t plane_src_y_prop;
    uint32_t plane_src_w_prop;
    uint32_t plane_src_h_prop;
    uint32_t plane_crtc_x_prop;
    uint32_t plane_crtc_y_prop;
    uint32_t plane_crtc_w_prop;
    uint32_t plane_crtc_h_prop;
} KmsState;

typedef struct Daemon {
    int sock;
    char socket_path[108];
    char status_path[256];
    char requested_connector[64];
    KmsState kms;
    Source sources[GVT_OUTPUTD_MAX_SOURCES];
    Source *active;
    uint64_t received;
    uint64_t presented;
    uint64_t failed;
    time_t last_report;
    time_t last_status;
} Daemon;

static const char *fourcc_str(uint32_t fourcc, char out[5])
{
    out[0] = fourcc & 0xff;
    out[1] = (fourcc >> 8) & 0xff;
    out[2] = (fourcc >> 16) & 0xff;
    out[3] = (fourcc >> 24) & 0xff;
    out[4] = 0;
    return out;
}

static uint32_t get_prop(int fd, uint32_t object_id, uint32_t object_type,
                         const char *name)
{
    drmModeObjectProperties *props;
    uint32_t id = 0;

    props = drmModeObjectGetProperties(fd, object_id, object_type);
    if (!props) {
        return 0;
    }
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) {
            continue;
        }
        if (!strcmp(prop->name, name)) {
            id = prop->prop_id;
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static void connector_name(drmModeConnector *conn, char *out, size_t out_len)
{
    const char *type = "UNK";

    switch (conn->connector_type) {
    case DRM_MODE_CONNECTOR_HDMIA:
        type = "HDMI-A";
        break;
    case DRM_MODE_CONNECTOR_DisplayPort:
        type = "DP";
        break;
    case DRM_MODE_CONNECTOR_eDP:
        type = "eDP";
        break;
    default:
        break;
    }
    snprintf(out, out_len, "%s-%u", type, conn->connector_type_id);
}

static bool pick_mode(drmModeConnector *conn, uint32_t width, uint32_t height,
                      drmModeModeInfo *mode)
{
    int fallback = -1;

    for (int i = 0; i < conn->count_modes; i++) {
        if (conn->modes[i].hdisplay == width &&
            conn->modes[i].vdisplay == height) {
            *mode = conn->modes[i];
            return true;
        }
        if (fallback < 0 &&
            (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED)) {
            fallback = i;
        }
    }
    if (fallback < 0 && conn->count_modes > 0) {
        fallback = 0;
    }
    if (fallback >= 0) {
        *mode = conn->modes[fallback];
        return true;
    }
    return false;
}

static uint32_t crtc_index(drmModeRes *res, uint32_t crtc_id)
{
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) {
            return (uint32_t)i;
        }
    }
    return UINT32_MAX;
}

static uint32_t pick_crtc(int fd, drmModeRes *res, drmModeConnector *conn)
{
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc) {
            uint32_t crtc = enc->crtc_id;
            drmModeFreeEncoder(enc);
            if (crtc) {
                return crtc;
            }
        }
    }
    for (int i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!enc) {
            continue;
        }
        for (int j = 0; j < res->count_crtcs; j++) {
            if (enc->possible_crtcs & (1u << j)) {
                uint32_t crtc = res->crtcs[j];
                drmModeFreeEncoder(enc);
                return crtc;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return 0;
}

static bool plane_supports(drmModePlane *plane, uint32_t fourcc)
{
    for (uint32_t i = 0; i < plane->count_formats; i++) {
        if (plane->formats[i] == fourcc) {
            return true;
        }
    }
    return false;
}

static uint32_t find_primary_plane(KmsState *kms, uint32_t fourcc)
{
    drmModePlaneRes *planes = drmModeGetPlaneResources(kms->fd);
    uint32_t found = 0;

    if (!planes) {
        return 0;
    }
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(kms->fd, planes->planes[i]);
        uint64_t type = 0;
        if (!plane) {
            continue;
        }
        if (!(plane->possible_crtcs & (1u << kms->crtc_index)) ||
            !plane_supports(plane, fourcc)) {
            drmModeFreePlane(plane);
            continue;
        }

        drmModeObjectProperties *props =
            drmModeObjectGetProperties(kms->fd, plane->plane_id,
                                       DRM_MODE_OBJECT_PLANE);
        if (props) {
            for (uint32_t j = 0; j < props->count_props; j++) {
                drmModePropertyPtr prop =
                    drmModeGetProperty(kms->fd, props->props[j]);
                if (!prop) {
                    continue;
                }
                if (!strcmp(prop->name, "type")) {
                    type = props->prop_values[j];
                }
                drmModeFreeProperty(prop);
            }
            drmModeFreeObjectProperties(props);
        }
        if (type == DRM_PLANE_TYPE_PRIMARY) {
            found = plane->plane_id;
            drmModeFreePlane(plane);
            break;
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(planes);
    return found;
}

static bool init_kms(Daemon *d, Source *src)
{
    KmsState *kms = &d->kms;
    drmModeRes *res = NULL;
    drmModeConnector *conn = NULL;
    char name[64] = { 0 };

    if (kms->ready) {
        return true;
    }
    if (kms->fd < 0) {
        kms->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (kms->fd < 0) {
            fprintf(stderr, "gvt-outputd: open card0 failed: %s\n",
                    strerror(errno));
            return false;
        }
        drmSetClientCap(kms->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        if (drmSetClientCap(kms->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
            fprintf(stderr, "gvt-outputd: atomic cap failed: %s\n",
                    strerror(errno));
            return false;
        }
    }

    res = drmModeGetResources(kms->fd);
    if (!res) {
        return false;
    }
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *candidate =
            drmModeGetConnector(kms->fd, res->connectors[i]);
        if (!candidate) {
            continue;
        }
        connector_name(candidate, name, sizeof(name));
        if (candidate->connection == DRM_MODE_CONNECTED &&
            candidate->count_modes > 0 &&
            (!d->requested_connector[0] ||
             !strcmp(d->requested_connector, name))) {
            conn = candidate;
            break;
        }
        drmModeFreeConnector(candidate);
    }
    if (!conn) {
        drmModeFreeResources(res);
        return false;
    }

    kms->connector_id = conn->connector_id;
    snprintf(kms->connector_name, sizeof(kms->connector_name), "%s", name);
    kms->crtc_id = pick_crtc(kms->fd, res, conn);
    kms->crtc_index = crtc_index(res, kms->crtc_id);
    if (!kms->crtc_id || kms->crtc_index == UINT32_MAX ||
        !pick_mode(conn, src->width, src->height, &kms->mode)) {
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return false;
    }
    kms->primary_plane_id = find_primary_plane(kms, src->fourcc);
    if (!kms->primary_plane_id) {
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return false;
    }

    kms->conn_crtc_id_prop = get_prop(kms->fd, kms->connector_id,
                                      DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    kms->crtc_active_prop = get_prop(kms->fd, kms->crtc_id,
                                     DRM_MODE_OBJECT_CRTC, "ACTIVE");
    kms->crtc_mode_id_prop = get_prop(kms->fd, kms->crtc_id,
                                      DRM_MODE_OBJECT_CRTC, "MODE_ID");
    kms->plane_fb_id_prop = get_prop(kms->fd, kms->primary_plane_id,
                                     DRM_MODE_OBJECT_PLANE, "FB_ID");
    kms->plane_crtc_id_prop = get_prop(kms->fd, kms->primary_plane_id,
                                       DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    kms->plane_src_x_prop = get_prop(kms->fd, kms->primary_plane_id,
                                     DRM_MODE_OBJECT_PLANE, "SRC_X");
    kms->plane_src_y_prop = get_prop(kms->fd, kms->primary_plane_id,
                                     DRM_MODE_OBJECT_PLANE, "SRC_Y");
    kms->plane_src_w_prop = get_prop(kms->fd, kms->primary_plane_id,
                                     DRM_MODE_OBJECT_PLANE, "SRC_W");
    kms->plane_src_h_prop = get_prop(kms->fd, kms->primary_plane_id,
                                     DRM_MODE_OBJECT_PLANE, "SRC_H");
    kms->plane_crtc_x_prop = get_prop(kms->fd, kms->primary_plane_id,
                                      DRM_MODE_OBJECT_PLANE, "CRTC_X");
    kms->plane_crtc_y_prop = get_prop(kms->fd, kms->primary_plane_id,
                                      DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    kms->plane_crtc_w_prop = get_prop(kms->fd, kms->primary_plane_id,
                                      DRM_MODE_OBJECT_PLANE, "CRTC_W");
    kms->plane_crtc_h_prop = get_prop(kms->fd, kms->primary_plane_id,
                                      DRM_MODE_OBJECT_PLANE, "CRTC_H");

    if (!kms->conn_crtc_id_prop || !kms->crtc_active_prop ||
        !kms->crtc_mode_id_prop || !kms->plane_fb_id_prop ||
        !kms->plane_crtc_id_prop || !kms->plane_src_x_prop ||
        !kms->plane_src_y_prop || !kms->plane_src_w_prop ||
        !kms->plane_src_h_prop || !kms->plane_crtc_x_prop ||
        !kms->plane_crtc_y_prop || !kms->plane_crtc_w_prop ||
        !kms->plane_crtc_h_prop ||
        drmModeCreatePropertyBlob(kms->fd, &kms->mode, sizeof(kms->mode),
                                  &kms->mode_blob_id) != 0) {
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return false;
    }

    kms->ready = true;
    fprintf(stderr, "gvt-outputd: kms-ready connector=%s crtc=%u plane=%u "
            "mode=%ux%u source=%s\n",
            kms->connector_name, kms->crtc_id, kms->primary_plane_id,
            kms->mode.hdisplay, kms->mode.vdisplay, src->name);

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return true;
}

static bool atomic_add(drmModeAtomicReq *req, uint32_t obj, uint32_t prop,
                       uint64_t value)
{
    return drmModeAtomicAddProperty(req, obj, prop, value) >= 0;
}

static void draw_cursor(uint32_t *pixels, uint32_t width, uint32_t height)
{
    static const char *shape[] = {
        "B...............................",
        "BB..............................",
        "BWB.............................",
        "BWWB............................",
        "BWWWB...........................",
        "BWWWWB..........................",
        "BWWWWWB.........................",
        "BWWWWWWB........................",
        "BWWWWWWWB.......................",
        "BWWWWWWWWB......................",
        "BWWWWWWWWWB.....................",
        "BWWWWWWWWWWB....................",
        "BWWWWWWWWWWWB...................",
        "BWWWWWWBBBBBB...................",
        "BWWWBWWB........................",
        "BWWB.BWWB.......................",
        "BWB..BWWB.......................",
        "BB...BWWWB......................",
        "B.....BWWB......................",
        "......BWWB......................",
        ".......BB.......................",
    };
    const uint32_t white = 0xffffffff;
    const uint32_t black = 0xff000000;
    const size_t rows = sizeof(shape) / sizeof(shape[0]);

    memset(pixels, 0, width * height * sizeof(uint32_t));
    for (uint32_t y = 0; y < height && y < rows; y++) {
        const char *row = shape[y];
        for (uint32_t x = 0; x < width && row[x]; x++) {
            if (row[x] == 'B') {
                pixels[y * width + x] = black;
            } else if (row[x] == 'W') {
                pixels[y * width + x] = white;
            }
        }
    }
}

static bool ensure_cursor(KmsState *kms)
{
    struct drm_mode_create_dumb create = { 0 };
    struct drm_mode_map_dumb map = { 0 };
    struct drm_mode_destroy_dumb destroy = { 0 };
    uint32_t *pixels;

    if (kms->cursor_ready) {
        return true;
    }
    if (kms->fd < 0 || !kms->crtc_id) {
        return false;
    }

    create.width = GVT_OUTPUTD_CURSOR_W;
    create.height = GVT_OUTPUTD_CURSOR_H;
    create.bpp = 32;
    if (ioctl(kms->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        return false;
    }

    map.handle = create.handle;
    if (ioctl(kms->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        destroy.handle = create.handle;
        ioctl(kms->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return false;
    }

    pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  kms->fd, map.offset);
    if (pixels == MAP_FAILED) {
        destroy.handle = create.handle;
        ioctl(kms->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return false;
    }
    draw_cursor(pixels, GVT_OUTPUTD_CURSOR_W, GVT_OUTPUTD_CURSOR_H);
    munmap(pixels, create.size);

    if (drmModeSetCursor2(kms->fd, kms->crtc_id, create.handle,
                          GVT_OUTPUTD_CURSOR_W, GVT_OUTPUTD_CURSOR_H,
                          0, 0) != 0) {
        destroy.handle = create.handle;
        ioctl(kms->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return false;
    }

    kms->cursor_handle = create.handle;
    kms->cursor_width = GVT_OUTPUTD_CURSOR_W;
    kms->cursor_height = GVT_OUTPUTD_CURSOR_H;
    kms->cursor_ready = true;
    kms->cursor_visible = true;
    return true;
}

static void move_cursor(Daemon *d, Source *src)
{
    KmsState *kms = &d->kms;
    int x;
    int y;

    if (!src || !src->cursor_seen || !kms->ready) {
        return;
    }
    if (!ensure_cursor(kms)) {
        d->failed++;
        return;
    }
    x = (int)((uint64_t)src->cursor_x * kms->mode.hdisplay / 0x7fff);
    y = (int)((uint64_t)src->cursor_y * kms->mode.vdisplay / 0x7fff);
    if (drmModeMoveCursor(kms->fd, kms->crtc_id, x, y) != 0) {
        d->failed++;
    }
}

static bool present_source(Daemon *d, Source *src)
{
    KmsState *kms = &d->kms;
    drmModeAtomicReq *req;
    uint32_t flags = 0;
    bool ok;
    int commit_errno = 0;

    if (!src || !src->fb_id || !init_kms(d, src)) {
        return false;
    }

    req = drmModeAtomicAlloc();
    if (!req) {
        return false;
    }
    if (!kms->active) {
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
        ok = atomic_add(req, kms->connector_id, kms->conn_crtc_id_prop,
                        kms->crtc_id) &&
             atomic_add(req, kms->crtc_id, kms->crtc_active_prop, 1) &&
             atomic_add(req, kms->crtc_id, kms->crtc_mode_id_prop,
                        kms->mode_blob_id);
        if (!ok) {
            drmModeAtomicFree(req);
            return false;
        }
    }
    ok = atomic_add(req, kms->primary_plane_id, kms->plane_fb_id_prop,
                    src->fb_id) &&
         atomic_add(req, kms->primary_plane_id, kms->plane_crtc_id_prop,
                    kms->crtc_id) &&
         atomic_add(req, kms->primary_plane_id, kms->plane_src_x_prop, 0) &&
         atomic_add(req, kms->primary_plane_id, kms->plane_src_y_prop, 0) &&
         atomic_add(req, kms->primary_plane_id, kms->plane_src_w_prop,
                    (uint64_t)src->width << 16) &&
         atomic_add(req, kms->primary_plane_id, kms->plane_src_h_prop,
                    (uint64_t)src->height << 16) &&
         atomic_add(req, kms->primary_plane_id, kms->plane_crtc_x_prop, 0) &&
         atomic_add(req, kms->primary_plane_id, kms->plane_crtc_y_prop, 0) &&
         atomic_add(req, kms->primary_plane_id, kms->plane_crtc_w_prop,
                    kms->mode.hdisplay) &&
         atomic_add(req, kms->primary_plane_id, kms->plane_crtc_h_prop,
                    kms->mode.vdisplay);
    if (ok && drmModeAtomicCommit(kms->fd, req, flags, NULL) == 0) {
        kms->active = true;
        d->presented++;
        ok = true;
    } else {
        commit_errno = errno;
        if (d->failed < 8 || d->failed % 60 == 0) {
            fprintf(stderr, "gvt-outputd: atomic-present failed source=%s "
                    "fb=%u size=%ux%u mode=%ux%u flags=0x%x props_ok=%d "
                    "errno=%d (%s)\n",
                    src->name, src->fb_id, src->width, src->height,
                    kms->mode.hdisplay, kms->mode.vdisplay, flags, ok,
                    commit_errno, strerror(commit_errno));
        }
        d->failed++;
        ok = false;
    }
    drmModeAtomicFree(req);
    if (ok) {
        move_cursor(d, src);
    }
    return ok;
}

static void write_status(Daemon *d);

static Source *get_source(Daemon *d, const char *name)
{
    Source *empty = NULL;

    for (int i = 0; i < GVT_OUTPUTD_MAX_SOURCES; i++) {
        if (d->sources[i].seen && !strcmp(d->sources[i].name, name)) {
            return &d->sources[i];
        }
        if (!d->sources[i].seen && !empty) {
            empty = &d->sources[i];
        }
    }
    if (!empty) {
        return NULL;
    }
    memset(empty, 0, sizeof(*empty));
    snprintf(empty->name, sizeof(empty->name), "%s", name);
    empty->seen = true;
    return empty;
}

static void handle_frame(Daemon *d, GVTOutputdFrameMsg *msg, int fd)
{
    Source *src = get_source(d, msg->source);
    uint32_t handle = 0;
    uint32_t handles[4] = { 0 };
    uint32_t strides[4] = { 0 };
    uint32_t offsets[4] = { 0 };
    uint64_t modifiers[4] = { 0 };
    uint32_t fb_id = 0;
    char fourcc[5];

    d->received++;
    if (!src) {
        d->failed++;
        close(fd);
        return;
    }

    src->width = msg->width;
    src->height = msg->height;
    src->fourcc = msg->fourcc;
    src->stride = msg->stride;
    src->offset = msg->offset;
    src->modifier = msg->modifier;
    src->frames++;
    if (!d->active) {
        d->active = src;
    }

    if (!init_kms(d, src)) {
        if (d->received <= 8 || d->received % 60 == 0) {
            fprintf(stderr, "gvt-outputd: source=%s frame=%" PRIu64
                    " size=%ux%u fourcc=%s no-kms-yet\n",
                    src->name, src->frames, src->width, src->height,
                    fourcc_str(src->fourcc, fourcc));
        }
        close(fd);
        return;
    }

    if (drmPrimeFDToHandle(d->kms.fd, fd, &handle) != 0) {
        if (d->failed < 8 || d->failed % 60 == 0) {
            fprintf(stderr, "gvt-outputd: prime-fd-to-handle failed "
                    "source=%s size=%ux%u fourcc=%s stride=%u "
                    "modifier=0x%016" PRIx64 " errno=%d (%s)\n",
                    src->name, src->width, src->height,
                    fourcc_str(src->fourcc, fourcc), src->stride,
                    src->modifier, errno, strerror(errno));
        }
        d->failed++;
        close(fd);
        return;
    }
    close(fd);
    handles[0] = handle;
    strides[0] = src->stride;
    offsets[0] = src->offset;
    modifiers[0] = src->modifier;

    if (drmModeAddFB2WithModifiers(d->kms.fd, src->width, src->height,
                                   src->fourcc, handles, strides, offsets,
                                   modifiers, &fb_id,
                                   DRM_MODE_FB_MODIFIERS) != 0) {
        if (d->failed < 8 || d->failed % 60 == 0) {
            fprintf(stderr, "gvt-outputd: addfb2-modifiers failed "
                    "source=%s size=%ux%u fourcc=%s stride=%u offset=%u "
                    "modifier=0x%016" PRIx64 " handle=%u errno=%d (%s)\n",
                    src->name, src->width, src->height,
                    fourcc_str(src->fourcc, fourcc), src->stride, src->offset,
                    src->modifier, handle, errno, strerror(errno));
        }
        d->failed++;
        drmCloseBufferHandle(d->kms.fd, handle);
        return;
    }
    drmCloseBufferHandle(d->kms.fd, handle);

    if (src == d->active) {
        uint32_t old = src->fb_id;
        src->fb_id = fb_id;
        if (!present_source(d, src)) {
            drmModeRmFB(d->kms.fd, fb_id);
            src->fb_id = old;
            return;
        }
        if (old) {
            drmModeRmFB(d->kms.fd, old);
        }
    } else {
        if (src->fb_id) {
            drmModeRmFB(d->kms.fd, src->fb_id);
        }
        src->fb_id = fb_id;
    }
}

static void handle_cursor_pos(Daemon *d, GVTOutputdFrameMsg *msg)
{
    Source *src = get_source(d, msg->source);

    if (!src) {
        d->failed++;
        return;
    }
    src->cursor_x = msg->width;
    src->cursor_y = msg->height;
    src->cursor_seen = true;
    if (src == d->active) {
        move_cursor(d, src);
    }
}

static void select_source(Daemon *d, const char *name)
{
    Source *src = get_source(d, name);

    if (!src || !src->seen) {
        fprintf(stderr, "gvt-outputd: select unknown source=%s\n", name);
        return;
    }
    d->active = src;
    fprintf(stderr, "gvt-outputd: selected source=%s fb=%u\n",
            src->name, src->fb_id);
    present_source(d, src);
    move_cursor(d, src);
    write_status(d);
}

static void write_status(Daemon *d)
{
    char tmp[320];
    FILE *f;

    if (!d->status_path[0]) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", d->status_path);
    f = fopen(tmp, "w");
    if (!f) {
        return;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"active\": \"%s\",\n", d->active ? d->active->name : "");
    fprintf(f, "  \"received\": %" PRIu64 ",\n", d->received);
    fprintf(f, "  \"presented\": %" PRIu64 ",\n", d->presented);
    fprintf(f, "  \"failed\": %" PRIu64 ",\n", d->failed);
    fprintf(f, "  \"connector\": \"%s\",\n", d->kms.connector_name);
    fprintf(f, "  \"mode\": \"%ux%u\",\n",
            d->kms.mode.hdisplay, d->kms.mode.vdisplay);
    fprintf(f, "  \"sources\": [\n");
    for (int i = 0, written = 0; i < GVT_OUTPUTD_MAX_SOURCES; i++) {
        Source *s = &d->sources[i];
        if (!s->seen) {
            continue;
        }
        fprintf(f, "%s    {\"name\": \"%s\", \"frames\": %" PRIu64
                ", \"fb\": %u, \"width\": %u, \"height\": %u"
                ", \"cursor\": %s, \"cursor_x\": %u, \"cursor_y\": %u}",
                written ? ",\n" : "", s->name, s->frames, s->fb_id,
                s->width, s->height, s->cursor_seen ? "true" : "false",
                s->cursor_x, s->cursor_y);
        written++;
    }
    fprintf(f, "\n  ]\n");
    fprintf(f, "}\n");
    fclose(f);
    rename(tmp, d->status_path);
}

static void report(Daemon *d)
{
    time_t now = time(NULL);

    if (now - d->last_status >= 1) {
        d->last_status = now;
        write_status(d);
    }
    if (now - d->last_report < 5) {
        return;
    }
    d->last_report = now;
    fprintf(stderr, "gvt-outputd: stats received=%" PRIu64
            " presented=%" PRIu64 " failed=%" PRIu64 " active=%s\n",
            d->received, d->presented, d->failed,
            d->active ? d->active->name : "");
    for (int i = 0; i < GVT_OUTPUTD_MAX_SOURCES; i++) {
        Source *s = &d->sources[i];
        if (!s->seen) {
            continue;
        }
        fprintf(stderr, "gvt-outputd: source=%s frames=%" PRIu64
                " fb=%u size=%ux%u\n",
                s->name, s->frames, s->fb_id, s->width, s->height);
    }
}

static void recv_one(Daemon *d)
{
    GVTOutputdFrameMsg msg = { 0 };
    struct msghdr msgh = { 0 };
    struct iovec iov = {
        .iov_base = &msg,
        .iov_len = sizeof(msg),
    };
    char control[CMSG_SPACE(sizeof(int))];
    ssize_t n;
    int fd = -1;

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = control;
    msgh.msg_controllen = sizeof(control);
    n = recvmsg(d->sock, &msgh, 0);
    if (n < 0) {
        return;
    }
    if ((size_t)n < sizeof(msg) || msg.magic != GVT_OUTPUTD_MAGIC ||
        msg.version != GVT_OUTPUTD_VERSION) {
        d->failed++;
        return;
    }

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh); cmsg;
         cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
            break;
        }
    }

    msg.source[GVT_OUTPUTD_SOURCE_LEN - 1] = 0;
    if (msg.type == GVT_OUTPUTD_MSG_SELECT) {
        select_source(d, msg.source);
    } else if (msg.type == GVT_OUTPUTD_MSG_CURSOR_POS) {
        handle_cursor_pos(d, &msg);
        if (fd >= 0) {
            close(fd);
        }
    } else if (msg.type == GVT_OUTPUTD_MSG_FRAME && fd >= 0) {
        handle_frame(d, &msg, fd);
    } else {
        d->failed++;
        if (fd >= 0) {
            close(fd);
        }
    }
}

static int setup_socket(const char *path)
{
    struct sockaddr_un addr = { 0 };
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        return -1;
    }
    unlink(path);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0666);
    return fd;
}

int main(int argc, char **argv)
{
    Daemon d = {
        .sock = -1,
        .kms.fd = -1,
    };
    const char *socket_path = "/run/gvt-outputd.sock";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (!strcmp(argv[i], "--connector") && i + 1 < argc) {
            snprintf(d.requested_connector, sizeof(d.requested_connector),
                     "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--status") && i + 1 < argc) {
            snprintf(d.status_path, sizeof(d.status_path), "%s", argv[++i]);
        } else {
            fprintf(stderr, "usage: %s [--socket PATH] [--connector NAME] "
                    "[--status PATH]\n",
                    argv[0]);
            return 2;
        }
    }

    snprintf(d.socket_path, sizeof(d.socket_path), "%s", socket_path);
    d.sock = setup_socket(socket_path);
    if (d.sock < 0) {
        fprintf(stderr, "gvt-outputd: bind %s failed: %s\n",
                socket_path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "gvt-outputd: listening socket=%s connector=%s\n",
            socket_path, d.requested_connector[0] ? d.requested_connector :
            "auto");

    for (;;) {
        struct pollfd pfd = {
            .fd = d.sock,
            .events = POLLIN,
        };
        int ret = poll(&pfd, 1, 1000);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            recv_one(&d);
        }
        report(&d);
    }
}
