#pragma once

#include <gbm.h>
#include <EGL/egl.h>

struct EGLState {
    int         drm_fd  = -1;
    gbm_device *gbm_dev = nullptr;
    EGLDisplay  dpy     = EGL_NO_DISPLAY;
    EGLContext  ctx     = EGL_NO_CONTEXT;
};

bool setup_egl(EGLState &egl);
bool check_extensions(const EGLState &egl);
void teardown_egl(EGLState &egl);
