#include "vkrender.hpp"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <algorithm>
#include <cstring>

namespace {

bool isSrgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_R8G8B8_SRGB:
    case VK_FORMAT_B8G8R8_SRGB:
        return true;
    default:
        return false;
    }
}

VkImageMemoryBarrier imageBarrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                                  VkAccessFlags dstAccess, const VkImageSubresourceRange& range,
                                  uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED, uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = srcFamily;
    b.dstQueueFamilyIndex = dstFamily;
    b.image = image;
    b.subresourceRange = range;
    return b;
}

}  // namespace

// ============================================================================
// Setting up

bool VkRender::loadInstanceFunctions() {
#define LOAD_I(name) vk_.name = reinterpret_cast<PFN_vk##name>(vk_.GetInstanceProcAddr(instance_, "vk" #name))
    LOAD_I(DestroyInstance);
    LOAD_I(DestroySurfaceKHR);
    LOAD_I(EnumeratePhysicalDevices);
    LOAD_I(GetPhysicalDeviceProperties);
    LOAD_I(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(GetPhysicalDeviceMemoryProperties);
    LOAD_I(GetPhysicalDeviceFormatProperties);
    LOAD_I(CreateDevice);
    LOAD_I(GetDeviceProcAddr);
#undef LOAD_I
    return vk_.DestroyInstance && vk_.EnumeratePhysicalDevices && vk_.GetPhysicalDeviceProperties &&
           vk_.GetPhysicalDeviceQueueFamilyProperties && vk_.GetPhysicalDeviceMemoryProperties &&
           vk_.GetPhysicalDeviceFormatProperties && vk_.CreateDevice && vk_.GetDeviceProcAddr;
}

bool VkRender::loadDeviceFunctions() {
    bool ok = true;
#define LOAD_D(name)                                                                                   \
    vk_.name = reinterpret_cast<PFN_vk##name>(vk_.GetDeviceProcAddr(device_, "vk" #name));             \
    ok = ok && vk_.name != nullptr
    LOAD_D(DestroyDevice);
    LOAD_D(GetDeviceQueue);
    LOAD_D(DeviceWaitIdle);
    LOAD_D(CreateCommandPool);
    LOAD_D(DestroyCommandPool);
    LOAD_D(AllocateCommandBuffers);
    LOAD_D(ResetCommandBuffer);
    LOAD_D(BeginCommandBuffer);
    LOAD_D(EndCommandBuffer);
    LOAD_D(CmdPipelineBarrier);
    LOAD_D(CmdBlitImage);
    LOAD_D(CmdCopyImageToBuffer);
    LOAD_D(QueueSubmit);
    LOAD_D(CreateFence);
    LOAD_D(DestroyFence);
    LOAD_D(WaitForFences);
    LOAD_D(ResetFences);
    LOAD_D(CreateImage);
    LOAD_D(DestroyImage);
    LOAD_D(GetImageMemoryRequirements);
    LOAD_D(BindImageMemory);
    LOAD_D(CreateBuffer);
    LOAD_D(DestroyBuffer);
    LOAD_D(GetBufferMemoryRequirements);
    LOAD_D(BindBufferMemory);
    LOAD_D(AllocateMemory);
    LOAD_D(FreeMemory);
    LOAD_D(MapMemory);
    LOAD_D(UnmapMemory);
    LOAD_D(InvalidateMappedMemoryRanges);
#undef LOAD_D
    return ok;
}

// A graphics + compute queue family on a GPU, preferring a real graphics card.
bool VkRender::pickGpu(VkPhysicalDevice& gpu, uint32_t& family) {
    uint32_t n = 0;
    vk_.EnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    if (n == 0 || vk_.EnumeratePhysicalDevices(instance_, &n, gpus.data()) < 0) return false;
    int bestRank = -1;
    for (VkPhysicalDevice g : gpus) {
        VkPhysicalDeviceProperties p{};
        vk_.GetPhysicalDeviceProperties(g, &p);
        uint32_t qn = 0;
        vk_.GetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vk_.GetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        for (uint32_t i = 0; i < qn; ++i) {
            const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((qs[i].queueFlags & need) != need) continue;
            const int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 4
                             : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                             : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 2
                             : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                                      : 1;
            if (rank > bestRank) {
                bestRank = rank;
                gpu = g;
                family = i;
            }
            break;
        }
    }
    return bestRank >= 0;
}

bool VkRender::createDefaultDevice(VkPhysicalDevice gpu, uint32_t family) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &q;
    if (vk_.CreateDevice(gpu, &info, nullptr, &device_) != VK_SUCCESS) return false;
    gpu_ = gpu;
    queueFamily_ = family;
    return true;
}

