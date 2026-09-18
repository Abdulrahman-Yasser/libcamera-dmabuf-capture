#include "dmabuf_ring.h"

#ifdef BEV_IHS_PV_VULKAN
#include "vk_dmabuf_allocator.h"
#endif

#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// Whole-token match: a name must not be found inside a longer extension.
bool has_ext(const char *list, const char *name)
{
    if (!list) return false;
    const std::string haystack = std::string(" ") + list + " ";
    return haystack.find(std::string(" ") + name + " ") != std::string::npos;
}

} // namespace

const char *ring_allocator_name(RingAllocator allocator)
{
    switch (allocator) {
    case RingAllocator::Vulkan:     return "vulkan";
    case RingAllocator::ShellGbm:   return "shell-gbm";
    case RingAllocator::RenderNode: return "render-node-gbm";
    }
    return "?";
}

DmabufRing::~DmabufRing()
{
    // The render thread releases the ring before it tears its context down, so
    // by now there is normally nothing left that needs GL.
    release();
}

bool DmabufRing::init(const EGLState &egl, RingAllocator allocator,
                      gbm_device *shell_gbm, const VkDmabufAllocator *vk)
{
    egl_       = &egl;
    allocator_ = allocator;
    shell_gbm_ = shell_gbm;
    vk_        = vk;

    const char *exts = eglQueryString(egl.dpy, EGL_EXTENSIONS);
    if (!has_ext(exts, "EGL_EXT_image_dma_buf_import")) {
        std::fprintf(stderr, "[bev/ring] EGL display lacks EGL_EXT_image_dma_buf_import\n");
        return false;
    }
    has_import_modifiers_ = has_ext(exts, "EGL_EXT_image_dma_buf_import_modifiers");

    create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    image_target_texture_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!create_image_ || !destroy_image_ || !image_target_texture_) {
        std::fprintf(stderr, "[bev/ring] EGLImage entry points missing\n");
        return false;
    }

    // Optional: without a native fence the frame is synchronized with glFinish().
    //
    // BEV_IMPLICIT_SYNC=1 forces that path. It exists for ivi-homescreen shells
    // older than v3.0 83887302, whose EGL submit path, on a submit carrying an
    // acquire fence, stored a dup of the compositor's release fence in the
    // out-parameter and then overwrote it with the per-buffer eventfd, leaking
    // one sync_file per frame. The fd table then grew through 256, 512, ...
    // (each expansion an RCU wait, seen here as a ~120 ms stall) and hit the
    // 1024-fd limit about half a minute in. Submitting without an acquire fence
    // skips that branch — at the cost of the pipelining the fence buys, so this
    // is a fallback for old shells, not a default.
    const char *implicit = std::getenv("BEV_IMPLICIT_SYNC");
    const bool force_implicit = implicit && implicit[0] == '1';
    if (force_implicit)
        std::printf("[bev/ring] BEV_IMPLICIT_SYNC=1: submitting without acquire fences\n");
    if (!force_implicit && has_ext(exts, "EGL_ANDROID_native_fence_sync")) {
        create_sync_ = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        destroy_sync_ = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        dup_native_fence_ = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
            eglGetProcAddress("eglDupNativeFenceFDANDROID"));
        if (!create_sync_ || !destroy_sync_ || !dup_native_fence_)
            create_sync_ = nullptr, destroy_sync_ = nullptr, dup_native_fence_ = nullptr;
    }

    if (allocator_ == RingAllocator::Vulkan && !vk_) return false;
    if (allocator_ == RingAllocator::ShellGbm && !shell_gbm_) return false;
    if (allocator_ == RingAllocator::RenderNode && !egl.gbm_dev) return false;

    glGenFramebuffers(1, &src_fbo_);
    return true;
}

std::vector<uint64_t> DmabufRing::renderable_modifiers(EGLDisplay display, uint32_t fourcc)
{
    std::vector<uint64_t> out;
    if (!has_ext(eglQueryString(display, EGL_EXTENSIONS), "EGL_EXT_image_dma_buf_import_modifiers"))
        return out;
    auto query = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
        eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (!query) return out;

    EGLint n = 0;
    if (!query(display, (EGLint)fourcc, 0, nullptr, nullptr, &n) || n <= 0) return out;
    std::vector<EGLuint64KHR> mods((size_t)n);
    std::vector<EGLBoolean>   external_only((size_t)n);
    if (!query(display, (EGLint)fourcc, n, mods.data(), external_only.data(), &n)) return out;
    for (EGLint i = 0; i < n; ++i)
        if (!external_only[(size_t)i]) out.push_back(mods[(size_t)i]); // external-only can't be rendered to
    return out;
}

