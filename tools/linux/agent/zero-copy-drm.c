/* Inspect active framebuffer PRIME exporter. No modeset, mmap or pixel reads.
 * gcc -Wall -O2 zero-copy-drm.c $(pkg-config --cflags --libs libdrm) -o drm-probe
 * GETFB2 handles require DRM master or CAP_SYS_ADMIN; use sudo if handles=0.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/dev/dri/card1";
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(path); return 1; }
    drmVersionPtr ver = drmGetVersion(fd);
    if (ver) { printf("driver=%.*s\n", ver->name_len, ver->name); drmFreeVersion(ver); }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) { perror("client cap"); return 1; }
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes) { perror("planes"); return 1; }
    int exported = 0;
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(fd, planes->planes[i]);
        if (!p) continue;
        if (!p->fb_id || !p->crtc_id) { drmModeFreePlane(p); continue; }
        printf("plane=%u crtc=%u fb=%u\n", p->plane_id, p->crtc_id, p->fb_id);
        drmModeFB2 *fb = drmModeGetFB2(fd, p->fb_id);
        drmModeFreePlane(p);
        if (!fb) { perror("GETFB2"); continue; }
        printf("width=%u height=%u format=%.4s modifier=0x%llx pitch=%u handle=%u\n",
               fb->width, fb->height, (char *)&fb->pixel_format,
               (unsigned long long)fb->modifier, fb->pitches[0], fb->handles[0]);
        if (!fb->handles[0]) printf("NEEDS_PRIVILEGE: GETFB2 returned no GEM handle\n");
        for (int j = 0; j < 4; j++) {
            uint32_t h = fb->handles[j]; if (!h) continue;
            int duplicate = 0;
            for (int k = 0; k < j; k++) if (fb->handles[k] == h) duplicate = 1;
            if (duplicate) continue;
            int dma = -1;
            if (!drmPrimeHandleToFD(fd, h, DRM_CLOEXEC, &dma)) {
                char info[128], line[512];
                snprintf(info, sizeof(info), "/proc/self/fdinfo/%d", dma);
                FILE *f = fopen(info, "r");
                if (f) { while (fgets(line, sizeof(line), f)) fputs(line, stdout); fclose(f); }
                exported++; close(dma);
            } else perror("PRIME export");
            drmCloseBufferHandle(fd, h);
        }
        drmModeFreeFB2(fb);
    }
    drmModeFreePlaneResources(planes); close(fd);
    printf("exported=%d; exporter identity alone does not prove no upstream readback\n", exported);
    return exported ? 0 : 2;
}