VkInstance VkRender::createInstanceWrapper(void* opaque, const VkInstanceCreateInfo* info) {
    auto* self = static_cast<VkRender*>(opaque);
    // Add the extensions our window surface needs to whatever the core asked for.
    std::vector<const char*> ext(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
    for (const char* e : self->windowExtensions_)
        if (std::none_of(ext.begin(), ext.end(), [&](const char* x) { return std::strcmp(x, e) == 0; })) ext.push_back(e);
    VkInstanceCreateInfo ci = *info;
    ci.enabledExtensionCount = uint32_t(ext.size());
    ci.ppEnabledExtensionNames = ext.data();
    VkInstance inst = VK_NULL_HANDLE;
    if (self->vk_.CreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) {
        // Without the surface extensions, then (the core may still work).
        if (self->vk_.CreateInstance(info, nullptr, &inst) != VK_SUCCESS) return VK_NULL_HANDLE;
        self->windowExtensions_.clear();
    }
    return inst;
}

VkDevice VkRender::createDeviceWrapper(VkPhysicalDevice gpu, void* opaque, const VkDeviceCreateInfo* info) {
    auto* self = static_cast<VkRender*>(opaque);
    VkDevice dev = VK_NULL_HANDLE;
    if (self->vk_.CreateDevice(gpu, info, nullptr, &dev) != VK_SUCCESS) return VK_NULL_HANDLE;
    return dev;
}

bool VkRender::create(const retro_hw_render_context_negotiation_interface_vulkan* negotiation, std::string& error) {
    destroy();
    negotiation_ = negotiation;
    if (negotiation_ && negotiation_->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
        negotiation_ = nullptr;
    const unsigned version = negotiation_ ? negotiation_->interface_version : 0;

    // 1. The Vulkan loader (vulkan-1.dll / libvulkan.so), through SDL.
    if (SDL_Vulkan_LoadLibrary(nullptr) != 0) {
        error = std::string("Vulkan isn't installed on this PC (") + SDL_GetError() + ")";
        return false;
    }
    vk_.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!vk_.GetInstanceProcAddr) {
        error = "Vulkan isn't installed on this PC";
        return false;
    }
    vk_.CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vk_.GetInstanceProcAddr(nullptr, "vkCreateInstance"));
    vk_.EnumerateInstanceVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vk_.GetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (!vk_.CreateInstance) {
        error = "Vulkan isn't working on this PC";
        return false;
    }

    // A hidden window for the surface, and the instance extensions it needs.
    window_ = SDL_CreateWindow("Nintendall Vulkan", 0, 0, 16, 16, SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
    if (window_) {
        unsigned n = 0;
        if (SDL_Vulkan_GetInstanceExtensions(window_, &n, nullptr) && n) {
            windowExtensions_.resize(n);
            if (!SDL_Vulkan_GetInstanceExtensions(window_, &n, windowExtensions_.data())) windowExtensions_.clear();
        }
    }

    // 2. The instance: the core's own way if it has one (v2), otherwise ours.
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Nintendall";
    app.pEngineName = "Nintendall";
    app.apiVersion = VK_API_VERSION_1_1;
    const VkApplicationInfo* coreApp = negotiation_ && negotiation_->get_application_info ? negotiation_->get_application_info() : nullptr;
    if (coreApp) app = *coreApp;
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vk_.EnumerateInstanceVersion) vk_.EnumerateInstanceVersion(&loaderVersion);
    if (app.apiVersion < VK_API_VERSION_1_1 && loaderVersion >= VK_API_VERSION_1_1) app.apiVersion = VK_API_VERSION_1_1;

    if (version >= 2 && negotiation_->create_instance) {
        instance_ = negotiation_->create_instance(vk_.GetInstanceProcAddr, &app, createInstanceWrapper, this);
    } else {
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        instance_ = createInstanceWrapper(this, &info);
    }
    if (!instance_ || !loadInstanceFunctions()) {
        error = "Couldn't start Vulkan (no compatible graphics driver)";
        destroy();
        return false;
    }
    if (window_ && !windowExtensions_.empty() && !SDL_Vulkan_CreateSurface(window_, instance_, &surface_))
        surface_ = VK_NULL_HANDLE;

    // 3. The device: the core creates it if it wants to (it knows the features it needs).
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t family = 0;
    if (!pickGpu(gpu, family)) {
        error = "No graphics card with Vulkan support was found";
        destroy();
        return false;
    }
    retro_vulkan_context ctx{};
    bool made = false;
    if (version >= 2 && negotiation_->create_device2) {
        made = negotiation_->create_device2(&ctx, instance_, gpu, surface_, vk_.GetInstanceProcAddr,
                                            createDeviceWrapper, this);
        if (!made)  // let the core pick the GPU itself
            made = negotiation_->create_device2(&ctx, instance_, VK_NULL_HANDLE, surface_, vk_.GetInstanceProcAddr,
                                                createDeviceWrapper, this);
    } else if (negotiation_ && negotiation_->create_device) {
        made = negotiation_->create_device(&ctx, instance_, gpu, surface_, vk_.GetInstanceProcAddr, nullptr, 0,
                                           nullptr, 0, nullptr);
    }
    if (made) {
        coreMadeDevice_ = true;
        gpu_ = ctx.gpu;
        device_ = ctx.device;
        queue_ = ctx.queue;
        queueFamily_ = ctx.queue_family_index;
    } else if (!createDefaultDevice(gpu, family)) {
        error = "Couldn't start Vulkan on this graphics card";
        destroy();
        return false;
    }
    if (!loadDeviceFunctions()) {
        error = "Couldn't start Vulkan (missing functions)";
        destroy();
        return false;
    }
    if (!queue_) vk_.GetDeviceQueue(device_, queueFamily_, 0, &queue_);
    VkPhysicalDeviceProperties props{};
    vk_.GetPhysicalDeviceProperties(gpu_, &props);
    deviceName_ = props.deviceName;
    vk_.GetPhysicalDeviceMemoryProperties(gpu_, &memProps_);

    if (!createResources()) {
        error = "Couldn't set up Vulkan";
        destroy();
        return false;
    }

    iface_ = {};
    iface_.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    iface_.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    iface_.handle = this;
    iface_.instance = instance_;
    iface_.gpu = gpu_;
    iface_.device = device_;
    iface_.get_device_proc_addr = vk_.GetDeviceProcAddr;
    iface_.get_instance_proc_addr = vk_.GetInstanceProcAddr;
    iface_.queue = queue_;
    iface_.queue_index = queueFamily_;
    iface_.set_image = cbSetImage;
    iface_.get_sync_index = cbGetSyncIndex;
    iface_.get_sync_index_mask = cbGetSyncIndexMask;
    iface_.set_command_buffers = cbSetCommandBuffers;
    iface_.wait_sync_index = cbWaitSyncIndex;
    iface_.lock_queue = cbLockQueue;
    iface_.unlock_queue = cbUnlockQueue;
    iface_.set_signal_semaphore = cbSetSignalSemaphore;
    return true;
}