bool DmabufRing::allocate(uint32_t count, uint32_t width, uint32_t height,
                          uint32_t fourcc, uint64_t modifier)
{
    release();
    ++generation_;
    for (uint32_t i = 0; i < count; ++i) {
        RingBuffer b;
        b.buffer_id = generation_ * 8 + i;
        b.fourcc    = fourcc;
        b.width     = width;
        b.height    = height;
        if (!alloc_storage(b, modifier) || !import(b)) {
            destroy(b);
            release();
            return false;
        }
        buffers_.push_back(b);
    }
    std::printf("[bev/ring] %u x %ux%u fourcc=0x%08x mod=0x%llx via %s\n", count, width, height,
                fourcc, (unsigned long long)buffers_[0].modifier, ring_allocator_name(allocator_));
    return true;
}

bool DmabufRing::alloc_storage(RingBuffer &b, uint64_t modifier)
{
#ifdef BEV_IHS_PV_VULKAN
    if (allocator_ == RingAllocator::Vulkan) {
        auto *img = new VkDmabufImage;
        if (!vk_->create(b.width, b.height, modifier, *img)) {
            delete img;
            return false;
        }
        b.vk       = img;
        b.fd       = img->fd; // the ring owns the fd from here on
        img->fd    = -1;
        b.offset   = img->offset;
        b.stride   = img->stride;
        b.modifier = img->modifier;
        return true;
    }
#else
    if (allocator_ == RingAllocator::Vulkan) return false;
#endif

    gbm_device *dev = allocator_ == RingAllocator::ShellGbm ? shell_gbm_ : egl_->gbm_dev;
    // Scanout-capable on the shell's device, so a DRM_PLANE grant can put the
    // buffer straight on a KMS plane.
    const uint32_t flags = GBM_BO_USE_RENDERING |
                           (allocator_ == RingAllocator::ShellGbm ? GBM_BO_USE_SCANOUT : 0);
    gbm_bo *bo = nullptr;
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        uint64_t mod = modifier;
        bo = gbm_bo_create_with_modifiers2(dev, b.width, b.height, b.fourcc, &mod, 1, flags);
    }
    // Some drivers only take LINEAR as "no modifier".
    if (!bo && (modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID))
        bo = gbm_bo_create(dev, b.width, b.height, b.fourcc, flags);
    if (!bo) {
        std::fprintf(stderr, "[bev/ring] gbm_bo_create %ux%u fourcc=0x%08x mod=0x%llx failed\n",
                     b.width, b.height, b.fourcc, (unsigned long long)modifier);
        return false;
    }
    b.bo = bo;
    if (gbm_bo_get_plane_count(bo) != 1) {
        std::fprintf(stderr, "[bev/ring] gbm gave a multi-plane buffer; single plane needed\n");
        return false;
    }
    b.fd = gbm_bo_get_fd(bo);
    if (b.fd < 0) return false;
    b.offset = gbm_bo_get_offset(bo, 0);
    b.stride = gbm_bo_get_stride_for_plane(bo, 0);
    const uint64_t bo_mod = gbm_bo_get_modifier(bo);
    b.modifier = bo_mod != DRM_FORMAT_MOD_INVALID ? bo_mod : modifier;
    return true;
}

