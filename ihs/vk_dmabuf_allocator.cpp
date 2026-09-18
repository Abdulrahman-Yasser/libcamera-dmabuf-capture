#include "vk_dmabuf_allocator.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr uint64_t kModLinear = 0; // DRM_FORMAT_MOD_LINEAR

bool has_extension(const char *const *list, size_t count, const char *name)
{
    for (size_t i = 0; i < count; ++i)
        if (list[i] && std::strcmp(list[i], name) == 0) return true;
    return false;
}

uint32_t pick_device_local(const VkPhysicalDeviceMemoryProperties &props, uint32_t type_bits)
{
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const bool allowed = (type_bits & (1u << i)) != 0;
        const bool local = (props.memoryTypes[i].propertyFlags &
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        if (allowed && local) return i;
    }
    return UINT32_MAX;
}

} // namespace

bool VkDmabufAllocator::init(VkInstance instance, VkPhysicalDevice physical_device,
                             VkDevice device, void *get_instance_proc_addr,
                             const char *const *device_extensions,
                             size_t device_extension_count)
{
    if (instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE ||
        device == VK_NULL_HANDLE || get_instance_proc_addr == nullptr)
        return false;

    // A device can't grow extensions after creation, so check what the shell
    // enabled before resolving anything.
    for (const char *name : {VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                             VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                             VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME}) {
        if (device_extensions && device_extension_count &&
            !has_extension(device_extensions, device_extension_count, name)) {
            std::fprintf(stderr, "[bev/vk] shell device lacks %s -- no Vulkan dma-buf export\n",
                         name);
            return false;
        }
    }

    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_instance_proc_addr);
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
    if (!gdpa) return false;
    auto dev  = [&](const char *name) { return gdpa(device, name); };
    auto inst = [&](const char *name) { return gipa(instance, name); };

    auto get_format_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
        inst("vkGetPhysicalDeviceFormatProperties2"));
    auto get_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        inst("vkGetPhysicalDeviceProperties2"));
    auto enumerate_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        inst("vkEnumerateDeviceExtensionProperties"));
    get_memory_properties_ = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        inst("vkGetPhysicalDeviceMemoryProperties"));
    create_image_  = reinterpret_cast<PFN_vkCreateImage>(dev("vkCreateImage"));
    destroy_image_ = reinterpret_cast<PFN_vkDestroyImage>(dev("vkDestroyImage"));
    get_image_memory_requirements_ = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
        dev("vkGetImageMemoryRequirements"));
    allocate_memory_   = reinterpret_cast<PFN_vkAllocateMemory>(dev("vkAllocateMemory"));
    free_memory_       = reinterpret_cast<PFN_vkFreeMemory>(dev("vkFreeMemory"));
    bind_image_memory_ = reinterpret_cast<PFN_vkBindImageMemory>(dev("vkBindImageMemory"));
    get_image_subresource_layout_ = reinterpret_cast<PFN_vkGetImageSubresourceLayout>(
        dev("vkGetImageSubresourceLayout"));
    get_modifier_properties_ = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
        dev("vkGetImageDrmFormatModifierPropertiesEXT"));
    get_memory_fd_ = reinterpret_cast<PFN_vkGetMemoryFdKHR>(dev("vkGetMemoryFdKHR"));

    if (!get_format_properties2 || !get_memory_properties_ || !create_image_ ||
        !destroy_image_ || !get_image_memory_requirements_ || !allocate_memory_ ||
        !free_memory_ || !bind_image_memory_ || !get_image_subresource_layout_ ||
        !get_modifier_properties_ || !get_memory_fd_) {
        std::fprintf(stderr, "[bev/vk] shell device is missing a dma-buf export entry point\n");
        return false;
    }

    instance_        = instance;
    physical_device_ = physical_device;
    device_          = device;

    // Modifiers the device can render, sample and export for B8G8R8A8.
    // Single-plane only: the submit path describes one plane.
    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    props.pNext = &list;
    get_format_properties2(physical_device_, VK_FORMAT_B8G8R8A8_UNORM, &props);
    std::vector<VkDrmFormatModifierPropertiesEXT> entries(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = entries.data();
    get_format_properties2(physical_device_, VK_FORMAT_B8G8R8A8_UNORM, &props);

    constexpr VkFormatFeatureFlags kNeeded =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    modifiers_.clear();
    bool linear_ok = false;
    for (const auto &e : entries) {
        if (e.drmFormatModifierPlaneCount != 1 ||
            (e.drmFormatModifierTilingFeatures & kNeeded) != kNeeded)
            continue;
        if (e.drmFormatModifier == kModLinear) linear_ok = true;
        else modifiers_.push_back(e.drmFormatModifier);
    }
    // Tiled modifiers avoid a detile per frame on a tiled GPU, so they lead;
    // LINEAR is the one every importer accepts, so it stays as the floor.
    if (linear_ok || modifiers_.empty()) modifiers_.push_back(kModLinear);

    // Which render node is this device? Lets the producer's EGL display sit on
    // the same GPU on a multi-GPU host.
    render_node_.clear();
    if (get_properties2 && enumerate_extensions) {
        uint32_t n = 0;
        enumerate_extensions(physical_device_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        enumerate_extensions(physical_device_, nullptr, &n, exts.data());
        bool has_drm = false;
        for (const auto &e : exts)
            if (std::strcmp(e.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0)
                has_drm = true;
        if (has_drm) {
            VkPhysicalDeviceDrmPropertiesEXT drm{};
            drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 p{};
            p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            p.pNext = &drm;
            get_properties2(physical_device_, &p);
            if (drm.hasRender)
                render_node_ = "/dev/dri/renderD" + std::to_string(drm.renderMinor);
        }
    }

    std::printf("[bev/vk] shell device: %zu exportable modifier(s), render node %s\n",
                modifiers_.size(), render_node_.empty() ? "(unknown)" : render_node_.c_str());
    return true;
}

bool VkDmabufAllocator::create(uint32_t width, uint32_t height, uint64_t modifier,
                               VkDmabufImage &out) const
{
    out = VkDmabufImage{};
    if (!ready() || width == 0 || height == 0) return false;

    // Offer exactly the negotiated modifier; a list would let the driver pick
    // one the compositor never agreed to.
    VkImageDrmFormatModifierListCreateInfoEXT mod_list{};
    mod_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    mod_list.drmFormatModifierCount = 1;
    mod_list.pDrmFormatModifiers = &modifier;

    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.pNext = &mod_list;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ic{};
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.pNext = &ext;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_B8G8R8A8_UNORM;
    ic.extent = {width, height, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    // GL renders into it through the imported dma-buf; the compositor samples it.
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (create_image_(device_, &ic, nullptr, &image) != VK_SUCCESS) {
        std::fprintf(stderr, "[bev/vk] vkCreateImage %ux%u mod=0x%llx failed\n",
                     width, height, (unsigned long long)modifier);
        return false;
    }

    // The exported layout must describe the image as created, not as requested.
    VkImageDrmFormatModifierPropertiesEXT mp{};
    mp.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    if (get_modifier_properties_(device_, image, &mp) != VK_SUCCESS) {
        destroy_image_(device_, image, nullptr);
        return false;
    }

    VkMemoryRequirements req{};
    get_image_memory_requirements_(device_, image, &req);
    VkPhysicalDeviceMemoryProperties mem_props{};
    get_memory_properties_(physical_device_, &mem_props);
    const uint32_t type_index = pick_device_local(mem_props, req.memoryTypeBits);
    if (type_index == UINT32_MAX) {
        destroy_image_(device_, image, nullptr);
        return false;
    }

    // Dedicated and exportable, so the fd names exactly this image's storage.
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    VkExportMemoryAllocateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    export_info.pNext = &dedicated;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &export_info;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type_index;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (allocate_memory_(device_, &mai, nullptr, &memory) != VK_SUCCESS) {
        destroy_image_(device_, image, nullptr);
        return false;
    }
    if (bind_image_memory_(device_, image, memory, 0) != VK_SUCCESS) {
        free_memory_(device_, memory, nullptr);
        destroy_image_(device_, image, nullptr);
        return false;
    }

    VkImageSubresource sub{};
    sub.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
    VkSubresourceLayout layout{};
    get_image_subresource_layout_(device_, image, &sub, &layout);

    VkMemoryGetFdInfoKHR get_fd{};
    get_fd.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    get_fd.memory = memory;
    get_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if (get_memory_fd_(device_, &get_fd, &fd) != VK_SUCCESS || fd < 0) {
        free_memory_(device_, memory, nullptr);
        destroy_image_(device_, image, nullptr);
        return false;
    }

    out.image    = image;
    out.memory   = memory;
    out.fd       = fd;
    out.modifier = mp.drmFormatModifier;
    out.offset   = (uint32_t)layout.offset;
    out.stride   = (uint32_t)layout.rowPitch;
    return true;
}

void VkDmabufAllocator::destroy(VkDmabufImage &image) const
{
    // The fd is an independent reference to the memory; release both.
    if (image.fd >= 0) close(image.fd);
    if (ready()) {
        if (image.image != VK_NULL_HANDLE) destroy_image_(device_, image.image, nullptr);
        if (image.memory != VK_NULL_HANDLE) free_memory_(device_, image.memory, nullptr);
    }
    image = VkDmabufImage{};
}
