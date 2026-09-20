/*
 * appsandbox-display.c — Linux frame capture for AppSandbox.
 *
 * Listens on AF_VSOCK port 2, captures the primary KMS framebuffer via
 * libdrm (drmModeGetFB2 + drmPrimeHandleToFD + mmap), and ships frames
 * in the ASFR wire format the Windows host already understands.
 *
 * No PipeWire, no X11 capture — one layer deeper, straight off KMS.
 *
 * Requires CAP_SYS_ADMIN to call drmModeGetFB2 (since kernel 4.15).
 * The systemd unit runs the binary as root, which satisfies that.
 *
 * Build:  gcc -O2 -Wall -o appsandbox-display appsandbox-display.c -ldrm
 *
 * Wire protocol reference: src/backend_win/vm_display_idd.c (FRAME_MAGIC
 * 'ASFR', packed FrameHeader { magic, w, h, stride, frame_seq, rect_cnt },
 * then rect_cnt RECT(int32 ltrb), then u32 data_size, then BGRA bytes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/vm_sockets.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "../../../src/core/display_protocol.h"

#define VSOCK_PORT      2
#define FRAME_MAGIC     ASB_DISPLAY_RAW_MAGIC
#define CURSOR_MAGIC    ASB_DISPLAY_CURSOR_MAGIC
#define TARGET_FPS      60
#define FRAME_INTERVAL_NS (1000000000L / TARGET_FPS)
#define MAX_DIRTY_RECTS ASB_DISPLAY_MAX_DIRTY_RECTS
#define TILE_SIZE       ASB_DISPLAY_TILE_SIZE

#define CURSOR_TYPE_MASKED_COLOR  1
#define CURSOR_TYPE_ALPHA         2

#pragma pack(push, 1)
typedef AsbDisplayFrameHeader frame_header;

struct cursor_header {
    uint32_t magic;            /* CURSOR_MAGIC */
    int32_t  x, y;             /* CRTC coordinates (can be negative) */
    uint32_t visible;          /* 1 if a fb is attached, else 0 */
    uint32_t shape_updated;    /* 1 if bitmap follows */
    uint32_t shape_id;         /* monotonic — host caches by id */
    uint32_t width, height, pitch;
    uint32_t xhot, yhot;
    uint32_t cursor_type;      /* CURSOR_TYPE_* */
    uint32_t shape_data_size;  /* bytes of bitmap after header */
};
#pragma pack(pop)

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void agent_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[display] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---- vsock listener ---- */

static int vsock_listen(unsigned port)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) { agent_log("socket: %s", strerror(errno)); return -1; }

    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = port;

    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("bind :%u: %s", port, strerror(errno));
        close(s); return -1;
    }
    if (listen(s, 1) < 0) {
        agent_log("listen: %s", strerror(errno));
        close(s); return -1;
    }
    return s;
}

static ssize_t send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n; left -= (size_t)n;
    }
    return (ssize_t)len;
}

/* ---- DRM capture ---- */

struct capture_ctx {
    int fd;
    uint32_t fb_id_last;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t *mem;        /* mmapped framebuffer (read-only) */
    size_t   mem_size;
    int      dma_fd;

    /* The current DRM userspace interface exposes the active framebuffer but
     * not reliable compositor damage clips. Keep a compact shadow and use a
     * 64x64 tile fallback until a trustworthy damage source is available. */
    uint8_t  *shadow;
    size_t    shadow_size;
    uint32_t  shadow_stride;
    uint32_t  shadow_width;
    uint32_t  shadow_height;
    uint8_t  *dirty_tiles;
    size_t    dirty_tiles_cap;
    AsbDisplayRect *work_rects;
    size_t    work_rects_cap;
    int       force_full;

    struct {
        uint64_t frames_scanned;
        uint64_t frames_sent;
        uint64_t full_frames_sent;
        uint64_t dirty_frames_sent;
        uint64_t unchanged_frames_skipped;
        uint64_t dirty_rects_total;
        uint64_t dirty_area_sum;
        uint64_t wire_bytes_total;
        uint64_t ratio_buckets[101];
        uint64_t start_ns;
        uint64_t last_log_ns;
    } stats;
};

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int checked_frame_size(uint32_t stride, uint32_t height, size_t *out)
{
    uint64_t size;

    if (!stride || !height) return -1;
    size = (uint64_t)stride * height;
    if (size > ASB_DISPLAY_MAX_FRAME_DATA_SIZE || size > SIZE_MAX)
        return -1;
    *out = (size_t)size;
    return 0;
}