bool VkRender::createResources() {
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = queueFamily_;
    if (vk_.CreateCommandPool(device_, &pi, nullptr, &pool_) != VK_SUCCESS) return false;
    for (Slot& s : slots_) {
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vk_.AllocateCommandBuffers(device_, &ai, &s.cmd) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vk_.CreateFence(device_, &fi, nullptr, &s.fence) != VK_SUCCESS) return false;
    }
    return true;
}

void VkRender::freeSlotImage(Slot& s) {
    if (s.image) vk_.DestroyImage(device_, s.image, nullptr);
    if (s.imageMem) vk_.FreeMemory(device_, s.imageMem, nullptr);
    s.image = VK_NULL_HANDLE;
    s.imageMem = VK_NULL_HANDLE;
    s.imageW = s.imageH = 0;
    s.imageFormat = VK_FORMAT_UNDEFINED;
}

void VkRender::freeSlotBuffer(Slot& s) {
    if (s.mapped) vk_.UnmapMemory(device_, s.bufferMem);
    if (s.buffer) vk_.DestroyBuffer(device_, s.buffer, nullptr);
    if (s.bufferMem) vk_.FreeMemory(device_, s.bufferMem, nullptr);
    s.mapped = nullptr;
    s.buffer = VK_NULL_HANDLE;
    s.bufferMem = VK_NULL_HANDLE;
    s.bufferSize = 0;
}

