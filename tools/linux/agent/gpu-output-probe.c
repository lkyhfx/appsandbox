/* SPDX-License-Identifier: MIT
 * Isolated compositor-output prerequisite probe; never modesets.
 * GPU render -> EGLImage -> DMA-BUF -> EGLImage -> GL framebuffer.
 * glFinish and diagnostic pixels deliberately use CPU synchronization /
 * readback: this is a correctness probe, NOT a zero-copy performance claim.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int has(const char *list, const char *name)
{
    if (!list) return 0;
    size_t n = strlen(name);
    const char *p = list;
    while ((p = strstr(p, name))) {
        if ((p == list || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return 1;
        p += n;
    }
    return 0;
}

#define CHECK(c, label) do { if (!(c)) { \
    fprintf(stderr, "FAIL stage=%s egl=0x%x gl=0x%x\n", label, eglGetError(), \
            current ? glGetError() : 0); goto done; } } while (0)
#define LOAD(type, name) type name = (type)eglGetProcAddress(#name)

int main(int argc, char **argv)
{
    int rc = 1, node = -1, current = 0;
    int fds[4] = {-1, -1, -1, -1}, strides[4] = {0}, offsets[4] = {0}, planes = 0, fourcc;
    EGLuint64KHR modifiers[4] = {0};
    struct gbm_device *gbm = NULL;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLImageKHR source = EGL_NO_IMAGE_KHR, imported = EGL_NO_IMAGE_KHR;
    GLuint tex[2] = {0}, fb[2] = {0};
    LOAD(PFNEGLGETPLATFORMDISPLAYEXTPROC, eglGetPlatformDisplayEXT);
    LOAD(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR);
    LOAD(PFNEGLDESTROYIMAGEKHRPROC, eglDestroyImageKHR);
    LOAD(PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC, eglExportDMABUFImageQueryMESA);
    LOAD(PFNEGLEXPORTDMABUFIMAGEMESAPROC, eglExportDMABUFImageMESA);
    LOAD(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2) {
        fprintf(stderr, "Usage: %s surfaceless|/dev/dri/cardN|/dev/dri/renderDN\n", argv[0]);
        return 2;
    }
    CHECK(eglGetPlatformDisplayEXT, "platform-entrypoint");
    if (!strcmp(argv[1], "surfaceless")) {
        dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    } else {
        node = open(argv[1], O_RDWR | O_CLOEXEC);
        CHECK(node >= 0, "open-drm-node");
        gbm = gbm_create_device(node);
        CHECK(gbm, "gbm-create-device");
        printf("gbm_backend=%s node=%s\n", gbm_device_get_backend_name(gbm), argv[1]);
        dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    }
    CHECK(dpy != EGL_NO_DISPLAY && eglInitialize(dpy, NULL, NULL), "egl-initialize");
    const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
    printf("egl_vendor=%s\nextensions=%s\n", eglQueryString(dpy, EGL_VENDOR), ext ? ext : "");
    CHECK(has(ext, "EGL_KHR_surfaceless_context"), "surfaceless-context-extension");
    CHECK(eglBindAPI(EGL_OPENGL_ES_API), "bind-gles");
    EGLConfig config;
    EGLint count;
    const EGLint cfg[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                         EGL_SURFACE_TYPE, 0, EGL_NONE};
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    CHECK(eglChooseConfig(dpy, cfg, &config, 1, &count) && count, "egl-config");
    ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ca);
    CHECK(ctx != EGL_NO_CONTEXT, "egl-context");
    CHECK(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx), "make-current");
    current = 1;
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("renderer=%s\n", renderer ? renderer : "unknown");
    /* This probe targets the known GPU-PV backend; reject silent fallback. */
    CHECK(renderer && strstr(renderer, "D3D12") && !strstr(renderer, "llvmpipe"), "d3d12-renderer");
    glGenTextures(2, tex);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(2, fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[0], 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "source-fbo");
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    unsigned char pixel[4] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    CHECK(glGetError() == GL_NO_ERROR && pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255, "source-pixel");
    puts("PASS stage=gpu-render diagnostic_readback=1-pixel");
    CHECK(has(ext, "EGL_KHR_image_base") && has(ext, "EGL_KHR_gl_texture_2D_image") && eglCreateImageKHR && eglDestroyImageKHR, "egl-image-extension");
    const EGLint ia[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    source = eglCreateImageKHR(dpy, ctx, EGL_GL_TEXTURE_2D_KHR,
                               (EGLClientBuffer)(uintptr_t)tex[0], ia);
    CHECK(source != EGL_NO_IMAGE_KHR, "source-image");
    CHECK(has(ext, "EGL_MESA_image_dma_buf_export") && eglExportDMABUFImageQueryMESA && eglExportDMABUFImageMESA, "dma-buf-export-extension");
    CHECK(eglExportDMABUFImageQueryMESA(dpy, source, &fourcc, &planes, modifiers), "export-query");
    printf("fourcc=0x%x planes=%d modifier=0x%llx\n", fourcc, planes, (unsigned long long)modifiers[0]);
    /* RGB probe only; do not silently discard auxiliary modifier planes. */
    CHECK(planes == 1, "single-plane-rgb");
    CHECK(eglExportDMABUFImageMESA(dpy, source, fds, strides, offsets), "export-fd");
    printf("export_fd=%d stride=%d offset=%d\n", fds[0], strides[0], offsets[0]);
    CHECK(fds[0] >= 0 && strides[0] > 0 && offsets[0] >= 0, "export-metadata");
    printf("PASS stage=dma-buf-export stride=%d offset=%d\n", strides[0], offsets[0]);
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fds[0]);
    FILE *info = fopen(path, "r");
    if (info) { while (fgets(line, sizeof(line), info)) fputs(line, stdout); fclose(info); }
    CHECK(has(ext, "EGL_EXT_image_dma_buf_import"), "dma-buf-import-extension");
    EGLint attrs[24] = {EGL_WIDTH, 256, EGL_HEIGHT, 256,
        EGL_LINUX_DRM_FOURCC_EXT, fourcc, EGL_DMA_BUF_PLANE0_FD_EXT, fds[0],
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, offsets[0], EGL_DMA_BUF_PLANE0_PITCH_EXT, strides[0]};
    int a = 12;
    if (modifiers[0] != DRM_FORMAT_MOD_INVALID) {
        CHECK(has(ext, "EGL_EXT_image_dma_buf_import_modifiers"), "modifier-extension");
        attrs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attrs[a++] = (EGLint)(uint32_t)modifiers[0];
        attrs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attrs[a++] = (EGLint)(uint32_t)(modifiers[0] >> 32);
    }
    attrs[a] = EGL_NONE;
    imported = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    CHECK(imported != EGL_NO_IMAGE_KHR, "dma-buf-import");
    CHECK(has((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image") && glEGLImageTargetTexture2DOES, "gl-image-extension");
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, imported);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[1]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
    CHECK(glGetError() == GL_NO_ERROR && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "import-fbo");
    for (int i = 0; i < 120; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, fb[0]);
        glClearColor((float)(i & 1), (float)((i >> 1) & 1), (float)((i >> 2) & 1), 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, fb[1]);
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        CHECK(glGetError() == GL_NO_ERROR && pixel[0] == (i & 1) * 255 &&
            pixel[1] == ((i >> 1) & 1) * 255 && pixel[2] == ((i >> 2) & 1) * 255 && pixel[3] == 255, "import-pixel-reuse");
    }
    puts("PASS stage=egl-roundtrip frames=120 sync=glFinish diagnostic_readback=1-pixel/frame");
    puts("NOT_TESTED: compositor, cross-process, CUDA, native fences, scanout, no-hidden-copy");
    rc = 0;
done:
    if (current) { glDeleteFramebuffers(2, fb); glDeleteTextures(2, tex); }
    if (imported != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(dpy, imported);
    if (source != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(dpy, source);
    for (int i = 0; i < 4; ++i) if (fds[i] >= 0) close(fds[i]);
    if (current) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    if (dpy != EGL_NO_DISPLAY) eglTerminate(dpy);
    if (gbm) gbm_device_destroy(gbm);
    if (node >= 0) close(node);
    return rc;
}