static int ensure_tile_buffers(struct capture_ctx *c)
{
    uint64_t cols = ((uint64_t)c->width + TILE_SIZE - 1) / TILE_SIZE;
    uint64_t rows = ((uint64_t)c->height + TILE_SIZE - 1) / TILE_SIZE;
    uint64_t tiles = cols * rows;
    uint8_t *tile_buf;
    AsbDisplayRect *rect_buf;

    if (!cols || !rows || tiles > SIZE_MAX ||
        tiles > SIZE_MAX / sizeof(*c->work_rects))
        return -1;

    if ((size_t)tiles > c->dirty_tiles_cap) {
        tile_buf = (uint8_t *)realloc(c->dirty_tiles, (size_t)tiles);
        if (!tile_buf) return -1;
        c->dirty_tiles = tile_buf;
        c->dirty_tiles_cap = (size_t)tiles;
    }
    if ((size_t)tiles > c->work_rects_cap) {
        rect_buf = (AsbDisplayRect *)realloc(c->work_rects,
                                             (size_t)tiles * sizeof(*rect_buf));
        if (!rect_buf) return -1;
        c->work_rects = rect_buf;
        c->work_rects_cap = (size_t)tiles;
    }
    return 0;
}

static void free_capture_buffers(struct capture_ctx *c)
{
    free(c->shadow);
    free(c->dirty_tiles);
    free(c->work_rects);
    c->shadow = NULL;
    c->dirty_tiles = NULL;
    c->work_rects = NULL;
    c->shadow_size = c->dirty_tiles_cap = c->work_rects_cap = 0;
}

static int prepare_shadow(struct capture_ctx *c, size_t size)
{
    if (c->shadow_width == c->width && c->shadow_height == c->height &&
        c->shadow_stride == c->stride && c->shadow_size == size && c->shadow)
        return 0;

    free(c->shadow);
    c->shadow = (uint8_t *)malloc(size);
    if (!c->shadow) {
        c->shadow_size = 0;
        c->force_full = 1;
        return -1;
    }
    c->shadow_size = size;
    c->shadow_stride = c->stride;
    c->shadow_width = c->width;
    c->shadow_height = c->height;
    memset(c->shadow, 0, size);
    c->force_full = 1;
    return 0;
}

static void drm_release_fb(struct capture_ctx *c)
{
    if (c->mem && c->mem != MAP_FAILED) {
        munmap(c->mem, c->mem_size);
        c->mem = NULL; c->mem_size = 0;
    }
    if (c->dma_fd >= 0) { close(c->dma_fd); c->dma_fd = -1; }
    c->fb_id_last = 0;
    c->width = c->height = c->stride = 0;
}

/* Find the active primary-plane framebuffer and map it for read.
 * Re-maps only when the fb_id changes (compositor flipped to a new buffer). */
static int drm_acquire_fb(struct capture_ctx *c)
{
    int rc = -1;
    drmModePlaneRes *pres = drmModeGetPlaneResources(c->fd);
    if (!pres) return -1;

    for (uint32_t i = 0; i < pres->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(c->fd, pres->planes[i]);
        if (!p) continue;
        if (p->fb_id == 0 || p->crtc_id == 0) {
            drmModeFreePlane(p);
            continue;
        }

        /* Skip non-PRIMARY planes — cursor / overlay planes also carry
         * a fb but they're small / partial. We want the desktop. */
        {
            drmModeObjectProperties *props = drmModeObjectGetProperties(
                c->fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            int is_primary = 0;
            if (props) {
                for (uint32_t k = 0; k < props->count_props; k++) {
                    drmModePropertyRes *pr = drmModeGetProperty(c->fd, props->props[k]);
                    if (!pr) continue;
                    if (strcmp(pr->name, "type") == 0 &&
                        props->prop_values[k] == DRM_PLANE_TYPE_PRIMARY)
                        is_primary = 1;
                    drmModeFreeProperty(pr);
                    if (is_primary) break;
                }
                drmModeFreeObjectProperties(props);
            }
            if (!is_primary) {
                drmModeFreePlane(p);
                continue;
            }
        }

        /* Same fb as last frame? Reuse the existing mapping. */
        if (p->fb_id == c->fb_id_last && c->mem) {
            drmModeFreePlane(p);
            rc = 0;
            goto out;
        }

        uint32_t fb_id = p->fb_id;
        drmModeFB2 *fb2 = drmModeGetFB2(c->fd, fb_id);
        drmModeFreePlane(p);
        if (!fb2) continue;

        /* Only XRGB8888 / ARGB8888 for now — that's what hyperv_drm uses. */
        if (fb2->pixel_format != DRM_FORMAT_XRGB8888 &&
            fb2->pixel_format != DRM_FORMAT_ARGB8888) {
            drmModeFreeFB2(fb2);
            continue;
        }

        uint32_t handle = fb2->handles[0];
        uint32_t pitch  = fb2->pitches[0];
        uint32_t height = fb2->height;
        uint32_t width  = fb2->width;
        drmModeFreeFB2(fb2);
        if (!handle || !pitch || !height || !width ||
            width > ASB_DISPLAY_RAW_MAX_WIDTH ||
            height > ASB_DISPLAY_RAW_MAX_HEIGHT ||
            (uint64_t)width * 4 > pitch)
            continue;

        size_t size;
        if (checked_frame_size(pitch, height, &size) < 0)
            continue;

        int dma_fd = -1;
        if (drmPrimeHandleToFD(c->fd, handle, DRM_CLOEXEC | O_RDONLY, &dma_fd) < 0) {
            continue;
        }
        void *m = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_fd, 0);
        if (m == MAP_FAILED) {
            close(dma_fd);
            continue;
        }

        drm_release_fb(c);
        c->fb_id_last = fb_id;
        c->width  = width;
        c->height = height;
        c->stride = pitch;
        c->mem      = (uint8_t *)m;
        c->mem_size = size;
        c->dma_fd   = dma_fd;
        (void)prepare_shadow(c, size);
        (void)ensure_tile_buffers(c);
        rc = 0;
        goto out;
    }

out:
    drmModeFreePlaneResources(pres);
    return rc;
}