void VkRender::destroy() {
    if (device_) {
        if (vk_.DeviceWaitIdle) vk_.DeviceWaitIdle(device_);
        for (Slot& s : slots_) {
            freeSlotImage(s);
            freeSlotBuffer(s);
            if (s.fence) vk_.DestroyFence(device_, s.fence, nullptr);
            s = Slot{};
        }
        if (pool_) vk_.DestroyCommandPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    // The core frees its own extra device resources first (before the instance goes).
    if (coreMadeDevice_ && negotiation_ && negotiation_->destroy_device) negotiation_->destroy_device();
    coreMadeDevice_ = false;
    if (device_ && vk_.DestroyDevice) vk_.DestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    if (surface_ && vk_.DestroySurfaceKHR) vk_.DestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (instance_ && vk_.DestroyInstance) vk_.DestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    gpu_ = VK_NULL_HANDLE;
    if (window_) SDL_DestroyWindow(window_);
    window_ = nullptr;
    windowExtensions_.clear();
    if (vk_.GetInstanceProcAddr) SDL_Vulkan_UnloadLibrary();
    vk_ = Fn{};
    negotiation_ = nullptr;
    haveImage_ = false;
    waitSemaphores_.clear();
    coreCommands_.clear();
    signalSemaphore_ = VK_NULL_HANDLE;
    frame_ = 0;
}

uint32_t VkRender::memoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice, bool* got) const {
    for (int pass = 0; pass < 2; ++pass) {
        const VkMemoryPropertyFlags flags = pass == 0 ? want | nice : want;
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & flags) == flags) {
                if (got) *got = (memProps_.memoryTypes[i].propertyFlags & nice) == nice;
                return i;
            }
    }
    return UINT32_MAX;
}

