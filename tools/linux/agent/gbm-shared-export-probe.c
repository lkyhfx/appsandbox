/* SPDX-License-Identifier: MIT
 * Verify that a GBM shared allocation exports a real DRM PRIME dma-buf.
 * This does not modeset, map pixels, or modify compositor configuration.
 */
#include <drm.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

int main(int argc, char **argv)
{
    int rc = 1, node = -1, dma_fd = -1;
    struct gbm_device *gbm = NULL;
    struct gbm_bo *bo = NULL;
    uint32_t gem_handle = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2) {
        fprintf(stderr, "Usage: %s /dev/dri/cardN|/dev/dri/renderDN\n", argv[0]);
        return 2;
    }

    node = open(argv[1], O_RDWR | O_CLOEXEC);
    if (node < 0) {
        perror("open-drm-node");
        goto done;
    }
    gbm = gbm_create_device(node);
    if (!gbm) {
        perror("gbm-create-device");
        goto done;
    }
    printf("gbm_backend=%s node=%s\n", gbm_device_get_backend_name(gbm), argv[1]);

    /* The Mesa GBM DRI backend adds __DRI_IMAGE_USE_SHARE for this path. */
    bo = gbm_bo_create(gbm, 256, 256, GBM_FORMAT_ABGR8888,
                       GBM_BO_USE_RENDERING);
    if (!bo) {
        perror("gbm-bo-create-shared");
        goto done;
    }

    printf("fourcc=0x%x stride=%u modifier=0x%llx\n",
           gbm_bo_get_format(bo), gbm_bo_get_stride(bo),
           (unsigned long long)gbm_bo_get_modifier(bo));
    dma_fd = gbm_bo_get_fd(bo);
    printf("export_fd=%d\n", dma_fd);
    if (dma_fd < 0 || fcntl(dma_fd, F_GETFD) < 0) {
        perror("gbm-bo-get-fd");
        puts("FAIL stage=gbm-export-valid-fd");
        goto done;
    }

    char proc_path[64], target[256];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", dma_fd);
    ssize_t target_len = readlink(proc_path, target, sizeof(target) - 1);
    if (target_len >= 0) {
        target[target_len] = '\0';
        printf("fd_target=%s\n", target);
    }

    /* A D3D12 opaque shared handle is not interchangeable with dma-buf. */
    if (drmPrimeFDToHandle(node, dma_fd, &gem_handle) != 0) {
        perror("drm-prime-fd-to-handle");
        puts("FAIL stage=drm-prime-import descriptor-is-not-dma-buf");
        goto done;
    }
    printf("PASS stage=drm-prime-import gem_handle=%u\n", gem_handle);
    rc = 0;

done:
    if (gem_handle) {
        struct drm_gem_close close_arg = { .handle = gem_handle };
        if (ioctl(node, DRM_IOCTL_GEM_CLOSE, &close_arg) != 0)
            perror("drm-gem-close");
    }
    if (dma_fd >= 0)
        close(dma_fd);
    if (bo)
        gbm_bo_destroy(bo);
    if (gbm)
        gbm_device_destroy(gbm);
    if (node >= 0)
        close(node);
    return rc;
}
