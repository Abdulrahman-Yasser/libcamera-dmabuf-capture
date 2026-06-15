#include "egl_context.h"

#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

static bool has_ext(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return false;
    return strstr(haystack, needle) != nullptr;
}

bool setup_egl(EGLState &egl)
{
    /* 1. DRM render node — no display server, no root privileges needed */
    egl.drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (egl.drm_fd < 0) {
        perror("[egl] open /dev/dri/renderD128");
        return false;
    }
    std::cout << "[egl] opened /dev/dri/renderD128 (fd=" << egl.drm_fd << ")\n";

    /* 2. GBM device wraps the DRM fd and lets EGL work without a window */
    egl.gbm_dev = gbm_create_device(egl.drm_fd);
    if (!egl.gbm_dev) {
        std::cerr << "[egl] gbm_create_device failed\n";
        return false;
    }
    std::cout << "[egl] GBM backend: " << gbm_device_get_backend_name(egl.gbm_dev) << "\n";

    /* 3. EGL display — prefer the explicit platform extension so Mesa
     *    knows this is a GBM device, not a raw fd or X11 display. */
    {
        auto fn = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
                      eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (fn) {
            egl.dpy = fn(EGL_PLATFORM_GBM_MESA,
                         static_cast<void *>(egl.gbm_dev), nullptr);
        } else {
            /* Mesa also accepts the GBM pointer cast to EGLNativeDisplayType */
            egl.dpy = eglGetDisplay(
                reinterpret_cast<EGLNativeDisplayType>(egl.gbm_dev));
        }
    }
    if (egl.dpy == EGL_NO_DISPLAY) {
        std::cerr << "[egl] failed to get EGL display\n";
        return false;
    }

    /* 4. Initialize EGL */
    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl.dpy, &major, &minor)) {
        std::cerr << "[egl] eglInitialize failed: 0x"
                  << std::hex << eglGetError() << std::dec << "\n";
        return false;
    }
    std::cout << "[egl] EGL version : " << major << "." << minor << "\n";
    std::cout << "[egl] EGL vendor  : " << eglQueryString(egl.dpy, EGL_VENDOR)  << "\n";
    std::cout << "[egl] EGL version : " << eglQueryString(egl.dpy, EGL_VERSION) << "\n";

    /* 5. Surfaceless context requires this extension — check before use */
    const char *egl_exts = eglQueryString(egl.dpy, EGL_EXTENSIONS);
    if (!has_ext(egl_exts, "EGL_KHR_surfaceless_context")) {
        std::cerr << "[egl] EGL_KHR_surfaceless_context not supported "
                     "(driver too old)\n";
        return false;
    }

    /* 6. Bind OpenGL ES API */
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        std::cerr << "Maybe EGL_OPENGL_ES_API is not supported ! \n";

    /* 7. Find a GLES2-capable config — no surface attributes needed */
    static const EGLint cfg_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig cfg   = nullptr;
    EGLint    n_cfg = 0;
    if (!eglChooseConfig(egl.dpy, cfg_attribs, &cfg, 1, &n_cfg) || n_cfg == 0) {
        std::cerr << "[egl] no EGL config with EGL_OPENGL_ES2_BIT\n";
        return false;
    }

    /* 8. GLES2 context */
    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    egl.ctx = eglCreateContext(egl.dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (egl.ctx == EGL_NO_CONTEXT) {
        std::cerr << "[egl] eglCreateContext failed: 0x"
                  << std::hex << eglGetError() << std::dec << "\n";
        return false;
    }

    /* 9. Surfaceless MakeCurrent — RPi4 v3d driver supports this */
    if (!eglMakeCurrent(egl.dpy,
                        EGL_NO_SURFACE, EGL_NO_SURFACE,
                        egl.ctx)) {
        std::cerr << "[egl] eglMakeCurrent (surfaceless) failed: 0x"
                  << std::hex << eglGetError() << std::dec << "\n";
        return false;
    }

    /* GL strings are only valid after MakeCurrent */
    std::cout << "[egl] GL renderer : " << glGetString(GL_RENDERER) << "\n";
    std::cout << "[egl] GL version  : " << glGetString(GL_VERSION)  << "\n";

    return true;
}

bool check_extensions(const EGLState &egl)
{
    const char *egl_exts = eglQueryString(egl.dpy, EGL_EXTENSIONS);
    const char *gl_exts  =
        reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));

    struct { const char *name; bool is_egl; } required[] = {
        { "EGL_EXT_image_dma_buf_import", true  },
        { "EGL_KHR_image_base",           true  },
        { "GL_OES_EGL_image",             false },
        { "GL_OES_EGL_image_external",    false },
    };

    std::cout << "\n[egl] Required extension check:\n";
    bool all_ok = true;
    for (auto &r : required) {
        const char *pool  = r.is_egl ? egl_exts : gl_exts;
        bool        found = has_ext(pool, r.name);
        std::cout << "  " << (found ? "[OK]     " : "[MISSING]")
                  << " " << r.name << "\n";
        if (!found) all_ok = false;
    }
    std::cout << "\n";
    return all_ok;
}

void teardown_egl(EGLState &egl)
{
    if (egl.dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl.dpy,
                       EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (egl.ctx != EGL_NO_CONTEXT)
            eglDestroyContext(egl.dpy, egl.ctx);
        eglTerminate(egl.dpy);
        eglReleaseThread();
    }
    if (egl.gbm_dev) gbm_device_destroy(egl.gbm_dev);
    if (egl.drm_fd >= 0) close(egl.drm_fd);
    egl = {};
}
