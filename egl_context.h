#pragma once

#include <gbm.h>
#include <EGL/egl.h>

struct wl_display;
struct wl_surface;
struct wl_egl_window;

struct EGLState {
    int         drm_fd  = -1;               // headless (GBM) path only
    gbm_device *gbm_dev = nullptr;           // headless (GBM) path only
    EGLDisplay  dpy     = EGL_NO_DISPLAY;
    EGLContext  ctx     = EGL_NO_CONTEXT;
    EGLSurface  surface = EGL_NO_SURFACE;    // windowed (Wayland) path only
};

// Headless, surfaceless GBM context — default pipeline-testing path.
bool setup_egl(EGLState &egl);

#ifdef HAVE_WAYLAND_PREVIEW
// Windowed EGL context bound to a Wayland surface — used by --preview.
// `display`/`surface` must already exist (surface must have a role assigned,
// i.e. this is called after the xdg_surface/agl_shell handshake). On success,
// `*out_egl_window` receives the wl_egl_window the caller must keep alive
// (for resize) and destroy (wl_egl_window_destroy) during teardown, before
// disconnecting the Wayland display.
bool setup_egl_wayland(EGLState &egl, wl_display *display, wl_surface *surface,
                       int width, int height, wl_egl_window **out_egl_window);
#endif

bool check_extensions(const EGLState &egl);
void teardown_egl(EGLState &egl);
