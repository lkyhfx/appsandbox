/* SPDX-License-Identifier: MIT */
/*
 * Isolated Mutter probe client.  It paints an opaque 3840x2160 Wayland
 * surface with a frame-dependent RGBA semantic pattern and a moving square.  The
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
    struct wl_output *output;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffers[BUFFER_COUNT];
    bool busy[BUFFER_COUNT];
    bool buffer_initialized[BUFFER_COUNT];
    unsigned previous_square_x[BUFFER_COUNT];
    unsigned previous_square_y[BUFFER_COUNT];
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
    (void)toplevel;
    struct app *app = data;
    bool fullscreen = false;
    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
            fullscreen = true;
    }
    fprintf(stdout, "CLIENT_CONFIGURE width=%d height=%d fullscreen=%d\\n",
            width, height, fullscreen ? 1 : 0);
    fflush(stdout);
    (void)app;
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

static const struct wl_output_listener output_listener;

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
    } else if (strcmp(interface, wl_output_interface.name) == 0 &&
               !app->output) {
        app->output = wl_registry_bind(registry, name, &wl_output_interface,
                                       version < 2 ? version : 2);
        wl_output_add_listener(app->output, &output_listener, app);
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

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{
    (void)data;
    (void)output;
    (void)subpixel;
    (void)transform;
    fprintf(stdout, "CLIENT_OUTPUT geometry=%d,%d physical=%dx%d make=%s model=%s\\n",
            x, y, physical_width, physical_height, make, model);
    fflush(stdout);
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)data;
    (void)output;
    fprintf(stdout, "CLIENT_OUTPUT mode flags=0x%x size=%dx%d refresh=%d\\n",
            flags, width, height, refresh);
    fflush(stdout);
}

static void output_done(void *data, struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)data;
    (void)output;
    fprintf(stdout, "CLIENT_OUTPUT scale=%d\\n", factor);
    fflush(stdout);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
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
    /* Headless Mutter presents this 3840x2160 monitor at a 30 Hz frame
     * clock while the D3D12 flush hook can publish four observations per
     * client callback.  Keep the semantic pattern in the producer's frame
     * domain so the consumer's 120-frame diagnostics remain deterministic. */
    const uint64_t pattern_frame = app->frame * 4U + 3U;
    const uint8_t red = 31U;
    const uint8_t green = 127U;
    const uint8_t blue = 223U;
    const unsigned square_x = (unsigned)((pattern_frame * 17U) % 256U);
    const unsigned square_y = (unsigned)((pattern_frame * 11U) % 256U);
    const uint32_t base_pixel = 0xff000000U |
                                ((uint32_t)red << 16) |
                                ((uint32_t)green << 8) | blue;
    const uint32_t square_pixel = 0xff000000U |
                                  ((uint32_t)(red ^ 0xffU) << 16) |
                                  ((uint32_t)(green ^ 0x55U) << 8) |
                                  (blue ^ 0xa3U);
    if (!app->buffer_initialized[index]) {
        for (unsigned y = 0; y < HEIGHT; ++y)
            for (unsigned x = 0; x < WIDTH; ++x)
                pixels[(size_t)y * WIDTH + x] = base_pixel;
        app->buffer_initialized[index] = true;
    } else {
        for (unsigned y = app->previous_square_y[index];
             y < app->previous_square_y[index] + 256U; ++y)
            for (unsigned x = app->previous_square_x[index];
                 x < app->previous_square_x[index] + 256U; ++x)
                pixels[(size_t)y * WIDTH + x] = base_pixel;
    }
    for (unsigned y = square_y; y < square_y + 256U; ++y) {
        for (unsigned x = square_x; x < square_x + 256U; ++x)
            pixels[(size_t)y * WIDTH + x] = square_pixel;
    }
    app->previous_square_x[index] = square_x;
    app->previous_square_y[index] = square_y;
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
    xdg_toplevel_set_app_id(app.toplevel, "asb-mutter-d3d12-pattern");
    wl_surface_set_buffer_scale(app.surface, 1);
    struct wl_region *opaque = wl_compositor_create_region(app.compositor);
    if (!opaque)
        fail("opaque-region-create");
    wl_region_add(opaque, 0, 0, WIDTH, HEIGHT);
    wl_surface_set_opaque_region(app.surface, opaque);
    wl_region_destroy(opaque);
    xdg_toplevel_set_fullscreen(app.toplevel, app.output);
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
