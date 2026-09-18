/* SPDX-License-Identifier: MIT */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <gbm.h>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    const char *device_path = "/dev/dri/renderD128";
    int fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open render node");
        return 2;
    }

    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) {
        perror("gbm_create_device");
        close(fd);
        return 3;
    }
    printf("GBM backend=%s fd=%d\n",
           gbm_device_get_backend_name(gbm), gbm_device_get_fd(gbm));

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
            "eglGetPlatformDisplayEXT");
    EGLDisplay display = EGL_NO_DISPLAY;
    if (get_platform_display)
        display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (display == EGL_NO_DISPLAY)
        display = eglGetDisplay((EGLNativeDisplayType)gbm);
    if (display == EGL_NO_DISPLAY) {
        fprintf(stderr, "FAIL eglGetDisplay error=0x%04x\n", eglGetError());
        gbm_device_destroy(gbm);
        close(fd);
        return 4;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "FAIL eglInitialize error=0x%04x\n", eglGetError());
        gbm_device_destroy(gbm);
        close(fd);
        return 5;
    }

    printf("EGL version=%s vendor=%s client_apis=%s\n",
           eglQueryString(display, EGL_VERSION),
           eglQueryString(display, EGL_VENDOR),
           eglQueryString(display, EGL_CLIENT_APIS));

    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "FAIL eglBindAPI error=0x%04x\n", eglGetError());
        eglTerminate(display);
        gbm_device_destroy(gbm);
        close(fd);
        return 6;
    }

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint config_count = 0;
    if (!eglChooseConfig(display, config_attributes, &config, 1,
                         &config_count) || config_count != 1) {
        fprintf(stderr, "FAIL eglChooseConfig error=0x%04x\n", eglGetError());
        eglTerminate(display);
        gbm_device_destroy(gbm);
        close(fd);
        return 7;
    }

    const EGLint context_attributes[] = { EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                          context_attributes);
    const EGLint surface_attributes[] = {
        EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE,
    };
    EGLSurface surface = eglCreatePbufferSurface(display, config,
                                                 surface_attributes);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) {
        fprintf(stderr, "FAIL eglCreateContext/MakeCurrent error=0x%04x\n",
                eglGetError());
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display, context);
        eglTerminate(display);
        gbm_device_destroy(gbm);
        close(fd);
        return 8;
    }

    const GLubyte *vendor = glGetString(GL_VENDOR);
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version = glGetString(GL_VERSION);
    printf("PASS egl-context vendor=%s renderer=%s version=%s\n",
           vendor ? (const char *)vendor : "(null)",
           renderer ? (const char *)renderer : "(null)",
           version ? (const char *)version : "(null)");

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    gbm_device_destroy(gbm);
    close(fd);
    return 0;
}
