/*
 * egl_renderer_probe.c — query the GL_RENDERER of the private Weston/EGL stack.
 *
 * Connects to the private Wayland compositor started by the gpu-compositor
 * probe (WAYLAND_DISPLAY + XDG_RUNTIME_DIR), creates a tiny EGL pbuffer
 * context on it, and prints the GL_RENDERER string as:
 *
 *     GL_RENDERER=<string>
 *
 * The probe validates that string against its software/hardware lists: a
 * software rasterizer (llvmpipe, softpipe, swrast, pixman, ...) must be
 * rejected, so the gpu-compositor capability can only be evidenced on a real
 * GL/VirGL stack.
 *
 * The helper is built inside the gpurunner bundle (libEGL, libGLESv2,
 * libwayland-client) and never runs in fixture mode.
 *
 * Build (gpurunner bundle):
 *   cc -O2 -o egl_renderer_probe egl_renderer_probe.c \
 *      $(pkg-config --cflags --libs egl glesv2) -lwayland-client
 */

#include <stdio.h>
#include <string.h>

#include <wayland-client.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#ifndef EGL_PLATFORM_WAYLAND_EXT
#define EGL_PLATFORM_WAYLAND_EXT 0x31D8
#endif

static EGLDisplay open_wayland_egl(struct wl_display *display)
{
    /* Prefer the platform-extension entry point; fall back to the classic
     * eglGetDisplay cast, which mesa also accepts with a wl_display*. */
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform) {
        EGLDisplay dpy = get_platform(EGL_PLATFORM_WAYLAND_EXT, display, NULL);
        if (dpy != EGL_NO_DISPLAY)
            return dpy;
    }
    return eglGetDisplay((EGLNativeDisplayType)display);
}

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "egl-renderer-probe: cannot connect to the Wayland compositor\n");
        return 1;
    }

    EGLDisplay egl = open_wayland_egl(display);
    if (egl == EGL_NO_DISPLAY) {
        fprintf(stderr, "egl-renderer-probe: cannot open an EGL display on Wayland\n");
        wl_display_disconnect(display);
        return 1;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl, &major, &minor)) {
        fprintf(stderr, "egl-renderer-probe: eglInitialize failed\n");
        eglTerminate(egl);
        wl_display_disconnect(display);
        return 1;
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint config_count = 0;
    if (!eglChooseConfig(egl, config_attribs, &config, 1, &config_count)
            || config_count < 1 || config == NULL) {
        fprintf(stderr, "egl-renderer-probe: no suitable EGL config\n");
        eglTerminate(egl);
        wl_display_disconnect(display);
        return 1;
    }

    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(egl, config, EGL_NO_CONTEXT, context_attribs);
    if (context == EGL_NO_CONTEXT) {
        fprintf(stderr, "egl-renderer-probe: eglCreateContext failed\n");
        eglTerminate(egl);
        wl_display_disconnect(display);
        return 1;
    }

    const EGLint pbuf_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(egl, config, pbuf_attribs);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "egl-renderer-probe: eglCreatePbufferSurface failed\n");
        eglDestroyContext(egl, context);
        eglTerminate(egl);
        wl_display_disconnect(display);
        return 1;
    }

    if (!eglMakeCurrent(egl, surface, surface, context)) {
        fprintf(stderr, "egl-renderer-probe: eglMakeCurrent failed\n");
        eglDestroySurface(egl, surface);
        eglDestroyContext(egl, context);
        eglTerminate(egl);
        wl_display_disconnect(display);
        return 1;
    }

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    if (!renderer || !renderer[0]) {
        fprintf(stderr, "egl-renderer-probe: GL_RENDERER is empty\n");
        return 1;
    }
    printf("GL_RENDERER=%s\n", renderer);
    fflush(stdout);

    eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl, surface);
    eglDestroyContext(egl, context);
    eglTerminate(egl);
    wl_display_disconnect(display);
    return 0;
}