/* ---- Cursor plane: discover + emit ASCR on change ----
 *
 * Mutter updates the cursor plane independently of the primary plane,
 * so the host sees smooth pointer movement even at our TARGET_FPS primary
 * cadence. We cache the plane id + the CRTC_X / CRTC_Y / HOTSPOT_X /
 * HOTSPOT_Y property ids once at startup, then on every tick read the
 * current values via drmModeObjectGetProperties (one ioctl, ~µs cost). */

struct cursor_state {
    int       discovered;
    uint32_t  plane_id;
    uint32_t  prop_crtc_x, prop_crtc_y;
    uint32_t  prop_hotspot_x, prop_hotspot_y;

    /* Last sent values — only re-send when something changes. */
    uint32_t  last_fb_id;
    int32_t   last_x, last_y;
    uint32_t  last_hotspot_x, last_hotspot_y;
    uint32_t  shape_id;
};

static void cursor_init(int fd, struct cursor_state *cur)
{
    drmModePlaneRes *pres;
    memset(cur, 0, sizeof(*cur));

    pres = drmModeGetPlaneResources(fd);
    if (!pres) return;

    for (uint32_t i = 0; i < pres->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pres->planes[i]);
        if (!p) continue;
        drmModeObjectProperties *props = drmModeObjectGetProperties(
            fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
        int is_cursor = 0;
        uint32_t cx = 0, cy = 0, hx = 0, hy = 0;
        if (props) {
            for (uint32_t k = 0; k < props->count_props; k++) {
                drmModePropertyRes *pr = drmModeGetProperty(fd, props->props[k]);
                if (!pr) continue;
                if (strcmp(pr->name, "type") == 0 &&
                    props->prop_values[k] == DRM_PLANE_TYPE_CURSOR)
                    is_cursor = 1;
                else if (strcmp(pr->name, "CRTC_X") == 0)    cx = pr->prop_id;
                else if (strcmp(pr->name, "CRTC_Y") == 0)    cy = pr->prop_id;
                else if (strcmp(pr->name, "HOTSPOT_X") == 0) hx = pr->prop_id;
                else if (strcmp(pr->name, "HOTSPOT_Y") == 0) hy = pr->prop_id;
                drmModeFreeProperty(pr);
            }
            drmModeFreeObjectProperties(props);
        }
        if (is_cursor) {
            cur->discovered     = 1;
            cur->plane_id       = p->plane_id;
            cur->prop_crtc_x    = cx;
            cur->prop_crtc_y    = cy;
            cur->prop_hotspot_x = hx;
            cur->prop_hotspot_y = hy;
            drmModeFreePlane(p);
            /* Log the cached cur->plane_id, not p->plane_id: p was just freed
               above, so reading p->plane_id here was a use-after-free (L12). */
            agent_log("cursor plane %u (CRTC_X=%u CRTC_Y=%u HOTSPOT_X=%u HOTSPOT_Y=%u)",
                      cur->plane_id, cx, cy, hx, hy);
            break;
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pres);
    if (!cur->discovered)
        agent_log("no cursor plane found — host will see no smooth cursor");
}

/* Read a property value from the cached object-properties blob. */
static uint64_t prop_get(drmModeObjectProperties *props, uint32_t prop_id)
{
    if (!prop_id || !props) return 0;
    for (uint32_t k = 0; k < props->count_props; k++)
        if (props->props[k] == prop_id)
            return props->prop_values[k];
    return 0;
}

/* Sign-extend CRTC_X / CRTC_Y: kernel stores them as u64 but they're
 * semantically int32_t (cursor can be off-screen). */
static int32_t sext32(uint64_t v) { return (int32_t)(uint32_t)v; }

static int cursor_tick(int client_fd, int drm_fd, struct cursor_state *cur)
{
    if (!cur->discovered) return 0;

    drmModePlane *p = drmModeGetPlane(drm_fd, cur->plane_id);
    if (!p) return 0;
    uint32_t fb_id = p->fb_id;
    drmModeFreePlane(p);

    drmModeObjectProperties *props = drmModeObjectGetProperties(
        drm_fd, cur->plane_id, DRM_MODE_OBJECT_PLANE);
    int32_t  cx  = sext32(prop_get(props, cur->prop_crtc_x));
    int32_t  cy  = sext32(prop_get(props, cur->prop_crtc_y));
    uint32_t hx  = (uint32_t)prop_get(props, cur->prop_hotspot_x);
    uint32_t hy  = (uint32_t)prop_get(props, cur->prop_hotspot_y);
    if (props) drmModeFreeObjectProperties(props);

    int pos_changed   = (cx != cur->last_x) || (cy != cur->last_y) ||
                        (hx != cur->last_hotspot_x) || (hy != cur->last_hotspot_y);
    int shape_changed = (fb_id != cur->last_fb_id);

    if (!pos_changed && !shape_changed) return 0;

    /* If shape changed, read the cursor fb shape bytes via PRIME. */
    drmModeFB2 *fb2 = NULL;
    int     dma_fd = -1;
    void   *bmp    = NULL;
    size_t  bmp_sz = 0;
    uint32_t fb_w = 0, fb_h = 0, fb_pitch = 0;

    if (shape_changed && fb_id != 0) {
        fb2 = drmModeGetFB2(drm_fd, fb_id);
        if (fb2 && fb2->handles[0]) {
            if (drmPrimeHandleToFD(drm_fd, fb2->handles[0],
                                   DRM_CLOEXEC | O_RDONLY, &dma_fd) == 0) {
                fb_w     = fb2->width;
                fb_h     = fb2->height;
                fb_pitch = fb2->pitches[0];
                bmp_sz   = (size_t)fb_pitch * fb_h;
                bmp = mmap(NULL, bmp_sz, PROT_READ, MAP_SHARED, dma_fd, 0);
                if (bmp == MAP_FAILED) { bmp = NULL; bmp_sz = 0; }
            }
        }
    }

    /* Crop the cursor bitmap to its non-transparent bounding box, then
     * find the artist's hotspot heuristically as the topmost-leftmost
     * pixel with "really opaque" alpha (>= HOT_ALPHA_THRESHOLD). The
     * full bbox includes anti-aliased edge pixels that fade in with
     * alpha 1..127 before the cursor body starts; those aren't the
     * cursor tip, just edge feathering. The first pixel above the
     * threshold approximates where the artist would have placed the
     * hotspot for arrow / hand / crosshair cursors (it'd be a few
     * pixels off for I-beam / centered cursors, but those are uncommon
     * over the IDD window).
     *
     * Mutter doesn't set HOTSPOT_X/HOTSPOT_Y on us (asb_drm isn't in
     * Mutter's hotspot-driver allowlist), so we have to recover the
     * hotspot from the bitmap. Mutter has already pre-adjusted CRTC_X
     * by the hotspot, but the host ignores ASCR x/y anyway — it uses
     * the OS pointer position and renders the HCURSOR with the hotspot
     * we provide.
     *
     * Alpha stays premultiplied: the host's create_cursor_from_data
     * (vm_display_idd.c, CURSOR_TYPE_ALPHA branch) explicitly expects
     * 32bpp BGRA with premultiplied alpha. */
    #define HOT_ALPHA_THRESHOLD  128
    uint8_t  *cropped     = NULL;
    uint32_t  out_w       = 0, out_h = 0, out_pitch = 0;
    size_t    out_sz      = 0;
    uint32_t  hot_x_in_crop = 0, hot_y_in_crop = 0;

    if (shape_changed && bmp && bmp_sz > 0 && fb_pitch > 0) {
        const uint8_t *src = (const uint8_t *)bmp;
        uint32_t min_x = fb_w, min_y = fb_h, max_x = 0, max_y = 0;
        uint32_t hot_x = 0, hot_y = 0;
        int any = 0, hot_found = 0;
        for (uint32_t y = 0; y < fb_h; y++) {
            const uint8_t *row = src + (size_t)y * fb_pitch;
            for (uint32_t x = 0; x < fb_w; x++) {
                uint8_t a = row[x * 4 + 3];
                if (a != 0) {
                    if (!any) { min_x = max_x = x; min_y = max_y = y; any = 1; }
                    else {
                        if (x < min_x) min_x = x;
                        if (x > max_x) max_x = x;
                        if (y < min_y) min_y = y;
                        if (y > max_y) max_y = y;
                    }
                    if (!hot_found && a >= HOT_ALPHA_THRESHOLD) {
                        hot_x = x; hot_y = y; hot_found = 1;
                    }
                }
            }
        }
        if (any) {
            out_w     = max_x - min_x + 1;
            out_h     = max_y - min_y + 1;
            out_pitch = out_w * 4;
            out_sz    = (size_t)out_pitch * out_h;
            cropped   = (uint8_t *)malloc(out_sz);
            if (cropped) {
                /* Pass Mutter's premultiplied bytes through unchanged. */
                for (uint32_t y = 0; y < out_h; y++) {
                    memcpy(cropped + (size_t)y * out_pitch,
                           src + ((size_t)(y + min_y) * fb_pitch) + (size_t)min_x * 4,
                           out_pitch);
                }
                if (hot_found) {
                    hot_x_in_crop = hot_x - min_x;
                    hot_y_in_crop = hot_y - min_y;
                }
            } else {
                out_sz = 0;
            }
        }
    }

    struct cursor_header h;
    memset(&h, 0, sizeof(h));
    h.magic         = CURSOR_MAGIC;
    h.x             = cx;
    h.y             = cy;
    h.visible       = (fb_id != 0) ? 1 : 0;
    h.shape_updated = (cropped && out_sz > 0) ? 1 : 0;
    h.shape_id      = cur->shape_id + (h.shape_updated ? 1 : 0);
    h.width         = out_w;
    h.height        = out_h;
    h.pitch         = out_pitch;
    h.xhot          = hot_x_in_crop;
    h.yhot          = hot_y_in_crop;
    h.cursor_type   = CURSOR_TYPE_ALPHA;
    h.shape_data_size = h.shape_updated ? (uint32_t)out_sz : 0;

    int rc = 0;
    if (send_all(client_fd, &h, sizeof(h)) < 0) rc = -1;
    else if (h.shape_updated &&
             send_all(client_fd, cropped, out_sz) < 0) rc = -1;

    if (cropped) free(cropped);

    if (bmp)    munmap(bmp, bmp_sz);
    if (dma_fd >= 0) close(dma_fd);
    if (fb2)    drmModeFreeFB2(fb2);

    if (rc == 0) {
        cur->last_fb_id      = fb_id;
        cur->last_x          = cx;
        cur->last_y          = cy;
        cur->last_hotspot_x  = hx;
        cur->last_hotspot_y  = hy;
        if (h.shape_updated) cur->shape_id = h.shape_id;
    }
    return rc;
}

/* ---- Main capture loop ---- */

static int scan_dirty_tiles(struct capture_ctx *c, AsbDisplayRect *rects,
                            uint32_t *rect_count, int *full,
                            uint64_t *dirty_area)
{
    uint64_t cols = ((uint64_t)c->width + TILE_SIZE - 1) / TILE_SIZE;
    uint64_t rows = ((uint64_t)c->height + TILE_SIZE - 1) / TILE_SIZE;
    uint64_t total_area = (uint64_t)c->width * c->height;
    size_t work_count = 0;
    uint32_t ty;

    *rect_count = 0;
    *dirty_area = 0;
    *full = c->force_full || !c->shadow;
    if (*full) {
        *dirty_area = total_area;
        return 0;
    }
    if (ensure_tile_buffers(c) < 0) {
        *full = 1;
        *dirty_area = total_area;
        return 0;
    }

    memset(c->dirty_tiles, 0, (size_t)(cols * rows));
    for (ty = 0; ty < rows; ty++) {
        uint32_t tx;
        uint32_t top = ty * TILE_SIZE;
        uint32_t bottom = top + TILE_SIZE < c->height
            ? top + TILE_SIZE : c->height;
        for (tx = 0; tx < cols; tx++) {
            uint32_t left = tx * TILE_SIZE;
            uint32_t right = left + TILE_SIZE < c->width
                ? left + TILE_SIZE : c->width;
            uint32_t y;
            int changed = 0;

            for (y = top; y < bottom && !changed; y++) {
                const uint8_t *now = c->mem + (size_t)y * c->stride + left * 4;
                const uint8_t *old = c->shadow + (size_t)y * c->shadow_stride + left * 4;
                if (memcmp(now, old, (size_t)(right - left) * 4) != 0)
                    changed = 1;
            }
            if (changed) {
                c->dirty_tiles[(size_t)ty * cols + tx] = 1;
                *dirty_area += (uint64_t)(right - left) * (bottom - top);
            }
        }
    }

    if (!*dirty_area) return 0;

    /* Once the changed area is close to the full desktop, one contiguous
     * upload is cheaper and avoids producing a pathological rect list. */
    if (*dirty_area * 100 >= total_area * 60) {
        *full = 1;
        *dirty_area = total_area;
        return 0;
    }

    /* Build horizontal tile runs and vertically coalesce equal runs. */
    for (ty = 0; ty < rows; ty++) {
        uint32_t tx = 0;
        uint32_t top = ty * TILE_SIZE;
        uint32_t bottom = top + TILE_SIZE < c->height
            ? top + TILE_SIZE : c->height;
        while (tx < cols) {
            uint32_t start;
            uint32_t end;
            AsbDisplayRect run;

            while (tx < cols && !c->dirty_tiles[(size_t)ty * cols + tx]) tx++;
            if (tx == cols) break;
            start = tx++;
            while (tx < cols && c->dirty_tiles[(size_t)ty * cols + tx]) tx++;
            end = tx;
            run.left = (int32_t)(start * TILE_SIZE);
            run.top = (int32_t)top;
            run.right = (int32_t)(end * TILE_SIZE < c->width
                ? end * TILE_SIZE : c->width);
            run.bottom = (int32_t)bottom;

            if (work_count > 0 &&
                c->work_rects[work_count - 1].left == run.left &&
                c->work_rects[work_count - 1].right == run.right &&
                c->work_rects[work_count - 1].bottom == run.top) {
                c->work_rects[work_count - 1].bottom = run.bottom;
            } else {
                if (work_count >= c->work_rects_cap) {
                    *full = 1;
                    *dirty_area = total_area;
                    return 0;
                }
                c->work_rects[work_count++] = run;
            }
        }
    }

    if (work_count > MAX_DIRTY_RECTS) {
        *full = 1;
        *dirty_area = total_area;
        return 0;
    }
    memcpy(rects, c->work_rects, work_count * sizeof(*rects));
    *rect_count = (uint32_t)work_count;
    return 0;
}

static void shadow_apply(struct capture_ctx *c, const AsbDisplayRect *rects,
                         uint32_t rect_count, int full)
{
    uint32_t i;
    if (!c->shadow) return;
    if (full) {
        uint32_t y;
        size_t row_bytes = (size_t)c->width * 4;
        for (y = 0; y < c->height; y++)
            memcpy(c->shadow + (size_t)y * c->shadow_stride,
                   c->mem + (size_t)y * c->stride, row_bytes);
    } else {
        for (i = 0; i < rect_count; i++) {
            uint32_t y;
            uint32_t width = (uint32_t)(rects[i].right - rects[i].left);
            uint32_t height = (uint32_t)(rects[i].bottom - rects[i].top);
            size_t row_bytes = (size_t)width * 4;
            for (y = 0; y < height; y++)
                memcpy(c->shadow + (size_t)(rects[i].top + y) * c->shadow_stride
                                      + (size_t)rects[i].left * 4,
                       c->mem + (size_t)(rects[i].top + y) * c->stride
                                + (size_t)rects[i].left * 4,
                       row_bytes);
        }
    }
    c->force_full = 0;
}

static int send_frame(int client_fd, struct capture_ctx *c, uint64_t seq,
                      const AsbDisplayRect *rects, uint32_t rect_count,
                      int full, uint64_t *wire_bytes)
{
    frame_header h;
    uint64_t data_size64 = full ? c->mem_size : 0;
    uint32_t i;
    uint32_t data_size;
    uint64_t sent = sizeof(h) + sizeof(data_size) +
                    (uint64_t)rect_count * sizeof(*rects);

    if (!full) {
        for (i = 0; i < rect_count; i++) {
            uint64_t width = (uint64_t)(rects[i].right - rects[i].left);
            uint64_t height = (uint64_t)(rects[i].bottom - rects[i].top);
            data_size64 += width * height * 4;
        }
    }
    if (data_size64 > UINT32_MAX || data_size64 > ASB_DISPLAY_MAX_FRAME_DATA_SIZE)
        return -1;

    h.magic = FRAME_MAGIC;
    h.width = c->width;
    h.height = c->height;
    h.stride = c->stride;
    h.frame_seq = seq;
    h.dirty_rect_count = full ? 0 : rect_count;
    data_size = (uint32_t)data_size64;

    if (send_all(client_fd, &h, sizeof(h)) < 0 ||
        (rect_count && send_all(client_fd, rects,
                                (size_t)rect_count * sizeof(*rects)) < 0) ||
        send_all(client_fd, &data_size, sizeof(data_size)) < 0)
        return -1;

    if (full) {
        if (send_all(client_fd, c->mem, c->mem_size) < 0) return -1;
    } else {
        for (i = 0; i < rect_count; i++) {
            uint32_t y;
            uint32_t width = (uint32_t)(rects[i].right - rects[i].left);
            uint32_t height = (uint32_t)(rects[i].bottom - rects[i].top);
            size_t row_bytes = (size_t)width * 4;
            for (y = 0; y < height; y++) {
                const uint8_t *row = c->mem +
                    (size_t)(rects[i].top + y) * c->stride +
                    (size_t)rects[i].left * 4;
                if (send_all(client_fd, row, row_bytes) < 0) return -1;
            }
        }
    }
    *wire_bytes = sent + data_size64;
    return 0;
}

static void maybe_log_stats(struct capture_ctx *c)
{
    uint64_t now = monotonic_ns();
    uint64_t total = c->stats.frames_scanned;
    uint64_t target, seen = 0, p95_bucket = 100;
    double seconds, avg, mib_per_sec;
    int i;

    if (!c->stats.start_ns) c->stats.start_ns = now;
    if (c->stats.last_log_ns && now - c->stats.last_log_ns < 5000000000ULL)
        return;
    c->stats.last_log_ns = now;
    if (!total) return;
    target = (total * 95 + 99) / 100;
    for (i = 0; i <= 100; i++) {
        seen += c->stats.ratio_buckets[i];
        if (seen >= target) { p95_bucket = (uint64_t)i; break; }
    }
    seconds = (double)(now - c->stats.start_ns) / 1000000000.0;
    avg = (double)c->stats.dirty_area_sum /
          ((double)total * c->width * c->height);
    mib_per_sec = seconds > 0.0
        ? (double)c->stats.wire_bytes_total / seconds / (1024.0 * 1024.0)
        : 0.0;
    agent_log("display_stats scope=guest resolution=%ux%u logical_refresh_hz=%u "
              "frames_scanned=%llu frames_sent=%llu full_frames_sent=%llu "
              "dirty_frames_sent=%llu unchanged_frames_skipped=%llu "
              "dirty_rects_total=%llu dirty_area_ratio_avg=%.5f "
              "dirty_area_ratio_p95=%.2f wire_bytes_total=%llu wire_mib_per_sec=%.3f",
              c->width, c->height, TARGET_FPS,
              (unsigned long long)c->stats.frames_scanned,
              (unsigned long long)c->stats.frames_sent,
              (unsigned long long)c->stats.full_frames_sent,
              (unsigned long long)c->stats.dirty_frames_sent,
              (unsigned long long)c->stats.unchanged_frames_skipped,
              (unsigned long long)c->stats.dirty_rects_total, avg,
              (double)p95_bucket / 100.0,
              (unsigned long long)c->stats.wire_bytes_total, mib_per_sec);
}

static void capture_loop(int client_fd)
{
    struct capture_ctx ctx = { .fd = -1, .dma_fd = -1 };

    /* Find a /dev/dri/cardN by DRM driver name, in preference order:
     *   asb_drm     — our custom virtual display driver (preferred)
     *   hyperv_drm  — Hyper-V synthetic GPU (fallback for VMs without asb_drm)
     *
     * Matching on driver name inherently skips the boot framebuffers
     * (simpledrm/efifb/vesafb) that Mutter isn't rendering to. The legacy
     * fallback below (used only if neither driver is found) is the path
     * that filters on a connected connector instead. */
    static const char *DRIVER_PRIORITY[] = {
        "asb_drm",
        "hyperv_drm",
        NULL,
    };

    int ctx_fd = -1;
    char chosen_path[64] = {0};
    for (int prio = 0; DRIVER_PRIORITY[prio] && ctx_fd < 0; prio++) {
        for (int i = 0; i < 8 && ctx_fd < 0; i++) {
            char path[64], uevent_path[96], uevent[256];
            snprintf(path, sizeof(path), "/dev/dri/card%d", i);
            snprintf(uevent_path, sizeof(uevent_path),
                     "/sys/class/drm/card%d/device/uevent", i);
            FILE *uf = fopen(uevent_path, "r");
            if (!uf) continue;
            int match = 0;
            while (fgets(uevent, sizeof(uevent), uf)) {
                if (strncmp(uevent, "DRIVER=", 7) != 0) continue;
                char *nl = strchr(uevent + 7, '\n');
                if (nl) *nl = '\0';
                if (strcmp(uevent + 7, DRIVER_PRIORITY[prio]) == 0)
                    match = 1;
                break;
            }
            fclose(uf);
            if (!match) continue;
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            ctx_fd = fd;
            snprintf(chosen_path, sizeof(chosen_path), "%s", path);
            agent_log("selected %s (%s)", path, DRIVER_PRIORITY[prio]);
        }
    }
    if (ctx_fd >= 0) {
        ctx.fd = ctx_fd;
        agent_log("using %s", chosen_path);
        /* Skip the legacy fallback enumeration below. */
        goto have_card;
    }

    /* Legacy fallback: scan unfiltered, skip boot framebuffers. */
    for (int i = 0; i < 8; i++) {
        char path[64], uevent_path[96], uevent[256];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        snprintf(uevent_path, sizeof(uevent_path),
                 "/sys/class/drm/card%d/device/uevent", i);
        FILE *uf = fopen(uevent_path, "r");
        if (uf) {
            int boot_fb = 0;
            while (fgets(uevent, sizeof(uevent), uf)) {
                if (strncmp(uevent, "DRIVER=", 7) != 0) continue;
                const char *drv = uevent + 7;
                if (strstr(drv, "simple-framebuffer") ||
                    strstr(drv, "simpledrm") ||
                    strstr(drv, "efifb") ||
                    strstr(drv, "vesafb")) {
                    boot_fb = 1;
                }
                break;
            }
            fclose(uf);
            if (boot_fb) {
                agent_log("skipping %s (boot framebuffer)", path);
                continue;
            }
        }
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        /* The first userspace opener of /dev/dri/cardN becomes the
         * implicit DRM master. If we hold master, Mutter/gnome-shell
         * later gets EBUSY when it tries to claim master and the
         * compositor never starts. Drop master immediately; we only
         * need read access to the framebuffer (via drmModeGetFB2 +
         * drmPrimeHandleToFD, both of which want CAP_SYS_ADMIN, not
         * master). EACCES here just means we weren't master to start
         * with — fine, nothing to drop. */
        drmDropMaster(fd);
        drmModeRes *res = drmModeGetResources(fd);
        if (res) {
            int has_conn = 0;
            for (int k = 0; k < res->count_connectors; k++) {
                drmModeConnector *con = drmModeGetConnector(fd, res->connectors[k]);
                if (con) {
                    if (con->connection == DRM_MODE_CONNECTED) has_conn = 1;
                    drmModeFreeConnector(con);
                    if (has_conn) break;
                }
            }
            drmModeFreeResources(res);
            if (has_conn) {
                ctx.fd = fd;
                agent_log("using %s", path);
                break;
            }
        }
        close(fd);
    }
    if (ctx.fd < 0) {
        agent_log("no /dev/dri/cardN with a connected output");
        return;
    }

have_card:
    /* Drop implicit DRM master we may have picked up by being first to
     * open the device — Mutter needs to become master to render. */
    drmDropMaster(ctx.fd);

    /* Universal planes lets us enumerate the primary; atomic exposes the
     * standard property set (CRTC_X / CRTC_Y / FB_ID / type / ...) so we
     * can read cursor position via drmModeObjectGetProperties. Without
     * the atomic cap those property ids come back as 0 and cursor sync
     * silently degrades to "position never changes". */
    drmSetClientCap(ctx.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(ctx.fd, DRM_CLIENT_CAP_ATOMIC, 1);
    /* Drivers that set DRIVER_CURSOR_HOTSPOT (we do, in asb_drm) hide the
     * cursor plane from clients that don't ack hotspot semantics — they'd
     * otherwise paint cursors at the wrong place. Opt in so the daemon
     * actually sees the cursor plane in drmModeGetPlaneResources(). */
#ifdef DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT
    drmSetClientCap(ctx.fd, DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT, 1);
#else
    /* libdrm < 2.4.118 doesn't define the constant. Value is 6 per the
     * uapi in <drm/drm.h>; safe to set numerically. */
    drmSetClientCap(ctx.fd, 6, 1);
#endif

    /* Cache cursor plane id + property ids once. */
    struct cursor_state cur;
    cursor_init(ctx.fd, &cur);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t seq = 0;
    int last_status = 0;

    while (!g_stop) {
        int rc = drm_acquire_fb(&ctx);
        if (rc < 0) {
            if (last_status != -1) {
                agent_log("no active framebuffer; waiting");
                last_status = -1;
            }
        } else {
            AsbDisplayRect rects[MAX_DIRTY_RECTS];
            uint32_t rect_count = 0;
            uint64_t dirty_area = 0;
            uint64_t wire_bytes = 0;
            uint64_t total_area = (uint64_t)ctx.width * ctx.height;
            int full = 0;

            if (last_status != 1) {
                agent_log("capturing %ux%u stride=%u", ctx.width, ctx.height, ctx.stride);
                last_status = 1;
            }
            ctx.stats.frames_scanned++;
            (void)scan_dirty_tiles(&ctx, rects, &rect_count, &full, &dirty_area);
            if (!full && rect_count == 0) {
                ctx.stats.unchanged_frames_skipped++;
            } else {
                if (send_frame(client_fd, &ctx, ++seq, rects, rect_count,
                               full, &wire_bytes) < 0) {
                    agent_log("client disconnected");
                    break;
                }
                shadow_apply(&ctx, rects, rect_count, full);
                ctx.stats.frames_sent++;
                ctx.stats.wire_bytes_total += wire_bytes;
                ctx.stats.dirty_rects_total += rect_count;
                if (full) ctx.stats.full_frames_sent++;
                else      ctx.stats.dirty_frames_sent++;
            }
            if (dirty_area > total_area) dirty_area = total_area;
            ctx.stats.dirty_area_sum += dirty_area;
            ctx.stats.ratio_buckets[total_area
                ? (dirty_area * 100 / total_area) : 0]++;
            maybe_log_stats(&ctx);
        }

        /* Emit cursor update if its position/shape changed since last tick.
         * Cheap when nothing changed (one drmModeGetPlane + one props read). */
        if (cursor_tick(client_fd, ctx.fd, &cur) < 0) {
            agent_log("client disconnected (cursor)");
            break;
        }

        /* Pace to TARGET_FPS using absolute wakeup. */
        next.tv_nsec += FRAME_INTERVAL_NS;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec  += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    drm_release_fb(&ctx);
    free_capture_buffers(&ctx);
    close(ctx.fd);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    /* Install SIGINT/SIGTERM WITHOUT SA_RESTART so a blocking accept()
     * returns EINTR on signal (the loop below checks for it) instead of
     * auto-restarting. glibc's signal() defaults to BSD SA_RESTART
     * semantics, which would leave accept() blocked through shutdown and
     * make systemd wait the full stop timeout. Mirrors the agent. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    int srv = vsock_listen(VSOCK_PORT);
    if (srv < 0) return 1;
    agent_log("listening on vsock :%u", VSOCK_PORT);

    while (!g_stop) {
        struct sockaddr_vm peer;
        socklen_t plen = sizeof(peer);
        int c = accept(srv, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            if (errno == EINTR) continue;
            agent_log("accept: %s", strerror(errno));
            break;
        }
        agent_log("client connected (cid=%u)", peer.svm_cid);
        capture_loop(c);
        close(c);
    }

    close(srv);
    return 0;
}