// The slot's shrunk image (w x h, `format`) and the memory buffer it's copied into.
bool VkRender::ensureSlot(Slot& s, unsigned w, unsigned h, VkFormat format) {
    if (s.image && (s.imageW != w || s.imageH != h || s.imageFormat != format)) freeSlotImage(s);
    if (!s.image) {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {w, h, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk_.CreateImage(device_, &ii, nullptr, &s.image) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vk_.GetImageMemoryRequirements(device_, s.image, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = memoryType(req.memoryTypeBits, 0, 0);
        if (ai.memoryTypeIndex == UINT32_MAX || vk_.AllocateMemory(device_, &ai, nullptr, &s.imageMem) != VK_SUCCESS ||
            vk_.BindImageMemory(device_, s.image, s.imageMem, 0) != VK_SUCCESS) {
            freeSlotImage(s);
            return false;
        }
        s.imageW = w;
        s.imageH = h;
        s.imageFormat = format;
    }
    const VkDeviceSize need = VkDeviceSize(w) * h * 4;
    if (s.buffer && s.bufferSize < need) freeSlotBuffer(s);
    if (!s.buffer) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = need;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vk_.CreateBuffer(device_, &bi, nullptr, &s.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vk_.GetBufferMemoryRequirements(device_, s.buffer, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        // Cached memory makes reading it on the CPU fast.
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || vk_.AllocateMemory(device_, &ai, nullptr, &s.bufferMem) != VK_SUCCESS ||
            vk_.BindBufferMemory(device_, s.buffer, s.bufferMem, 0) != VK_SUCCESS ||
            vk_.MapMemory(device_, s.bufferMem, 0, VK_WHOLE_SIZE, 0, &s.mapped) != VK_SUCCESS) {
            freeSlotBuffer(s);
            return false;
        }
        s.coherent = (memProps_.memoryTypes[ai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        s.bufferSize = need;
    }
    return true;
}

// ============================================================================
// Each frame

void VkRender::waitSlot(Slot& s) {
    if (s.fence) vk_.WaitForFences(device_, 1, &s.fence, VK_TRUE, UINT64_MAX);
}

const uint32_t* VkRender::collect(Slot& s, unsigned& outW, unsigned& outH) {
    if (!s.pending) return nullptr;
    waitSlot(s);
    s.pending = false;
    if (!s.coherent) {
        VkMappedMemoryRange r{};
        r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        r.memory = s.bufferMem;
        r.size = VK_WHOLE_SIZE;
        vk_.InvalidateMappedMemoryRanges(device_, 1, &r);
    }
    // BGRA8 bytes are XRGB8888 pixels; the alpha byte is ignored.
    pixels_.resize(size_t(s.w) * s.h);
    std::memcpy(pixels_.data(), s.mapped, pixels_.size() * 4);
    outW = s.w;
    outH = s.h;
    return pixels_.data();
}

const uint32_t* VkRender::readFrame(unsigned w, unsigned h, unsigned maxW, unsigned maxH, unsigned& outW, unsigned& outH) {
    if (!active() || !haveImage_ || w == 0 || h == 0) return nullptr;
    Slot& s = slots_[frame_ % kSlots];
    Slot& prev = slots_[(frame_ + kSlots - 1) % kSlots];

    // Shrink to the size it's shown at (keeping the shape).
    unsigned ow = w, oh = h;
    if (maxW && maxH && (w > maxW || h > maxH)) {
        const double k = std::min(double(maxW) / w, double(maxH) / h);
        ow = std::max(1u, unsigned(w * k + 0.5));
        oh = std::max(1u, unsigned(h * k + 0.5));
    }
    const VkImageViewCreateInfo& ci = image_.create_info;
    const VkFormat dstFormat = isSrgb(ci.format) ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;

    waitSlot(s);  // this slot's previous copy (two frames ago) is done
    if (s.pending) {  // not collected yet: keep it for a duplicate frame? No, it's older than prev.
        s.pending = false;
    }
    if (!ensureSlot(s, ow, oh, dstFormat)) return collect(prev, outW, outH);

    VkFormatProperties fp{};
    vk_.GetPhysicalDeviceFormatProperties(gpu_, ci.format, &fp);
    const bool linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;

    vk_.ResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_.BeginCommandBuffer(s.cmd, &bi);

    VkImageSubresourceRange srcRange{};
    srcRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    srcRange.baseMipLevel = ci.subresourceRange.baseMipLevel;
    srcRange.levelCount = 1;
    srcRange.baseArrayLayer = ci.subresourceRange.baseArrayLayer;
    srcRange.layerCount = 1;
    const VkImageSubresourceRange dstRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const bool general = image_.image_layout == VK_IMAGE_LAYOUT_GENERAL;
    const VkImageLayout srcLayout = general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    // Take the image from the core's queue family if it's a different one.
    const bool transfer = srcQueueFamily_ != VK_QUEUE_FAMILY_IGNORED && srcQueueFamily_ != queueFamily_;

    VkImageMemoryBarrier before[2] = {
        imageBarrier(ci.image, image_.image_layout, srcLayout, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, srcRange, transfer ? srcQueueFamily_ : VK_QUEUE_FAMILY_IGNORED,
                     transfer ? queueFamily_ : VK_QUEUE_FAMILY_IGNORED),
        imageBarrier(s.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     dstRange),
    };
    vk_.CmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 2, before);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, srcRange.baseMipLevel, srcRange.baseArrayLayer, 1};
    blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(ow), int32_t(oh), 1};
    vk_.CmdBlitImage(s.cmd, ci.image, srcLayout, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                     linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

    VkImageMemoryBarrier mid[2] = {
        imageBarrier(s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, dstRange),
        // Give the core its image back as it was.
        imageBarrier(ci.image, srcLayout, image_.image_layout, VK_ACCESS_TRANSFER_READ_BIT, 0, srcRange,
                     transfer ? queueFamily_ : VK_QUEUE_FAMILY_IGNORED, transfer ? srcQueueFamily_ : VK_QUEUE_FAMILY_IGNORED),
    };
    vk_.CmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           0, 0, nullptr, 0, nullptr, general && !transfer ? 1 : 2, mid);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {ow, oh, 1};
    vk_.CmdCopyImageToBuffer(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &copy);

    VkBufferMemoryBarrier host{};
    host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = s.buffer;
    host.size = VK_WHOLE_SIZE;
    vk_.CmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0,
                           nullptr);
    vk_.EndCommandBuffer(s.cmd);

    // Submit: the core's own command buffers first (if it handed them over), then our copy.
    std::vector<VkPipelineStageFlags> waitStages(waitSemaphores_.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo submits[2]{};
    uint32_t count = 0;
    if (!coreCommands_.empty()) {
        submits[count].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submits[count].commandBufferCount = uint32_t(coreCommands_.size());
        submits[count].pCommandBuffers = coreCommands_.data();
        ++count;
    }
    VkSubmitInfo& mine = submits[count++];
    mine.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (coreCommands_.empty() && !waitSemaphores_.empty()) {  // semaphores are ignored with set_command_buffers
        mine.waitSemaphoreCount = uint32_t(waitSemaphores_.size());
        mine.pWaitSemaphores = waitSemaphores_.data();
        mine.pWaitDstStageMask = waitStages.data();
    }
    mine.commandBufferCount = 1;
    mine.pCommandBuffers = &s.cmd;
    if (signalSemaphore_) {
        mine.signalSemaphoreCount = 1;
        mine.pSignalSemaphores = &signalSemaphore_;
    }
    vk_.ResetFences(device_, 1, &s.fence);
    VkResult r;
    {
        std::lock_guard<std::mutex> lock(queueLock_);
        r = vk_.QueueSubmit(queue_, count, submits, s.fence);
    }
    waitSemaphores_.clear();  // waited for once
    coreCommands_.clear();
    signalSemaphore_ = VK_NULL_HANDLE;
    if (r == VK_SUCCESS) {
        s.pending = true;
        s.w = ow;
        s.h = oh;
    }
    ++frame_;
    // Hand back the previous frame's picture (its copy has had a whole frame to finish).
    return collect(prev, outW, outH);
}

const uint32_t* VkRender::takePending(unsigned& outW, unsigned& outH) {
    for (int i = 1; i <= kSlots; ++i) {
        Slot& s = slots_[(frame_ + kSlots - i) % kSlots];
        if (s.pending) return collect(s, outW, outH);
    }
    return nullptr;
}

// ============================================================================
// The core's calls

void VkRender::cbSetImage(void* h, const retro_vulkan_image* image, uint32_t numSemaphores, const VkSemaphore* semaphores,
                          uint32_t srcQueueFamily) {
    auto* self = static_cast<VkRender*>(h);
    if (!image) return;
    self->image_ = *image;
    self->image_.create_info.pNext = nullptr;
    self->haveImage_ = true;
    self->srcQueueFamily_ = srcQueueFamily;
    self->waitSemaphores_.assign(semaphores, semaphores + numSemaphores);
}

uint32_t VkRender::cbGetSyncIndex(void* h) { return static_cast<VkRender*>(h)->frame_ % kSlots; }

uint32_t VkRender::cbGetSyncIndexMask(void*) { return (1u << kSlots) - 1; }

void VkRender::cbSetCommandBuffers(void* h, uint32_t num, const VkCommandBuffer* cmd) {
    static_cast<VkRender*>(h)->coreCommands_.assign(cmd, cmd + num);
}

void VkRender::cbWaitSyncIndex(void* h) {
    auto* self = static_cast<VkRender*>(h);
    self->waitSlot(self->slots_[self->frame_ % kSlots]);
}

void VkRender::cbLockQueue(void* h) { static_cast<VkRender*>(h)->queueLock_.lock(); }

void VkRender::cbUnlockQueue(void* h) { static_cast<VkRender*>(h)->queueLock_.unlock(); }

void VkRender::cbSetSignalSemaphore(void* h, VkSemaphore semaphore) { static_cast<VkRender*>(h)->signalSemaphore_ = semaphore; }
