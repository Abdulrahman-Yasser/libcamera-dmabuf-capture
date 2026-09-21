#pragma once

// Allocates the platform view's ring buffers on the shell's Vulkan device, for
// a Vulkan backend (wayland-vulkan / drm-kms-vulkan).
//
// The producer still renders with GLES -- the pipeline's shaders are GLSL ES --
// but it renders into memory the compositor's own device allocated and
// exported. That is what makes the frame importable on the Vulkan side: the
// modifier is one that device reported it can sample, rather than one gbm on
// some render node chose and the compositor may refuse.
//
// Every entry point is resolved through the shell's interposed
// vkGetInstanceProcAddr (IhsVulkanContext::get_instance_proc_addr), as
// ihs/platform_view.h requires. Nothing here submits to the shared queue.

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct VkDmabufImage {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int            fd     = -1;   // exported dma-buf; closed by destroy() unless taken
    uint64_t       modifier = 0;
    uint32_t       offset = 0;
    uint32_t       stride = 0;
};

class VkDmabufAllocator {
public:
    // Resolves entry points and probes modifiers. Returns false when the device
    // was created without VK_EXT_external_memory_dma_buf,
    // VK_KHR_external_memory_fd and VK_EXT_image_drm_format_modifier -- there is
    // then no export path, and the caller allocates from gbm instead.
    bool init(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
              void *get_instance_proc_addr,
              const char *const *device_extensions, size_t device_extension_count);

    bool ready() const { return device_ != VK_NULL_HANDLE; }

    // B8G8R8A8 (DRM ARGB8888/XRGB8888) single-plane modifiers the device can
    // sample, render and export -- tiled first, LINEAR last.
    const std::vector<uint64_t> &modifiers() const { return modifiers_; }

    // "/dev/dri/renderD<N>" of this device (VK_EXT_physical_device_drm), or
    // empty when the driver doesn't say. The producer opens its EGL display on
    // that node so the GL import happens on the same GPU that allocated.
    const std::string &render_node() const { return render_node_; }

    bool create(uint32_t width, uint32_t height, uint64_t modifier, VkDmabufImage &out) const;
    void destroy(VkDmabufImage &image) const;

private:
    VkInstance       instance_        = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice         device_          = VK_NULL_HANDLE;
    std::vector<uint64_t> modifiers_;
    std::string           render_node_;

    PFN_vkGetPhysicalDeviceMemoryProperties       get_memory_properties_         = nullptr;
    PFN_vkCreateImage                             create_image_                  = nullptr;
    PFN_vkDestroyImage                            destroy_image_                 = nullptr;
    PFN_vkGetImageMemoryRequirements              get_image_memory_requirements_ = nullptr;
    PFN_vkAllocateMemory                          allocate_memory_               = nullptr;
    PFN_vkFreeMemory                              free_memory_                   = nullptr;
    PFN_vkBindImageMemory                         bind_image_memory_             = nullptr;
    PFN_vkGetImageSubresourceLayout               get_image_subresource_layout_  = nullptr;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT  get_modifier_properties_       = nullptr;
    PFN_vkGetMemoryFdKHR                          get_memory_fd_                 = nullptr;
};