bool DmabufRing::import(RingBuffer &b)
{
    EGLint attribs[24];
    int n = 0;
    attribs[n++] = EGL_WIDTH;                     attribs[n++] = (EGLint)b.width;
    attribs[n++] = EGL_HEIGHT;                    attribs[n++] = (EGLint)b.height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;      attribs[n++] = (EGLint)b.fourcc;
    attribs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;     attribs[n++] = b.fd;
    attribs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attribs[n++] = (EGLint)b.offset;
    attribs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attribs[n++] = (EGLint)b.stride;
    if (has_import_modifiers_ && b.modifier != DRM_FORMAT_MOD_INVALID) {
        attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attribs[n++] = (EGLint)(b.modifier & 0xffffffffULL);
        attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attribs[n++] = (EGLint)(b.modifier >> 32);
    } else if (b.modifier != DRM_FORMAT_MOD_LINEAR && b.modifier != DRM_FORMAT_MOD_INVALID) {
        std::fprintf(stderr, "[bev/ring] modifier 0x%llx needs EGL_EXT_image_dma_buf_import_modifiers\n",
                     (unsigned long long)b.modifier);
        return false;
    }
    attribs[n++] = EGL_NONE;

    // EGL dups the fd internally; ours stays valid for the shell.
    b.image = create_image_(egl_->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (b.image == EGL_NO_IMAGE_KHR) {
        std::fprintf(stderr, "[bev/ring] eglCreateImageKHR(dma_buf) failed (0x%x)\n", eglGetError());
        return false;
    }

    glGenTextures(1, &b.texture);
    glBindTexture(GL_TEXTURE_2D, b.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    image_target_texture_(GL_TEXTURE_2D, b.image);
    const GLenum tex_err = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (tex_err != GL_NO_ERROR) {
        std::fprintf(stderr, "[bev/ring] binding the EGLImage to a texture failed (0x%x)\n", tex_err);
        return false;
    }

    glGenFramebuffers(1, &b.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) {
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[bev/ring] dma-buf FBO incomplete (0x%x) -- fourcc 0x%08x is not "
                     "renderable here\n", status, b.fourcc);
        return false;
    }
    return true;
}

void DmabufRing::destroy(RingBuffer &b)
{
    if (b.fbo) glDeleteFramebuffers(1, &b.fbo);
    if (b.texture) glDeleteTextures(1, &b.texture);
    if (b.image != EGL_NO_IMAGE_KHR && destroy_image_) destroy_image_(egl_->dpy, b.image);
    if (b.fd >= 0) close(b.fd);
    if (b.bo) gbm_bo_destroy(b.bo);
#ifdef BEV_IHS_PV_VULKAN
    if (b.vk) {
        vk_->destroy(*b.vk);
        delete b.vk;
    }
#endif
    b = RingBuffer{};
}

void DmabufRing::release()
{
    for (auto &b : buffers_) destroy(b);
    buffers_.clear();
}

void DmabufRing::compose(uint32_t slot, GLuint src_tex, int src_w, int src_h, BevParams::Fit fit)
{
    const RingBuffer &b = buffers_[slot];
    const int W = (int)b.width, H = (int)b.height;

    if (src_tex != src_fbo_tex_) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo_);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src_tex, 0);
        src_fbo_tex_ = src_tex;
    }

    int sx0 = 0, sy0 = 0, sx1 = src_w, sy1 = src_h;
    int dx0 = 0, dy0 = 0, dx1 = W, dy1 = H;
    if (fit != BevParams::Fit::Fill && src_w > 0 && src_h > 0 && W > 0 && H > 0) {
        const double sx = (double)W / src_w, sy = (double)H / src_h;
        if (fit == BevParams::Fit::Contain) {
            // Letterbox/pillarbox, as the --preview window does.
            const double s = std::min(sx, sy);
            const int dw = (int)std::lround(src_w * s), dh = (int)std::lround(src_h * s);
            dx0 = (W - dw) / 2; dx1 = dx0 + dw;
            dy0 = (H - dh) / 2; dy1 = dy0 + dh;
        } else {
            // Cover: crop the source instead.
            const double s = std::max(sx, sy);
            const int cw = (int)std::lround(W / s), ch = (int)std::lround(H / s);
            sx0 = (src_w - cw) / 2; sx1 = sx0 + cw;
            sy0 = (src_h - ch) / 2; sy1 = sy0 + ch;
        }
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b.fbo);
    glDisable(GL_SCISSOR_TEST);
    if (dx0 > 0 || dy0 > 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // Destination rows reversed: the renderer's output is bottom-up, the
    // compositor reads a dma-buf top-down.
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy1, dx1, dy0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DmabufRing::clear(uint32_t slot)
{
    glBindFramebuffer(GL_FRAMEBUFFER, buffers_[slot].fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

int DmabufRing::finish()
{
    if (dup_native_fence_) {
        const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID,
                                  EGL_NONE};
        EGLSyncKHR sync = create_sync_(egl_->dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
        if (sync != EGL_NO_SYNC_KHR) {
            // The fence fd only exists once the commands it covers are flushed.
            glFlush();
            const int fd = dup_native_fence_(egl_->dpy, sync);
            destroy_sync_(egl_->dpy, sync);
            if (fd >= 0) return fd;
        }
    }
    glFinish();
    return -1;
}
