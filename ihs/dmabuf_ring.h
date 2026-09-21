#pragma once

// The ring of dma-bufs a platform view renders into and hands the shell.
//
// Each buffer is allocated by whoever makes it importable on the running
// backend (see RingAllocator), imported into the view's own EGL display as an
// EGLImage, and wrapped in a texture + FBO. Per frame the pipeline's output
// texture is blitted into one slot -- scaled to the view, and flipped, since
// GpuRenderer's FBO is GL bottom-up while a dma-buf's first row is its top --
// and the slot's fd goes to ihs_pv_submit.
//
// Every method except the destructor needs the view's EGL context current.

#include "bev_params.h"
#include "egl_context.h"

#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <cstdint>
#include <vector>

struct VkDmabufImage;
class VkDmabufAllocator;

// Values match BEV_ALLOCATOR_* in bev_view_api.h.
enum class RingAllocator {
    Vulkan     = 1, // exportable images on the shell's Vulkan device
    ShellGbm   = 2, // scanout-capable bos on the shell's gbm device (DRM backends)
    RenderNode = 3, // bos on the view's own render node (wayland-egl)
};

const char *ring_allocator_name(RingAllocator allocator);

struct RingBuffer {
    uint32_t buffer_id = 0;
    uint32_t fourcc    = 0;
    uint64_t modifier  = 0;
    uint32_t width = 0, height = 0;
    int      fd     = -1; // owned; the submitter dups it
    uint32_t offset = 0;
    uint32_t stride = 0;

    EGLImageKHR    image   = EGL_NO_IMAGE_KHR;
    GLuint         texture = 0;
    GLuint         fbo     = 0;
    gbm_bo        *bo      = nullptr;
    VkDmabufImage *vk      = nullptr;
};

class DmabufRing {
public:
    DmabufRing() = default;
    ~DmabufRing();

    DmabufRing(const DmabufRing &) = delete;
    DmabufRing &operator=(const DmabufRing &) = delete;

    // @shell_gbm is required for ShellGbm, @vk for Vulkan; RenderNode uses the
    // gbm device inside @egl.
    bool init(const EGLState &egl, RingAllocator allocator,
              gbm_device *shell_gbm, const VkDmabufAllocator *vk);

    // (Re)allocates @count buffers. Buffer ids are fresh on every call, so the
    // registry never serves a cached import of a buffer that no longer exists.
    bool allocate(uint32_t count, uint32_t width, uint32_t height,
                  uint32_t fourcc, uint64_t modifier);
    void release();

    uint32_t size() const { return (uint32_t)buffers_.size(); }
    const RingBuffer &buffer(uint32_t slot) const { return buffers_[slot]; }
    RingAllocator allocator() const { return allocator_; }

    // Scale @src_tex (a GL-upright @src_w x @src_h texture) into @slot.
    void compose(uint32_t slot, GLuint src_tex, int src_w, int src_h, BevParams::Fit fit);
    void clear(uint32_t slot);

    // Ends the frame: returns an acquire fence fd that fires when the GPU is
    // done, or -1 after a glFinish() when the display can't export one.
    int finish();

    // Single-plane modifiers this display can import @fourcc with *and* render
    // to. Empty when the display can't enumerate them (no
    // EGL_EXT_image_dma_buf_import_modifiers), in which case only LINEAR is safe.
    static std::vector<uint64_t> renderable_modifiers(EGLDisplay display, uint32_t fourcc);

private:
    bool alloc_storage(RingBuffer &b, uint64_t modifier);
    bool import(RingBuffer &b);
    void destroy(RingBuffer &b);

    const EGLState          *egl_       = nullptr;
    RingAllocator            allocator_ = RingAllocator::RenderNode;
    gbm_device              *shell_gbm_ = nullptr;
    const VkDmabufAllocator *vk_        = nullptr;

    std::vector<RingBuffer> buffers_;
    uint32_t generation_ = 0;
    GLuint   src_fbo_ = 0;
    GLuint   src_fbo_tex_ = 0;
    bool     has_import_modifiers_ = false;

    PFNEGLCREATEIMAGEKHRPROC            create_image_         = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC           destroy_image_        = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_ = nullptr;
    PFNEGLCREATESYNCKHRPROC             create_sync_          = nullptr;
    PFNEGLDESTROYSYNCKHRPROC            destroy_sync_         = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC   dup_native_fence_     = nullptr;
};
