/* SPDX-License-Identifier: MIT */
/*
 * Isolated Mutter probe client.  It paints an opaque 3840x2160 Wayland
 * surface with a frame-dependent BGRA pattern and a moving square.  The
 * surface is client content; the compositor output is captured by the
 * opt-in Mesa D3D12 hook, never by this client.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

enum {
    WIDTH = 3840,
    HEIGHT = 2160,
    BUFFER_COUNT = 2,
    FRAME_COUNT_DEFAULT = 3600,
    STRIDE = WIDTH * 4,
    BUFFER_BYTES = STRIDE * HEIGHT,
};

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffers[BUFFER_COUNT];
    bool busy[BUFFER_COUNT];
    struct wl_callback *frame_callback;
    void *pixels;
    int shm_fd;
    size_t mapped_bytes;
    uint64_t frame;
    uint64_t frame_limit;
    bool configured;
    bool pending_paint;
    bool running;
};

static void fail(const char *what)
{
    fprintf(stderr, "FAIL stage=wayland-pattern-client reason=%s errno=%d\n",
            what, errno);
    exit(1);
}

static int create_memfd(const char *name)
{
    return (int)syscall(SYS_memfd_create, name, MFD_CLOEXEC);
}

static void paint(struct app *app);

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct app *app = data;
    for (unsigned i = 0; i < BUFFER_COUNT; ++i) {
        if (app->buffers[i] == buffer) {
            app->busy[i] = false;
            if (app->pending_paint && !app->frame_callback)
                paint(app);
            return;
        }
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void frame_done(void *data, struct wl_callback *callback,
                       uint32_t callback_time)
{
    struct app *app = data;
    (void)callback_time;
    wl_callback_destroy(callback);
    app->frame_callback = NULL;
    ++app->frame;
    if (app->frame >= app->frame_limit) {
        app->running = false;
        return;
    }
    paint(app);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
                         uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface,
                                  uint32_t serial)
{
    struct app *app = data;
    xdg_surface_ack_configure(surface, serial);
    app->configured = true;
    paint(app);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    struct app *app = data;
    (void)toplevel;
    app->running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct app *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t bind_version = version < 4 ? version : 4;
        app->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface,
                                           bind_version);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                        1);
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void create_buffers(struct app *app)
{
    app->shm_fd = create_memfd("asb-mutter-wayland-pattern");
    if (app->shm_fd < 0)
        fail("memfd-create");
    app->mapped_bytes = (size_t)BUFFER_COUNT * BUFFER_BYTES;
    if (ftruncate(app->shm_fd, (off_t)app->mapped_bytes) != 0)
        fail("memfd-resize");
    app->pixels = mmap(NULL, app->mapped_bytes, PROT_READ | PROT_WRITE,
                       MAP_SHARED, app->shm_fd, 0);
    if (app->pixels == MAP_FAILED)
        fail("memfd-map");

    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, app->shm_fd,
                                                   (int)app->mapped_bytes);
    if (!pool)
        fail("shm-pool-create");
    for (unsigned i = 0; i < BUFFER_COUNT; ++i) {
        app->buffers[i] = wl_shm_pool_create_buffer(
            pool, (int)(i * BUFFER_BYTES), WIDTH, HEIGHT, STRIDE,
            WL_SHM_FORMAT_ARGB8888);
        if (!app->buffers[i])
            fail("shm-buffer-create");
        wl_buffer_add_listener(app->buffers[i], &buffer_listener, app);
    }
    wl_shm_pool_destroy(pool);
    close(app->shm_fd);
    app->shm_fd = -1;
}

static void paint(struct app *app)
{
    if (!app->configured || !app->running)
        return;
    unsigned index = BUFFER_COUNT;
    for (unsigned i = 0; i < BUFFER_COUNT; ++i) {
        if (!app->busy[i]) {
            index = i;
            break;
        }
    }
    if (index == BUFFER_COUNT) {
        app->pending_paint = true;
        return;
    }
    app->pending_paint = false;

    uint32_t *pixels = (uint32_t *)((uint8_t *)app->pixels +
                                    index * BUFFER_BYTES);
    const uint8_t red = (uint8_t)(app->frame & 0xffU);
    const uint8_t green = (uint8_t)((app->frame >> 8) & 0xffU);
    const uint8_t blue = (uint8_t)((app->frame >> 16) & 0xffU);
    const unsigned square_x = (unsigned)((app->frame * 17U) % (WIDTH - 256));
    const unsigned square_y = (unsigned)((app->frame * 11U) % (HEIGHT - 256));
    for (unsigned y = 0; y < HEIGHT; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            uint8_t r = red;
            uint8_t g = green;
            uint8_t b = blue;
            if (x >= square_x && x < square_x + 256 &&
                y >= square_y && y < square_y + 256) {
                r ^= 0xffU;
                g ^= 0x55U;
                b ^= 0xa3U;
            }
            /* ARGB8888 is BGRA in memory on the little-endian guest. */
            pixels[(size_t)y * WIDTH + x] = 0xff000000U |
                                            ((uint32_t)r << 16) |
                                            ((uint32_t)g << 8) | b;
        }
    }
    app->busy[index] = true;
    wl_surface_attach(app->surface, app->buffers[index], 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0, WIDTH, HEIGHT);
    app->frame_callback = wl_surface_frame(app->surface);
    wl_callback_add_listener(app->frame_callback, &frame_listener, app);
    wl_surface_commit(app->surface);
    if ((app->frame % 120U) == 0U) {
        fprintf(stdout, "CLIENT_FRAME frame=%llu square=%u,%u\n",
                (unsigned long long)app->frame, square_x, square_y);
        fflush(stdout);
    }
}

int main(void)
{
    struct app app = {.shm_fd = -1, .frame_limit = FRAME_COUNT_DEFAULT,
                      .running = true};
    const char *frame_limit = getenv("ASB_PATTERN_CLIENT_FRAMES");
    if (frame_limit && *frame_limit) {
        char *end = NULL;
        unsigned long long parsed = strtoull(frame_limit, &end, 10);
        if (end != frame_limit && *end == '\0' && parsed > 0)
            app.frame_limit = parsed;
    }
    app.display = wl_display_connect(NULL);
    if (!app.display)
        fail("wayland-connect");
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    if (wl_display_roundtrip(app.display) < 0 || !app.compositor ||
        !app.shm || !app.wm_base)
        fail("wayland-registry");

    create_buffers(&app);
    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    if (!app.surface || !app.xdg_surface || !app.toplevel)
        fail("xdg-surface-create");
    xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_title(app.toplevel, "AppSandbox Mutter D3D12 probe");
    xdg_toplevel_set_fullscreen(app.toplevel, NULL);
    wl_surface_commit(app.surface);
    fprintf(stdout, "CLIENT_READY width=%d height=%d opaque=1 dynamic=1\n",
            WIDTH, HEIGHT);
    fflush(stdout);

    while (app.running) {
        if (wl_display_dispatch(app.display) < 0) {
            fprintf(stderr, "CLIENT_EXIT reason=wayland-dispatch errno=%d\\n",
                    errno);
            return 1;
        }
    }
    fprintf(stdout, "CLIENT_EXIT reason=frame-limit-or-close frame=%llu\\n",
            (unsigned long long)app.frame);
    fflush(stdout);
    return 0;
}
